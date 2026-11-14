#pragma once

// LfoView (docs/architecture.md 9.2.2, 9.3.3): the LFO's waveform on a linear time axis with two
// handles on the zero line: the end of the fade-in (`delay`) and the right end of the first whole
// cycle after it (`rate`). Both are secondary bindings of the sliders next to the picture. The
// window (0.5 ... 20 s) is chosen from the settings when no handle is dragged, and kept during a
// drag. The waveform is the instrument's own LFO code, supplied by the page through a function.

#include "common/ui/graphs/GraphView.h"
#include "common/ui/models/LfoModel.h"

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

    // ---- What is drawn (tests) ----
    const model::LfoAxis& getAxis() const noexcept { return axis; }
    const model::LfoPreview& shownPreview() const noexcept { return preview; }
    int shownWave() const noexcept { return wave; }
    float shownRateValue() const noexcept { return rateValue; }
    float shownDelayValue() const noexcept { return delayValue; }
    const juce::String& delayCaption() const noexcept { return delayText; }
    const juce::String& cycleCaption() const noexcept { return cycleText; }
    int previewComputations() const noexcept { return previewCount; }
    int pathBuilds() const noexcept { return pathCount; }
    /** The window the picture would choose for the mirror's current settings. */
    float windowForMirror() const;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void mirrorChanged (int index, bool valueChanged) override;
    void mirrorIdle (double nowMs) override;
    float read (int which, bool live) const;
    bool anyDragging() const;
    void refresh();
    void placeHandles();
    juce::Point<float> handleCentre (int which, const model::LfoAxis& a, float delayV, float rateV) const;
    juce::Rectangle<float> plotArea() const;
    model::LfoAxis axisFor (float windowSec) const;

    UiContext& ctx;
    Ids ids;
    PreviewFn previewFn;
    int idx[3] { -1, -1, -1 }; // waveform, rate, delay

    model::LfoAxis axis;
    model::LfoPreview preview;
    int wave = 1;
    float rateValue = 0.0f, delayValue = 0.0f;
    float lastWindow = 0.0f;
    float lastInput[3] { -1.0f, -1.0f, -1.0f };
    bool dirty = true, havePreview = false;

    juce::Path wavePath, waveStroke, delayDashed, cycleBracket, bracketStroke; // the strokes are made once in refresh(), not in paint()
    juce::Font smallFont;
    juce::StringArray tickText;
    juce::String delayText, cycleText;
    float delayX = 0.0f, cycleX = 0.0f;
    juce::Colour accentColour;
    int previewCount = 0, pathCount = 0;
};
} // namespace vs::ui
