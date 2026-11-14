#pragma once

// LFO picture maths (docs/architecture.md 9.3.3). JUCE-free. Linear time axis; the window is the
// smallest of 0.5, 1, 2, 5, 10, 20 s that holds the delay plus two cycles (20 s at most).

#include "common/params/Mappings.h"
#include "common/ui/models/ParamTarget.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vs::ui::model
{
inline constexpr std::array<float, 6> kLfoWindowsSec { 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f };
inline constexpr float kLfoMinRateHz = 0.05f, kLfoMaxRateHz = 40.0f, kLfoMaxDelaySec = 5.0f;

/** The window (seconds) that holds the delay and two cycles at the given rate. */
inline float lfoWindowFor (float delaySec, float rateHz) noexcept
{
    const float need = delaySec + 2.0f / std::max (rateHz, 1.0e-3f);
    for (const float w : kLfoWindowsSec)
        if (need <= w)
            return w;
    return kLfoWindowsSec.back();
}

/** The LFO's waveform over the window, one point per 1/kPoints of it (the instrument computes it
    with its own LFO code; the view draws it). Point i is the value at time (i + 1) / kPoints x window. */
struct LfoPreview
{
    static constexpr int kPoints = 1000;
    std::array<float, kPoints> y {};
    float windowSec = 1.0f;
};

/** Time <-> x over the plot: `windowSec` spans `width` px from `x0`. */
struct LfoAxis
{
    float x0 = 0.0f, width = 100.0f, windowSec = 1.0f;
    float xOf (float t) const noexcept { return x0 + t / windowSec * width; }
    float timeAt (float x) const noexcept { return (x - x0) / width * windowSec; }
};

/** Time of the delay handle: the end of the fade-in (the delay setting itself). */
inline float lfoDelayHandleSec (float delayValue) noexcept { return map::lfoDelaySec (clamp01 (delayValue)); }
/** Time of the cycle handle: the right end of the first whole cycle after the delay. */
inline float lfoCycleHandleSec (float delayValue, float rateValue) noexcept
{
    return lfoDelayHandleSec (delayValue) + 1.0f / map::lfoRateHz (clamp01 (rateValue));
}
/** `lfo.delay` for a delay handle dragged to time `t` (0 ... 5 s). */
inline float lfoDelayFromHandle (float t) noexcept
{
    return clamp01 (map::lfoDelayFromSec (std::clamp (t, 0.0f, kLfoMaxDelaySec)));
}
/** `lfo.rate` for a cycle handle dragged to time `t`, with the delay as it is now (0.05 ... 40 Hz). */
inline float lfoRateFromHandle (float t, float delayValue) noexcept
{
    const float cycle = std::max (t - lfoDelayHandleSec (delayValue), 1.0f / kLfoMaxRateHz);
    return clamp01 (map::lfoRateFromHz (std::clamp (1.0f / cycle, kLfoMinRateHz, kLfoMaxRateHz)));
}
} // namespace vs::ui::model
