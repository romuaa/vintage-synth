#pragma once

// StepGrid (docs/architecture.md 9.2.2, 9.3.5): the sequencer's 16 steps as a picture that is edited
// directly. A row of pitch bars (drag y, 3 px per semitone, Shift snaps to octaves), a row of
// velocity bars (click or drag y), the R / T buttons of each step, the step numbers and the length
// flag (drag x, snaps to a step boundary). A drag across columns paints every crossed step. The
// step that plays is outlined; steps beyond the length are dimmed and stay editable.
//
// Bindings: the 16 pitch and 16 velocity bars are the primary controls of seq.stepNN.note and
// .velocity, the 16 StepTypeToggles of .type; the flag is a secondary binding of seq.length (its
// primary is the Length slider on the page). The interaction logic is models/StepGridModel.h.

#include "common/ui/controls/StepTypeToggle.h"
#include "common/ui/models/SliderModel.h"
#include "common/ui/models/StepGridModel.h"

#include <array>
#include <memory>
#include <vector>

namespace vs::ui
{
class StepGrid final : public juce::Component, public InteractiveContent, private ParamMirror::IdleClient
{
public:
    // Layout in logical pixels (the width is whatever the page gives; the height is fixed).
    static constexpr int kLabelW = 96, kValueRow = 18, kPitchH = 144, kGap = 10, kVelH = 64, kTypeH = 26, kNumberH = 18, kFlagH = 28;
    static constexpr int kHeight = kValueRow + kPitchH + kGap + kVelH + kGap + kTypeH + 2 + kNumberH + 2 + kFlagH;

    class Bar;
    class LengthFlag;

    explicit StepGrid (UiContext& ctx);
    /** The type of a step as the mirror holds it (0 Note, 1 Rest, 2 Tie). */
    int mirroredType (int step) const;
    ~StepGrid() override;

    /** The highlighted step (0..15) or -1. Called by the page from the feed. */
    void setPlayingStep (int step);
    int playingStep() const noexcept { return playing; }
    /** Steps of the pattern as the grid shows them (the flag's value, live while it is dragged). */
    int shownLength() const noexcept { return length; }
    /** Arp + Seq mode Off: everything dimmed (and inactive for assistive technology), still editable. */
    void setOff (bool isOff);
    bool isOff() const noexcept { return off; }

    int columnWidth() const noexcept { return colW; }
    juce::Rectangle<int> pitchBounds (int step) const;
    juce::Rectangle<int> velocityBounds (int step) const;
    juce::Rectangle<int> typeBounds (int step) const;
    /** The bar components (tests drive their mouse handlers; the pictures below read what they draw). */
    juce::Component& pitchBar (int step) noexcept;
    juce::Component& velocityBar (int step) noexcept;
    /** The semitones the pitch bar of a step draws; where a bar's value sits (grid y); the text it shows. */
    int drawnSemitones (int step) const;
    float barValueY (int step, bool pitchBarNotVelocity) const;
    juce::String barText (int step, bool pitchBarNotVelocity) const;
    StepTypeToggle& typeToggle (int step) noexcept { return *types[static_cast<size_t> (step)]; }
    BoundComponent& lengthFlag() noexcept;
    model::StepGridModel& getModel() noexcept { return model; }
    bool stepIsDimmed (int step) const noexcept { return step >= length; }

    // ---- Used by the bars and the flag ----
    void pressBar (model::StepGridModel::Lane lane, juce::Point<float> inGrid, bool octaves);
    void dragBar (juce::Point<float> inGrid, bool octaves);
    void releaseBars();
    void resetBar (int step, int field);
    void lengthShown (int newLength);
    juce::Point<float> toGrid (const juce::MouseEvent& e);

    void cancelInteraction() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void mirrorIdle (double) override {}
    void layoutFlag();
    void refreshOpen();
    void rebuildOverlay();

    /** Repaints the pitch bar of a step whose type changed (a rest shows no pitch). */
    struct TypeListener final : ParamMirror::Listener
    {
        StepGrid* owner = nullptr;
        std::array<int, 16> indices {};
        void mirrorChanged (int index, bool) override;
    };
    static std::vector<std::unique_ptr<ParamHandle>> makeHandles (ParamMirror& mirror);

    UiContext& ctx;
    TypeListener typeListener;
    juce::StringArray numbers, rowLabels;
    std::vector<std::unique_ptr<ParamHandle>> handles; // 49 targets, the model writes through them (shared gesture counters in the mirror)
    model::StepGridModel model;
    std::array<std::unique_ptr<Bar>, 16> pitch, velocity;
    std::array<std::unique_ptr<StepTypeToggle>, 16> types;
    std::unique_ptr<LengthFlag> flag;
    juce::Path lengthDash, playMark;
    bool off = false;
    int colW = 60, length = 16, playing = -1;
};
} // namespace vs::ui
