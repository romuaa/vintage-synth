// The chip's digital core against the reference behaviours of docs/architecture.md 8.4.3, one
// section per row of its table (rows 1-14), plus the register file of 8.4.1. Expected values are
// worked out from the contract's text, not from the implementation.

#include "bside/chip/Chip.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

using namespace vs::bside::chip;

namespace
{
Chip makeChip (Model m = Model::B)
{
    Chip c;
    Config cfg;
    cfg.model = m;
    cfg.authentic = true;
    c.prepare (cfg);
    return c;
}

void wr (Chip& c, int osc, int r, int v)
{
    c.write (static_cast<std::uint8_t> (osc * reg::oscStride + r), static_cast<std::uint8_t> (v));
}

void setFreq (Chip& c, int osc, std::uint32_t f)
{
    wr (c, osc, reg::freqLo, static_cast<int> (f & 0xff));
    wr (c, osc, reg::freqHi, static_cast<int> (f >> 8));
}

void setPw (Chip& c, int osc, std::uint32_t pw)
{
    wr (c, osc, reg::pwLo, static_cast<int> (pw & 0xff));
    wr (c, osc, reg::pwHi, static_cast<int> (pw >> 8));
}

/** One LFSR step written from the contract's words (row 5), independent of the implementation. */
std::uint32_t lfsr (std::uint32_t r)
{
    const std::uint32_t b22 = (r >> 22) & 1u, b17 = (r >> 17) & 1u;
    return ((r << 1) & 0x7fffffu) | (b22 ^ b17);
}

/** Time from 0xff to 0 in rate periods, from the divider table of row 10 (value being decremented). */
std::uint64_t dividerSumFromFull()
{
    std::uint64_t sum = 0;
    for (int v = 1; v <= 255; ++v)
        sum += v > 0x5d ? 1 : v > 0x36 ? 2 : v > 0x1a ? 4 : v > 0x0e ? 8 : v > 0x06 ? 16 : 30;
    return sum;
}
} // namespace

TEST_CASE ("chip: the object owns no memory and its audio-thread calls are noexcept", "[bside][chip]")
{
    STATIC_REQUIRE (std::is_trivially_copyable_v<Chip>);
    Chip c;
    STATIC_REQUIRE (noexcept (c.write (0, 0)));
    STATIC_REQUIRE (noexcept (c.setOscillator (0, OscInput {})));
    STATIC_REQUIRE (noexcept (c.setFilter (FilterInput {})));
    STATIC_REQUIRE (noexcept (c.read (0x1b)));
    STATIC_REQUIRE (noexcept (c.advanceCycles (1)));
    STATIC_REQUIRE (noexcept (c.stepCycle()));
    STATIC_REQUIRE (noexcept (c.advanceSamples (1)));
    STATIC_REQUIRE (noexcept (c.state()));
    STATIC_REQUIRE (noexcept (c.reset()));
}

TEST_CASE ("chip 8.4.1: power-on state and the register file", "[bside][chip]")
{
    auto c = makeChip();
    const auto s = c.state();
    CHECK (s.cycle == 0);
    for (int n = 0; n < 3; ++n)
    {
        const auto i = static_cast<std::size_t> (n);
        CHECK (s.osc[i].acc == 0);
        CHECK (s.osc[i].noise == 0x7fffff);
        CHECK (s.env[i].counter == 0);
        CHECK (s.env[i].phase == 2); // release
        CHECK (s.env[i].rateCounter == 0);
        CHECK (s.env[i].divider == 0);
    }
    for (auto v : s.registers)
        CHECK (v == 0);

    // Every writable register stores its value; addresses are taken modulo 32; 0x19-0x1f are not stored.
    for (int r = 0; r < 0x19; ++r)
        c.write (static_cast<std::uint8_t> (r), static_cast<std::uint8_t> (0x40 + r));
    for (int r = 0; r < 0x19; ++r)
        CHECK (c.registerValue (static_cast<std::uint8_t> (r)) == 0x40 + r);
    c.write (0x20 + 0x18, 0x5a); // mirrors 0x18
    CHECK (c.registerValue (0x18) == 0x5a);
    const auto before = c.state();
    c.write (0x19, 0x77);
    c.write (0x1f, 0x77);
    c.write (0x1b, 0x77);
    CHECK (c.state() == before);
    // Frequency 16 bits, pulse width 12 bits (the high nibble of 0x03 is ignored).
    auto d = makeChip();
    setFreq (d, 0, 0xabcd);
    setPw (d, 0, 0xf123);
    CHECK (d.increment (0) == (std::uint64_t { 0xabcd } << 32));
    CHECK (d.pulseWidth (0) == 0x123);
}

TEST_CASE ("chip 8.4.3 row 1: the accumulator adds F every cycle and wraps at 2^24", "[bside][chip]")
{
    auto c = makeChip();
    setFreq (c, 0, 0x1234);
    setFreq (c, 1, 0xffff);
    c.advanceCycles (1);
    CHECK (c.accumulator (0) == 0x1234);
    CHECK (c.accumulator (1) == 0xffff);
    CHECK (c.accumulator (2) == 0);
    c.advanceCycles (999);
    CHECK (c.accumulator (0) == (1000u * 0x1234u) % (1u << 24));
    CHECK (c.accumulator (1) == (1000u * 0xffffu) % (1u << 24));

    // Output frequency = F x clock / 2^24: count top-bit rises over a second of the 50 Hz clock.
    auto d = makeChip();
    const std::uint32_t f = 7382; // about 433.5 Hz
    setFreq (d, 0, f);
    std::uint64_t rises = 0;
    std::uint32_t prev = 0;
    for (int i = 0; i < 985248; ++i)
    {
        d.stepCycle();
        const auto a = d.accumulator (0);
        if ((prev & 0x800000u) == 0 && (a & 0x800000u) != 0)
            ++rises;
        prev = a;
    }
    // Rises happen at cycles k where k F crosses 2^23 + m 2^24.
    const std::uint64_t expected = (985248ull * f + (1ull << 23)) / (1ull << 24);
    CHECK (rises == expected);
}

TEST_CASE ("chip 8.4.3 row 2: saw = accumulator bits 23..12", "[bside][chip]")
{
    auto c = makeChip();
    setFreq (c, 2, 0x0d0b);
    wr (c, 2, reg::control, ctrl::saw);
    for (int i = 0; i < 2000; ++i)
    {
        c.advanceCycles (37);
        const auto acc = c.accumulator (2);
        CHECK (c.output (2) == ((acc >> 12) & 0xfff));
        CHECK (c.read (0x1b) == ((acc >> 16) & 0xff));
    }
}

TEST_CASE ("chip 8.4.3 row 3: triangle folds on the top bit; ring XORs the source's top bit into the fold", "[bside][chip]")
{
    // Hand values.
    CHECK (wave::triangle (0x000000, false, 0) == 0x000);
    CHECK (wave::triangle (0x400000, false, 0) == 0x800);
    CHECK (wave::triangle (0x7ff800, false, 0) == 0xfff);
    CHECK (wave::triangle (0x800000, false, 0) == 0xfff); // ~0x800000 = 0x7fffff (bits 22..11 all ones)
    CHECK (wave::triangle (0xc00000, false, 0) == 0x7ff);
    CHECK (wave::triangle (0xffffff, false, 0) == 0x000);
    CHECK (wave::triangle (0x400000, true, 0x800000) == 0x7ff); // fold inverted by the source's top bit
    CHECK (wave::triangle (0xc00000, true, 0x800000) == 0x800);
    CHECK (wave::triangle (0xc00000, true, 0x7fffff) == 0x7ff); // source top bit clear: no change

    // In the chip: oscillator 3's ring source is oscillator 2.
    auto c = makeChip();
    setFreq (c, 1, 0x0777);
    setFreq (c, 2, 0x1301);
    wr (c, 2, reg::control, ctrl::triangle | ctrl::ring);
    for (int i = 0; i < 3000; ++i)
    {
        c.advanceCycles (29);
        const std::uint32_t acc = c.accumulator (2), src = c.accumulator (1);
        const bool fold = ((acc ^ src) & 0x800000u) != 0;
        const std::uint32_t expected = ((fold ? ~acc : acc) >> 11) & 0xfff;
        CHECK (c.output (2) == expected);
    }
    // Ring without triangle changes nothing (saw is not ring-modulated).
    wr (c, 2, reg::control, ctrl::saw | ctrl::ring);
    CHECK (c.output (2) == ((c.accumulator (2) >> 12) & 0xfff));
}

TEST_CASE ("chip 8.4.3 row 4: pulse compares bits 23..12 with PW; the test bit forces it high", "[bside][chip]")
{
    CHECK (wave::pulse (0x7ff000, 0x800, false) == 0);
    CHECK (wave::pulse (0x800000, 0x800, false) == 0xfff);
    CHECK (wave::pulse (0x000000, 0x000, false) == 0xfff); // PW 0: always high
    CHECK (wave::pulse (0xfff000, 0xfff, false) == 0xfff);
    CHECK (wave::pulse (0xffefff, 0xfff, false) == 0);
    CHECK (wave::pulse (0x000000, 0x800, true) == 0xfff);

    auto c = makeChip();
    setFreq (c, 2, 0x0405);
    setPw (c, 2, 0x0a00);
    wr (c, 2, reg::control, ctrl::pulse);
    for (int i = 0; i < 2000; ++i)
    {
        c.advanceCycles (31);
        CHECK (c.output (2) == (((c.accumulator (2) >> 12) & 0xfff) >= 0xa00 ? 0xfff : 0));
    }
    wr (c, 2, reg::control, ctrl::pulse | ctrl::test);
    CHECK (c.read (0x1b) == 0xff);
    c.advanceCycles (100);
    CHECK (c.read (0x1b) == 0xff);
}

TEST_CASE ("chip 8.4.3 row 5: noise is clocked on bit-19 rises; feedback 22 XOR 17; output bit map", "[bside][chip]")
{
    // Output bit map, one register bit at a time.
    const int regBits[8] = { 20, 18, 14, 11, 9, 5, 2, 0 };
    for (int k = 0; k < 8; ++k)
        CHECK (wave::noise (1u << regBits[k]) == (1u << (11 - k)));
    for (int b = 0; b < 23; ++b)
    {
        bool used = false;
        for (int k = 0; k < 8; ++k)
            used = used || regBits[k] == b;
        if (! used)
            CHECK (wave::noise (1u << b) == 0);
    }

    // F = 0x8000: bit 19 rises first at cycle 16 (16 x 2^15 = 2^19), then every 32 cycles.
    auto c = makeChip();
    setFreq (c, 0, 0x8000);
    wr (c, 0, reg::control, ctrl::noise);
    c.advanceCycles (15);
    CHECK (c.noiseRegister (0) == 0x7fffff);
    c.advanceCycles (1);
    CHECK (c.noiseRegister (0) == 0x7ffffe);
    std::uint32_t r = 0x7ffffe;
    for (int k = 0; k < 500; ++k)
    {
        c.advanceCycles (31);
        CHECK (c.noiseRegister (0) == r); // no clock between rises
        c.advanceCycles (1);
        r = lfsr (r);
        REQUIRE (c.noiseRegister (0) == r);
    }
    // The register is clocked whatever waveform is selected (here none).
    auto d = makeChip();
    setFreq (d, 1, 0x8000);
    d.advanceCycles (16);
    CHECK (d.noiseRegister (1) == 0x7ffffe);
    // Output of the selected noise.
    CHECK (c.output (0) == wave::noise (c.noiseRegister (0)));
}

TEST_CASE ("chip 8.4.3 row 6: sync resets the target in the cycle of the source's top-bit rise", "[bside][chip]")
{
    // Pairs: source -> target 1 -> 2, 2 -> 3, 3 -> 1 (0-based 0 -> 1, 1 -> 2, 2 -> 0).
    for (int src = 0; src < 3; ++src)
    {
        const int dst = (src + 1) % 3;
        auto c = makeChip();
        setFreq (c, src, 0x1000); // top bit rises at cycle 2048
        setFreq (c, dst, 0x0100);
        wr (c, dst, reg::control, ctrl::sync);
        c.advanceCycles (2047);
        CHECK (c.accumulator (dst) == 2047u * 0x100u);
        c.advanceCycles (1);
        CHECK (c.accumulator (dst) == 0);
        c.advanceCycles (1);
        CHECK (c.accumulator (dst) == 0x100);
        // The third oscillator is unaffected.
        CHECK (c.accumulator ((src + 2) % 3) == 0);
        // Without the target's sync bit there is no reset.
        auto d = makeChip();
        setFreq (d, src, 0x1000);
        setFreq (d, dst, 0x0100);
        d.advanceCycles (2048);
        CHECK (d.accumulator (dst) == 2048u * 0x100u);
    }
}

TEST_CASE ("chip 8.4.3 row 6 (confirm): a source synced in the same cycle does not sync its own target", "[bside][chip]")
{
    // Oscillators 1 and 2 both rise at cycle 2048; 2 is synced by 1 and has 3 as its target.
    auto chained = makeChip();
    setFreq (chained, 0, 0x1000);
    setFreq (chained, 1, 0x1000);
    setFreq (chained, 2, 0x0100);
    wr (chained, 1, reg::control, ctrl::sync);
    wr (chained, 2, reg::control, ctrl::sync);
    chained.advanceCycles (2048);
    CHECK (chained.accumulator (1) == 0);                  // synced by 1
    CHECK (chained.accumulator (2) == 2048u * 0x100u);     // not synced: its source was synced this cycle

    // The same without oscillator 2's sync bit: 2 rises freely and syncs 3.
    auto plain = makeChip();
    setFreq (plain, 0, 0x1000);
    setFreq (plain, 1, 0x1000);
    setFreq (plain, 2, 0x0100);
    wr (plain, 2, reg::control, ctrl::sync);
    plain.advanceCycles (2048);
    CHECK (plain.accumulator (1) == 0x800000);
    CHECK (plain.accumulator (2) == 0);
}

TEST_CASE ("chip 8.4.3 row 7: the test bit holds the accumulator at 0 and resets the noise register", "[bside][chip]")
{
    auto c = makeChip();
    setFreq (c, 0, 0x4321);
    wr (c, 0, reg::control, ctrl::noise);
    c.advanceCycles (5000);
    REQUIRE (c.accumulator (0) != 0);
    REQUIRE (c.noiseRegister (0) != 0x7fffff);
    wr (c, 0, reg::control, ctrl::noise | ctrl::test);
    CHECK (c.accumulator (0) == 0); // at the write
    CHECK (c.noiseRegister (0) == 0x7fffff);
    c.advanceCycles (100000);
    CHECK (c.accumulator (0) == 0); // held
    CHECK (c.noiseRegister (0) == 0x7fffff);
    wr (c, 0, reg::control, ctrl::noise);
    c.advanceCycles (3);
    CHECK (c.accumulator (0) == 3u * 0x4321u); // runs again from 0
    // A test-mode oscillator neither syncs nor is synced.
    auto d = makeChip();
    setFreq (d, 0, 0x1000);
    wr (d, 0, reg::control, ctrl::test);
    setFreq (d, 1, 0x0100);
    wr (d, 1, reg::control, ctrl::sync);
    d.advanceCycles (5000);
    CHECK (d.accumulator (1) == 5000u * 0x100u);
}

TEST_CASE ("chip 8.4.3 row 8: noise combined with another waveform writes back and locks up until a test reset", "[bside][chip]")
{
    for (auto model : { Model::A, Model::B })
    {
        auto c = makeChip (model);
        setFreq (c, 0, 0x2345);
        wr (c, 0, reg::control, ctrl::noise | ctrl::saw);
        // Write-back is per cycle: after any cycle, the output bits of the register are cleared where the
        // combined output (AND, as built) is 0.
        for (int i = 0; i < 2000; ++i)
        {
            c.advanceCycles (1);
            const std::uint16_t out = c.output (0);
            CHECK ((wave::noise (c.noiseRegister (0)) & ~out & 0xff0) == 0);
        }
        c.advanceCycles (2000000);
        CHECK (c.noiseRegister (0) == 0); // locked up
        wr (c, 0, reg::control, ctrl::noise);
        c.advanceCycles (100000);
        CHECK (c.noiseRegister (0) == 0); // an all-zero register stays zero: silent
        CHECK (c.read (0x1b) == 0);
        wr (c, 0, reg::control, ctrl::noise | ctrl::test);
        wr (c, 0, reg::control, ctrl::noise);
        CHECK (c.noiseRegister (0) == 0x7fffff);
        c.advanceCycles (100000);
        CHECK (c.noiseRegister (0) != 0);
    }
    // Combined outputs (as built: AND of the selected outputs), through the read register.
    auto d = makeChip();
    setFreq (d, 2, 0x0fed);
    setPw (d, 2, 0x600);
    wr (d, 2, reg::control, ctrl::saw | ctrl::triangle | ctrl::pulse);
    for (int i = 0; i < 1000; ++i)
    {
        d.advanceCycles (41);
        const std::uint32_t acc = d.accumulator (2);
        const std::uint32_t expected = wave::saw (acc) & wave::triangle (acc, false, 0) & wave::pulse (acc, 0x600, false);
        CHECK (d.output (2) == expected);
    }
    // Without noise there is no write-back.
    CHECK (d.noiseRegister (2) != 0);
}

TEST_CASE ("chip 8.4.3 row 9: envelope rate periods (attack from 0 to 0xff takes 255 periods)", "[bside][chip]")
{
    const std::uint32_t periods[16] = { 9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251 };
    for (int nib = 0; nib < 16; ++nib)
    {
        auto c = makeChip();
        wr (c, 0, reg::attackDecay, (nib << 4) | 0x0f);
        wr (c, 0, reg::sustainRelease, 0xf0);
        wr (c, 0, reg::control, ctrl::gate);
        const std::uint64_t p = periods[nib];
        c.advanceCycles (p - 1);
        CHECK (c.envelope (0) == 0);
        c.advanceCycles (1);
        CHECK (c.envelope (0) == 1);
        c.advanceCycles (253 * p);
        CHECK (c.envelope (0) == 254);
        CHECK (c.envelopePhase (0) == EnvPhase::attack);
        c.advanceCycles (p);
        CHECK (c.envelope (0) == 0xff);
        CHECK (c.envelopePhase (0) == EnvPhase::decaySustain);
    }
}

TEST_CASE ("chip 8.4.3 row 10: decay and release divide the rate by the exponential divider and stop at 0", "[bside][chip]")
{
    CHECK (dividerSumFromFull() == 756);
    for (int nib : { 0, 1, 5, 9 })
    {
        const std::uint64_t p = kRatePeriods[static_cast<std::size_t> (nib)];
        auto c = makeChip();
        wr (c, 0, reg::attackDecay, 0x00 | nib); // attack 9 cycles
        wr (c, 0, reg::sustainRelease, 0x00);    // sustain 0
        wr (c, 0, reg::control, ctrl::gate);
        c.advanceCycles (255 * 9);
        REQUIRE (c.envelope (0) == 0xff);
        // Step by step: each value v is left after p x divider(v) cycles.
        for (int v = 255; v >= 1; --v)
        {
            const std::uint64_t div = v > 0x5d ? 1 : v > 0x36 ? 2 : v > 0x1a ? 4 : v > 0x0e ? 8 : v > 0x06 ? 16 : 30;
            c.advanceCycles (p * div - 1);
            REQUIRE (c.envelope (0) == v);
            c.advanceCycles (1);
            REQUIRE (c.envelope (0) == v - 1);
        }
        c.advanceCycles (1000000);
        CHECK (c.envelope (0) == 0); // does not wrap
    }
    // Release the same way, from a full sustain.
    auto r = makeChip();
    wr (r, 0, reg::attackDecay, 0x00);
    wr (r, 0, reg::sustainRelease, 0xf3);
    wr (r, 0, reg::control, ctrl::gate);
    r.advanceCycles (255 * 9);
    REQUIRE (r.envelope (0) == 0xff);
    r.advanceCycles (5); // rate counter now 5 (decay period 9, sustain holds at 0xff)
    wr (r, 0, reg::control, 0);
    const std::uint64_t p = kRatePeriods[3];
    r.advanceCycles (p - 5 - 1 + 1); // first match of the release period
    CHECK (r.envelope (0) == 0xfe);
    r.advanceCycles (p * (dividerSumFromFull() - 1));
    CHECK (r.envelope (0) == 0);
    r.advanceCycles (3000000);
    CHECK (r.envelope (0) == 0);

    // Gate on and off before the first attack step: release from 0 stays at 0 (no wrap to 0xff).
    auto z = makeChip();
    wr (z, 0, reg::control, ctrl::gate);
    z.advanceCycles (5);
    wr (z, 0, reg::control, 0);
    z.advanceCycles (100000);
    CHECK (z.envelope (0) == 0);
}

TEST_CASE ("chip 8.4.3 row 10: sustain level = nibble x 0x11; lowering decays to it, raising never raises the counter", "[bside][chip]")
{
    for (int s = 0; s < 16; ++s)
    {
        auto c = makeChip();
        wr (c, 0, reg::attackDecay, 0x00);
        wr (c, 0, reg::sustainRelease, s << 4);
        wr (c, 0, reg::control, ctrl::gate);
        c.advanceCycles (255 * 9 + 9 * 756 + 1000);
        CHECK (c.envelope (0) == s * 0x11);
    }
    auto c = makeChip();
    wr (c, 0, reg::attackDecay, 0x00);
    wr (c, 0, reg::sustainRelease, 0x80);
    wr (c, 0, reg::control, ctrl::gate);
    c.advanceCycles (100000);
    REQUIRE (c.envelope (0) == 0x88);
    wr (c, 0, reg::sustainRelease, 0x40); // lower: decays to the new level
    c.advanceCycles (100000);
    CHECK (c.envelope (0) == 0x44);
    // Raise: the counter never rises. As built (equality comparator, contract question), it decays on
    // to 0, because it never meets the new level from below.
    wr (c, 0, reg::sustainRelease, 0xc0);
    std::uint8_t last = c.envelope (0);
    for (int i = 0; i < 200; ++i)
    {
        c.advanceCycles (1000);
        CHECK (c.envelope (0) <= last);
        last = c.envelope (0);
    }
    CHECK (c.envelope (0) == 0);
}

TEST_CASE ("chip 8.4.3 row 10 (confirm): the divider period is latched at the exact threshold values", "[bside][chip]")
{
    // Attack from 0 to 0x80 passes 0x06, 0x0e, 0x1a, 0x36 and 0x5d: the latched divider is 2 there.
    // A release from 0x80 then takes 2 periods per step until 0x5d (where it latches 2 again).
    auto c = makeChip();
    wr (c, 0, reg::attackDecay, 0x00);
    wr (c, 0, reg::sustainRelease, 0x00); // release 9
    wr (c, 0, reg::control, ctrl::gate);
    c.advanceCycles (0x80 * 9);
    REQUIRE (c.envelope (0) == 0x80);
    CHECK (c.state().env[0].dividerPeriod == 2);
    wr (c, 0, reg::control, 0); // rate counter is 0 after the last attack step
    c.advanceCycles (9);
    CHECK (c.envelope (0) == 0x80);
    c.advanceCycles (9);
    CHECK (c.envelope (0) == 0x7f);
}

TEST_CASE ("chip 8.4.3 row 11: the rate counter is 15 bits; a smaller period waits for the wrap (delay quirk)", "[bside][chip]")
{
    auto c = makeChip();
    wr (c, 0, reg::attackDecay, 0xf0); // attack 31251
    wr (c, 0, reg::control, ctrl::gate);
    c.advanceCycles (20000);
    CHECK (c.state().env[0].rateCounter == 20000);
    CHECK (c.envelope (0) == 0);
    wr (c, 0, reg::attackDecay, 0x00); // attack 9: below the count
    // 0x7fff - 20000 cycles to reach 0x7fff, one to wrap to 1, 8 more to reach 9.
    const std::uint64_t wait = 0x7fff - 20000 + 9;
    CHECK (wait == 12776);
    c.advanceCycles (wait - 1);
    CHECK (c.envelope (0) == 0);
    CHECK (c.state().env[0].rateCounter == 8);
    c.advanceCycles (1);
    CHECK (c.envelope (0) == 1);
    CHECK (c.state().env[0].rateCounter == 0);
    // Then at the new period.
    c.advanceCycles (9);
    CHECK (c.envelope (0) == 2);

    // The counter value after 0x7fff is 1 (it never reads 0 except right after a match).
    auto d = makeChip();
    wr (d, 0, reg::sustainRelease, 0x0f); // release 31251
    d.advanceCycles (31250);
    CHECK (d.state().env[0].rateCounter == 31250);
    wr (d, 0, reg::sustainRelease, 0x00);
    d.advanceCycles (0x7fff - 31250);
    CHECK (d.state().env[0].rateCounter == 0x7fff);
    d.advanceCycles (1);
    CHECK (d.state().env[0].rateCounter == 1);
}

TEST_CASE ("chip 8.4.3 row 12: gate edges start attack and release from the current counter", "[bside][chip]")
{
    auto c = makeChip();
    wr (c, 0, reg::attackDecay, 0x00);
    wr (c, 0, reg::sustainRelease, 0x00);
    wr (c, 0, reg::control, ctrl::gate);
    c.advanceCycles (0x40 * 9);
    REQUIRE (c.envelope (0) == 0x40);
    wr (c, 0, reg::control, 0);
    CHECK (c.envelopePhase (0) == EnvPhase::release);
    CHECK (c.envelope (0) == 0x40);
    c.advanceCycles (9 * 4 * 2); // divider latched 4 (0x36 passed going up)
    CHECK (c.envelope (0) == 0x3e);
    wr (c, 0, reg::control, ctrl::gate);
    CHECK (c.envelopePhase (0) == EnvPhase::attack);
    c.advanceCycles (9);
    CHECK (c.envelope (0) == 0x3f);
    // Writing the control register with the gate unchanged starts nothing.
    wr (c, 0, reg::control, ctrl::gate | ctrl::saw);
    CHECK (c.envelopePhase (0) == EnvPhase::attack);
    c.advanceCycles (9);
    CHECK (c.envelope (0) == 0x40);

    // (confirm) An attack started at 0xff wraps to 0 on its first step (8-bit counter).
    auto w = makeChip();
    wr (w, 0, reg::attackDecay, 0x00);
    wr (w, 0, reg::sustainRelease, 0xf0);
    wr (w, 0, reg::control, ctrl::gate);
    w.advanceCycles (255 * 9 + 3);
    REQUIRE (w.envelope (0) == 0xff);
    wr (w, 0, reg::control, 0);
    wr (w, 0, reg::control, ctrl::gate);
    w.advanceCycles (6); // rate counter was 3; the attack period is 9
    CHECK (w.envelope (0) == 0x00);
    CHECK (w.envelopePhase (0) == EnvPhase::attack);
}

TEST_CASE ("chip 8.4.3 row 13: read registers 0x1B and 0x1C", "[bside][chip]")
{
    auto c = makeChip();
    setFreq (c, 2, 0x1111);
    wr (c, 2, reg::control, ctrl::saw | ctrl::gate);
    wr (c, 2, reg::attackDecay, 0x20);
    wr (c, 2, reg::sustainRelease, 0xf0);
    for (int i = 0; i < 300; ++i)
    {
        c.advanceCycles (101);
        CHECK (c.read (0x1b) == (c.output (2) >> 4));
        CHECK (c.read (0x1c) == c.envelope (2));
        CHECK (c.read (0x1b + 0x20) == c.read (0x1b)); // address modulo 32
    }
    CHECK (c.read (0x1c) != 0);
    // Oscillators 1 and 2 do not show in the read registers.
    auto d = makeChip();
    setFreq (d, 0, 0x2222);
    wr (d, 0, reg::control, ctrl::saw | ctrl::gate);
    d.advanceCycles (5000);
    CHECK (d.read (0x1b) == 0);
    CHECK (d.read (0x1c) == 0);
    // No waveform selected: 0 (as built; contract question). Other addresses read 0.
    CHECK (c.read (0x00) == 0);
    CHECK (c.read (0x19) == 0);
    wr (c, 2, reg::control, ctrl::gate);
    CHECK (c.read (0x1b) == 0);
}

TEST_CASE ("chip 8.4.3 row 14: oscillator 3 off changes no digital state and not the read registers", "[bside][chip]")
{
    auto a = makeChip();
    auto b = makeChip();
    for (auto* c : { &a, &b })
    {
        setFreq (*c, 2, 0x3456);
        wr (*c, 2, reg::control, ctrl::triangle | ctrl::gate);
    }
    b.write (0x18, 0x8f);
    a.write (0x18, 0x0f);
    for (int i = 0; i < 100; ++i)
    {
        a.advanceCycles (997);
        b.advanceCycles (997);
        auto sa = a.state();
        auto sb = b.state();
        CHECK (sa.osc3Output == sb.osc3Output);
        CHECK (sa.env3Output == sb.env3Output);
        sb.registers[0x18] = sa.registers[0x18];
        CHECK (sa == sb);
    }
}

TEST_CASE ("chip 8.4.6: Authentic setOscillator equals the register writes; Smooth carries a fraction", "[bside][chip]")
{
    auto viaRegs = makeChip();
    auto viaInput = makeChip();
    setFreq (viaRegs, 1, 1234);
    setPw (viaRegs, 1, 2049);
    wr (viaRegs, 1, reg::attackDecay, 0x37);
    wr (viaRegs, 1, reg::sustainRelease, 0xa5);
    wr (viaRegs, 1, reg::control, ctrl::pulse | ctrl::gate);
    viaInput.setOscillator (1, { 1234.4, 2048.6f, ctrl::pulse | ctrl::gate, 0x37, 0xa5 });
    CHECK (viaRegs.state() == viaInput.state());
    viaRegs.advanceCycles (123456);
    viaInput.advanceCycles (123456);
    CHECK (viaRegs.state() == viaInput.state());
    // Authentic clamps to the 16-bit register.
    viaInput.setOscillator (0, { 1.0e6, 9000.0f, 0, 0, 0 });
    CHECK (viaInput.increment (0) == (std::uint64_t { 65535 } << 32));
    CHECK (viaInput.pulseWidth (0) == 4095);

    Chip smooth;
    Config cfg;
    cfg.authentic = false;
    smooth.prepare (cfg);
    smooth.setOscillator (0, { 1000.25, 100.75f, ctrl::saw, 0, 0 });
    CHECK (smooth.increment (0) == (std::uint64_t { 1000 } << 32) + (std::uint64_t { 1 } << 30));
    CHECK (smooth.pulseWidth (0) == 100);
    smooth.advanceCycles (4);
    CHECK (smooth.accumulator (0) == 4001); // 4 x 1000.25
    CHECK (smooth.state().osc[0].accFraction == 0);
    smooth.advanceCycles (1);
    CHECK (smooth.state().osc[0].accFraction == (1u << 30));
    // Non-finite inputs are taken as 0; huge ones are clamped below 2^19.
    smooth.setOscillator (1, { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<float>::infinity(), 0, 0, 0 });
    CHECK (smooth.increment (1) == 0);
    smooth.setOscillator (2, { 1.0e12, 0.0f, 0, 0, 0 });
    CHECK (smooth.increment (2) == kMaxIncrement);
}

TEST_CASE ("chip 8.4.3 row 6 (confirm): the chain case in all three rotations", "[bside][chip]")
{
    // Rotation a -> b -> c: a and b rise in the same cycle (2048), b and c have their sync bits.
    // b is synced by a; c is NOT synced, because its source b was synced in that cycle.
    for (int a = 0; a < 3; ++a)
    {
        const int b = (a + 1) % 3, c = (a + 2) % 3;
        auto chip = makeChip();
        setFreq (chip, a, 0x1000);
        setFreq (chip, b, 0x1000);
        setFreq (chip, c, 0x0100);
        wr (chip, b, reg::control, ctrl::sync);
        wr (chip, c, reg::control, ctrl::sync);
        chip.advanceCycles (2048);
        INFO ("rotation starting at oscillator " << (a + 1));
        CHECK (chip.accumulator (a) == 0x800000);
        CHECK (chip.accumulator (b) == 0);
        CHECK (chip.accumulator (c) == 2048u * 0x100u);
        // One cycle later nothing else happened.
        chip.stepCycle();
        CHECK (chip.accumulator (b) == 0x1000);
        CHECK (chip.accumulator (c) == 2049u * 0x100u);
    }
}

TEST_CASE ("chip 8.4.3 row 6 (confirm): all three rise together with every sync bit set: nobody is reset", "[bside][chip]")
{
    // Each source is synced in that cycle by its own source, so no target is reset. A loop that
    // applied resets one by one ("was my source reset already?") would reset some of them.
    for (int round = 0; round < 2; ++round)
    {
        auto chip = makeChip();
        for (int n = 0; n < 3; ++n)
        {
            setFreq (chip, n, 0x1000);
            wr (chip, n, reg::control, ctrl::sync | ctrl::saw);
        }
        if (round == 0)
            chip.advanceCycles (2048);
        else
            for (int i = 0; i < 2048; ++i)
                chip.stepCycle();
        for (int n = 0; n < 3; ++n)
            CHECK (chip.accumulator (n) == 0x800000);
    }
    // Two of three rising together, all sync bits set: only the target of the unsynced riser resets.
    auto chip = makeChip();
    setFreq (chip, 0, 0x1000);
    setFreq (chip, 1, 0x1000);
    setFreq (chip, 2, 0x0300);
    for (int n = 0; n < 3; ++n)
        wr (chip, n, reg::control, ctrl::sync);
    chip.advanceCycles (2048);
    // Oscillator 3 did not rise (2048 x 0x300 = 0x180000), so 1 is not synced; 1 rose and syncs 2;
    // 2 rose but was synced by 1 in that cycle, so 3 keeps running.
    CHECK (chip.accumulator (0) == 0x800000);
    CHECK (chip.accumulator (1) == 0);
    CHECK (chip.accumulator (2) == 2048u * 0x300u);
}

TEST_CASE ("chip 8.4.3 rows 7/8: no write-back while the test bit is held", "[bside][chip]")
{
    // Noise + saw with the test bit: the accumulator is held at 0, so saw is 0 and an AND write-back
    // would clear the register. While the test bit is set the register stays all ones.
    for (auto model : { Model::A, Model::B })
    {
        auto c = makeChip (model);
        setFreq (c, 1, 0x2345);
        wr (c, 1, reg::control, ctrl::noise | ctrl::saw | ctrl::test);
        for (int i = 0; i < 100; ++i)
        {
            c.stepCycle();
            REQUIRE (c.noiseRegister (1) == 0x7fffff);
        }
        c.advanceCycles (1000000);
        CHECK (c.noiseRegister (1) == 0x7fffff);
        // Released: the first cycle's saw is 0x002 (0x2345 >> 12) and the write-back starts at once.
        wr (c, 1, reg::control, ctrl::noise | ctrl::saw);
        c.stepCycle();
        CHECK (c.noiseRegister (1) != 0x7fffff);
    }
}

namespace
{
struct SyncOnlySink
{
    static constexpr bool kWantsNoiseEvents = false;
    int calls = 0, noiseCalls = 0;
    SyncResetEvent last;
    void syncReset (const SyncResetEvent& e) noexcept
    {
        ++calls;
        last = e;
    }
    void noiseClock (const NoiseClockEvent&) noexcept { ++noiseCalls; }
    void noiseWriteBack (const NoiseWriteBackEvent&) noexcept { ++noiseCalls; }
};
} // namespace

TEST_CASE ("chip: the sync event carries the sub-cycle crossing and the target's phase", "[bside][chip]")
{
    using Sink = SyncOnlySink;
    auto c = makeChip();
    setFreq (c, 0, 0x1001); // top bit: 2048 x 0x1001 = 0x800800 is the first value >= 0x800000
    setFreq (c, 1, 0x0100);
    wr (c, 1, reg::control, ctrl::sync);
    Sink sink;
    c.advanceCycles (5000, sink); // also many noise clocks: none may reach the sink
    REQUIRE (sink.calls >= 1);
    CHECK (sink.noiseCalls == 0); // kWantsNoiseEvents = false: neither path calls them
    for (int i = 0; i < 5000; ++i)
        c.stepCycle (sink);
    CHECK (sink.noiseCalls == 0);
    // The first reset: re-run to it.
    auto d = makeChip();
    setFreq (d, 0, 0x1001);
    setFreq (d, 1, 0x0100);
    wr (d, 1, reg::control, ctrl::sync);
    Sink first;
    d.advanceCycles (2048, first);
    REQUIRE (first.calls == 1);
    CHECK (first.last.cycle == 2048);
    CHECK (first.last.target == 1);
    CHECK (first.last.source == 0);
    CHECK (first.last.sourcePhase == (std::uint64_t { 0x800800 } << 32));
    CHECK (first.last.sourceIncrement == (std::uint64_t { 0x1001 } << 32));
    CHECK (first.last.targetPhaseBefore == (std::uint64_t { 2048 * 0x100 } << 32));
    CHECK (std::abs (first.last.crossingBefore() - 2048.0 / 4097.0) < 1e-15);
    // Sub-sample position: a sample starting at cycle 2040.5 lasting 10.5 cycles.
    const auto inc = static_cast<std::uint64_t> (10.5 * 4294967296.0);
    const double p = ChipClock::position (2040, 1u << 31, inc, first.last.cycle, first.last.crossingBefore());
    CHECK (std::abs (p - (2048.0 - 2048.0 / 4097.0 - 2040.5) / 10.5) < 1e-12);
    CHECK (std::abs (ChipClock::position (100, 0, inc, 100)) < 1e-15);
    CHECK (std::abs (ChipClock::position (100, 0, inc, 110, -0.5) - 1.0) < 1e-12);
}

TEST_CASE ("chip: a default-constructed chip advances by samples at the default clock and rate", "[bside][chip]")
{
    Chip c;
    CHECK (c.sampleClock().increment() == ChipClock::incrementFor (Config {}.clockHz, Config {}.internalRate));
    const auto cycles = c.advanceSamples (96000);
    // One second at 96 kHz internal is one second of chip clock (to the rounding of the increment).
    CHECK (cycles == (96000ull * c.sampleClock().increment()) >> 32);
    CHECK (cycles >= 985247);
    CHECK (cycles <= 985248);
    CHECK (c.cycle() == cycles);
}

TEST_CASE ("chip 8.4.6: Authentic rounds half away from zero; the chip accepts F = 0", "[bside][chip]")
{
    auto c = makeChip();
    c.setOscillator (0, { 1234.5, 2048.5f, 0, 0, 0 });
    CHECK (c.increment (0) == (std::uint64_t { 1235 } << 32));
    CHECK (c.pulseWidth (0) == 2049);
    c.setOscillator (0, { 0.5, 0.5f, 0, 0, 0 });
    CHECK (c.increment (0) == (std::uint64_t { 1 } << 32));
    CHECK (c.pulseWidth (0) == 1);
    c.setOscillator (0, { 0.49, -3.0f, 0, 0, 0 });
    CHECK (c.increment (0) == 0); // the 1..65535 clamp is the pitch mapping's (program engine), not the chip's
    CHECK (c.pulseWidth (0) == 0);
    FilterInput f;
    f.cutoff = 1000.5f;
    c.setFilter (f);
    CHECK (c.registerValue (0x15) == (1001 & 7));
    CHECK (c.registerValue (0x16) == (1001 >> 3));
}

TEST_CASE ("chip 8.4.1: setFilter keeps the external-input bit of 0x17 at 0", "[bside][chip]")
{
    auto c = makeChip();
    c.setFilter ({ 0.0f, 0x0f, 0x0f, 0x7f });
    CHECK (c.registerValue (0x17) == 0xf7);
    CHECK (c.registerValue (0x18) == 0x7f);
    c.setFilter ({ 0.0f, 0x03, 0x08, 0x00 });
    CHECK (c.registerValue (0x17) == 0x30);
}
