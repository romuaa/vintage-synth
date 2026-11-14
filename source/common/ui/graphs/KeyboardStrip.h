#pragma once

// KeyboardStrip (docs/architecture.md 9.2.2, 9.3.4): the Play page's keyboard. Six octaves, keys
// coloured by the part that plays them (the key mode's rule), bars above the keys that name each
// part's range, the split marker (a slider on `key.split_point`: press or drag on the keys, arrows,
// wheel) and dots for the keys held (filled) and sounding (ring) from the UI feed. Everything it
// draws is prepared when the value, the key mode or the feed's masks change; paint() only draws.

#include "common/engine/UiFeed.h"
#include "common/ui/controls/BoundComponent.h"
#include "common/ui/models/KeyboardModel.h"

#include <array>

namespace vs::ui
{
class KeyboardStrip final : public BoundComponent
{
public:
    static constexpr int kPillRow = 26, kBarRow = 30;

    KeyboardStrip (UiContext& ctx, const UiFeed& feed, const juce::String& splitId = "key.split_point", const juce::String& modeId = "key.mode");
    ~KeyboardStrip() override;

    /** What is drawn now (tests). */
    int drawnKeyMode() const noexcept { return mode; }
    int drawnSplitNote() const noexcept { return splitNote; }
    int windowLow() const noexcept { return geometry.low(); }
    const model::KeyboardGeometry& getGeometry() const noexcept { return geometry; }
    model::SplitKeyModel& getModel() noexcept { return model; }
    bool drawsHeld (int partIndex, int note) const noexcept;
    bool drawsSounding (int partIndex, int note) const noexcept;
    /** The key area and the marker (the draggable pill) in this component's coordinates. */
    juce::Rectangle<float> keysArea() const noexcept { return keys; }
    juce::Rectangle<float> pillBounds() const noexcept { return pill; }
    const juce::String& barText (int partIndex) const noexcept { return bars[static_cast<size_t> (partIndex & 1)].text; }
    int repaintCount() const noexcept { return repaints; }

    float shownValue() const override;
    bool showsMirroredValue (juce::String& why) const override;
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

    void mirrorChanged (int index, bool valueChanged) override;

private:
    struct Bar
    {
        juce::String text;
        model::NoteSpan span;
    };
    using Masks = std::array<std::uint64_t, 8>; // held lower, upper, sounding lower, upper: two words each

    void valueChanged() override;
    void idle (double nowMs) override;
    bool isInGesture() const override { return model.isInGesture(); }
    void prepare();
    juce::Point<float> local (const juce::MouseEvent& e) const noexcept;
    static bool has (const Masks& m, int slot, int note) noexcept;
    juce::Rectangle<float> keyBounds (int note) const noexcept;
    void paintKey (juce::Graphics&, int note) const;
    void paintDots (juce::Graphics&, int note) const;

    const UiFeed& feed;
    int modeIndex = -1;
    model::SplitKeyModel model;
    model::KeyboardGeometry geometry;
    juce::Rectangle<float> keys, barArea, pill;
    int mode = 0, splitNote = 60;
    bool ownNoteValid = false;
    int ownNote = 0;
    std::array<Bar, 2> bars;
    juce::StringArray octaveLabels;
    juce::String pillText;
    Masks masks {};
    int repaints = 0;
};
} // namespace vs::ui
