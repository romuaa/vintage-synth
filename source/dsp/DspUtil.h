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

/** One-pole smoothing coefficient for time constant tau (seconds) at sampleRate. */
inline float onePoleCoef (float tauSeconds, float sampleRate) noexcept
{
    return tauSeconds <= 0.0f ? 1.0f : 1.0f - std::exp (-1.0f / (tauSeconds * sampleRate));
}

inline float midiNoteToHz (float note) noexcept { return 440.0f * std::exp2 ((note - 69.0f) / 12.0f); }

template <typename T>
constexpr T clamp (T v, T lo, T hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace vs::dsp
