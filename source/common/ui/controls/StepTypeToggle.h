#pragma once

// StepTypeToggle (docs/architecture.md 9.2.2): one step's Note / Rest / Tie choice as two latching
// buttons, R and T. Pressing the lit one returns the step to Note. One gesture per press.

#include "common/ui/controls/BoundComponent.h"
#include "common/ui/models/SegmentModel.h"

namespace vs::ui
{
class StepTypeToggle final : public BoundComponent
{
public:
    /** `step` 1..16 (the parameter is seq.stepNN.type). */
    StepTypeToggle (UiContext& ctx, int step, int partIndex = -1);
    ~StepTypeToggle() override;

    int stepNumber() const noexcept { return step; }
    /** The type as drawn now: 0 Note, 1 Rest, 2 Tie. */
    int drawnType() const;
    bool isRest() const { return drawnType() == 1; }
    bool isTie() const { return drawnType() == 2; }
    juce::Rectangle<int> buttonBounds (int which) const; // 0 R, 1 T, in this component's coordinates
    /** What a click on R (0) or T (1) does (tests drive the buttons through the model). */
    void press (int which);
    void release();
    model::SegmentModel& getModel() noexcept { return model; }
    bool showsMirroredValue (juce::String& why) const override;
    void cancelInteraction() override { model.cancel(); }
    void setDimmed (bool dim);
    void openContextMenu() { showContextMenu(); }

    void resized() override;

private:
    class Button;
    void idle (double nowMs) override { model.idle (nowMs); }
    bool isInGesture() const override { return model.isInGesture(); }
    float shownValue() const override;
    void valueChanged() override;

    int step;
    model::SegmentModel model;
    std::unique_ptr<Button> rest, tie;
    bool dimmed = false;
};
} // namespace vs::ui
