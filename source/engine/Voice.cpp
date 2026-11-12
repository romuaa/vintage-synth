#include "engine/Voice.h"

#include "dsp/FastMath.h"
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
    driftWalk = drift.current();
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

bool Voice::beginBlock (const ModBlock& m) noexcept
{
    if (! env2.isActive())
        return false;

    const int n = m.numSamples;
    const float invN = 1.0f / static_cast<float> (n);
    const float* lfo = m.lfo;

    // ---- Envelopes (serial, short dependency chain) ----
    float e1[kN], e1Mod[kN], e2[kN];
    {
        const float* sus1 = m.ctl[ModBlock::Env1Sustain];
        const float* sus2 = m.ctl[ModBlock::Env2Sustain];
        const float polarity = env1Inverted ? -1.0f : 1.0f;
        for (int i = 0; i < n; ++i)
        {
            e1[i] = env1.process (sus1[i]);
            e1Mod[i] = polarity * e1[i];
            e2[i] = env2.process (sus2[i]);
        }
    }

    // ---- Pitch and pulse width (log2 Hz domain; vectorisable) ----
    const float vintage = m.vintage;
    const float note = static_cast<float> (currentNote);
    const float static1 = drift.vco1Cents * dsp::Drift::kStaticCents * vintage / 1200.0f;
    const float static2 = drift.vco2Cents * dsp::Drift::kStaticCents * vintage / 1200.0f;
    const float base1 = kLog2A4 + (note - 69.0f + 12.0f * static_cast<float> (vco1RangeOct)) / 12.0f + static1;
    const float base2 = vco2LoFreq ? kLog2LoFreqBase + static_cast<float> (vco2RangeOct) + static2
                                   : kLog2A4 + (note - 69.0f + 12.0f * static_cast<float> (vco2RangeOct)) / 12.0f + static2;
    const float bend2 = vco2LoFreq ? 0.0f : 1.0f; // Lo Freq ignores the keyboard and the bender
    const float mod1 = vcoModTarget != kTargetVco2 ? 1.0f : 0.0f;
    const float mod2 = vcoModTarget != kTargetVco1 ? 1.0f : 0.0f;

    // Dynamic drift (1.2 s time constant) is advanced once per sub-block and ramped linearly.
    const float walk0 = driftWalk;
    driftWalk = drift.advance (n);
    const float dynScale = dsp::Drift::kDynamicCents * vintage / 1200.0f;
    const float dyn0 = walk0 * dynScale;
    const float dynStep = (driftWalk - walk0) * dynScale * invN;

    // PWM amount = a + b * lfo + c * env1, clamped to 0..1.
    float pwA = 1.0f, pwLfo = 0.0f, pwEnv = 0.0f;
    if (pwmSource == kPwmLfo)
    {
        pwA = 0.5f;
        pwLfo = 0.5f;
    }
    else if (pwmSource == kPwmEnv1)
    {
        pwA = env1Inverted ? 1.0f : 0.0f;
        pwEnv = env1Inverted ? -1.0f : 1.0f;
    }

    {
        const float* bendOct = m.ctl[ModBlock::BendOct];
        const float* lfoOct = m.ctl[ModBlock::VcoLfoOct];
        const float* envOct = m.ctl[ModBlock::VcoEnvOct];
        const float* fineOct = m.ctl[ModBlock::Vco2FineOct];
        const float* pwmDepth = m.ctl[ModBlock::PwmDepth];
        for (int i = 0; i < n; ++i)
        {
            const float dyn = dyn0 + dynStep * static_cast<float> (i + 1);
            const float pitchMod = lfo[i] * lfoOct[i] + e1Mod[i] * envOct[i];
            const float log2F2 = base2 + dyn + fineOct[i] + bend2 * bendOct[i] + mod2 * pitchMod;
            const float log2F1 = base1 + dyn + bendOct[i] + mod1 * pitchMod;
            dt2[i] = dsp::clamp (dsp::fast::exp2 (log2F2) * invFs, kMinDt, kMaxDt);
            dt1Raw[i] = dsp::fast::exp2 (log2F1) * invFs; // clamped after cross-mod

            const float amount = dsp::clamp (pwA + pwLfo * lfo[i] + pwEnv * e1[i], 0.0f, 1.0f);
            width[i] = 0.5f - 0.45f * pwmDepth[i] * amount;
        }
    }

    crossModOn = m.crossModActive;

    // ---- VCF coefficients and VCA gain (vectorisable). The oscillators run in renderSample(). ----
    {
        const float* env = vcfEnvSource == 0 ? e1Mod : e2;
        const float* cutoff = m.ctl[ModBlock::CutoffLog2];
        const float* envOct = m.ctl[ModBlock::VcfEnvOct];
        const float* lfoOct = m.ctl[ModBlock::VcfLfoOct];
        const float* keyOct = m.ctl[ModBlock::VcfKeyOct];
        const float* resK = m.ctl[ModBlock::ResonanceK];
        const float keyDist = note - 60.0f;
        const float driftOct = drift.cutoffOct * dsp::Drift::kCutoffOct * vintage;
        const float piOverFs = dsp::kPi * invFs;
        const float maxLog2 = maxLog2Cutoff;
        for (int i = 0; i < n; ++i)
        {
            const float log2Fc = cutoff[i] + env[i] * envOct[i] + lfo[i] * lfoOct[i] + keyOct[i] * keyDist + driftOct;
            const float fc = dsp::fast::exp2 (dsp::clamp (log2Fc, 3.0f, maxLog2));
            const auto c = dsp::OtaLadder::makeCoefs (dsp::fast::tanPrewarp (fc * piOverFs), resK[i]);
            cG[i] = c.G;
            c1[i] = c.c1;
            c2[i] = c.c2;
            c3[i] = c.c3;
            c4[i] = c.c4;
            cK[i] = c.k;
            cInv[i] = c.inv;
            gain[i] = e2[i] * m.vcaGain[i] * c.comp;
        }
    }
    return true;
}

void Voice::renderBlock (Voice* const* voices, int numVoices, const ModBlock& m, float* out) noexcept
{
    Voice* active[kMaxVoicesPerBlock];
    int numActive = 0;
    for (int v = 0; v < numVoices && numActive < kMaxVoicesPerBlock; ++v)
        if (voices[v]->beginBlock (m))
            active[numActive++] = voices[v];

    const int n = m.numSamples;
    if (numActive == 0)
    {
        for (int i = 0; i < n; ++i)
            out[i] = 0.0f;
        return;
    }

    const float* hpfG = m.ctl[ModBlock::HpfG];
    const float* crossOct = m.ctl[ModBlock::CrossModOct];
    const float* mix = m.ctl[ModBlock::SourceMix];
    for (int i = 0; i < n; ++i)
    {
        float sum = 0.0f;
        for (int a = 0; a < numActive; ++a)
            sum += active[a]->renderSample (i, hpfG[i], crossOct[i], mix[i]);
        out[i] = sum;
    }
}
} // namespace vs
