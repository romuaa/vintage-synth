#pragma once

// The voice's oscillator path as shared code (docs/architecture.md 9.4): how a part's parameters
// configure the two oscillators (OscConfig), the pulse-width mapping, and one sample of the VCO pair
// with cross mod and sync. Voice uses exactly these functions, and so does the editor's
// oscilloscope picture (OscScope.h), so the picture cannot drift from the sound. JUCE-free.

#include "common/dsp/DspUtil.h"
#include "common/dsp/FastMath.h"
#include "vintage/dsp/Oscillator.h"
#include "vintage/params/PartParamList.h"

namespace vs
{
namespace osc
{
inline constexpr float kMinDt = 1.0e-7f, kMaxDt = 0.45f; // phase increment limits (cycles/sample)

enum PwmSource
{
    kPwmLfo = 0,
    kPwmManual = 1,
    kPwmEnv1 = 2
};
} // namespace osc

/** Everything about the oscillators that the part's switches set (no continuous values). */
struct OscConfig
{
    int wave1 = 2, wave2 = 2;    // kVcoWaveLabels order
    int octave1 = 0, octave2 = 0; // octaves relative to 8'
    bool loFreq2 = false;        // VCO-2 "Lo Freq" mode
    bool sync = false;
    int pwmSource = osc::kPwmLfo;
    bool env1Inverted = false;   // ENV-1 polarity (moves the pulse width with source ENV-1)

    bool operator== (const OscConfig&) const = default; // integers and bools only
};

/** The oscillator configuration of a part's parameters (Voice::setBlockParams uses this). */
inline OscConfig oscConfigFor (const PartParams& p) noexcept
{
    OscConfig c;
    c.wave1 = p.vco1Wave;
    c.wave2 = p.vco2Wave;
    c.octave1 = p.vco1Range - kVco1RangeIndexOf8ft;
    c.octave2 = p.vco2Range - kVco2RangeIndexOf8ft;
    c.loFreq2 = p.vco2Mode == 1;
    c.sync = p.vcoSync == 1;
    c.pwmSource = p.pwmSource;
    c.env1Inverted = p.env1Polarity == 1;
    return c;
}

/** PWM amount = a + lfo x LFO + env x ENV-1, clamped to 0..1 (per source). */
struct PwmCoefs
{
    float a = 1.0f, lfo = 0.0f, env = 0.0f;
};

inline PwmCoefs pwmCoefs (int pwmSource, bool env1Inverted) noexcept
{
    PwmCoefs c;
    if (pwmSource == osc::kPwmLfo)
    {
        c.a = 0.5f;
        c.lfo = 0.5f;
    }
    else if (pwmSource == osc::kPwmEnv1)
    {
        c.a = env1Inverted ? 1.0f : 0.0f;
        c.env = env1Inverted ? -1.0f : 1.0f;
    }
    return c;
}

inline float pwmAmount (const PwmCoefs& c, float lfo, float env1) noexcept
{
    return dsp::clamp (c.a + c.lfo * lfo + c.env * env1, 0.0f, 1.0f);
}

/** Pulse width for a PWM depth (slider 0..1) and amount. */
inline float pulseWidth (float depth, float amount) noexcept
{
    return 0.5f - 0.45f * depth * amount;
}

/** The pulse width at rest: LFO at its centre, ENV-1 at 0 (no note). */
inline float restingPulseWidth (const OscConfig& c, float depth) noexcept
{
    return pulseWidth (depth, pwmAmount (pwmCoefs (c.pwmSource, c.env1Inverted), 0.0f, 0.0f));
}

/** One sample of the VCO pair (Voice::renderSample): VCO-2 first, then VCO-1 with audio-rate
    cross mod (2^(log2 f1 + depth x o2)) and hard sync to VCO-2's wrap. */
inline void oscPairSample (dsp::Oscillator& vco1, dsp::Oscillator& vco2, float dt1Raw, float dt2, float width, bool crossModOn,
                           float crossModOct, bool sync, float& o1, float& o2) noexcept
{
    float wrap2 = -1.0f, unused = -1.0f;
    o2 = vco2.process (dt2, width, -1.0f, wrap2);
    const float dt1 = dsp::clamp (crossModOn ? dt1Raw * dsp::fast::exp2 (crossModOct * o2) : dt1Raw, osc::kMinDt, osc::kMaxDt);
    o1 = vco1.process (dt1, width, sync ? wrap2 : -1.0f, unused);
}
} // namespace vs
