#pragma once

#include "dsp/BlepTable.h"
#include "dsp/DspUtil.h"

#include <array>
#include <cstddef>

namespace vs::dsp
{
/** Order matches kVcoWaveLabels in params/PartParamList.h. */
enum class Wave
{
    Sine,
    Triangle,
    Saw,
    Square,
    Pulse,
    Noise
};
inline constexpr std::size_t kNumWaves = 6;

/**
    Anti-aliased VCO core: windowed-sinc BLEP for steps, BLAMP for slope corners
    (see BlepTable).

    Discontinuities are corrected at their exact sub-sample position, which keeps
    hard sync and audio-rate FM clean. The correction needs kLatency samples of
    look-back, so the output is delayed by exactly kLatency samples.

    "d" below always means: time from the discontinuity to the current sample
    point, in samples, in (0, 1].
*/
class Oscillator
{
public:
    static constexpr int kLatency = BlepTable::kZeroCrossings;

    Oscillator() noexcept : table (&BlepTable::get()) {}

    void setWave (Wave w) noexcept { wave = w; }
    Wave getWave() const noexcept { return wave; }

    void setSeed (std::uint32_t seed) noexcept { noise.setSeed (seed); }

    void reset (float startPhase = 0.0f) noexcept
    {
        phase = startPhase;
        ring.fill (0.0f);
        n = 0;
    }

    /**
        Advance one sample.
        @param dt       phase increment in cycles/sample, must be in (0, 0.5)
        @param width    pulse width for Wave::Pulse, in (0, 1)
        @param syncD    >= 0: hard-sync reset occurred d samples before this sample point
        @param wrapD    out: this oscillator's own cycle reset position (d), or -1 if none
    */
    float process (float dt, float width, float syncD, float& wrapD) noexcept
    {
        const float w = wave == Wave::Pulse ? width : 0.5f;
        wrapD = -1.0f;

        if (syncD < 0.0f)
        {
            if (phase + dt >= 1.0f)
                wrapD = (phase + dt - 1.0f) / dt;

            phase = advance (phase, dt, 0.0f, dt, w);
        }
        else
        {
            // Run up to the sync instant, jump to phase 0, then run the rest of the sample.
            const float pe = advance (phase, dt * (1.0f - syncD), syncD, dt, w);
            const float jump = naive (0.0f, w) - naive (pe, w);
            if (std::abs (jump) > 1.0e-9f)
                addStep (jump, syncD);

            if (wave == Wave::Triangle)
                addKink (triSlope (0.0f, dt) - triSlope (pe, dt), syncD);

            phase = advance (0.0f, dt * syncD, 0.0f, dt, w);
        }

        ring[n & kMask] += wave == Wave::Noise ? noise.bipolar() : naive (phase, w);
        auto& slot = ring[(n - static_cast<unsigned> (kLatency)) & kMask];
        const float out = slot;
        slot = 0.0f;
        ++n;
        return out;
    }

    float getPhase() const noexcept { return phase; }

private:
    float naive (float p, float w) const noexcept
    {
        switch (wave)
        {
            case Wave::Sine:     return std::sin (2.0f * kPi * p);
            case Wave::Triangle: return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
            case Wave::Saw:      return 2.0f * p - 1.0f;
            case Wave::Square:
            case Wave::Pulse:    return p < w ? 1.0f : -1.0f;
            case Wave::Noise:    return 0.0f;
        }
        return 0.0f;
    }

    static float triSlope (float p, float dt) noexcept { return p < 0.5f ? 4.0f * dt : -4.0f * dt; }

    /** Value step of height h, d samples before the current sample. */
    void addStep (float h, float d) noexcept
    {
        d = clamp (d, 0.0f, 1.0f); // float rounding can land a hair outside (0, 1]
        for (int j = -kLatency; j < kLatency; ++j)
            ring[(n + static_cast<unsigned> (j)) & kMask] += h * table->step (j, d);
    }

    /** Slope change of m (per sample), d samples before the current sample. */
    void addKink (float m, float d) noexcept
    {
        d = clamp (d, 0.0f, 1.0f);
        for (int j = -kLatency; j < kLatency; ++j)
            ring[(n + static_cast<unsigned> (j)) & kMask] += m * table->ramp (j, d);
    }

    /** Move phase from p0 by delta (< 1). The end point is dEnd samples before the sample point. */
    float advance (float p0, float delta, float dEnd, float dt, float w) noexcept
    {
        float p = p0 + delta;
        const bool pulseLike = wave == Wave::Square || wave == Wave::Pulse;

        if (pulseLike && p0 < w && p >= w)
            addStep (-2.0f, dEnd + (p - w) / dt);
        else if (wave == Wave::Triangle && p0 < 0.5f && p >= 0.5f)
            addKink (-8.0f * dt, dEnd + (p - 0.5f) / dt);

        if (p >= 1.0f)
        {
            p -= 1.0f;
            const float d = dEnd + p / dt;
            switch (wave)
            {
                case Wave::Saw:      addStep (-2.0f, d); break;
                case Wave::Square:
                case Wave::Pulse:    addStep (2.0f, d); break;
                case Wave::Triangle: addKink (8.0f * dt, d); break;
                case Wave::Sine:
                case Wave::Noise:    break;
            }

            if (pulseLike && p >= w)
                addStep (-2.0f, dEnd + (p - w) / dt);
        }
        return p;
    }

    static constexpr unsigned kMask = 2 * kLatency - 1;
    static_assert ((2 * kLatency & (2 * kLatency - 1)) == 0, "ring size must be a power of two");

    const BlepTable* table;
    std::array<float, 2 * kLatency> ring {};
    unsigned n = 0;
    Wave wave = Wave::Saw;
    float phase = 0.0f;
    Random noise;
};
} // namespace vs::dsp
