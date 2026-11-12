#pragma once

#include "dsp/Lfo.h"
#include "params/PartParamList.h"

#include <array>
#include <cstdint>

namespace vs
{
/** Longest sub-block (oversampled samples) that voices render in one go. */
inline constexpr int kMaxSubBlock = 32;

/**
    Part-level modulation for one sub-block of up to kMaxSubBlock oversampled samples, computed
    once and shared by every voice of the part.

    Every value is per sample. Continuous parameters are smoothed exactly as a per-sample
    one-pole would do it (closed form, so the loop vectorises) and arrive mapped to their
    physical unit.
*/
struct ModBlock
{
    enum Ctl
    {
        BendOct,      // pitch bend, octaves
        VcoLfoOct,    // VCO LFO-mod depth incl. the mod wheel's share, octaves at full LFO swing
        VcoEnvOct,    // VCO ENV-mod depth, octaves at full ENV-1
        PwmDepth,     // 0..1
        CrossModOct,  // VCO-2 -> VCO-1 FM depth, octaves at full VCO-2 swing
        Vco2FineOct,  // VCO-2 fine tune, octaves
        SourceMix,    // 0 = VCO-1, 1 = VCO-2
        CutoffLog2,   // VCF cutoff slider, log2 Hz
        VcfEnvOct,    // VCF ENV-mod depth, octaves
        VcfLfoOct,    // VCF LFO-mod depth, octaves
        VcfKeyOct,    // VCF key follow, octaves per semitone from middle C
        ResonanceK,   // ladder feedback gain
        Env1Sustain,
        Env2Sustain,
        HpfG,         // one-pole TPT gain of the HPF (shared by all voices)
        UnisonSpreadOct, // total unison detune spread, octaves (a voice adds position x spread)
        VelToVca,     // velocity -> VCA amount 0..1
        VelToVcf,     // velocity -> VCF ENV depth amount 0..1
        NumCtl
    };

    int numSamples = 0;
    float vintage = 0.0f;
    bool crossModActive = false; // false: CrossModOct is 0 over the whole sub-block

    float ctl[NumCtl][kMaxSubBlock] {};
    float lfo[kMaxSubBlock] {};     // bipolar, delay/fade-in applied
    float vcaGain[kMaxSubBlock] {}; // level x tremolo

    // Closed-form one-pole tables, index k = what is left of a deviation after k samples
    // (k = 0 .. kMaxSubBlock). Owned by the PartModulator; valid while it lives.
    const float* smoothDecay = nullptr; // parameter smoothing (5 ms): per-voice velocity and level
    const float* glideDecay = nullptr;  // portamento (RC, 99 % in the portamento time)
    const double* glideDecayD = nullptr; // the same in double precision
    double glideLog = 0.0;              // ln of the portamento's per-sample factor (glideDecay[1])
};

/**
    Owns everything a part shares between its voices: target and smoothed parameters, the LFO
    and pitch bend. JUCE-free; no allocation.
*/
class PartModulator
{
public:
    void prepare (float oversampledRate, std::uint32_t lfoSeed) noexcept;

    /** Jump to the target values (no smoothing) and restart the LFO. */
    void reset (const PartParams& target) noexcept;

    /** Per block: new targets, plus the block-rate LFO settings and tremolo depth. */
    void setTarget (const PartParams& p) noexcept;
    void setBendTarget (float semitones) noexcept;
    /** Mod wheel 0..1 (smoothed; its depth is the "Mod Wheel Vibrato" parameter). */
    void setModWheel (float wheel) noexcept;
    void setVintage (float v) noexcept { vintage = v; }
    void triggerLfo() noexcept { lfo.trigger(); }

    /** Advances n samples (1..kMaxSubBlock) and fills block. */
    void next (int n, ModBlock& block) noexcept;

private:
    float fs = 96000.0f;

    // Each smoother is target + deviation, and only the deviation decays. Unlike a value-form
    // one-pole (s += (t - s) * a), it cannot stall a few hundred ulps short of the target when
    // (t - s) * a drops below half an ulp of s; it converges exactly.
    PartParams target, dev;
    float bendTarget = 0.0f, bendDev = 0.0f;
    float wheelTarget = 0.0f, wheelDev = 0.0f;
    float glideSeconds = -1.0f; // portamento time the glide table was built for
    double glideLog = 0.0;
    float vintage = 0.0f;
    float tremoloDepth = 0.0f;

    // decay[k] = (1 - a)^k: what is left of a one-pole smoother's distance to target after k samples.
    std::array<float, kMaxSubBlock + 1> decay {};
    std::array<float, kMaxSubBlock + 1> glide {};
    std::array<double, kMaxSubBlock + 1> glideD {};

    void updateGlideTable (float seconds) noexcept;

    dsp::Lfo lfo;
};
} // namespace vs
