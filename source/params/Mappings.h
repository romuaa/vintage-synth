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
} // namespace vs::map
