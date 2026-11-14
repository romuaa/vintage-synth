// The chip's fast multi-cycle path against its own per-cycle definition (docs/architecture.md 8.4.2:
// "closed form, not per cycle", proven equal), the event seam for S2b, and the independence of the
// digital state from the block size and the internal rate (8.9.1 "Robustness").
//
// The long run is hidden: `bside_unit "[.chip-long]"` repeats the equivalence for 10^8 cycles per
// model (BS_CHIP_CYCLES overrides the count).

#include "FuzzSupport.h"

#include "bside/chip/Chip.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace vs::bside::chip;

namespace
{
struct Event
{
    int kind = 0; // 0 sync reset, 1 noise clock
    int osc = 0;
    std::uint64_t cycle = 0;
    std::uint32_t acc = 0;
    bool operator== (const Event&) const = default;
};

struct RecordingSink
{
    static constexpr bool kWantsNoiseClocks = true;
    std::vector<Event> events;
    void syncReset (int osc, std::uint64_t cycle, std::uint32_t accBefore) { events.push_back ({ 0, osc, cycle, accBefore }); }
    void noiseClock (int osc, std::uint64_t cycle) { events.push_back ({ 1, osc, cycle, 0 }); }
};

std::uint8_t randomControl (bst::Rng& r, std::uint8_t old)
{
    static const std::uint8_t waves[] = { 0x00, 0x10, 0x20, 0x40, 0x80, 0x30, 0x50, 0x60, 0x70, 0xa0, 0xc0, 0x90, 0xf0, 0x20, 0x40, 0x10 };
    std::uint8_t c = waves[r.below (16)];
    const bool gate = r.chance (50) ? (old & ctrl::gate) == 0 : (old & ctrl::gate) != 0;
    if (gate)
        c |= ctrl::gate;
    if (r.chance (30))
        c |= ctrl::sync;
    if (r.chance (30))
        c |= ctrl::ring;
    if (r.chance (4))
        c |= ctrl::test;
    return c;
}

std::uint16_t randomFrequency (bst::Rng& r)
{
    switch (r.below (8))
    {
        case 0: return 0;
        case 1: return 0xffff;
        case 2: return static_cast<std::uint16_t> (1 + r.below (300));
        case 3: return static_cast<std::uint16_t> (1u << r.below (16));
        case 4: return static_cast<std::uint16_t> ((1u << r.below (16)) - 1u + r.below (3));
        default: return static_cast<std::uint16_t> (r.below (65536));
    }
}

/** One random input, applied identically to every chip given. */
void randomInput (bst::Rng& r, std::vector<Chip*> chips, bool smooth)
{
    const int n = static_cast<int> (r.below (3));
    const auto base = static_cast<std::uint8_t> (n * reg::oscStride);
    const auto k = r.below (100);
    auto writeAll = [&] (std::uint8_t a, std::uint8_t v) {
        for (auto* c : chips)
            c->write (a, v);
    };
    if (k < 26)
        writeAll (base + reg::control, randomControl (r, chips[0]->control (n)));
    else if (k < 44)
    {
        const auto f = randomFrequency (r);
        if (r.chance (50))
            writeAll (base + reg::freqLo, static_cast<std::uint8_t> (f & 0xff));
        if (r.chance (70))
            writeAll (base + reg::freqHi, static_cast<std::uint8_t> (f >> 8));
    }
    else if (k < 52)
        writeAll (static_cast<std::uint8_t> (base + (r.chance (50) ? reg::pwLo : reg::pwHi)), static_cast<std::uint8_t> (r.below (256)));
    else if (k < 72)
        writeAll (static_cast<std::uint8_t> (base + (r.chance (50) ? reg::attackDecay : reg::sustainRelease)), static_cast<std::uint8_t> (r.below (256)));
    else if (k < 80)
        writeAll (static_cast<std::uint8_t> (0x15 + r.below (4)), static_cast<std::uint8_t> (r.below (256)));
    else if (k < 88)
        writeAll (static_cast<std::uint8_t> (r.below (64)), static_cast<std::uint8_t> (r.below (256)));
    else
    {
        OscInput in;
        in.frequency = smooth ? static_cast<double> (r.next() % 300000000000000ull) / 1.0e9 + (r.chance (10) ? 6.0e5 : 0.0)
                              : static_cast<double> (r.below (70000)) + 0.25 * r.below (4);
        in.pulseWidth = static_cast<float> (r.below (5000)) * 0.875f;
        in.control = randomControl (r, chips[0]->control (n));
        in.attackDecay = static_cast<std::uint8_t> (r.below (256));
        in.sustainRelease = static_cast<std::uint8_t> (r.below (256));
        for (auto* c : chips)
            c->setOscillator (n, in);
        if (r.chance (30))
        {
            const FilterInput fi { static_cast<float> (r.below (2100)) * 0.99f, static_cast<std::uint8_t> (r.below (16)),
                                   static_cast<std::uint8_t> (r.below (16)), static_cast<std::uint8_t> (r.below (256)) };
            for (auto* c : chips)
                c->setFilter (fi);
        }
    }
}

std::uint64_t randomGap (bst::Rng& r)
{
    const auto k = r.below (100);
    if (k < 45)
        return r.below (13);
    if (k < 75)
        return r.below (601);
    if (k < 95)
        return r.below (40001);
    return r.below (400001);
}

std::string describe (const DigitalState& a, const DigitalState& b)
{
    std::string s = "cycle " + std::to_string (a.cycle) + " vs " + std::to_string (b.cycle);
    for (std::size_t n = 0; n < 3; ++n)
    {
        s += "\n osc" + std::to_string (n + 1) + ": acc " + std::to_string (a.osc[n].acc) + "/" + std::to_string (b.osc[n].acc) + " frac "
             + std::to_string (a.osc[n].accFraction) + "/" + std::to_string (b.osc[n].accFraction) + " noise " + std::to_string (a.osc[n].noise) + "/"
             + std::to_string (b.osc[n].noise);
        s += "\n env" + std::to_string (n + 1) + ": cnt " + std::to_string (a.env[n].counter) + "/" + std::to_string (b.env[n].counter) + " ph "
             + std::to_string (a.env[n].phase) + "/" + std::to_string (b.env[n].phase) + " rate " + std::to_string (a.env[n].rateCounter) + "/"
             + std::to_string (b.env[n].rateCounter) + " div " + std::to_string (a.env[n].divider) + "/" + std::to_string (b.env[n].divider) + " divp "
             + std::to_string (a.env[n].dividerPeriod) + "/" + std::to_string (b.env[n].dividerPeriod);
    }
    return s;
}

/** Random writes at random gaps; `fast` advances with advanceCycles, `ref` with stepCycle. Returns the
    number of cycles compared; stops at the first difference (REQUIRE). */
std::uint64_t runEquivalence (Model model, bool smooth, std::uint64_t cycles, std::uint64_t seed, bool withEvents)
{
    Config cfg;
    cfg.model = model;
    cfg.authentic = ! smooth;
    Chip fast, ref;
    fast.prepare (cfg);
    ref.prepare (cfg);
    bst::Rng r (seed);
    RecordingSink fastEvents, refEvents;
    std::uint64_t done = 0, writes = 0;
    while (done < cycles)
    {
        const std::uint64_t gap = randomGap (r);
        if (withEvents)
        {
            fast.advanceCycles (gap, fastEvents);
            for (std::uint64_t i = 0; i < gap; ++i)
                ref.stepCycle (refEvents);
            // The sink contract: per oscillator in cycle order; across oscillators a closed-form jump
            // reports oscillator by oscillator. Ordered by cycle (stable), the lists must be identical.
            std::uint64_t lastPerOsc[3] = { 0, 0, 0 };
            for (const auto& ev : fastEvents.events)
            {
                REQUIRE (ev.cycle >= lastPerOsc[ev.osc]);
                lastPerOsc[ev.osc] = ev.cycle;
            }
            std::stable_sort (fastEvents.events.begin(), fastEvents.events.end(), [] (const Event& x, const Event& y) { return x.cycle < y.cycle; });
            if (fastEvents.events != refEvents.events)
            {
                std::size_t i = 0;
                while (i < fastEvents.events.size() && i < refEvents.events.size() && fastEvents.events[i] == refEvents.events[i])
                    ++i;
                const auto show = [] (const std::vector<Event>& v, std::size_t k) {
                    return k < v.size() ? std::to_string (v[k].kind) + " osc " + std::to_string (v[k].osc) + " cycle " + std::to_string (v[k].cycle)
                                              + " acc " + std::to_string (v[k].acc)
                                        : std::string ("(none)");
                };
                INFO ("seed " << seed << ": event " << i << " of " << fastEvents.events.size() << "/" << refEvents.events.size() << ": fast "
                              << show (fastEvents.events, i) << ", stepped " << show (refEvents.events, i));
                REQUIRE (fastEvents.events.size() == refEvents.events.size());
                REQUIRE (false);
            }
            fastEvents.events.clear();
            refEvents.events.clear();
        }
        else
        {
            fast.advanceCycles (gap);
            for (std::uint64_t i = 0; i < gap; ++i)
                ref.stepCycle();
        }
        done += gap;
        {
            const auto a = fast.state(), b = ref.state();
            INFO ("seed " << seed << " after " << writes << " writes: " << describe (a, b));
            REQUIRE (a == b);
        }
        randomInput (r, { &fast, &ref }, smooth);
        ++writes;
        REQUIRE (fast.state() == ref.state());
    }
    return done;
}

std::uint64_t longCycles()
{
    if (const char* e = std::getenv ("BS_CHIP_CYCLES"))
        return std::strtoull (e, nullptr, 10);
    return 100000000ull;
}
} // namespace

TEST_CASE ("chip: the fast path equals per-cycle stepping, 10^7 random cycles with random writes per model", "[bside][chip][equivalence]")
{
    CHECK (runEquivalence (Model::A, false, 10000000, 1, false) >= 10000000);
    CHECK (runEquivalence (Model::B, false, 10000000, 2, false) >= 10000000);
}

TEST_CASE ("chip: the fast path equals per-cycle stepping with Smooth inputs (fractions, large frequencies)", "[bside][chip][equivalence]")
{
    CHECK (runEquivalence (Model::A, true, 3000000, 3, false) >= 3000000);
    CHECK (runEquivalence (Model::B, true, 3000000, 4, false) >= 3000000);
}

TEST_CASE ("chip: the fast path reports the same sync resets and noise clocks at the same cycles", "[bside][chip][equivalence]")
{
    CHECK (runEquivalence (Model::B, false, 2000000, 5, true) >= 2000000);
    CHECK (runEquivalence (Model::A, true, 1000000, 6, true) >= 1000000);
}

TEST_CASE ("chip: long equivalence run", "[.chip-long]")
{
    const auto n = longCycles();
    CHECK (runEquivalence (Model::A, false, n, 101, false) >= n);
    CHECK (runEquivalence (Model::B, false, n, 102, false) >= n);
    CHECK (runEquivalence (Model::A, true, n / 4, 103, true) >= n / 4);
    CHECK (runEquivalence (Model::B, true, n / 4, 104, true) >= n / 4);
}

TEST_CASE ("chip: one long span in sustain equals many short spans", "[bside][chip][equivalence]")
{
    // A held note in sustain with a running oscillator, no sync: the frozen envelope and the oscillator
    // are jumped in closed form (in jumps of at most kMaxSkip), here over 50 million cycles at once.
    Chip c;
    Config cfg;
    cfg.authentic = true;
    c.prepare (cfg);
    c.write (0x00, 0x34);
    c.write (0x01, 0x12);
    c.write (0x05, 0x00);
    c.write (0x06, 0xa0);
    c.write (0x04, ctrl::saw | ctrl::gate);
    c.advanceCycles (100000);
    REQUIRE (c.envelope (0) == 0xaa);
    Chip d = c;
    c.advanceCycles (50000000);
    for (int i = 0; i < 50000000 / 10; ++i)
        d.advanceCycles (10);
    CHECK (c.state() == d.state());
}

TEST_CASE ("chip 8.4.2: the cycle of a sample start depends only on the sample index", "[bside][chip][time]")
{
    for (const double rate : { 44100.0 * 4, 96000.0, 192000.0, 211200.0, 384000.0 })
        for (const std::uint32_t clockHz : { 985248u, 1022727u })
        {
            Chip c;
            Config cfg;
            cfg.clockHz = clockHz;
            cfg.internalRate = rate;
            c.prepare (cfg);
            const std::uint64_t inc = ChipClock::incrementFor (clockHz, rate);
            CHECK (inc == static_cast<std::uint64_t> (std::llround (clockHz * 4294967296.0 / rate)));
            bst::Rng r (clockHz);
            std::uint64_t samples = 0;
            while (samples < 2000000)
            {
                static const int sizes[] = { 1, 7, 61, 512 };
                const int n = sizes[r.below (4)];
                c.advanceSamples (n);
                samples += static_cast<std::uint64_t> (n);
                REQUIRE (c.cycle() == (samples * inc) >> 32);
            }
        }
}

TEST_CASE ("chip: the digital state does not depend on the block size or the internal rate", "[bside][chip][time]")
{
    // A schedule of writes at cycles. Every chip runs it sample by sample at its own internal rate and
    // block size, splitting a sample at a write's cycle (what the renderer does); the state right after
    // each write and at the end must be identical everywhere and equal to a chip run by cycles only.
    struct W
    {
        std::uint64_t cycle;
        std::uint8_t reg, value;
    };
    std::vector<W> schedule;
    {
        bst::Rng r (77);
        Chip shadow; // only to generate plausible control values
        std::uint64_t at = 0;
        for (int i = 0; i < 3000; ++i)
        {
            at += randomGap (r) / 4;
            const auto a = static_cast<std::uint8_t> (r.below (0x19));
            const auto v = (a % 7 == 4) ? randomControl (r, shadow.control (a / 7)) : static_cast<std::uint8_t> (r.below (256));
            shadow.write (a, v);
            schedule.push_back ({ at, a, v });
        }
    }
    const std::uint64_t endCycle = schedule.back().cycle + 100000;

    for (auto model : { Model::A, Model::B })
    {
        // Reference: cycles only.
        Chip byCycles;
        Config cfg;
        cfg.model = model;
        cfg.authentic = true;
        byCycles.prepare (cfg);
        std::vector<DigitalState> expected;
        for (const auto& w : schedule)
        {
            byCycles.advanceCycles (w.cycle - byCycles.cycle());
            byCycles.write (w.reg, w.value);
            expected.push_back (byCycles.state());
        }
        byCycles.advanceCycles (endCycle - byCycles.cycle());
        expected.push_back (byCycles.state());

        for (const double rate : { 96000.0, 88200.0 * 2, 211200.0, 384000.0 })
            for (const int block : { 1, 7, 61, 512 })
            {
                cfg.internalRate = rate;
                Chip c;
                c.prepare (cfg);
                std::size_t wi = 0, got = 0;
                bool ok = true;
                while (c.cycle() < endCycle && ok)
                {
                    // One block of `block` samples; the block boundary itself is invisible to the chip.
                    for (int s = 0; s < block && c.cycle() < endCycle; ++s)
                    {
                        const std::uint64_t sampleEnd = c.cycle() + c.takeSampleCycles();
                        while (wi < schedule.size() && schedule[wi].cycle < sampleEnd)
                        {
                            c.advanceCycles (schedule[wi].cycle - c.cycle());
                            c.write (schedule[wi].reg, schedule[wi].value);
                            ok = ok && c.state() == expected[got];
                            ++got;
                            ++wi;
                        }
                        c.advanceCycles (sampleEnd - c.cycle());
                    }
                }
                INFO ("model " << (model == Model::A ? "A" : "B") << ", rate " << rate << ", block " << block);
                CHECK (ok);
                CHECK (got == schedule.size());
                // Past the end: run the cycles-only chip to the same cycle and compare.
                Chip tail = byCycles;
                if (c.cycle() > tail.cycle())
                    tail.advanceCycles (c.cycle() - tail.cycle());
                CHECK (c.state() == tail.state());
            }
    }
}
