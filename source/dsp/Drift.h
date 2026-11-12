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
        const double a = static_cast<double> (onePoleCoef (1.2f, sampleRate));
        for (int n = 0; n <= kMaxAdvance; ++n)
            decayN[static_cast<size_t> (n)] = static_cast<float> (std::pow (1.0 - a, n));
        retargetSamples = std::max (1, static_cast<int> (0.25f * sampleRate)); // >= 1: advance() always progresses
        counter = 0;
        target = dev = 0.0f;
    }

    static constexpr int kMaxAdvance = 64;

    /**
        Advance the slow drift by n (<= kMaxAdvance) samples at once; returns the dynamic drift at
        the end, in units of kDynamicCents at full amount. A retarget is applied at its exact sample
        (the n samples are split there), so the walk is the same however a render is cut into
        blocks: n per-sample steps and one n-sample step differ only by float rounding.
        The walk is kept as target + deviation (only the deviation decays): a value-form one-pole
        with this tiny coefficient would stall up to ~0.3 % of full scale short of its target
        when stepped per sample, but not when stepped per 32 samples.
    */
    float advance (int n) noexcept
    {
        n = clamp (n, 0, kMaxAdvance);
        while (n > 0)
        {
            if (counter <= 0) // a retarget is due at this sample
            {
                counter = retargetSamples;
                const float newTarget = rng.bipolar();
                dev = (target + dev) - newTarget;
                target = newTarget;
            }
            const int k = n < counter ? n : counter;
            dev *= decayN[static_cast<size_t> (k)];
            counter -= k;
            n -= k;
        }
        return current();
    }

    float current() const noexcept { return target + dev; }

    // Full-scale ranges at Vintage = 1.
    static constexpr float kStaticCents = 6.0f;
    static constexpr float kDynamicCents = 3.0f;
    static constexpr float kCutoffOct = 0.12f;
    static constexpr float kEnvTimeSpread = 0.08f;

    float vco1Cents = 0.0f, vco2Cents = 0.0f, cutoffOct = 0.0f, envScale = 0.0f;

private:
    Random rng;
    float target = 0.0f, dev = 0.0f; // walk = target + dev
    int retargetSamples = 1, counter = 0;
    std::array<float, kMaxAdvance + 1> decayN {}; // (1 - a)^n of the walk's one-pole

};
} // namespace vs::dsp
