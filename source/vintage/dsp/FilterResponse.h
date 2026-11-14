#pragma once

// The voice filter's small-signal response, for the editor's filter curve (docs/architecture.md
// 9.4). JUCE-free. The exact transfer function of the code in Filters.h with tanh taken as the
// identity (the linear region): the TPT one-pole stage, the 4-pole ladder with its global feedback
// and passband compensation, and the TPT one-pole high-pass in front of it, evaluated at
// z = e^{jw} at the voice's real-time rate. Inputs are the parameters' own mappings, clamped like
// Voice: the picture is computed from the same numbers as the sound.

#include "vintage/params/PartMappings.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <complex>

namespace vs::dsp
{
struct FilterCurveSettings
{
    double cutoffHz = 1000.0; // after the Voice clamp (2^3 Hz .. 0.42 fs)
    double k = 0.0;           // ladder feedback (map::resonanceK)
    bool slope24 = true;
    double hpfHz = 5.0;       // map::hpfHz
    double sampleRate = 96000.0;

    /** Bitwise equality (no float ==): the picture is recomputed when any input changed. */
    bool operator== (const FilterCurveSettings& o) const noexcept
    {
        return std::bit_cast<std::uint64_t> (cutoffHz) == std::bit_cast<std::uint64_t> (o.cutoffHz)
               && std::bit_cast<std::uint64_t> (k) == std::bit_cast<std::uint64_t> (o.k) && slope24 == o.slope24
               && std::bit_cast<std::uint64_t> (hpfHz) == std::bit_cast<std::uint64_t> (o.hpfHz)
               && std::bit_cast<std::uint64_t> (sampleRate) == std::bit_cast<std::uint64_t> (o.sampleRate);
    }
};

/** Settings for the panel values at middle C without modulation: the cutoff clamp is Voice's
    (log2 fc in [3, log2 (0.42 fs)]). `slopeIndex`: 0 = -12 dB, 1 = -24 dB (kSlopeLabels). */
inline FilterCurveSettings filterCurveSettings (float cutoff, float resonance, int slopeIndex, float hpf, double sampleRate) noexcept
{
    FilterCurveSettings s;
    const double maxLog2 = std::log2 (0.42 * sampleRate);
    s.cutoffHz = std::exp2 (std::clamp (static_cast<double> (map::cutoffLog2Hz (cutoff)), 3.0, maxLog2));
    s.k = static_cast<double> (map::resonanceK (resonance));
    s.slope24 = slopeIndex != 0;
    s.hpfHz = static_cast<double> (map::hpfHz (hpf));
    s.sampleRate = sampleRate;
    return s;
}

/** |H| in dB at `hz` (0 < hz < fs/2). */
inline double filterMagnitudeDb (double hz, const FilterCurveSettings& s) noexcept
{
    using C = std::complex<double>;
    constexpr double pi = 3.14159265358979323846;
    const double w = 2.0 * pi * hz / s.sampleRate;
    const C zInv = std::polar (1.0, -w);

    // TPT one-pole low-pass: H1 = g (1 + z^-1) / ((1 + g) - (1 - g) z^-1), g = tan (pi fc / fs).
    const auto onePole = [&] (double fc) {
        const double g = std::tan (pi * fc / s.sampleRate);
        return g * (1.0 + zInv) / ((1.0 + g) - (1.0 - g) * zInv);
    };

    const C h1 = onePole (s.cutoffHz);
    const C h2 = h1 * h1;
    const C h4 = h2 * h2;
    const C lp = (s.slope24 ? h4 : h2) / (1.0 + s.k * h4) * (1.0 + 0.5 * s.k);
    const C hp = 1.0 - onePole (s.hpfHz);
    const double mag = std::abs (lp * hp);
    return 20.0 * std::log10 (std::max (mag, 1e-12));
}
} // namespace vs::dsp
