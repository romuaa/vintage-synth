#pragma once

// The keyboard strip's geometry and interaction (docs/architecture.md 9.2.2, 9.3.4). JUCE-free.
//
// The strip shows six octaves, C to C (73 keys, 43 white), as a window of the 128 MIDI notes. The
// default window is C1-C7 (MIDI 24-96, middle C = C4). It shifts by whole octaves so the split key
// is always visible with at least one octave to spare on each side where the keyboard allows it.
// Ownership follows the KeyRouter's rule (common/engine/KeyRouter.h): Whole = every key Lower, Dual
// = every key both, Split = keys below the split point Lower, the rest Upper.

#include "common/ui/models/ParamTarget.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace vs::ui::model
{
struct KeyRect
{
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool contains (float px, float py) const noexcept { return px >= x && px < x + w && py >= y && py < y + h; }
};

inline bool isBlackKey (int note) noexcept
{
    const int s = ((note % 12) + 12) % 12;
    return s == 1 || s == 3 || s == 6 || s == 8 || s == 10;
}

/** "C4", "F#2": middle C (MIDI 60) is C4, as the parameter's own text shows it. */
inline std::string noteLabel (int note)
{
    static constexpr const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int n = note < 0 ? 0 : (note > 127 ? 127 : note);
    return std::string (names[n % 12]) + std::to_string (n / 12 - 1);
}

enum class KeyOwner : int
{
    None = 0,
    Lower = 1,
    Upper = 2,
    Both = 3
};

/** `keyMode`: 0 Whole, 1 Dual, 2 Split. */
inline int ownerMask (int keyMode, int splitPoint, int note) noexcept
{
    if (keyMode == 1)
        return 3;
    if (keyMode == 2)
        return note < splitPoint ? 1 : 2;
    return 1;
}

/** The first and last note a part plays inside a window (first > last: it plays none of them). */
struct NoteSpan
{
    int first = 0, last = -1;
    bool empty() const noexcept { return first > last; }
};

inline NoteSpan partSpan (int part, int keyMode, int splitPoint, int lo, int hi) noexcept
{
    if (keyMode == 1)
        return { lo, hi };
    if (keyMode == 2)
        return part == 0 ? NoteSpan { lo, std::min (hi, splitPoint - 1) } : NoteSpan { std::max (lo, splitPoint), hi };
    return part == 0 ? NoteSpan { lo, hi } : NoteSpan {};
}

class KeyboardGeometry
{
public:
    static constexpr int kSpan = 72;       // semitones between the window's first and last key (C to C)
    static constexpr int kDefaultLow = 24; // C1
    static constexpr int kWhiteKeys = 43;

    /** The window start (a C, multiple of 12) that shows `splitPoint` with an octave to spare. */
    static int windowFor (int splitPoint) noexcept
    {
        int lo = kDefaultLow;
        while (splitPoint < lo + 12 && lo >= 12)
            lo -= 12;
        while (splitPoint > lo + kSpan - 12 && lo + 12 + kSpan <= 127)
            lo += 12;
        return lo;
    }

    void setSize (float widthPx, float heightPx) noexcept
    {
        width = widthPx > 1.0f ? widthPx : 1.0f;
        height = heightPx > 1.0f ? heightPx : 1.0f;
    }
    void setWindow (int low) noexcept { lo = low; }
    int low() const noexcept { return lo; }
    int high() const noexcept { return lo + kSpan; }
    bool inWindow (int note) const noexcept { return note >= lo && note <= lo + kSpan; }
    float whiteWidth() const noexcept { return width / static_cast<float> (kWhiteKeys); }
    float blackWidth() const noexcept { return whiteWidth() * 0.62f; }
    float blackHeight() const noexcept { return height * 0.62f; }

    /** The key's rectangle (a black key's covers only its upper part). Notes outside the window: empty. */
    KeyRect keyRect (int note) const noexcept
    {
        if (! inWindow (note))
            return {};
        const float ww = whiteWidth();
        const int w = whiteIndex (note) - whiteIndex (lo);
        if (isBlackKey (note))
            return { (static_cast<float> (w) + 1.0f) * ww - blackWidth() * 0.5f, 0.0f, blackWidth(), blackHeight() };
        return { static_cast<float> (w) * ww, 0.0f, ww, height };
    }

    /** The key under a point (black keys first); -1 if none. */
    int noteAt (float x, float y) const noexcept
    {
        if (! (x >= 0.0f) || ! (x < width) || ! (y >= 0.0f) || ! (y < height))
            return -1;
        for (int n = lo; n <= lo + kSpan; ++n)
            if (isBlackKey (n) && keyRect (n).contains (x, y))
                return n;
        for (int n = lo; n <= lo + kSpan; ++n)
            if (! isBlackKey (n) && keyRect (n).contains (x, y))
                return n;
        return -1;
    }

    /** Dragging: the key under x, the pointer's height clamped onto the keys (black keys win where
        the pointer is over their upper part); beyond the ends the first or last key. */
    int noteNearest (float x, float y) const noexcept
    {
        const float cx = std::fmin (std::fmax (x, 0.0f), width - 0.001f);
        const float cy = std::fmin (std::fmax (y, 0.0f), height - 0.001f);
        const int n = noteAt (cx, cy);
        return n >= 0 ? n : lo;
    }

    /** Left edge of the key at which the split marker stands (clamped into the window). */
    float markerX (int note) const noexcept
    {
        const int n = note < lo ? lo : (note > lo + kSpan ? lo + kSpan : note);
        return keyRect (n).x;
    }

    float getWidth() const noexcept { return width; }
    float getHeight() const noexcept { return height; }

private:
    static int whiteIndex (int note) noexcept
    {
        // Index of the note's white key within its octave (a black key: the white key to its left).
        static constexpr int below[12] { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };
        return (note / 12) * 7 + below[note % 12];
    }

    float width = 100.0f, height = 100.0f;
    int lo = kDefaultLow;
};

/**
    The split key's interaction: a press on a key sets the split point there (one gesture from
    mouse-down to mouse-up, dragging moves it), the arrow keys and the wheel move it by one key,
    Page Up/Down by an octave, Home/End to 0 / 127, double-click resets. The parameter is stepped
    (0..127, normalised note / 127).
*/
class SplitKeyModel
{
public:
    explicit SplitKeyModel (ParamTarget& t) noexcept : target (t) {}

    static float valueOf (int note) noexcept { return clamp01 (static_cast<float> (note) / 127.0f); }
    static int noteOf (float v) noexcept { return static_cast<int> (std::lround (clamp01 (v) * 127.0f)); }
    int note() const noexcept { return noteOf (target.value()); }

    void press (int noteValue)
    {
        cancel();
        target.beginGesture();
        pressed = true;
        shown = clampNote (noteValue);
        target.setValue (valueOf (shown));
    }
    void drag (int noteValue)
    {
        if (! pressed)
            return;
        shown = clampNote (noteValue);
        target.setValue (valueOf (shown));
    }
    void release()
    {
        if (! pressed)
            return;
        pressed = false;
        target.endGesture();
    }
    void doubleClick()
    {
        cancel();
        writeOnce (target, target.defaultValue());
    }
    void typed (float normalised)
    {
        cancel();
        writeOnce (target, normalised);
    }
    /** `semitones`: +-1 arrows, +-12 Page keys. Held keys and the wheel share one timed gesture. */
    void nudge (int semitones, double nowMs)
    {
        if (pressed || semitones == 0)
            return;
        timed.touch (target, nowMs);
        target.setValue (valueOf (clampNote (note() + semitones)));
    }
    void toEnd (bool top, double nowMs)
    {
        if (pressed)
            return;
        timed.touch (target, nowMs);
        target.setValue (top ? 1.0f : 0.0f);
    }
    void wheel (float notches, double nowMs)
    {
        if (pressed || ! std::isfinite (notches))
            return;
        accumulator += notches;
        const float whole = std::trunc (accumulator);
        if (std::fpclassify (whole) == FP_ZERO)
            return;
        accumulator -= whole;
        nudge (static_cast<int> (whole), nowMs);
    }
    void idle (double nowMs)
    {
        timed.idle (target, nowMs);
        if (! timed.isOpen())
            accumulator = 0.0f;
    }
    void cancel()
    {
        release();
        timed.close (target);
    }
    bool isDragging() const noexcept { return pressed; }
    bool isInGesture() const noexcept { return pressed || timed.isOpen(); }
    /** The user's note during a drag (shown even if the host writes something else meanwhile). */
    int dragNote() const noexcept { return shown; }

private:
    static int clampNote (int n) noexcept { return n < 0 ? 0 : (n > 127 ? 127 : n); }

    ParamTarget& target;
    TimedGesture timed;
    bool pressed = false;
    int shown = 0;
    float accumulator = 0.0f;
};
} // namespace vs::ui::model
