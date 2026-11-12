#pragma once

#include "dsp/BlepTable.h"
#include "dsp/DspUtil.h"

#include <algorithm>
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

    // ==== STUDY (cross-mod aliasing, experimental, not production; branch study/crossmod-aliasing) ====

    /**
        Phase increment inside one sample, s = 0..1 from the previous sample point: a1 + b1 s on
        [0, se), a2 + b2 s on [se, 1]. se > 1: one piece. A modulator edge at se may make the
        increment jump; it is corrected on this oscillator's output (BLAMP + curvature).
    */
    struct FmSeg
    {
        float a1 = 0.0f, b1 = 0.0f, se = 2.0f, a2 = 0.0f, b2 = 0.0f;
    };

    /** The naive waveform value at phase p (no band-limiting); used as the FM modulator. */
    float naiveAt (float p, float width) const noexcept { return naive (p, wave == Wave::Pulse ? width : 0.5f); }

    /**
        Like process(), but the phase increment varies inside the sample (FmSeg) and every crossing
        is placed by solving the quadratic phase exactly. Sync adds the slope (and curvature)
        change for every waveform, not only the triangle's.
    */
    float processFm (const FmSeg& g, float width, float syncD, float& wrapD) noexcept
    {
        const float w = wave == Wave::Pulse ? width : 0.5f;
        wrapD = -1.0f;
        const bool hasEdge = g.se <= 1.0f;
        float syncS = syncD >= 0.0f ? 1.0f - syncD : 2.0f;
        const bool coincident = hasEdge && syncD >= 0.0f && std::abs (syncS - g.se) < 1.0e-4f;
        if (coincident)
            syncS = g.se; // the master's wrap is the modulator's edge: same instant

        float p = phase, cur = 0.0f;
        auto runTo = [&] (float end) {
            if (end > cur)
            {
                const bool second = hasEdge && cur >= g.se;
                p = advanceFm (p, cur, end, second ? g.a2 : g.a1, second ? g.b2 : g.b1, w, wrapD);
                cur = end;
            }
        };
        auto onEdge = [&] {
            const float before = g.a1 + g.b1 * g.se, after = g.a2 + g.b2 * g.se;
            const float d = 1.0f - g.se;
            addKink (dWdp (p) * (after - before), d);
            addCurve (d2Wdp2 (p) * (after * after - before * before) + dWdp (p) * (g.b2 - g.b1), d);
        };
        auto onSync = [&] (float s, bool edgeHere) {
            const bool afterIn2 = hasEdge && s >= g.se;
            const bool beforeIn2 = hasEdge && s > g.se && ! edgeHere;
            const float before = beforeIn2 ? g.a2 + g.b2 * s : g.a1 + g.b1 * s;
            const float cb = beforeIn2 ? g.b2 : g.b1;
            const float after = afterIn2 ? g.a2 + g.b2 * s : g.a1 + g.b1 * s;
            const float ca = afterIn2 ? g.b2 : g.b1;
            const float d = 1.0f - s;
            const float jump = naive (0.0f, w) - naive (p, w);
            if (std::abs (jump) > 1.0e-9f)
                addStep (jump, d);
            addKink (dWdp (0.0f) * after - dWdp (p) * before, d);
            addCurve (d2Wdp2 (0.0f) * after * after + dWdp (0.0f) * ca - d2Wdp2 (p) * before * before - dWdp (p) * cb, d);
            p = 0.0f;
        };

        const bool syncFirst = syncS < g.se;
        if (hasEdge && ! syncFirst)
        {
            runTo (g.se);
            if (coincident)
                onSync (g.se, true);
            else
                onEdge();
        }
        if (syncS <= 1.0f && ! coincident)
        {
            runTo (syncS);
            onSync (syncS, false);
        }
        if (hasEdge && syncFirst)
        {
            runTo (g.se);
            onEdge();
        }
        runTo (1.0f);
        phase = p;

        ring[n & kMask] += wave == Wave::Noise ? noise.bipolar() : naive (phase, w);
        auto& slot = ring[(n - static_cast<unsigned> (kLatency)) & kMask];
        const float out = slot;
        slot = 0.0f;
        ++n;
        return out;
    }

private:
    float dWdp (float p) const noexcept
    {
        switch (wave)
        {
            case Wave::Sine:     return 2.0f * kPi * std::cos (2.0f * kPi * p);
            case Wave::Triangle: return p < 0.5f ? 4.0f : -4.0f;
            case Wave::Saw:      return 2.0f;
            case Wave::Square:
            case Wave::Pulse:
            case Wave::Noise:    return 0.0f;
        }
        return 0.0f;
    }
    float d2Wdp2 (float p) const noexcept { return wave == Wave::Sine ? -4.0f * kPi * kPi * std::sin (2.0f * kPi * p) : 0.0f; }

    void addCurve (float c, float d) noexcept
    {
        if (std::abs (c) < 1.0e-12f)
            return;
        d = clamp (d, 0.0f, 1.0f);
        for (int j = -kLatency; j < kLatency; ++j)
            ring[(n + static_cast<unsigned> (j)) & kMask] += c * table->curve (j, d);
    }

    /** advance() with a linearly varying increment a + b s over [s0, s1]; d is measured from s to the sample point. */
    float advanceFm (float p, float s0, float s1, float a, float b, float w, float& wrapD) noexcept
    {
        const bool pulseLike = wave == Wave::Square || wave == Wave::Pulse;
        float remaining = a * (s1 - s0) + 0.5f * b * (s1 * s1 - s0 * s0);
        float s = s0;
        for (int guard = 0; guard < 4; ++guard)
        {
            float th = 1.0f;
            int kind = 2; // 0 pulse fall, 1 triangle peak, 2 wrap
            if (pulseLike && p < w)
            {
                th = w;
                kind = 0;
            }
            else if (wave == Wave::Triangle && p < 0.5f)
            {
                th = 0.5f;
                kind = 1;
            }
            if (p + remaining < th)
            {
                p += remaining;
                break;
            }
            const float need = th - p;
            // a (x - s) + b/2 (x^2 - s^2) = need, solved stably for x
            const float B = a + b * s;
            const float disc = std::max (0.0f, B * B + 2.0f * b * need);
            const float sc = std::min (s1, s + 2.0f * need / (B + std::sqrt (disc)));
            const float d = 1.0f - sc;
            const float v = a + b * sc;
            if (kind == 0)
                addStep (-2.0f, d);
            else if (kind == 1)
            {
                addKink (-8.0f * v, d);
                addCurve (-8.0f * b, d);
            }
            else
            {
                switch (wave)
                {
                    case Wave::Saw:      addStep (-2.0f, d); break;
                    case Wave::Square:
                    case Wave::Pulse:    addStep (2.0f, d); break;
                    case Wave::Triangle: addKink (8.0f * v, d); addCurve (8.0f * b, d); break;
                    case Wave::Sine:
                    case Wave::Noise:    break;
                }
                if (wrapD < 0.0f)
                    wrapD = d;
            }
            remaining -= need;
            p = kind == 2 ? 0.0f : th;
            s = sc;
        }
        return p;
    }
    // ==== end STUDY ====

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
