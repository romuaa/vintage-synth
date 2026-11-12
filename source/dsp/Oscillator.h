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
            // Saw, square and pulse change only their value at the reset (their slope is constant
            // or zero): one step. The triangle also changes its slope: step + ramp. A sine changes
            // every derivative, so it gets the exact band-limited correction (addSineSync).
            const float pe = advance (phase, dt * (1.0f - syncD), syncD, dt, w);
            if (wave == Wave::Sine)
            {
                addSineSync (pe, dt, syncD);
            }
            else
            {
                const float jump = naive (0.0f, w) - naive (pe, w);
                if (std::abs (jump) > 1.0e-9f)
                    addStep (jump, syncD);

                if (wave == Wave::Triangle)
                    addKink (triSlope (0.0f, dt) - triSlope (pe, dt), syncD);
            }

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

    /**
        Hard sync of a sine from phase pe to 0, d samples before the sample point.

        After the reset the output differs from the free-running sine by a gated sinusoid:
        e(t) = Im {C e^(i w t)} u(t), C = 1 - e^(i x), w = 2 pi dt, x = 2 pi pe, t in samples after
        the reset. A step + ramp + ... series of polynomial residuals converges too slowly for a
        high VCO-1 (w near 1) over the 16-sample kernel (and its truncated tails do not cancel), so
        the gated sinusoid is band-limited directly with the same kernel h as the step table:
            BL[e](t) = Im {C e^(i w t) G(t)},  G(t) = integral from -Z to t of h(s) e^(-i w s) ds,
        and the residual Im {C e^(i w t_j) (G(t_j) - u(t_j))} is added at the taps t_j = j + d.
        G is accumulated tap by tap with 6-point Gauss-Legendre quadrature per sample interval
        (the integrand has at most 0.9 cycles per interval, which 6 points integrate far below the
        float floor). It includes the value step (Im C = sin 0 - sin x). Cost: about 100 kernel
        look-ups and 15 sin/cos per reset; only a synced sine pays it. Measured: -109 to -119 dB
        aliasing at a 5 kHz master (VCO-1 up to 13.5 kHz), where a step + ramp + curvature
        correction left -71 to -100 dB and the step alone -43 to -60 dB.
    */
    void addSineSync (float pe, float dt, float d) noexcept
    {
        // Gauss-Legendre, 6 points on [0, 1]
        static constexpr float xq[6] { 0.0337652429f, 0.1693953068f, 0.3806904070f, 0.6193095930f, 0.8306046932f, 0.9662347571f };
        static constexpr float wq[6] { 0.0856622462f, 0.1803807865f, 0.2339569673f, 0.2339569673f, 0.1803807865f, 0.0856622462f };
        constexpr float Z = static_cast<float> (kLatency);

        d = clamp (d, 0.0f, 1.0f);
        const float w = 2.0f * kPi * dt;
        const float x = 2.0f * kPi * pe;
        const float cRe = 1.0f - std::cos (x), cIm = -std::sin (x); // C = 1 - e^(i x)

        // Integral of h(s) e^(-i w s) over [a, a + len], added to (gRe, gIm).
        float gRe = 0.0f, gIm = 0.0f;
        auto integrate = [&] (float a, float len, const float* pRe, const float* pIm) {
            for (int q = 0; q < 6; ++q)
            {
                const float hw = len * wq[q] * table->kernelAt (a + len * xq[q]);
                gRe += hw * pRe[q];
                gIm += hw * pIm[q];
            }
        };

        // First (partial) interval [-Z, -Z + d]: node phasors e^(-i w s).
        float pRe[6], pIm[6];
        for (int q = 0; q < 6; ++q)
        {
            const float s = -Z + d * xq[q];
            pRe[q] = std::cos (w * s);
            pIm[q] = -std::sin (w * s);
        }
        integrate (-Z, d, pRe, pIm);

        // Unit intervals [t_(j-1), t_j]: nodes t_(j-1) + xq, rotated by e^(-i w) per interval.
        for (int q = 0; q < 6; ++q)
        {
            const float s = -Z + d + xq[q];
            pRe[q] = std::cos (w * s);
            pIm[q] = -std::sin (w * s);
        }
        const float rRe = std::cos (w), rIm = -std::sin (w);

        // e^(i w t_j), from t_(-Z) = -Z + d, rotated by e^(i w) per tap.
        float eRe = std::cos (w * (d - Z)), eIm = std::sin (w * (d - Z));
        for (int j = -kLatency; j < kLatency; ++j)
        {
            if (j > -kLatency)
            {
                integrate (static_cast<float> (j - 1) + d, 1.0f, pRe, pIm);
                for (int q = 0; q < 6; ++q)
                {
                    const float re = pRe[q] * rRe - pIm[q] * rIm;
                    pIm[q] = pRe[q] * rIm + pIm[q] * rRe;
                    pRe[q] = re;
                }
            }
            const float vRe = gRe - (j >= 0 ? 1.0f : 0.0f), vIm = gIm; // G (t_j) - u (t_j)
            // Im {C e v}
            const float ceRe = cRe * eRe - cIm * eIm, ceIm = cRe * eIm + cIm * eRe;
            ring[(n + static_cast<unsigned> (j)) & kMask] += ceRe * vIm + ceIm * vRe;

            const float re = eRe * rRe + eIm * rIm; // e^(i w) = conj (r)
            eIm = eIm * rRe - eRe * rIm;
            eRe = re;
        }
    }

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
