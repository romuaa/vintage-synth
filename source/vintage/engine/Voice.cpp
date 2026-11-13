#include "vintage/engine/Voice.h"

#include "common/dsp/FastMath.h"
#include "vintage/params/PartMappings.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace vs
{
static_assert (kVcoWaveLabels.size() == dsp::kNumWaves, "VCO wave labels must match dsp::Wave");
static_assert (kMaxSubBlock <= dsp::Drift::kMaxAdvance, "a sub-block must fit in one Drift::advance()");

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
    maxLog2CutoffRt = std::log2 (0.42f * (0.5f * fs));
    env1.setSampleRate (fs);
    env2.setSampleRate (fs);
    drift.prepare (fs, seed);
    driftWalk = drift.current();
    driftValid = false;
    vco1.setSeed (seed * 2654435761u + 1u);
    vco2.setSeed (seed * 2246822519u + 7u);
    // Free-running oscillators: start each voice at a different phase.
    dsp::Random r (seed ^ 0xA5A5A5A5u);
    vco1.reset (r.unipolar());
    vco2.reset (r.unipolar());
    mirrorRealTimePair();
    rtPairRatio = 1;
    rtPair = false;
    clock = blockClock = 0;
    restartRng.setSeed (seed * 747796405u + 2891336453u);
    freshPhases = true;
    reset();
}

void Voice::reset() noexcept
{
    env1.reset();
    env2.reset();
    hpf.reset();
    vcf.reset();
    snapSlope();
    glideDev = 0.0f;
    glideCur = 0.0;
    glideAnchored = false;
    velDev = scaleDev = 0.0f;
    fadeRemaining = 0;
    restartPhasesSet = false;
}

void Voice::setBlockParams (const PartParams& p, float vintage) noexcept
{
    vco1.setWave (static_cast<dsp::Wave> (p.vco1Wave));
    vco2.setWave (static_cast<dsp::Wave> (p.vco2Wave));
    vco1Rt.setWave (static_cast<dsp::Wave> (p.vco1Wave));
    vco2Rt.setWave (static_cast<dsp::Wave> (p.vco2Wave));

    vcoModTarget = p.vcoModTarget;
    pwmSource = p.pwmSource;
    vco1RangeOct = p.vco1Range - kVco1RangeIndexOf8ft;
    vco2RangeOct = p.vco2Range - kVco2RangeIndexOf8ft;
    vco2LoFreq = p.vco2Mode == 1;
    sync = p.vcoSync == 1;
    const bool newSlope24 = p.vcfSlope == 1;
    if (newSlope24 != slope24)
    {
        slope24 = newSlope24;
        if (env2.isActive())
        {
            // Sounding: crossfade to the new tap (from wherever a running fade is). The VCF ticks
            // at half this rate on the real-time-rate path.
            slopeFading = true;
            slopeFadeStep = 1.0f / (kSlopeFadeSeconds * (rtPair ? 0.5f * fs : fs));
        }
        else
            snapSlope();
    }
    vcfEnvSource = p.vcfEnvSource;
    env1Inverted = p.env1Polarity == 1;
    env1KeyFollow = p.env1KeyFollow == 1;
    env2KeyFollow = p.env2KeyFollow == 1;
    vintageAmount = vintage;
    crossModWanted = p.vco1CrossMod > 0.0f;

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

void Voice::setNote (int note, bool glide) noexcept
{
    const float from = getPitch();
    currentNote = note;
    if (glide)
        glideFrom (from);
    else
    {
        glideDev = 0.0f;
        glideCur = 0.0;
    }
    // Key follow tracks the keyboard at the event, not at the next host block (block-size
    // independence; a legato line upwards gets the shorter times of the higher note at once).
    applyEnvTimes();
}

void Voice::restartFromIdle() noexcept
{
    // The voice is silent here (ENV-2 idle), so none of this is audible. ENV-1 may still have
    // been releasing when ENV-2 finished; it froze at a level that depends on where the
    // sub-block ended, and a new note must not start its filter/pitch envelope part-way open.
    env1.reset();
    hpf.reset();
    vcf.reset();
    snapSlope(); // a slope crossfade of the previous note ends with it

    // Offline: a note that starts while Cross Mod is up renders at the real-time rate.
    rtPair = rtPairRatio == 2 && crossModWanted;
    rtHist.fill (0.0f);

    const bool forced = restartPhasesSet;
    restartPhasesSet = false;
    if (freshPhases && ! forced)
    {
        freshPhases = false; // the first note keeps the prepare-time phases
        return;
    }
    freshPhases = false;
    const std::uint32_t s = restartRng.nextUInt();
    vco1.setSeed (s * 2654435761u + 1u);
    vco2.setSeed (s * 2246822519u + 7u);
    const float p1 = restartRng.unipolar(), p2 = restartRng.unipolar();
    vco1.reset (forced ? restartPhase1 : p1);
    vco2.reset (forced ? restartPhase2 : p2);
    mirrorRealTimePair();
}

void Voice::mirrorRealTimePair() noexcept
{
    // Same start state as the main pair (phases, noise seeds, empty BLEP rings): the real-time
    // pair then renders what the real-time plugin renders for this note.
    vco1Rt = vco1;
    vco2Rt = vco2;
}

void Voice::start (int note, float vel, float levelScale, float pos) noexcept
{
    if (! env2.isActive())
        restartFromIdle();

    if (env2.isActive())
    {
        // Retrigger: move velocity and level smoothly (a step in gain would click).
        velDev = (velTgt + velDev) - vel;
        scaleDev = (scaleTgt + scaleDev) - levelScale;
    }
    else
    {
        velDev = scaleDev = 0.0f;
    }
    velTgt = vel;
    scaleTgt = levelScale;
    unisonPos = pos;
    glideDev = 0.0f;
    glideCur = 0.0;
    currentNote = note;
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
    const int n = m.numSamples;
    blockClock = clock;
    clock += static_cast<unsigned> (n);
    if (! env2.isActive())
    {
        // A steal fade keeps counting even if the old note has finished meanwhile.
        fadeRemaining = fadeRemaining > n ? fadeRemaining - n : 0;
        idleAt = 0;
        return false;
    }

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
    // It advances only while the voice sounds: if ENV-2 finished inside this sub-block, only up
    // to that sample (where it finished must not depend on how the render is split).
    int activeN = n;
    if (! env2.isActive())
    {
        activeN = 0;
        for (int i = n - 1; i >= 0; --i)
            if (e2[i] > 0.0f)
            {
                activeN = i + 1;
                break;
            }
    }
    idleAt = activeN;
    // The walk itself (an exp) is only evaluated when Vintage can make it audible.
    float walk0 = 0.0f;
    if (vintage > 0.0f)
        walk0 = driftValid ? driftWalk : drift.current();
    drift.advance (activeN);
    if (vintage > 0.0f)
        driftWalk = drift.current();
    driftValid = vintage > 0.0f;
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

    // Portamento (semitones above the note, closed-form RC) and velocity (smoothed per voice).
    float glideSemis[kN], vel[kN], levelScale[kN];
    {
        if (std::fpclassify (glideDev) != FP_ZERO) // same as != 0.0f; GCC's -Wfloat-equal rejects the operator
        {
            if (! glideAnchored || std::bit_cast<std::uint64_t> (m.glideLog) != std::bit_cast<std::uint64_t> (glideLog))
            {
                // New glide, or the portamento time changed: continue from the current deviation.
                glideStart = glideCur;
                glideLog = m.glideLog;
                glideK = 0;
                glideAnchored = true;
            }
            // Per sample in double, rounded to float once: the values are then the same however
            // the render is split (a float base rounded per sub-block left -91 dB at 1-sample blocks).
            const double g0 = glideCur; // = glideStart * g^glideK
            const double* gd = m.glideDecayD;
            for (int i = 0; i < n; ++i)
                glideSemis[i] = static_cast<float> (g0 * gd[i + 1]);
            glideK += n;
            const double next = glideStart * std::exp (glideLog * static_cast<double> (glideK));
            // 1e-9 semitone: what is left can no longer shift the phase measurably.
            glideCur = std::abs (next) < 1.0e-9 ? 0.0 : next;
            glideDev = static_cast<float> (glideCur);
        }
        else
        {
            for (int i = 0; i < n; ++i)
                glideSemis[i] = 0.0f;
        }

        const float* sd = m.smoothDecay;
        const float v0 = velTgt, vd = velDev, s0 = scaleTgt, sdv = scaleDev;
        for (int i = 0; i < n; ++i)
        {
            vel[i] = v0 + vd * sd[i + 1];
            levelScale[i] = s0 + sdv * sd[i + 1];
        }
        velDev = std::abs (vd) < 1.0e-20f ? 0.0f : vd * sd[n];
        scaleDev = std::abs (sdv) < 1.0e-20f ? 0.0f : sdv * sd[n];
    }

    {
        const float* bendOct = m.ctl[ModBlock::BendOct];
        const float* lfoOct = m.ctl[ModBlock::VcoLfoOct];
        const float* envOct = m.ctl[ModBlock::VcoEnvOct];
        const float* fineOct = m.ctl[ModBlock::Vco2FineOct];
        const float* pwmDepth = m.ctl[ModBlock::PwmDepth];
        const float* spread = m.ctl[ModBlock::UnisonSpreadOct];
        const float pos = unisonPos;
        for (int i = 0; i < n; ++i)
        {
            const float dyn = dyn0 + dynStep * static_cast<float> (i + 1);
            const float pitchMod = lfo[i] * lfoOct[i] + e1Mod[i] * envOct[i];
            // Keyboard-related offsets (glide, unison detune); Lo Freq VCO-2 ignores them like the bender.
            const float keyOct = glideSemis[i] * (1.0f / 12.0f) + pos * spread[i];
            const float log2F2 = base2 + dyn + fineOct[i] + bend2 * (bendOct[i] + keyOct) + mod2 * pitchMod;
            const float log2F1 = base1 + dyn + bendOct[i] + keyOct + mod1 * pitchMod;
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
        const float* velVca = m.ctl[ModBlock::VelToVca];
        const float* velVcf = m.ctl[ModBlock::VelToVcf];
        const float keyDist = note - 60.0f;
        const float driftOct = drift.cutoffOct * dsp::Drift::kCutoffOct * vintage;
        // A note on the real-time-rate path (offline) takes its VCF coefficients at that rate.
        const float piOverFs = rtPair ? dsp::kPi * (2.0f * invFs) : dsp::kPi * invFs;
        const float maxLog2 = rtPair ? maxLog2CutoffRt : maxLog2Cutoff;
        for (int i = 0; i < n; ++i)
        {
            const float envDepth = envOct[i] * map::velocityScale (velVcf[i], vel[i]);
            const float log2Fc = cutoff[i] + env[i] * envDepth + lfo[i] * lfoOct[i] + keyOct[i] * (keyDist + glideSemis[i]) + driftOct;
            const float fc = dsp::fast::exp2 (dsp::clamp (log2Fc, 3.0f, maxLog2));
            const auto c = dsp::OtaLadder::makeCoefs (dsp::fast::tanPrewarp (fc * piOverFs), resK[i]);
            cG[i] = c.G;
            c1[i] = c.c1;
            c2[i] = c.c2;
            c3[i] = c.c3;
            c4[i] = c.c4;
            cK[i] = c.k;
            cInv[i] = c.inv;
            gain[i] = e2[i] * m.vcaGain[i] * c.comp * map::velocityScale (velVca[i], vel[i]) * levelScale[i];
        }
    }

    // Steal fade: linear to 0 at the fade's last sample. The Part never renders past it.
    if (fadeRemaining > 0)
    {
        const float invLen = 1.0f / static_cast<float> (fadeLength);
        const float rem = static_cast<float> (fadeRemaining);
        for (int i = 0; i < n; ++i)
            gain[i] *= std::max (0.0f, (rem - static_cast<float> (i + 1)) * invLen);
        fadeRemaining = fadeRemaining > n ? fadeRemaining - n : 0;
    }
    return true;
}

void Voice::renderBlock (Voice* const* voices, int numVoices, const ModBlock& m, float* out, float* outRtPath) noexcept
{
    const int n = m.numSamples;
    for (int i = 0; i < n; ++i)
        out[i] = 0.0f;
    float* rtOut = outRtPath != nullptr ? outRtPath : out;
    if (outRtPath != nullptr)
        for (int i = 0; i < n; ++i)
            outRtPath[i] = 0.0f;

    const float* hpfG = m.ctl[ModBlock::HpfG];
    const float* crossOct = m.ctl[ModBlock::CrossModOct];
    const float* mix = m.ctl[ModBlock::SourceMix];

    // Voices are interleaved in groups of kVoiceGroup (fixed-size list on the stack); any number
    // of voices is rendered, a part's 16 in one group.
    for (int first = 0; first < numVoices; first += kVoiceGroup)
    {
        Voice* active[kVoiceGroup];
        Voice* rtPairs[kVoiceGroup]; // offline Cross Mod notes rendered at the real-time rate
        int numActive = 0, numRtPairs = 0;
        const int last = std::min (numVoices, first + kVoiceGroup);
        for (int v = first; v < last; ++v)
            if (voices[v]->beginBlock (m))
            {
                if (voices[v]->rtPair)
                    rtPairs[numRtPairs++] = voices[v];
                else
                    active[numActive++] = voices[v];
            }

        for (int i = 0; i < n; ++i)
        {
            float sum = 0.0f;
            for (int a = 0; a < numActive; ++a)
                sum += active[a]->renderSample (i, hpfG[i], crossOct[i], mix[i]);
            out[i] += sum;
        }
        if (numRtPairs > 0) // offline only
            for (int i = 0; i < n; ++i)
            {
                float sum = 0.0f;
                for (int a = 0; a < numRtPairs; ++a)
                    sum += rtPairs[a]->renderSampleRtPair (i, hpfG[i], crossOct[i], mix[i]);
                rtOut[i] += sum;
            }
    }
}
} // namespace vs
