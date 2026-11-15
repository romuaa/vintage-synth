// The two restart entry points for the program engine (Chip.h): powerOnKeepingTime (a stolen voice
// restarts its chip while absolute cycles keep running) and clearEnvelopeCounters (8.6.4, Clean restart:
// rate counter position and divider counter to 0, nothing else). Both keep the fast path equal to
// per-cycle stepping.

#include "bside/chip/Chip.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>

using namespace vs::bside::chip;

namespace
{
Chip prepared (Model m)
{
    Config cfg;
    cfg.model = m;
    cfg.authentic = true;
    Chip c;
    c.prepare (cfg);
    return c;
}

/** A busy chip: three oscillators with envelopes, a combined waveform, sync and ring. */
void play (Chip& c)
{
    const std::uint8_t regs[][2] = { { 0x00, 0x31 }, { 0x01, 0x1c }, { 0x05, 0x26 }, { 0x06, 0x9a }, { 0x04, 0x41 }, { 0x07, 0x77 }, { 0x08, 0x05 },
                                     { 0x0c, 0x5c }, { 0x0d, 0x33 }, { 0x0b, 0x33 }, { 0x0e, 0x10 }, { 0x0f, 0x40 }, { 0x13, 0x21 }, { 0x14, 0xf3 },
                                     { 0x12, 0x15 }, { 0x15, 0x03 }, { 0x16, 0x80 }, { 0x17, 0xf7 }, { 0x18, 0x1f } };
    for (const auto& r : regs)
        c.write (r[0], r[1]);
}

bool sameExceptCycle (DigitalState a, DigitalState b)
{
    a.cycle = b.cycle = 0;
    return a == b;
}
} // namespace

TEST_CASE ("chip restart: powerOnKeepingTime is power-on without moving time", "[bside][chip][restart]")
{
    for (const auto m : { Model::A, Model::B })
    {
        Chip c = prepared (m);
        play (c);
        c.advanceSamples (1000);
        const auto cycle = c.cycle();
        const auto fraction = c.sampleClock().fraction();
        c.powerOnKeepingTime();
        CHECK (c.cycle() == cycle);
        CHECK (c.sampleClock().fraction() == fraction);
        const Chip fresh = prepared (m);
        CHECK (sameExceptCycle (c.state(), fresh.state()));
        // From here on it behaves exactly like a chip powered on at this cycle.
        Chip ref = prepared (m);
        play (c);
        play (ref);
        c.advanceCycles (123457);
        ref.advanceCycles (123457);
        CHECK (c.cycle() == cycle + 123457);
        CHECK (sameExceptCycle (c.state(), ref.state()));
    }
}

TEST_CASE ("chip restart: clearEnvelopeCounters changes the rate counter position and the divider counter only", "[bside][chip][restart]")
{
    for (const auto m : { Model::A, Model::B })
        for (int n = 0; n < 3; ++n)
            for (int t = 0; t < 200; ++t)
            {
                Chip c = prepared (m);
                play (c);
                c.advanceCycles (static_cast<std::uint64_t> (1 + t * 9973));
                if (t % 3 == 1)
                    c.write (static_cast<std::uint8_t> (7 * n + 4), 0x40); // gate off: release
                c.advanceCycles (static_cast<std::uint64_t> (t % 7));
                DigitalState expect = c.state();
                auto& e = expect.env[static_cast<std::size_t> (n)];
                e.position = 0;
                e.divider = 0;
                c.clearEnvelopeCounters (n);
                INFO ("model " << (m == Model::A ? "A" : "B") << " envelope " << n << " run " << t);
                REQUIRE (c.state() == expect);
            }
    Chip c = prepared (Model::B);
    play (c);
    c.advanceCycles (5000);
    const auto before = c.state();
    c.clearEnvelopeCounters (3);
    c.clearEnvelopeCounters (-1);
    CHECK (c.state() == before);
}

TEST_CASE ("chip restart: the fast path equals per-cycle stepping across both restarts", "[bside][chip][restart][equivalence]")
{
    std::mt19937 rng (20261115);
    for (const auto m : { Model::A, Model::B })
    {
        Chip stepped = prepared (m), fast = prepared (m);
        play (stepped);
        play (fast);
        for (int i = 0; i < 400; ++i)
        {
            const auto gap = static_cast<std::uint64_t> (rng() % 5000);
            for (std::uint64_t k = 0; k < gap; ++k)
                stepped.stepCycle();
            fast.advanceCycles (gap);
            REQUIRE (fast.state() == stepped.state());
            const auto op = rng() % 4;
            const auto which = static_cast<int> (rng() % 3);
            if (op >= 2)
            {
                const auto r = static_cast<std::uint8_t> (rng() % 0x19), v = static_cast<std::uint8_t> (rng() % 256);
                stepped.write (r, v);
                fast.write (r, v);
            }
            for (Chip* c : { &stepped, &fast })
            {
                if (op == 0)
                {
                    c->powerOnKeepingTime();
                    play (*c);
                }
                else if (op == 1)
                    c->clearEnvelopeCounters (which);
            }
            REQUIRE (fast.state() == stepped.state());
        }
    }
}
