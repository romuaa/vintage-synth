#pragma once

// The LFO page of one part (docs/architecture.md 9.3.3): the waveform picture with its delay and
// cycle handles, the waveform, rate and delay controls, and what the LFO moves (the pitch, the
// pulse width, the filter, the volume), each with its own amount. 9 primary controls; the handles
// are secondary bindings of rate and delay. The pulse width controls are dimmed (display only)
// while neither oscillator is set to Pulse (9.3.9).

#include "common/ui/controls/Panel.h"
#include "common/ui/controls/SegmentedSwitch.h"
#include "common/ui/controls/Slider.h"
#include "common/ui/graphs/LfoView.h"

namespace vs::ui
{
class LfoPage final : public juce::Component, public InteractiveContent, private ParamMirror::Listener
{
public:
    LfoPage (UiContext& ctx, int part);
    ~LfoPage() override;

    int getPart() const noexcept { return part; }
    LfoView& getLfoView() noexcept { return view; }
    /** The pulse width controls' dimmed state, and what it should be for the mirror's values. */
    bool pulseWidthInactive() const noexcept { return pwmAmount.isInactive() && pwmSource.isInactive(); }
    bool pulseWidthExpectedInactive() const;
    void cancelInteraction() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;

private:
    void mirrorChanged (int index, bool valueChanged) override;
    void updateInactive();

    UiContext& ctx;
    int part;
    int waveIndex[2] { -1, -1 };

    Panel lfoPanel, pitchPanel, pwmPanel, filterPanel, volumePanel;
    LfoView view;
    SegmentedSwitch waveform;
    Slider rate, delay;
    Slider pitchAmount;
    SegmentedSwitch pitchTarget;
    Slider pwmAmount;
    SegmentedSwitch pwmSource;
    Slider filterAmount;
    SegmentedSwitch volumeAmount;

    juce::Font smallFont, boldFont;
    juce::String movesCaption, notePitch, notePwm, noteFilter, noteVolume;
};
} // namespace vs::ui
