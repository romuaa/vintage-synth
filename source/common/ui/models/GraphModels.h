#pragma once

// Graph geometry and handle dragging (docs/architecture.md 9.2.2, 9.3.1). JUCE-free.

#include "common/ui/models/ParamTarget.h"

#include <array>
#include <cmath>
#include <functional>

namespace vs::ui::model
{
/** Logarithmic axis: `lo` at `start` px, `hi` at `start + length` px. */
struct LogAxis
{
    float start = 0.0f, length = 100.0f;
    double lo = 20.0, hi = 20000.0;

    float positionOf (double v) const noexcept
    {
        const double t = std::log (v / lo) / std::log (hi / lo);
        return start + static_cast<float> (t) * length;
    }
    double valueAt (float px) const noexcept
    {
        const double t = static_cast<double> ((px - start) / length);
        return lo * std::pow (hi / lo, t);
    }
    float clampPx (float px) const noexcept { return std::clamp (px, start, start + length); }
};

/** Linear axis: `top` at `start` px (screen top), `bottom` at `start + length` px. */
struct LinearAxis
{
    float start = 0.0f, length = 100.0f;
    double top = 24.0, bottom = -48.0;

    float positionOf (double v) const noexcept
    {
        return start + static_cast<float> ((top - v) / (top - bottom)) * length;
    }
    double valueAt (float px) const noexcept { return top - static_cast<double> ((px - start) / length) * (top - bottom); }
};

/**
    Dragging one graph handle that writes up to two parameters (docs/architecture.md 9.1.5):
    x through an absolute mapping of the handle's position (the grab offset is kept; the fine
    modifier scales the pointer's motion by 0.1), y relative (up = more, `pxPerRange` = the full
    range). Each parameter's gesture begins when the drag first changes it and all end at mouse-up.
*/
class HandleModel
{
public:
    using PxToValue = std::function<float (float px)>;

    void setX (ParamTarget* t, PxToValue map)
    {
        axes[0].target = t;
        xMap = std::move (map);
    }
    void setY (ParamTarget* t, float pxPerRange)
    {
        axes[1].target = t;
        yPxPerRange = pxPerRange > 1.0f ? pxPerRange : 1.0f;
    }
    ParamTarget* xTarget() const noexcept { return axes[0].target; }
    ParamTarget* yTarget() const noexcept { return axes[1].target; }

    /** `handleX`: where the handle is drawn now (px). */
    void mouseDown (float x, float y, float handleX, bool fine)
    {
        cancel();
        dragging = true;
        anchorX = x;
        anchorY = y;
        anchorHandleX = lastHandleX = handleX;
        fineNow = fine;
        if (axes[1].target != nullptr)
            anchorYValue = lastY = axes[1].target->value();
    }

    void mouseDrag (float x, float y, bool fine)
    {
        if (! dragging)
            return;
        const float scale = fine ? 0.1f : 1.0f;
        if (fine != fineNow)
        {
            // Re-anchor so the handle does not jump when the modifier changes.
            anchorHandleX = lastHandleX;
            anchorX = x;
            if (axes[1].target != nullptr)
                anchorYValue = lastY;
            anchorY = y;
            fineNow = fine;
        }
        lastHandleX = anchorHandleX + (x - anchorX) * scale;
        if (axes[0].target != nullptr && xMap)
            write (0, xMap (lastHandleX));
        if (axes[1].target != nullptr)
        {
            lastY = clamp01 (anchorYValue + (anchorY - y) / yPxPerRange * scale);
            write (1, lastY);
        }
    }

    void mouseUp()
    {
        if (! dragging)
            return;
        dragging = false;
        for (auto& a : axes)
            if (a.touched)
            {
                a.touched = false;
                a.target->endGesture();
            }
    }

    /** Double-click: every parameter of the handle back to its default. */
    void doubleClick()
    {
        cancel();
        for (auto& a : axes)
            if (a.target != nullptr)
                writeOnce (*a.target, a.target->defaultValue());
    }

    /** Arrow keys on a focused handle: dx moves x, dy moves y, by 1 % (0.1 % fine) of the range. */
    void nudge (int dx, int dy, bool fine, double nowMs)
    {
        if (dragging)
            return;
        const float stepSize = fine ? 0.001f : 0.01f;
        for (int i = 0; i < 2; ++i)
        {
            const int d = i == 0 ? dx : dy;
            auto& a = axes[static_cast<size_t> (i)];
            if (d == 0 || a.target == nullptr)
                continue;
            a.timed.touch (*a.target, nowMs);
            a.target->setValue (snap (a.target->value() + static_cast<float> (d) * stepSize, a.target->numSteps()));
        }
    }
    void idle (double nowMs)
    {
        for (auto& a : axes)
            if (a.target != nullptr)
                a.timed.idle (*a.target, nowMs);
    }
    void cancel()
    {
        mouseUp();
        for (auto& a : axes)
            if (a.target != nullptr)
                a.timed.close (*a.target);
    }
    bool isDragging() const noexcept { return dragging; }
    bool isInGesture() const noexcept { return dragging || axes[0].timed.isOpen() || axes[1].timed.isOpen(); }
    /** The values the drag wrote last (shown while dragging). */
    float draggedValue (int axis) const noexcept { return axes[static_cast<size_t> (axis & 1)].last; }
    bool hasDragged (int axis) const noexcept { return axes[static_cast<size_t> (axis & 1)].touched; }

private:
    struct Axis
    {
        ParamTarget* target = nullptr;
        bool touched = false;
        float last = 0.0f;
        TimedGesture timed;
    };

    void write (int i, float v)
    {
        auto& a = axes[static_cast<size_t> (i)];
        v = snap (v, a.target->numSteps());
        if (! a.touched)
        {
            if (sameBits (v, a.target->value()))
                return; // not touched yet: no gesture for a parameter the drag does not change
            a.target->beginGesture();
            a.touched = true;
        }
        a.last = v;
        a.target->setValue (v);
    }

    std::array<Axis, 2> axes;
    PxToValue xMap;
    float yPxPerRange = 200.0f;
    bool dragging = false, fineNow = false;
    float anchorX = 0.0f, anchorY = 0.0f, anchorHandleX = 0.0f, lastHandleX = 0.0f;
    float anchorYValue = 0.0f, lastY = 0.0f;
};
} // namespace vs::ui::model
