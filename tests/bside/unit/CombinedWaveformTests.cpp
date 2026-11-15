// Our combined-waveform tables (source/bside/chip/CombinedWaveforms.h; docs/architecture.md 8.15.2 (c),
// 8.15.8): built from the parameters, the same on every platform (pinned hash, decision margins), used by
// the chip at every place 8.15.2 (c) names, and the fast path stays equal to per-cycle stepping with them.
// Their agreement with the reference is tested by bside_trace (ShapeDataTests.cpp).

#include "bside/chip/Chip.h"
#include "bside/chip/CombinedWaveforms.h"
#include "bside/trace/Sha256.h"

#include "../../seq_unit/AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

using namespace vs::bside::chip;

namespace
{
std::string tableBytes (const combined::ShapeTables& t)
{
    std::string s;
    s.reserve (16 * 4096 * 2);
    auto put = [&s] (std::uint16_t v) {
        s.push_back (static_cast<char> (v & 0xff));
        s.push_back (static_cast<char> (v >> 8));
    };
    for (const auto& row : t.wave)
        for (const auto v : row)
            put (v);
    for (const auto v : t.pulseNoise)
        put (v);
    return s;
}

void write (Chip& c, std::uint8_t r, std::uint8_t v) { c.write (r, v); }
} // namespace

TEST_CASE ("combined waveforms: the fitted tables are the same on every platform (pinned SHA-256)", "[bside][chip][shapes]")
{
    // Changes only with a refit (CombinedWaveformParams.cpp); a compiler or library that rounds differently
    // enough to flip a decision changes it too, which the margin test below rules out by construction.
    CHECK (vs::bside::trace::sha256Hex (tableBytes (combined::fittedTables (Model::A)))
           == "80ceba8c0e41d85a781192e1988bf723ccb00f757fd338649b232c64f6e16562");
    CHECK (vs::bside::trace::sha256Hex (tableBytes (combined::fittedTables (Model::B)))
           == "c2aeb22b2f2dcbbc39798bc7c2203f5141c06db9805177b130c955e4cc135bf2");
}

TEST_CASE ("combined waveforms: every decision of the network model keeps the floating-point margin", "[bside][chip][shapes]")
{
    for (const auto m : { Model::A, Model::B })
        for (const auto w : combined::kNetworkCombinations)
        {
            double margin = 1e300;
            for (unsigned ix = 0; ix < 4096; ++ix)
            {
                const auto e = combined::evaluate (combined::fittedParams (m).network[w], w, static_cast<std::uint16_t> (ix));
                margin = std::min (margin, e.margin);
                REQUIRE (e.value == combined::fittedTables (m).wave[w][ix]);
            }
            INFO ("model " << (m == Model::A ? "A" : "B") << " waveform " << int (w));
            CHECK (margin >= combined::kMinMargin);
        }
    for (const auto m : { Model::A, Model::B })
    {
        double margin = 1e300;
        for (unsigned k = 0; k < 256; ++k)
        {
            const auto e = combined::evaluatePulseNoise (combined::fittedParams (m).pulseNoise, static_cast<std::uint16_t> (k << 4));
            margin = std::min (margin, e.margin);
            REQUIRE (e.value == combined::fittedTables (m).pulseNoise[k]);
        }
        CHECK (margin >= combined::kMinMargin);
    }
}

TEST_CASE ("combined waveforms: the bits the read-back cannot show are plausible", "[bside][chip][shapes]")
{
    // CombinedWaveforms.h, constraints: a line is high only if a selected triangle or saw input is high,
    // and bits 3..0 only continue a run of set bits from above (never low bits alone).
    for (const auto m : { Model::A, Model::B })
    {
        const auto& t = combined::fittedTables (m);
        for (const auto w : combined::kNetworkCombinations)
            for (unsigned ix = 0; ix < 4096; ++ix)
            {
                const auto i = static_cast<std::uint16_t> (ix);
                const unsigned inputs = ((w & wave::kTri) != 0 ? wave::triangle (i) : 0u) | ((w & wave::kSaw) != 0 ? i : 0u);
                const unsigned v = t.wave[w][ix];
                INFO ("model " << (m == Model::A ? "A" : "B") << " waveform " << int (w) << " index " << ix);
                REQUIRE ((v & ~inputs) == 0u);
                if ((w & wave::kTri) != 0 && (w & wave::kSaw) == 0)
                    REQUIRE ((v & 1u) == 0u); // the triangle's bit 0 is always 0
                for (int b = 0; b < 4; ++b)
                    if (((v >> b) & 1u) != 0)
                        REQUIRE (((v >> (b + 1)) & 1u) != 0);
                REQUIRE ((v == 0 || (v & 0xff0u) != 0));
            }
        for (unsigned k = 0; k < 256; ++k)
        {
            const unsigned v = t.pulseNoise[k];
            REQUIRE ((v & ~(k << 4)) == 0u); // only lines the noise output drives high, never bits 3..0
        }
    }
    CHECK (combined::constrain (0x00f, 0xfff) == 0);
    CHECK (combined::constrain (0x01b, 0xfff) == 0x018);
    CHECK (combined::constrain (0x0ff, 0x0f0) == 0x0f0);
}

TEST_CASE ("combined waveforms: rows without a table and the noise combinations", "[bside][chip][shapes]")
{
    for (const auto m : { Model::A, Model::B })
    {
        const auto& t = combined::fittedTables (m);
        for (unsigned ix = 0; ix < 4096; ++ix)
        {
            const auto i = static_cast<std::uint16_t> (ix);
            REQUIRE (t.wave[0][ix] == 0);
            REQUIRE (t.wave[wave::kTri][ix] == wave::triangle (i));
            REQUIRE (t.wave[wave::kTri | wave::kNoise][ix] == wave::triangle (i));
            REQUIRE (t.wave[wave::kSaw][ix] == i);
            REQUIRE (t.wave[wave::kSaw | wave::kNoise][ix] == i);
            REQUIRE (t.wave[wave::kPulse][ix] == 0xfff);
            REQUIRE (t.wave[wave::kNoise][ix] == 0xfff);
            for (const auto w : combined::kConstantNoiseCombinations)
                REQUIRE (t.wave[w][ix] == combined::fittedParams (m).noiseShape[w]);
        }
    }
    // The placeholder (bside_independent only) is the S2a rule: AND of the selected triangle and saw,
    // 0xfff without either, for every entry; pulse with noise 0xfff.
    const auto& p = combined::placeholderTables();
    for (unsigned w = 1; w < 16; ++w)
        for (unsigned ix = 0; ix < 4096; ++ix)
        {
            const auto i = static_cast<std::uint16_t> (ix);
            const auto bits = static_cast<std::uint8_t> (w);
            unsigned expect = 0xfff;
            if ((bits & wave::kTri) != 0)
                expect &= wave::triangle (i);
            if ((bits & wave::kSaw) != 0)
                expect &= i;
            if (bits == wave::kTri || bits == (wave::kTri | wave::kNoise))
                expect = wave::triangle (i);
            REQUIRE (p.wave[w][ix] == expect);
        }
    for (const auto v : p.pulseNoise)
        REQUIRE (v == 0xfff);
    // This executable is built with the fitted default.
    CHECK (Config {}.shapes == ShapeSet::fitted);
}

TEST_CASE ("combined waveforms: building the tables and preparing a chip allocate nothing", "[bside][chip][shapes]")
{
    // Counted: the work of the first use (buildTables for both models, which is all the function-local
    // static's constructor does) and a chip's construction and prepare. The first use itself cannot be
    // forced inside a shared test executable (another test may have built the statics already); its only
    // other step, the thread-safe static guard, does not allocate.
    auto t = std::make_unique<combined::ShapeTables>();
    // The shared BLEP table (common/dsp/BlepTable.h, vector storage, Vintage's too) allocates once per
    // process on its first use, which a chip's construction makes (S2b-2); everything of our own (shape
    // tables, render tables, the chip) allocates nothing. Take that first use outside the count.
    (void) vs::dsp::BlepTable::get();
#if VS_SEQ_COUNT_ALLOCATIONS
    {
        vs::seq::test::AllocationCounter probe;
        vs::seq::test::gEscape = std::malloc (8); // the counter itself works
        CHECK (probe.count() == 1);
        std::free (vs::seq::test::gEscape);
    }
    vs::seq::test::AllocationCounter counter;
#endif
    combined::buildTables (combined::fittedParams (Model::A), *t);
    combined::buildTables (combined::fittedParams (Model::B), *t);
    Chip c;
    Config cfg;
    cfg.model = Model::A;
    c.prepare (cfg);
#if VS_SEQ_COUNT_ALLOCATIONS
    CHECK (counter.count() == 0);
#else
    SUCCEED ("allocations are counted on macOS without AddressSanitizer only (AllocationCounter.h)");
#endif
    CHECK (t->wave[3] == combined::fittedTables (Model::B).wave[3]);
}

TEST_CASE ("combined waveforms: the chip's output, read value, latch, pull-down and write-back follow the table", "[bside][chip][shapes]")
{
    for (const auto m : { Model::A, Model::B })
        for (const auto w : combined::kNetworkCombinations)
            for (const unsigned pw : { 0u, 0x800u })
            {
                Config cfg;
                cfg.model = m;
                cfg.authentic = true;
                Chip c;
                c.prepare (cfg);
                const auto& t = c.shapeTables();
                REQUIRE (&t == &combined::fittedTables (m));
                write (c, 0x12, 0x08);
                write (c, 0x0f, 0x10); // F = 0x1000: index + 1 per cycle
                write (c, 0x10, static_cast<std::uint8_t> (pw & 0xff));
                write (c, 0x11, static_cast<std::uint8_t> (pw >> 8));
                c.advanceCycles (4);
                write (c, 0x12, static_cast<std::uint8_t> (w << 4));
                std::uint16_t prevShape = 0;
                bool havePrev = false;
                for (int k = 0; k < 6000; ++k)
                {
                    const std::uint32_t before = c.accumulator (2);
                    c.stepCycle();
                    const auto ix = static_cast<std::uint16_t> (((before + 0x1000) >> 12) & 0xfff);
                    const auto shape = t.wave[w][ix];
                    // The pulse mask is the comparison of the previous step (its accumulator after any pull-down).
                    const std::uint16_t pmask = (w & wave::kPulse) == 0 || ((before >> 12) & 0xfff) >= pw ? 0xfff : 0;
                    INFO ("model " << (m == Model::A ? "A" : "B") << " waveform " << int (w) << " pulse width " << pw << " index " << ix);
                    REQUIRE (c.output (2) == (shape & pmask));
                    if (m == Model::A)
                    {
                        REQUIRE (c.read (0x1b) == ((shape & pmask) >> 4));
                        // Step 4: the saw combinations clear the top bit when the output's bit 11 is 0.
                        const std::uint32_t added = (before + 0x1000) & 0xffffff;
                        if ((w & wave::kSaw) != 0 && ((shape & pmask) & 0x800) == 0)
                            REQUIRE (c.accumulator (2) == (added & 0x7fffffu));
                        else
                            REQUIRE (c.accumulator (2) == added); // bit 11 set (or no saw): no pull-down
                    }
                    else if (havePrev) // the latch holds the previous step's unmasked shape; this step's mask applies
                        REQUIRE (c.read (0x1b) == ((prevShape & pmask) >> 4));
                    prevShape = shape;
                    havePrev = true;
                }
            }
    // Noise write-back (step 6). Pulse + noise: the shape is Tpn (noise output), ANDed with it; the
    // constant shapes of the other noise combinations (0) clear the register's tap bits.
    for (const auto m : { Model::A, Model::B })
        for (const std::uint8_t w : { std::uint8_t { 0xb }, std::uint8_t { 0xc }, std::uint8_t { 0xd }, std::uint8_t { 0xe }, std::uint8_t { 0xf } })
        {
            Config cfg;
            cfg.model = m;
            cfg.authentic = true;
            Chip c;
            c.prepare (cfg);
            write (c, 0x12, 0x80);
            c.advanceCycles (10);
            const auto noiseOut = c.state().osc[2].noiseOutput;
            const auto reg = c.noiseRegister (2);
            REQUIRE (noiseOut != 0);
            write (c, 0x12, static_cast<std::uint8_t> (w << 4));
            c.stepCycle();
            const std::uint16_t shape = w == 0xc ? combined::fittedTables (m).pulseNoise[noiseOut >> 4] : combined::fittedParams (m).noiseShape[w];
            const auto out = static_cast<std::uint16_t> (shape & noiseOut);
            INFO ("model " << (m == Model::A ? "A" : "B") << " waveform " << int (w));
            CHECK (c.output (2) == out);
            CHECK (c.state().osc[2].noiseOutput == out);
            CHECK (c.noiseRegister (2) == (reg & wave::noiseKeepMask (out)));
            if (w != 0xc)
                CHECK ((c.noiseRegister (2) & wave::kTapBits) == 0u);
        }
}

TEST_CASE ("combined waveforms: the fast path equals per-cycle stepping for every waveform combination", "[bside][chip][shapes][equivalence]")
{
    for (const auto m : { Model::A, Model::B })
        for (unsigned w = 1; w < 16; ++w)
            for (const std::uint16_t pw : { std::uint16_t { 0 }, std::uint16_t { 0x7d3 } })
            {
                Config cfg;
                cfg.model = m;
                cfg.authentic = true;
                Chip stepped, fast;
                stepped.prepare (cfg);
                fast.prepare (cfg);
                for (Chip* c : { &stepped, &fast })
                {
                    for (std::uint8_t o = 0; o < 3; ++o)
                    {
                        const auto base = static_cast<std::uint8_t> (7 * o);
                        write (*c, static_cast<std::uint8_t> (base + 0), static_cast<std::uint8_t> (0x35 + 17 * o));
                        write (*c, static_cast<std::uint8_t> (base + 1), static_cast<std::uint8_t> (0x0b + 5 * o));
                        write (*c, static_cast<std::uint8_t> (base + 2), static_cast<std::uint8_t> (pw & 0xff));
                        write (*c, static_cast<std::uint8_t> (base + 3), static_cast<std::uint8_t> (pw >> 8));
                        // Oscillator 3 the combination; 1 and 2 ring/sync variants of it.
                        const auto ctl = static_cast<std::uint8_t> ((w << 4) | (o == 0 ? 0x04 : o == 1 ? 0x02 : 0x00));
                        write (*c, static_cast<std::uint8_t> (base + 4), ctl);
                    }
                }
                for (int k = 0; k < 2000; ++k)
                    stepped.stepCycle();
                fast.advanceCycles (2000);
                INFO ("model " << (m == Model::A ? "A" : "B") << " waveform " << w << " pulse width " << pw);
                CHECK (fast.state() == stepped.state());
                for (int k = 0; k < 40000; ++k)
                    stepped.stepCycle();
                for (int k = 0; k < 40; ++k)
                    fast.advanceCycles (1000);
                CHECK (fast.state() == stepped.state());
                // While running: a test-bit pulse on oscillator 3, then a change to the next combination.
                const auto next = static_cast<std::uint8_t> (((w % 15) + 1) << 4);
                for (Chip* c : { &stepped, &fast })
                    write (*c, 0x12, static_cast<std::uint8_t> ((w << 4) | 0x08));
                for (int k = 0; k < 777; ++k)
                    stepped.stepCycle();
                fast.advanceCycles (777);
                CHECK (fast.state() == stepped.state());
                for (Chip* c : { &stepped, &fast })
                    write (*c, 0x12, next);
                for (int k = 0; k < 30011; ++k)
                    stepped.stepCycle();
                fast.advanceCycles (13);
                fast.advanceCycles (29998);
                CHECK (fast.state() == stepped.state());
            }
}
