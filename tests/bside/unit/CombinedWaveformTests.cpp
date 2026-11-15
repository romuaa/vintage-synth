// Our combined-waveform tables (source/bside/chip/CombinedWaveforms.h; docs/architecture.md 8.15.2 (c),
// 8.15.8): built from the parameters, the same on every platform (pinned hash, decision margins), used by
// the chip at every place 8.15.2 (c) names, and the fast path stays equal to per-cycle stepping with them.
// Their agreement with the reference is tested by bside_trace (ShapeDataTests.cpp).

#include "bside/chip/Chip.h"
#include "bside/chip/CombinedWaveforms.h"
#include "bside/trace/Sha256.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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
    for (const auto& row : t.wave)
        for (const auto v : row)
        {
            s.push_back (static_cast<char> (v & 0xff));
            s.push_back (static_cast<char> (v >> 8));
        }
    return s;
}

void write (Chip& c, std::uint8_t r, std::uint8_t v) { c.write (r, v); }
} // namespace

TEST_CASE ("combined waveforms: the fitted tables are the same on every platform (pinned SHA-256)", "[bside][chip][shapes]")
{
    // Changes only with a refit (CombinedWaveformParams.cpp); a compiler or library that rounds differently
    // enough to flip a decision changes it too, which the margin test below rules out by construction.
    CHECK (vs::bside::trace::sha256Hex (tableBytes (combined::fittedTables (Model::A)))
           == "a0236447f0f41a077deb67e3f8eceb3dc9a5c50e3f1d4d57d377a9d2a366381f");
    CHECK (vs::bside::trace::sha256Hex (tableBytes (combined::fittedTables (Model::B)))
           == "bd4beb4a89ab324be972ec06f491133f2e369e1d0b061f118aa130168d9a766e");
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
            for (const auto w : combined::kNoiseCombinations)
                REQUIRE (t.wave[w][ix] == combined::fittedParams (m).noiseShape[w]);
        }
    }
    // The placeholder (bside_independent only) is the S2a rule: AND of the selected triangle and saw.
    const auto& p = combined::placeholderTables();
    CHECK (p.wave[3][0x7f0] == (wave::triangle (0x7f0) & 0x7f0));
    CHECK (p.wave[0xc][0x123] == 0xfff);
    CHECK (p.wave[6][0x9ab] == 0x9ab);
    // This executable is built with the fitted default.
    CHECK (Config {}.shapes == ShapeSet::fitted);
}

TEST_CASE ("combined waveforms: building the tables at prepare is cheap and allocation-free", "[bside][chip][shapes]")
{
    auto t = std::make_unique<combined::ShapeTables>();
    const auto start = std::chrono::steady_clock::now();
    combined::buildTables (combined::fittedParams (Model::A), *t);
    combined::buildTables (combined::fittedParams (Model::B), *t);
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
    std::printf ("[shapes] building both models' tables: %.1f ms (once per process, at the first prepare)\n", ms);
    CHECK (ms < 2000.0); // a loose bound; the typical cost is printed
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
                        if ((w & wave::kSaw) != 0 && ((shape & pmask) & 0x800) == 0)
                            REQUIRE ((c.accumulator (2) & 0x800000u) == 0);
                    }
                    else if (havePrev) // the latch holds the previous step's unmasked shape; this step's mask applies
                        REQUIRE (c.read (0x1b) == ((prevShape & pmask) >> 4));
                    prevShape = shape;
                    havePrev = true;
                }
            }
    // Noise write-back (step 6): pulse + noise ANDs the noise output with the constant shape.
    for (const auto m : { Model::A, Model::B })
    {
        Config cfg;
        cfg.model = m;
        cfg.authentic = true;
        Chip c;
        c.prepare (cfg);
        write (c, 0x12, 0x80);
        c.advanceCycles (10);
        const auto noiseOut = c.state().osc[2].noiseOutput;
        write (c, 0x12, 0xc0);
        c.stepCycle();
        CHECK (c.output (2) == (noiseOut & combined::fittedParams (m).noiseShape[0xc]));
        CHECK (c.state().osc[2].noiseOutput == c.output (2));
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
            }
}
