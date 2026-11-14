#pragma once

// The parameter a control model writes to, and the shared gesture helpers (docs/architecture.md
// 9.1.5, 9.2.1). JUCE-free: the editor implements ParamTarget with a ParamHandle (host gestures),
// tests with a recorder.

#include <algorithm>
#include <cmath>

namespace vs::ui
{
/** One host parameter as a control model sees it: normalised values 0..1. */
class ParamTarget
{
public:
    virtual ~ParamTarget() = default;
    /** The parameter's current normalised value. */
    virtual float value() const noexcept = 0;
    virtual float defaultValue() const noexcept = 0;
    /** Number of discrete values (2 or more), or 0 for a continuous parameter. */
    virtual int numSteps() const noexcept = 0;
    /** Host gestures: values are written only between a begin and its end. */
    virtual void beginGesture() = 0;
    virtual void setValue (float normalised) = 0;
    virtual void endGesture() = 0;
};

namespace model
{
inline float clamp01 (float v) noexcept { return std::isfinite (v) ? std::clamp (v, 0.0f, 1.0f) : 0.0f; }

/** `v` on the parameter's grid (discrete parameters), else clamped. */
inline float snap (float v, int steps) noexcept
{
    v = clamp01 (v);
    if (steps < 2)
        return v;
    const float last = static_cast<float> (steps - 1);
    return std::round (v * last) / last;
}

/** Index of `v` on a grid of `steps` values. */
inline int stepIndex (float v, int steps) noexcept
{
    if (steps < 2)
        return 0;
    return static_cast<int> (std::lround (clamp01 (v) * static_cast<float> (steps - 1)));
}

inline bool sameBits (float a, float b) noexcept
{
    // Bitwise identity without float ==: equal, or both NaN (never written).
    return ! (a < b) && ! (a > b) && std::isnan (a) == std::isnan (b) && std::signbit (a) == std::signbit (b);
}

/** Wheel and arrow keys have no mouse-up: their gesture begins at the first notch or key and
    ends 300 ms after the last one (docs/architecture.md 9.1.5). */
class TimedGesture
{
public:
    static constexpr double kIdleMs = 300.0;

    void touch (ParamTarget& t, double nowMs)
    {
        if (! open)
        {
            t.beginGesture();
            open = true;
        }
        lastMs = nowMs;
    }
    void idle (ParamTarget& t, double nowMs)
    {
        if (open && nowMs - lastMs >= kIdleMs)
            close (t);
    }
    void close (ParamTarget& t)
    {
        if (open)
        {
            open = false;
            t.endGesture();
        }
    }
    bool isOpen() const noexcept { return open; }

private:
    bool open = false;
    double lastMs = 0.0;
};

/** A one-shot write (double-click reset, typed value, menu): begin, value, end. */
inline void writeOnce (ParamTarget& t, float normalised)
{
    t.beginGesture();
    t.setValue (snap (normalised, t.numSteps()));
    t.endGesture();
}

/** Keyboard and wheel actions shared by every control (9.2.1). */
enum class Key
{
    Increase,
    Decrease,
    PageUp,
    PageDown,
    Home,
    End,
    Reset
};

/** The value after `k` from `v`: arrows +-1 % (fine 0.1 %) or one step, Page +-10 % (discrete:
    a tenth of the steps, at least one), Home/End min/max, Reset the default. */
inline float keyValue (Key k, float v, float def, int steps, bool fine) noexcept
{
    const bool discrete = steps >= 2;
    const float step = discrete ? 1.0f / static_cast<float> (steps - 1) : (fine ? 0.001f : 0.01f);
    const float page = discrete ? step * std::max (1.0f, std::round (0.1f * static_cast<float> (steps - 1))) : 0.1f;
    switch (k)
    {
        case Key::Increase: return snap (v + step, steps);
        case Key::Decrease: return snap (v - step, steps);
        case Key::PageUp: return snap (v + page, steps);
        case Key::PageDown: return snap (v - page, steps);
        case Key::Home: return 0.0f;
        case Key::End: return 1.0f;
        case Key::Reset: return snap (def, steps);
    }
    return v;
}
} // namespace model
} // namespace vs::ui
