#pragma once

// Interaction logic of a segmented switch and a toggle (docs/architecture.md 9.2.1, 9.2.2).
// JUCE-free. A segment press is one gesture: begin at mouse-down, one value, end at mouse-up.

#include "common/ui/models/ParamTarget.h"

namespace vs::ui::model
{
class SegmentModel
{
public:
    SegmentModel (ParamTarget& t, int numSegments) noexcept : target (t), count (numSegments > 1 ? numSegments : 1) {}

    int numSegments() const noexcept { return count; }
    /** Segment under `x` for equal segments over `width` px; -1 outside. */
    int indexAt (float x, float width) const noexcept
    {
        if (! (x >= 0.0f) || ! (x < width) || width <= 0.0f)
            return -1;
        const int i = static_cast<int> (x / width * static_cast<float> (count));
        return i < count ? i : count - 1;
    }
    float valueOf (int index) const noexcept
    {
        return count < 2 ? 0.0f : clamp01 (static_cast<float> (index) / static_cast<float> (count - 1));
    }
    /** The lit segment for a normalised value. */
    int indexOf (float v) const noexcept { return stepIndex (v, count); }
    int index() const noexcept { return indexOf (target.value()); }

    void press (int index)
    {
        if (index < 0 || index >= count)
            return;
        timed.close (target);
        if (pressed)
            release();
        target.beginGesture();
        pressed = true;
        target.setValue (valueOf (index));
    }
    void release()
    {
        if (! pressed)
            return;
        pressed = false;
        target.endGesture();
    }

    /** Arrow keys / wheel: the neighbouring segment (no wrap). */
    void step (int delta, double nowMs)
    {
        if (pressed || delta == 0)
            return;
        const int i = index() + (delta > 0 ? 1 : -1);
        if (i < 0 || i >= count)
            return;
        timed.touch (target, nowMs);
        target.setValue (valueOf (i));
    }
    void key (Key k, double nowMs)
    {
        if (pressed)
            return;
        if (k == Key::Increase)
            return step (1, nowMs);
        if (k == Key::Decrease)
            return step (-1, nowMs);
        timed.touch (target, nowMs);
        target.setValue (snap (keyValue (k, target.value(), target.defaultValue(), count, false), count));
    }
    void doubleClick()
    {
        cancel();
        writeOnce (target, target.defaultValue());
    }
    void idle (double nowMs) { timed.idle (target, nowMs); }
    void cancel()
    {
        release();
        timed.close (target);
    }
    bool isInGesture() const noexcept { return pressed || timed.isOpen(); }

private:
    ParamTarget& target;
    int count;
    bool pressed = false;
    TimedGesture timed;
};

/** A latching Off/On button: a press flips the value (one gesture: mouse-down to mouse-up). */
class ToggleModel
{
public:
    explicit ToggleModel (ParamTarget& t) noexcept : segments (t, 2), target (t) {}
    bool isOn() const noexcept { return segments.index() == 1; }
    void press() { segments.press (isOn() ? 0 : 1); }
    void release() { segments.release(); }
    void key (Key k, double nowMs)
    {
        if (k == Key::Reset || k == Key::Home || k == Key::End)
            segments.key (k, nowMs);
        else
            segments.step (k == Key::Increase || k == Key::PageUp ? 1 : -1, nowMs);
    }
    void doubleClick() { segments.doubleClick(); }
    void idle (double nowMs) { segments.idle (nowMs); }
    void cancel() { segments.cancel(); }
    bool isInGesture() const noexcept { return segments.isInGesture(); }
    ParamTarget& getTarget() noexcept { return target; }

private:
    SegmentModel segments;
    ParamTarget& target;
};
} // namespace vs::ui::model
