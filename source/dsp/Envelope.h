#pragma once

#include "dsp/DspUtil.h"

namespace vs::dsp
{
/**
    Analog-style ADSR: every segment is an RC (exponential) curve.

    - Attack charges toward 1.3 and ends when it reaches 1.0. That gives the
      fast, slightly convex attack of RC envelopes. The attack time is the time
      to reach the peak.
    - Decay and release times are the time to cover 99% of the distance to their target.
    - A retrigger starts from the current level (no click to zero).
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
        attackCoef = onePoleCoef (attackS / 1.4663371f, sampleRate);
        decayCoef = onePoleCoef (decayS / 4.6051702f, sampleRate);   // ln(100)
        releaseCoef = onePoleCoef (releaseS / 4.6051702f, sampleRate);
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
        level = 0.0f;
    }

    /** @param sustain current (smoothed) sustain level 0..1 */
    float process (float sustain) noexcept
    {
        switch (stage)
        {
            case Stage::Idle:
                return 0.0f;

            case Stage::Attack:
                level += (kAttackTarget - level) * attackCoef;
                if (level >= 1.0f)
                {
                    level = 1.0f;
                    stage = Stage::Decay;
                }
                break;

            case Stage::Decay:
                level += (sustain - level) * decayCoef;
                break;

            case Stage::Release:
                level += (0.0f - level) * releaseCoef;
                if (level < 1.0e-5f)
                {
                    level = 0.0f;
                    stage = Stage::Idle;
                }
                break;
        }
        return level;
    }

    bool isActive() const noexcept { return stage != Stage::Idle; }
    Stage getStage() const noexcept { return stage; }
    float getLevel() const noexcept { return level; }

private:
    float sampleRate = 48000.0f;
    float attackCoef = 1.0f, decayCoef = 1.0f, releaseCoef = 1.0f;
    float level = 0.0f;
    Stage stage = Stage::Idle;
};
} // namespace vs::dsp
