#pragma once

#include "dsp/Drift.h"
#include "dsp/Envelope.h"
#include "dsp/Filters.h"
#include "dsp/Oscillator.h"
#include "params/PartParamList.h"

namespace vs
{
/** Per-sample, part-level values shared by all voices of a part (oversampled rate). */
struct ModFrame
{
    const PartParams& p; // smoothed floats + current choices
    float lfo;           // bipolar, delay/fade-in applied
    float bendSemis;
    float hpfG;          // shared HPF coefficient
    float vcaGain;       // level x tremolo
    float vintage;       // 0..1
};

/**
    One synth voice: VCO-1, VCO-2, mixer, HPF, VCF, VCA, ENV-1, ENV-2.

    Routing follows the classic panel:
    - VCO-2 is the master/modulator: Cross Mod is VCO-2 -> VCO-1 exponential FM,
      and Sync resets VCO-1 on every VCO-2 cycle.
    - ENV-2 drives the VCA and also decides how long the voice lives.
*/
class Voice
{
public:
    void prepare (float oversampledRate, std::uint32_t seed) noexcept;
    void reset() noexcept;

    /** Per block: envelope times, waveforms and switches (not smoothed). */
    void setBlockParams (const PartParams& p, float vintage) noexcept;

    void noteOn (int note, float velocity) noexcept;
    /** Legato pitch change: no envelope retrigger. */
    void setNote (int note) noexcept { currentNote = note; }
    void noteOff() noexcept;

    bool isActive() const noexcept { return env2.isActive(); }
    int getNote() const noexcept { return currentNote; }

    float process (const ModFrame& f) noexcept;

private:
    void applyEnvTimes() noexcept;

    float fs = 96000.0f;
    float invFs = 1.0f / 96000.0f;
    float maxLog2Cutoff = 15.0f;

    dsp::Oscillator vco1, vco2;
    dsp::OnePoleHighPass hpf;
    dsp::OtaLadder vcf;
    dsp::Envelope env1, env2;
    dsp::Drift drift;

    int currentNote = 60;
    float velocity = 1.0f;

    // Block-rate state
    float env1Times[3] {}, env2Times[3] {};
    bool env1KeyFollow = false, env2KeyFollow = false;
    float vintageAmount = 0.0f;
    int vcoModTarget = 1, pwmSource = 0, vco1RangeOct = 0, vco2RangeOct = 0, vcfEnvSource = 0;
    bool vco2LoFreq = false, sync = false, slope24 = true, env1Inverted = false;
};
} // namespace vs
