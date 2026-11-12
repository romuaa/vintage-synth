#include "vintage/engine/Modulation.h"

#include "common/dsp/FastMath.h"
#include "vintage/dsp/Filters.h"
#include "vintage/params/PartMappings.h"

#include <bit>
#include <cmath>
#include <cstdint>

namespace vs
{
static_assert (kLfoWaveLabels.size() == dsp::kNumLfoWaves, "LFO wave labels must match dsp::LfoWave");

namespace
{
constexpr float kSmoothingSeconds = 0.005f; // parameter smoothing time constant

using Dev = PartModulator::Dev;

inline double devValue (const Dev& d, double logKeep) noexcept
{
    return d.k == 0 ? d.d0 : d.d0 * std::exp (logKeep * static_cast<double> (d.k));
}

/**
    n samples of a one-pole smoother: dst[i] = tgt + dev * decay[i + 1], the value after i + 1
    per-sample updates, with dev taken at the sub-block start from the absolute time since the
    retarget. No sample-to-sample dependency, so it vectorises.
*/
inline void smooth (Dev& d, float tgt, const float* decay, double logKeep, float* dst, int n) noexcept
{
    if (std::fpclassify (d.d0) == FP_ZERO)
    {
        for (int i = 0; i < n; ++i)
            dst[i] = tgt;
        return;
    }
    const float dv = static_cast<float> (devValue (d, logKeep));
    for (int i = 0; i < n; ++i)
        dst[i] = tgt + dv * decay[i + 1];
    d.k += n;
    if (std::abs (dv) < 1.0e-20f)
        d = Dev {}; // settled: exactly on the target
}

/** New target for a target + deviation smoother; the current value is kept. */
inline void retarget (float& tgt, Dev& d, float newTarget, double logKeep) noexcept
{
    if (std::bit_cast<std::uint32_t> (tgt) == std::bit_cast<std::uint32_t> (newTarget))
        return; // unchanged
    d.d0 = (static_cast<double> (tgt) + devValue (d, logKeep)) - static_cast<double> (newTarget);
    if (! std::isfinite (d.d0))
        d.d0 = 0.0; // leaving a non-finite value (a NaN from a host): jump, or it would stay NaN forever
    d.k = 0;
    tgt = newTarget;
}
} // namespace

void PartModulator::prepare (float oversampledRate, std::uint32_t lfoSeed) noexcept
{
    fs = oversampledRate;
    logKeep = -1.0 / (static_cast<double> (kSmoothingSeconds) * static_cast<double> (fs)); // ln (1 - a), exactly
    for (int k = 0; k <= kMaxSubBlock; ++k)
        decay[static_cast<size_t> (k)] = static_cast<float> (std::exp (logKeep * k));
    lfo.prepare (fs, lfoSeed);
    glideSeconds = -1.0f; // rebuild the glide table for the new rate
    reset (target);
}

void PartModulator::reset (const PartParams& p) noexcept
{
    target = p;
    setTarget (p);
    dev = DevSet {};
    bendTarget = 0.0f;
    bendDev = Dev {};
    wheelTarget = 0.0f;
    wheelDev = Dev {};
    lfo.reset();
}

void PartModulator::setBendTarget (float semitones) noexcept
{
    retarget (bendTarget, bendDev, semitones, logKeep);
}

void PartModulator::setModWheel (float wheel) noexcept
{
    retarget (wheelTarget, wheelDev, dsp::clamp (wheel, 0.0f, 1.0f), logKeep);
}

void PartModulator::setControllersNow (float bendSemitones, float wheel) noexcept
{
    bendTarget = bendSemitones;
    bendDev = Dev {};
    wheelTarget = dsp::clamp (wheel, 0.0f, 1.0f);
    wheelDev = Dev {};
}

void PartModulator::updateGlideTable (float seconds) noexcept
{
    if (std::bit_cast<std::uint32_t> (seconds) == std::bit_cast<std::uint32_t> (glideSeconds))
        return;
    glideSeconds = seconds;
    // RC glide: 99 % of the interval in `seconds` -> per-sample factor exp(-ln(100) / (T fs)).
    // Off (T = 0): nothing is left after the first sample.
    const double perSample = seconds > 0.0f
                                 ? std::exp (-4.605170185988091 / (static_cast<double> (seconds) * static_cast<double> (fs)))
                                 : 0.0;
    // Off: a finite stand-in for ln 0, so exp(glideLog * 0) is still 1.
    glideLog = seconds > 0.0f ? std::log (perSample) : -1.0e4;
    double g = 1.0;
    for (size_t k = 0; k < glide.size(); ++k)
    {
        glide[k] = static_cast<float> (g);
        glideD[k] = g;
        g *= perSample;
    }
}

void PartModulator::setTarget (const PartParams& p) noexcept
{
    // Every float parameter gets a target + deviation pair, but only the ones next() lists are
    // smoothed and used here. The part's pan (mix.pan) is smoothed by the PartMixer instead.
#define VS_RETARGET(member, ...) retarget (target.member, dev.member, p.member, logKeep);
    VS_PART_FLOAT_PARAMS (VS_RETARGET)
#undef VS_RETARGET
#define VS_COPY_CHOICE(member, ...) target.member = p.member;
    VS_PART_CHOICE_PARAMS (VS_COPY_CHOICE)
#undef VS_COPY_CHOICE
    lfo.setWave (static_cast<dsp::LfoWave> (p.lfoWave));
    lfo.setRate (map::lfoRateHz (p.lfoRate));
    lfo.setDelay (map::lfoDelaySec (p.lfoDelay));
    tremoloDepth = map::vcaLfoDepth (p.vcaLfoMod);
    updateGlideTable (map::portaTimeSec (p.portaTime));
}

void PartModulator::next (int n, ModBlock& block) noexcept
{
    n = dsp::clamp (n, 1, kMaxSubBlock);
    block.numSamples = n;
    block.vintage = vintage;
    block.smoothDecay = decay.data();
    block.glideDecay = glide.data();
    block.glideDecayD = glideD.data();
    block.glideLog = glideLog;

    const float* d = decay.data();
    auto& s = dev;
    const auto& t = target;
    auto ctl = [&block] (ModBlock::Ctl c) { return block.ctl[c]; };

    // Linear mappings: smooth straight into the block, then scale where needed.
    smooth (bendDev, bendTarget, d, logKeep, ctl (ModBlock::BendOct), n);
    smooth (s.pwmDepth, t.pwmDepth, d, logKeep, ctl (ModBlock::PwmDepth), n);
    smooth (s.vco2FineTune, t.vco2FineTune, d, logKeep, ctl (ModBlock::Vco2FineOct), n);
    smooth (s.sourceMix, t.sourceMix, d, logKeep, ctl (ModBlock::SourceMix), n);
    smooth (s.vcfCutoff, t.vcfCutoff, d, logKeep, ctl (ModBlock::CutoffLog2), n);
    smooth (s.vcfEnvMod, t.vcfEnvMod, d, logKeep, ctl (ModBlock::VcfEnvOct), n);
    smooth (s.vcfKeyFollow, t.vcfKeyFollow, d, logKeep, ctl (ModBlock::VcfKeyOct), n);
    smooth (s.vcfResonance, t.vcfResonance, d, logKeep, ctl (ModBlock::ResonanceK), n);
    smooth (s.env1Sustain, t.env1Sustain, d, logKeep, ctl (ModBlock::Env1Sustain), n);
    smooth (s.env2Sustain, t.env2Sustain, d, logKeep, ctl (ModBlock::Env2Sustain), n);
    // Squared tapers and the HPF.
    smooth (s.vcoModLfo, t.vcoModLfo, d, logKeep, ctl (ModBlock::VcoLfoOct), n);
    smooth (s.vcoModEnv, t.vcoModEnv, d, logKeep, ctl (ModBlock::VcoEnvOct), n);
    smooth (s.vco1CrossMod, t.vco1CrossMod, d, logKeep, ctl (ModBlock::CrossModOct), n);
    smooth (s.vcfLfoMod, t.vcfLfoMod, d, logKeep, ctl (ModBlock::VcfLfoOct), n);
    smooth (s.hpfCutoff, t.hpfCutoff, d, logKeep, ctl (ModBlock::HpfG), n);
    smooth (s.unisonDetune, t.unisonDetune, d, logKeep, ctl (ModBlock::UnisonSpreadOct), n);
    smooth (s.velToVca, t.velToVca, d, logKeep, ctl (ModBlock::VelToVca), n);
    smooth (s.velToVcf, t.velToVcf, d, logKeep, ctl (ModBlock::VelToVcf), n);
    float level[kMaxSubBlock], wheel[kMaxSubBlock], wheelDepth[kMaxSubBlock];
    smooth (s.vcaLevel, t.vcaLevel, d, logKeep, level, n);
    smooth (wheelDev, wheelTarget, d, logKeep, wheel, n);
    smooth (s.modWheelDepth, t.modWheelDepth, d, logKeep, wheelDepth, n);

    {
        float* bendOct = ctl (ModBlock::BendOct);
        float* fineOct = ctl (ModBlock::Vco2FineOct);
        float* cutoff = ctl (ModBlock::CutoffLog2);
        float* envOct = ctl (ModBlock::VcfEnvOct);
        float* keyOct = ctl (ModBlock::VcfKeyOct);
        float* resK = ctl (ModBlock::ResonanceK);
        float* vcoLfo = ctl (ModBlock::VcoLfoOct);
        float* vcoEnv = ctl (ModBlock::VcoEnvOct);
        float* cross = ctl (ModBlock::CrossModOct);
        float* vcfLfo = ctl (ModBlock::VcfLfoOct);
        float* hpf = ctl (ModBlock::HpfG);
        float* spread = ctl (ModBlock::UnisonSpreadOct);
        const float piOverFs = dsp::kPi / fs;
        for (int i = 0; i < n; ++i)
        {
            bendOct[i] *= 1.0f / 12.0f;
            fineOct[i] = map::fineTuneSemis (fineOct[i]) / 12.0f;
            cutoff[i] = map::cutoffLog2Hz (cutoff[i]);
            envOct[i] = map::vcfEnvModOct (envOct[i]);
            keyOct[i] *= 1.0f / 12.0f;
            resK[i] = map::resonanceK (resK[i]);
            vcoLfo[i] = map::vcoLfoModOct (map::vcoLfoModWithWheel (vcoLfo[i], wheel[i], wheelDepth[i]));
            spread[i] = map::unisonDetuneCents (spread[i]) * (1.0f / 1200.0f);
            vcoEnv[i] = map::vcoEnvModOct (vcoEnv[i]);
            cross[i] = map::crossModOct (cross[i]);
            vcfLfo[i] = map::vcfLfoModOct (vcfLfo[i]);
            // map::hpfHz (v) = 5 Hz * 2^(9 v)
            const float hpfHz = 5.0f * dsp::fast::exp2 (9.0f * hpf[i]);
            hpf[i] = dsp::tptOnePoleG (dsp::fast::tanPrewarp (hpfHz * piOverFs));
        }
        // Smoothing is monotonic, so the end points bound the whole sub-block.
        block.crossModActive = cross[0] > 0.0f || cross[n - 1] > 0.0f;
    }

    for (int i = 0; i < n; ++i)
    {
        const float l = lfo.process();
        block.lfo[i] = l;
        block.vcaGain[i] = map::vcaGain (level[i]) * (1.0f - tremoloDepth * 0.5f * (1.0f - l));
    }
}
} // namespace vs
