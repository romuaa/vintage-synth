// Rendering against the chip's own digital output stream, and the smaller properties from the S2b-2
// review: no corrections over random patches, physical peak, a smooth start after activation, hysteresis
// of the Model A converter terms, a write right after render() at its exact cycle.

#include "FuzzSupport.h"
#include "RenderTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

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
Chip makeChipWith (Model m, double rate, vs::bside::chip::ShapeSet shapes, bool authentic = false)
{
    Chip c;
    Config cfg;
    cfg.model = m;
    cfg.internalRate = rate;
    cfg.shapes = shapes;
    cfg.authentic = authentic;
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
        int obs = 1;          // the oscillator observed (and the only one with a gate)
        double hzScale = 1.0; // hz of this case = the loop's hz x this
    };
    for (const double baseHz : { 220.7, 1001.3 })
    {
        const double hz = baseHz;
        const double extra = baseHz > 500.0 ? 12.0 : 0.0;
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
            // noise with the saw on Model A (per-cycle mode): pulled down where the output's bit 11 is clear,
            // which depends on the noise output; the renderer takes these sources' flips and resets from the chip.
            // Under write-back the noise output locks to 0, the source is pulled down at once and never syncs:
            // the target runs free (compared at its own lines)
            { "Model A saw sync, source noise+saw (0xA0, per-cycle)", Model::A,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz * 0.377), 2048.0f, 0xa0); setOsc (c, 1, regOf (hz), 2048.0f, 0x23); }, 1, -45.0 },
            { "Model A saw sync, source noise+triangle+saw+pulse (0xF0, per-cycle)", Model::A,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz * 0.377), 2048.0f, 0xf0); setOsc (c, 1, regOf (hz), 2048.0f, 0x23); }, 1, -45.0 },
            { "Model A triangle ring, source noise+saw (0xA0, per-cycle)", Model::A,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz * 0.6), 2048.0f, 0xa0); setOsc (c, 1, regOf (hz), 2048.0f, 0x15); }, 1, -45.0,
              vs::bside::chip::ShapeSet::fitted, 5.0 },
            { "Model A triangle ring, source noise+triangle+saw+pulse (0xF0, per-cycle)", Model::A,
              [=] (Chip& c) { setOsc (c, 0, regOf (hz * 0.6), 2048.0f, 0xf0); setOsc (c, 1, regOf (hz), 2048.0f, 0x15); }, 1, -45.0,
              vs::bside::chip::ShapeSet::fitted, 5.0 },
            // oscillator 1 reads oscillator 3 before oscillator 3's pull-down: a one-cycle glitch per rise
            { "Model A triangle ring on oscillator 1, source oscillator 3 triangle+saw (pulled down, one-cycle glitch)", Model::A,
              [=] (Chip& c) { setOsc (c, 2, regOf (hz * 0.6), 2048.0f, 0x30); setOsc (c, 0, regOf (hz), 2048.0f, 0x15); }, 0, -50.0,
              vs::bside::chip::ShapeSet::fitted, 5.0, 0 },
            { "Model A triangle ring on oscillator 1, source oscillator 3 noise+saw (per-cycle, glitch)", Model::A,
              [=] (Chip& c) { setOsc (c, 2, regOf (hz * 0.6), 2048.0f, 0xa0); setOsc (c, 0, regOf (hz), 2048.0f, 0x15); }, 0, -45.0,
              vs::bside::chip::ShapeSet::fitted, 5.0, 0 },
            { "placeholder shapes, Model A triangle ring on oscillator 1, source oscillator 3 triangle+saw (later pull-down, glitch)", Model::A,
              [=] (Chip& c) { setOsc (c, 2, regOf (hz * 0.6), 2048.0f, 0x30); setOsc (c, 0, regOf (hz), 2048.0f, 0x15); }, 0, -45.0,
              vs::bside::chip::ShapeSet::placeholder, 5.0, 0 },
            { "Model B triangle ring on oscillator 1, source oscillator 3 saw", Model::B,
              [=] (Chip& c) { setOsc (c, 2, regOf (hz * 0.6), 2048.0f, 0x20); setOsc (c, 0, regOf (hz), 2048.0f, 0x15); }, 0, -50.0,
              vs::bside::chip::ShapeSet::fitted, 5.0, 0 },
            // the pulse gate at the rise, decided at whole cycles: widths just below 2048 (placeholder shapes)
            // (fixed frequencies, the reviewer's: 800 Hz in the first pass, 2346 Hz in the second)
            { "placeholder shapes, Model A saw+pulse width 2047 (gate at the rise, whole cycles)", Model::A,
              [=] (Chip& c) { setOsc (c, 1, regOf (baseHz < 500.0 ? 800.0 : 2346.0), 2047.0f, 0x61); }, 1, -30.0, vs::bside::chip::ShapeSet::placeholder },
            { "placeholder shapes, Model A saw+pulse width 2040 (gate at the rise, whole cycles)", Model::A,
              [=] (Chip& c) { setOsc (c, 1, regOf (baseHz < 500.0 ? 1500.0 : 2346.0), 2040.0f, 0x61); }, 1, -30.0, vs::bside::chip::ShapeSet::placeholder },
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
            const double e = streamError (cs.m, 96000.0, cs.setup, line, cs.obs, 8000.0, cs.shapes);
            UNSCOPED_INFO (cs.name << " at " << hz << " Hz: " << e);
            INFO (cs.name << " at " << hz << " Hz: complex error " << e << " dB re the strongest line (allowed " << cs.tol + extra << ")");
            CHECK (e <= cs.tol + extra);
        }
    }
}

TEST_CASE ("render: no corrections and a physical peak over random patches without sync chains, both models", "[render][fuzz]")
{
    bst::Rng rng (77);
    // every waveform combination, the noise ones included (per-cycle mode: 0xa0, 0xb0, 0xc0, 0xe0, 0xf0)
    const std::uint8_t waves[] { 0x10, 0x20, 0x40, 0x30, 0x50, 0x60, 0x70, 0x00, 0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0 };
    int checked = 0;
    for (int trial = 0; trial < 200; ++trial)
    {
        const Model m = rng.chance (50) ? Model::A : Model::B;
        const double rate = rateConfigs()[rng.below (7)].internalRate;
        Chip c = makeChip (m, rate);
        std::uint8_t ct[3] {};
        for (int n = 0; n < 3; ++n)
            ct[n] = static_cast<std::uint8_t> (waves[rng.below (16)] | (rng.chance (50) ? ctrl::sync : 0) | (rng.chance (25) ? ctrl::ring : 0) | ctrl::gate);
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

namespace
{
/** One write of a schedule: all three oscillators (kind 0: controls), or oscillator n only (1: frequency,
    2: width, 3: the test bit, 7: the gate, 8: sync or ring); 4: one raw register write (reg, value);
    5: powerOnKeepingTime(); 6: clearEnvelopeCounters (n). f, pw, ct hold the generator's view of the
    oscillators after the write (kinds 0-3, 7, 8 write them through setOscillator). */
struct ScheduledWrite
{
    std::uint64_t at = 0;
    int kind = 0;
    int n = 0;
    double f[3] {};
    float pw[3] {};
    std::uint8_t ct[3] {};
    std::uint8_t reg = 0, value = 0;
};

void applyWrite (Chip& c, const ScheduledWrite& w)
{
    if (w.kind == 4)
        c.write (w.reg, w.value);
    else if (w.kind == 5)
        c.powerOnKeepingTime();
    else if (w.kind == 6)
        c.clearEnvelopeCounters (w.n);
    else
        for (int q = 0; q < 3; ++q)
            if (w.kind == 0 || q == w.n)
                setOsc (c, q, w.f[q], w.pw[q], w.ct[q]);
}

struct Schedule
{
    Model m = Model::A;
    vs::bside::chip::ShapeSet shapes = vs::bside::chip::ShapeSet::fitted;
    double rate = 96000.0;
    bool authentic = false;
    ScheduledWrite initial;
    std::vector<ScheduledWrite> writes;
    std::uint64_t total = 240000; // cycles rendered
};

/** A random patch and a random schedule of writes. Patch: every one of the 16 waveform values, sync and
    ring (no sync chains, 8.15.2 (d), at the controls the generator writes; a raw write may make one),
    both models and shape sets, every internal rate, Authentic or not. Writes, at random cycles (dense:
    30 to 600 cycles apart in a third of the schedules, else 300 to 6000): all three controls, a frequency,
    a width, the test bit, the gate, sync or ring, a raw single register byte, powerOnKeepingTime,
    clearEnvelopeCounters. Widths are whole: a fractional width is rendered continuous (pulseWidthSmooth),
    the chip compares the integer part. */
Schedule randomSchedule (bst::Rng& rng)
{
    Schedule s;
    s.m = rng.chance (50) ? Model::A : Model::B;
    s.shapes = rng.chance (33) ? vs::bside::chip::ShapeSet::placeholder : vs::bside::chip::ShapeSet::fitted;
    s.rate = rateConfigs()[rng.below (7)].internalRate;
    s.authentic = rng.chance (25);
    const bool syncRing = rng.chance (60);
    const bool dense = rng.chance (33);
    auto noChain = [] (std::uint8_t* ct) {
        for (int n = 0; n < 3; ++n)
            if ((ct[n] & ctrl::sync) != 0 && (ct[vs::bside::chip::detail::syncSource (n)] & ctrl::sync) != 0)
                ct[n] = static_cast<std::uint8_t> (ct[n] & ~ctrl::sync);
    };
    // a write of oscillator n alone may only change n: it drops its own sync bit if that would make a chain
    auto noChainAt = [] (std::uint8_t* ct, int n) {
        const int src = vs::bside::chip::detail::syncSource (n), tgt = vs::bside::chip::detail::syncTarget (n);
        if ((ct[n] & ctrl::sync) != 0 && ((ct[src] & ctrl::sync) != 0 || (ct[tgt] & ctrl::sync) != 0))
            ct[n] = static_cast<std::uint8_t> (ct[n] & ~ctrl::sync);
    };
    auto pick = [&] (std::uint8_t* ct) {
        for (int n = 0; n < 3; ++n)
            ct[n] = static_cast<std::uint8_t> ((rng.below (16) << 4) | (syncRing && rng.chance (50) ? ctrl::sync : 0) | (syncRing && rng.chance (25) ? ctrl::ring : 0)
                                               | (rng.chance (90) ? ctrl::gate : 0));
        noChain (ct);
    };
    auto freq = [&] {
        const int r = rng.range (0, 4);
        return r == 0 ? static_cast<double> (rng.below (65536)) : r == 1 ? 60000.0 + rng.below (5536) + 0.37
               : r == 2 ? 2000.0 + rng.below (30000) * 1.0001 : r == 3 ? rng.below (2000) + 0.5 : 65535.0;
    };
    ScheduledWrite cur;
    pick (cur.ct);
    for (int n = 0; n < 3; ++n)
    {
        cur.f[n] = freq();
        cur.pw[n] = static_cast<float> (rng.below (4096));
    }
    s.initial = cur;
    std::uint64_t at = 20000;
    while (true)
    {
        at += static_cast<std::uint64_t> (dense ? rng.range (30, 600) : rng.range (300, 6000));
        if (at + 3000 > s.total)
            break;
        ScheduledWrite w = cur;
        w.at = at;
        const unsigned r = rng.below (100);
        w.kind = r < 25 ? 0 : r < 40 ? 1 : r < 52 ? 2 : r < 60 ? 3 : r < 70 ? 7 : r < 80 ? 8 : r < 92 ? 4 : r < 96 ? 6 : 5;
        w.n = static_cast<int> (rng.below (3));
        if (w.kind == 0)
            pick (w.ct);
        else if (w.kind == 1)
            w.f[w.n] = rng.chance (20) ? freq() : rng.below (65536) + 0.25;
        else if (w.kind == 2)
            w.pw[w.n] = static_cast<float> (rng.below (4096));
        else if (w.kind == 3)
            w.ct[w.n] = static_cast<std::uint8_t> (w.ct[w.n] ^ ctrl::test);
        else if (w.kind == 7)
            w.ct[w.n] = static_cast<std::uint8_t> (w.ct[w.n] ^ ctrl::gate);
        else if (w.kind == 8)
        {
            w.ct[w.n] = static_cast<std::uint8_t> (w.ct[w.n] ^ (rng.chance (50) ? ctrl::sync : ctrl::ring));
            noChainAt (w.ct, w.n);
        }
        else if (w.kind == 4)
        {
            // one byte of an oscillator's registers (0..6), or of the filter and volume (0x15..0x18)
            w.reg = static_cast<std::uint8_t> (rng.chance (85) ? 7 * w.n + static_cast<int> (rng.below (7)) : 0x15 + static_cast<int> (rng.below (4)));
            w.value = static_cast<std::uint8_t> (rng.below (256));
            if (w.reg % 7 == 4 && w.reg < 21)
            {
                const int q = w.reg / 7;
                w.ct[q] = w.value;
                noChainAt (w.ct, q);
                w.value = w.ct[q];
            }
        }
        s.writes.push_back (w);
        cur = w;
    }
    return s;
}

struct ScheduleResult
{
    std::uint64_t corrections = 0;
    std::uint64_t largeCorrections = 0; // above 1e-4 of a period
    std::uint64_t stateMismatches = 0;  // Chip::renderCheckStates
    double worst = 0.0; // the largest difference of the smoothed signals (full scale 1 per oscillator)
    double rms = 0.0;
};

/** Renders a schedule (whole blocks with render() up to just before each write, then renderUntil() to the
    write's cycle) and compares it with the chip's own digital output stream of the same schedule (every
    cycle: the three converter values times their envelopes). Both are smoothed by the same Gaussian (sigma
    20 cycles, about -3 dB at 9 kHz): a misprediction (a wrong line, a lost edge, a wrong ring state, until
    the next correction or for good) shows as a difference of up to the full scale; what remains without one
    is the chip's own sampling at the clock rate (a combined waveform with narrow cells, sampled every cycle,
    beats against its period; the rendering is the continuous waveform), up to about 0.25 at the highest
    frequencies, 0.04 typical. */
ScheduleResult runSchedule (const Schedule& s)
{
    Chip r = makeChipWith (s.m, s.rate, s.shapes, s.authentic);
    r.renderCheckStates (true);
    applyWrite (r, s.initial);
    const double cps = cyclesPerSample (r);
    std::vector<float> y;
    std::vector<float> block (256);
    auto renderTo = [&] (std::uint64_t at) {
        while (static_cast<double> (r.cycle()) + (256.0 + 2.0) * cps < static_cast<double> (at))
        {
            std::fill (block.begin(), block.end(), 0.0f);
            r.render (block.data(), 256);
            y.insert (y.end(), block.begin(), block.end());
        }
        while (r.cycle() < at)
        {
            std::fill (block.begin(), block.end(), 0.0f);
            const int done = r.renderUntil (block.data(), 256, at);
            y.insert (y.end(), block.begin(), block.begin() + done);
        }
    };
    for (const auto& w : s.writes)
    {
        renderTo (w.at);
        applyWrite (r, w);
    }
    renderTo (s.total);

    // the digital twin: the same writes at the same cycles; the value after each step holds for one cycle
    const auto& T = vs::bside::chip::render::tablesFor (s.m, s.shapes);
    Chip d = makeChipWith (s.m, s.rate, s.shapes, s.authentic);
    applyWrite (d, s.initial);
    std::vector<double> v;
    v.reserve (static_cast<std::size_t> (s.total));
    std::size_t next = 0;
    while (d.cycle() < s.total)
    {
        while (next < s.writes.size() && s.writes[next].at == d.cycle())
            applyWrite (d, s.writes[next++]);
        d.stepCycle();
        double sum = 0.0;
        for (int n = 0; n < 3; ++n)
            sum += static_cast<double> (T.wave[d.output (n) & 0xfffu]) * static_cast<double> (T.envelope[d.envelope (n)]);
        v.push_back (sum);
    }

    constexpr double sigma = 20.0;
    const int reach = static_cast<int> (4.0 * sigma);
    const auto lpDigital = [&] (double tc) {
        double sum = 0.0, wsum = 0.0;
        const auto c0 = static_cast<std::int64_t> (std::floor (tc));
        for (std::int64_t c = c0 - reach; c <= c0 + reach; ++c)
        {
            if (c < 1 || c > static_cast<std::int64_t> (v.size()))
                continue;
            const double x = (static_cast<double> (c) + 0.5 - tc) / sigma; // v[c - 1] holds over [c, c + 1)
            const double g = std::exp (-0.5 * x * x);
            sum += g * v[static_cast<std::size_t> (c - 1)];
            wsum += g;
        }
        return sum / wsum;
    };
    const double sigmaS = sigma / cps;
    const int reachS = static_cast<int> (std::ceil (4.0 * sigmaS));
    const auto lpRendered = [&] (int i) {
        double sum = 0.0, wsum = 0.0;
        for (int k = i - reachS; k <= i + reachS; ++k)
        {
            const double x = (k - i) / sigmaS;
            const double g = std::exp (-0.5 * x * x);
            sum += g * static_cast<double> (y[static_cast<std::size_t> (k)]);
            wsum += g;
        }
        return sum / wsum;
    };
    ScheduleResult res;
    res.corrections = r.renderCorrections();
    res.largeCorrections = r.renderLargeCorrections();
    res.stateMismatches = r.renderStateMismatches();
    const int from = static_cast<int> (8000.0 / cps) + reachS; // after the attack
    const int to = static_cast<int> (y.size()) - reachS - 1;
    double sum = 0.0;
    int count = 0;
    for (int i = from; i < to; ++i)
    {
        const double tc = (i - Chip::kLatencySamples) * cps; // rendered sample i: the start of internal sample i - latency
        const double e = std::abs (lpRendered (i) - lpDigital (tc));
        res.worst = std::max (res.worst, e);
        sum += e * e;
        ++count;
    }
    res.rms = std::sqrt (sum / std::max (count, 1));
    return res;
}
} // namespace

TEST_CASE ("render: random write schedules against the chip's own stream (every waveform, sync, ring, every write kind)", "[render][stream][schedule]")
{
    bst::Rng rng (2024);
    double worst = 0.0, rms = 0.0;
    for (int trial = 0; trial < 120; ++trial)
    {
        const Schedule s = randomSchedule (rng);
        const auto res = runSchedule (s);
        INFO ("trial " << trial << ", " << (s.m == Model::A ? "Model A" : "Model B") << ", " << s.rate << " Hz" << (s.authentic ? ", Authentic" : "") << ", "
                       << s.writes.size() << " writes, controls " << int (s.initial.ct[0]) << " " << int (s.initial.ct[1]) << " " << int (s.initial.ct[2])
                       << ": corrections " << res.corrections << " (" << res.largeCorrections << " large), state mismatches " << res.stateMismatches
                       << ", worst " << res.worst << ", rms " << res.rms);
        CHECK (res.largeCorrections == 0);
        CHECK (res.stateMismatches == 0);
        CHECK (res.worst <= 0.3);
        // (no bound on the rms: a static combined waveform at the top of the range, sampled at the clock,
        // reaches 0.1 rms against the continuous rendering, with nothing wrong; the worst value and the
        // state check are the defect detectors)
        worst = std::max (worst, res.worst);
        rms = std::max (rms, res.rms);
    }
    UNSCOPED_INFO ("largest smoothed difference " << worst << ", largest rms " << rms);
}

TEST_CASE ("render: against the chip's own stream, two cases the random schedules found, and oscillator 1's glitch", "[render][stream][schedule]")
{
    // (0) oscillator 1's ring source oscillator 3 is a Model A triangle+saw, pulled down at every rise:
    // oscillator 1 sees the rise for one cycle (8.15.2 (c) step 1), a one-cycle flip of its triangle at
    // every rise; without it the smoothed difference is about 0.016, with it 0.001 to 0.002
    for (const double f : { 3800.0, 17000.0 })
    {
        Schedule s;
        s.m = Model::A;
        s.rate = 96000.0;
        s.total = 160000;
        s.initial.ct[0] = 0x15;
        s.initial.ct[2] = 0x30;
        s.initial.f[0] = f;
        s.initial.f[2] = 0.6 * f;
        s.initial.pw[0] = s.initial.pw[1] = s.initial.pw[2] = 2048.0f;
        const auto res = runSchedule (s);
        INFO ("triangle ring on oscillator 1, source oscillator 3 triangle+saw, frequency " << f << ": worst " << res.worst);
        CHECK (res.worst <= 0.006);
    }
    // (1) oscillator 1's ring source (oscillator 3) in step with it: same frequency and phase from power-on,
    // so both top bits change at the same instant: the source's change must still be taken (an inverted ring
    // for half of every period otherwise)
    for (const double f : { 1000.0, 65535.0 })
    {
        Schedule s;
        s.m = Model::B;
        s.rate = 128000.0;
        s.total = 120000;
        s.initial.ct[0] = 0x15;
        s.initial.ct[2] = 0x20;
        s.initial.f[0] = s.initial.f[2] = f;
        s.initial.pw[0] = s.initial.pw[1] = s.initial.pw[2] = 2048.0f;
        const auto res = runSchedule (s);
        INFO ("triangle ring on oscillator 1, source saw at the same frequency " << f << ": worst " << res.worst);
        CHECK (res.worst <= 0.05);
    }
    // (2) a pulse synced by a per-cycle source (noise+saw) after a write: a reset that the sweep reaches more
    // than a cycle late must still emit the gate's falling edge one cycle after it (a pulse stuck high until
    // the next rising edge, which then inverts it, otherwise)
    {
        Schedule s;
        s.m = Model::B;
        s.rate = 96000.0;
        s.total = 90000;
        s.initial.ct[0] = 0x86;
        s.initial.ct[1] = 0x01;
        s.initial.ct[2] = 0x60;
        s.initial.f[0] = 64348.37;
        s.initial.f[1] = 61099.37;
        s.initial.f[2] = 18003.6;
        s.initial.pw[0] = 1539.0f;
        s.initial.pw[1] = 3047.0f;
        s.initial.pw[2] = 864.0f;
        ScheduledWrite w = s.initial;
        w.at = 47402;
        w.kind = 0;
        w.ct[0] = 0xa4;
        w.ct[1] = 0x43;
        w.f[1] = 56585.25;
        s.writes.push_back (w);
        const auto res = runSchedule (s);
        INFO ("pulse synced by a noise+saw source, after a write: worst " << res.worst);
        CHECK (res.worst <= 0.1);
    }
}

namespace
{
/** A schedule from full oscillator states: the initial one and writes {cycle, kind, n, states}. */
Schedule fixedSchedule (Model m, vs::bside::chip::ShapeSet shapes, double rate, std::uint64_t total, const double (&f)[3], const float (&pw)[3], const std::uint8_t (&ct)[3])
{
    Schedule s;
    s.m = m;
    s.shapes = shapes;
    s.rate = rate;
    s.total = total;
    for (int n = 0; n < 3; ++n)
    {
        s.initial.f[n] = f[n];
        s.initial.pw[n] = pw[n];
        s.initial.ct[n] = ct[n];
    }
    return s;
}

ScheduledWrite after (const Schedule& s, std::uint64_t at, int kind, int n)
{
    ScheduledWrite w = s.writes.empty() ? s.initial : s.writes.back();
    w.at = at;
    w.kind = kind;
    w.n = n;
    return w;
}
} // namespace

// Hidden, for manual use: 3 000 random schedules (as "[schedule]"), on all cores.
TEST_CASE ("render: random write schedules against the chip's own stream, long run", "[.render-schedule-long]")
{
    constexpr int count = 3000;
    std::vector<Schedule> all;
    bst::Rng rng (4711);
    for (int i = 0; i < count; ++i)
        all.push_back (randomSchedule (rng));
    std::atomic<int> next { 0 };
    std::mutex mu;
    std::vector<std::string> bad;
    double worst = 0.0;
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::max (1u, std::thread::hardware_concurrency()); ++t)
        pool.emplace_back ([&] {
            for (int i; (i = next++) < count;)
            {
                const auto r = runSchedule (all[static_cast<std::size_t> (i)]);
                std::lock_guard<std::mutex> lock (mu);
                worst = std::max (worst, r.worst);
                if (r.largeCorrections != 0 || r.stateMismatches != 0 || r.worst > 0.3)
                    bad.push_back (std::to_string (i) + " (large " + std::to_string (r.largeCorrections) + ", mismatches " + std::to_string (r.stateMismatches)
                                   + ", worst " + std::to_string (r.worst) + ")");
            }
        });
    for (auto& th : pool)
        th.join();
    std::string list;
    for (const auto& b : bad)
        list += " " + b;
    WARN (count << " schedules, largest smoothed difference " << worst << "; with a finding:" << list);
    CHECK (bad.empty());
}

TEST_CASE ("render: the third review's cases against the chip's own stream and the state check", "[render][stream][schedule]")
{
    using vs::bside::chip::ShapeSet;
    auto check = [] (const char* name, const Schedule& s, double tol) {
        const auto res = runSchedule (s);
        INFO (name << ": large corrections " << res.largeCorrections << ", state mismatches " << res.stateMismatches << ", smoothed difference " << res.worst);
        CHECK (res.largeCorrections == 0);
        CHECK (res.stateMismatches == 0);
        CHECK (res.worst <= tol);
    };
    // F1: a ring target whose source is in per-cycle mode (Model A noise+saw: its top bit never changes
    // again) must take the source's top bit from the accumulator at a write, not from the line (which
    // carries offU); a write anywhere (here oscillator 3's gate) recomputed it, inverted, for good
    for (const std::uint64_t w : { 40133u, 40269u })
    {
        Schedule s = fixedSchedule (Model::A, ShapeSet::fitted, 384000.0, 60000, { 61632.37, 11633.96, 65427.37 }, { 2048.0f, 2048.0f, 2048.0f }, { 0x23, 0x15, 0x21 });
        auto w1 = after (s, 30000, 0, 0);
        w1.ct[0] = 0xa1;
        s.writes.push_back (w1);
        auto w2 = after (s, w, 7, 2);
        w2.ct[2] = 0x20;
        s.writes.push_back (w2);
        check ("F1: ring from a noise+saw source, a write elsewhere", s, 0.3);
    }
    // F2: a write that stops the source's rise (here its test bit) after the source's line has crossed: the
    // target's predicted reset must be dropped
    for (const std::uint64_t w : { 40553u, 41185u })
    {
        Schedule s = fixedSchedule (Model::B, ShapeSet::fitted, 192000.0, 60000, { 35658.0, 27409.0, 26524.0 }, { 2048.0f, 2048.0f, 2048.0f }, { 0x21, 0x21, 0x23 });
        auto w1 = after (s, 30000, 0, 0);
        w1.ct[0] = 0x23;
        w1.ct[2] = 0x21;
        s.writes.push_back (w1);
        auto w2 = after (s, w, 3, 2);
        w2.ct[2] = 0x29;
        s.writes.push_back (w2);
        check ("F2: the source's test bit set after its line crossed", s, 0.3);
    }
    // F4 (placeholder shapes): a width write one step before the rise; the gate forced open at the rise from
    // the stale comparison must be re-read a cycle later with the new width
    {
        Schedule s = fixedSchedule (Model::A, ShapeSet::placeholder, 384000.0, 60000, { 64407.37, 0.0, 0.0 }, { 405.0f, 2048.0f, 2048.0f }, { 0x71, 0x00, 0x00 });
        Chip probe = makeChipWith (Model::A, 384000.0, ShapeSet::placeholder);
        applyWrite (probe, s.initial);
        probe.advanceCycles (30000);
        while ((probe.accumulator (0) >> 16) != 0x7f)
            probe.stepCycle();
        auto w1 = after (s, probe.cycle(), 2, 0);
        w1.pw[0] = 2646.0f;
        s.writes.push_back (w1);
        check ("F4: a width write one step before the rise (placeholder shapes)", s, 0.3);
    }
    // F5: a reset reached late loses the falling edge (row 21) and, at a narrow width, the rising edge after it
    {
        Schedule s = fixedSchedule (Model::B, ShapeSet::fitted, 352800.0, 120000, { 61432.37, 63771.37, 63235.37 }, { 2048.0f, 2.0f, 2048.0f }, { 0x23, 0x41, 0x21 });
        auto w1 = after (s, 30000, 0, 0);
        w1.ct[0] = 0xe1;
        w1.ct[1] = 0x43;
        s.writes.push_back (w1);
        check ("F5: a late reset of a narrow pulse", s, 0.3);
    }
}

namespace
{
struct NearWriteResult
{
    std::uint64_t corrections = 0, large = 0, mismatches = 0;
};

/** One patch of the review's harness kind: a random patch (noise combinations, sync, ring, test bit, both
    models and shape sets, every rate, Authentic in a quarter), blocks of 256 samples, and after a third of
    them a write 1 to 40 cycles into the next sample. mode 0: mixed (the next three and a test-bit toggle),
    1: a frequency, 2: a width, 3: all three controls. */
NearWriteResult nearWritePatch (int mode, std::uint64_t seed)
{
    static constexpr std::uint8_t waves[] { 0x10, 0x20, 0x40, 0x30, 0x50, 0x60, 0x70, 0x00, 0x80, 0x90, 0xa0, 0xc0, 0xf0 };
    bst::Rng rng (seed);
    const Model m = rng.chance (50) ? Model::A : Model::B;
    const auto shapes = rng.chance (33) ? vs::bside::chip::ShapeSet::placeholder : vs::bside::chip::ShapeSet::fitted;
    Chip c = makeChipWith (m, rateConfigs()[rng.below (7)].internalRate, shapes, rng.chance (25));
    c.renderCheckStates (true);
    std::uint8_t ct[3] {};
    auto pick = [&] {
        for (int n = 0; n < 3; ++n)
            ct[n] = static_cast<std::uint8_t> (waves[rng.below (static_cast<std::uint32_t> (std::size (waves)))] | (rng.chance (50) ? ctrl::sync : 0)
                                               | (rng.chance (25) ? ctrl::ring : 0) | ctrl::gate | (rng.below (40) == 0 ? ctrl::test : 0));
        for (int n = 0; n < 3; ++n)
            if ((ct[n] & ctrl::sync) != 0 && (ct[vs::bside::chip::detail::syncSource (n)] & ctrl::sync) != 0)
                ct[n] = static_cast<std::uint8_t> (ct[n] & ~ctrl::sync);
    };
    pick();
    double f[3];
    float pw[3];
    for (int n = 0; n < 3; ++n)
    {
        const int r = rng.range (0, 4);
        f[n] = r == 0 ? static_cast<double> (rng.below (65536)) : r == 1 ? 60000.0 + rng.below (5536) + 0.37
               : r == 2 ? 2000.0 + rng.below (30000) * 1.0001 : r == 3 ? rng.below (2000) + 0.5 : 65535.0;
        pw[n] = static_cast<float> (rng.below (4096));
        setOsc (c, n, f[n], pw[n], ct[n]);
    }
    std::vector<float> y (256), part (64);
    for (int b = 0; b < 80; ++b)
    {
        c.render (y.data(), 256);
        if (! rng.chance (33))
            continue;
        c.renderUntil (part.data(), 64, c.cycle() + 1 + rng.below (40));
        const int n = static_cast<int> (rng.below (3));
        const int kind = mode == 0 ? static_cast<int> (rng.below (4)) : mode;
        if (kind == 3)
        {
            pick();
            for (int q = 0; q < 3; ++q)
                setOsc (c, q, f[q], pw[q], ct[q]);
            continue;
        }
        if (kind == 1)
            f[n] = rng.below (65536) + 0.25;
        else if (kind == 2)
            pw[n] = static_cast<float> (rng.below (4096));
        else
            ct[n] = static_cast<std::uint8_t> (ct[n] ^ ctrl::test);
        setOsc (c, n, f[n], pw[n], ct[n]);
    }
    return { c.renderCorrections(), c.renderLargeCorrections(), c.renderStateMismatches() };
}
} // namespace

TEST_CASE ("render: writes a few cycles into a sample, no prediction error above rounding", "[render][corrections]")
{
    // The review's harness, smaller (nearWritePatch, 300 patches per write kind). Corrections below 1e-4 of
    // a period round the sub-cycle time of a sync reset (docs/decisions.md S2b-2 row 17); a larger one is a
    // prediction the chip did not make (a pull-down, a reset, a ring state). The white-box state check
    // (Chip::renderCheckStates) runs throughout.
    const char* names[] { "mixed writes", "frequency writes", "width writes", "control writes" };
    std::uint64_t all = 0;
    for (int mode = 0; mode < 4; ++mode)
    {
        std::uint64_t large = 0, mismatches = 0;
        for (int trial = 0; trial < 300; ++trial)
        {
            const auto r = nearWritePatch (mode, static_cast<std::uint64_t> (1000003 * trial + 77 * mode + 5));
            large += r.large;
            mismatches += r.mismatches;
            all += r.corrections;
        }
        INFO (names[mode] << ": " << large << " corrections above 1e-4 of a period, " << mismatches << " state mismatches in 300 patches");
        CHECK (large == 0);
        CHECK (mismatches == 0);
    }
    UNSCOPED_INFO ("corrections in all " << all);
}

TEST_CASE ("render: writes a few cycles into a sample, the long run's findings", "[render][corrections]")
{
    // Patches of the long run (index i: write kind i % 4, seed 7777777 + i) that found defects, fixed since:
    // 10395, 33935, 47331 a late reset at width 0 made a falling edge (the comparison is always high there);
    // 27567 a flip back carried across a write that had taken the ring state afresh; 39314 a width write one
    // step before the rise re-read the gate from the continuous line instead of the chip's first landing;
    // 6591 a digital sync reset taken as the confirmation of a pending pull-down (third review, F3)
    for (const int i : { 6591, 10395, 27567, 33935, 39314, 47331 })
    {
        const auto r = nearWritePatch (i % 4, static_cast<std::uint64_t> (7777777 + i));
        INFO ("patch " << i << ": " << r.large << " corrections above 1e-4 of a period, " << r.mismatches << " state mismatches");
        CHECK (r.large == 0);
        CHECK (r.mismatches == 0);
    }
}

// Hidden, for manual use: the same over 50 000 patches (12 500 per write kind), on all cores.
TEST_CASE ("render: writes a few cycles into a sample, long run", "[.render-corrections-long]")
{
    constexpr int perMode = 12500;
    std::atomic<int> next { 0 };
    std::atomic<std::uint64_t> large { 0 }, mismatches { 0 }, patchesLarge { 0 }, patchesMismatch { 0 };
    std::mutex mu;
    std::vector<int> bad;
    std::vector<std::thread> pool;
    const unsigned threads = std::max (1u, std::thread::hardware_concurrency());
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back ([&] {
            for (int i; (i = next++) < 4 * perMode;)
            {
                const int mode = i % 4;
                const auto r = nearWritePatch (mode, static_cast<std::uint64_t> (7777777 + i));
                large += r.large;
                mismatches += r.mismatches;
                patchesLarge += r.large != 0 ? 1u : 0u;
                patchesMismatch += r.mismatches != 0 ? 1u : 0u;
                if (r.large != 0 || r.mismatches != 0)
                {
                    std::lock_guard<std::mutex> lock (mu);
                    bad.push_back (i);
                }
            }
        });
    for (auto& th : pool)
        th.join();
    std::sort (bad.begin(), bad.end());
    std::string list;
    for (const int i : bad)
        list += " " + std::to_string (i);
    WARN ("patches with a finding (index; write kind = index % 4):" << list);
    WARN (4 * perMode << " patches: " << large.load() << " corrections above 1e-4 of a period (in " << patchesLarge.load() << " patches), " << mismatches.load()
                      << " state mismatches (in " << patchesMismatch.load() << " patches)");
    CHECK (large.load() == 0);
    CHECK (mismatches.load() == 0);
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

TEST_CASE ("render: Model A converter terms switch at their limits on a slow glide, at most once on a vibrato around a limit", "[render][hysteresis]")
{
    // At 96 kHz index bit 4's square toggles once per sample at 96000 / 2^8 = 375 Hz (bit 3's at 187.5 Hz):
    // a bit is dropped at its limit and taken back only below half of it. Cases: a glide 150 -> 525 -> 150 Hz
    // switches three times (bit 3 dropped at 187.5 Hz, bit 4 at 375 Hz; on the way down bit 4 back below
    // 187.5 Hz, bit 3 not, which would need 93.75 Hz); a +-3 % vibrato around 375 Hz never (it starts at the
    // limit, where the bit is already left out); the reviewers' +-12 % vibrato around 90 % of the limit
    // (crossing it on every cycle, 23 switches in 2 s with the old band of 0.8) at most once.
    struct Case
    {
        const char* name;
        double centre, depth, cycles;
        bool glide;
        unsigned most, least;
    };
    const Case cases[] { { "glide 150 -> 525 -> 150 Hz", 0.0, 0.0, 0.0, true, 3, 3 },
                         { "vibrato +-3 % around 375 Hz", 1.0, 0.03, 12.0, false, 0, 0 },
                         { "vibrato +-12 % around 337.5 Hz (90 % of the limit), 6 Hz for 2 s", 0.9, 0.12, 12.0, false, 1, 0 } };
    for (const auto& cs : cases)
    {
        Chip c = makeChip (Model::A, 96000.0);
        const double limitHz = 96000.0 / 256.0;
        const int blocks = cs.glide ? 1500 : 3000; // 64 samples each: 1 s or 2 s
        std::uint64_t switchesBefore = 0;
        for (int block = 0; block < blocks; ++block)
        {
            const double x = block / static_cast<double> (blocks);
            const double hz = cs.glide ? limitHz * (0.4 + 1.0 * std::sin (3.14159265358979323846 * x)) // up and down once
                                       : limitHz * cs.centre * (1.0 + cs.depth * std::sin (2.0 * 3.14159265358979323846 * cs.cycles * x));
            setOsc (c, 0, regOf (hz), 2048.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
            renderSamples (c, 64);
            if (block == 10)
                switchesBefore = c.renderConverterSwitches();
        }
        const auto switches = c.renderConverterSwitches() - switchesBefore;
        INFO (cs.name << ": " << switches << " switches");
        CHECK (switches <= cs.most);
        CHECK (switches >= cs.least);
    }
}

TEST_CASE ("render: a change of waveform starts the converter terms afresh and counts no switch", "[render][hysteresis]")
{
    // saw and pulse (no converter terms) alternating at 80 % of the limit: the saw's terms are found anew on
    // each change (no hysteresis carried over from the pulse, whose state is not a converter state), and a
    // change of waveform is no switch
    Chip c = makeChip (Model::A, 96000.0);
    const double hz = 0.8 * 96000.0 / 256.0;
    for (int block = 0; block < 400; ++block)
    {
        const bool saw = (block / 20) % 2 == 0;
        setOsc (c, 0, regOf (hz), 2048.0f, static_cast<std::uint8_t> ((saw ? ctrl::saw : ctrl::pulse) | ctrl::gate));
        renderSamples (c, 64);
    }
    CHECK (c.renderConverterSwitches() == 0u);
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
