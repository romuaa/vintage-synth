#pragma once

#include "dsp/DspUtil.h"

#include <cstddef>

namespace vs::dsp
{
/** Order matches kLfoWaveLabels in params/PartParamList.h. */
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
        phase = 0.0f;
        noisePhase = 0.0f;
        held = 0.0f;
        noiseFrom = noiseTo = 0.0f;
        smoothed = 0.0f;
        delayElapsed = 1.0e9f;
    }

    void setWave (LfoWave w) noexcept { wave = w; }
    void setRate (float hz) noexcept { inc = hz / sampleRate; }
    void setDelay (float seconds) noexcept { delaySeconds = seconds; }

    /** Restart the delay/fade-in (called on note-on when no other keys are held). */
    void trigger() noexcept { delayElapsed = 0.0f; }

    float process() noexcept
    {
        phase += inc;
        if (phase >= 1.0f)
        {
            phase -= 1.0f;
            held = rng.bipolar();
        }

        noisePhase += inc * 8.0f;
        if (noisePhase >= 1.0f)
        {
            noisePhase -= static_cast<float> (static_cast<int> (noisePhase));
            noiseFrom = noiseTo;
            noiseTo = rng.bipolar();
        }

        smoothed += (raw() - smoothed) * edgeCoef;
        delayElapsed += 1.0f / sampleRate;
        return smoothed * delayAmount();
    }

    /** Fade-in amount 0..1 from the delay setting. */
    float delayAmount() const noexcept
    {
        if (delaySeconds <= 0.0f)
            return 1.0f;
        const float half = 0.5f * delaySeconds;
        return clamp ((delayElapsed - half) / half, 0.0f, 1.0f);
    }

private:
    float raw() const noexcept
    {
        switch (wave)
        {
            case LfoWave::Sine:     return std::sin (2.0f * kPi * phase);
            case LfoWave::Triangle: return phase < 0.25f ? 4.0f * phase
                                         : (phase < 0.75f ? 2.0f - 4.0f * phase : 4.0f * phase - 4.0f);
            case LfoWave::Saw:      return 1.0f - 2.0f * phase; // falling ramp, like classic panels
            case LfoWave::Square:   return phase < 0.5f ? 1.0f : -1.0f;
            case LfoWave::Random:   return held;
            case LfoWave::Noise:    return noiseFrom + (noiseTo - noiseFrom) * noisePhase;
        }
        return 0.0f;
    }

    float sampleRate = 48000.0f;
    float inc = 0.0f;
    float phase = 0.0f;
    float noisePhase = 0.0f;
    float held = 0.0f;
    float noiseFrom = 0.0f, noiseTo = 0.0f;
    float smoothed = 0.0f;
    float edgeCoef = 1.0f;
    float delaySeconds = 0.0f;
    float delayElapsed = 1.0e9f;
    LfoWave wave = LfoWave::Triangle;
    Random rng;
};
} // namespace vs::dsp
