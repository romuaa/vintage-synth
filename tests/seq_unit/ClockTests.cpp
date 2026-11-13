// Shared clock of the arpeggiator and sequencer: grid positions, sample mapping, host/internal
// source handling and discontinuity detection. Expectations are computed with integer ticks
// (1/kTick quarter note) in SeqTestUtil.h, not with the module's own arithmetic.
#include <catch2/catch_test_macros.hpp>

#include "SeqTestUtil.h"
#include "common/seq/Clock.h"

#include <cmath>

using namespace vs::seq;
using namespace vs::seq::test;

namespace
{
constexpr Rate kRates[] = { Rate::Quarter, Rate::Eighth, Rate::EighthTriplet, Rate::Sixteenth, Rate::SixteenthTriplet, Rate::ThirtySecond };
constexpr int kStepsPerQuarter[] = { 1, 2, 3, 4, 6, 8 };

Transport host (double sr, double bpm, double ppq, bool playing = true) { return Transport { sr, bpm, ppq, playing }; }
} // namespace

TEST_CASE ("Rate: steps per quarter note for every rate", "[seq][clock]")
{
    for (std::size_t i = 0; i < 6; ++i)
        CHECK (stepsPerQuarter (kRates[i]) == kStepsPerQuarter[i]);
}

TEST_CASE ("Grid: step positions equal the exact tick model for every rate and swing", "[seq][clock]")
{
    for (int r = 0; r < 6; ++r)
        for (int m = 40; m <= 60; m += 2)
        {
            const Grid g { kStepsPerQuarter[r], static_cast<double> (m) / 80.0 };
            MCfg c;
            c.d = kStepsPerQuarter[r];
            c.swing80 = m;
            for (std::int64_t k = -9; k < 200; ++k)
            {
                INFO ("rate " << r << " swing " << m << " k " << k);
                CHECK (std::fabs (g.pos (k) * static_cast<double> (kTick) - static_cast<double> (c.pos (k))) < 1.0e-2);
            }
        }
}

TEST_CASE ("Grid: swing delays every second step by (swing - 50 %) of a step pair", "[seq][clock]")
{
    const Grid straight { 4, 0.5 };
    const Grid swung { 4, 0.75 };
    for (std::int64_t j = -3; j < 20; ++j)
    {
        // Even steps stay, odd steps move from the middle of the pair to 75 % of the pair.
        CHECK (std::fabs (swung.pos (2 * j) - straight.pos (2 * j)) < 1.0e-12);
        CHECK (std::fabs (straight.pos (2 * j + 1) - (straight.pos (2 * j) + 0.25)) < 1.0e-12);
        CHECK (std::fabs (swung.pos (2 * j + 1) - (swung.pos (2 * j) + 0.375)) < 1.0e-12); // 0.75 of a 0.5 pair
    }
    // Strictly increasing even at the maximum swing.
    for (std::int64_t k = -20; k < 20; ++k)
        CHECK (swung.pos (k + 1) > swung.pos (k));
}

TEST_CASE ("ceilSamples: first sample at or after a position, with a tolerance of 1e-6 samples", "[seq][clock]")
{
    using vs::seq::detail::ceilSamples;
    CHECK (ceilSamples (0.0) == 0);
    CHECK (ceilSamples (12000.0) == 12000);
    CHECK (ceilSamples (12000.0 + 1.0e-8) == 12000);  // float noise above an integer
    CHECK (ceilSamples (12000.0 - 1.0e-8) == 12000);
    CHECK (ceilSamples (12000.001) == 12001);          // a real fraction rounds up
    CHECK (ceilSamples (12000.5) == 12001);
    CHECK (ceilSamples (-0.5) == 0);
    CHECK (ceilSamples (-1.0) == -1);
    CHECK (ceilSamples (-1.5) == -1);
    CHECK (ceilSamples (std::nan ("")) == 1000000000000000LL);    // "never", not 0
    CHECK (ceilSamples (HUGE_VAL) == 1000000000000000LL);
    CHECK (ceilSamples (-HUGE_VAL) == -1000000000000000LL);
    CHECK (ceilSamples (1.0e300) <= 1000000000000000LL);
}

TEST_CASE ("Clock: host view maps ppq to samples with the block's own position and tempo", "[seq][clock]")
{
    Clock clock;
    const auto b = clock.begin (host (48000.0, 120.0, 3.5), 512, 90.0);
    CHECK (b.view.hostSynced);
    CHECK (b.jumped); // the first block always resynchronises
    CHECK_FALSE (b.sourceChanged);
    // 120 bpm at 48 kHz: 24000 samples per quarter note.
    CHECK (std::fabs (b.view.spp - 24000.0) < 1.0e-9);
    CHECK (b.view.sampleOf (3.5) == 0);
    CHECK (b.view.sampleOf (3.75) == 6000);
    CHECK (b.view.sampleOf (3.75 + 1.0e-12) == 6000);
    CHECK (b.view.sampleOf (3.75 + 1.0e-7) == 6001); // 0.0024 samples later: next sample
    CHECK (b.view.sampleOf (3.4) == -2400);
    CHECK (std::fabs (b.view.ppqAt (6000) - 3.75) < 1.0e-12);
}

TEST_CASE ("Clock: continuous host blocks are not jumps, at any block size and across tempo changes", "[seq][clock]")
{
    for (const int block : { 1, 7, 64, 480, 4096 })
    {
        Clock clock;
        double ppq = 10.37;
        double bpm = 97.5;
        std::int64_t pos = 0;
        int jumps = 0;
        for (int i = 0; i < 4000; ++i)
        {
            const auto b = clock.begin (host (44100.0, bpm, ppq), block, 120.0);
            if (b.jumped)
                ++jumps;
            clock.end (block);
            ppq += static_cast<double> (block) * bpm / (60.0 * 44100.0);
            pos += block;
            if (pos % 9999 < block) // a tempo step at a block boundary: continuous in ppq
                bpm = bpm > 100.0 ? 133.37 : 101.0;
        }
        INFO ("block " << block);
        CHECK (jumps == 1); // only the first block
    }
}

TEST_CASE ("Clock: loop jumps, locates and transport start/stop are detected", "[seq][clock]")
{
    Clock clock;
    const double sr = 48000.0;
    const int n = 480;
    double ppq = 4.0;
    clock.begin (host (sr, 120.0, ppq), n, 120.0);
    clock.end (n);
    ppq += 480.0 / 24000.0;

    SECTION ("continuous")
    {
        const auto b = clock.begin (host (sr, 120.0, ppq), n, 120.0);
        CHECK_FALSE (b.jumped);
    }
    SECTION ("jitter well under the tolerance is not a jump")
    {
        const auto b = clock.begin (host (sr, 120.0, ppq + 0.01), n, 120.0);
        CHECK_FALSE (b.jumped);
    }
    SECTION ("jitter backwards")
    {
        const auto b = clock.begin (host (sr, 120.0, ppq - 0.01), n, 120.0);
        CHECK_FALSE (b.jumped);
    }
    SECTION ("backward jump (loop)")
    {
        const auto b = clock.begin (host (sr, 120.0, 0.0), n, 120.0);
        CHECK (b.jumped);
        CHECK_FALSE (b.sourceChanged);
    }
    SECTION ("forward jump (locate)")
    {
        const auto b = clock.begin (host (sr, 120.0, ppq + 8.0), n, 120.0);
        CHECK (b.jumped);
    }
    SECTION ("a jump just above the tolerance")
    {
        const auto b = clock.begin (host (sr, 120.0, ppq + driftLimitPpq (0.25, 480.0 / 24000.0) * 1.5), n, 120.0);
        CHECK (b.jumped);
    }
    SECTION ("transport stops, then starts")
    {
        const auto stop = clock.begin (host (sr, 120.0, ppq, false), n, 100.0);
        CHECK (stop.sourceChanged);
        CHECK (stop.jumped);
        CHECK_FALSE (stop.view.hostSynced);
        clock.end (n);
        const auto stop2 = clock.begin (host (sr, 120.0, ppq, false), n, 100.0);
        CHECK_FALSE (stop2.sourceChanged);
        CHECK_FALSE (stop2.jumped);
        clock.end (n);
        const auto start = clock.begin (host (sr, 120.0, ppq, true), n, 100.0);
        CHECK (start.sourceChanged);
        CHECK (start.jumped);
        CHECK (start.view.hostSynced);
    }
}

TEST_CASE ("Clock: unusable host data falls back to the internal clock", "[seq][clock]")
{
    for (const Transport& tr : { host (48000.0, 0.0, 1.0), host (48000.0, -120.0, 1.0), host (48000.0, std::nan (""), 1.0),
                                 host (48000.0, 120.0, std::nan ("")), host (48000.0, 120.0, 1.0e12) })
    {
        Clock clock;
        const auto b = clock.begin (tr, 64, 120.0);
        CHECK_FALSE (b.view.hostSynced);
        CHECK (std::isfinite (b.view.spp));
        CHECK (b.view.spp > 0.0);
    }
    Clock clock;
    const auto b = clock.begin (host (0.0, 120.0, 0.0, false), 64, 120.0); // bad sample rate
    CHECK (std::isfinite (b.view.spp));
    CHECK (b.view.spp > 0.0);
}

TEST_CASE ("Clock: the internal clock is anchored at the start sample and independent of block size", "[seq][clock]")
{
    // 100 bpm at 44.1 kHz: 26460 samples per quarter note. Anchor (ppq 0) at absolute sample 1000.
    constexpr std::int64_t anchor = 1000;
    for (const int block : { 1, 33, 480, 1000, 5000 })
    {
        Clock clock;
        std::int64_t s = 0;
        bool started = false;
        double maxErr = 0.0;
        while (s < 200000)
        {
            auto b = clock.begin (host (44100.0, 120.0, 0.0, false), block, 100.0);
            if (! started && s + block > anchor)
            {
                clock.startInternal (b.view, static_cast<int> (anchor - s));
                started = true;
            }
            if (started)
            {
                const double expected = static_cast<double> (s - anchor) / 26460.0;
                maxErr = std::max (maxErr, std::fabs (b.view.ppq0 - expected));
            }
            clock.end (block);
            s += block;
        }
        INFO ("block " << block);
        CHECK (maxErr < 1.0e-12);
    }
}

TEST_CASE ("Clock: an internal tempo change continues from the current position", "[seq][clock]")
{
    Clock clock;
    auto b = clock.begin (host (48000.0, 120.0, 0.0, false), 100, 120.0);
    clock.startInternal (b.view, 0);
    clock.end (100);
    // 24000 samples later (one quarter at 120 bpm), the tempo drops to 60 bpm.
    for (int i = 0; i < 240; ++i)
    {
        b = clock.begin (host (48000.0, 0.0, 0.0, false), 100, 120.0);
        clock.end (100);
    }
    b = clock.begin (host (48000.0, 0.0, 0.0, false), 100, 60.0);
    CHECK (std::fabs (b.view.ppq0 - 1.0 - 100.0 / 24000.0) < 1.0e-12); // 24100 samples = 1 + 100/24000 quarters
    clock.end (100);
    b = clock.begin (host (48000.0, 0.0, 0.0, false), 100, 60.0);
    CHECK (std::fabs (b.view.ppq0 - (1.0 + 100.0 / 24000.0 + 100.0 / 48000.0)) < 1.0e-12);
}

TEST_CASE ("Clock: stopping the internal clock makes the next start a fresh anchor", "[seq][clock]")
{
    Clock clock;
    auto b = clock.begin (host (48000.0, 0.0, 0.0, false), 100, 120.0);
    clock.startInternal (b.view, 10);
    CHECK (clock.internalRunning());
    CHECK (std::fabs (b.view.ppq0 - (-10.0 / 24000.0)) < 1.0e-15);
    clock.end (100);
    clock.stopInternal();
    CHECK_FALSE (clock.internalRunning());
    b = clock.begin (host (48000.0, 0.0, 0.0, false), 100, 120.0);
    clock.startInternal (b.view, 0);
    CHECK (std::fabs (b.view.ppq0) < 1.0e-15);
}
