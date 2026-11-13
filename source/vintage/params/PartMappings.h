#pragma once

// Voice-specific panel value -> physical unit mappings of this instrument (VCA, resonance, modulation
// depths, velocity, unison voices, bend range). Includes the shared mappings (common/params/Mappings.h).

#include "common/params/Mappings.h"

namespace vs::map
{
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
} // namespace vs::map
