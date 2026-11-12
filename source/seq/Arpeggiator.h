#pragma once

// Arpeggiator note selection: which note the next step plays. Pure state machine (position
// counter and a seeded random generator); timing, gate and note-offs belong to the front-end.
//
// Changing the pattern while it plays (a key added or released, another pattern or octave range)
// continues from the pitch that was played last, in the direction of travel, instead of from a
// bare step counter: C E G with E just played and B added goes on with G, not E again.

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
        haveLast_ = false;
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
        std::array<std::uint8_t, kListSize> note {};
        std::array<std::uint8_t, kListSize> key {};
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

        if (haveLast_ && (pattern != lastPattern_ || m != lastM_ || ! sameList (note, m)))
            continueFromLast (note, m, pattern);

        const std::int64_t mm = m;
        const std::int64_t p = position_++;
        std::int64_t i = 0;
        ascending_ = true;
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
                    ascending_ = q < mm;
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

        lastNote_ = a.note;
        lastPattern_ = pattern;
        lastM_ = m;
        prevNote_ = note;
        haveLast_ = true;
        return a;
    }

private:
    static constexpr std::size_t kListSize = 128 * kMaxOctaves;

    bool sameList (const std::array<std::uint8_t, kListSize>& note, int m) const noexcept
    {
        for (int i = 0; i < m; ++i)
            if (note[static_cast<std::size_t> (i)] != prevNote_[static_cast<std::size_t> (i)])
                return false;
        return true;
    }

    /** The list or mode changed: choose the position so that the next note is the one that follows
        the last played pitch in the direction of travel. (Entries are ordered by pitch within an
        octave repeat; for chords wider than an octave the order is that of the list.) */
    void continueFromLast (const std::array<std::uint8_t, kListSize>& note, int m, ArpPattern pattern) noexcept
    {
        std::int64_t atOrBelow = 0, below = 0;
        for (int i = 0; i < m; ++i)
        {
            const int v = note[static_cast<std::size_t> (i)];
            atOrBelow += v <= lastNote_ ? 1 : 0;
            below += v < lastNote_ ? 1 : 0;
        }
        const std::int64_t mm = m;
        switch (pattern)
        {
            case ArpPattern::Up:
                position_ = atOrBelow % mm;
                break;
            case ArpPattern::Down:
            {
                const std::int64_t next = below - 1 < 0 ? mm - 1 : below - 1;
                position_ = mm - 1 - next;
                break;
            }
            case ArpPattern::UpDown:
            {
                if (mm == 1)
                {
                    position_ = 0;
                    break;
                }
                const std::int64_t period = 2 * mm - 2;
                if (ascending_ || lastPattern_ != ArpPattern::UpDown)
                {
                    std::int64_t next = atOrBelow;
                    if (next >= mm)
                        position_ = period - (mm - 2); // turn round: the next note is the one below the top
                    else
                        position_ = next;
                }
                else
                {
                    const std::int64_t next = below - 1;
                    if (next < 0)
                        position_ = 1; // turn round at the bottom
                    else
                        position_ = period - next;
                }
                break;
            }
            case ArpPattern::AsPlayed:
            {
                for (int i = 0; i < m; ++i)
                    if (note[static_cast<std::size_t> (i)] == lastNote_)
                    {
                        position_ = (i + 1) % mm;
                        return;
                    }
                position_ %= mm;
                break;
            }
            case ArpPattern::Random:
                break;
        }
    }

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
    bool haveLast_ = false;
    bool ascending_ = true; // Up & Down: the last note was played on the way up
    int lastNote_ = 0;
    int lastM_ = 0;
    ArpPattern lastPattern_ = ArpPattern::Up;
    std::array<std::uint8_t, kListSize> prevNote_ {};
};
} // namespace vs::seq
