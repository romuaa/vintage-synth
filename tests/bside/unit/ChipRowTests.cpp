// The chip's digital core against the per-cycle specification docs/architecture.md 8.15 (which wins
// over the rows of 8.4.3 where they differ), one test per rule, plus the register file of 8.4.1 and the
// program-engine inputs of 8.4.6. Expected values are worked out from the specification's text (and
// its "consequences"), not from the implementation; the committed reference data (bside_trace) and the
// naive stepper (ChipStepperTests.cpp) check the same rules on every cycle.

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

/** Accumulator to 0 through the test bit (set and cleared at the same cycle, 8.15.5), control then `ctl`. */
void zeroAcc (Chip& c, int osc, int ctl = 0)
{
    wr (c, osc, reg::control, ctl | ctrl::test);
    wr (c, osc, reg::control, ctl);
}

/** One LFSR shift with the first-shift flag clear, from the specification's words (8.15.3). */
std::uint32_t lfsr (std::uint32_t r)
{
    const std::uint32_t b22 = (r >> 22) & 1u, b17 = (r >> 17) & 1u;
    return ((r << 1) & 0x7fffffu) | (b22 ^ b17);
}

std::uint32_t fillStep (std::uint32_t r) { return (r | (r << 1) | 1u) & 0x7fffffu; }

EnvState envOf (const Chip& c, int n) { return c.state().env[static_cast<std::size_t> (n)]; }
OscState oscOf (const Chip& c, int n) { return c.state().osc[static_cast<std::size_t> (n)]; }

/** Steps until `pred` holds (at most `limit` cycles); returns the cycles stepped, or limit + 1. */
template <class Pred>
std::uint64_t stepsUntil (Chip& c, Pred pred, std::uint64_t limit)
{
    for (std::uint64_t k = 1; k <= limit; ++k)
    {
        c.stepCycle();
        if (pred (c))
            return k;
    }
    return limit + 1;
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

TEST_CASE ("chip 8.15.1: power-on state (also reset())", "[bside][chip]")
{
    for (auto m : { Model::A, Model::B })
    {
        auto c = makeChip (m);
        for (int pass = 0; pass < 2; ++pass)
        {
            const auto s = c.state();
            CHECK (s.cycle == 0);
            for (std::size_t n = 0; n < 3; ++n)
            {
                const auto& o = s.osc[n];
                CHECK (o.acc == 0x555555);
                CHECK (o.accFraction == 0);
                CHECK (o.noise == 0x7ffffe);
                CHECK (o.noiseLatch == 0x7ffffe);
                CHECK (o.noiseOutput == 0xfe0); // the output of the power-on register (8.15.1 is silent; docs/decisions.md)
                CHECK (o.firstShift);
                CHECK (o.noisePipeline == 0);
                CHECK_FALSE (o.rise);
                CHECK (o.fillTimer == 0);
                CHECK (o.fadeTimer == 0);
                CHECK (o.output == 0);
                CHECK (o.readValue == 0);
                CHECK (o.pulseHigh);
                CHECK (o.readLatch == 0x555);
                const auto& e = s.env[n];
                CHECK (e.counter == 0xaa);
                CHECK (e.phase == 2);
                CHECK (e.target == 2);
                CHECK (e.phasePipeline == 0);
                CHECK (e.stepPipeline == 0);
                CHECK (e.dividerPipeline == 0);
                CHECK (e.position == 0);
                CHECK (e.resetPending);
                CHECK (e.divider == 0);
                CHECK (e.dividerPeriod == 1);
                CHECK (e.pendingPeriod == 0);
                CHECK_FALSE (e.frozen);
                CHECK (e.readSample == 0);
                CHECK (e.compare == 8); // release rate 0
            }
            for (auto v : s.registers)
                CHECK (v == 0);
            CHECK (c.read (0x1b) == 0);
            CHECK (c.read (0x1c) == 0);
            // Disturb everything, then reset(): the same power-on state.
            for (int r = 0; r < 0x19; ++r)
                c.write (static_cast<std::uint8_t> (r), static_cast<std::uint8_t> (0x5a ^ r));
            c.advanceCycles (123457);
            c.reset();
        }
    }
}

TEST_CASE ("chip 8.15.1: noise selected before any shift shows the power-on register's output", "[bside][chip]")
{
    for (auto m : { Model::A, Model::B })
    {
        auto c = makeChip (m);
        wr (c, 2, reg::control, ctrl::noise); // F = 0: no shift, no fill, no write-back
        c.advanceCycles (3);
        CHECK (c.output (2) == 0xfe0);
        CHECK (c.read (0x1b) == 0xfe);
    }
}

TEST_CASE ("chip 8.4.1: the register file", "[bside][chip]")
{
    auto c = makeChip();
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

TEST_CASE ("chip 8.15.1/8.15.5: a write at cycle c applies between steps c and c+1; F and PW from the next step", "[bside][chip]")
{
    auto c = makeChip();
    c.advanceCycles (10);
    CHECK (c.accumulator (0) == 0x555555); // F = 0
    setFreq (c, 0, 0x0123);
    CHECK (c.accumulator (0) == 0x555555); // the write itself moves nothing
    c.stepCycle();
    CHECK (c.accumulator (0) == 0x555555 + 0x123);
    // A pulse-width write is used by the comparison at the end of the next step.
    auto d = makeChip();
    zeroAcc (d, 2);
    setPw (d, 2, 0x001);
    CHECK (oscOf (d, 2).pulseHigh); // power-on value, not recomputed by the write
    d.stepCycle();
    CHECK_FALSE (oscOf (d, 2).pulseHigh); // acc bits 23..12 = 0 < 1
}

TEST_CASE ("chip 8.15.2 (a): the accumulator adds F every step and wraps at 2^24", "[bside][chip]")
{
    auto c = makeChip();
    zeroAcc (c, 0);
    zeroAcc (c, 1);
    setFreq (c, 0, 0x1234);
    setFreq (c, 1, 0xffff);
    c.advanceCycles (1);
    CHECK (c.accumulator (0) == 0x1234);
    CHECK (c.accumulator (1) == 0xffff);
    CHECK (c.accumulator (2) == 0x555555);
    c.advanceCycles (999);
    CHECK (c.accumulator (0) == (1000u * 0x1234u) % (1u << 24));
    CHECK (c.accumulator (1) == (1000u * 0xffffu) % (1u << 24));

    // Output frequency = F x clock / 2^24: count top-bit rises over a second of the 50 Hz clock.
    auto d = makeChip();
    zeroAcc (d, 0);
    const std::uint32_t f = 7382; // about 433.5 Hz
    setFreq (d, 0, f);
    std::uint64_t rises = 0;
    for (int i = 0; i < 985248; ++i)
    {
        d.stepCycle();
        rises += oscOf (d, 0).rise ? 1u : 0u;
    }
    const std::uint64_t expected = (985248ull * f + (1ull << 23)) / (1ull << 24);
    CHECK (rises == expected);
}

TEST_CASE ("chip 8.15.2 (a)/8.15.3: the noise register shifts 2 steps after a bit-19 rise, from its latch", "[bside][chip]")
{
    // F = 0x8000 from acc 0: bit 19 rises in step 16 (16 x 2^15 = 2^19), then every 32 steps.
    for (auto m : { Model::A, Model::B })
    {
        auto c = makeChip (m);
        setFreq (c, 0, 0x8000);
        zeroAcc (c, 0, ctrl::noise);
        std::uint32_t r = c.noiseRegister (0);
        c.advanceCycles (15);
        CHECK (c.noiseRegister (0) == r);
        c.stepCycle(); // 16: rise, pipeline 2
        CHECK (oscOf (c, 0).noisePipeline == 2);
        CHECK (c.noiseRegister (0) == r);
        c.stepCycle(); // 17: pipeline 1, latch := register, first-shift flag cleared
        CHECK (oscOf (c, 0).noisePipeline == 1);
        CHECK (oscOf (c, 0).noiseLatch == r);
        CHECK_FALSE (oscOf (c, 0).firstShift);
        CHECK (c.noiseRegister (0) == r);
        c.stepCycle(); // 18: the shift
        r = lfsr (r);
        CHECK (c.noiseRegister (0) == r);
        CHECK (oscOf (c, 0).noiseOutput == wave::noise (r));
        for (int k = 0; k < 300; ++k)
        {
            c.advanceCycles (31);
            CHECK (c.noiseRegister (0) == r); // no shift between
            c.advanceCycles (1);
            r = lfsr (r);
            REQUIRE (c.noiseRegister (0) == r);
        }
        // The register is clocked whatever waveform is selected (here none).
        auto d = makeChip (m);
        setFreq (d, 1, 0x8000);
        zeroAcc (d, 1);
        const auto r0 = d.noiseRegister (1);
        d.advanceCycles (18);
        CHECK (d.noiseRegister (1) == lfsr (r0));
    }
}

TEST_CASE ("chip 8.15.2 (a): a bit-19 rise restarts a running noise pipeline (its shift is lost)", "[bside][chip]")
{
    // Smooth F just below 2^19: bit 19 rises every 2 steps, so the pipeline never reaches 0.
    Chip c;
    Config cfg;
    cfg.authentic = false;
    c.prepare (cfg);
    c.setOscillator (0, { 524287.9, 0.0f, ctrl::noise | ctrl::test, 0, 0 });
    c.setOscillator (0, { 524287.9, 0.0f, ctrl::noise, 0, 0 });
    const auto r = c.noiseRegister (0);
    c.advanceCycles (4000);
    CHECK (c.noiseRegister (0) == r);
    // At F = 2^18 from 0 bit 19 rises in steps 2, 6, 10, ... and every rise shifts 2 steps later (4, 8, ...).
    Chip d;
    d.prepare (cfg);
    d.setOscillator (0, { 262144.0, 0.0f, ctrl::noise | ctrl::test, 0, 0 });
    d.setOscillator (0, { 262144.0, 0.0f, ctrl::noise, 0, 0 });
    REQUIRE (d.accumulator (0) == 0);
    auto r2 = d.noiseRegister (0);
    d.advanceCycles (4000);
    for (int k = 0; k < 1000; ++k)
        r2 = lfsr (r2);
    CHECK (d.noiseRegister (0) == r2);
}

TEST_CASE ("chip 8.15.2 (a)/8.15.5: the test bit holds the accumulator and fills the noise register with ones", "[bside][chip]")
{
    for (auto m : { Model::A, Model::B })
    {
        const auto t = timingFor (m);
        CHECK (t.testHold == (m == Model::A ? 50000u : 986000u));
        CHECK (t.fillInterval == (m == Model::A ? 15000u : 314300u));
        // Set while the noise pipeline runs: it stops (no latch, no shift).
        auto p = makeChip (m);
        setFreq (p, 0, 0x8000);
        zeroAcc (p, 0);
        p.advanceCycles (16); // bit 19 rose in step 16
        REQUIRE (oscOf (p, 0).noisePipeline == 2);
        const auto rp = p.noiseRegister (0);
        wr (p, 0, reg::control, ctrl::test);
        CHECK (oscOf (p, 0).noisePipeline == 0);
        p.advanceCycles (10);
        CHECK (p.noiseRegister (0) == rp);

        auto c = makeChip (m);
        setFreq (c, 0, 0x4321);
        wr (c, 0, reg::control, ctrl::noise);
        c.advanceCycles (5000);
        REQUIRE (c.accumulator (0) != 0);
        std::uint32_t r = c.noiseRegister (0);
        REQUIRE (r != 0x7fffff);
        wr (c, 0, reg::control, ctrl::noise | ctrl::test);
        CHECK (c.accumulator (0) == 0);              // at the write
        CHECK (oscOf (c, 0).noisePipeline == 0);
        CHECK (oscOf (c, 0).noiseLatch == r);        // latch := register
        CHECK (oscOf (c, 0).fillTimer == t.testHold);
        CHECK (c.noiseRegister (0) == r);            // the register is not reset
        c.advanceCycles (t.testHold - 1);
        CHECK (c.accumulator (0) == 0);              // held
        CHECK (c.noiseRegister (0) == r);
        CHECK (oscOf (c, 0).firstShift);             // set by every step under the test bit
        CHECK_FALSE (oscOf (c, 0).rise);
        c.stepCycle();
        r = fillStep (r);
        CHECK (c.noiseRegister (0) == r);
        CHECK (oscOf (c, 0).noiseLatch == r);
        CHECK (oscOf (c, 0).noiseOutput == wave::noise (r));
        int fills = 1;
        while (r != 0x7fffff)
        {
            c.advanceCycles (t.fillInterval - 1);
            CHECK (c.noiseRegister (0) == r);
            c.stepCycle();
            r = fillStep (r);
            REQUIRE (c.noiseRegister (0) == r);
            ++fills;
        }
        CHECK (fills <= 23);
        CHECK (oscOf (c, 0).fillTimer == 0); // stopped at all ones
        c.advanceCycles (3 * t.fillInterval);
        CHECK (c.noiseRegister (0) == 0x7fffff);
        // Pulse reads high under the test bit.
        wr (c, 0, reg::control, ctrl::pulse | ctrl::test);
        setPw (c, 0, 0xfff);
        c.advanceCycles (2); // the comparison of acc 0 with 0xfff is low; under the test bit the pulse reads high
        CHECK_FALSE (oscOf (c, 0).pulseHigh);
        CHECK (c.output (0) == 0xfff);
        // Released: the accumulator runs from 0.
        wr (c, 0, reg::control, ctrl::noise);
        c.advanceCycles (3);
        CHECK (c.accumulator (0) == 3u * 0x4321u);
    }
}

TEST_CASE ("chip 8.15.5/8.15.3: clearing the test bit shifts the register at once, with the first-shift flag", "[bside][chip]")
{
    for (auto m : { Model::A, Model::B })
    {
        // Steps under the test bit set the flag: new bit 0 = (bit 22 OR 1) XOR bit 17 = NOT bit 17.
        auto c = makeChip (m);
        setFreq (c, 0, 0x2345);
        c.advanceCycles (20000);
        wr (c, 0, reg::control, ctrl::test);
        const auto latch = oscOf (c, 0).noiseLatch;
        c.advanceCycles (10);
        wr (c, 0, reg::control, 0);
        const std::uint32_t expected = ((latch << 1) & 0x7fffffu) | (((latch >> 17) & 1u) ^ 1u);
        CHECK (c.noiseRegister (0) == expected);
        CHECK (oscOf (c, 0).noiseOutput == wave::noise (expected));
        // Set and cleared in the same cycle after a latch step cleared the flag: a plain LFSR shift.
        auto d = makeChip (m);
        setFreq (d, 0, 0x8000);
        zeroAcc (d, 0);
        d.advanceCycles (17); // rise at 16, latch at 17: flag clear
        REQUIRE_FALSE (oscOf (d, 0).firstShift);
        const auto reg0 = d.noiseRegister (0);
        zeroAcc (d, 0);
        CHECK (d.noiseRegister (0) == lfsr (reg0));
    }
}

TEST_CASE ("chip 8.15.3: when a shift first writes the output into the latch's tap bits", "[bside][chip]")
{
    // The exceptions of 8.15.3, restated independently over all 256 pairs.
    for (auto m : { Model::A, Model::B })
        for (int o = 0; o < 16; ++o)
            for (int n = 0; n < 16; ++n)
            {
                bool applies = true;
                if (o <= 8 || n < 8 || (n == 8 && o != 0xf) || o == 0xc || n == 0xc)
                    applies = false;
                if (m == Model::A && (((o & 3) == 1 && (n & 3) == 2) || ((o & 3) == 2 && (n & 3) == 1)))
                    applies = false;
                INFO ("old " << o << " new " << n);
                CHECK (wave::shiftWritesBack (m, static_cast<std::uint8_t> (o), static_cast<std::uint8_t> (n)) == applies);
            }
    // In the chip: noise + triangle under the test bit, released to the same bits. The output (with
    // the test bit: triangle of acc 0 = 0, AND noise) clears every tap bit of the latch before the shift.
    for (auto m : { Model::A, Model::B })
    {
        auto c = makeChip (m);
        wr (c, 0, reg::control, ctrl::noise | ctrl::triangle | ctrl::test);
        c.stepCycle();
        REQUIRE (c.output (0) == 0);
        const auto latch = oscOf (c, 0).noiseLatch;
        wr (c, 0, reg::control, ctrl::noise | ctrl::triangle);
        const std::uint32_t l = latch & ~wave::kTapBits;
        CHECK (c.noiseRegister (0) == (((l << 1) & 0x7fffffu) | (((l >> 17) & 1u) ^ 1u)));
        // Released to noise + pulse (0xc): no write-back, the latch shifts as it is.
        auto d = makeChip (m);
        wr (d, 0, reg::control, ctrl::noise | ctrl::triangle | ctrl::test);
        d.stepCycle();
        const auto latch2 = oscOf (d, 0).noiseLatch;
        wr (d, 0, reg::control, ctrl::noise | ctrl::pulse);
        CHECK (d.noiseRegister (0) == (((latch2 << 1) & 0x7fffffu) | ((((latch2 >> 22) | 1u) ^ (latch2 >> 17)) & 1u)));
    }
}

TEST_CASE ("chip 8.15.2 (c): triangle, saw, pulse and noise shapes; ring folds with XOR NOT the source's top bit", "[bside][chip]")
{
    // Index and shapes by hand.
    CHECK (wave::index (0x123456, 0, 0) == 0x123);
    CHECK (wave::triangle (0x000) == 0x000);
    CHECK (wave::triangle (0x400) == 0x800);
    CHECK (wave::triangle (0x7ff) == 0xffe); // bit 0 is always 0
    CHECK (wave::triangle (0x800) == 0xffe);
    CHECK (wave::triangle (0xc00) == 0x7fe);
    CHECK (wave::triangle (0xfff) == 0x000);
    CHECK (wave::saw (0xabc) == 0xabc);
    CHECK (wave::shape (combined::fittedTables (Model::A), wave::kPulse, 0x123, 0) == 0xfff);
    CHECK (wave::shape (combined::fittedTables (Model::B), wave::kNoise, 0x123, 0) == 0xfff);
    CHECK (wave::shape (combined::fittedTables (Model::B), wave::kTri | wave::kNoise, 0x123, 0) == wave::triangle (0x123));
    CHECK (wave::shape (combined::fittedTables (Model::A), wave::kSaw | wave::kNoise, 0x123, 0) == 0x123);
    // Ring (saw clear): bit 11 := own bit 23 XOR NOT source bit 23.
    CHECK (wave::index (0x400000, ctrl::ring | ctrl::triangle, 0x800000) == 0x400); // 0 ^ !1 = 0
    CHECK (wave::index (0x400000, ctrl::ring | ctrl::triangle, 0x000000) == 0xc00); // 0 ^ !0 = 1
    CHECK (wave::index (0xc00000, ctrl::ring | ctrl::triangle, 0x800000) == 0xc00); // 1 ^ !1 = 1
    CHECK (wave::index (0xc00000, ctrl::ring | ctrl::triangle, 0x7fffff) == 0x400); // 1 ^ !0 = 0
    CHECK (wave::index (0x400000, ctrl::ring | ctrl::saw, 0x000000) == 0x400);      // saw: no ring

    // In the chip: oscillator 3's ring source is oscillator 2 (its output comes first in the step).
    auto c = makeChip();
    zeroAcc (c, 1);
    zeroAcc (c, 2);
    setFreq (c, 1, 0x0777);
    setFreq (c, 2, 0x1301);
    wr (c, 2, reg::control, ctrl::triangle | ctrl::ring);
    for (int i = 0; i < 3000; ++i)
    {
        c.advanceCycles (29);
        const std::uint32_t acc = c.accumulator (2), src = c.accumulator (1);
        const std::uint32_t bit11 = ((acc >> 23) ^ ~(src >> 23)) & 1u;
        const std::uint32_t ix = ((acc >> 12) & 0x7ffu) | (bit11 << 11);
        const std::uint32_t folded = (ix & 0x800u) != 0 ? (ix ^ 0xfffu) : ix;
        CHECK (c.output (2) == ((folded << 1) & 0xfffu));
    }
    // Saw with ring: no ring modulation.
    wr (c, 2, reg::control, ctrl::saw | ctrl::ring);
    c.stepCycle();
    CHECK (c.output (2) == ((c.accumulator (2) >> 12) & 0xfff));
}

TEST_CASE ("chip 8.15.2 (c): oscillator 1 reads its ring source 3 before 3's pull-down, 2 reads 1 after 1's", "[bside][chip]")
{
    // Model A, oscillator 3 (and in the second part 1): saw + pulse with PW 0xfff. The pulse comparison is
    // low below 0xfff000, so the output is 0 there and its top bit is pulled down in every step where the
    // add raised it. The ring-modulated triangle of the next oscillator shows which value it saw.
    for (int src = 0; src < 2; ++src)
    {
        const int s = src == 0 ? 2 : 0; // ring source
        const int t = src == 0 ? 0 : 1; // ring-modulated triangle
        auto c = makeChip (Model::A);
        setFreq (c, s, 0xac01);
        setPw (c, s, 0xfff);
        zeroAcc (c, s, ctrl::saw | ctrl::pulse);
        zeroAcc (c, t, ctrl::triangle | ctrl::ring);
        int risesSeen = 0;
        for (int k = 0; k < 20000; ++k)
        {
            const std::uint32_t sBefore = c.accumulator (s);
            c.stepCycle();
            const std::uint32_t afterAdd = (sBefore + 0xac01u) & 0xffffffu;
            const std::uint32_t afterPullDown = c.accumulator (s);
            REQUIRE ((afterPullDown & 0x800000u) == 0); // always pulled down below 0xfff000
            const std::uint32_t seen = src == 0 ? afterAdd : afterPullDown;
            risesSeen += (afterAdd & 0x800000u) != 0 ? 1 : 0;
            const std::uint32_t acc = c.accumulator (t);
            const std::uint32_t bit11 = ((acc >> 23) ^ ~(seen >> 23)) & 1u;
            const std::uint32_t ix = ((acc >> 12) & 0x7ffu) | (bit11 << 11);
            const std::uint32_t folded = (ix & 0x800u) != 0 ? (ix ^ 0xfffu) : ix;
            REQUIRE (c.output (t) == ((folded << 1) & 0xfffu));
        }
        CHECK (risesSeen > 100);
    }
}

TEST_CASE ("chip 8.15.2 (c): pulse uses the comparison of the previous step; the comparison is made last", "[bside][chip]")
{
    auto c = makeChip();
    zeroAcc (c, 2);
    setFreq (c, 2, 0x0405);
    setPw (c, 2, 0x0a00);
    wr (c, 2, reg::control, ctrl::pulse);
    for (int i = 0; i < 20000; ++i)
    {
        const std::uint32_t before = c.accumulator (2);
        const bool prevHigh = oscOf (c, 2).pulseHigh; // made at the end of the previous step (power-on: high)
        if (i > 0)
            REQUIRE (prevHigh == (((before >> 12) & 0xfff) >= 0xa00));
        c.stepCycle();
        REQUIRE (c.output (2) == (prevHigh ? 0xfff : 0));
        REQUIRE (oscOf (c, 2).pulseHigh == (((c.accumulator (2) >> 12) & 0xfff) >= 0xa00));
    }
    // The power-on comparison is high: the first pulse step after power-on outputs 0xfff.
    auto d = makeChip();
    setPw (d, 2, 0xfff);
    wr (d, 2, reg::control, ctrl::pulse);
    d.stepCycle();
    CHECK (d.output (2) == 0xfff);
    d.stepCycle();
    CHECK (d.output (2) == 0);
}

TEST_CASE ("chip 8.15.2 (c)/(d): Model A pulls a combined saw's top bit down when the output's bit 11 is 0; it never syncs", "[bside][chip]")
{
    // Saw + pulse with PW 0xfff: the pulse mask is 0 below 0xfff000, so the output is 0 whatever the
    // combined-waveform table holds. Model A clears the top bit and the rise flag; Model B does not.
    for (auto m : { Model::A, Model::B })
    {
        auto c = makeChip (m);
        setFreq (c, 0, 0x1000);
        setPw (c, 0, 0xfff);
        zeroAcc (c, 0, ctrl::saw | ctrl::pulse);
        setFreq (c, 1, 0x0100);
        zeroAcc (c, 1, ctrl::sync);
        c.advanceCycles (2048); // oscillator 1's add raises bit 23 in step 2048
        if (m == Model::A)
        {
            CHECK (c.accumulator (0) == 0);       // 0x800000 pulled down
            CHECK_FALSE (oscOf (c, 0).rise);
            CHECK (c.accumulator (1) == 2048u * 0x100u); // no sync
        }
        else
        {
            CHECK (c.accumulator (0) == 0x800000);
            CHECK (c.accumulator (1) == 0); // synced
        }
        // Saw alone never pulls down (its bit 11 is the top bit).
        auto d = makeChip (m);
        setFreq (d, 0, 0x1000);
        zeroAcc (d, 0, ctrl::saw);
        d.advanceCycles (2048);
        CHECK (d.accumulator (0) == 0x800000);
    }
}

TEST_CASE ("chip 8.15.2 (c) step 5: read value, Model A the output, Model B triangle/saw through a latch", "[bside][chip]")
{
    // Model A: 0x1B shows the output of the same step.
    auto a = makeChip (Model::A);
    setFreq (a, 2, 0x0d0b);
    zeroAcc (a, 2, ctrl::saw);
    for (int i = 0; i < 2000; ++i)
    {
        a.advanceCycles (37);
        REQUIRE (a.output (2) == ((a.accumulator (2) >> 12) & 0xfff));
        REQUIRE (a.read (0x1b) == ((a.accumulator (2) >> 16) & 0xff));
    }
    // Model B: the first saw step after power-on reads the latch's power-on value 0x555; then each saw step
    // reads the previous saw step's shape.
    auto b = makeChip (Model::B);
    setFreq (b, 2, 0x0d0b);
    wr (b, 2, reg::control, ctrl::saw);
    b.stepCycle();
    CHECK (b.read (0x1b) == 0x55);
    for (int i = 0; i < 2000; ++i)
    {
        const auto prevShape = b.output (2);
        b.stepCycle();
        REQUIRE (b.read (0x1b) == (prevShape >> 4));
    }
    // Pulse (no triangle/saw): the output, and the latch keeps the last saw shape.
    const auto lastSaw = b.output (2);
    setPw (b, 2, 0);
    wr (b, 2, reg::control, ctrl::pulse);
    CHECK (b.read (0x1b) == (lastSaw >> 4)); // a control write shows only in the next step
    b.advanceCycles (100);
    CHECK (b.read (0x1b) == 0xff);
    CHECK (oscOf (b, 2).readLatch == lastSaw);
    wr (b, 2, reg::control, ctrl::saw);
    b.stepCycle();
    CHECK (b.read (0x1b) == (lastSaw >> 4)); // the last saw shape before the pulse
    // Oscillators 1 and 2 do not show in the read registers; other addresses read 0; modulo 32.
    auto d = makeChip();
    setFreq (d, 0, 0x2222);
    wr (d, 0, reg::control, ctrl::saw | ctrl::gate);
    d.advanceCycles (5000);
    CHECK (d.read (0x1b) == 0);
    CHECK (d.read (0x00) == 0);
    CHECK (d.read (0x19) == 0);
    CHECK (a.read (0x1b + 0x20) == a.read (0x1b));
}

TEST_CASE ("chip 8.15.2 (c): a deselected output holds, then fades after the hold time, one step per interval", "[bside][chip]")
{
    for (auto m : { Model::A, Model::B })
    {
        const auto t = timingFor (m);
        CHECK (t.fadeHold == (m == Model::A ? 54000u : 800000u));
        CHECK (t.fadeInterval == (m == Model::A ? 1400u : 50000u));
        auto c = makeChip (m);
        setFreq (c, 2, 0x0101);
        zeroAcc (c, 2, ctrl::saw);
        c.advanceCycles (0x7f0000 / 0x101); // the saw near 0x7f0
        std::uint16_t out = c.output (2);
        REQUIRE (out != 0);
        wr (c, 2, reg::control, 0);
        CHECK (oscOf (c, 2).fadeTimer == t.fadeHold);
        // The step after the deselect holds the output (and the read value takes it on both models).
        c.stepCycle();
        CHECK (c.output (2) == out);
        CHECK (oscOf (c, 2).readValue == out);
        c.advanceCycles (t.fadeHold - 2);
        CHECK (c.output (2) == out); // still held after hold - 1 steps
        int steps = 0;
        while (out != 0)
        {
            c.stepCycle();
            out = static_cast<std::uint16_t> (out & (out >> 1));
            REQUIRE (c.output (2) == out);
            REQUIRE (oscOf (c, 2).readValue == out);
            ++steps;
            if (out != 0)
            {
                c.advanceCycles (t.fadeInterval - 1);
                REQUIRE (c.output (2) == out);
            }
        }
        CHECK (steps <= 12);
        CHECK (oscOf (c, 2).fadeTimer == 0);
        // Selecting a waveform again computes the output at once.
        wr (c, 2, reg::control, ctrl::saw);
        c.stepCycle();
        CHECK (c.output (2) == ((c.accumulator (2) >> 12) & 0xfff));
    }
}

TEST_CASE ("chip 8.15.2 (c) step 6: noise with another waveform writes the output back and can lock up", "[bside][chip]")
{
    for (auto model : { Model::A, Model::B })
    {
        auto c = makeChip (model);
        setFreq (c, 0, 0x2345);
        zeroAcc (c, 0, ctrl::noise | ctrl::saw);
        for (int i = 0; i < 2000; ++i)
        {
            c.stepCycle();
            const auto o = oscOf (c, 0);
            if (o.noisePipeline != 1)
            {
                REQUIRE ((wave::noise (o.noise) & ~o.output & 0xff0) == 0);
                REQUIRE ((o.noiseOutput & ~o.output) == 0);
            }
            else
                REQUIRE (o.noiseOutput == o.output); // the pipeline is in its latch step: noise output := out
        }
        c.advanceCycles (2000000);
        CHECK (c.noiseRegister (0) == 0); // locked up
        wr (c, 0, reg::control, ctrl::noise);
        c.advanceCycles (100000);
        CHECK (c.noiseRegister (0) == 0); // an all-zero register stays zero: silent
        {
            // A short test pulse (shorter than the hold time): the shift at its end feeds the first-shift
            // flag in, (0 OR 1) XOR 0 = 1.
            Chip k = c;
            wr (k, 0, reg::control, ctrl::noise | ctrl::test);
            k.advanceCycles (10);
            wr (k, 0, reg::control, ctrl::noise);
            CHECK (k.noiseRegister (0) == 1);
        }
        // The test bit refills it (from 0: 23 fill steps), then noise runs again.
        const auto t = timingFor (model);
        wr (c, 0, reg::control, ctrl::noise | ctrl::test);
        c.advanceCycles (t.testHold + 22ull * t.fillInterval);
        CHECK (c.noiseRegister (0) == 0x7fffff);
        wr (c, 0, reg::control, ctrl::noise);
        c.advanceCycles (100000);
        CHECK (c.noiseRegister (0) != 0);
        CHECK (c.noiseRegister (0) != 0x7fffff);
    }
    // No write-back while the test bit is held: the noise output takes the output, the register stays.
    auto d = makeChip();
    setFreq (d, 1, 0x2345);
    wr (d, 1, reg::control, ctrl::noise | ctrl::saw | ctrl::test);
    const auto r = d.noiseRegister (1);
    d.advanceCycles (5000);
    CHECK (d.noiseRegister (1) == r);
    CHECK (oscOf (d, 1).noiseOutput == d.output (1));
    // Without noise there is no write-back.
    auto e = makeChip();
    setFreq (e, 2, 0x0fed);
    zeroAcc (e, 2, ctrl::saw | ctrl::triangle | ctrl::pulse);
    const auto before = e.noiseRegister (2);
    e.advanceCycles (200); // bit 19 rises once (step 129), the shift follows in step 131
    CHECK (e.noiseRegister (2) == lfsr (before));
}

TEST_CASE ("chip 8.15.2 (c) step 6: no write-back into the register in the noise pipeline's latch step", "[bside][chip]")
{
    // Noise alone, F = 0x8000 from 0: bit 19 rises in step 16. Switched to noise + saw right after it, the
    // next step is the pipeline's latch step: the noise output takes the output, the register is kept.
    // One step later (pipeline 0, a shift) the write-back ANDs the register.
    for (auto m : { Model::A, Model::B })
    {
        auto c = makeChip (m);
        setFreq (c, 0, 0x8000);
        zeroAcc (c, 0, ctrl::noise);
        c.advanceCycles (16);
        REQUIRE (oscOf (c, 0).noisePipeline == 2);
        const auto reg0 = c.noiseRegister (0);
        wr (c, 0, reg::control, ctrl::noise | ctrl::saw);
        c.stepCycle();
        auto o = oscOf (c, 0);
        REQUIRE (o.noisePipeline == 1);
        CHECK (o.noise == reg0);
        CHECK (o.noiseOutput == o.output);
        REQUIRE ((wave::noise (reg0) & ~o.output & 0xff0) != 0); // an AND would have changed the register
        const auto latchOut = o.output;
        c.stepCycle();
        o = oscOf (c, 0);
        // The shift first writes the previous output into the latch's tap bits (8.15.3, noise + saw), then
        // (c) step 6 ANDs this step's output into the register.
        const std::uint32_t latch = (reg0 & ~wave::kTapBits) | wave::taps (latchOut);
        CHECK (o.noise == (lfsr (latch) & wave::noiseKeepMask (o.output)));
    }
}

TEST_CASE ("chip 8.15.2 (d): sync resets the target in the step of the source's top-bit rise", "[bside][chip]")
{
    // Pairs: source -> target 1 -> 2, 2 -> 3, 3 -> 1 (0-based 0 -> 1, 1 -> 2, 2 -> 0).
    for (int src = 0; src < 3; ++src)
    {
        const int dst = (src + 1) % 3, other = (src + 2) % 3;
        auto c = makeChip();
        for (int n = 0; n < 3; ++n)
            zeroAcc (c, n);
        setFreq (c, src, 0x1000); // top bit rises in step 2048
        setFreq (c, dst, 0x0100);
        wr (c, dst, reg::control, ctrl::sync);
        c.advanceCycles (2047);
        CHECK (c.accumulator (dst) == 2047u * 0x100u);
        c.advanceCycles (1);
        CHECK (c.accumulator (dst) == 0);
        c.advanceCycles (1);
        CHECK (c.accumulator (dst) == 0x100);
        CHECK (c.accumulator (other) == 0);
        // Without the target's sync bit there is no reset.
        auto d = makeChip();
        zeroAcc (d, src);
        zeroAcc (d, dst);
        setFreq (d, src, 0x1000);
        setFreq (d, dst, 0x0100);
        d.advanceCycles (2048);
        CHECK (d.accumulator (dst) == 2048u * 0x100u);
    }
    // A source held by the test bit never syncs (our chip clears its rise flag, 8.15.6).
    auto e = makeChip();
    setFreq (e, 0, 0x1000);
    wr (e, 0, reg::control, ctrl::test);
    zeroAcc (e, 1, ctrl::sync);
    setFreq (e, 1, 0x0100);
    e.advanceCycles (5000);
    CHECK (e.accumulator (1) == 5000u * 0x100u);
}

TEST_CASE ("chip 8.15.2 (d): a source reset by its own source in the same step does not reset its target", "[bside][chip]")
{
    for (int a = 0; a < 3; ++a)
    {
        const int b = (a + 1) % 3, c = (a + 2) % 3;
        auto chip = makeChip();
        for (int n = 0; n < 3; ++n)
            zeroAcc (chip, n);
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
        chip.stepCycle();
        CHECK (chip.accumulator (b) == 0x1000);
        CHECK (chip.accumulator (c) == 2049u * 0x100u);
    }
    // All three rise together with every sync bit set: nobody is reset.
    for (int round = 0; round < 2; ++round)
    {
        auto chip = makeChip();
        for (int n = 0; n < 3; ++n)
        {
            setFreq (chip, n, 0x1000);
            zeroAcc (chip, n, ctrl::sync | ctrl::saw);
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
        zeroAcc (chip, n, ctrl::sync);
    chip.advanceCycles (2048);
    CHECK (chip.accumulator (0) == 0x800000);
    CHECK (chip.accumulator (1) == 0);
    CHECK (chip.accumulator (2) == 2048u * 0x300u);
}

TEST_CASE ("chip 8.15.4: rate counter positions, match positions and the read sample", "[bside][chip]")
{
    for (int i = 0; i < 16; ++i)
        CHECK (kRateMatch[static_cast<std::size_t> (i)] + 1 == kRatePeriods[static_cast<std::size_t> (i)]);
    // From power-on (release, rate 0, match 8, a reset pending): reset in step 1 (position 0 -> 1), then
    // 2 .. 8, the match sets the pending flag in step 9, the next reset in step 10: period 9.
    auto c = makeChip();
    c.stepCycle();
    CHECK (envOf (c, 0).position == 1);
    CHECK_FALSE (envOf (c, 0).resetPending);
    c.advanceCycles (7);
    CHECK (envOf (c, 0).position == 8);
    CHECK_FALSE (envOf (c, 0).resetPending);
    c.stepCycle();
    CHECK (envOf (c, 0).position == 8);
    CHECK (envOf (c, 0).resetPending);
    c.stepCycle();
    CHECK (envOf (c, 0).position == 1);
    // Period 1 divider: the release step comes 2 steps after each reset (steps 3, 12, 21, ...).
    auto d = makeChip();
    d.advanceCycles (2);
    CHECK (d.envelope (0) == 0xaa);
    d.stepCycle();
    CHECK (d.envelope (0) == 0xa9);
    d.advanceCycles (8);
    CHECK (d.envelope (0) == 0xa9);
    d.stepCycle();
    CHECK (d.envelope (0) == 0xa8);
    // The read sample (0x1C) is the counter at the start of the step: one step behind.
    CHECK (d.read (0x1c) == 0xa9);
    d.stepCycle();
    CHECK (d.read (0x1c) == 0xa8);
}

TEST_CASE ("chip 8.15.4: attack steps every period, 2 steps after each reset; 0xff -> decay/sustain 3 steps later", "[bside][chip]")
{
    for (int nib : { 0, 1, 4, 9 })
    {
        const std::uint64_t p = kRatePeriods[static_cast<std::size_t> (nib)];
        auto c = makeChip();
        wr (c, 0, reg::attackDecay, (nib << 4) | 0x0f);
        wr (c, 0, reg::sustainRelease, 0xf0);
        wr (c, 0, reg::control, ctrl::gate);
        REQUIRE (stepsUntil (c, [] (const Chip& x) { return x.envelopePhase (0) == EnvPhase::attack; }, 100) <= 3);
        // Once in attack, every reset is followed by a step 2 steps later.
        std::uint64_t lastReset = 0, steps = 0, cycleAtFf = 0;
        std::uint8_t prev = c.envelope (0);
        const std::uint8_t start = prev;
        while (c.envelope (0) != 0xff && c.cycle() < 300 * p + 100)
        {
            const bool pending = envOf (c, 0).resetPending;
            c.stepCycle();
            if (pending && ! envOf (c, 0).resetPending)
                lastReset = c.cycle();
            if (c.envelope (0) != prev)
            {
                REQUIRE (c.envelope (0) == static_cast<std::uint8_t> (prev + 1));
                REQUIRE (c.cycle() == lastReset + 2);
                ++steps;
                prev = c.envelope (0);
            }
        }
        cycleAtFf = c.cycle();
        CHECK (steps == 0xffu - start);
        CHECK (stepsUntil (c, [] (const Chip& x) { return x.envelopePhase (0) == EnvPhase::decaySustain; }, 10) == 3);
        CHECK (c.cycle() == cycleAtFf + 3);
        // Between steps: one rate period (match + pending + reset).
        (void) p;
    }
}

TEST_CASE ("chip 8.15.4: decay and release use the divider state; the counter freezes at 0", "[bside][chip]")
{
    // Release at rate 0 from 0xaa: period 1 above 0x5d (step every 9 steps), period 2 once 0x5d is taken
    // (effective from the next step: every 18), ..., 30 from 0x06; frozen at 0.
    auto c = makeChip();
    std::uint8_t prev = c.envelope (0);
    std::uint64_t lastChange = 0;
    int gaps9 = 0, gaps18 = 0, gaps270 = 0;
    while (c.envelope (0) != 0)
    {
        c.stepCycle();
        if (c.envelope (0) != prev)
        {
            REQUIRE (c.envelope (0) == prev - 1);
            const std::uint64_t gap = c.cycle() - lastChange;
            if (prev > 0x5d && lastChange != 0)
                REQUIRE (gap == 9);
            if (prev <= 0x5d && prev > 0x36 && prev != 0x5d)
                REQUIRE (gap == 18);
            if (prev <= 0x06 && prev != 0x06)
                REQUIRE (gap == 270);
            gaps9 += gap == 9;
            gaps18 += gap == 18;
            gaps270 += gap == 270;
            lastChange = c.cycle();
            prev = c.envelope (0);
        }
        REQUIRE (c.cycle() < 2000000);
    }
    CHECK (gaps9 > 70);
    CHECK (gaps18 > 30);
    CHECK (gaps270 >= 5);
    CHECK (envOf (c, 0).frozen);
    c.advanceCycles (1000000);
    CHECK (c.envelope (0) == 0); // never wraps
    CHECK (envOf (c, 0).divider == 0); // frozen: the divider does not count
}

TEST_CASE ("chip 8.15.4: sustain level = nibble x 0x11, compared for equality", "[bside][chip]")
{
    for (int s = 0; s < 16; ++s)
    {
        auto c = makeChip();
        wr (c, 0, reg::attackDecay, 0x00);
        wr (c, 0, reg::sustainRelease, s << 4);
        wr (c, 0, reg::control, ctrl::gate);
        c.advanceCycles (2000000);
        CHECK (c.envelope (0) == s * 0x11);
        CHECK (c.envelopePhase (0) == EnvPhase::decaySustain);
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
    // Raise: the counter never rises, and with the equality comparison it decays on to 0.
    wr (c, 0, reg::sustainRelease, 0xc0);
    std::uint8_t last = c.envelope (0);
    for (int i = 0; i < 2000; ++i)
    {
        c.advanceCycles (1000);
        CHECK (c.envelope (0) <= last);
        last = c.envelope (0);
    }
    CHECK (c.envelope (0) == 0);
    CHECK (c.envelopePhase (0) == EnvPhase::decaySustain);
}

TEST_CASE ("chip 8.15.4: the divider period is a state, set when the counter takes a threshold value", "[bside][chip]")
{
    // An attack from 0xaa to 0xff passes 0xff (period 1); stopped at 0x60 on the way up from 0 it keeps 2
    // (0x5d was taken going up), so a release from 0x60 steps every 2 periods (cf. d_env_quirks_b).
    auto c = makeChip();
    wr (c, 0, reg::sustainRelease, 0x00);
    c.advanceCycles (3000); // release at rate 0 from 0xaa: down to 0x5d and below
    wr (c, 0, reg::attackDecay, 0x00);
    // Release fully to 0 first.
    c.advanceCycles (300000);
    REQUIRE (c.envelope (0) == 0);
    wr (c, 0, reg::control, ctrl::gate);
    REQUIRE (stepsUntil (c, [] (const Chip& x) { return x.envelope (0) == 0x5d; }, 2000) <= 2000);
    CHECK (envOf (c, 0).dividerPeriod == 4); // 0x36 set 4; 0x5d taken in this step: the change is pending for the next one
    CHECK (envOf (c, 0).pendingPeriod == 2);
    c.stepCycle();
    CHECK (envOf (c, 0).dividerPeriod == 2);
    CHECK (envOf (c, 0).pendingPeriod == 0);
    REQUIRE (stepsUntil (c, [] (const Chip& x) { return x.envelope (0) == 0x60; }, 2000) <= 2000);
    CHECK (envOf (c, 0).dividerPeriod == 2);
    wr (c, 0, reg::control, 0);
    std::uint8_t prev = c.envelope (0);
    std::uint64_t last = 0;
    int checked = 0;
    while (c.envelope (0) > 0x5e)
    {
        c.stepCycle();
        if (c.envelope (0) != prev)
        {
            if (last != 0)
            {
                CHECK (c.cycle() - last == 18);
                ++checked;
            }
            last = c.cycle();
            prev = c.envelope (0);
        }
    }
    CHECK (checked >= 1);
}

TEST_CASE ("chip 8.15.4: the delay quirk waits for the rate counter to pass 32 766 and wrap", "[bside][chip]")
{
    auto c = makeChip();
    wr (c, 0, reg::sustainRelease, 0x0f); // release 31251 (compare 31250), in release from power-on
    c.advanceCycles (20000);
    CHECK (envOf (c, 0).position == 20000);
    wr (c, 0, reg::sustainRelease, 0x00); // compare 8, below the position
    CHECK (envOf (c, 0).compare == 8);
    c.advanceCycles (32766 - 20000);
    CHECK (envOf (c, 0).position == 32766);
    c.stepCycle();
    CHECK (envOf (c, 0).position == 0);
    c.advanceCycles (8);
    CHECK (envOf (c, 0).position == 8);
    CHECK_FALSE (envOf (c, 0).resetPending);
    c.stepCycle();
    CHECK (envOf (c, 0).resetPending);
    c.stepCycle();
    CHECK (envOf (c, 0).position == 1);
}

TEST_CASE ("chip 8.15.4: gate-on takes effect 2 steps after the write (3 in the divider's last cycle)", "[bside][chip]")
{
    // At every offset within two rate periods of a release at rate 1 (divider period 1 above 0x5d).
    for (int offset = 0; offset < 80; ++offset)
    {
        auto c = makeChip();
        wr (c, 0, reg::sustainRelease, 0x01);
        c.advanceCycles (static_cast<std::uint64_t> (1000 + offset));
        const auto before = envOf (c, 0);
        wr (c, 0, reg::control, ctrl::gate);
        const std::uint64_t expected = before.dividerPipeline == 1 && ! before.resetPending ? 3 : 2;
        INFO ("offset " << offset);
        CHECK (stepsUntil (c, [] (const Chip& x) { return x.envelopePhase (0) == EnvPhase::attack; }, 10) == expected);
        // With a reset pending or the divider pipeline at 2, an attack step comes 2 steps after the write
        // (divider period 1, or the divider-pipeline case), or 4.
        if (before.resetPending || before.dividerPipeline == 2)
        {
            auto d = makeChip();
            wr (d, 0, reg::sustainRelease, 0x01);
            d.advanceCycles (static_cast<std::uint64_t> (1000 + offset));
            const auto cnt = d.envelope (0);
            wr (d, 0, reg::control, ctrl::gate);
            const std::uint64_t at = (before.dividerPeriod == 1 || before.dividerPipeline == 2) ? 2 : 4;
            CHECK (stepsUntil (d, [cnt] (const Chip& x) { return x.envelope (0) != cnt; }, 10) == at);
            CHECK (d.envelope (0) == static_cast<std::uint8_t> (cnt + 1));
        }
    }
}

TEST_CASE ("chip 8.15.4: gate-off takes effect 1 step later from decay/sustain, 2 from attack, +1 with a step pending", "[bside][chip]")
{
    for (int fromAttack = 0; fromAttack < 2; ++fromAttack)
        for (int offset = 0; offset < 40; ++offset)
        {
            auto c = makeChip();
            wr (c, 0, reg::attackDecay, fromAttack ? 0x50 : 0x00);
            wr (c, 0, reg::sustainRelease, 0x80);
            wr (c, 0, reg::control, ctrl::gate);
            c.advanceCycles (fromAttack ? static_cast<std::uint64_t> (5000 + offset) : static_cast<std::uint64_t> (200000 + offset));
            REQUIRE (c.envelopePhase (0) == (fromAttack ? EnvPhase::attack : EnvPhase::decaySustain));
            const auto before = envOf (c, 0);
            wr (c, 0, reg::control, 0);
            const std::uint64_t expected = (fromAttack ? 2u : 1u) + (before.stepPipeline > 0 ? 1u : 0u);
            INFO ((fromAttack ? "attack" : "decay/sustain") << " offset " << offset);
            CHECK (stepsUntil (c, [] (const Chip& x) { return x.envelopePhase (0) == EnvPhase::release; }, 10) == expected);
        }
}

TEST_CASE ("chip 8.15.4: a pending attack step that reaches 0xff replaces a pending release (a lost gate-off)", "[bside][chip]")
{
    // Attack at rate 0 up to 0xfe; the step to 0xff is pending (2 steps after a reset); the gate goes off
    // in between. The step to 0xff sets the target to decay/sustain: the release never comes.
    auto c = makeChip();
    wr (c, 0, reg::attackDecay, 0x00);
    wr (c, 0, reg::sustainRelease, 0x80);
    wr (c, 0, reg::control, ctrl::gate);
    REQUIRE (stepsUntil (c, [] (const Chip& x) { return x.envelope (0) == 0xfe; }, 5000) <= 5000);
    // Run to the reset after which the step to 0xff is scheduled; the gate goes off one step later.
    REQUIRE (stepsUntil (c, [] (const Chip& x) { return x.state().env[0].stepPipeline == 2; }, 20) <= 20);
    c.stepCycle(); // step pipeline 1
    wr (c, 0, reg::control, 0);
    c.stepCycle(); // the counter step: 0xff, target decay/sustain
    CHECK (c.envelope (0) == 0xff);
    c.advanceCycles (200000);
    CHECK (c.envelopePhase (0) == EnvPhase::decaySustain); // the gate-off was lost
    CHECK (c.envelope (0) == 0x88);
    // The same gate-off one rate period earlier is not lost.
    auto d = makeChip();
    wr (d, 0, reg::attackDecay, 0x00);
    wr (d, 0, reg::sustainRelease, 0x80);
    wr (d, 0, reg::control, ctrl::gate);
    REQUIRE (stepsUntil (d, [] (const Chip& x) { return x.envelope (0) == 0xfd; }, 5000) <= 5000);
    d.advanceCycles (3);
    wr (d, 0, reg::control, 0);
    d.advanceCycles (200000);
    CHECK (d.envelopePhase (0) == EnvPhase::release);
    CHECK (d.envelope (0) == 0);
}

TEST_CASE ("chip 8.15.4: an attack that starts at 0xff wraps the counter to 0", "[bside][chip]")
{
    auto c = makeChip();
    wr (c, 0, reg::attackDecay, 0x00);
    wr (c, 0, reg::sustainRelease, 0xf0);
    wr (c, 0, reg::control, ctrl::gate);
    c.advanceCycles (5000);
    REQUIRE (c.envelope (0) == 0xff);
    REQUIRE (c.envelopePhase (0) == EnvPhase::decaySustain);
    wr (c, 0, reg::control, 0);
    wr (c, 0, reg::control, ctrl::gate); // off and on again before any step
    REQUIRE (stepsUntil (c, [] (const Chip& x) { return x.envelope (0) != 0xff; }, 100) <= 100);
    CHECK (c.envelope (0) == 0x00);
    CHECK (c.envelopePhase (0) == EnvPhase::attack);
    c.advanceCycles (9);
    CHECK (c.envelope (0) == 0x01);
}

TEST_CASE ("chip 8.15.4: a release step at counter 0 wraps to 0xff when an attack phase cleared the freeze", "[bside][chip]")
{
    // The counter is 8 bits in every phase. Proven by committed data: d_env_cases_b, env3 00 -> ff in
    // release at cycle 407 985 (and 447 990); d_env_rates_b, env1 and env3 at 1 108 861 and 2 060 784.
    auto c = makeChip();
    wr (c, 0, reg::sustainRelease, 0x00);
    c.advanceCycles (100000);
    REQUIRE (c.envelope (0) == 0);
    REQUIRE (envOf (c, 0).frozen);
    wr (c, 0, reg::attackDecay, 0xf0); // attack rate 15: no attack step for a long time
    while (envOf (c, 0).resetPending || envOf (c, 0).dividerPipeline != 0)
        c.stepCycle();
    wr (c, 0, reg::control, ctrl::gate);
    c.advanceCycles (2);
    REQUIRE (c.envelopePhase (0) == EnvPhase::attack);
    CHECK_FALSE (envOf (c, 0).frozen); // the attack phase clears the freeze
    wr (c, 0, reg::control, 0);
    c.advanceCycles (2);
    REQUIRE (c.envelopePhase (0) == EnvPhase::release);
    REQUIRE (c.envelope (0) == 0);
    REQUIRE (stepsUntil (c, [] (const Chip& x) { return x.envelope (0) != 0; }, 40000) <= 40000);
    CHECK (c.envelope (0) == 0xff);
    CHECK (c.envelopePhase (0) == EnvPhase::release);
}

TEST_CASE ("chip 8.15.2 (c) step 6 (Smooth): the write-back ANDs the output into the noise output", "[bside][chip]")
{
    // With F just below 2^19 bit 19 rises every 2 steps: the pipeline alternates 2 and 1 and never shifts.
    // Steps at 1 copy the output into the noise output (the register is kept); steps at 2 AND it into both.
    // Without a shift nothing recomputes the noise output from the register, so the copy reaches the next
    // output: "noise output := the register's output" in the latch step would differ wherever the register
    // keeps a tap bit that the output had cleared (only Smooth frequencies get here; at register
    // frequencies a shift follows every latch step).
    Chip c;
    Config cfg;
    cfg.authentic = false;
    c.prepare (cfg);
    // Noise + ring-modulated triangle (no table): the ring keeps the triangle near its top for the first
    // steps, so the register has not locked up to 0 yet when the first latch steps come.
    constexpr std::uint8_t w = ctrl::noise | ctrl::triangle | ctrl::ring;
    c.setOscillator (0, { 524287.9, 0.0f, static_cast<std::uint8_t> (w | ctrl::test), 0, 0 });
    c.setOscillator (0, { 524287.9, 0.0f, w, 0, 0 });
    int discriminating = 0;
    for (int i = 0; i < 4000; ++i)
    {
        const auto before = oscOf (c, 0);
        c.stepCycle();
        const auto o = oscOf (c, 0);
        if (i == 0)
            continue; // step 1 comes before the first rise (pipeline still 0)
        REQUIRE (o.noisePipeline != 0); // never a shift
        if (o.noisePipeline == 1)
        {
            REQUIRE (o.noiseOutput == o.output);
            REQUIRE (o.noise == before.noise);
            discriminating += wave::noise (o.noise) != o.noiseOutput ? 1 : 0;
        }
        else
        {
            REQUIRE (o.noiseOutput == (before.noiseOutput & o.output));
            REQUIRE (o.noise == (before.noise & wave::noiseKeepMask (o.output)));
        }
    }
    CHECK (discriminating > 0);
}

TEST_CASE ("chip 8.15.4: rate writes change the compare position only in the matching phase", "[bside][chip]")
{
    auto c = makeChip(); // release
    wr (c, 0, reg::attackDecay, 0x9a);
    CHECK (envOf (c, 0).compare == kRateMatch[0]);
    wr (c, 0, reg::sustainRelease, 0x03);
    CHECK (envOf (c, 0).compare == kRateMatch[3]);
    wr (c, 0, reg::control, ctrl::gate);
    c.advanceCycles (2);
    REQUIRE (c.envelopePhase (0) == EnvPhase::attack);
    CHECK (envOf (c, 0).compare == kRateMatch[9]);
    wr (c, 0, reg::sustainRelease, 0x0c);
    CHECK (envOf (c, 0).compare == kRateMatch[9]);
    wr (c, 0, reg::attackDecay, 0x2a);
    CHECK (envOf (c, 0).compare == kRateMatch[2]);
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
    CHECK (smooth.accumulator (0) == 0x555555 + 4001); // 4 x 1000.25
    CHECK (smooth.state().osc[0].accFraction == 0);
    smooth.advanceCycles (1);
    CHECK (smooth.state().osc[0].accFraction == (1u << 30));
    smooth.setOscillator (1, { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<float>::infinity(), 0, 0, 0 });
    CHECK (smooth.increment (1) == 0);
    smooth.setOscillator (2, { 1.0e12, 0.0f, 0, 0, 0 });
    CHECK (smooth.increment (2) == kMaxIncrement);
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
    CHECK (c.increment (0) == 0);
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
    auto prep = [] (Chip& c) {
        zeroAcc (c, 0);
        zeroAcc (c, 1, ctrl::sync);
        setFreq (c, 0, 0x1001); // top bit: 2048 x 0x1001 = 0x800800 is the first value >= 0x800000
        setFreq (c, 1, 0x0100);
    };
    auto c = makeChip();
    prep (c);
    Sink sink;
    c.advanceCycles (5000, sink); // also many noise clocks: none may reach the sink
    REQUIRE (sink.calls >= 1);
    CHECK (sink.noiseCalls == 0);
    for (int i = 0; i < 5000; ++i)
        c.stepCycle (sink);
    CHECK (sink.noiseCalls == 0);
    auto d = makeChip();
    prep (d);
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
    CHECK (cycles == (96000ull * c.sampleClock().increment()) >> 32);
    CHECK (cycles >= 985247);
    CHECK (cycles <= 985248);
    CHECK (c.cycle() == cycles);
}
