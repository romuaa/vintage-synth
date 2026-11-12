#pragma once

#include "common/dsp/DspUtil.h"

#include <cstddef>

namespace vs::dsp
{
/** Order matches kLfoWaveLabels in vintage/params/PartParamList.h. */
enum class LfoWave
{
    Sine,
    Triangle,
    Saw,
    Square,
    Random,
    Noise
};
inline constexpr std::size_t kNumLfoWaves = 6;

/**
    Per-part LFO, bipolar output in [-1, 1].

    - Delay: after trigger(), the output stays at 0 for half the delay time, then
      fades in linearly over the other half.
    - Random: sample & hold, a new value each cycle.
    - Noise: smoothed random at 8x the rate.
    - Hard edges (square, saw, S&H) are softened by a 1 ms smoother to avoid
      clicks in the modulated signal.
*/
class Lfo
{
public:
    void prepare (float sr, std::uint32_t seed) noexcept
    {
        sampleRate = sr;
        rng.setSeed (seed);
        edgeCoef = onePoleCoef (0.001f, sr);
        reset();
    }

    void reset() noexcept
    {
        phase = 0.0;
        noisePhase = 0.0;
        held = 0.0f;
        noiseFrom = noiseTo = 0.0f;
        smoothed = 0.0f;
        delayElapsed = kDelayDone;
    }

    void setWave (LfoWave w) noexcept { wave = w; }
    void setRate (float hz) noexcept { inc = static_cast<double> (hz) / static_cast<double> (sampleRate); }
    void setDelay (float seconds) noexcept { delaySeconds = seconds; }

    /** Restart the delay/fade-in (called on note-on when no other keys are held). */
    void trigger() noexcept { delayElapsed = 0; }

    float process() noexcept
    {
        // Phases in double: at 384 kHz a 0.05 Hz LFO advances 1.3e-7 per sample, about two float
        // ulps of a phase near 1, so a float phase ran at a rate off by tens of percent.
        phase += inc;
        if (phase >= 1.0)
        {
            phase -= 1.0;
            held = rng.bipolar();
        }

        noisePhase += inc * 8.0;
        if (noisePhase >= 1.0)
        {
            noisePhase -= std::floor (noisePhase);
            noiseFrom = noiseTo;
            noiseTo = rng.bipolar();
        }

        smoothed += (raw() - smoothed) * edgeCoef;
        if (delayElapsed < kDelayDone)
            ++delayElapsed; // samples since trigger (a float seconds sum lost up to 10 % at 384 kHz)
        return smoothed * delayAmount();
    }

    /** Fade-in amount 0..1 from the delay setting. */
    float delayAmount() const noexcept
    {
        if (delaySeconds <= 0.0f)
            return 1.0f;
        const double half = 0.5 * static_cast<double> (delaySeconds);
        const double elapsed = static_cast<double> (delayElapsed) / static_cast<double> (sampleRate);
        return static_cast<float> (clamp ((elapsed - half) / half, 0.0, 1.0));
    }

private:
    float raw() const noexcept
    {
        switch (wave)
        {
            case LfoWave::Sine:     return std::sin (2.0f * kPi * static_cast<float> (phase));
            case LfoWave::Triangle: return static_cast<float> (phase < 0.25 ? 4.0 * phase
                                                                  : (phase < 0.75 ? 2.0 - 4.0 * phase : 4.0 * phase - 4.0));
            case LfoWave::Saw:      return static_cast<float> (1.0 - 2.0 * phase); // falling ramp, like classic panels
            case LfoWave::Square:   return phase < 0.5 ? 1.0f : -1.0f;
            case LfoWave::Random:   return held;
            case LfoWave::Noise:    return noiseFrom + (noiseTo - noiseFrom) * static_cast<float> (noisePhase);
        }
        return 0.0f;
    }

    float sampleRate = 48000.0f;
    static constexpr std::int64_t kDelayDone = std::int64_t { 1 } << 40; // "long ago": fully faded in
    double inc = 0.0;
    double phase = 0.0;
    double noisePhase = 0.0;
    float held = 0.0f;
    float noiseFrom = 0.0f, noiseTo = 0.0f;
    float smoothed = 0.0f;
    float edgeCoef = 1.0f;
    float delaySeconds = 0.0f;
    std::int64_t delayElapsed = kDelayDone;
    LfoWave wave = LfoWave::Triangle;
    Random rng;
};
} // namespace vs::dsp
