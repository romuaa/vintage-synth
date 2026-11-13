#pragma once

#include "vintage/dsp/Drift.h"
#include "vintage/dsp/Envelope.h"
#include "common/dsp/FastMath.h"
#include "vintage/dsp/Filters.h"
#include "vintage/dsp/Oscillator.h"
#include "vintage/engine/Modulation.h"
#include "vintage/params/PartParamList.h"

#include <array>

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

    /**
        Samples until this voice's drift walk retargets, if that matters for rendering (sounding,
        Vintage on), else a large number. The voice ramps the walk linearly across a sub-block, so
        a retarget (a kink in the walk) inside a sub-block would be approximated differently for
        different splits: the Part ends sub-blocks there.
    */
    int samplesToDriftRetarget() const noexcept
    {
        return env2.isActive() && vintageAmount > 0.0f ? drift.samplesToRetarget() : 1 << 30;
    }
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
        const float y = slopeFading ? vcfSlopeFade (hp, k) : vcf.tick (hp, cG[k], c1[k], c2[k], c3[k], c4[k], cK[k], cInv[k], slope24);
        return y * gain[k];
    }

    /**
        renderSample() for a note on the real-time-rate path (see setRealTimePairRatio). The whole
        voice (VCO pair, mixer, HPF, VCF, VCA) runs on every second sample, on the samples that
        are real-time samples (even absolute index: real-time sample k is sample 2k here), with
        the same code and coefficients the real-time plugin uses; its output is interpolated back
        to this rate (half-band FIR). The result is the real-time voice, delayed by the
        interpolator's 2 kRtInterpTaps - 1 samples.
    */
    float renderSampleRtPair (int i, float hpfG, float crossModOct, float sourceMix) noexcept
    {
        const auto k = static_cast<size_t> (i);
        constexpr auto K = static_cast<size_t> (kRtInterpTaps);
        if (((blockClock + static_cast<unsigned> (i)) & 1u) != 0u)
            return rtHist[K];

        // Increments at the real-time rate are twice those at this rate (exact: invFs halves).
        float wrap2 = -1.0f, unused = -1.0f;
        const float o2 = vco2Rt.process (dsp::clamp (2.0f * dt2[k], kMinDt, kMaxDt), width[k], -1.0f, wrap2);
        const float raw1 = 2.0f * dt1Raw[k];
        const float dt1 = dsp::clamp (crossModOn ? raw1 * dsp::fast::exp2 (crossModOct * o2) : raw1, kMinDt, kMaxDt);
        const float o1 = vco1Rt.process (dt1, width[k], sync ? wrap2 : -1.0f, unused);

        // The part's HPF coefficient is G = g / (1 + g), g = tan (theta) at this rate; the
        // real-time rate needs tan (2 theta) = 2 g / (1 - g^2). (The VCF coefficients of this voice
        // are already computed for the real-time rate in beginBlock.)
        const float g = hpfG / (1.0f - hpfG);
        const float g2 = 2.0f * g / (1.0f - g * g);
        const float mixed = o1 + (o2 - o1) * sourceMix;
        const float hp = hpf.processG (mixed, g2 / (1.0f + g2));
        const float y = (slopeFading ? vcfSlopeFade (hp, k) : vcf.tick (hp, cG[k], c1[k], c2[k], c3[k], c4[k], cK[k], cInv[k], slope24)) * gain[k];

        for (size_t j = 0; j + 1 < rtHist.size(); ++j)
            rtHist[j] = rtHist[j + 1];
        rtHist[rtHist.size() - 1] = y;

        // Half-way point between rtHist[K - 1] and rtHist[K].
        float h = 0.0f;
        for (size_t m = 0; m < K; ++m)
            h += kRtInterp[m] * (rtHist[K - 1 - m] + rtHist[K + m]);
        return h;
    }

    /**
        Offline rendering (docs/architecture.md 4.4) runs the voice path at twice the real-time
        internal rate. Classic Cross Mod is exempt from the alias gate and its aliasing (in the
        oscillators and in the VCF's saturation) depends on the rate, so a note that starts from
        idle while the part's Cross Mod slider is above zero renders its whole voice at the
        real-time rate instead: same code, same rate, same sound as in real time. The choice is
        made at note start and holds for the whole note (no path switch mid-note, no click; a note
        already sounding when Cross Mod is raised from zero stays on the offline-rate path).
        ratio 2: offline at twice the real-time rate; 1: off (real time, or the same factor in
        both). Call after prepare().
    */
    void setRealTimePairRatio (int ratio) noexcept { rtPairRatio = ratio; }
    /** True while the current note renders at the real-time rate. For tests. */
    bool usesRealTimePair() const noexcept { return rtPair; }

    /**
        Renders one sub-block of any number of voices into out (overwrites m.numSamples samples).
        If outRtPath is given, notes on the real-time-rate path are summed there instead (also
        overwritten), so the caller can align the two groups (Part); else they go into out too.
    */
    static void renderBlock (Voice* const* voices, int numVoices, const ModBlock& m, float* out, float* outRtPath = nullptr) noexcept;

    /** Real-time-rate path: interpolator taps on each side of the half-sample point. */
    static constexpr int kRtInterpTaps = 4;
    /**
        The latency a note on the real-time-rate path adds, in samples at this rate: its BLEP
        look-ahead counts at the real-time rate (2 kLatency instead of kLatency) and the
        interpolator adds 2 kRtInterpTaps - 1. Part delays its ordinary notes by this much and
        reports it, so the real-time-rate notes are exactly at the reported latency (they are
        the real-time voice, delayed by the interpolator). Ordinary notes then sound one sample
        (at this rate) later than those, as offline renders always did against real time: an
        oscillator outputs the phase at the end of its sample, one sample at either rate.
    */
    static constexpr int kRtPathExtraDelay = dsp::Oscillator::kLatency + 2 * kRtInterpTaps - 1;

    /**
        A VCF slope change on a sounding voice crossfades between the two filter taps over this
        time (the taps differ in phase and level: switching at once steps the output). An idle
        voice, and a note starting from idle, take the new slope at once.
    */
    static constexpr float kSlopeFadeSeconds = 0.005f;
    /** True while a slope change crossfades. For tests. */
    bool isSlopeFading() const noexcept { return slopeFading; }

    /** Voices interleaved together in renderBlock (more are rendered in further groups). */
    static constexpr int kVoiceGroup = 32;

private:
    /** One VCF sample during a slope crossfade (see kSlopeFadeSeconds); ends the fade at its end. */
    float vcfSlopeFade (float hp, size_t k) noexcept
    {
        slopeMix += slope24 ? slopeFadeStep : -slopeFadeStep;
        if (slopeMix >= 1.0f || slopeMix <= 0.0f)
        {
            slopeMix = slope24 ? 1.0f : 0.0f;
            slopeFading = false;
        }
        return vcf.tickMix (hp, cG[k], c1[k], c2[k], c3[k], c4[k], cK[k], cInv[k], slopeMix);
    }
    /** Ends any slope crossfade: the voice uses the current slope from now on. */
    void snapSlope() noexcept
    {
        slopeMix = slope24 ? 1.0f : 0.0f;
        slopeFading = false;
    }

    void applyEnvTimes() noexcept;
    void restartFromIdle() noexcept;
    void mirrorRealTimePair() noexcept;

    static constexpr float kMinDt = 1.0e-7f, kMaxDt = 0.45f; // phase increment limits (cycles/sample)

    float fs = 96000.0f;
    float invFs = 1.0f / 96000.0f;
    float maxLog2Cutoff = 15.0f;
    float maxLog2CutoffRt = 15.0f; // maxLog2Cutoff at the real-time rate (half this rate)

    dsp::Oscillator vco1, vco2;

    // Offline only (setRealTimePairRatio): a Cross Mod note rendered at the real-time rate. Its
    // VCO pair is seeded and reset together with vco1/vco2, so it starts from exactly the state
    // the real-time pair would have; HPF and VCF are the voice's own, ticked at the real-time
    // rate. The output passes a 2x half-band interpolator: half-sample points from kRtInterpTaps
    // taps on each side (least squares, flat to 0.25 of the real-time rate: within 0.0004
    // (0.003 dB) up to 20 kHz at 88.2 kHz; images below -68 dB before the decimator, which removes
    // them) and delays the note by 2 kRtInterpTaps - 1 samples (see kRtPathExtraDelay).
    static constexpr float kRtInterp[kRtInterpTaps] { 0.6100089598f, -0.1426984697f, 0.0394689421f, -0.0067794322f };
    dsp::Oscillator vco1Rt, vco2Rt;
    std::array<float, 2 * static_cast<size_t> (kRtInterpTaps)> rtHist {}; // the voice's output at the real-time rate, newest last
    int rtPairRatio = 1;
    bool rtPair = false;          // this note renders at the real-time rate (vco1Rt/vco2Rt, renderSampleRtPair)
    bool crossModWanted = false;  // the part's Cross Mod slider is above zero (block parameters)
    unsigned clock = 0;           // samples since prepare(), counted for every sub-block (parity only)
    unsigned blockClock = 0;      // clock at the start of the current sub-block

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
    // VCF slope crossfade: the output is stage 2 + (stage 4 - stage 2) * slopeMix. While not
    // fading, slopeMix is 1 (-24 dB) or 0 (-12 dB) and the plain tap is used (bit-identical).
    float slopeMix = 1.0f, slopeFadeStep = 0.0f;
    bool slopeFading = false;

    // Per-sample results of beginBlock(), consumed by renderSample().
    static constexpr size_t kN = kMaxSubBlock;
    float dt2[kN] {}, dt1Raw[kN] {}, width[kN] {}; // dt1Raw: before cross-mod and clamping
    float cG[kN] {}, c1[kN] {}, c2[kN] {}, c3[kN] {}, c4[kN] {}, cK[kN] {}, cInv[kN] {};
    float gain[kN] {}; // ENV-2 x VCA x ladder compensation
};
} // namespace vs
