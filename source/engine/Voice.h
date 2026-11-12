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

    /** Starts (or retriggers) a note at full level, centred: start (note, velocity, 1, 0). */
    void noteOn (int note, float velocity) noexcept { start (note, velocity, 1.0f, 0.0f); }

    /**
        Starts or retriggers a note. A sounding voice keeps its envelope levels (no click), and
        its velocity and level move to the new values over the 5 ms parameter smoothing; an idle
        voice takes them at once. Any glide is cancelled (see glideFrom).
        @param levelScale  output scale (unison: 1/sqrt(n))
        @param unisonPos   -0.5 .. 0.5: this voice's share of the unison detune spread
    */
    void start (int note, float velocity, float levelScale, float unisonPos) noexcept;

    /** Portamento: the pitch starts at fromSemis (MIDI note units) and glides to the note. */
    void glideFrom (float fromSemis) noexcept
    {
        glideDev = fromSemis - static_cast<float> (currentNote);
        glideCur = static_cast<double> (glideDev);
        glideAnchored = false; // anchored to the part's glide rate in the next beginBlock()
    }

    /**
        Legato pitch change, no envelope retrigger. With glide, the pitch glides from where it is
        now. Key-follow envelope times follow the new note at once (as the keyboard CV would).
    */
    void setNote (int note, bool glide) noexcept;

    /**
        Unison: the oscillator start phases (cycles, 0..1) used if the next start() begins from
        idle, so the voices of a unison group start in a fixed phase relationship.
    */
    void setRestartPhases (float vco1Phase, float vco2Phase) noexcept
    {
        restartPhase1 = vco1Phase;
        restartPhase2 = vco2Phase;
        restartPhasesSet = true;
    }

    /**
        After a sub-block in which ENV-2 finished: how many samples of it the voice was still
        sounding (the finish position inside the sub-block). The Part reports idle voices in this
        order, so the allocator's idle ages do not depend on how the render is split.
    */
    int getIdleAt() const noexcept { return idleAt; }

    void noteOff() noexcept;

    /**
        Steal fade: the output falls linearly to 0 over the next `samples` samples (the last one
        is 0). The caller must not render past the end of the fade (sub-block boundary), then
        reset() the voice and start the next note.
    */
    void startFade (int samples) noexcept
    {
        fadeLength = samples < 1 ? 1 : samples;
        fadeRemaining = fadeLength;
    }
    int getFadeRemaining() const noexcept { return fadeRemaining; }

    bool isActive() const noexcept { return env2.isActive(); }
    int getNote() const noexcept { return currentNote; }
    /** Current keyboard pitch in MIDI note units, including any glide in progress. */
    float getPitch() const noexcept { return static_cast<float> (currentNote) + glideDev; }

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

    /** Renders one sub-block of any number of voices into out (overwrites m.numSamples samples). */
    static void renderBlock (Voice* const* voices, int numVoices, const ModBlock& m, float* out) noexcept;

    /** Voices interleaved together in renderBlock (more are rendered in further groups). */
    static constexpr int kVoiceGroup = 32;

private:
    void applyEnvTimes() noexcept;
    void restartFromIdle() noexcept;

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

    // A voice that starts from idle draws new oscillator start phases and noise seeds here. Its
    // oscillators stopped at the end of the sub-block in which ENV-2 finished, which depends on
    // the host block size; without a fresh start the next note would too. The first note after
    // prepare() keeps the prepare-time phases.
    dsp::Random restartRng;
    bool freshPhases = true;
    float restartPhase1 = 0.0f, restartPhase2 = 0.0f;
    bool restartPhasesSet = false;
    int idleAt = 0;
    bool driftValid = false; // driftWalk holds the walk at the last control point (Vintage > 0)

    int currentNote = 60;

    // Per-note state. Velocity and level are target + deviation smoothers (5 ms).
    // Portamento: pitch - currentNote (semitones) = glideStart * g^k, k = samples since the anchor.
    // Evaluated from the absolute k (double), not stepped per sub-block, so the glide does not
    // depend on how the render is split: a stepped deviation's rounding differences integrate
    // into a phase difference over a long glide (measured -74 dB between host block sizes).
    float glideDev = 0.0f;       // at the start of the next sub-block (0 = not gliding)
    double glideCur = 0.0;       // the same in double precision (the per-sample base)
    double glideStart = 0.0;     // deviation at the anchor
    double glideLog = 0.0;       // ln g per sample the anchor was made with
    std::int64_t glideK = 0;     // samples since the anchor
    bool glideAnchored = false;
    float velTgt = 1.0f, velDev = 0.0f;
    float scaleTgt = 1.0f, scaleDev = 0.0f;
    float unisonPos = 0.0f;
    int fadeRemaining = 0, fadeLength = 1;

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
