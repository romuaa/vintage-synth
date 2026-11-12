#pragma once

#include "dsp/DspUtil.h"

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
        walkCoef = onePoleCoef (1.2f, sampleRate);
        retargetSamples = static_cast<int> (0.25f * sampleRate);
        counter = 0;
        walk = target = 0.0f;
    }

    /** Advance the slow drift; returns dynamic drift in cents at full amount. */
    float process() noexcept
    {
        if (--counter <= 0)
        {
            counter = retargetSamples;
            target = rng.bipolar();
        }
        walk += (target - walk) * walkCoef;
        return walk;
    }

    // Full-scale ranges at Vintage = 1.
    static constexpr float kStaticCents = 6.0f;
    static constexpr float kDynamicCents = 3.0f;
    static constexpr float kCutoffOct = 0.12f;
    static constexpr float kEnvTimeSpread = 0.08f;

    float vco1Cents = 0.0f, vco2Cents = 0.0f, cutoffOct = 0.0f, envScale = 0.0f;

private:
    Random rng;
    float walk = 0.0f, target = 0.0f, walkCoef = 0.0f;
    int retargetSamples = 1, counter = 0;
};
} // namespace vs::dsp
