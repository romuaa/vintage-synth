// Rendering against the chip's own digital output stream, and the smaller properties from the S2b-2
// review: no corrections over random patches, physical peak, a smooth start after activation, hysteresis
// of the Model A converter terms, a write right after render() at its exact cycle.

#include "FuzzSupport.h"
#include "RenderTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <functional>
#include <string>

using namespace bs_render_test;
namespace ctrl = vs::bside::chip::ctrl;

namespace
{
double i0 (double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 200; ++k)
    {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
        if (t < 1e-20 * s)
            break;
    }
    return s;
}

/** The rendered output of oscillator `obs` against the chip's own digital output stream (the converter
    value of every cycle): both windowed (Kaiser, beta 20), compared as complex amplitudes at the multiples
    of `lineCyc` (periods per cycle) up to maxHz, time-aligned (the render's latency removed). Returns the
    largest complex difference relative to the strongest line, in dB. The digital stream itself is sampled
    at 1 MHz and the rendering leads it by 0 to 1 cycle, so the difference grows with frequency; what the
    test catches are wrong shapes (an inverted ring, a missing or extra reset), tens of dB above that. */
Chip makeChipWith (Model m, double rate, vs::bside::chip::ShapeSet shapes)
{
    Chip c;
    Config cfg;
    cfg.model = m;
    cfg.internalRate = rate;
    cfg.shapes = shapes;
    c.prepare (cfg);
    return c;
}

double streamError (Model m, double rate, const std::function<void (Chip&)>& setup, double lineCyc, int obs, double maxHz = 8000.0,
                    vs::bside::chip::ShapeSet shapes = vs::bside::chip::ShapeSet::fitted)
{
    const auto& T = vs::bside::chip::render::tablesFor (m, shapes);
    Chip d = makeChipWith (m, rate, shapes);
    Chip r = makeChipWith (m, rate, shapes);
    setup (d);
    setup (r);
    const std::uint64_t c0 = 300000;
    d.advanceCycles (c0);
    const std::size_t len = std::size_t { 1 } << 19;
    std::vector<double> v (len), wd (len);
    double wds = 0.0;
    for (std::size_t c = 0; c < len; ++c)
    {
        d.stepCycle();
        v[c] = static_cast<double> (T.wave[d.output (obs) & 0xfffu]);
        const double rr = 2.0 * static_cast<double> (c) / static_cast<double> (len - 1) - 1.0;
        wd[c] = i0 (20.0 * std::sqrt (std::max (0.0, 1.0 - rr * rr)));
        wds += wd[c];
    }
    const double cps = cyclesPerSample (r);
    const int n0 = static_cast<int> (std::ceil (static_cast<double> (c0) / cps)) + 16;
    const int n = static_cast<int> (std::floor (static_cast<double> (len) / cps)) - 32;
    const auto y = renderSamples (r, n0 + n);
    std::vector<double> wr (static_cast<std::size_t> (n));
    double wrs = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double rr = 2.0 * i / (n - 1.0) - 1.0;
        wr[static_cast<std::size_t> (i)] = i0 (20.0 * std::sqrt (std::max (0.0, 1.0 - rr * rr)));
        wrs += wr[static_cast<std::size_t> (i)];
    }
    const int k = static_cast<int> (std::floor (maxHz / (lineCyc * 985248.0)));
    std::vector<std::complex<double>> D (static_cast<std::size_t> (k + 1)), R (static_cast<std::size_t> (k + 1));
    double strong = 0.0;
    constexpr double pi = 3.14159265358979323846;
    for (int h = 1; h <= k; ++h)
    {
        const double om = 2.0 * pi * h * lineCyc;
        std::complex<double> s = 0.0, t = 0.0;
        // the state after the step ending at cycle c0 + c + 1 holds over [c0 + c + 1, c0 + c + 2): its centre
        const std::complex<double> rot = std::polar (1.0, -om);
        std::complex<double> ph = std::polar (1.0, -om * (static_cast<double> (c0) + 1.5));
        for (std::size_t c = 0; c < len; ++c)
        {
            s += v[c] * wd[c] * ph;
            ph *= rot;
        }
        for (int i = 0; i < n; ++i)
        {
            const double tc = (n0 + i - Chip::kLatencySamples) * cps;
            t += static_cast<double> (y[static_cast<std::size_t> (n0 + i)]) * wr[static_cast<std::size_t> (i)] * std::polar (1.0, -om * tc);
        }
        D[static_cast<std::size_t> (h)] = 2.0 * s / wds;
        R[static_cast<std::size_t> (h)] = 2.0 * t / wrs;
        strong = std::max (strong, std::abs (D[static_cast<std::size_t> (h)]));
    }
    double err = 0.0;
    for (int h = 1; h <= k; ++h)
        err = std::max (err, std::abs (R[static_cast<std::size_t> (h)] - D[static_cast<std::size_t> (h)]) / strong);
    return 20.0 * std::log10 (err + 1e-300);
}

double regOf (double hz) { return hz * 16777216.0 / 985248.0; }
double lineOf (const Chip& c, int n) { return static_cast<double> (c.increment (n)) / 72057594037927936.0; }
} // namespace

TEST_CASE ("render: against the chip's own output stream (ring and sync from a pulled-down Model A source, pull-down gate)", "[render][stream]")
{
    struct Case
    {
        const char* name;
        Model m;
        std::function<void (Chip&)> setup;
        int lineOsc; // whose increment sets the lines
        double tol;  // dB at about 220 Hz (the 1 kHz case allows 12 dB more)
        vs::bside::chip::ShapeSet shapes = vs::bside::chip::ShapeSet::fitted;
        double lineDiv = 1.0; // the lines: the increment of lineOsc divided by this (ring: 5, source at 3/5)
    };
    for (const double hz : { 220.7, 1001.3 })
    {
        const double extra = hz > 500.0 ? 12.0 : 0.0;
        const std::vector<Case> cases {
            { "Model A saw (baseline)", Model::A, [=] (Chip& c) { setOsc (c, 1, regOf (hz), 2048.0f, 0x21); }, 1, -55.0 },
            { "Model A triangle ring, source triangle+saw (pulled down)", Model::A,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz * 0.6), 2048.0f, 0x30); setOsc (c, 1, regOf (hz), 2048.0f, 0x15); }, 1, -55.0 , vs::bside::chip::ShapeSet::fitted, 5.0 },
            { "Model A saw sync, source triangle+saw (pulled down: never syncs)", Model::A,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz / 1.37), 2048.0f, 0x30); setOsc (c, 1, regOf (hz), 2048.0f, 0x23); }, 1, -55.0 },
            // (with our fitted shapes every Model A saw combination is pulled down in its first upper-half
            // cell, so none of them ever syncs; a later pull-down is reached only through a pulse gate that
            // opens late: saw+pulse at width 3000 is pulled at once too, by the closed gate)
            { "Model A saw sync, source saw+pulse at width 1024 (pulled down: never syncs)", Model::A,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz / 1.37), 1024.0f, 0x60); setOsc (c, 1, regOf (hz), 2048.0f, 0x23); }, 1, -55.0 },
            { "Model A saw sync, source triangle+pulse (no pull-down: syncs)", Model::A,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz / 1.37), 2048.0f, 0x50); setOsc (c, 1, regOf (hz), 2048.0f, 0x23); }, 0, -50.0 },
            { "Model A triangle+pulse ring, source triangle+saw", Model::A,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz * 0.6), 2048.0f, 0x30); setOsc (c, 1, regOf (hz), 2048.0f, 0x55); }, 1, -30.0 , vs::bside::chip::ShapeSet::fitted, 5.0 },
            { "Model A saw+pulse width 1024 (pull-down at once)", Model::A,
              [=] (Chip& c) { setOsc (c, 1, regOf (hz), 1024.0f, 0x61); }, 1, -30.0 },
            // With the fitted shapes every pull-down happens at cell 0x800; the S2a placeholder shapes (the AND
            // of triangle and saw) keep bit 11 up to cell 0xc00, so their pull-down comes later in the upper
            // half: the pulse gate must then be re-read from the new line, and a source keeps its rise (syncs).
            { "Model B triangle ring, source saw", Model::B,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz * 0.6), 2048.0f, 0x20); setOsc (c, 1, regOf (hz), 2048.0f, 0x15); }, 1, -50.0 , vs::bside::chip::ShapeSet::fitted, 5.0 },
            { "Model B triangle ring, source silent", Model::B,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz * 0.6), 2048.0f, 0x00); setOsc (c, 1, regOf (hz), 2048.0f, 0x15); }, 1, -50.0 , vs::bside::chip::ShapeSet::fitted, 5.0 },
            { "Model B triangle ring, source triangle+saw (a table source, no pull-down)", Model::B,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz * 0.6), 2048.0f, 0x30); setOsc (c, 1, regOf (hz), 2048.0f, 0x15); }, 1, -50.0 , vs::bside::chip::ShapeSet::fitted, 5.0 },
            { "placeholder shapes, Model A triangle+saw alone (pulled down at 0xc00)", Model::A,
              [=] (Chip& c) { setOsc (c, 1, regOf (hz), 2048.0f, 0x31); }, 1, -30.0, vs::bside::chip::ShapeSet::placeholder },
            { "placeholder shapes, Model A triangle+saw+pulse width 1536 (pulled down at 0xc00, gate re-read)", Model::A,
              [=] (Chip& c) { setOsc (c, 1, regOf (hz), 1536.0f, 0x71); }, 1, -30.0, vs::bside::chip::ShapeSet::placeholder },
            { "placeholder shapes, Model A saw sync, source triangle+saw (pulled down at 0xc00: syncs)", Model::A,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz / 1.37), 2048.0f, 0x30); setOsc (c, 1, regOf (hz), 2048.0f, 0x23); }, 0, -45.0,
              vs::bside::chip::ShapeSet::placeholder },
            { "placeholder shapes, Model A triangle ring, source triangle+saw (top bit low after 0xc00)", Model::A,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz * 0.6), 2048.0f, 0x30); setOsc (c, 1, regOf (hz), 2048.0f, 0x15); }, 1, -50.0,
              vs::bside::chip::ShapeSet::placeholder, 5.0 },
        };
        for (const auto& cs : cases)
        {
            Chip probe = makeChipWith (cs.m, 96000.0, cs.shapes);
            cs.setup (probe);
            const double line = lineOf (probe, cs.lineOsc) / cs.lineDiv; // compared at the multiples of this line
            const double e = streamError (cs.m, 96000.0, cs.setup, line, 1, 8000.0, cs.shapes);
            INFO (cs.name << " at " << hz << " Hz: complex error " << e << " dB re the strongest line (allowed " << cs.tol + extra << ")");
            CHECK (e <= cs.tol + extra);
        }
    }
}

TEST_CASE ("render: no corrections and a physical peak over random patches without sync chains, both models", "[render][fuzz]")
{
    bst::Rng rng (77);
    const std::uint8_t waves[] { 0x10, 0x20, 0x40, 0x30, 0x50, 0x60, 0x70, 0x00, 0x80, 0x90 };
    int checked = 0;
    for (int trial = 0; trial < 160; ++trial)
    {
        const Model m = rng.chance (50) ? Model::A : Model::B;
        const double rate = rateConfigs()[rng.below (7)].internalRate;
        Chip c = makeChip (m, rate);
        std::uint8_t ct[3] {};
        for (int n = 0; n < 3; ++n)
            ct[n] = static_cast<std::uint8_t> (waves[rng.below (10)] | (rng.chance (50) ? ctrl::sync : 0) | (rng.chance (25) ? ctrl::ring : 0) | ctrl::gate);
        // no sync chains (a synced oscillator whose own source is synced too: 8.15.2 (d)'s exception)
        bool chain = false;
        for (int n = 0; n < 3; ++n)
            chain = chain || ((ct[n] & ctrl::sync) != 0 && (ct[vs::bside::chip::detail::syncSource (n)] & ctrl::sync) != 0);
        if (chain)
            continue;
        for (int n = 0; n < 3; ++n)
        {
            const int r = rng.range (0, 3);
            const double f = r == 0 ? static_cast<double> (rng.below (65536)) : r == 1 ? 60000.0 + rng.below (5536) + 0.37
                                     : r == 2 ? 2000.0 + rng.below (30000) * 1.0001 : rng.below (2000) + 0.5;
            const float pw = rng.chance (33) ? static_cast<float> (rng.below (4096)) + 0.5f : static_cast<float> (rng.below (4096));
            setOsc (c, n, f, pw, ct[n]);
        }
        const auto y = renderSamples (c, 20000);
        double peak = 0.0;
        bool finite = true;
        for (const float v : y)
        {
            peak = std::max (peak, static_cast<double> (std::abs (v)));
            finite = finite && std::isfinite (v);
        }
        INFO ("trial " << trial << ", " << (m == Model::A ? "Model A" : "Model B") << ", " << rate << " Hz, controls " << int (ct[0]) << " " << int (ct[1]) << " "
                       << int (ct[2]));
        CHECK (c.renderCorrections() == 0);
        CHECK (finite);
        // three voices of at most +-0.5 each (converter output minus mid-scale, envelope <= 1), plus the
        // band-limited overshoot of full steps on all three at once (Gibbs, about 9 to 12 % for these
        // kernels): 1.5 x 1.12 = 1.68; measured at most 1.671 over these patches
        CHECK (peak <= 1.7);
        ++checked;
    }
    CHECK (checked > 60);
}

TEST_CASE ("render: the first samples after activation or reset start on the chip's value (no step from 0)", "[render][start]")
{
    for (const Model m : { Model::A, Model::B })
    {
        Chip c = makeChip (m, 96000.0);
        setOsc (c, 0, 3000.0, 2048.0f, ctrl::saw); // gate off: the power-on envelope decays in release
        for (int round = 0; round < 2; ++round)
        {
            const auto y = renderSamples (c, 32);
            double jump = 0.0;
            for (std::size_t i = 1; i < y.size(); ++i)
                jump = std::max (jump, static_cast<double> (std::abs (y[i] - y[i - 1])));
            INFO ((m == Model::A ? "Model A" : "Model B") << (round == 0 ? ", first activation" : ", after reset()") << ": first sample " << y[0]
                                                        << ", largest change between samples " << jump);
            CHECK (std::abs (y[0]) > 0.2); // the power-on state is not silent ...
            CHECK (jump < 0.05);           // ... and the output starts on it
            c.reset();
            setOsc (c, 0, 3000.0, 2048.0f, ctrl::saw);
        }
    }
}

TEST_CASE ("render: Model A converter terms switch once on a slow glide and not on a vibrato around the limit", "[render][hysteresis]")
{
    // At 96 kHz index bit 4's square stops at 96000 / 2^8 = 375 Hz (dropped at 375 Hz, taken back below
    // 300 Hz): a glide from 225 to 525 Hz and back switches twice; a +-3 % vibrato around 375 Hz never.
    for (const int mode : { 0, 1 })
    {
        Chip c = makeChip (Model::A, 96000.0);
        const double limitHz = 96000.0 / 256.0;
        std::uint64_t switchesBefore = 0;
        for (int block = 0; block < 1500; ++block)
        {
            const double x = block / 1500.0;
            const double hz = mode == 0 ? limitHz * (0.6 + 0.8 * std::sin (3.14159265358979323846 * x)) // up and down once
                                        : limitHz * (1.0 + 0.03 * std::sin (2.0 * 3.14159265358979323846 * 12.0 * x));
            setOsc (c, 0, regOf (hz), 2048.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
            renderSamples (c, 64);
            if (block == 10)
                switchesBefore = c.renderConverterSwitches();
        }
        const auto switches = c.renderConverterSwitches() - switchesBefore;
        INFO ((mode == 0 ? "glide 225 -> 525 -> 225 Hz" : "vibrato +-3 % around 375 Hz") << ": " << switches << " switches");
        CHECK (switches == (mode == 0 ? 2u : 0u));
    }
}

TEST_CASE ("render: a write right after render() lands at its own cycle", "[render][timing]")
{
    // render() completes whole samples; a write made then happens at the current cycle, which lies up to
    // one cycle before the end of the last sample: its step must sit at that cycle, not at the sample end
    for (const double rate : { 88200.0, 192000.0 })
    {
        Chip c = makeChip (Model::B, rate);
        setOsc (c, 0, 0.0, 256.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
        std::vector<float> y;
        std::vector<std::uint64_t> at;
        std::vector<float> one (1);
        bst::Rng r (9);
        std::uint64_t next = 30000;
        bool tri = false;
        while (at.size() < 24 || c.cycle() < next + 3000)
        {
            one[0] = 0.0f;
            c.render (one.data(), 1);
            y.push_back (one[0]);
            if (at.size() < 24 && c.cycle() >= next)
            {
                tri = ! tri;
                c.write (0x04, static_cast<std::uint8_t> ((tri ? ctrl::triangle : ctrl::saw) | ctrl::gate));
                at.push_back (c.cycle());
                next = c.cycle() + static_cast<std::uint64_t> (r.range (400, 1500));
            }
        }
        const double cps = cyclesPerSample (c);
        const auto& blep = Chip::ShortBlep::get();
        auto blepCum = [&] (double t) {
            if (t <= -8.0)
                return 0.0;
            if (t > 8.0)
                return 1.0;
            const int j = static_cast<int> (std::ceil (t)) - 1;
            return static_cast<double> (blep.step (j, static_cast<float> (t - j))) + (j >= 0 ? 1.0 : 0.0);
        };
        std::vector<double> levels;
        for (std::size_t i = 0; i <= at.size(); ++i)
        {
            const double from = i == 0 ? 26000.0 : static_cast<double> (at[i - 1]);
            const double to = i < at.size() ? static_cast<double> (at[i]) : static_cast<double> (c.cycle());
            levels.push_back (static_cast<double> (y[static_cast<std::size_t> (0.5 * (from + to) / cps) + Chip::kLatencySamples]));
        }
        auto model = [&] (double shift) {
            double worst = 0.0;
            for (auto s = static_cast<std::size_t> (26000.0 / cps); s + 1 < y.size(); ++s)
            {
                double v = levels[0];
                for (std::size_t i = 0; i < at.size(); ++i)
                    v += (levels[i + 1] - levels[i]) * blepCum (static_cast<double> (s) - ((static_cast<double> (at[i]) + shift) / cps + Chip::kLatencySamples));
                worst = std::max (worst, std::abs (static_cast<double> (y[s]) - v));
            }
            return worst;
        };
        const double exact = model (0.0), half = model (0.5);
        INFO (rate << " Hz: max deviation " << exact << " (half a cycle later " << half << ")");
        CHECK (exact < 1.0e-5);
        CHECK (half > 100.0 * exact);
    }
}
