#pragma once

#include <cmath>
#include <cstdint>

namespace vs::dsp
{
inline constexpr float kPi = 3.14159265358979323846f;

/** xorshift32: tiny, deterministic, allocation-free RNG for the audio thread. */
class Random
{
public:
    explicit Random (std::uint32_t seed = 0x9E3779B9u) noexcept { setSeed (seed); }

    void setSeed (std::uint32_t seed) noexcept { state = seed != 0 ? seed : 0x9E3779B9u; }

    std::uint32_t nextUInt() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    /** Uniform in [-1, 1). */
    float bipolar() noexcept { return static_cast<float> (static_cast<std::int32_t> (nextUInt())) * (1.0f / 2147483648.0f); }

    /** Uniform in [0, 1). */
    float unipolar() noexcept { return static_cast<float> (nextUInt() >> 8) * (1.0f / 16777216.0f); }

private:
    std::uint32_t state = 0x9E3779B9u;
};

/**
    One-pole coefficient 1 - e^(-1 / (tau fs)) for time constant tau (seconds) at sampleRate,
    in double via expm1. At high internal rates (up to 384 kHz) the argument is ~1e-6 and
    1 - exp() in float loses several percent (a 10 s time was 7.7 % long at 384 kHz).
*/
inline double onePoleCoefD (double tauSeconds, double sampleRate) noexcept
{
    return tauSeconds <= 0.0 ? 1.0 : -std::expm1 (-1.0 / (tauSeconds * sampleRate));
}

/** onePoleCoefD rounded once to float (exact to float precision at any rate). */
inline float onePoleCoef (float tauSeconds, float sampleRate) noexcept
{
    return static_cast<float> (onePoleCoefD (static_cast<double> (tauSeconds), static_cast<double> (sampleRate)));
}

inline float midiNoteToHz (float note) noexcept { return 440.0f * std::exp2 ((note - 69.0f) / 12.0f); }

template <typename T>
constexpr T clamp (T v, T lo, T hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace vs::dsp
