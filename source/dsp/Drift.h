#pragma once

#include "dsp/DspUtil.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace vs::dsp
{
/**
    "Vintage" component tolerances for one voice, deterministic per seed.

    Static offsets model part-to-part tolerances (fixed for the voice).
    The dynamic part is a slow random walk that models thermal pitch drift.
    Every value is scaled by the global Vintage amount (0 = clean, all offsets zero).
*/
class Drift
{
public:
    void prepare (float sampleRate, std::uint32_t seed) noexcept
    {
        rng.setSeed (seed);
        vco1Cents = rng.bipolar();
        vco2Cents = rng.bipolar();
        cutoffOct = rng.bipolar();
        envScale = rng.bipolar();
        // One-pole with tau = 1.2 s: per-sample factor e^(-1 / (tau fs)), so ln of it exactly.
        logDecay = -1.0 / (1.2 * static_cast<double> (sampleRate));
        retargetSamples = std::max (1, static_cast<int> (0.25f * sampleRate)); // >= 1: advance() always progresses
        counter = 0;
        target = 0.0f;
        dev0 = 0.0;
        sinceRetarget = 0;
    }

    static constexpr int kMaxAdvance = 64;

    /**
        Advance the slow drift by n (<= kMaxAdvance) samples at once (cheap: counters, and an exp
        only at a retarget). current() gives the dynamic drift, in units of kDynamicCents at full
        amount; it is computed on demand, so a voice at Vintage 0 never pays for it. A retarget is applied at its exact sample
        (the n samples are split there), so the walk is the same however a render is cut into
        blocks: n per-sample steps and one n-sample step differ only by float rounding.
        The walk is kept as target + deviation (only the deviation decays): a value-form one-pole
        with this tiny coefficient would stall up to ~0.3 % of full scale short of its target
        when stepped per sample, but not when stepped per 32 samples. The deviation is computed
        from the time since the retarget, so it does not depend on how calls are split.
    */
    void advance (int n) noexcept
    {
        n = clamp (n, 0, kMaxAdvance);
        while (n > 0)
        {
            if (counter <= 0) // a retarget is due at this sample
            {
                counter = retargetSamples;
                const float newTarget = rng.bipolar();
                const float dev = deviation(); // at this sample
                dev0 = static_cast<double> ((target + dev) - newTarget);
                target = newTarget;
                sinceRetarget = 0;
            }
            const int k = n < counter ? n : counter;
            sinceRetarget += k;
            counter -= k;
            n -= k;
        }
    }

    /**
        The walk now. From the absolute time since the retarget, not stepped per call: stepping
        rounded differently for different splits, and a pitch difference integrates into a phase
        difference (-102 dB between 37- and 512-sample host blocks with one voice).
    */
    float current() const noexcept { return target + deviation(); }

    /** Samples until the next retarget (>= 1 once a walk has started; 0 = due now). */
    int samplesToRetarget() const noexcept { return counter; }

    // Full-scale ranges at Vintage = 1.
    static constexpr float kStaticCents = 6.0f;
    static constexpr float kDynamicCents = 3.0f;
    static constexpr float kCutoffOct = 0.12f;
    static constexpr float kEnvTimeSpread = 0.08f;

    float vco1Cents = 0.0f, vco2Cents = 0.0f, cutoffOct = 0.0f, envScale = 0.0f;

private:
    Random rng;
    float deviation() const noexcept
    {
        return static_cast<float> (dev0 * std::exp (logDecay * static_cast<double> (sinceRetarget)));
    }

    float target = 0.0f; // walk = target + deviation()
    int retargetSamples = 1, counter = 0;
    double dev0 = 0.0;           // deviation right after the last retarget
    double logDecay = 0.0;       // ln (1 - a) of the walk's one-pole
    int sinceRetarget = 0;       // samples since the last retarget (< retargetSamples)

};
} // namespace vs::dsp
