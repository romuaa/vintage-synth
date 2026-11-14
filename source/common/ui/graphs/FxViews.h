#pragma once

// The effect pictures of the Effects page (docs/architecture.md 9.3.6, 9.4): ChorusView (display
// only), DelayView and ReverbView (with draggable handles, secondary bindings of the parameters
// their page lists). Each is computed from the JUCE-free picture models (models/FxPictureModels.h)
// when one of its inputs changes; every string, path and font it draws is prepared then, so
// paint() allocates nothing (9.5). A picture whose effect is off is grey.

#include "common/ui/PaintFonts.h"
#include "common/ui/graphs/GraphView.h"
#include "common/ui/models/FxPictureModels.h"

namespace vs::ui
{
/** The delay (ms) of the chorus's two taps over four seconds: the wobble that makes the sound wide. */
class ChorusView final : public juce::Component
{
public:
    static constexpr double kWindowSec = 4.0;
    static constexpr float kAxisMs = 8.0f; // the picture's delay axis: 0 (dry) .. 8 ms

    explicit ChorusView (const Theme& t);
    /** `mode`: 0 Off, 1 I, 2 II; `mix` 0..1 (how strongly the copies are drawn). */
    void setState (int mode, float mix);
    int drawnMode() const noexcept { return mode; }
    float drawnMix() const noexcept { return mix; }
    int recomputeCount() const noexcept { return recomputes; }
    /** The tap delay in ms at the picture's x position (tests: equals the effect block's LFO). */
    float tapAtX (float x, bool anti) const;
    /** Where the picture draws a delay of `ms` (y, in this component's coordinates). */
    float yOfMs (float ms) const noexcept;
    const juce::String& caption() const noexcept { return note; }
    juce::Rectangle<float> plotArea() const noexcept { return plot; }

    void paint (juce::Graphics&) override;
    void resized() override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void rebuild();

    PaintFonts fonts;
    int mode = 0;
    float mix = 0.5f;
    juce::Rectangle<float> plot;
    juce::Path pathA, pathB;
    juce::String note, dryLabel, topLabel, endLabel, footnote;
    int recomputes = 0;
    bool stateSet = false;
};

class DelayView final : public GraphView
{
public:
    explicit DelayView (const Theme& t);
    /** `timeSec`, `feedbackGain` as the engine reads them; `on`: the level is above 0. */
    void setState (double timeSec, float feedbackGain, bool pingPong, bool on, const juce::String& timeText);
    const model::DelayPicture& picture() const noexcept { return pic; }
    const model::LogTimeAxis& timeAxis() const noexcept { return axis; }
    juce::Rectangle<float> plotArea() const noexcept { return plot; }
    /** Height of one channel lane; the feedback handle moves `laneHeight () * 0.95` px per full range. */
    float laneHeight() const noexcept { return laneH; }
    /** Where the handle sits: on top of the second echo (x: its time, y: its height). */
    juce::Point<float> handleAt (double timeSec, float feedbackGain, bool pingPong) const;
    /** Time from the handle's x position (the handle stands on the second echo, at twice the time). */
    double timeAtHandleX (float px) const noexcept;
    bool isOn() const noexcept { return on; }
    int recomputeCount() const noexcept { return recomputes; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    float laneBottom (int lane) const noexcept { return lanesTop + laneH * static_cast<float> (lane + 1) + 4.0f * static_cast<float> (lane); }

    PaintFonts fonts;
    model::DelayPicture pic;
    model::LogTimeAxis axis;
    juce::Rectangle<float> plot;
    float laneH = 40.0f, lanesTop = 0.0f;
    double time = 0.7;
    float feedback = 0.35f;
    bool pingPong = false, on = false;
    juce::String timeText, laneLabels[2], inputLabel, tickLabels[6];
    float inputX = 0.0f;
    int recomputes = 0;
    bool stateSet = false;
};

class ReverbView final : public GraphView
{
public:
    explicit ReverbView (const Theme& t);
    void setState (float preDelaySec, float decaySec, bool on, const juce::String& decayText);
    const model::ReverbPicture& picture() const noexcept { return pic; }
    const model::ReverbTimeAxis& timeAxis() const noexcept { return axis; }
    juce::Rectangle<float> plotArea() const noexcept { return plot; }
    float topY() const noexcept { return plot.getY() + 4.0f; }
    float floorY() const noexcept { return plot.getBottom() - 8.0f; }
    /** The pre-delay handle: where the tail starts; the decay handle: where it reaches -60 dB. */
    juce::Point<float> preDelayHandleAt (float preDelaySec) const noexcept;
    juce::Point<float> decayHandleAt (float preDelaySec, float decaySec) const noexcept;
    bool isOn() const noexcept { return on; }
    int recomputeCount() const noexcept { return recomputes; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void rebuild();

    PaintFonts fonts;
    model::ReverbPicture pic;
    model::ReverbTimeAxis axis;
    juce::Rectangle<float> plot;
    juce::Path wedge;
    bool on = false;
    juce::String decayText, caption, tickLabels[4], timeLabel, floorLabel, topLabel;
    int recomputes = 0;
    bool stateSet = false;
};
} // namespace vs::ui
