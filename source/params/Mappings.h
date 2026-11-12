#pragma once

// Normalised panel value -> physical unit. Shared by DSP and parameter display text,
// so what the host shows is exactly what the engine does.

#include <cmath>

namespace vs::map
{
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

/** VCA level slider -> linear gain (squared: perceptually even). */
inline float vcaGain (float v) noexcept { return v * v; }

/** VCA LFO-mod switch position 0..3 -> tremolo depth (max dip, linear). */
inline float vcaLfoDepth (int position) noexcept
{
    constexpr float depths[] { 0.0f, 0.25f, 0.5f, 1.0f };
    return depths[position < 0 ? 0 : (position > 3 ? 3 : position)];
}

/** Resonance slider -> ladder feedback gain k. k >= 4 self-oscillates. */
inline float resonanceK (float v) noexcept { return 4.15f * v; }

/** Modulation depths (slider 0..1 -> octaves at full modulator swing). */
inline float vcoLfoModOct (float v) noexcept { return 2.0f * v * v; }
inline float vcoEnvModOct (float v) noexcept { return 4.0f * v * v; }
inline float crossModOct (float v) noexcept { return 4.0f * v * v; }
inline float vcfEnvModOct (float v) noexcept { return 7.0f * v; }
inline float vcfLfoModOct (float v) noexcept { return 4.0f * v * v; }

/** Portamento: time to cover 99 % of the interval, 0 = off, else 5 s x v^2. */
inline float portaTimeSec (float v) noexcept { return 5.0f * v * v; }
inline float portaFromSec (float s) noexcept { return std::sqrt ((s < 0.0f ? 0.0f : s) / 5.0f); }

/** Unison detune: total spread in cents (outermost voices are +-half of it). */
inline float unisonDetuneCents (float v) noexcept { return 50.0f * v; }

/** Velocity amount: gain (or VCF ENV depth) = 1 - amount x (1 - velocity). */
inline float velocityScale (float amount, float velocity) noexcept { return 1.0f - amount * (1.0f - velocity); }

/** VCO LFO-mod slider plus the mod wheel's share (wheel 0..1 x depth), capped at the panel maximum. */
inline float vcoLfoModWithWheel (float slider, float wheel, float depth) noexcept
{
    const float v = slider + wheel * depth;
    return v < 1.0f ? v : 1.0f;
}

/** Choice index -> unison voice count (2, 4, 8, 16). */
inline int unisonVoices (int index) noexcept { return 2 << (index < 0 ? 0 : (index > 3 ? 3 : index)); }

/** Choice index -> pitch-bend range in semitones (0..12). */
inline float bendRangeSemis (int index) noexcept { return static_cast<float> (index < 0 ? 0 : (index > 12 ? 12 : index)); }

struct PanGains
{
    float left, right;
};

/**
    Constant-power pan, -1 (left) .. +1 (right): unity in both channels at the centre,
    left^2 + right^2 = 2 everywhere (+3 dB in one channel at the extremes, the other silent).
*/
inline PanGains panGains (float pan) noexcept
{
    constexpr float kSqrt2 = 1.414213562f;
    if (std::isnan (pan) || std::fpclassify (pan) == FP_ZERO)
        return { 1.0f, 1.0f }; // exactly unity at the centre (sqrt2 x cos (pi/4) rounds in float)
    if (pan <= -1.0f)
        return { kSqrt2, 0.0f }; // exactly silent on the far side (cos (pi/2) is not 0 in float)
    if (pan >= 1.0f)
        return { 0.0f, kSqrt2 };
    const float theta = (pan + 1.0f) * 0.785398163f; // 0 .. pi/2
    return { kSqrt2 * std::cos (theta), kSqrt2 * std::sin (theta) };
}

struct BalanceGains
{
    float lower, upper;
};

/** Constant-power part balance, -1 (Lower only) .. +1 (Upper only): both parts at unity at 0. */
inline BalanceGains balanceGains (float balance) noexcept
{
    const auto g = panGains (balance);
    return { g.left, g.right };
}
} // namespace vs::map
