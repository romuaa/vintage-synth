#include "common/ui/graphs/LfoView.h"

namespace vs::ui
{
namespace
{
constexpr int kTicks = 5;
constexpr float kLabelStrip = 18.0f, kCaptionWidth = 240.0f;

juce::String secondsText (float t)
{
    const bool whole = std::abs (t - std::round (t)) < 0.01f;
    return juce::String (t, whole ? 0 : 1) + " s";
}
} // namespace

LfoView::LfoView (UiContext& c, int part, const Ids& idList, PreviewFn previewFunction)
    : GraphView (c.theme), ctx (c), ids (idList), previewFn (std::move (previewFunction)), examplePhrase ("Example: different every time"), smallFont (c.theme.font (metrics::kTextSmall)), accentColour (accentFor (part))
{
    setTitle (partPrefix (part) + "LFO waveform");
    idx[0] = ctx.mirror.indexOf (ids.waveform);
    idx[1] = ctx.mirror.indexOf (ids.rate);
    idx[2] = ctx.mirror.indexOf (ids.delay);
    jassert (idx[0] >= 0 && idx[1] >= 0 && idx[2] >= 0);

    const auto expected = [this] (int which) {
        return [this, which] { return handleCentre (which, axis, read (2, false), read (1, false)); };
    };
    const auto clampedTime = [this] (float px) { return axis.timeAt (std::clamp (px, axis.x0, axis.x0 + axis.width)); };
    addHandle (std::make_unique<GraphHandle> (
        ctx, "LFO delay", part, ids.delay, [clampedTime] (float px) { return model::lfoDelayFromHandle (clampedTime (px)); }, juce::String(), 200.0f,
        expected (Delay)));
    addHandle (std::make_unique<GraphHandle> (
        ctx, "LFO cycle", part, ids.rate, [this, clampedTime] (float px) { return model::lfoRateFromHandle (clampedTime (px), read (2, true)); },
        juce::String(), 200.0f, expected (Cycle)));
    for (auto& h : handles)
    {
        h->setAccent (accentColour);
        h->onChange = [this] { refresh (true); };
    }
    for (int i : idx)
        ctx.mirror.subscribe (i, this);
    ctx.mirror.addIdleClient (this);
}

LfoView::~LfoView()
{
    cancelInteraction();
    ctx.mirror.unsubscribe (this);
    ctx.mirror.removeIdleClient (this);
}

void LfoView::setAccent (juce::Colour c)
{
    accentColour = c;
    for (auto& h : handles)
        h->setAccent (c);
    repaint();
}

float LfoView::read (int which, bool live) const
{
    const int i = idx[which];
    return live ? ctx.mirror.parameter (i).getValue() : ctx.mirror.value (i);
}

bool LfoView::anyDragging() const
{
    for (const auto& h : handles)
        if (h->getModel().isDragging())
            return true;
    return false;
}

juce::Rectangle<float> LfoView::plotArea() const
{
    const float margin = static_cast<float> (metrics::kHandleRadius) + 4.0f;
    return getLocalBounds().toFloat().reduced (margin, margin).withTrimmedBottom (kLabelStrip);
}

model::LfoAxis LfoView::axisFor (float windowSec) const
{
    const auto plot = plotArea();
    return { plot.getX(), plot.getWidth(), windowSec };
}

float LfoView::amplitude() const
{
    return plotArea().getHeight() * 0.46f;
}

juce::Point<float> LfoView::handleCentre (int which, const model::LfoAxis& a, float delayV, float rateV) const
{
    // The delay handle sits on the fade-in's envelope line (the top of the wedge) where the LFO reaches
    // full strength; the cycle handle on the zero line at the end of the first whole cycle.
    const float t = which == Delay ? model::lfoDelayHandleSec (delayV) : model::lfoCycleHandleSec (delayV, rateV);
    const float mid = plotArea().getCentreY();
    return { std::clamp (a.xOf (t), a.x0, a.x0 + a.width), which == Delay ? mid - amplitude() : mid };
}

void LfoView::mirrorChanged (int, bool valueChanged)
{
    if (valueChanged)
        dirty = true;
}

void LfoView::mirrorIdle (double)
{
    if (dirty)
        refresh (false);
}

void LfoView::resized()
{
    refresh (false);
}

void LfoView::placeHandles()
{
    for (int i = 0; i < numHandles(); ++i)
        getHandle (i).setCentre (handleCentre (i, axis, delayValue, rateValue));
}

void LfoView::setExampleWaves (std::set<int> waves)
{
    exampleWaves = std::move (waves);
    dirty = true;
}

void LfoView::refresh (bool live)
{
    dirty = false;
    if (getLocalBounds().isEmpty())
        return;
    const bool dragging = anyDragging();
    auto& waveParam = ctx.mirror.parameter (idx[0]);
    const float waveValue = read (0, live);
    wave = model::stepIndex (waveValue, waveParam.getNumSteps());
    rateValue = read (1, live);
    delayValue = read (2, live);
    // During a drag the dragged parameter shows the user's value, whatever the host writes meanwhile.
    if (auto& m = getHandle (Delay).getModel(); m.hasDragged (0))
        delayValue = m.draggedValue (0);
    if (auto& m = getHandle (Cycle).getModel(); m.hasDragged (0))
        rateValue = m.draggedValue (0);
    const float rateHz = map::lfoRateHz (rateValue), delaySec = map::lfoDelaySec (delayValue);
    // The window is kept through a drag, and after it while both handles lie in the middle of the plot.
    const float window = dragging && lastWindow > 0.0f ? lastWindow : model::lfoChooseWindow (lastWindow, delaySec, rateHz);
    axis = axisFor (window);

    const float in[3] { static_cast<float> (wave), rateValue, delayValue };
    bool changed = ! havePreview || ! model::sameBits (window, lastWindow);
    for (int i = 0; i < 3; ++i)
        changed = changed || ! model::sameBits (in[i], lastInput[i]);
    if (changed)
    {
        if (previewFn)
            previewFn (wave, rateHz, delaySec, window, preview);
        ++previewCount;
        havePreview = true;
        for (int i = 0; i < 3; ++i)
            lastInput[i] = in[i];
        if (! model::sameBits (window, lastWindow))
        {
            tickText.clear();
            for (int i = 1; i <= kTicks; ++i)
                tickText.add (secondsText (window * static_cast<float> (i) / kTicks));
        }
        lastWindow = window;
    }

    // Paths and captions, prepared here (never in paint()).
    ++pathCount;
    const auto plot = plotArea();
    const float amp = amplitude(), mid = plot.getCentreY();
    wavePath.clear();
    constexpr int n = model::LfoPreview::kPoints;
    wavePath.startNewSubPath (axis.x0, mid);
    for (int i = 0; i < n; ++i)
        wavePath.lineTo (axis.x0 + axis.width * static_cast<float> (i + 1) / n, mid - std::clamp (preview.y[static_cast<size_t> (i)], -1.0f, 1.0f) * amp);
    waveStroke.clear();
    juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (waveStroke, wavePath);

    delayX = std::clamp (axis.xOf (model::lfoDelayHandleSec (delayValue)), axis.x0, axis.x0 + axis.width);
    cycleX = std::clamp (axis.xOf (model::lfoCycleHandleSec (delayValue, rateValue)), axis.x0, axis.x0 + axis.width);
    startX = std::clamp (axis.xOf (0.5f * model::lfoDelayHandleSec (delayValue)), axis.x0, axis.x0 + axis.width);
    const bool delayed = delaySec > 0.0f;
    delayDashed.clear();
    wedge.clear();
    startMarker.clear();
    if (delayed)
    {
        // The fade-in: silent until the start marker, then growing linearly to full strength at the handle.
        wedge.startNewSubPath (startX, mid);
        wedge.lineTo (delayX, mid - amp);
        wedge.lineTo (delayX, mid + amp);
        wedge.closeSubPath();
        startMarker.addRectangle (startX - 0.5f, mid - 8.0f, 1.0f, 16.0f);
        juce::Path line;
        line.startNewSubPath (delayX, plot.getY());
        line.lineTo (delayX, plot.getBottom());
        const float dashes[] { 4.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (delayDashed, line, dashes, 2);
    }
    cycleBracket.clear();
    const float by = plot.getBottom() - 22.0f;
    cycleBracket.startNewSubPath (delayX, by - 4.0f);
    cycleBracket.lineTo (delayX, by + 4.0f);
    cycleBracket.startNewSubPath (delayX, by);
    cycleBracket.lineTo (cycleX, by);
    cycleBracket.startNewSubPath (cycleX, by - 4.0f);
    cycleBracket.lineTo (cycleX, by + 4.0f);
    bracketStroke.clear();
    juce::PathStrokeType (1.0f).createStrokedPath (bracketStroke, cycleBracket);

    const auto text = [this] (int which, float v) {
        auto& p = ctx.mirror.parameter (idx[which]);
        const auto label = p.getLabel();
        const auto t = p.getText (v, 1024);
        return label.isEmpty() ? t : t + " " + label;
    };
    delayText = delayed ? "Full strength after " + text (2, delayValue) + " (drag)" : juce::String();
    startText = delayed ? "starts" : juce::String();
    cycleText = "1 cycle (rate " + text (1, rateValue) + ")";
    exampleText = exampleWaves.count (wave) > 0 ? examplePhrase : juce::String();
    setDescription ("Waveform " + waveParam.getText (waveValue, 32) + ", rate " + text (1, rateValue) + ", full strength after " + text (2, delayValue));

    placeHandles();
    repaint();
}

void LfoView::paint (juce::Graphics& g)
{
    using namespace metrics;
    const auto r = getLocalBounds().toFloat();
    g.setColour (colour (Tok::BgWindow));
    g.fillRoundedRectangle (r, metrics::kRadiusControl);
    if (axis.width <= 0.0f)
        return;
    const auto plot = plotArea();

    g.setFont (smallFont);
    for (int i = 0; i < tickText.size(); ++i)
    {
        const float x = axis.x0 + axis.width * static_cast<float> (i + 1) / kTicks;
        g.setColour (colour (Tok::BgTrack).withAlpha (metrics::kGridAlpha));
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.setColour (colour (Tok::TextSecondary));
        g.drawText (tickText[i], juce::Rectangle<float> (x - 28.0f, plot.getBottom() + 12.0f, 56.0f, 14.0f), juce::Justification::centred, false);
    }
    g.setColour (colour (Tok::BgTrack).withAlpha (metrics::kAxisAlpha));
    g.drawHorizontalLine (juce::roundToInt (plot.getCentreY()), plot.getX(), plot.getRight());

    g.setColour (colour (Tok::TextSecondary).withAlpha (0.14f));
    g.fillPath (wedge);
    g.setColour (colour (Tok::TextSecondary).withAlpha (0.6f));
    g.fillPath (startMarker);
    g.setColour (accentColour.withAlpha (metrics::kCurveAlpha));
    g.fillPath (delayDashed);
    g.setColour (accentColour);
    g.fillPath (waveStroke);
    g.setColour (colour (Tok::TextSecondary));
    g.fillPath (bracketStroke);

    const auto pill = [&] (const juce::String& text, juce::Rectangle<float> area) {
        g.setColour (colour (Tok::BgWindow).withAlpha (0.8f));
        g.fillRoundedRectangle (area, metrics::kRadiusSmall);
        g.setColour (colour (Tok::TextPrimary));
        g.drawText (text, area.reduced (4.0f, 0.0f), juce::Justification::centredLeft, true);
    };
    if (delayText.isNotEmpty())
    {
        const float ex = std::min (delayX + 10.0f, plot.getRight() - kCaptionWidth);
        pill (delayText, { std::max (plot.getX(), ex), plot.getY() + 2.0f, kCaptionWidth, 16.0f });
        g.setColour (colour (Tok::TextSecondary));
        g.drawText (startText, juce::Rectangle<float> (startX + 3.0f, plot.getCentreY() + 4.0f, 44.0f, 12.0f), juce::Justification::centredLeft, false);
    }
    const float cx = std::clamp (delayX, plot.getX(), plot.getRight() - kCaptionWidth);
    pill (cycleText, { cx, plot.getBottom() - 20.0f, kCaptionWidth, 16.0f });
    if (exampleText.isNotEmpty())
    {
        g.setColour (colour (Tok::TextSecondary));
        g.drawText (exampleText, juce::Rectangle<float> (plot.getRight() - 240.0f, plot.getBottom() - 40.0f, 236.0f, 14.0f), juce::Justification::centredRight, false);
    }
}
} // namespace vs::ui
