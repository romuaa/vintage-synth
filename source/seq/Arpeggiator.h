#pragma once

// Arpeggiator note selection: which note the next step plays. Pure state machine (position
// counter and a seeded random generator); timing, gate and note-offs belong to the front-end.

#include "KeyState.h"
#include "Settings.h"

#include <array>
#include <cstdint>

namespace vs::seq
{
class Arpeggiator
{
public:
    /** A new pattern starts: position 0 and the random generator back at its seed. */
    void restart (std::uint32_t seed) noexcept
    {
        position_ = 0;
        reseed (seed);
    }

    void reseed (std::uint32_t seed) noexcept { rng_ = 0x9E3779B97F4A7C15ull + seed; }

    /** The action for the next step from the current pattern (keys, mode, octave range). */
    StepAction step (const KeyState& keys, ArpPattern pattern, int octaves) noexcept
    {
        StepAction a;
        std::array<std::uint8_t, 128> base {};
        int n = 0;
        if (pattern == ArpPattern::AsPlayed)
        {
            for (int i = 0; i < keys.size(); ++i)
                base[static_cast<std::size_t> (n++)] = static_cast<std::uint8_t> (keys.ordered (i));
        }
        else
        {
            for (int k = 0; k < 128; ++k)
                if (keys.contains (k))
                    base[static_cast<std::size_t> (n++)] = static_cast<std::uint8_t> (k);
        }
        if (n == 0)
            return a;

        // The pattern repeated `octaves` times, each repeat transposed up by 12 (notes above 127 dropped).
        std::array<std::uint8_t, 128 * kMaxOctaves> note {};
        std::array<std::uint8_t, 128 * kMaxOctaves> key {};
        int m = 0;
        for (int o = 0; o < octaves; ++o)
            for (int j = 0; j < n; ++j)
            {
                const int v = base[static_cast<std::size_t> (j)] + 12 * o;
                if (v <= 127)
                {
                    note[static_cast<std::size_t> (m)] = static_cast<std::uint8_t> (v);
                    key[static_cast<std::size_t> (m)] = base[static_cast<std::size_t> (j)];
                    ++m;
                }
            }
        if (m == 0)
            return a;

        const std::int64_t p = position_++;
        const std::int64_t mm = m;
        std::int64_t i = 0;
        switch (pattern)
        {
            case ArpPattern::Up:
            case ArpPattern::AsPlayed:
                i = p % mm;
                break;
            case ArpPattern::Down:
                i = mm - 1 - p % mm;
                break;
            case ArpPattern::UpDown:
            {
                if (mm > 1)
                {
                    const std::int64_t period = 2 * mm - 2; // 0 1 .. m-1 m-2 .. 1
                    const std::int64_t q = p % period;
                    i = q < mm ? q : period - q;
                }
                break;
            }
            case ArpPattern::Random:
                i = static_cast<std::int64_t> ((static_cast<std::uint64_t> (nextRandom()) * static_cast<std::uint64_t> (mm)) >> 32);
                break;
        }
        const auto iu = static_cast<std::size_t> (i);
        a.kind = StepAction::Kind::Note;
        a.note = note[iu];
        a.velocity = keys.velocity (key[iu]);
        return a;
    }

private:
    /** splitmix64; the high 32 bits are the output. */
    std::uint32_t nextRandom() noexcept
    {
        rng_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = rng_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<std::uint32_t> (z >> 32);
    }

    std::int64_t position_ = 0;
    std::uint64_t rng_ = 0x9E3779B97F4A7C15ull;
};
} // namespace vs::seq
