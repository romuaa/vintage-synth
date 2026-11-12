#pragma once

// Stereo chorus in the style of the classic bucket-brigade (BBD) units of 80s polysynths.
//
//   mono = (L + R) / 2 ─► HPF 1-pole 120 Hz ─► LPF 2-pole 9 kHz ─► delay line
//                                                        ├─ tap A: 4 ms + depth * lfo(t)
//                                                        └─ tap B: 4 ms - depth * lfo(t)   (anti-phase)
//   L = dryL + g * tapA
//   R = dryR - g * tapB          g = 0.45 * mix
//
// - The wet path is band-limited like a BBD's anti-alias/reconstruction filters, but clean:
//   no clock noise, no companding, no saturation.
// - The wet signal is subtracted on the right. With anti-phase taps the mono sum becomes
//   L + R = 2 * dry + g * (tapA - tapB): the two taps nearly cancel there, so the mono sum
//   never shows the deep comb notches of a "dry + wet on both sides" design. The worst-case
//   mono dip is 20 log10 (1 - g) = -5.2 dB at mix 1; the side signal (L - R) is all wet.
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
    float mix = 1.0f; ///< 0..1. Wet gain = 0.45 * mix next to the unchanged dry signal; 0 = bypass.
};

class Chorus
{
public:
    static constexpr float kCentreMs = 4.0f;
    static constexpr float kWetGain = 0.45f; ///< wet level at mix 1 (about -7 dB below dry)
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
        cleared = true;
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
            finishClear();
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
        wet.setTarget (kWetGain * mix);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        if (idle)
        {
            if (! cleared) // zero the stale line a slice at a time while idle
            {
                std::size_t budget = kClearBudget;
                cleared = line.clearSome (budget);
            }
            return; // bit-exact bypass: the audio is not touched
        }

        const float centre = kCentreMs * msToSamples;
        const double invFs = 1.0 / fs;

        for (int i = 0; i < numSamples; ++i)
        {
            const float f = fade.next();
            const float g = wet.next() * f;

            rate += (rateTarget - rate) * paramCoef;
            depth += (depthTarget - depth) * paramCoef;

            // Rounded triangle LFO in [-1, 1].
            phase += static_cast<double> (rate) * invFs;
            if (phase >= 1.0)
                phase -= 1.0;
            const float p = static_cast<float> (phase);
            const float tri = p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
            const float lfo = 1.5f * tri - 0.5f * tri * tri * tri;

            const float dl = left[i], dr = right[i];
            const float mono = 0.5f * (dl + dr) * f;
            line.push (lpf.process (hpf.highPass (mono, hpfG)));

            const float dA = centre + depth * lfo;
            const float dB = centre - depth * lfo;
            left[i] = dl + g * line.read (dA);
            right[i] = dr - g * line.read (dB);

            if (f <= 0.0f && fade.isSettled())
            {
                goIdle(); // fully faded out; the rest of the block stays untouched
                break;
            }
        }
    }

    double tailSeconds() const noexcept { return static_cast<double> (kMaxDelayMs) / 1000.0; }

    // ---- test hooks (cheap, read-only) ----
    /** Current tap delays in samples (A = left tap, B = right tap), as used by the next sample. */
    float delaySamplesA() const noexcept { return kCentreMs * msToSamples + depth * currentLfo(); }
    float delaySamplesB() const noexcept { return kCentreMs * msToSamples - depth * currentLfo(); }
    std::size_t lineCapacity() const noexcept { return line.capacity(); }
    bool isIdle() const noexcept { return idle; }

private:
    void goIdle() noexcept
    {
        idle = true;
        cleared = false;
        line.startClear();
        hpf.reset();
        lpf.reset();
    }

    void finishClear() noexcept
    {
        if (! cleared)
        {
            std::size_t budget = line.capacity();
            line.clearSome (budget);
            cleared = true;
        }
    }

    float currentLfo() const noexcept
    {
        const float p = static_cast<float> (phase);
        const float tri = p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
        return 1.5f * tri - 0.5f * tri * tri * tri;
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
    bool idle = true, cleared = true, snapNext = true;
};
} // namespace vs::fx
