#pragma once

// B-Side's value -> unit mappings (docs/architecture.md 8.7), shared by the display text and, later, by
// the DSP, so that what a host shows is what the engine does. JUCE-free and header-only: the display
// text of each parameter is built from these in ParameterLayout.cpp.
//
// Every host value of a B-Side per-part parameter is a plain value in its own unit (cents, percent,
// register units, an integer step); nothing is normalised 0..1 except where the contract says
// "0 ... 1" (velocity amounts, mod wheel vibrato, unison detune, portamento time).

#include <array>
#include <cmath>

namespace bs::map
{
// ---- Pulse width and macro offsets: percent -> chip register units --------------------------------
inline constexpr int kPulseWidthMax = 4095; // 12-bit pulse width register

/** Pulse width in percent (0 ... 100) -> the continuous register value 0 ... 4095. In Smooth resolution the
    chip is written with this value as it is (8.4.6: pulse width and cutoff are continuous); Authentic
    rounds it with `authenticRegister`. */
inline float pulseWidthRegister (float percent) noexcept
{
    const float p = percent < 0.0f ? 0.0f : (percent > 100.0f ? 100.0f : percent);
    return p * static_cast<float> (kPulseWidthMax) / 100.0f;
}

/** PW offset in percent (-50 ... +50) -> a continuous register offset of -2048 ... +2048. */
inline float pwOffsetRegister (float percent) noexcept
{
    const float p = percent < -50.0f ? -50.0f : (percent > 50.0f ? 50.0f : percent);
    return p * 2048.0f / 50.0f;
}

/** Cutoff offset in percent (-100 ... +100) -> a continuous register offset of -2047 ... +2047. */
inline float cutoffOffsetRegister (float percent) noexcept
{
    const float p = percent < -100.0f ? -100.0f : (percent > 100.0f ? 100.0f : percent);
    return p * 2047.0f / 100.0f;
}

/** A continuous register value as the integer the chip stores (Authentic resolution, and display text where an
    integer is shown): rounded to nearest (halves away from zero) and limited to lo ... hi. A non-finite value gives lo. */
inline int authenticRegister (float value, int lo, int hi) noexcept
{
    if (! std::isfinite (value))
        return lo;
    const float c = value < static_cast<float> (lo) ? static_cast<float> (lo) : (value > static_cast<float> (hi) ? static_cast<float> (hi) : value);
    return static_cast<int> (std::lround (c));
}

/** Cutoff is shown and used in register units (0 ... 2047) until S6 shows Hz. */
inline constexpr int kCutoffMax = 2047;

/** Percent (0 ... 100) -> fraction 0 ... 1 (filter key follow, chip variation). */
inline float percentToFraction (float percent) noexcept { return percent * 0.01f; }

/** Sweep depth in percent (0 ... 200): the factor applied to every sweep and pingpong step (100 % = 1). */
inline float sweepDepthFactor (float percent) noexcept { return percent * 0.01f; }

// ---- Pitch -----------------------------------------------------------------------------------------
/** Fine tune: the host value is in cents already (-100 ... +100). */
inline float fineSemitones (float cents) noexcept { return cents * 0.01f; }

/** Mod wheel vibrato 0 ... 1 -> 0 ... 100 cents of depth. */
inline float wheelVibratoCents (float v) noexcept { return v * 100.0f; }
inline float wheelVibratoFromCents (float cents) noexcept { return cents * 0.01f; }

/** Unison detune 0 ... 1 -> 0 ... 50 cents (spread of the unison voices). */
inline float unisonDetuneCents (float v) noexcept { return v * 50.0f; }
inline float unisonDetuneFromCents (float cents) noexcept { return cents * 0.02f; }

// ---- Envelope times --------------------------------------------------------------------------------
/** The nominal attack times of the 16 envelope settings, in milliseconds (8.7). Decay and release
    are three times these. */
inline constexpr std::array<int, 16> kAttackMs { 2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000 };
inline constexpr int kDecayReleaseFactor = 3;

enum class EnvStage { Attack, Decay, Release };

/** Nominal time in milliseconds of an envelope setting 0 ... 15 for a stage. */
inline int envNominalMs (EnvStage stage, int setting) noexcept
{
    const int s = setting < 0 ? 0 : (setting > 15 ? 15 : setting);
    const int base = kAttackMs[static_cast<size_t> (s)];
    return stage == EnvStage::Attack ? base : base * kDecayReleaseFactor;
}

/** The setting 0 ... 15 whose nominal time is nearest to `ms` (nearest in the log domain). */
inline int envSettingFromMs (EnvStage stage, double ms) noexcept
{
    int best = 0;
    double bestDistance = 1.0e300;
    const double target = ms < 0.001 ? 0.001 : ms;
    for (int i = 0; i < 16; ++i)
    {
        const double d = std::fabs (std::log (static_cast<double> (envNominalMs (stage, i)) / target));
        if (d < bestDistance)
        {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

// ---- Tick rate ---------------------------------------------------------------------------------------
inline constexpr float kTickFreeMinHz = 10.0f;
inline constexpr float kTickFreeMaxHz = 400.0f;

/** Free tick rate: normalised position 0 ... 1 -> 10 ... 400 Hz, exponential. */
inline float tickFreeHz (float norm) noexcept
{
    const float n = norm < 0.0f ? 0.0f : (norm > 1.0f ? 1.0f : norm);
    return kTickFreeMinHz * std::pow (kTickFreeMaxHz / kTickFreeMinHz, n);
}
inline float tickFreeNorm (float hz) noexcept
{
    const float h = hz < kTickFreeMinHz ? kTickFreeMinHz : (hz > kTickFreeMaxHz ? kTickFreeMaxHz : hz);
    return std::log (h / kTickFreeMinHz) / std::log (kTickFreeMaxHz / kTickFreeMinHz);
}

/** Tempo-synced tick rates: ticks per beat, by choice index. */
inline constexpr std::array<int, 4> kTickSyncPerBeat { 6, 12, 24, 48 };

// ---- Voice assignment ----------------------------------------------------------------------------------
/** Unison voice count by choice index. */
inline constexpr std::array<int, 4> kUnisonVoiceCounts { 2, 4, 8, 16 };

// ---- Vibrato -------------------------------------------------------------------------------------------
inline constexpr float kVibratoMinHz = 0.5f;
inline constexpr float kVibratoMaxHz = 12.0f;
} // namespace bs::map
