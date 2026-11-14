#pragma once

// Slider (docs/architecture.md 9.2.2): horizontal or vertical, unipolar or bipolar from the
// centre; label, value text (the parameter's own), track and thumb in the part accent.

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

    Style style;
    bool bipolar;
    model::SliderModel model;
    juce::Rectangle<float> track, labelArea, valueArea;
    juce::String valueText; // prepared on change
};
} // namespace vs::ui
