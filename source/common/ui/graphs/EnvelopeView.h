#pragma once

// EnvelopeView (docs/architecture.md 9.2.2, 9.3.2): one ADSR picture with four handles. The shape
// is drawn from `EnvelopeShape` (the instrument's own envelope code, supplied by the page through
// a function); the x axis is the sliders' own scale (EnvelopeModel.h). The handles are secondary
// bindings of the four sliders below the picture:
//   peak -> attack, sustain start -> decay (x) and sustain (y), sustain end -> sustain,
//   release end -> release.
// When `polarity` is Inverted a mirrored ghost of the shape is drawn under the axis (the picture
// then shows the level range 1 ... -1). Time labels under the handles are the parameters' own text.

#include "common/ui/graphs/GraphView.h"
#include "common/ui/models/EnvelopeModel.h"

namespace vs::ui
{
class EnvelopeView final : public GraphView, private ParamMirror::Listener, private ParamMirror::IdleClient
{
public:
    struct Ids
    {
        juce::String attack, decay, sustain, release;
        juce::String polarity; // empty: no inverted mode (the picture never shows a ghost)
    };
    /** The shape for the normalised values (the instrument's envelope code). */
    using ShapeFn = std::function<model::EnvelopeShape (const model::EnvelopeValues&)>;

    enum Handle { Peak = 0, SustainStart = 1, SustainEnd = 2, ReleaseEnd = 3 };

    /** `name`: "Envelope 1" (accessibility names and help labels of the handles). */
    EnvelopeView (UiContext& ctx, int part, const Ids& ids, const juce::String& name, ShapeFn shape);
    ~EnvelopeView() override;

    void setAccent (juce::Colour c);

    // ---- What is drawn (tests) ----
    const model::EnvelopeLayout& getLayout() const noexcept { return layout; }
    const model::EnvelopeValues& shownValues() const noexcept { return values; }
    const model::EnvelopeShape& shownShape() const noexcept { return shape; }
    bool showsGhost() const noexcept { return inverted; }
    const juce::String& timeLabel (int i) const noexcept { return timeText[static_cast<size_t> (i)]; }
    int shapeComputations() const noexcept { return shapeCount; }
    int pathBuilds() const noexcept { return pathCount; }
    /** Where the page's mapping puts the handles for these values (also the handles' own "expected"). */
    model::EnvelopeLayout layoutFor (const model::EnvelopeValues& v, bool ghost) const;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void mirrorChanged (int index, bool valueChanged) override;
    void mirrorIdle (double nowMs) override;
    float read (int which, bool live) const;
    model::EnvelopeValues readValues (bool live) const;
    bool anyDragging() const;
    void refresh();
    void placeHandles();
    juce::Point<float> handleCentre (int which, const model::EnvelopeLayout& l, const model::EnvelopeValues& v) const;
    juce::Rectangle<float> plotArea() const;

    UiContext& ctx;
    Ids ids;
    ShapeFn shapeFn;
    int idx[5] { -1, -1, -1, -1, -1 };

    model::EnvelopeLayout layout;
    model::EnvelopeValues values;
    model::EnvelopeShape shape;
    bool inverted = false, dirty = true;
    std::array<juce::String, 3> timeText;
    std::array<float, 3> labelX {};

    juce::Path linePath, lineStroke, fillPath, ghostPath, ghostDashed, releaseDashed; // lineStroke: linePath stroked once, here, not in paint()
    juce::Font smallFont;
    juce::String keyReleasedText, timeAxisText; // fixed captions, made once
    juce::Colour accentColour;
    int shapeCount = 0, pathCount = 0;
    float lastShapeInput[4] { -1.0f, -1.0f, -1.0f, -1.0f };
};
} // namespace vs::ui
