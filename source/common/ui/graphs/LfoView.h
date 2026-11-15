#pragma once

// LfoView (docs/architecture.md 9.2.2, 9.3.3): the LFO's waveform on a linear time axis with two
// handles: the delay handle sits on the fade-in's envelope line at the time the LFO reaches full
// strength (`delay`; the LFO is silent for the first half of the delay, a faint non-draggable
// "starts" marker shows where it begins), and the cycle handle on the zero line at the right end
// of the first whole cycle after it (`rate`). Both are secondary bindings of the sliders next to
// the picture. The window (0.5 ... 50 s, LfoModel.h) is kept through a drag and after it while both
// handles lie in the middle of the plot, else chosen again. During a drag the dragged parameters
// show the user's value, whatever the host writes. The waveform is the instrument's own LFO code,
// supplied by the page through a function.

#include "common/ui/graphs/GraphView.h"
#include "common/ui/models/LfoModel.h"

#include <set>

namespace vs::ui
{
class LfoView final : public GraphView, private ParamMirror::Listener, private ParamMirror::IdleClient
{
public:
    struct Ids
    {
        juce::String waveform, rate, delay;
    };
    /** Fills the preview for the wave (choice index), rate (Hz), delay (s) and window (s). */
    using PreviewFn = std::function<void (int wave, float rateHz, float delaySec, float windowSec, model::LfoPreview& out)>;

    enum Handle { Delay = 0, Cycle = 1 };

    LfoView (UiContext& ctx, int part, const Ids& ids, PreviewFn preview);
    ~LfoView() override;

    void setAccent (juce::Colour c);
    /** Waveforms (choice indexes) that are different every time: the picture says "Example: ...". */
    void setExampleWaves (std::set<int> waves);

    // ---- What is drawn (tests) ----
    const model::LfoAxis& getAxis() const noexcept { return axis; }
    const model::LfoPreview& shownPreview() const noexcept { return preview; }
    int shownWave() const noexcept { return wave; }
    float shownRateValue() const noexcept { return rateValue; }
    float shownDelayValue() const noexcept { return delayValue; }
    /** Captions as drawn; empty = not shown. */
    const juce::String& delayCaption() const noexcept { return delayText; }
    const juce::String& startCaption() const noexcept { return startText; }
    const juce::String& cycleCaption() const noexcept { return cycleText; }
    const juce::String& exampleCaption() const noexcept { return exampleText; }
    float startMarkerX() const noexcept { return startX; }
    int previewComputations() const noexcept { return previewCount; }
    int pathBuilds() const noexcept { return pathCount; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void mirrorChanged (int index, bool valueChanged) override;
    void mirrorIdle (double nowMs) override;
    float read (int which, bool live) const;
    bool anyDragging() const;
    /** `live`: read the parameters themselves (the mirror lags by a tick) - after the picture's own edit. */
    void refresh (bool live);
    void placeHandles();
    juce::Point<float> handleCentre (int which, const model::LfoAxis& a, float delayV, float rateV) const;
    juce::Rectangle<float> plotArea() const;
    float amplitude() const;
    model::LfoAxis axisFor (float windowSec) const;

    UiContext& ctx;
    Ids ids;
    PreviewFn previewFn;
    int idx[3] { -1, -1, -1 }; // waveform, rate, delay
    std::set<int> exampleWaves;

    model::LfoAxis axis;
    model::LfoPreview preview;
    int wave = 1;
    float rateValue = 0.0f, delayValue = 0.0f;
    float lastWindow = 0.0f;
    float lastInput[3] { -1.0f, -1.0f, -1.0f };
    bool dirty = true, havePreview = false;

    juce::Path wavePath, waveStroke, delayDashed, cycleBracket, bracketStroke, wedge, startMarker;
    juce::StringArray tickText;
    juce::String delayText, startText, cycleText, exampleText;
    juce::String examplePhrase;
    juce::Font smallFont;
    float delayX = 0.0f, cycleX = 0.0f, startX = 0.0f;
    juce::Colour accentColour;
    int previewCount = 0, pathCount = 0;
};
} // namespace vs::ui
