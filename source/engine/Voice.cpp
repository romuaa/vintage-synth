#include "engine/Voice.h"

#include "params/Mappings.h"

namespace vs
{
static_assert (kVcoWaveLabels.size() == dsp::kNumWaves, "VCO wave labels must match dsp::Wave");

namespace
{
constexpr float kLog2A4 = 8.7813597f; // log2(440)
constexpr float kLog2LoFreqBase = 1.0f; // VCO-2 Lo Freq: 2 Hz at 8'

enum ModTarget
{
    kTargetVco1 = 0,
    kTargetBoth = 1,
    kTargetVco2 = 2
};

enum PwmSrc
{
    kPwmLfo = 0,
    kPwmManual = 1,
    kPwmEnv1 = 2
};
} // namespace

void Voice::prepare (float oversampledRate, std::uint32_t seed) noexcept
{
    fs = oversampledRate;
    invFs = 1.0f / fs;
    maxLog2Cutoff = std::log2 (0.42f * fs);
    env1.setSampleRate (fs);
    env2.setSampleRate (fs);
    drift.prepare (fs, seed);
    vco1.setSeed (seed * 2654435761u + 1u);
    vco2.setSeed (seed * 2246822519u + 7u);
    // Free-running oscillators: start each voice at a different phase.
    dsp::Random r (seed ^ 0xA5A5A5A5u);
    vco1.reset (r.unipolar());
    vco2.reset (r.unipolar());
    reset();
}

void Voice::reset() noexcept
{
    env1.reset();
    env2.reset();
    hpf.reset();
    vcf.reset();
}

void Voice::setBlockParams (const PartParams& p, float vintage) noexcept
{
    vco1.setWave (static_cast<dsp::Wave> (p.vco1Wave));
    vco2.setWave (static_cast<dsp::Wave> (p.vco2Wave));

    vcoModTarget = p.vcoModTarget;
    pwmSource = p.pwmSource;
    vco1RangeOct = p.vco1Range - kVco1RangeIndexOf8ft;
    vco2RangeOct = p.vco2Range - kVco2RangeIndexOf8ft;
    vco2LoFreq = p.vco2Mode == 1;
    sync = p.vcoSync == 1;
    slope24 = p.vcfSlope == 1;
    vcfEnvSource = p.vcfEnvSource;
    env1Inverted = p.env1Polarity == 1;
    env1KeyFollow = p.env1KeyFollow == 1;
    env2KeyFollow = p.env2KeyFollow == 1;
    vintageAmount = vintage;

    env1Times[0] = map::envTimeSec (p.env1Attack);
    env1Times[1] = map::envTimeSec (p.env1Decay);
    env1Times[2] = map::envTimeSec (p.env1Release);
    env2Times[0] = map::envTimeSec (p.env2Attack);
    env2Times[1] = map::envTimeSec (p.env2Decay);
    env2Times[2] = map::envTimeSec (p.env2Release);
    applyEnvTimes();
}

void Voice::applyEnvTimes() noexcept
{
    // Key follow: times halve every two octaves above middle C.
    const float keyScale = std::exp2 (-static_cast<float> (currentNote - 60) / 24.0f);
    const float vintageScale = 1.0f + drift.envScale * dsp::Drift::kEnvTimeSpread * vintageAmount;

    const float s1 = (env1KeyFollow ? keyScale : 1.0f) * vintageScale;
    const float s2 = (env2KeyFollow ? keyScale : 1.0f) * vintageScale;
    env1.setTimes (env1Times[0] * s1, env1Times[1] * s1, env1Times[2] * s1);
    env2.setTimes (env2Times[0] * s2, env2Times[1] * s2, env2Times[2] * s2);
}

void Voice::noteOn (int note, float vel) noexcept
{
    currentNote = note;
    velocity = vel;
    applyEnvTimes();
    env1.noteOn();
    env2.noteOn();
}

void Voice::noteOff() noexcept
{
    env1.noteOff();
    env2.noteOff();
}

float Voice::process (const ModFrame& f) noexcept
{
    if (! env2.isActive())
        return 0.0f;

    const auto& p = f.p;

    const float e1 = env1.process (p.env1Sustain);
    const float e2 = env2.process (p.env2Sustain);
    const float e1Mod = env1Inverted ? -e1 : e1;

    // ---- Pitch (log2 Hz domain: one exp2 per oscillator) ----
    const float dynCents = drift.process() * dsp::Drift::kDynamicCents * f.vintage;
    const float pitchModOct = f.lfo * map::vcoLfoModOct (p.vcoModLfo) + e1Mod * map::vcoEnvModOct (p.vcoModEnv);
    const float keySemis = static_cast<float> (currentNote) + f.bendSemis;

    float log2F2 = 0.0f;
    const float vco2Cents = drift.vco2Cents * dsp::Drift::kStaticCents * f.vintage + dynCents;
    if (vco2LoFreq)
        log2F2 = kLog2LoFreqBase + static_cast<float> (vco2RangeOct) + (map::fineTuneSemis (p.vco2FineTune) + vco2Cents * 0.01f) / 12.0f;
    else
        log2F2 = kLog2A4 + (keySemis - 69.0f + 12.0f * static_cast<float> (vco2RangeOct) + map::fineTuneSemis (p.vco2FineTune) + vco2Cents * 0.01f) / 12.0f;
    if (vcoModTarget != kTargetVco1)
        log2F2 += pitchModOct;

    float log2F1 = kLog2A4 + (keySemis - 69.0f + 12.0f * static_cast<float> (vco1RangeOct)
                              + (drift.vco1Cents * dsp::Drift::kStaticCents * f.vintage + dynCents) * 0.01f) / 12.0f;
    if (vcoModTarget != kTargetVco2)
        log2F1 += pitchModOct;

    // ---- Pulse width ----
    float pwmAmount = 1.0f;
    if (pwmSource == kPwmLfo)
        pwmAmount = 0.5f + 0.5f * f.lfo;
    else if (pwmSource == kPwmEnv1)
        pwmAmount = env1Inverted ? 1.0f - e1 : e1;
    const float width = 0.5f - 0.45f * p.pwmDepth * dsp::clamp (pwmAmount, 0.0f, 1.0f);

    // ---- Oscillators: VCO-2 (master) first, then VCO-1 with cross-mod and sync ----
    constexpr float kMinDt = 1.0e-7f, kMaxDt = 0.45f;
    float wrap2 = -1.0f, unused = -1.0f;
    const float dt2 = dsp::clamp (std::exp2 (log2F2) * invFs, kMinDt, kMaxDt);
    const float o2 = vco2.process (dt2, width, -1.0f, wrap2);

    log2F1 += map::crossModOct (p.vco1CrossMod) * o2;
    const float dt1 = dsp::clamp (std::exp2 (log2F1) * invFs, kMinDt, kMaxDt);
    const float o1 = vco1.process (dt1, width, sync ? wrap2 : -1.0f, unused);

    // ---- Mixer -> HPF -> VCF -> VCA ----
    const float mixed = o1 + (o2 - o1) * p.sourceMix;
    const float hp = hpf.process (mixed, f.hpfG);

    const float envForVcf = vcfEnvSource == 0 ? e1Mod : e2;
    const float log2Fc = map::cutoffLog2Hz (p.vcfCutoff)
                         + envForVcf * map::vcfEnvModOct (p.vcfEnvMod)
                         + f.lfo * map::vcfLfoModOct (p.vcfLfoMod)
                         + p.vcfKeyFollow * static_cast<float> (currentNote - 60) / 12.0f
                         + drift.cutoffOct * dsp::Drift::kCutoffOct * f.vintage;
    const float fc = std::exp2 (dsp::clamp (log2Fc, 3.0f, maxLog2Cutoff));
    const float y = vcf.process (hp, dsp::tptG (fc, fs), map::resonanceK (p.vcfResonance), slope24);

    return y * e2 * f.vcaGain;
}
} // namespace vs
