#include "common/ui/graphs/GraphView.h"

namespace vs::ui
{
namespace
{
bool fineModifier (const juce::ModifierKeys& m) { return m.isShiftDown() || m.isCommandDown(); }
} // namespace

// ---- GraphHandle ----

struct GraphHandle::AxisBinding final : public BoundControl, public ParamMirror::Listener
{
    AxisBinding (GraphHandle& h, int a) : handle (h), axis (a) {}

    bool showsMirroredValue (juce::String& why) const override
    {
        const auto want = handle.expectedCentre();
        const auto got = handle.getCentre();
        if (got.getDistanceFrom (want) > 0.5f)
        {
            why << handle.getTitle() << ": handle at " << got.toString() << ", mapping says " << want.toString();
            return false;
        }
        return true;
    }
    void mirrorChanged (int index, bool) override
    {
        handle.setMarkerAlpha (handle.ctx.mirror.markerAlpha (index, handle.ctx.mirror.lastTickTime()));
    }

    GraphHandle& handle;
    int axis;
};

GraphHandle::GraphHandle (UiContext& c, const juce::String& name, int part, const juce::String& xParam, model::HandleModel::PxToValue xMap,
                          const juce::String& yParam, float yPxPerRange, std::function<juce::Point<float>()> expectedCentre)
    : ctx (c), expected (std::move (expectedCentre)), accentColour (accentFor (part))
{
    setTitle (partPrefix (part) + name);
    setWantsKeyboardFocus (true);
    setComponentID (name);
    const int r = metrics::kHandleRadius;
    setSize (2 * r + 8, 2 * r + 8);

    auto bind = [&] (const juce::String& id, int axis) {
        const int index = ctx.mirror.indexOf (id);
        jassert (index >= 0);
        auto h = std::make_unique<ParamHandle> (ctx.mirror, index);
        auto b = std::make_unique<AxisBinding> (*this, axis);
        ctx.mirror.subscribe (index, b.get());
        ctx.registry.add ({ id, ctx.registry.page(), ctx.registry.part(), Placement::Secondary, "GraphHandle", 0, 0, b.get() });
        bindings.push_back (std::move (b));
        return h;
    };
    if (xParam.isNotEmpty())
    {
        xHandle = bind (xParam, 0);
        model.setX (xHandle.get(), std::move (xMap));
        setHelp (*this, xParam, name, ctx.help);
    }
    if (yParam.isNotEmpty())
    {
        yHandle = bind (yParam, 1);
        model.setY (yHandle.get(), yPxPerRange);
        if (xParam.isEmpty())
            setHelp (*this, yParam, name, ctx.help);
    }
    ctx.mirror.addIdleClient (this);
}

GraphHandle::~GraphHandle()
{
    model.cancel();
    for (auto& b : bindings)
        ctx.mirror.unsubscribe (b.get());
    ctx.mirror.removeIdleClient (this);
}

void GraphHandle::setCentre (juce::Point<float> c)
{
    centre = c;
    const int w = getWidth(), h = getHeight();
    setBounds (juce::roundToInt (c.x) - w / 2, juce::roundToInt (c.y) - h / 2, w, h);
}

void GraphHandle::setMarkerAlpha (float a)
{
    if (std::abs (a - markerAlpha) > 0.01f || (a <= 0.0f) != (markerAlpha <= 0.0f))
    {
        markerAlpha = a;
        repaint();
    }
}

void GraphHandle::paint (juce::Graphics& g)
{
    const auto c = getLocalBounds().toFloat().getCentre() + (centre - getBounds().toFloat().getCentre());
    const float r = static_cast<float> (metrics::kHandleRadius) - 2.0f;
    const juce::Rectangle<float> dot (c.x - r, c.y - r, 2.0f * r, 2.0f * r);
    g.setColour (colour (Tok::TextPrimary));
    g.fillEllipse (dot);
    g.setColour (accentColour);
    g.drawEllipse (dot.reduced (1.0f), 2.0f);
    if (markerAlpha > 0.0f)
        drawExternalMarker (g, dot, {}, markerAlpha, ctx.theme);
    if (hasKeyboardFocus (false))
        drawFocusRing (g, dot, r);
}

void GraphHandle::changed()
{
    if (onChange)
        onChange();
}

void GraphHandle::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getEventRelativeTo (getParentComponent()).position;
    model.mouseDown (p.x, p.y, centre.x, fineModifier (e.mods));
}

void GraphHandle::mouseDrag (const juce::MouseEvent& e)
{
    const auto p = e.getEventRelativeTo (getParentComponent()).position;
    model.mouseDrag (p.x, p.y, fineModifier (e.mods));
    changed();
}

void GraphHandle::mouseUp (const juce::MouseEvent&)
{
    model.mouseUp();
    changed();
}

void GraphHandle::mouseDoubleClick (const juce::MouseEvent&)
{
    model.doubleClick();
    changed();
}

bool GraphHandle::keyPressed (const juce::KeyPress& k)
{
    const int code = k.getKeyCode();
    const bool fine = fineModifier (k.getModifiers());
    int dx = 0, dy = 0;
    if (code == juce::KeyPress::rightKey)
        dx = 1;
    else if (code == juce::KeyPress::leftKey)
        dx = -1;
    else if (code == juce::KeyPress::upKey)
        dy = 1;
    else if (code == juce::KeyPress::downKey)
        dy = -1;
    else
        return false;
    model.nudge (dx, dy, fine, juce::Time::getMillisecondCounterHiRes());
    changed();
    return true;
}

std::unique_ptr<juce::AccessibilityHandler> GraphHandle::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::slider);
}

// ---- GraphView ----

GraphHandle& GraphView::addHandle (std::unique_ptr<GraphHandle> h)
{
    addAndMakeVisible (*h);
    handles.push_back (std::move (h));
    return *handles.back();
}

void GraphView::cancelInteraction()
{
    for (auto& h : handles)
        h->cancelInteraction();
}

std::unique_ptr<juce::AccessibilityHandler> GraphView::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::group);
}

// ---- ResponseCurveView ----

namespace
{
constexpr double kGridHz[] { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 };
}

ResponseCurveView::ResponseCurveView (const Theme& t) : GraphView (t), accentColour (colour (Tok::AccentGlobal))
{
    setTitle ("Filter response");
    for (double hz : kGridHz)
        freqLabels.add (hz >= 1000.0 ? juce::String (juce::roundToInt (hz / 1000.0)) + "k" : juce::String (juce::roundToInt (hz)));
    for (int db = 24; db > -48; db -= 12)
        dbLabels.add ((db > 0 ? "+" : "") + juce::String (db) + " dB");
    dbLabels.add ("Hz");
}

void ResponseCurveView::setCurves (const Magnitude& main, const Magnitude& other)
{
    mainCurve = main;
    otherCurve = other;
    rebuild();
}

void ResponseCurveView::setAccent (juce::Colour c)
{
    accentColour = c;
    for (auto& h : handles)
        h->setAccent (c);
    repaint();
}

void ResponseCurveView::setMarkerHz (double hz)
{
    markerHz = hz;
    markerDashed.clear();
    if (std::isfinite (markerHz) && markerHz >= xAxis.lo && ! plot.isEmpty())
    {
        const float x = xAxis.positionOf (markerHz);
        juce::Path line;
        line.startNewSubPath (x, plot.getY());
        line.lineTo (x, plot.getBottom());
        const float dashes[] { 4.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (markerDashed, line, dashes, 2);
    }
    repaint();
}

void ResponseCurveView::resized()
{
    plot = getLocalBounds().toFloat().reduced (1.0f).withTrimmedBottom (18.0f);
    xAxis = { plot.getX(), plot.getWidth(), 20.0, 20000.0 };
    yAxis = { plot.getY(), plot.getHeight(), 24.0, -48.0 };
    rebuild();
    setMarkerHz (markerHz);
}

void ResponseCurveView::rebuild()
{
    ++recomputes;
    mainPath.clear();
    fillPath.clear();
    otherPath.clear();
    if (plot.isEmpty())
        return;
    constexpr int points = 320;
    const auto curvePath = [&] (const Magnitude& f, juce::Path& p) {
        for (int i = 0; i <= points; ++i)
        {
            const float x = plot.getX() + plot.getWidth() * static_cast<float> (i) / static_cast<float> (points);
            const double db = std::clamp (f (xAxis.valueAt (x)), -60.0, 30.0);
            const float y = std::clamp (yAxis.positionOf (db), plot.getY(), plot.getBottom());
            if (i == 0)
                p.startNewSubPath (x, y);
            else
                p.lineTo (x, y);
        }
    };
    if (mainCurve)
    {
        curvePath (mainCurve, mainPath);
        fillPath = mainPath;
        fillPath.lineTo (plot.getRight(), plot.getBottom());
        fillPath.lineTo (plot.getX(), plot.getBottom());
        fillPath.closeSubPath();
    }
    otherDashed.clear();
    if (otherCurve)
    {
        curvePath (otherCurve, otherPath);
        const float dashes[] { 5.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (otherDashed, otherPath, dashes, 2);
    }
    repaint();
}

void ResponseCurveView::paint (juce::Graphics& g)
{
    g.setColour (colour (Tok::BgWindow));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

    // Grid: decades and their 2 and 5, dB lines every 12 dB; labels below and at the left.
    g.setFont (theme.font (metrics::kTextSmall));
    int label = 0;
    for (double hz : kGridHz)
    {
        const float x = xAxis.positionOf (hz);
        g.setColour (colour (Tok::BgTrack).withAlpha (0.5f));
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.setColour (colour (Tok::TextSecondary));
        g.drawText (freqLabels[label++], juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centred, false);
    }
    label = 0;
    for (int db = 24; db >= -48; db -= 12)
    {
        const float y = yAxis.positionOf (db);
        g.setColour (colour (Tok::BgTrack).withAlpha (db == 0 ? 0.9f : 0.4f));
        g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        if (db > -48)
        {
            g.setColour (colour (Tok::TextSecondary));
            g.drawText (dbLabels[label++], juce::Rectangle<float> (plot.getX() + 4.0f, y + 1.0f, 60.0f, 13.0f), juce::Justification::centredLeft, false);
        }
    }
    g.setColour (colour (Tok::TextSecondary));
    g.drawText (dbLabels[dbLabels.size() - 1], juce::Rectangle<float> (plot.getRight() - 24.0f, plot.getBottom() + 2.0f, 22.0f, 14.0f),
                juce::Justification::centredRight, false);

    g.setColour (colour (Tok::TextSecondary).withAlpha (0.8f));
    g.fillPath (otherDashed);
    g.setColour (accentColour.withAlpha (0.7f));
    g.fillPath (markerDashed);
    g.setColour (accentColour.withAlpha (0.22f));
    g.fillPath (fillPath);
    g.setColour (accentColour);
    g.strokePath (mainPath, juce::PathStrokeType (2.0f));
}

// ---- ScopeView ----

ScopeView::ScopeView (const Theme&) : accentColour (colour (Tok::AccentGlobal))
{
    setTitle ("Waveform");
}

void ScopeView::setSamples (const float* samples, int count, const juce::String& waveName)
{
    data.assign (samples, samples + juce::jmax (0, count));
    setDescription (waveName);
    rebuild();
}

void ScopeView::setAccent (juce::Colour c)
{
    accentColour = c;
    repaint();
}

void ScopeView::rebuild()
{
    ++recomputes;
    path.clear();
    const auto r = getLocalBounds().toFloat().reduced (6.0f, 10.0f);
    if (data.size() < 2 || r.isEmpty())
        return;
    for (size_t i = 0; i < data.size(); ++i)
    {
        const float x = r.getX() + r.getWidth() * static_cast<float> (i) / static_cast<float> (data.size() - 1);
        const float y = r.getCentreY() - std::clamp (data[i], -1.2f, 1.2f) * r.getHeight() * 0.42f;
        if (i == 0)
            path.startNewSubPath (x, y);
        else
            path.lineTo (x, y);
    }
    repaint();
}

void ScopeView::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (colour (Tok::BgWindow));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (colour (Tok::BgTrack).withAlpha (0.5f));
    g.drawHorizontalLine (juce::roundToInt (r.getCentreY()), r.getX() + 4.0f, r.getRight() - 4.0f);
    g.setColour (accentColour);
    g.strokePath (path, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

std::unique_ptr<juce::AccessibilityHandler> ScopeView::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::image);
}
} // namespace vs::ui
