#pragma once

#include "common/dsp/DspUtil.h"

namespace vs::dsp
{
/**
    Analog-style ADSR: every segment is an RC (exponential) curve.

    - Attack charges toward 1.3 and ends when it reaches 1.0. That gives the
      fast, slightly convex attack of RC envelopes. The attack time is the time
      to reach the peak.
    - Decay and release times are the time to cover 99% of the distance to their target.
    - A retrigger starts from the current level (no click to zero).

    The level and coefficients are double: at internal rates up to 384 kHz a long segment moves
    less than half a float ulp per sample near its target, so a float level stalled (a 10 s decay
    stopped 0.42 dB above the sustain level at 384 kHz) and rounded each step (attack +7.7 %).
*/
class Envelope
{
public:
    enum class Stage
    {
        Idle,
        Attack,
        Decay,
        Release
    };

    static constexpr float kAttackTarget = 1.3f;

    void setSampleRate (float sr) noexcept { sampleRate = sr; }

    /** Times in seconds, already scaled (key follow, vintage). Call per block. */
    void setTimes (float attackS, float decayS, float releaseS) noexcept
    {
        // ln(1.3 / 0.3): from 0, reach 1.0 when charging toward 1.3
        attackCoef = onePoleCoefD (static_cast<double> (attackS) / 1.4663370687934272, sr());
        decayCoef = onePoleCoefD (static_cast<double> (decayS) / 4.605170185988091, sr());   // ln(100)
        releaseCoef = onePoleCoefD (static_cast<double> (releaseS) / 4.605170185988091, sr());
    }

    void noteOn() noexcept { stage = Stage::Attack; }

    void noteOff() noexcept
    {
        if (stage != Stage::Idle)
            stage = Stage::Release;
    }

    void reset() noexcept
    {
        stage = Stage::Idle;
        level = 0.0;
    }

    /** @param sustain current (smoothed) sustain level 0..1 */
    float process (float sustain) noexcept
    {
        switch (stage)
        {
            case Stage::Idle:
                return 0.0f;

            case Stage::Attack:
                level += (static_cast<double> (kAttackTarget) - level) * attackCoef;
                if (level >= 1.0)
                {
                    level = 1.0;
                    stage = Stage::Decay;
                }
                break;

            case Stage::Decay:
                level += (static_cast<double> (sustain) - level) * decayCoef;
                break;

            case Stage::Release:
                level -= level * releaseCoef;
                if (level < 1.0e-5)
                {
                    level = 0.0;
                    stage = Stage::Idle;
                }
                break;
        }
        return static_cast<float> (level);
    }

    bool isActive() const noexcept { return stage != Stage::Idle; }
    Stage getStage() const noexcept { return stage; }
    float getLevel() const noexcept { return static_cast<float> (level); }

private:
    double sr() const noexcept { return static_cast<double> (sampleRate); }

    float sampleRate = 48000.0f;
    double attackCoef = 1.0, decayCoef = 1.0, releaseCoef = 1.0;
    double level = 0.0;
    Stage stage = Stage::Idle;
};
} // namespace vs::dsp
