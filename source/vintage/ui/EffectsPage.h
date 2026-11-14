#pragma once

// The Effects page (docs/architecture.md 9.3.6): chorus, delay and reverb in signal order, each
// with its picture. A global page (both parts share the effects): 11 primary controls. The delay
// and reverb pictures carry handles (secondary bindings: delay time and repeats, reverb pre-delay
// and decay time); delay and reverb have a power switch (U5: the level 0 and back, no parameter of
// its own). The pictures are computed by the JUCE-free models of common/ui/models/FxPictureModels.h
// once per tick in which one of their inputs changed.

#include "common/shell/EditorState.h"
#include "common/ui/controls/Panel.h"
#include "common/ui/controls/PowerSwitch.h"
#include "common/ui/controls/SegmentedSwitch.h"
#include "common/ui/controls/Slider.h"
#include "common/ui/graphs/FxViews.h"

#include <vector>

namespace vs::ui
{
class EffectsPage final : public juce::Component,
                          public InteractiveContent,
                          private ParamMirror::Listener,
                          private ParamMirror::IdleClient
{
public:
    EffectsPage (UiContext& ctx, shell::EditorState& editorState);
    ~EffectsPage() override;

    ChorusView& getChorusView() noexcept { return chorusView; }
    DelayView& getDelayView() noexcept { return delayView; }
    ReverbView& getReverbView() noexcept { return reverbView; }
    PowerSwitch& getDelayPower() noexcept { return delayPower; }
    PowerSwitch& getReverbPower() noexcept { return reverbPower; }
    Slider& getSlider (const juce::String& id) noexcept;
    SegmentedSwitch& getChorusMode() noexcept { return chorusMode; }
    SegmentedSwitch& getPingPong() noexcept { return pingPong; }
    GraphHandle& getDelayHandle() noexcept { return delayView.getHandle (0); }
    GraphHandle& getPreDelayHandle() noexcept { return reverbView.getHandle (0); }
    GraphHandle& getDecayHandle() noexcept { return reverbView.getHandle (1); }
    /** The panel header text as drawn (it says OFF while the level is 0). */
    const juce::String& delayHeader() const noexcept { return delayHeaderText; }
    const juce::String& reverbHeader() const noexcept { return reverbHeaderText; }
    /** The pictures' inputs as the mirror holds them now (tests compare these with the drawn state). */
    model::DelayPicture expectedDelay() const;
    model::ReverbPicture expectedReverb() const;
    int pictureComputations() const noexcept { return computations; }
    void cancelInteraction() override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void mirrorChanged (int index, bool valueChanged) override;
    void mirrorIdle (double nowMs) override;
    void updateStates();
    void recomputePictures (bool fromLive);
    float value (int index, bool fromLive) const;
    void placeHandles (bool fromLive);

    UiContext& ctx;
    Panel chorusPanel, delayPanel, reverbPanel;
    ChorusView chorusView;
    DelayView delayView;
    ReverbView reverbView;
    SegmentedSwitch chorusMode, pingPong;
    Slider chorusMix, delayTime, delayFeedback, delayDamping, delayLevel, reverbPreDelay, reverbDecay, reverbDamping, reverbLevel;
    PowerSwitch delayPower, reverbPower;
    std::vector<int> inputs; // mirror indices of everything a picture or a rule reads
    int iChorusMode = -1, iChorusMix = -1, iDelayTime = -1, iDelayFeedback = -1, iPingPong = -1, iDelayLevel = -1, iPreDelay = -1, iDecay = -1,
        iReverbLevel = -1;
    juce::String delayHeaderText, reverbHeaderText;
    bool dirty = false;
    int computations = 0;
    bool delayWasOn = true, reverbWasOn = true;
};
} // namespace vs::ui
