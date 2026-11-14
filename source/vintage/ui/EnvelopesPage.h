#pragma once

// The Envelopes page of one part (docs/architecture.md 9.3.2): two ADSR pictures with draggable
// handles, the four sliders under each, key follow, and what the envelopes do (the filter, pitch
// and velocity amounts). 16 primary controls; the handles are secondary bindings of the sliders.
// The panel titles name what each envelope drives and follow the filter source.

#include "common/ui/controls/Panel.h"
#include "common/ui/controls/SegmentedSwitch.h"
#include "common/ui/controls/Slider.h"
#include "common/ui/graphs/EnvelopeView.h"

namespace vs::ui
{
class EnvelopesPage final : public juce::Component, public InteractiveContent, private ParamMirror::Listener
{
public:
    EnvelopesPage (UiContext& ctx, int part);
    ~EnvelopesPage() override;

    int getPart() const noexcept { return part; }
    /** 0: Envelope 1, 1: Envelope 2. */
    EnvelopeView& getEnvelopeView (int envelope) noexcept { return envelope == 0 ? view1 : view2; }
    /** The panel title as shown ("Envelope 1 (filter, pitch)"). */
    juce::String getPanelTitle (int envelope) const { return envelope == 0 ? title1 : title2; }
    /** The title the filter source asks for (tests compare it with the shown one). */
    juce::String expectedTitle (int envelope) const;
    void cancelInteraction() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;

private:
    void mirrorChanged (int index, bool valueChanged) override;
    void updateTitles();

    UiContext& ctx;
    int part;
    int sourceIndex = -1;
    juce::String title1, title2;

    Panel panel1, panel2;
    EnvelopeView view1, view2;
    Slider attack1, decay1, sustain1, release1;
    Toggle keyFollow1;
    SegmentedSwitch direction;
    Slider filterAmount;
    SegmentedSwitch filterSource;
    Slider pitchAmount, velFilter;
    Slider attack2, decay2, sustain2, release2;
    Toggle keyFollow2;
    Slider velAmp;

    juce::Font smallFont, boldFont;
    juce::String what1, what2, noteTarget, note2a, note2b;
};
} // namespace vs::ui
