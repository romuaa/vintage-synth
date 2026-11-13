#pragma once

// Output soft limiter: stereo-linked, zero latency, soft knee, program-dependent release
// (docs/decisions.md, "Output level"). Header-only, JUCE-free, allocation-free.
//
//   level      x = max(|l|, |r|) (stereo-linked). Two detectors:
//              - fast: instant attack, a hold of kHoldSeconds, then a one-pole release
//                (kFastReleaseSeconds) towards the current level; never below the current sample.
//              - slow: follows the fast detector with kSlowAttackSeconds / kSlowReleaseSeconds.
//              The envelope is the larger of the two: a short transient charges the slow detector
//              only a little and is released fast; sustained loud material charges it fully, so the
//              gain stays steady (little gain modulation, i.e. little distortion).
//   gain       below the knee start S = ceiling - kKneeDb the gain is exactly 1 and the samples are
//              not touched at all (bit-identical to no limiter). Above it, in dB:
//                  out(L) = C - K * exp(-(L - S) / K)        (C ceiling, K knee width, L envelope)
//              continuous with slope 1 at S, rising monotonically towards C, so out(L) < C for any
//              L. The envelope is >= the current sample, so every output sample stays below the
//              ceiling (up to float rounding of the gain): no look-ahead, no latency, no overshoot.
//   ceiling    smoothed linearly in dB over kCeilingRampSeconds (a moving ceiling never clicks).
//
// Deterministic and block-size independent: all state advances per sample, and setting the same
// ceiling again does not restart its ramp. Denormal-safe: the detectors flush to 0 below
// kFlushThreshold. Non-finite input must be removed by the caller (the processor's guard does).

#include "common/fx/FxCommon.h"

#include <algorithm>
#include <cmath>

namespace vs::fx
{
class SoftLimiter
{
public:
    static constexpr float kKneeDb = 3.0f;               // knee width below the ceiling
    static constexpr double kHoldSeconds = 0.02;         // >= one period down to 50 Hz
    static constexpr double kFastReleaseSeconds = 0.15;  // one-pole time constant after the hold
    static constexpr double kSlowAttackSeconds = 0.02;
    static constexpr double kSlowReleaseSeconds = 1.0;
    static constexpr double kCeilingRampSeconds = 0.02;  // ceiling changes
    static constexpr float kMinCeilingDb = -12.0f, kMaxCeilingDb = 0.0f;

    /** Not real-time: computes the time constants. Leaves the state reset. */
    void prepare (double sampleRate) noexcept
    {
        holdSamples = std::max (1, static_cast<int> (std::lround (kHoldSeconds * sampleRate)));
        fastRelease = pole (kFastReleaseSeconds, sampleRate);
        slowAttack = pole (kSlowAttackSeconds, sampleRate);
        slowRelease = pole (kSlowReleaseSeconds, sampleRate);
        ceiling.setLength (std::max (1, static_cast<int> (std::lround (kCeilingRampSeconds * sampleRate))));
        reset (ceiling.targetValue());
    }

    /** O(1): forgets the envelope and puts the ceiling at `ceilingDb` at once. NaN-safe. */
    void reset (float ceilingDb) noexcept
    {
        ceiling.snap (clampCeiling (ceilingDb));
        updateKnee();
        fast = slow = 0.0f;
        hold = 0;
    }

    /** Once per block (the same value again does not restart the ramp). NaN-safe. */
    void setCeilingDb (float db) noexcept { ceiling.setTarget (clampCeiling (db)); }

    /** In place, stereo-linked. Real-time safe. */
    void process (float* left, float* right, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            if (! ceiling.isSettled())
            {
                ceiling.next();
                updateKnee();
            }
            const float x = std::max (std::abs (left[i]), std::abs (right[i]));
            if (x >= fast)
            {
                fast = x;
                hold = holdSamples;
            }
            else if (hold > 0)
                --hold;
            else
                fast = flushTiny (x + (fast - x) * fastRelease); // stays >= x
            slow = flushTiny (fast + (slow - fast) * (fast > slow ? slowAttack : slowRelease));

            const float level = std::max (fast, slow);
            if (level > kneeStartGain)
            {
                const float g = gainFor (level);
                left[i] *= g;
                right[i] *= g;
            }
        }
    }

    /** The gain the current envelope gives (1 below the knee). For tests and meters. */
    float currentGain() const noexcept
    {
        const float level = std::max (fast, slow);
        return level > kneeStartGain ? gainFor (level) : 1.0f;
    }
    float currentCeilingDb() const noexcept { return ceiling.value(); }

    /** The static curve: output level in dB for an input (envelope) level in dB. */
    static double curveDb (double inputDb, double ceilingDb) noexcept
    {
        const double k = static_cast<double> (kKneeDb), start = ceilingDb - k;
        return inputDb <= start ? inputDb : ceilingDb - k * std::exp (-(inputDb - start) / k);
    }

private:
    static float clampCeiling (float db) noexcept { return sanitize (db, kMinCeilingDb, kMaxCeilingDb); }
    static float pole (double seconds, double sampleRate) noexcept
    {
        return static_cast<float> (std::exp (-1.0 / (seconds * sampleRate)));
    }

    void updateKnee() noexcept
    {
        ceilingNowDb = static_cast<double> (ceiling.value());
        kneeStartGain = static_cast<float> (std::pow (10.0, (ceilingNowDb - static_cast<double> (kKneeDb)) / 20.0));
    }

    float gainFor (float level) const noexcept
    {
        const double inDb = 20.0 * std::log10 (static_cast<double> (level));
        return static_cast<float> (std::pow (10.0, (curveDb (inDb, ceilingNowDb) - inDb) / 20.0));
    }

    LinearRamp ceiling;
    double ceilingNowDb = -1.0;
    float kneeStartGain = 1.0f;
    float fast = 0.0f, slow = 0.0f;
    float fastRelease = 0.0f, slowAttack = 0.0f, slowRelease = 0.0f;
    int hold = 0, holdSamples = 1;
};
} // namespace vs::fx
