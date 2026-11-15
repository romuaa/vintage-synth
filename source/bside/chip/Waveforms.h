#pragma once

// The digital (12-bit) waveform logic of one oscillator (docs/architecture.md 8.15.2 (c), 8.15.3).
// These are the values the chip's waveform logic produces before its DAC; the read register 0x1B, the
// noise write-back and (Model A) the top-bit pull-down use them. S2b renders the band-limited analog form
// of the same shapes and takes the shapes from here, so both agree. JUCE-free, no state.

#include "bside/chip/ChipTypes.h"
#include "bside/chip/CombinedWaveforms.h"

#include <cassert>
#include <cstdint>

namespace vs::bside::chip::wave
{
/** The waveform bits of a control value (bits 7..4) as 0..15: 1 triangle, 2 saw, 4 pulse, 8 noise. */
constexpr std::uint8_t bits (std::uint8_t control) noexcept { return static_cast<std::uint8_t> (control >> 4); }
constexpr std::uint8_t kTri = 1, kSaw = 2, kPulse = 4, kNoise = 8;

/** More than one waveform bit selected. */
constexpr bool isCombined (std::uint8_t waveBits) noexcept { return (waveBits & (waveBits - 1)) != 0; }

/** The shape comes from a combined-waveform table (8.15.2 (c) step 2, as amended from the reference data):
    at least two of triangle, saw and pulse, or pulse together with noise. Noise with only triangle or only
    saw uses no table: the output is that shape AND the noise output. */
constexpr bool usesTable (std::uint8_t waveBits) noexcept
{
    const int tsp = ((waveBits & kTri) != 0 ? 1 : 0) + ((waveBits & kSaw) != 0 ? 1 : 0) + ((waveBits & kPulse) != 0 ? 1 : 0);
    return tsp >= 2 || ((waveBits & kPulse) != 0 && (waveBits & kNoise) != 0);
}

/** The waveform index: accumulator bits 23..12, with ring (ring set, saw clear) bit 11 := own bit 23 XOR
    NOT the ring source's bit 23 (8.15.2 (c) step 1). */
constexpr std::uint16_t index (std::uint32_t acc, std::uint8_t control, std::uint32_t ringSourceAcc) noexcept
{
    auto ix = static_cast<std::uint16_t> ((acc >> 12) & 0xfff);
    if ((control & ctrl::ring) != 0 && (control & ctrl::saw) == 0)
    {
        const std::uint32_t b = ((acc >> 23) ^ ~(ringSourceAcc >> 23)) & 1u;
        ix = static_cast<std::uint16_t> ((ix & 0x7ffu) | (b << 11));
    }
    return ix;
}

/** Triangle = (index < 0x800 ? index : index XOR 0xfff) << 1, 12 bits (bit 0 is always 0). */
constexpr std::uint16_t triangle (std::uint16_t ix) noexcept
{
    const std::uint16_t folded = (ix & 0x800u) != 0 ? static_cast<std::uint16_t> (ix ^ 0xfffu) : ix;
    return static_cast<std::uint16_t> ((static_cast<unsigned> (folded) << 1) & 0xfffu);
}

/** Saw = index. */
constexpr std::uint16_t saw (std::uint16_t ix) noexcept { return ix; }

/** Noise output: bits 11..4 = register bits 20, 18, 14, 11, 9, 5, 2, 0; bits 3..0 = 0 (8.15.3). */
constexpr std::uint16_t noise (std::uint32_t reg) noexcept
{
    return static_cast<std::uint16_t> ((((reg >> 20) & 1u) << 11) | (((reg >> 18) & 1u) << 10) | (((reg >> 14) & 1u) << 9)
                                       | (((reg >> 11) & 1u) << 8) | (((reg >> 9) & 1u) << 7) | (((reg >> 5) & 1u) << 6)
                                       | (((reg >> 2) & 1u) << 5) | ((reg & 1u) << 4));
}

/** The noise register's tap bits (the ones the output shows): 20, 18, 14, 11, 9, 5, 2, 0. */
constexpr std::uint32_t kTapBits = (1u << 20) | (1u << 18) | (1u << 14) | (1u << 11) | (1u << 9) | (1u << 5) | (1u << 2) | 1u;

/** Output bits 11..4 placed on the register's tap bits (the inverse of noise()). */
constexpr std::uint32_t taps (std::uint16_t out) noexcept
{
    return (((out >> 11) & 1u) << 20) | (((out >> 10) & 1u) << 18) | (((out >> 9) & 1u) << 14) | (((out >> 8) & 1u) << 11)
           | (((out >> 7) & 1u) << 9) | (((out >> 6) & 1u) << 5) | (((out >> 5) & 1u) << 2) | ((out >> 4) & 1u);
}

/** The register mask of the write-back: a set output bit keeps its tap bit, a clear one clears it. */
constexpr std::uint32_t noiseKeepMask (std::uint16_t out) noexcept { return (kNoiseMask & ~kTapBits) | taps (out); }

/** The shape of step 2 for waveform bits 1..15 (8.15.2 (c)): triangle (also with noise), saw (also with
    noise), 0xfff for pulse or noise alone, our combined-waveform shape otherwise (usesTable). The values
    come from the tables of CombinedWaveforms.h (one per chip model, computed from our fitted parameters);
    pulse with noise is indexed by the noise output (`noiseOutput`, used for that combination only). The
    read register 0x1B, the noise write-back and the Model A pull-down all follow them. */
inline std::uint16_t shape (const combined::ShapeTables& t, std::uint8_t waveBits, std::uint16_t ix, std::uint16_t noiseOutput) noexcept
{
    if ((waveBits & 15u) == combined::kPulseNoise)
    {
        assert ((noiseOutput & 0x00fu) == 0); // a noise output (bits 3..0 are always 0), not an index
        return t.pulseNoise[(noiseOutput >> 4) & 0xffu];
    }
    return t.wave[waveBits & 15u][ix & 0xfffu];
}

/** Noise together with another waveform bit: the output is written back into the noise register
    (8.15.2 (c) step 6). */
constexpr bool writesBack (std::uint8_t control) noexcept
{
    return (control & ctrl::noise) != 0 && (control & (ctrl::triangle | ctrl::saw | ctrl::pulse)) != 0;
}

/** Whether a noise shift with the waveform bits `oldBits` before and `newBits` after first replaces the
    latch's tap bits by the output (8.15.3; reference behaviour, followed). */
constexpr bool shiftWritesBack (Model model, std::uint8_t oldBits, std::uint8_t newBits) noexcept
{
    if (oldBits <= 8 || newBits < 8)
        return false;
    if (newBits == 8 && oldBits != 0xf)
        return false;
    if (model == Model::A)
    {
        const int o = oldBits & 3, n = newBits & 3;
        if ((o == 1 && n == 2) || (o == 2 && n == 1))
            return false;
    }
    return oldBits != 0xc && newBits != 0xc;
}
} // namespace vs::bside::chip::wave
