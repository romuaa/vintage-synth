#pragma once

// The Play page (docs/architecture.md 9.3.4): how the two parts combine and how each is played.
// A global page: the keyboard layout panel (key mode, the keyboard strip with the split marker and
// the held and sounding keys, balance), Character (Vintage), and the Lower and Upper part panels
// side by side (voices, unison, glide, bend range, mod wheel, pan). 4 global + 8 per part = 20
// primary controls. The inactive rules of 9.3.9 are applied here (display only).

#include "common/engine/UiFeed.h"
#include "common/ui/PaintFonts.h"
#include "common/ui/controls/Panel.h"
#include "common/ui/controls/SegmentedSwitch.h"
#include "common/ui/controls/Slider.h"
#include "common/ui/graphs/KeyboardStrip.h"

namespace vs::ui
{
class PlayPage final : public juce::Component, public InteractiveContent, private ParamMirror::Listener
{
public:
    PlayPage (UiContext& ctx, const UiFeed& feed);
    ~PlayPage() override;

    /** One part's controls (tests). */
    struct PartControls
    {
        std::unique_ptr<SegmentedSwitch> voices, unisonVoices, glideMode;
        std::unique_ptr<Slider> detune, glideTime, bend, vibrato, pan;
    };

    KeyboardStrip& getStrip() noexcept { return strip; }
    SegmentedSwitch& getModeSwitch() noexcept { return mode; }
    Slider& getBalance() noexcept { return balance; }
    Slider& getVintage() noexcept { return vintage; }
    PartControls& getPart (int part) noexcept { return parts[part & 1]; }
    /** The sentence under the mode switch as drawn now. */
    const juce::String& modeSentence() const noexcept { return sentence; }
    static juce::String sentenceFor (int keyMode);
    void cancelInteraction() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;

private:
    void mirrorChanged (int index, bool valueChanged) override;
    void updateStates();

    UiContext& ctx;
    Panel layoutPanel, characterPanel, partPanel[2];
    SegmentedSwitch mode;
    KeyboardStrip strip;
    Slider balance, vintage;
    PartControls parts[2];
    juce::String sentence, stripCaption, vintageCaption, glideCaption;
    PaintFonts fonts;
    int modeIndex = -1;
    int assignIndex[2] { -1, -1 }, portaIndex[2] { -1, -1 };
};
} // namespace vs::ui
