#pragma once

// Types and constants of the chip model (docs/architecture.md 8.4.1, 8.4.3, 8.4.7). JUCE-free.

#include <array>
#include <cstdint>

namespace vs::bside::chip
{
enum class Model : std::uint8_t { A, B };

struct Config
{
    Model model = Model::B;
    std::uint32_t clockHz = 985248;
    double internalRate = 96000.0;
    float variation = 0.5f; // Model A filter curve offset, 0..1 (analog, S3)
    bool volumeDc = false;  // analog, S3
    bool authentic = false; // register resolution (8.4.6)
};

/** One oscillator's input as the program engine sets it (8.4.7).
    `frequency` is in register units (the value F of 8.4.1: output frequency = F x clock / 2^24).
    Smooth: any F in [0, kMaxSmoothFrequency) with a fraction below the accumulator's last bit;
    pulseWidth a float in 0..4095. Authentic: both are rounded to integers, F clamped to 0..65535. */
struct OscInput
{
    double frequency = 0.0;
    float pulseWidth = 0.0f;
    std::uint8_t control = 0, attackDecay = 0, sustainRelease = 0;
};

/** The filter registers as the program engine sets them. `cutoff` 0..2047 (Smooth: continuous). */
struct FilterInput
{
    float cutoff = 0.0f;
    std::uint8_t resonance = 0, routing = 0, modeVolume = 0;
};

/** Register addresses (8.4.1). Oscillator n (0-based here) starts at 7 n. */
namespace reg
{
constexpr std::uint8_t freqLo = 0x00, freqHi = 0x01, pwLo = 0x02, pwHi = 0x03, control = 0x04, attackDecay = 0x05,
                       sustainRelease = 0x06;
constexpr int oscStride = 7;
constexpr std::uint8_t cutoffLo = 0x15, cutoffHi = 0x16, resonanceRouting = 0x17, modeVolume = 0x18;
constexpr std::uint8_t osc3Output = 0x1B, env3Output = 0x1C;
/** Write registers 0x00..0x18 are stored; 0x19..0x1F are read-only or not modelled. */
constexpr int kWritable = 0x19;
/** The chip decodes five address lines: an address is taken modulo 32. */
constexpr std::uint8_t kAddressMask = 0x1F;
} // namespace reg

/** Bits of the control register (8.4.1). */
namespace ctrl
{
constexpr std::uint8_t gate = 0x01, sync = 0x02, ring = 0x04, test = 0x08, triangle = 0x10, saw = 0x20, pulse = 0x40,
                       noise = 0x80;
constexpr std::uint8_t waveMask = 0xF0;
} // namespace ctrl

/** Envelope rate periods in cycles, nibble 0..15 (8.4.3 row 9). */
constexpr std::array<std::uint16_t, 16> kRatePeriods { 9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251 };

/** Envelope phase, numbered as the reference format's `phase` field. */
enum class EnvPhase : std::uint8_t { attack = 0, decaySustain = 1, release = 2 };

/** 15-bit rate counter: after 0x7fff it continues at 1 (period 32 767 on a wrap; 0 only after a match). */
constexpr std::uint16_t kRateCounterMax = 0x7fff;
/** Value of the noise shift register at power-on and while the test bit is set (23 ones). */
constexpr std::uint32_t kNoiseReset = 0x7fffff;
constexpr std::uint32_t kNoiseMask = 0x7fffff;

/** Accumulator phase in fixed point: the 24-bit accumulator in bits 55..32, a 32-bit fraction below
    (always 0 with Authentic resolution and with register writes). */
constexpr int kFractionBits = 32;
constexpr std::uint64_t kPhaseMask = (std::uint64_t { 1 } << 56) - 1;
constexpr std::uint64_t kPhaseMsb = std::uint64_t { 1 } << 55;  // accumulator bit 23
constexpr std::uint64_t kPhaseBit19 = std::uint64_t { 1 } << 51; // accumulator bit 19 (noise clock)
/** Largest frequency (register units, exclusive) the chip accepts: 2^19, so the accumulator's bit 19 can
    rise at most once per cycle. That is about 30 kHz at the 50 Hz system clock; Smooth uses up to 12 kHz. */
constexpr double kMaxSmoothFrequency = 524288.0;
constexpr std::uint64_t kMaxIncrement = (std::uint64_t { 1 } << 51) - 1;
} // namespace vs::bside::chip
