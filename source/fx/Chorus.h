#pragma once

// Stereo chorus in the style of the classic bucket-brigade (BBD) units of 80s polysynths.
//
//   mono = (L + R) / 2 ─► HPF 1-pole 120 Hz ─► LPF 2-pole 9 kHz ─► delay line
//                                                        ├─ tap A: 4 ms + depth * lfo(t)
//                                                        └─ tap B: 4 ms - depth * lfo(t)   (anti-phase)
//   L = k * (dryL + g * tapA)
//   R = k * (dryR - (gd * tapB + (g - gd) * tapA))
//
//   g  = mix^1.152               wet gain: 0 at mix 0, 0.45 at mix 0.5 (default), 1.0 at mix 1
//   gd = min (g, 0.7)            part of the wet that differs between the sides (anti-null cap)
//   k  = 1 for g <= 0.45, else sqrt ((1 + 0.45^2) / (1 + g^2))   (loudness hold above default)
//
// - The wet path is band-limited like a BBD's anti-alias/reconstruction filters, but clean:
//   no clock noise, no companding, no saturation.
// - The wet signal is subtracted on the right. With anti-phase taps the mono sum becomes
//   L + R = k * (2 * dry + gd * (tapA - tapB)): the two taps nearly cancel there, so the mono
//   sum never shows the deep comb notches of a "dry + wet on both sides" design. |tapA - tapB|
//   can reach 2, so with gd = 1 the mono sum could null completely; capping gd at 0.7 bounds
//   the worst mono dip at 20 log10 (k (1 - 0.7)) = about -12.5 dB at mix 1 (-5.1 dB at the default
//   mix). Above g = 0.7 the extra wet on the right is tapA (in phase with the left wet), so the
//   wet level still reaches the dry level; the side signal (L - R) is all wet.
// - The mix curve g = mix^p (p = ln 0.45 / ln 0.5) puts the pre-2026-11 maximum (0.45) at mix 0.5
//   and wet = dry at mix 1, smoothly and monotonically. Above the default, k keeps the RMS level
//   roughly constant (wet and dry add in power), so a full mix peaks at about +3.8 dB for an
//   in-phase sine instead of +6 dB.
// - The high-pass keeps the bass dry and centred (no low-frequency phase cancellation).
// - LFO: triangle rounded by a cubic (y = 1.5 t - 0.5 t^3): nearly constant pitch shift with
//   smooth turnarounds instead of the triangle's audible "kinks".
// - Mode changes glide (rate and depth are smoothed); Off/On and mix changes ramp over 20 ms.
// - Delay reads use 4-point Lagrange interpolation (artefacts below -100 dB at 1 kHz).

#include "fx/FxCommon.h"

namespace vs::fx
{
enum class ChorusMode
{
    Off,
    I,  ///< slow and deep: 0.5 Hz, +-1.85 ms
    II  ///< faster: 0.83 Hz, +-1.35 ms
};

struct ChorusParams
{
    ChorusMode mode = ChorusMode::Off;
    float mix = 0.5f; ///< 0..1, default 0.5. Wet gain mix^1.152: 0.45 at 0.5, 1.0 (wet = dry) at 1; 0 = bypass.
};

class Chorus
{
public:
    static constexpr float kCentreMs = 4.0f;
    static constexpr float kMixExponent = 1.1520031f; ///< ln 0.45 / ln 0.5: wet gain 0.45 at mix 0.5
    static constexpr float kDefaultWet = 0.45f;        ///< wet gain at the default mix; k = 1 up to here
    static constexpr float kMaxSideDiff = 0.7f;        ///< cap on the L/R-differing wet (mono anti-null)
    static constexpr float kRateHz[3] { 0.0f, 0.5f, 0.83f };
    static constexpr float kDepthMs[3] { 0.0f, 1.85f, 1.35f };
    static constexpr float kMaxDelayMs = 6.0f; ///< >= centre + max depth

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        fs = sampleRate;
        msToSamples = static_cast<float> (sampleRate / 1000.0);
        line.resize (static_cast<std::size_t> (std::ceil (kMaxDelayMs * msToSamples)) + 4);
        hpfG = tptCoef (120.0, sampleRate);
        lpf.setup (9000.0, sampleRate, 0.7071);
        paramCoef = smoothCoef (0.08, sampleRate);
        const int rampLen = static_cast<int> (std::lround (0.02 * sampleRate));
        fade.setLength (rampLen);
        wet.setLength (rampLen);
        reset();
    }

    void reset() noexcept
    {
        line.clear();
        hpf.reset();
        lpf.reset();
        phase = 0.0;
        fade.snap (0.0f);
        wet.snap (0.0f);
        idle = true;
        snapNext = true;
    }

    void setParams (const ChorusParams& p) noexcept
    {
        const int m = p.mode == ChorusMode::I ? 1 : (p.mode == ChorusMode::II ? 2 : 0);
        const float mix = sanitize (p.mix, 0.0f, 1.0f);
        const bool active = m != 0 && mix > 0.0f;

        if (m != 0)
        {
            rateTarget = kRateHz[m];
            depthTarget = kDepthMs[m] * msToSamples;
        }

        if (active && idle)
        {
            // Waking up: start from a clean state at the target settings (no glide from stale values).
            phase = 0.0;
            snapNext = true;
            idle = false;
        }
        if (snapNext)
        {
            rate = rateTarget;
            depth = depthTarget;
            snapNext = false;
        }
        fade.setTarget (active ? 1.0f : 0.0f);
        wet.setTarget (mix > 0.0f ? std::pow (mix, kMixExponent) : 0.0f);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        if (idle)
            return; // bit-exact bypass: the audio is not touched

        const float centre = kCentreMs * msToSamples;

        for (int i = 0; i < numSamples; ++i)
        {
            const float f = fade.next();
            const float g = wet.next() * f;

            const LfoStep st = nextLfo();
            rate = st.rate;
            depth = st.depth;
            phase = st.phase;
            const float lfo = st.value;

            const float dl = safeInput (left[i]), dr = safeInput (right[i]);
            const float mono = 0.5f * (dl + dr) * f;
            line.push (lpf.process (hpf.highPass (mono, hpfG)));

            const float dA = centre + depth * lfo;
            const float dB = centre - depth * lfo;
            const float a = line.read (dA), b = line.read (dB);
            if (g > kDefaultWet)
            {
                const float gd = std::min (g, kMaxSideDiff);
                const float k = std::sqrt ((1.0f + kDefaultWet * kDefaultWet) / (1.0f + g * g));
                left[i] = k * (dl + g * a);
                right[i] = k * (dr - (gd * b + (g - gd) * a));
            }
            else
            {
                left[i] = dl + g * a; // default range: exactly the original structure
                right[i] = dr - g * b;
            }

            if (f <= 0.0f && fade.isSettled())
            {
                goIdle(); // fully faded out; the rest of the block stays untouched
                break;
            }
        }
    }

    double tailSeconds() const noexcept { return static_cast<double> (kMaxDelayMs) / 1000.0; }

    // ---- test hooks (cheap, read-only) ----
    /** Tap delays in samples (A = left tap, B = right tap) that the next processed sample will use
        (same computation as process(), so they match it exactly). */
    float delaySamplesA() const noexcept
    {
        const LfoStep st = nextLfo();
        return kCentreMs * msToSamples + st.depth * st.value;
    }
    float delaySamplesB() const noexcept
    {
        const LfoStep st = nextLfo();
        return kCentreMs * msToSamples - st.depth * st.value;
    }
    std::size_t lineCapacity() const noexcept { return line.capacity(); }
    bool isIdle() const noexcept { return idle; }
    std::uint64_t bufferWrites() const noexcept { return line.writes(); }
    static constexpr int kLinesWritten = 1; ///< buffer writes per processed sample, at most

private:
    void goIdle() noexcept
    {
        idle = true;
        line.clear(); // O(1)
        hpf.reset();
        lpf.reset();
    }


    struct LfoStep
    {
        float rate, depth;
        double phase;
        float value; ///< rounded triangle in [-1, 1]
    };

    /** One sample of parameter smoothing and LFO advance (used by process() and the test hooks). */
    LfoStep nextLfo() const noexcept
    {
        LfoStep st { rate + (rateTarget - rate) * paramCoef, depth + (depthTarget - depth) * paramCoef, phase, 0.0f };
        st.phase += static_cast<double> (st.rate) / fs;
        if (st.phase >= 1.0)
            st.phase -= 1.0;
        const float p = static_cast<float> (st.phase);
        const float tri = p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
        st.value = 1.5f * tri - 0.5f * tri * tri * tri;
        return st;
    }

    double fs = 48000.0;
    float msToSamples = 48.0f;
    DelayLine line;
    OnePole hpf;
    SvfLowPass lpf;
    float hpfG = 0.0f, paramCoef = 1.0f;
    float rate = 0.0f, rateTarget = 0.5f, depth = 0.0f, depthTarget = 0.0f;
    double phase = 0.0;
    LinearRamp fade, wet;
    bool idle = true, snapNext = true;
};
} // namespace vs::fx
