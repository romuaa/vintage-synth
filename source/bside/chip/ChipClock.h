#pragma once

// Chip time per internal sample (docs/architecture.md 8.4.2): a 32-bit fraction of a cycle carried
// from sample to sample; each internal sample advances the position by round (clock x 2^32 / internal
// rate), computed once at prepare. No floating point at run time, so the cycle at which any sample
// starts depends only on the sample's index, never on how the samples were split into blocks.
// The whole cycles live in the chip (Chip::cycle()); this class holds the fraction. JUCE-free.

#include <cmath>
#include <cstdint>

namespace vs::bside::chip
{
class ChipClock
{
public:
    /** Cycles per internal sample in 32.32 fixed point. Rates outside 1 kHz .. 10 MHz and clocks outside
        1 kHz .. 100 MHz are clamped (they cannot come from the plugin; the clamp keeps the value finite). */
    static std::uint64_t incrementFor (std::uint32_t clockHz, double internalRate) noexcept
    {
        const double rate = std::isfinite (internalRate) ? std::fmin (std::fmax (internalRate, 1000.0), 1.0e7) : 96000.0;
        const double hz = std::fmin (std::fmax (static_cast<double> (clockHz), 1000.0), 1.0e8);
        return static_cast<std::uint64_t> (std::llround (hz * 4294967296.0 / rate));
    }

    void prepare (std::uint32_t clockHz, double internalRate) noexcept
    {
        inc = incrementFor (clockHz, internalRate);
        frac = 0;
    }
    void reset() noexcept { frac = 0; }

    /** Moves to the start of the next internal sample; returns the whole cycles crossed. */
    std::uint64_t nextSample() noexcept
    {
        const std::uint64_t t = static_cast<std::uint64_t> (frac) + inc;
        frac = static_cast<std::uint32_t> (t & 0xffffffffu);
        return t >> 32;
    }

    /** Moves n samples on at once; returns the whole cycles crossed: exactly the sum of n nextSample () calls (the
        fraction carries the same way), in chunks small enough that k x increment stays inside 64 bits for every
        clock and rate prepare accepts (increment < 2^49). */
    std::uint64_t nextSamples (std::uint64_t n) noexcept
    {
        std::uint64_t total = 0;
        while (n > 0)
        {
            const std::uint64_t k = n < kChunk ? n : kChunk;
            const std::uint64_t t = static_cast<std::uint64_t> (frac) + k * inc;
            frac = static_cast<std::uint32_t> (t & 0xffffffffu);
            total += t >> 32;
            n -= k;
        }
        return total;
    }

    /** Sub-sample position of the instant t = eventCycle - before in the sample that starts at chip time
        sampleStartCycle + sampleStartFraction / 2^32 and lasts increment / 2^32 cycles:
        (t - start) x 2^32 / increment. 0 is the sample's start, 1 the next sample's start. For a step
        ending inside the sample (before = 0) the result is in (0, 1] (Chip.h, "SAMPLES"). */
    static double position (std::uint64_t sampleStartCycle, std::uint32_t sampleStartFraction, std::uint64_t increment, std::uint64_t eventCycle,
                            double before = 0.0) noexcept
    {
        const double offsetCycles = static_cast<double> (static_cast<std::int64_t> (eventCycle - sampleStartCycle)) - before
                                    - static_cast<double> (sampleStartFraction) / 4294967296.0;
        return offsetCycles * 4294967296.0 / static_cast<double> (increment);
    }

    /** Back to a sample start taken before (its fraction()), undoing nextSample (the renderer's batched path
        looks one sample ahead). */
    void rewind (std::uint32_t startFraction) noexcept { frac = startFraction; }

    /** The fraction of a cycle at the current sample start (0 .. 2^32 - 1). Read it before
        Chip::takeSampleCycles() to get the start of the sample being taken. */
    std::uint32_t fraction() const noexcept { return frac; }
    std::uint64_t increment() const noexcept { return inc; }

private:
    static constexpr std::uint64_t kChunk = std::uint64_t { 1 } << 14;
    std::uint64_t inc = 0;
    std::uint32_t frac = 0;
};
} // namespace vs::bside::chip
