// The three defects docs/architecture.md 8.17 left for the render cost step (8.18), each a regression case
// that failed on the renderer of S2b-2: a synced pulse at frequency 0, a ring target after a width write in the
// step before its source's rise (placeholder shapes), a large frequency drop on a sync source between its
// rendered crossing and its digital rise. Public interface only.

#include "RenderTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace bs_render_test;
namespace ctrl = vs::bside::chip::ctrl;
using vs::bside::chip::kPhaseMsb;
using vs::bside::chip::ShapeSet;

namespace
{
Chip chipWith (Model m, double rate, ShapeSet shapes)
{
    Chip c;
    Config cfg;
    cfg.model = m;
    cfg.internalRate = rate;
    cfg.shapes = shapes;
    c.prepare (cfg);
    return c;
}

/** The chip's own output after its last step: the three converter values times their envelopes. */
double digitalOutput (const Chip& d, Model m, ShapeSet shapes)
{
    const auto& t = vs::bside::chip::render::tablesFor (m, shapes);
    double sum = 0.0;
    for (int n = 0; n < 3; ++n)
        sum += static_cast<double> (t.wave[d.output (n) & 0xfffu]) * static_cast<double> (t.envelope[d.envelope (n)]);
    return sum;
}
} // namespace

TEST_CASE ("render defect 1 (8.17): a synced pulse at frequency 0 takes the chip's gate one cycle after the reset", "[render][defects]")
{
    // Oscillator 2 runs a pulse until its accumulator is above the width, then its frequency is written to 0
    // and its sync bit set: oscillator 1 (no waveform, running) resets it to 0, where the comparison is low
    // for good. The renderer kept the gate high (no line event on a frozen line), and its state check skipped
    // the line (frozen at a boundary).
    for (const Model m : { Model::A, Model::B })
        for (const double rate : { 96000.0, 192000.0 })
            for (const float pw : { 100.0f, 3000.0f })
            {
                Chip c = chipWith (m, rate, ShapeSet::fitted);
                c.renderCheckStates (true);
                setOsc (c, 0, 2000.0, 2048.0f, 0x00);
                setOsc (c, 1, 3000.0, pw, ctrl::pulse | ctrl::gate);
                std::vector<float> y (8000, 0.0f);
                c.render (y.data(), 2000);
                while (((c.accumulator (1) >> 12) & 0xfffu) < 3500u)
                    c.render (y.data(), 1);
                Chip d = c;
                for (Chip* x : { &c, &d })
                {
                    x->write (0x07, 0x00);
                    x->write (0x08, 0x00);
                    x->write (0x0b, static_cast<std::uint8_t> (ctrl::pulse | ctrl::sync | ctrl::gate));
                }
                std::fill (y.begin(), y.end(), 0.0f);
                c.render (y.data(), 8000);
                d.advanceCycles (c.cycle() - d.cycle());
                INFO ((m == Model::A ? "Model A" : "Model B") << ", " << rate << " Hz, width " << pw << ": rendered " << y.back() << ", chip "
                                                              << digitalOutput (d, m, ShapeSet::fitted));
                CHECK (std::abs (static_cast<double> (y.back()) - digitalOutput (d, m, ShapeSet::fitted)) < 1.0e-6);
                CHECK (c.renderStateMismatches() == 0);
                CHECK (c.renderLargeCorrections() == 0);
                // the state check covers the frozen line (8.18: it skips one only for a stated reason)
                CHECK (c.renderFrozenStateChecks() > 1000);
            }
}

TEST_CASE ("render defect 2 (8.17): a width write in the step before a ring source's rise (placeholder shapes)", "[render][defects]")
{
    // Model A, placeholder shapes (a saw combination's top bit stays up for a quarter period): oscillator 1, a
    // saw+pulse or triangle+saw+pulse, gets a wider width in the step before its rise (or two steps before).
    // The rise still finds the gate open (the comparison was made before the write); the next comparison, with
    // the new width, is low, and the chip pulls the top bit down one cycle later. Oscillator 2's ring took the
    // rise and kept it until the shape's own pull, or, the source never rising again, for good.
    int cases = 0;
    for (const double f : { 64407.37, 3000.3 })
        for (const double rate : { 96000.0, 384000.0 })
            for (const std::uint8_t combo : { std::uint8_t { 0x61 }, std::uint8_t { 0x71 } })
                for (const float pwOld : { 405.0f, 1500.0f })
                    for (const float pwNew : { 2646.0f, 3500.0f })
                        for (const int ahead : { 1, 2 })
                        {
                            Chip c = chipWith (Model::A, rate, ShapeSet::placeholder);
                            c.renderCheckStates (true);
                            setOsc (c, 0, f, pwOld, combo);
                            setOsc (c, 1, 3000.0, 2048.0f, ctrl::triangle | ctrl::ring | ctrl::gate);
                            std::vector<float> y (2000);
                            c.render (y.data(), 2000);
                            Chip p = c;
                            const auto a = static_cast<std::uint64_t> (ahead);
                            while (! (p.phase (0) < kPhaseMsb && p.phase (0) + a * p.increment (0) >= kPhaseMsb && p.phase (0) + (a - 1) * p.increment (0) < kPhaseMsb))
                                p.stepCycle();
                            while (c.cycle() < p.cycle())
                                c.renderUntil (y.data(), 64, p.cycle());
                            setOsc (c, 0, f, pwNew, combo);
                            c.render (y.data(), 2000);
                            INFO ("frequency " << f << ", " << rate << " Hz, control " << int (combo) << ", width " << pwOld << " -> " << pwNew << ", " << ahead
                                               << " step(s) before the rise: state mismatches " << c.renderStateMismatches() << ", large corrections "
                                               << c.renderLargeCorrections());
                            CHECK (c.renderStateMismatches() == 0);
                            CHECK (c.renderLargeCorrections() == 0);
                            ++cases;
                        }
    CHECK (cases == 64);
}

TEST_CASE ("render defect 3 (8.17): a large frequency drop on a sync source between its rendered crossing and its digital rise", "[render][defects]")
{
    // A write right after render() lands at a sample boundary; the lines were rendered up to the sample's end,
    // up to a cycle beyond it. Where the source's line crossed its top bit inside that last part, the target's
    // reset was predicted; a lower frequency written then puts the crossing (and the chip's rise) later, and
    // the prediction timed out with a large correction (a glitch of a few cycles).
    for (const Model m : { Model::A, Model::B })
        for (const double rate : { 96000.0, 192000.0, 384000.0 })
            for (const double f0 : { 30000.0, 65000.0 })
                for (const double fDrop : { 100.0, 15000.0 })
                {
                    Chip c = chipWith (m, rate, ShapeSet::fitted);
                    c.renderCheckStates (true);
                    setOsc (c, 0, f0, 2048.0f, ctrl::saw | ctrl::gate);
                    setOsc (c, 1, 7000.0, 2048.0f, ctrl::saw | ctrl::sync | ctrl::gate);
                    setOsc (c, 2, 0.0, 2048.0f, 0x00);
                    std::vector<float> y (400);
                    int writes = 0;
                    for (int s = 0; s < 60000 && writes < 8; ++s)
                    {
                        c.render (y.data(), 1);
                        const std::uint64_t p = c.phase (0), inc = c.increment (0);
                        const double frac = static_cast<double> (c.sampleClock().fraction()) / 4294967296.0;
                        // the next step rises; the line crossed before the sample's end
                        if (s > 2000 && p < kPhaseMsb && p + inc >= kPhaseMsb && static_cast<double> (kPhaseMsb - p) / static_cast<double> (inc) < frac)
                        {
                            setOsc (c, 0, fDrop, 2048.0f, ctrl::saw | ctrl::gate);
                            c.render (y.data(), 400);
                            setOsc (c, 0, f0, 2048.0f, ctrl::saw | ctrl::gate);
                            ++writes;
                        }
                    }
                    INFO ((m == Model::A ? "Model A" : "Model B") << ", " << rate << " Hz, source " << f0 << " dropped to " << fDrop << ": " << writes
                                                                  << " writes, large corrections " << c.renderLargeCorrections() << ", state mismatches "
                                                                  << c.renderStateMismatches());
                    CHECK (writes == 8);
                    CHECK (c.renderLargeCorrections() == 0);
                    CHECK (c.renderStateMismatches() == 0);
                }
}

TEST_CASE ("render defect 1, sibling (render cost review): a frozen Model A pull-down takes the chip's gate one cycle later", "[render][defects]")
{
    // Oscillator 1 plays a triangle at the register's top until its cell is in the upper half, its frequency is
    // written to 0, then a saw+pulse combination with a width above the cell: the chip pulls the top bit down at
    // once and its comparison goes low. The re-read one cycle later was scheduled as a phase, which a frozen line
    // never reaches: the rendered gate stayed open for good.
    for (const std::uint8_t combo : { std::uint8_t { 0x61 }, std::uint8_t { 0x71 } })
        for (const double rate : { 88200.0, 96000.0 })
        {
            Chip c = chipWith (Model::A, rate, ShapeSet::fitted);
            c.renderCheckStates (true);
            setOsc (c, 0, 65535.0, 2048.0f, ctrl::triangle | ctrl::gate);
            std::vector<float> y (4000, 0.0f);
            c.render (y.data(), 1000);
            while (! (((c.accumulator (0) >> 12) & 0xfffu) >= 3000u && ((c.accumulator (0) >> 12) & 0xfffu) <= 3500u))
                c.render (y.data(), 1);
            Chip d = c;
            for (Chip* x : { &c, &d })
            {
                x->write (0x00, 0x00);
                x->write (0x01, 0x00);
                setOsc (*x, 0, 0.0, 2284.0f, combo);
            }
            std::fill (y.begin(), y.end(), 0.0f);
            c.render (y.data(), 4000);
            d.advanceCycles (c.cycle() - d.cycle());
            INFO ("control " << int (combo) << ", " << rate << " Hz: rendered " << y.back() << ", chip " << digitalOutput (d, Model::A, ShapeSet::fitted)
                             << ", state mismatches " << c.renderStateMismatches());
            CHECK (std::abs (static_cast<double> (y.back()) - digitalOutput (d, Model::A, ShapeSet::fitted)) < 1.0e-6);
            CHECK (c.renderStateMismatches() == 0);
            CHECK (c.renderFrozenStateChecks() > 1000);
        }
}

TEST_CASE ("render: a frozen Model A saw combination pulled down by a write (width or waveform), with a ring target", "[render][defects]")
{
    // Render cost review, item 3: frequency 0 in the upper half; a width write closes the comparison
    // (placeholder shapes: the shape keeps bit 11 there), or a waveform write lands on a cell whose bit 11 is
    // clear (fitted shapes): the chip pulls the top bit down one or two steps later. renderLanding returned at
    // once for a zero increment and nothing was predicted: one large correction, and oscillator 3's ring kept
    // the old top bit.
    struct Case
    {
        ShapeSet shapes;
        double rate;
        bool widthWrite;
        std::uint8_t control;
    };
    const Case cases[] { { ShapeSet::placeholder, 128000.0, true, 0x71 }, { ShapeSet::placeholder, 96000.0, true, 0x61 },
                         { ShapeSet::fitted, 128000.0, false, 0x31 }, { ShapeSet::fitted, 96000.0, false, 0x71 } };
    for (const auto& k : cases)
        for (const bool ring : { false, true })
        {
            Chip c = chipWith (Model::A, k.rate, k.shapes);
            c.renderCheckStates (true);
            const std::uint8_t first = k.widthWrite ? k.control : std::uint8_t { 0x11 };
            setOsc (c, 1, 20000.0, 2016.0f, first);
            if (ring)
                setOsc (c, 2, 3000.0, 2048.0f, ctrl::triangle | ctrl::ring | ctrl::gate);
            std::vector<float> y (4000, 0.0f);
            c.render (y.data(), 1000);
            while (! (((c.accumulator (1) >> 12) & 0xfffu) >= 2900u && ((c.accumulator (1) >> 12) & 0xfffu) <= 3100u))
                c.render (y.data(), 1);
            setOsc (c, 1, 0.0, 2016.0f, first); // frozen in the upper half
            c.render (y.data(), 200);
            const auto large = c.renderLargeCorrections();
            const auto mismatches = c.renderStateMismatches();
            Chip d = c;
            for (Chip* x : { &c, &d })
                setOsc (*x, 1, 0.0, k.widthWrite ? 4057.0f : 2016.0f, k.control);
            std::fill (y.begin(), y.end(), 0.0f);
            c.render (y.data(), 3000);
            d.advanceCycles (c.cycle() - d.cycle());
            INFO ((k.shapes == ShapeSet::fitted ? "fitted" : "placeholder") << ", " << k.rate << " Hz, " << (k.widthWrite ? "width write" : "waveform write")
                                                                               << ", control " << int (k.control) << (ring ? ", ring target" : "") << ": large corrections "
                                                                               << c.renderLargeCorrections() - large << ", state mismatches " << c.renderStateMismatches() - mismatches);
            REQUIRE ((d.phase (1) & kPhaseMsb) == 0); // the chip did pull it down
            CHECK (c.renderLargeCorrections() == large);
            CHECK (c.renderStateMismatches() == mismatches);
            if (! ring)
                CHECK (std::abs (static_cast<double> (y[2999]) - digitalOutput (d, Model::A, k.shapes)) < 1.0e-6);
        }
}
