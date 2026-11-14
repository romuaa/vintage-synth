#include "common/ui/graphs/EnvelopeView.h"

namespace vs::ui
{
namespace
{
constexpr float kLabelWidth = 72.0f, kLabelStrip = 18.0f;
} // namespace

EnvelopeView::EnvelopeView (UiContext& c, int part, const Ids& idList, const juce::String& name, ShapeFn shapeFunction)
    : GraphView (c.theme), ctx (c), ids (idList), shapeFn (std::move (shapeFunction)), smallFont (c.theme.font (metrics::kTextSmall)), keyReleasedText ("key released"), timeAxisText ("time >"), accentColour (accentFor (part))
{
    setTitle (partPrefix (part) + name + " shape");
    const juce::String* all[5] { &ids.attack, &ids.decay, &ids.sustain, &ids.release, &ids.polarity };
    for (int i = 0; i < 5; ++i)
        idx[i] = all[i]->isNotEmpty() ? ctx.mirror.indexOf (*all[i]) : -1;
    jassert (idx[0] >= 0 && idx[1] >= 0 && idx[2] >= 0 && idx[3] >= 0);

    // The handles (9.3.2). Their x mappings read the live values of the segments before them: the
    // decay is measured from the peak where it is now, the release from the end of the plateau.
    const auto live = [this] (int which) { return read (which, true); };
    const auto peakX = [this, live] { return layout.x0 + layout.segmentWidth (live (0)); };
    const auto plateauEndX = [this, live, peakX] { return peakX() + layout.segmentWidth (live (1)) + layout.plateauWidth(); };
    const auto expected = [this] (int which) {
        return [this, which] {
            const auto v = readValues (false);
            const bool ghost = idx[4] >= 0 && model::stepIndex (read (4, false), 2) == 1;
            return handleCentre (which, layoutFor (v, ghost), v);
        };
    };

    addHandle (std::make_unique<GraphHandle> (
        ctx, name + " attack", part, ids.attack, [this] (float px) { return layout.attackFromX (px); }, juce::String(), 200.0f, expected (Peak)));
    addHandle (std::make_unique<GraphHandle> (
        ctx, name + " decay and sustain", part, ids.decay, [this, peakX] (float px) { return layout.decayFromX (px, peakX()); }, ids.sustain, 200.0f,
        expected (SustainStart)));
    addHandle (std::make_unique<GraphHandle> (ctx, name + " sustain level", part, juce::String(), nullptr, ids.sustain, 200.0f, expected (SustainEnd)));
    addHandle (std::make_unique<GraphHandle> (
        ctx, name + " release", part, ids.release, [this, plateauEndX] (float px) { return layout.releaseFromX (px, plateauEndX()); }, juce::String(),
        200.0f, expected (ReleaseEnd)));
    for (auto& h : handles)
    {
        h->setAccent (accentColour);
        h->onChange = [this] { refresh(); };
    }

    for (int i : idx)
        if (i >= 0)
            ctx.mirror.subscribe (i, this);
    ctx.mirror.addIdleClient (this);
}

EnvelopeView::~EnvelopeView()
{
    cancelInteraction();
    ctx.mirror.unsubscribe (this);
    ctx.mirror.removeIdleClient (this);
}

void EnvelopeView::setAccent (juce::Colour c)
{
    accentColour = c;
    for (auto& h : handles)
        h->setAccent (c);
    repaint();
}

float EnvelopeView::read (int which, bool live) const
{
    const int i = idx[which];
    return live ? ctx.mirror.parameter (i).getValue() : ctx.mirror.value (i);
}

model::EnvelopeValues EnvelopeView::readValues (bool live) const
{
    return { read (0, live), read (1, live), read (2, live), read (3, live) };
}

bool EnvelopeView::anyDragging() const
{
    for (const auto& h : handles)
        if (h->getModel().isDragging())
            return true;
    return false;
}

juce::Rectangle<float> EnvelopeView::plotArea() const
{
    const float margin = static_cast<float> (metrics::kHandleRadius) + 4.0f;
    return getLocalBounds().toFloat().reduced (margin, margin).withTrimmedBottom (kLabelStrip);
}

model::EnvelopeLayout EnvelopeView::layoutFor (const model::EnvelopeValues&, bool ghost) const
{
    const auto plot = plotArea();
    model::EnvelopeLayout l;
    l.x0 = plot.getX();
    l.width = plot.getWidth();
    l.yTop = plot.getY();
    l.yBase = ghost ? plot.getCentreY() : plot.getBottom(); // inverted: the axis moves up, the level range is 1 ... -1
    return l;
}

juce::Point<float> EnvelopeView::handleCentre (int which, const model::EnvelopeLayout& l, const model::EnvelopeValues& v) const
{
    const auto p = l.points (v);
    switch (which)
    {
        case Peak:         return { p.peakX, p.topY };
        case SustainStart: return { p.decayEndX, p.sustainY };
        case SustainEnd:   return { p.plateauEndX, p.sustainY };
        default:           return { p.releaseEndX, p.baseY };
    }
}

void EnvelopeView::mirrorChanged (int, bool valueChanged)
{
    if (valueChanged)
        dirty = true;
}

void EnvelopeView::mirrorIdle (double)
{
    if (dirty)
        refresh();
}

void EnvelopeView::resized()
{
    refresh();
}

void EnvelopeView::placeHandles()
{
    const float range = std::max (1.0f, std::abs (layout.yBase - layout.yTop));
    for (int i = 0; i < numHandles(); ++i)
    {
        getHandle (i).setCentre (handleCentre (i, layout, values));
        auto& m = getHandle (i).getModel();
        if (m.yTarget() != nullptr && ! m.isDragging())
            m.setY (m.yTarget(), range);
    }
}

void EnvelopeView::refresh()
{
    dirty = false;
    if (getLocalBounds().isEmpty())
        return;
    const bool live = anyDragging();
    values = readValues (live);
    inverted = idx[4] >= 0 && model::stepIndex (read (4, live), 2) == 1;
    layout = layoutFor (values, inverted);

    const float in[4] { values.attack, values.decay, values.sustain, values.release };
    bool changed = false;
    for (int i = 0; i < 4; ++i)
        changed = changed || ! model::sameBits (in[i], lastShapeInput[i]);
    if (changed && shapeFn)
    {
        shape = shapeFn (values);
        ++shapeCount;
        for (int i = 0; i < 4; ++i)
            lastShapeInput[i] = in[i];
    }

    // Paths, prepared here (never in paint()).
    ++pathCount;
    const auto p = layout.points (values);
    constexpr int n = model::EnvelopeShape::kSteps;
    const auto trace = [&] (juce::Path& line, bool mirrored) {
        const auto y = [&] (float level) {
            const float v = layout.levelY (level);
            return mirrored ? 2.0f * layout.yBase - v : v;
        };
        line.clear();
        line.startNewSubPath (layout.x0, y (shape.attack[0]));
        const float aw = p.peakX - layout.x0, dw = p.decayEndX - p.peakX, rw = p.releaseEndX - p.plateauEndX;
        for (int i = 1; i <= n; ++i)
            line.lineTo (layout.x0 + aw * static_cast<float> (i) / n, y (shape.attack[static_cast<size_t> (i)]));
        for (int i = 1; i <= n; ++i)
            line.lineTo (p.peakX + dw * static_cast<float> (i) / n, y (shape.decay[static_cast<size_t> (i)]));
        line.lineTo (p.plateauEndX, y (shape.release[0]));
        for (int i = 1; i <= n; ++i)
            line.lineTo (p.plateauEndX + rw * static_cast<float> (i) / n, y (shape.release[static_cast<size_t> (i)]));
    };
    trace (linePath, false);
    lineStroke.clear();
    juce::PathStrokeType (2.0f).createStrokedPath (lineStroke, linePath);
    fillPath = linePath;
    fillPath.lineTo (p.releaseEndX, layout.yBase);
    fillPath.lineTo (layout.x0, layout.yBase);
    fillPath.closeSubPath();
    ghostPath.clear();
    ghostDashed.clear();
    if (inverted)
    {
        juce::Path line;
        trace (line, true);
        ghostPath = line;
        ghostPath.lineTo (p.releaseEndX, layout.yBase);
        ghostPath.lineTo (layout.x0, layout.yBase);
        ghostPath.closeSubPath();
        const float dashes[] { 5.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (ghostDashed, line, dashes, 2);
    }
    releaseDashed.clear();
    {
        juce::Path line;
        const auto plot = plotArea();
        line.startNewSubPath (p.plateauEndX, plot.getY());
        line.lineTo (p.plateauEndX, plot.getBottom());
        const float dashes[] { 4.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (releaseDashed, line, dashes, 2);
    }

    // Time labels under the handles: the parameters' own text, spread so they never overlap.
    const int which[3] { 0, 1, 3 };
    for (int i = 0; i < 3; ++i)
    {
        auto& param = ctx.mirror.parameter (idx[which[i]]);
        const auto label = param.getLabel();
        const auto text = param.getText (in[which[i]], 1024);
        timeText[static_cast<size_t> (i)] = label.isEmpty() ? text : text + " " + label;
    }
    const float desired[3] { p.peakX, p.decayEndX, p.releaseEndX };
    const auto bounds = getLocalBounds().toFloat();
    model::spreadLabels (desired, labelX.data(), 3, kLabelWidth, bounds.getX(), bounds.getRight());

    setDescription ("Attack " + timeText[0] + ", decay " + timeText[1] + ", sustain "
                    + ctx.mirror.parameter (idx[2]).getText (values.sustain, 1024) + ", release " + timeText[2]
                    + (inverted ? ". Inverted: the shape moves its targets down." : juce::String()));

    placeHandles();
    repaint();
}

void EnvelopeView::paint (juce::Graphics& g)
{
    using namespace metrics;
    const auto r = getLocalBounds().toFloat();
    g.setColour (colour (Tok::BgWindow));
    g.fillRoundedRectangle (r, 4.0f);
    if (layout.width <= 0.0f)
        return;

    // Level lines: the axis (level 0), the peak line and the half way line.
    g.setColour (colour (Tok::BgTrack).withAlpha (0.5f));
    for (const float level : { 0.5f, 1.0f })
        g.drawHorizontalLine (juce::roundToInt (layout.levelY (level)), layout.x0, layout.x0 + layout.width);
    if (inverted)
        for (const float level : { 0.5f, 1.0f })
            g.drawHorizontalLine (juce::roundToInt (2.0f * layout.yBase - layout.levelY (level)), layout.x0, layout.x0 + layout.width);
    g.setColour (colour (Tok::BgTrack).withAlpha (0.9f));
    g.drawHorizontalLine (juce::roundToInt (layout.yBase), layout.x0, layout.x0 + layout.width);

    if (inverted)
    {
        g.setColour (colour (Tok::TextSecondary).withAlpha (0.12f));
        g.fillPath (ghostPath);
        g.setColour (colour (Tok::TextSecondary).withAlpha (0.9f));
        g.fillPath (ghostDashed);
    }
    g.setColour (accentColour.withAlpha (0.7f));
    g.fillPath (releaseDashed);
    g.setColour (accentColour.withAlpha (0.22f));
    g.fillPath (fillPath);
    g.setColour (accentColour);
    g.fillPath (lineStroke);

    g.setFont (smallFont);
    g.setColour (colour (Tok::TextSecondary));
    const auto plot = plotArea();
    const float releasedX = layout.points (values).plateauEndX;
    g.drawText (keyReleasedText, juce::Rectangle<float> (releasedX + 4.0f, plot.getY() - 10.0f, 80.0f, 14.0f), juce::Justification::centredLeft, false);
    g.drawText (timeAxisText, juce::Rectangle<float> (r.getRight() - 52.0f, plot.getY() - 10.0f, 46.0f, 14.0f), juce::Justification::centredRight,
                false);
    for (size_t i = 0; i < timeText.size(); ++i)
        g.drawText (timeText[i], juce::Rectangle<float> (labelX[i] - 0.5f * kLabelWidth, plot.getBottom() + 12.0f, kLabelWidth, 14.0f),
                    juce::Justification::centred, false);
}
} // namespace vs::ui
