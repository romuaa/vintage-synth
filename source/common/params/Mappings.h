#pragma once

// Normalised panel value -> physical unit. Shared by DSP and parameter display text,
// so what the host shows is exactly what the engine does. This file holds what every
// instrument shares (sanitising, tapers used by the display text, pan/balance law, effects,
// arpeggiator/sequencer); the voice-specific mappings are in vintage/params/PartMappings.h.

#include <cmath>

namespace vs::map
{
/** A parameter value as the host left it, made safe for the engine: a non-finite value (NaN,
    +-Inf) becomes the parameter's default, anything else is clamped to its range. In-range values
    pass unchanged (bit-exact). */
inline float sanitiseParam (float v, float lo, float hi, float def) noexcept
{
    if (! std::isfinite (v))
        return def;
    return v < lo ? lo : (v > hi ? hi : v);
}

/** The same for a choice index or integer parameter (float -> int of NaN is undefined behaviour). */
inline int sanitiseIndex (float v, int lo, int hi, int def) noexcept
{
    if (! std::isfinite (v))
        return def;
    const float c = sanitiseParam (v, static_cast<float> (lo), static_cast<float> (hi), static_cast<float> (def));
    return static_cast<int> (std::lround (c));
}

inline constexpr float kLog2CutoffMinHz = 4.321928f; // log2(20)
inline constexpr float kCutoffOctaves = 10.0f;      // 20 Hz .. 20.48 kHz

/** VCF cutoff in octaves (log2 Hz). Modulation is added in this domain. */
inline float cutoffLog2Hz (float v) noexcept { return kLog2CutoffMinHz + kCutoffOctaves * v; }
inline float cutoffHz (float v) noexcept { return std::exp2 (cutoffLog2Hz (v)); }
inline float cutoffFromHz (float hz) noexcept { return (std::log2 (hz) - kLog2CutoffMinHz) / kCutoffOctaves; }

/** HPF: 5 Hz (DC blocker) .. 2.56 kHz. */
inline float hpfHz (float v) noexcept { return 5.0f * std::exp2 (9.0f * v); }
inline float hpfFromHz (float hz) noexcept { return std::log2 (hz / 5.0f) / 9.0f; }

/** LFO rate: 0.05 Hz .. 40 Hz. */
inline float lfoRateHz (float v) noexcept { return 0.05f * std::pow (800.0f, v); }
inline float lfoRateFromHz (float hz) noexcept { return std::log (hz / 0.05f) / std::log (800.0f); }

/** LFO delay: 0 .. 5 s, squared taper for fine control of short delays. */
inline float lfoDelaySec (float v) noexcept { return 5.0f * v * v; }
inline float lfoDelayFromSec (float s) noexcept { return std::sqrt (s / 5.0f); }

/** Envelope segment time: 1 ms .. 10 s. */
inline float envTimeSec (float v) noexcept { return 0.001f * std::pow (10000.0f, v); }
inline float envTimeFromSec (float s) noexcept { return std::log10 (s / 0.001f) / 4.0f; }

/** VCO-2 fine tune: +-1 semitone (100 cents). */
inline float fineTuneSemis (float v) noexcept { return v; }

/** Portamento: time to cover 99 % of the interval, 0 = off, else 5 s x v^2. */
inline float portaTimeSec (float v) noexcept { return 5.0f * v * v; }
inline float portaFromSec (float s) noexcept { return std::sqrt ((s < 0.0f ? 0.0f : s) / 5.0f); }

/** Unison detune: total spread in cents (outermost voices are +-half of it). */
inline float unisonDetuneCents (float v) noexcept { return 50.0f * v; }

struct PanGains
{
    float left, right;
};

/**
    Pan, -1 (left) .. +1 (right), with the classic balance-knob law (owner decision, M3 review):
    never a boost. Unity in both channels at the centre; toward a side, the near channel stays at
    exactly 1 and the far one falls along sqrt2 x cos (constant power's far-side curve), reaching
    exactly 0 at the extreme. Monotonic and continuous.
*/
inline PanGains panGains (float pan) noexcept
{
    if (std::isnan (pan) || std::fpclassify (pan) == FP_ZERO)
        return { 1.0f, 1.0f };
    if (pan <= -1.0f)
        return { 1.0f, 0.0f }; // exactly silent on the far side (cos (pi/2) is not 0 in float)
    if (pan >= 1.0f)
        return { 0.0f, 1.0f };
    constexpr float kSqrt2 = 1.414213562f;
    const float theta = (pan + 1.0f) * 0.785398163f; // 0 .. pi/2
    const float l = kSqrt2 * std::cos (theta), r = kSqrt2 * std::sin (theta);
    return { l < 1.0f ? l : 1.0f, r < 1.0f ? r : 1.0f };
}

struct BalanceGains
{
    float lower, upper;
};

/** Part balance, -1 (Lower only) .. +1 (Upper only): the same law as panGains (both at unity at 0, never above 1). */
inline BalanceGains balanceGains (float balance) noexcept
{
    const auto g = panGains (balance);
    return { g.left, g.right };
}

// ---- Effects (Wave 3, docs/architecture.md 6a.2). Slider 0..1 -> the effect block's units. ----

/** Delay time: 1 ms .. 2 s, exponential (2000:1 over the slider). */
inline constexpr float kDelayMinSec = 0.001f, kDelayMaxSec = 2.0f;
inline float delayTimeSec (float v) noexcept { return kDelayMinSec * std::pow (kDelayMaxSec / kDelayMinSec, v); }
inline float delayTimeFromSec (float s) noexcept
{
    const float c = s < kDelayMinSec ? kDelayMinSec : (s > kDelayMaxSec ? kDelayMaxSec : s);
    return std::log (c / kDelayMinSec) / std::log (kDelayMaxSec / kDelayMinSec);
}

/** Delay feedback: 0 .. 95 % gain per repeat. */
inline constexpr float kDelayMaxFeedback = 0.95f;
inline float delayFeedback (float v) noexcept { return kDelayMaxFeedback * v; }
inline float delayFeedbackFromGain (float g) noexcept { return g / kDelayMaxFeedback; }

/** Reverb decay (RT60 at 1 kHz): 0.3 .. 20 s, exponential. */
inline constexpr float kReverbMinDecaySec = 0.3f, kReverbMaxDecaySec = 20.0f;
inline float reverbDecaySec (float v) noexcept { return kReverbMinDecaySec * std::pow (kReverbMaxDecaySec / kReverbMinDecaySec, v); }
inline float reverbDecayFromSec (float s) noexcept
{
    const float c = s < kReverbMinDecaySec ? kReverbMinDecaySec : (s > kReverbMaxDecaySec ? kReverbMaxDecaySec : s);
    return std::log (c / kReverbMinDecaySec) / std::log (kReverbMaxDecaySec / kReverbMinDecaySec);
}

/** Reverb pre-delay: 0 .. 200 ms, linear. */
inline constexpr float kReverbMaxPreDelaySec = 0.2f;
inline float reverbPreDelaySec (float v) noexcept { return kReverbMaxPreDelaySec * v; }
inline float reverbPreDelayFromSec (float s) noexcept { return s / kReverbMaxPreDelaySec; }

// ---- Arpeggiator / sequencer (Wave 3). These parameters are in physical units already. ----

/** Swing (50 .. 75 %) and gate (5 .. 100 %) parameters -> fraction for the module. */
inline double percentToFraction (float percent) noexcept { return static_cast<double> (percent) / 100.0; }

/** Sequencer step offset in semitones, -24 .. +24 (shown signed: "+3 st", "0 st", "-12 st"). */
inline int clampStepSemitones (int st) noexcept { return st < -24 ? -24 : (st > 24 ? 24 : st); }
} // namespace vs::map
