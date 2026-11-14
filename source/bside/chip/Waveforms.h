#pragma once

// The digital (12-bit) waveform outputs of one oscillator (docs/architecture.md 8.4.3 rows 2-5, 8).
// These are the values the chip's waveform logic produces before its DAC; the read register 0x1B and
// the noise write-back use them. S2b renders the band-limited analog form of the same shapes and
// takes the shapes from here, so both agree. JUCE-free, constexpr, no state.

#include "bside/chip/ChipTypes.h"

#include <cstdint>

namespace vs::bside::chip::wave
{
/** Saw = accumulator bits 23..12 (row 2). */
constexpr std::uint16_t saw (std::uint32_t acc) noexcept { return static_cast<std::uint16_t> ((acc >> 12) & 0xfff); }

/** Triangle = bits 22..11 of acc XOR (fold bit ? all ones : 0). The fold bit is the own top bit, XORed with
    the ring source's top bit when ring is on (row 3). */
constexpr std::uint16_t triangle (std::uint32_t acc, bool ring, std::uint32_t ringSourceAcc) noexcept
{
    std::uint32_t fold = acc & 0x800000u;
    if (ring)
        fold ^= ringSourceAcc & 0x800000u;
    const std::uint32_t v = fold != 0 ? ~acc : acc;
    return static_cast<std::uint16_t> ((v >> 11) & 0xfff);
}

/** Pulse = 0xfff when bits 23..12 >= PW, else 0; the test bit forces it high (row 4). */
constexpr std::uint16_t pulse (std::uint32_t acc, std::uint16_t pw, bool test) noexcept
{
    return test || ((acc >> 12) & 0xfff) >= pw ? std::uint16_t { 0xfff } : std::uint16_t { 0 };
}

/** Noise output: bits 11..4 = register bits 20, 18, 14, 11, 9, 5, 2, 0; bits 3..0 = 0 (row 5). */
constexpr std::uint16_t noise (std::uint32_t reg) noexcept
{
    return static_cast<std::uint16_t> ((((reg >> 20) & 1u) << 11) | (((reg >> 18) & 1u) << 10) | (((reg >> 14) & 1u) << 9)
                                       | (((reg >> 11) & 1u) << 8) | (((reg >> 9) & 1u) << 7) | (((reg >> 5) & 1u) << 6)
                                       | (((reg >> 2) & 1u) << 5) | ((reg & 1u) << 4));
}

/** The noise register bits that the output bits 11..4 come from, as a mask built from a 12-bit output:
    a set output bit keeps its register bit, a clear one clears it (the write-back of row 8). */
constexpr std::uint32_t noiseKeepMask (std::uint16_t out) noexcept
{
    constexpr std::uint32_t outputBits = (1u << 20) | (1u << 18) | (1u << 14) | (1u << 11) | (1u << 9) | (1u << 5) | (1u << 2) | 1u;
    const std::uint32_t keep = (((out >> 11) & 1u) << 20) | (((out >> 10) & 1u) << 18) | (((out >> 9) & 1u) << 14) | (((out >> 8) & 1u) << 11)
                               | (((out >> 7) & 1u) << 9) | (((out >> 6) & 1u) << 5) | (((out >> 5) & 1u) << 2) | ((out >> 4) & 1u);
    return (kNoiseMask & ~outputBits) | keep;
}

/** The digital output for the selected waveform bits (control bits 7..4).
    One waveform: that waveform. None: 0. Several (row 8, "combined waveforms"): per model.

    AS BUILT (S2a), both models: the bitwise AND of the selected outputs. This is the documented
    first-order behaviour (a selected waveform can only pull output bits low), not the chip's measured
    combined shapes, which 8.4.5 fits per model from reference data (Q5). This function is the one place
    the fitted digital model replaces; the read register 0x1B and the noise write-back follow it. */
constexpr std::uint16_t combined (Model model, std::uint8_t waveBits, std::uint16_t tri, std::uint16_t sawOut, std::uint16_t pulseOut,
                                  std::uint16_t noiseOut) noexcept
{
    (void) model;
    if ((waveBits & ctrl::waveMask) == 0)
        return 0;
    std::uint16_t v = 0xfff;
    if ((waveBits & ctrl::triangle) != 0)
        v &= tri;
    if ((waveBits & ctrl::saw) != 0)
        v &= sawOut;
    if ((waveBits & ctrl::pulse) != 0)
        v &= pulseOut;
    if ((waveBits & ctrl::noise) != 0)
        v &= noiseOut;
    return v;
}

/** Whether the selected waveforms write back into the noise register: noise plus at least one other. */
constexpr bool writesBack (std::uint8_t control) noexcept
{
    return (control & ctrl::noise) != 0 && (control & (ctrl::triangle | ctrl::saw | ctrl::pulse)) != 0 && (control & ctrl::test) == 0;
}
} // namespace vs::bside::chip::wave
