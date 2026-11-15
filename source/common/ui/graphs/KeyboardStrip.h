#pragma once

// KeyboardStrip (docs/architecture.md 9.2.2, 9.3.4): the Play page's keyboard. Six octaves, keys
// coloured by the part that plays them (the key mode's rule), bars above the keys that name each
// part's range, the split marker (a slider on `key.split_point`: drag its pill, arrows, wheel,
// double-click the pill) and the keys held (tinted in the holding part's colour) and sounding (a small bar at the foot of the key) from the UI feed.
// A press on the keys plays them (9.3.4, "Playing the keys"): one note per mouse through UiNotes,
// dragging glides from key to key, velocity by the height in the key; the note is released on
// mouse-up, when the strip or its page is hidden, when the editor stops showing (its timer) or is
// destroyed, and by cancelInteraction(). Everything it draws
// is prepared when the value, the key mode or the feed's masks change; paint() only draws.

#include "common/engine/UiFeed.h"
#include "common/engine/UiNotes.h"
#include "common/ui/PaintFonts.h"
#include "common/ui/controls/BoundComponent.h"
#include "common/ui/models/KeyboardModel.h"

#include <array>

namespace vs::ui
{
class KeyboardStrip final : public BoundComponent
{
public:
    static constexpr int kPillRow = 26, kBarRow = 30;

    /** `notes`: where the keys play (the processor's UiNotes); nullptr: the keys play nothing. */
    KeyboardStrip (UiContext& ctx, const UiFeed& feed, UiNotes* notes, const juce::String& splitId = "key.split_point",
                   const juce::String& modeId = "key.mode");
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
    /** The note this strip's mouse plays now (-1: none) and its velocity (tests). */
    int playingNote() const noexcept { return playing; }
    int playingVelocity() const noexcept { return playingVel; }

    float shownValue() const override;
    bool showsMirroredValue (juce::String& why) const override;
    void cancelInteraction() override;
    /** Ends a press on the keys: its note is released and the press plays nothing more until the
        next mouse-down (cancel, hide, the editor no longer showing). */
    void endPlaying();

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
    void visibilityChanged() override;
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
    void paintSounding (juce::Graphics&, int note) const;
    /** The keys' note at `p` (keys' coordinates) or -1; plays it instead of the current note. */
    void playAt (juce::Point<float> p);
    void releaseNote();
    bool shownInTree() const noexcept;

    const UiFeed& feed;
    UiNotes* notes = nullptr;
    int playing = -1, playingVel = 0; // the note the mouse plays now
    bool playGesture = false;         // mouse-down on the keys until mouse-up
    float grabOffset = 0.0f;          // pill drag: pointer x minus the marker's x at the press
    int modeIndex = -1;
    model::SplitKeyModel model;
    model::KeyboardGeometry geometry;
    juce::Rectangle<float> keys, barArea, pill;
    int mode = 0, splitNote = 60;
    bool ownNoteValid = false;
    int ownNote = 0;
    std::array<Bar, 2> bars;
    juce::StringArray octaveLabels;
    juce::String pillText, dragHint;
    juce::Path markerTriangle;
    PaintFonts fonts;
    Masks masks {};
    int repaints = 0;
};
} // namespace vs::ui
