// The three defects docs/architecture.md 8.17 left for the render cost step (8.18), each a regression case
// that failed on the renderer of S2b-2: a synced pulse at frequency 0, a ring target after a width write in the
// step before its source's rise (placeholder shapes), a large frequency drop on a sync source between its
// rendered crossing and its digital rise. Public interface only.

#include "RenderTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    // never reaches: the rendered gate stayed open for good. The writes land at a sample's start, or 0 to 2 cycles
    // before its end (renderUntil): the pull-down and its re-read then straddle the sample's end, where a re-read
    // scheduled a cycle early, or one the batched path dropped, shows in the state check (render cost review,
    // round 3).
    for (const std::uint8_t combo : { std::uint8_t { 0x61 }, std::uint8_t { 0x71 } })
        for (const double rate : { 88200.0, 96000.0 })
            for (const int beforeEnd : { -1, 0, 1, 2 })
        {
            Chip c = chipWith (Model::A, rate, ShapeSet::fitted);
            c.renderCheckStates (true);
            setOsc (c, 0, 65535.0, 2048.0f, ctrl::triangle | ctrl::gate);
            std::vector<float> y (4000, 0.0f);
            c.render (y.data(), 1000);
            while (! (((c.accumulator (0) >> 12) & 0xfffu) >= 3000u && ((c.accumulator (0) >> 12) & 0xfffu) <= 3500u))
                c.render (y.data(), 1);
            if (beforeEnd >= 0)
            {
                Chip probe = c;
                probe.render (y.data(), 1);
                REQUIRE (c.renderUntil (y.data(), 1, probe.cycle() - static_cast<std::uint64_t> (beforeEnd)) == 0); // the sample stays in progress
            }
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
            INFO ("control " << int (combo) << ", " << rate << " Hz" << ", write " << beforeEnd << " cycles before a sample's end (-1: at its start)" << ": rendered " << y.back() << ", chip "
                             << digitalOutput (d, Model::A, ShapeSet::fitted) << ", state mismatches " << c.renderStateMismatches());
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

TEST_CASE ("render: a frozen Model A pull-down whose gate the re-read finds high (a width below the pulled cell), with and without a ring source", "[render][defects]")
{
    // Render cost review, round 3: the sibling above with the width below the cell the pull-down lands on. The
    // re-read one cycle later must take the comparison of the frozen accumulator, which is high here; a re-read
    // that always closed the gate passed every other test. With oscillator 3 running and the ring bit set, the
    // re-read falls in a stretch with the source's flips (the sweep's event loop instead of its start).
    int pulled = 0, high = 0;
    for (const std::uint8_t combo : { std::uint8_t { 0x61 }, std::uint8_t { 0x71 } })
        for (const bool ring : { false, true })
            for (const float pw : { 16.0f, 400.0f, 900.0f, 2284.0f })
                for (const double rate : { 88200.0, 96000.0 })
                {
                    Chip c = chipWith (Model::A, rate, ShapeSet::fitted);
                    c.renderCheckStates (true);
                    setOsc (c, 0, 65535.0, 2048.0f, ctrl::triangle | ctrl::gate);
                    if (ring)
                        setOsc (c, 2, 3000.0, 2048.0f, 0x00); // runs, silent: the output stays comparable
                    std::vector<float> y (4000, 0.0f);
                    c.render (y.data(), 1000);
                    while (! (((c.accumulator (0) >> 12) & 0xfffu) >= 3000u && ((c.accumulator (0) >> 12) & 0xfffu) <= 3500u))
                        c.render (y.data(), 1);
                    Chip d = c;
                    const auto control = static_cast<std::uint8_t> (combo | (ring ? ctrl::ring : 0));
                    for (Chip* x : { &c, &d })
                    {
                        x->write (0x00, 0x00);
                        x->write (0x01, 0x00);
                        setOsc (*x, 0, 0.0, pw, control);
                    }
                    std::fill (y.begin(), y.end(), 0.0f);
                    c.render (y.data(), 4000);
                    d.advanceCycles (c.cycle() - d.cycle());
                    const bool wasPulled = (d.phase (0) & kPhaseMsb) == 0;
                    const bool gate = d.state().osc[0].pulseHigh;
                    pulled += wasPulled ? 1 : 0;
                    high += wasPulled && gate ? 1 : 0;
                    INFO ("control " << int (control) << ", width " << pw << ", " << rate << " Hz: rendered " << y.back() << ", chip "
                                     << digitalOutput (d, Model::A, ShapeSet::fitted) << ", pulled " << wasPulled << ", gate " << gate
                                     << ", state mismatches " << c.renderStateMismatches());
                    CHECK (std::abs (static_cast<double> (y.back()) - digitalOutput (d, Model::A, ShapeSet::fitted)) < 1.0e-6);
                    CHECK (c.renderStateMismatches() == 0);
                    CHECK (c.renderLargeCorrections() == 0);
                }
    INFO (pulled << " pulled down, " << high << " of them with the gate high");
    CHECK (high > 0);
}

TEST_CASE ("render: a frozen Model A pull-down against one crawling below one accumulator step (report, asserts nothing)", "[.render-frozen-crawl]")
{
    // Round 3 of the render cost review, kept as a report (third review: not reproduced there). Model A,
    // oscillator 1 a triangle at register 65535 until its cell (accumulator bits 23..12) is in 3000 .. 3500;
    // then, at a sample's start or 0 to 2 cycles before its end (renderUntil), the same write on two copies:
    // frequency register 0 (frozen) or 1e-7 (crawling: increment > 0, far below one accumulator step), width
    // 16, 900 or 2284, control 0x61 or 0x71. Both copies make the same digital steps (checked cycle by cycle
    // in a scratch tool); their rendered outputs over 64 samples are compared. On this branch (2026-11-19):
    // placeholder shapes, 0x71, 88.2 kHz, the write at a sample's start, every width: 0.0138 at sample 8 (the
    // write's own sample after the latency); every other case bit-identical; no state mismatch or large
    // correction on either copy. Which copy is right is not settled (docs/decisions.md).
    int cases = 0;
    double worst = 0.0;
    for (const ShapeSet shapes : { ShapeSet::fitted, ShapeSet::placeholder })
    for (const std::uint8_t combo : { std::uint8_t { 0x61 }, std::uint8_t { 0x71 } })
        for (const double rate : { 88200.0, 96000.0 })
            for (const float pw : { 16.0f, 900.0f, 2284.0f })
            for (const int beforeEnd : { -1, 0, 1, 2 })
            {
                Chip base = chipWith (Model::A, rate, shapes);
                base.renderCheckStates (true);
                setOsc (base, 0, 65535.0, 2048.0f, ctrl::triangle | ctrl::gate);
                std::vector<float> y (1000, 0.0f);
                base.render (y.data(), 1000);
                while (! (((base.accumulator (0) >> 12) & 0xfffu) >= 3000u && ((base.accumulator (0) >> 12) & 0xfffu) <= 3500u))
                    base.render (y.data(), 1);
                if (beforeEnd >= 0)
                {
                    Chip probe = base;
                    probe.render (y.data(), 1);
                    REQUIRE (base.renderUntil (y.data(), 1, probe.cycle() - static_cast<std::uint64_t> (beforeEnd)) == 0);
                }
                Chip frozen = base, crawling = base;
                setOsc (frozen, 0, 0.0, pw, combo);
                setOsc (crawling, 0, 1.0e-7, pw, combo);
                REQUIRE (frozen.increment (0) == 0);
                REQUIRE (crawling.increment (0) > 0);
                std::vector<float> yf (64, 0.0f), yc (64, 0.0f);
                frozen.render (yf.data(), 64);
                crawling.render (yc.data(), 64);
                double diff = 0.0;
                std::size_t at = 0;
                for (std::size_t i = 0; i < yf.size(); ++i)
                    if (std::abs (static_cast<double> (yf[i]) - static_cast<double> (yc[i])) > diff)
                    {
                        diff = std::abs (static_cast<double> (yf[i]) - static_cast<double> (yc[i]));
                        at = i;
                    }
                worst = std::max (worst, diff);
                if (diff > 1.0e-6)
                    std::printf ("%s, control %02x, width %4.0f, %6.0f Hz, write %2d cycles before a sample's end: largest difference %.4g at sample %zu (frozen %.6f, crawling %.6f)\n",
                                 shapes == ShapeSet::fitted ? "fitted" : "placeholder", combo, static_cast<double> (pw), rate, beforeEnd, diff, at, static_cast<double> (yf[at]),
                                 static_cast<double> (yc[at]));
                ++cases;
            }
    std::printf ("%d cases, largest difference %.4g\n", cases, worst);
    SUCCEED();
}

TEST_CASE ("render: a frozen Model A saw combination pulled down by a write sounds as one crawling below one accumulator step (any oscillator, any write position)", "[render][defects][frozen-pull]")
{
    // Third review, item 3: the case above on oscillators 2 and 3 (oscillator 3's ring target is oscillator 1,
    // which sees a pull-down one cycle later), with the write at a sample's start or 0 to 2 cycles before its
    // end (the landing may then still be pending at the sample's end, where the batched path must leave the line
    // to the general one). A copy with frequency 1e-7 (an increment far below one accumulator step) makes the
    // same digital steps; its line is a moving one, which takes the landing as a line event. Both must sound
    // the same.
    struct Case
    {
        ShapeSet shapes;
        double rate;
        bool widthWrite;
        std::uint8_t control;
    };
    const Case cases[] { { ShapeSet::placeholder, 128000.0, true, 0x71 }, { ShapeSet::placeholder, 96000.0, true, 0x61 },
                         { ShapeSet::fitted, 128000.0, false, 0x31 }, { ShapeSet::fitted, 96000.0, false, 0x71 } };
    int pulled = 0;
    double worst = 0.0;
    for (const auto& k : cases)
        for (const int p : { 1, 2 })
            for (const bool ring : { false, true })
                for (const int beforeEnd : { -1, 0, 1, 2 })
                {
                    const int target = p == 1 ? 2 : 0;
                    Chip c = chipWith (Model::A, k.rate, k.shapes);
                    c.renderCheckStates (true);
                    const std::uint8_t first = k.widthWrite ? k.control : std::uint8_t { 0x11 };
                    setOsc (c, p, 20000.0, 2016.0f, first);
                    if (ring)
                        setOsc (c, target, 3000.0, 2048.0f, ctrl::triangle | ctrl::ring | ctrl::gate);
                    std::vector<float> y (1000, 0.0f);
                    c.render (y.data(), 1000);
                    while (! (((c.accumulator (p) >> 12) & 0xfffu) >= 2900u && ((c.accumulator (p) >> 12) & 0xfffu) <= 3100u))
                        c.render (y.data(), 1);
                    setOsc (c, p, 0.0, 2016.0f, first); // frozen in the upper half
                    c.render (y.data(), 200);
                    if (beforeEnd >= 0)
                    {
                        Chip probe = c;
                        probe.render (y.data(), 1);
                        REQUIRE (c.renderUntil (y.data(), 1, probe.cycle() - static_cast<std::uint64_t> (beforeEnd)) == 0);
                    }
                    const auto large = c.renderLargeCorrections();
                    const auto mismatches = c.renderStateMismatches();
                    Chip crawl = c, d = c;
                    setOsc (c, p, 0.0, k.widthWrite ? 4057.0f : 2016.0f, k.control);
                    setOsc (d, p, 0.0, k.widthWrite ? 4057.0f : 2016.0f, k.control);
                    setOsc (crawl, p, 1.0e-7, k.widthWrite ? 4057.0f : 2016.0f, k.control);
                    REQUIRE (crawl.increment (p) > 0);
                    std::vector<float> yf (400, 0.0f), yc (400, 0.0f);
                    c.render (yf.data(), 400);
                    crawl.render (yc.data(), 400);
                    d.advanceCycles (c.cycle() - d.cycle());
                    double diff = 0.0;
                    for (std::size_t i = 0; i < yf.size(); ++i)
                        diff = std::max (diff, std::abs (static_cast<double> (yf[i]) - static_cast<double> (yc[i])));
                    worst = std::max (worst, diff);
                    pulled += (d.phase (p) & kPhaseMsb) == 0 ? 1 : 0;
                    INFO ((k.shapes == ShapeSet::fitted ? "fitted" : "placeholder") << ", " << k.rate << " Hz, " << (k.widthWrite ? "width write" : "waveform write") << ", control "
                                                                                   << int (k.control) << ", oscillator " << p + 1 << (ring ? ", ring target" : "") << ", write "
                                                                                   << beforeEnd << " cycles before a sample's end: frozen against crawling " << diff
                                                                                   << ", large corrections " << c.renderLargeCorrections() - large << ", state mismatches "
                                                                                   << c.renderStateMismatches() - mismatches << "; crawling: large corrections "
                                                                                   << crawl.renderLargeCorrections() - large << ", state mismatches " << crawl.renderStateMismatches() - mismatches);
                    // With a ring target the two differ by up to 1e-3 (writes inside a sample) and, for a waveform write
                    // right after render() (at a sample's start), by up to 0.05 on fitted shapes: an open difference,
                    // not settled which copy is right (docs/decisions.md, render cost step row 24); that position
                    // is printed, not held. A ring flip a cycle off differs by about 0.05.
                    if (ring && beforeEnd < 0)
                    {
                        if (diff > 2.0e-3)
                            std::printf ("open: %s, %.0f Hz, control %02x, oscillator %d, ring target, write at a sample's start: frozen against crawling %.4g\n",
                                         k.shapes == ShapeSet::fitted ? "fitted" : "placeholder", k.rate, k.control, p + 1, diff);
                    }
                    else
                        CHECK (diff < (ring ? 2.0e-3 : 1.0e-5));
                    CHECK (c.renderLargeCorrections() == large);
                    CHECK (c.renderStateMismatches() == mismatches);
                }
    INFO ("largest difference " << worst);
    CHECK (pulled == 64);
}

TEST_CASE ("render: the fitted Model A saw combinations pull down onto cells whose value the gate does not change", "[render][defects][tables]")
{
    // Third review: the gate's re-read one cycle after a frozen line's pull-down (renderScheduleReread) could
    // be scheduled at the pull-down's own cycle without any test noticing. That is equivalent only because of a
    // property of the fitted tables, asserted here so that a refit which breaks it fails: a Model A saw+pulse or
    // triangle+saw+pulse line is pulled down with its gate open only from an upper-half cell whose shape has
    // bit 11 clear, onto the cell 2048 below; there the shape is 0, so the output is 0 with the gate open or
    // closed and the re-read's time cannot change it. (A pull-down with the gate closed lands below the width,
    // where the gate stays closed.) The placeholder shapes do not have this property (935 such cells of
    // triangle+saw+pulse): there the equivalence does not hold and is not tested (docs/decisions.md).
    const auto& tables = vs::bside::chip::render::tablesFor (Model::A, ShapeSet::fitted);
    for (const std::uint8_t bits : { std::uint8_t { 6 }, std::uint8_t { 7 } })
    {
        const auto& st = tables.steps[static_cast<std::size_t> (vs::bside::chip::render::tableSlot (bits))];
        int pullCells = 0, nonZero = 0;
        for (std::size_t c = 2048; c < 4096; ++c)
            if ((st.shape[c] & 0x800u) == 0)
            {
                ++pullCells;
                nonZero += st.shape[c - 2048] != 0 ? 1 : 0;
            }
        INFO ("waveform bits " << int (bits) << ": " << pullCells << " upper-half cells with bit 11 clear, " << nonZero << " of them onto a cell that is not 0");
        CHECK (pullCells > 1000);
        CHECK (nonZero == 0);
    }
}
