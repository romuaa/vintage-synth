#pragma once

// Slider (docs/architecture.md 9.2.2): horizontal or vertical, unipolar or bipolar from the
// centre; label, value text (the parameter's own), track and thumb in the part accent.

#include "common/ui/PreparedText.h"
#include "common/ui/controls/BoundComponent.h"
#include "common/ui/models/SliderModel.h"

namespace vs::ui
{
class Slider final : public BoundComponent
{
public:
    enum class Style
    {
        Horizontal, // label and value above the track
        Vertical,   // value above, label below
        Compact     // track only with the value inside the row (top bar)
    };

    Slider (UiContext& ctx, const juce::String& paramId, const juce::String& label, int part, Style style = Style::Horizontal,
            bool bipolar = false, Placement placement = Placement::Primary);
    ~Slider() override;

    float shownValue() const override;
    model::SliderModel& getModel() noexcept { return model; }
    /** Track geometry in this component's coordinates (for tests). */
    juce::Rectangle<float> trackBounds() const noexcept { return track; }
    juce::Rectangle<float> thumbBounds() const;
    /** The label, value and AUTO badge areas as drawn now (the badge is empty while no marker shows). */
    juce::Rectangle<float> labelBounds() const noexcept;
    juce::Rectangle<float> valueBounds() const noexcept { return valueArea; }
    /** Width of the value-text slot of a horizontal slider (default 84 px; a short value can use less). */
    void setValueSlotWidth (float px) { valueSlot = px; resized(); }
    juce::Rectangle<float> badgeBounds() const noexcept;
    void cancelInteraction() override { model.cancel(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void valueChanged() override;
    void idle (double nowMs) override { model.idle (nowMs); }
    bool isInGesture() const override { return model.isInGesture(); }
    float pxOf (juce::Point<float> p) const noexcept;
    /** Lays the label and the value text out again when what they depend on changed (paint() only draws them). */
    void prepareText();

    Style style;
    bool bipolar;
    model::SliderModel model;
    juce::Rectangle<float> track, labelArea, valueArea, badgeArea;
    float valueSlot = 84.0f;
    PreparedText labelPrepared, valuePrepared;
    juce::Rectangle<float> preparedLabelArea, preparedValueArea;
    juce::String preparedValue;
};
} // namespace vs::ui
