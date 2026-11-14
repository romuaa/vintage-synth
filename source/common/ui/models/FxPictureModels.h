#pragma once

// The effect pictures of the Effects page (docs/architecture.md 9.3.6, 9.4). JUCE-free: the data
// behind each picture and the axes its handles use, computed from the parameters' own mappings
// (common/params/Mappings.h) and the effect blocks' own constants (common/fx/). Tests compare them
// with the real effect blocks.

#include "common/fx/Chorus.h"
#include "common/params/Mappings.h"
#include "common/ui/models/GraphModels.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vs::ui::model
{
// ---- Delay ----

/** The echoes the delay makes of a single impulse: echo n (1-based) arrives at n x time with the
    gain feedback^(n-1) of the first one (the engine writes input + feedback x output into the line
    and reads it `time` later). Ping-pong alternates left (odd n) and right (even n); otherwise
    both channels carry every echo. */
struct DelayEcho
{
    double timeSec = 0.0;
    float height = 0.0f; // 0..1, relative to the first echo
    int lane = 2;        // 0 left, 1 right, 2 both
};

struct DelayPicture
{
    static constexpr int kMaxEchoes = 12;
    int count = 0;
    std::array<DelayEcho, kMaxEchoes> echoes {};
};

/** The echoes up to `endSec` (at most kMaxEchoes). `feedbackGain` is map::delayFeedback of the
    parameter (0 .. 0.95): echoes below 1/256 of the first one are left out. */
inline DelayPicture delayPicture (double timeSec, float feedbackGain, bool pingPong, double endSec)
{
    DelayPicture p;
    float h = 1.0f;
    for (int n = 1; n <= DelayPicture::kMaxEchoes; ++n)
    {
        const double t = static_cast<double> (n) * timeSec;
        if (t > endSec || h < 1.0f / 256.0f)
            break;
        p.echoes[static_cast<size_t> (p.count++)] = { t, h, pingPong ? ((n - 1) % 2) : 2 };
        h *= feedbackGain;
    }
    return p;
}

/** The delay picture's time axis: the square root of the time, 0 .. 6 s. Linear time would crowd
    the echoes of a short delay into a few pixels, logarithmic time would crowd the echoes of a long
    one; the square root keeps both readable and is still monotonic for the handle. */
struct SqrtAxis
{
    float start = 0.0f, length = 100.0f;
    double hi = 6.0; // seconds at `start + length`

    float positionOf (double sec) const noexcept
    {
        const double t = sec <= 0.0 ? 0.0 : std::sqrt (sec / hi);
        return start + static_cast<float> (t) * length;
    }
    double valueAt (float px) const noexcept
    {
        const double t = static_cast<double> ((px - start) / length);
        return t <= 0.0 ? 0.0 : hi * t * t;
    }
    float clampPx (float px) const noexcept { return std::clamp (px, start, start + length); }
};

inline SqrtAxis delayTimeAxis (float startPx, float lengthPx) noexcept
{
    return { startPx, lengthPx, 6.0 };
}

// ---- Reverb ----

/** Reverb time axis: logarithmic in (t + 10 ms), so 0 and the short pre-delays have room and the
    long tails fit: 0 .. 30 s. */
struct ReverbTimeAxis
{
    static constexpr double kShiftSec = 0.01, kEndSec = 30.0;
    LogAxis axis;
    ReverbTimeAxis() = default;
    ReverbTimeAxis (float startPx, float lengthPx) : axis { startPx, lengthPx, kShiftSec, kEndSec + kShiftSec } {}
    float positionOf (double sec) const noexcept { return axis.positionOf (sec + kShiftSec); }
    double timeAt (float px) const noexcept { return axis.valueAt (px) - kShiftSec; }
};

/** The tail: silent for the pre-delay, then a straight line in dB reaching -60 dB at
    pre-delay + decay time (the RT60 point, the engine's decaySeconds). */
struct ReverbPicture
{
    static constexpr float kFloorDb = -60.0f;
    double preDelaySec = 0.0, endSec = 0.0;
};

inline ReverbPicture reverbPicture (float preDelaySec, float decaySec) noexcept
{
    return { static_cast<double> (preDelaySec), static_cast<double> (preDelaySec) + static_cast<double> (decaySec) };
}

// ---- Chorus ----

/** The two taps' delay (ms) over time: centre +- depth x the effect block's own LFO shape (a
    rounded triangle) at the mode's rate. Mode 0 (Off) has no modulation. */
struct ChorusPicture
{
    float rateHz = 0.0f, depthMs = 0.0f, centreMs = fx::Chorus::kCentreMs;
    bool active() const noexcept { return rateHz > 0.0f; }
};

inline ChorusPicture chorusPicture (int mode) noexcept
{
    const int m = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
    return { fx::Chorus::kRateHz[m], fx::Chorus::kDepthMs[m], fx::Chorus::kCentreMs };
}

/** The LFO value in [-1, 1] at a phase in [0, 1) (Chorus::nextLfo). */
inline float chorusLfo (double phase) noexcept
{
    const float p = static_cast<float> (phase);
    const float tri = p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
    return 1.5f * tri - 0.5f * tri * tri * tri;
}

/** Tap A (anti = false) or B (anti = true) delay in ms at `sec` seconds after the start. */
inline float chorusTapMs (const ChorusPicture& c, double sec, bool anti) noexcept
{
    const double phase = std::fmod (sec * static_cast<double> (c.rateHz), 1.0);
    const float v = chorusLfo (phase < 0.0 ? phase + 1.0 : phase);
    return c.centreMs + (anti ? -1.0f : 1.0f) * c.depthMs * v;
}

// ---- Power switch ----

/** The delay/reverb power switch (U5, no new parameters): lit while the level is above 0. Off stores
    the level (if it was above 0) and writes 0; On writes the stored level, or the default if the
    stored one is 0. */
struct PowerSwitchModel
{
    static constexpr float kDefaultLevel = 0.35f;
    static bool isOn (float level) noexcept { return level > 0.0f; }
    /** The level to write when the switch is pressed at `level`; `stored` is updated when going off. */
    static float press (float level, float& stored) noexcept
    {
        if (isOn (level))
        {
            stored = level;
            return 0.0f;
        }
        return stored > 0.0f ? stored : kDefaultLevel;
    }
};
} // namespace vs::ui::model
