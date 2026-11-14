#pragma once

// The Arp + Seq page (docs/architecture.md 9.3.5): a global page that shows the pattern as a
// picture and is edited directly. Arpeggiator view: pattern cards, octaves, timing and feel, and a
// piano-roll preview of what the engine will play (the engine's own ArpSeq run offline,
// models/ArpPreviewModel.h). Sequencer view: the 16-step grid (pitch and velocity bars, R / T,
// length flag, the playing step from the feed) and the same timing panel below it. The view
// follows arp.mode; while the mode is Off it is the last one used (EditorState::arpView), dimmed.
// 58 primary controls: the nine arp.* controls, the Length slider and the grid's 48 step controls.

#include "common/engine/UiFeed.h"
#include "common/params/FxArpParams.h"
#include "common/shell/EditorState.h"
#include "common/ui/StepGrid.h"
#include "common/ui/controls/Panel.h"
#include "common/ui/controls/SegmentedSwitch.h"
#include "common/ui/controls/Slider.h"
#include "common/ui/graphs/PianoRollView.h"
#include "common/ui/models/ArpFeedModels.h"
#include "common/ui/models/ArpPreviewModel.h"

#include <array>

namespace vs::ui
{
class ArpSeqPage final : public juce::Component,
                         public InteractiveContent,
                         private ParamMirror::Listener,
                         private ParamMirror::IdleClient
{
public:
    ArpSeqPage (UiContext& ctx, UiFeed& feed, juce::AudioProcessorValueTreeState& state, shell::EditorState& editorState);
    ~ArpSeqPage() override;

    /** 0 Arpeggiator, 1 Sequencer: the view shown now. */
    int shownView() const noexcept { return view; }
    bool isOff() const noexcept { return off; }

    StepGrid& getGrid() noexcept { return grid; }
    PianoRollView& getRoll() noexcept { return roll; }
    SegmentedSwitch& getModeSwitch() noexcept { return modeSwitch; }
    SegmentedSwitch& getTargetSwitch() noexcept { return target; }
    SegmentedSwitch& getPatternSwitch() noexcept { return pattern; }
    SegmentedSwitch& getRateSwitch() noexcept { return rate; }
    Slider& getTempoSlider() noexcept { return tempo; }
    Slider& getLengthSlider() noexcept { return length; }
    Toggle& getHoldToggle() noexcept { return hold; }
    juce::Rectangle<int> panelBounds (int which) const; // 0 mode, 1 pattern, 2 timing, 3 preview, 4 steps

    /** The preview as shown, what it was made from, and how often it was computed (tests). */
    const model::ArpPreviewModel& preview() const noexcept { return previewModel; }
    const model::ArpPreviewInput& previewInput() const noexcept { return input; }
    bool previewUsesPlayerKeys() const noexcept { return usingKeys; }
    int previewComputations() const noexcept { return previewRuns; }
    /** The settings the engine reads for the mirror's current values (for the preview test). */
    ArpSettings engineSettings() const;
    const juce::String& previewSource() const noexcept { return sourceText; }
    const juce::String& hostCaption() const noexcept { return hostText; }

    void cancelInteraction() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;

private:
    void mirrorChanged (int index, bool valueChanged) override;
    void mirrorIdle (double nowMs) override;
    void updateState (double nowMs);
    void applyView();
    void applyInactive();
    void recomputePreview();
    void rebuildIcons();
    void layoutAll();

    UiContext& ctx;
    UiFeed& feed;
    shell::EditorState& editorState;
    ArpParamBinding binding;

    Panel patternPanel, timingPanel, previewPanel, stepsPanel;
    SegmentedSwitch modeSwitch, target, pattern, octaves, rate;
    Slider gate, swing, tempo, length;
    Toggle hold;
    StepGrid grid;
    PianoRollView roll;
    std::vector<BoundComponent*> controls; // everything but the mode switch (inactive while Off)

    // Inputs of the preview and the feed readers.
    int modeIdx = -1, targetIdx = -1, keyModeIdx = -1, splitIdx = -1;
    std::vector<int> previewInputs;
    model::ArpPreviewModel previewModel;
    model::ArpPreviewInput input;
    std::array<model::HeldKeysMemory, 2> memory;
    model::FeedAlive alive;
    bool previewDirty = true, usingKeys = false;
    int previewRuns = 0;
    juce::String sourceText, hostText, sentence;
    // Texts the page draws, made once (paint() allocates no strings).
    const juce::String playsThrough { "Plays through" };
    const juce::String legendRoll { "Each block = one note played. Bar length = Gate; every 2nd block shifts with Swing." };
    const juce::String legendRestTie { "R = rest (silent step), T = tie (hold the previous note, no new attack), neither = normal note." };
    const juce::String legendBars { "Bar heights: orange = pitch (up / down from the middle), teal = velocity. Steps past the length are dimmed." };
    std::array<juce::Path, 5> iconLines, iconDots; // the pattern cards' pictures, made when the layout changes

    int view = 0;
    bool off = true, hostPlaying = false;
    int hostBpm = 0;
    int modeNow = 0;
};
} // namespace vs::ui
