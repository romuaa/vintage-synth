#pragma once

// SegmentedSwitch and Toggle (docs/architecture.md 9.2.2). A segmented switch shows one segment
// per value of a choice or stepped parameter (captions = the choice labels unless the page gives
// aliases, which must be as many as the choices); a toggle is a latching Off/On button.

#include "common/ui/controls/BoundComponent.h"
#include "common/ui/models/SegmentModel.h"

#include <functional>

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

    /** Optional decoration of a segment (pattern icons, per-segment colours): called after the
        lit fill and before the caption, with the segment's area. `cards`: the segments are as tall
        as the control (below the label) and the caption sits at the bottom of each, leaving the
        rest to the painter. Called from paint(): it must not allocate. */
    using SegmentPainter = std::function<void (juce::Graphics&, juce::Rectangle<float> segment, int index, bool lit)>;
    void setSegmentPainter (SegmentPainter painter, bool cards = false);
    /** A larger switch (the Arp + Seq mode switch): caption size and row height in logical px. */
    void setLargeStyle (float captionPx, int rowHeightPx);

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
    SegmentPainter painter;
    bool cardLayout = false;
    float captionPx = metrics::kText;
    int rowHeightPx = metrics::kSegment;
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
    void valueChanged() override;
    float shownValue() const override;

    model::ToggleModel model;
};
} // namespace vs::ui
