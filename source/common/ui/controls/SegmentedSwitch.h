#pragma once

// SegmentedSwitch and Toggle (docs/architecture.md 9.2.2). A segmented switch shows one segment
// per value of a choice or stepped parameter (captions = the choice labels unless the page gives
// aliases, which must be as many as the choices); a toggle is a latching Off/On button.

#include "common/ui/controls/BoundComponent.h"
#include "common/ui/models/SegmentModel.h"

namespace vs::ui
{
class SegmentedSwitch final : public BoundComponent
{
public:
    /** `captions` empty: the parameter's own choice labels. `label` empty: no label row. */
    SegmentedSwitch (UiContext& ctx, const juce::String& paramId, const juce::String& label, int part,
                     juce::StringArray captions = {}, Placement placement = Placement::Primary);
    ~SegmentedSwitch() override;

    int numSegments() const noexcept { return model.numSegments(); }
    /** The lit segment as shown now. */
    int litSegment() const;
    const juce::StringArray& getCaptions() const noexcept { return captions; }
    juce::Rectangle<float> segmentBounds (int index) const;
    model::SegmentModel& getModel() noexcept { return model; }
    bool showsMirroredValue (juce::String& why) const override;
    void cancelInteraction() override { model.cancel(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void idle (double nowMs) override { model.idle (nowMs); }
    bool isInGesture() const override { return model.isInGesture(); }
    float shownValue() const override;

    juce::StringArray captions;
    model::SegmentModel model;
    juce::Rectangle<float> labelArea, row;
};

class Toggle final : public BoundComponent
{
public:
    Toggle (UiContext& ctx, const juce::String& paramId, const juce::String& caption, int part, Placement placement = Placement::Primary);
    ~Toggle() override;

    bool isOn() const;
    model::ToggleModel& getModel() noexcept { return model; }
    bool showsMirroredValue (juce::String& why) const override;
    void cancelInteraction() override { model.cancel(); }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void idle (double nowMs) override { model.idle (nowMs); }
    float shownValue() const override;

    model::ToggleModel model;
};
} // namespace vs::ui
