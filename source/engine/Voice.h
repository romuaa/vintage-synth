#pragma once

#include "dsp/Drift.h"
#include "dsp/Envelope.h"
#include "dsp/FastMath.h"
#include "dsp/Filters.h"
#include "dsp/Oscillator.h"
#include "engine/Modulation.h"
#include "params/PartParamList.h"

namespace vs
{
/**
    One synth voice: VCO-1, VCO-2, mixer, HPF, VCF, VCA, ENV-1, ENV-2.

    Routing follows the classic panel:
    - VCO-2 is the master/modulator: Cross Mod is VCO-2 -> VCO-1 exponential FM,
      and Sync resets VCO-1 on every VCO-2 cycle.
    - ENV-2 drives the VCA and also decides how long the voice lives.

    Rendering is split in two phases per sub-block (see renderBlock):
    1. beginBlock(), voice by voice: envelopes, then phase increments, pulse widths and filter
       coefficients for the whole sub-block. These loops have no sample-to-sample dependency,
       so they vectorise.
    2. renderSample(), sample by sample across all voices: VCO-2 -> VCO-1 (cross-mod, sync) ->
       mixer -> HPF -> VCF -> VCA. This is a long serial dependency chain per voice; interleaving
       the voices lets the CPU overlap the chains of different voices.
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

    /** Phase 1 for the sub-block in m. Returns false (and does nothing) if the voice is idle. */
    bool beginBlock (const ModBlock& m) noexcept;

    /**
        Phase 2: output of sample i (< m.numSamples) of the sub-block begun last. The other
        arguments are that sample's shared values from the ModBlock.
    */
    float renderSample (int i, float hpfG, float crossModOct, float sourceMix) noexcept
    {
        const auto k = static_cast<size_t> (i);

        // VCO-2 (master) first, then VCO-1 with audio-rate cross-mod and sync.
        float wrap2 = -1.0f, unused = -1.0f;
        const float o2 = vco2.process (dt2[k], width[k], -1.0f, wrap2);
        // Exponential FM: 2^(log2F1 + depth * o2) = 2^log2F1 * 2^(depth * o2).
        const float dt1 = dsp::clamp (crossModOn ? dt1Raw[k] * dsp::fast::exp2 (crossModOct * o2) : dt1Raw[k], kMinDt, kMaxDt);
        const float o1 = vco1.process (dt1, width[k], sync ? wrap2 : -1.0f, unused);

        const float mixed = o1 + (o2 - o1) * sourceMix;
        const float hp = hpf.processG (mixed, hpfG);
        const float y = vcf.tick (hp, cG[k], c1[k], c2[k], c3[k], c4[k], cK[k], cInv[k], slope24);
        return y * gain[k];
    }

    /** Renders one sub-block of all given voices into out (overwrites m.numSamples samples). */
    static void renderBlock (Voice* const* voices, int numVoices, const ModBlock& m, float* out) noexcept;

    static constexpr int kMaxVoicesPerBlock = 32;

private:
    void applyEnvTimes() noexcept;

    static constexpr float kMinDt = 1.0e-7f, kMaxDt = 0.45f; // phase increment limits (cycles/sample)

    float fs = 96000.0f;
    float invFs = 1.0f / 96000.0f;
    float maxLog2Cutoff = 15.0f;

    dsp::Oscillator vco1, vco2;
    dsp::OnePoleHighPass hpf;
    dsp::OtaLadder vcf;
    dsp::Envelope env1, env2;
    dsp::Drift drift;
    float driftWalk = 0.0f; // dynamic drift at the last control point

    int currentNote = 60;
    float velocity = 1.0f;

    // Block-rate state
    float env1Times[3] {}, env2Times[3] {};
    bool env1KeyFollow = false, env2KeyFollow = false;
    float vintageAmount = 0.0f;
    int vcoModTarget = 1, pwmSource = 0, vco1RangeOct = 0, vco2RangeOct = 0, vcfEnvSource = 0;
    bool vco2LoFreq = false, sync = false, slope24 = true, env1Inverted = false;
    bool crossModOn = false; // this sub-block

    // Per-sample results of beginBlock(), consumed by renderSample().
    static constexpr size_t kN = kMaxSubBlock;
    float dt2[kN] {}, dt1Raw[kN] {}, width[kN] {}; // dt1Raw: before cross-mod and clamping
    float cG[kN] {}, c1[kN] {}, c2[kN] {}, c3[kN] {}, c4[kN] {}, cK[kN] {}, cInv[kN] {};
    float gain[kN] {}; // ENV-2 x VCA x ladder compensation
};
} // namespace vs
