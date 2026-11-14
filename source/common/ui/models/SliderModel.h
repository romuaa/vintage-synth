#pragma once

// Interaction logic of a slider (docs/architecture.md 9.2.1). JUCE-free. Positions are pixels
// along the value axis, increasing with the value (the component maps a vertical track's
// upward direction to increasing pixels).

#include "common/ui/models/ParamTarget.h"

namespace vs::ui::model
{
class SliderModel
{
public:
    static constexpr float kFine = 0.1f;   // Shift / Cmd (Ctrl): x 0.1
    static constexpr float kWheelStep = 0.01f, kWheelFineStep = 0.001f;

    explicit SliderModel (ParamTarget& t) noexcept : target (t) {}

    /** Track: value 0 at `startPx`, value 1 at `startPx + lengthPx`. */
    void setTrack (float startPx, float lengthPx) noexcept
    {
        start = startPx;
        length = lengthPx > 1.0f ? lengthPx : 1.0f;
    }
    float positionOf (float v) const noexcept { return start + clamp01 (v) * length; }
    float valueAt (float px) const noexcept { return clamp01 ((px - start) / length); }

    /** Mouse-down: the gesture begins. A press beside the thumb jumps there first. */
    void mouseDown (float px, bool onThumb, bool fine)
    {
        cancel(); // a second press without a release (lost mouse-up) ends the first drag
        target.beginGesture();
        dragging = true;
        raw = target.value();
        if (! onThumb)
            raw = valueAt (px);
        shown = snap (raw, target.numSteps());
        if (! onThumb)
            write (shown);
        anchorPx = px;
        anchorRaw = raw;
        fineNow = fine;
    }

    /** Relative drag: the full track length is the full range (x 0.1 with the fine modifier). */
    void mouseDrag (float px, bool fine)
    {
        if (! dragging)
            return;
        if (fine != fineNow)
        {
            // Changing the modifier mid-drag continues from where the value is now.
            anchorPx = px;
            anchorRaw = raw;
            fineNow = fine;
        }
        raw = clamp01 (anchorRaw + (px - anchorPx) / length * (fine ? kFine : 1.0f));
        write (snap (raw, target.numSteps()));
    }

    void mouseUp()
    {
        if (! dragging)
            return;
        dragging = false;
        target.endGesture();
    }

    void doubleClick()
    {
        cancel();
        writeOnce (target, target.defaultValue());
    }

    /** Mouse wheel over the control: 1 % per notch (0.1 % fine), one step for discrete parameters. */
    void wheel (float notches, bool fine, double nowMs)
    {
        if (dragging || ! std::isfinite (notches) || std::fpclassify (notches) == FP_ZERO)
            return;
        const int steps = target.numSteps();
        const float base = target.value();
        float v;
        if (steps >= 2)
            v = snap (base + (notches > 0.0f ? 1.0f : -1.0f) / static_cast<float> (steps - 1), steps);
        else
            v = clamp01 (base + notches * (fine ? kWheelFineStep : kWheelStep));
        timed.touch (target, nowMs);
        write (v);
    }

    void key (Key k, bool fine, double nowMs)
    {
        if (dragging)
            return;
        timed.touch (target, nowMs);
        write (keyValue (k, target.value(), target.defaultValue(), target.numSteps(), fine));
    }

    /** Typed entry (already parsed by the parameter): begin, value, end. */
    void typed (float normalised)
    {
        cancel();
        writeOnce (target, normalised);
    }

    /** Called on every tick: ends a wheel/key gesture 300 ms after the last notch or key. */
    void idle (double nowMs) { timed.idle (target, nowMs); }

    /** Ends every open gesture (component destroyed or hidden mid-interaction). */
    void cancel()
    {
        if (dragging)
            mouseUp();
        endTimed();
    }

    bool isDragging() const noexcept { return dragging; }
    bool isInGesture() const noexcept { return dragging || timed.isOpen(); }
    /** The user's value during a drag (shown even if the host writes something else meanwhile). */
    float dragValue() const noexcept { return shown; }

private:
    void write (float v)
    {
        shown = v;
        target.setValue (v);
    }
    void endTimed() { timed.close (target); }

    ParamTarget& target;
    TimedGesture timed;
    float start = 0.0f, length = 100.0f;
    bool dragging = false, fineNow = false;
    float raw = 0.0f, anchorRaw = 0.0f, anchorPx = 0.0f, shown = 0.0f;
};
} // namespace vs::ui::model
