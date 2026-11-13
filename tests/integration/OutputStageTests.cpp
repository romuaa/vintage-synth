// Wave 4: the output stage (docs/decisions.md, "Output level"): the fixed output trim and the soft
// limiter. The limiter block on its own (transparency, ceiling, transients, block-size
// independence) and through the processor (headroom of the loudest chord, ceiling on sustained
// material, limiter off = the plain path, parameter sweeps, non-finite values, distortion).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "IntegrationRig.h"
#include "common/fx/SoftLimiter.h"
#include "vintage/params/ParameterLayout.h"

#include <cstdio>
#include <limits>
#include <random>

using namespace vs::itest;
using vs::fx::SoftLimiter;

namespace
{
double peakDbOf (const Stereo& s, size_t from, size_t to)
{
    return toDb (std::max (peakAbs (s.l, from, to), peakAbs (s.r, from, to)));
}

double dbToGain (double db) { return std::pow (10.0, db / 20.0); }

bool allFiniteSamples (const Stereo& s)
{
    for (size_t i = 0; i < s.l.size(); ++i)
        if (! std::isfinite (s.l[i]) || ! std::isfinite (s.r[i]))
            return false;
    return true;
}

/** A stereo test signal: a few detuned sines (a chord) with a slow beat, peak about `peak`. */
Stereo chordSignal (int n, double fs, double peak, int seed = 1)
{
    Stereo s;
    s.l.resize (static_cast<size_t> (n));
    s.r.resize (static_cast<size_t> (n));
    const double f[] { 110.0, 138.6, 164.8, 220.7, 277.2 };
    std::mt19937 rng (static_cast<unsigned> (seed));
    std::uniform_real_distribution<double> ph (0.0, 6.283);
    double p[5], q[5];
    for (int k = 0; k < 5; ++k)
    {
        p[k] = ph (rng);
        q[k] = ph (rng);
    }
    for (int i = 0; i < n; ++i)
    {
        double a = 0.0, b = 0.0;
        for (int k = 0; k < 5; ++k)
        {
            a += std::sin (6.283185307179586 * f[k] * i / fs + p[k]);
            b += std::sin (6.283185307179586 * f[k] * 1.001 * i / fs + q[k]);
        }
        s.l[static_cast<size_t> (i)] = static_cast<float> (a * peak / 5.0);
        s.r[static_cast<size_t> (i)] = static_cast<float> (b * peak / 5.0);
    }
    return s;
}

Stereo runLimiter (Stereo s, double fs, float ceilingDb, int block)
{
    SoftLimiter lim;
    lim.prepare (fs);
    lim.reset (ceilingDb);
    const int n = static_cast<int> (s.l.size());
    for (int pos = 0; pos < n; pos += block)
    {
        lim.setCeilingDb (ceilingDb);
        lim.process (s.l.data() + pos, s.r.data() + pos, std::min (block, n - pos));
    }
    return s;
}

/** Level of (y - a x) relative to y, a the least-squares gain: what the limiter added beyond a
    plain gain change (gain modulation = distortion), in dB, over [from, to). */
double residualDb (const Stereo& x, const Stereo& y, size_t from, size_t to)
{
    double xy = 0.0, xx = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        xy += static_cast<double> (x.l[i]) * y.l[i] + static_cast<double> (x.r[i]) * y.r[i];
        xx += static_cast<double> (x.l[i]) * x.l[i] + static_cast<double> (x.r[i]) * x.r[i];
    }
    const double a = xy / std::max (xx, 1.0e-30);
    double num = 0.0, den = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        const double dl = y.l[i] - a * x.l[i], dr = y.r[i] - a * x.r[i];
        num += dl * dl + dr * dr;
        den += static_cast<double> (y.l[i]) * y.l[i] + static_cast<double> (y.r[i]) * y.r[i];
    }
    return 10.0 * std::log10 (std::max (num, 1.0e-30) / std::max (den, 1.0e-30));
}

/** residualDb per window of `window` samples, energy-averaged: gain changes slower than the
    window are a level change, not distortion, and are fitted away. */
double windowedResidualDb (const Stereo& x, const Stereo& y, size_t from, size_t to, size_t window)
{
    double num = 0.0, den = 0.0;
    for (size_t w = from; w + window <= to; w += window)
    {
        double xy = 0.0, xx = 0.0, yy = 0.0;
        for (size_t i = w; i < w + window; ++i)
        {
            xy += static_cast<double> (x.l[i]) * y.l[i] + static_cast<double> (x.r[i]) * y.r[i];
            xx += static_cast<double> (x.l[i]) * x.l[i] + static_cast<double> (x.r[i]) * x.r[i];
            yy += static_cast<double> (y.l[i]) * y.l[i] + static_cast<double> (y.r[i]) * y.r[i];
        }
        const double a = xy / std::max (xx, 1.0e-30);
        for (size_t i = w; i < w + window; ++i)
        {
            const double dl = y.l[i] - a * x.l[i], dr = y.r[i] - a * x.r[i];
            num += dl * dl + dr * dr;
        }
        den += yy;
    }
    return 10.0 * std::log10 (std::max (num, 1.0e-30) / std::max (den, 1.0e-30));
}

/** The loudest chord the instrument can play: Dual, 2 x 16 voices, velocity 127. */
std::vector<Midi> fullChord (int at = 0)
{
    std::vector<Midi> midi;
    for (int n = 0; n < 16; ++n)
        midi.push_back (noteOn (at, 36 + 3 * n, 127));
    return midi;
}

void setExtremeEffects (Rig& rig)
{
    rig.set ("fx.chorus.mode", 2.0f);
    rig.set ("fx.chorus.mix", 1.0f);
    rig.set ("fx.delay.level", 1.0f);
    rig.set ("fx.delay.feedback", 1.0f);
    rig.set ("fx.delay.time", vs::map::delayTimeFromSec (0.25f));
    rig.set ("fx.reverb.level", 1.0f);
    rig.set ("fx.reverb.decay", 1.0f);
}

constexpr double kCeilingToleranceDb = 0.001; // float rounding of the gain only
} // namespace

// =================================================================================================
// The limiter block
// =================================================================================================

TEST_CASE ("Soft limiter: the static curve is continuous, has slope 1 at the knee and stays below the ceiling", "[wave4][limiter]")
{
    for (const double c : { -12.0, -6.0, -1.0, 0.0 })
    {
        const double start = c - SoftLimiter::kKneeDb;
        CHECK (std::abs (SoftLimiter::curveDb (start - 10.0, c) - (start - 10.0)) < 1.0e-12);
        CHECK (std::abs (SoftLimiter::curveDb (start + 1.0e-6, c) - (start + 1.0e-6)) < 1.0e-9);
        double prev = SoftLimiter::curveDb (start, c);
        for (double in = start + 0.01; in < c + 60.0; in += 0.01)
        {
            const double out = SoftLimiter::curveDb (in, c);
            CHECK (out < c);
            CHECK (out > prev);       // monotonic
            CHECK (out - prev < 0.0101); // slope <= 1
            prev = out;
        }
    }
}

TEST_CASE ("Soft limiter: below the knee the output is bit-identical to the input", "[wave4][limiter]")
{
    const double fs = 48000.0;
    const float ceiling = GENERATE (-12.0f, -1.0f, 0.0f);
    const double kneeStart = dbToGain (ceiling - SoftLimiter::kKneeDb);
    const auto in = chordSignal (48000, fs, kneeStart * 0.999);
    REQUIRE (std::max (peakAbs (in.l, 0, in.l.size()), peakAbs (in.r, 0, in.r.size())) < kneeStart);
    REQUIRE (std::max (peakAbs (in.l, 0, in.l.size()), peakAbs (in.r, 0, in.r.size())) > kneeStart * 0.9);
    CHECK (bitIdentical (runLimiter (in, fs, ceiling, 512), in));

    // After a loud burst the gain returns to exactly 1: once the envelope has fallen below the
    // knee, the samples are untouched again.
    auto burst = chordSignal (240000, fs, kneeStart * 0.5);
    for (size_t i = 1000; i < 6000; ++i)
    {
        burst.l[i] *= 8.0f;
        burst.r[i] *= 8.0f;
    }
    const auto out = runLimiter (burst, fs, ceiling, 512);
    CHECK_FALSE (sameBits (std::vector<float> (out.l.begin() + 1000, out.l.begin() + 6000),
                           std::vector<float> (burst.l.begin() + 1000, burst.l.begin() + 6000)));
    // The slow detector falls from 4x to below the knee in ln(7) slow time constants.
    const size_t settled = 6000 + static_cast<size_t> (fs * (SoftLimiter::kHoldSeconds + 2.5 * SoftLimiter::kSlowReleaseSeconds));
    REQUIRE (settled + 48000 < burst.l.size());
    CHECK (sameBits (std::vector<float> (out.l.begin() + static_cast<std::ptrdiff_t> (settled), out.l.end()),
                     std::vector<float> (burst.l.begin() + static_cast<std::ptrdiff_t> (settled), burst.l.end())));
}

TEST_CASE ("Soft limiter: no sample above the ceiling, sustained or on a hard transient", "[wave4][limiter]")
{
    const double fs = GENERATE (44100.0, 48000.0, 192000.0);
    const float ceiling = GENERATE (-12.0f, -1.0f, 0.0f);
    INFO ("rate " << fs << ", ceiling " << ceiling);
    // Sustained: a chord at +6, +12 and +24 dB over the ceiling.
    for (const double over : { 6.0, 12.0, 24.0 })
    {
        const auto out = runLimiter (chordSignal (static_cast<int> (fs), fs, dbToGain (ceiling + over)), fs, ceiling, 256);
        CHECK (peakDbOf (out, 0, out.l.size()) <= ceiling + kCeilingToleranceDb);
        CHECK (peakDbOf (out, out.l.size() / 2, out.l.size()) > ceiling - 1.0); // limited, not squashed
    }
    // Hard transient: silence, then a full-scale square wave 24 dB over the ceiling on the next sample.
    Stereo step;
    step.l.assign (static_cast<size_t> (fs / 4), 0.0f);
    step.r.assign (step.l.size(), 0.0f);
    for (size_t i = 1000; i < step.l.size(); ++i)
        step.l[i] = step.r[i] = static_cast<float> (dbToGain (ceiling + 24.0) * ((i / 100) % 2 == 0 ? 1.0 : -1.0));
    const auto out = runLimiter (step, fs, ceiling, 64);
    CHECK (peakDbOf (out, 0, out.l.size()) <= ceiling + kCeilingToleranceDb);
}

TEST_CASE ("Soft limiter: block-size independent, also while the ceiling moves", "[wave4][limiter]")
{
    const double fs = 48000.0;
    const auto in = chordSignal (24000, fs, 2.0);
    auto run = [&] (int block) {
        SoftLimiter lim;
        lim.prepare (fs);
        lim.reset (-1.0f);
        Stereo s = in;
        // The ceiling changes at fixed samples (a host splits its blocks there), any block size.
        const int changes[] { 3000, 7001, 12000 };
        const float ceilings[] { -6.0f, std::numeric_limits<float>::quiet_NaN(), -12.0f };
        int pos = 0, next = 0;
        float c = -1.0f;
        while (pos < 24000)
        {
            if (next < 3 && pos == changes[next])
                c = ceilings[next++];
            int len = std::min (block, 24000 - pos);
            if (next < 3)
                len = std::min (len, changes[next] - pos);
            lim.setCeilingDb (c);
            lim.process (s.l.data() + pos, s.r.data() + pos, len);
            pos += len;
        }
        CHECK (std::abs (lim.currentCeilingDb() + 12.0f) < 1.0e-6f);
        return s;
    };
    const auto ref = run (512);
    for (const int block : { 1, 7, 61, 4096 })
    {
        INFO ("block " << block);
        CHECK (bitIdentical (run (block), ref));
    }
}

// =================================================================================================
// Through the processor
// =================================================================================================

TEST_CASE ("Output trim: the loudest dry chord peaks at or below -1 dBFS at the default master volume", "[wave4][output]")
{
    Rig rig (48000.0, 512);
    rig.set ("key.mode", 1);
    rig.set ("master.limiter", 0); // the trim alone
    rig.prepare();
    const auto out = rig.render (rig.samples (3.0), fullChord(), 512);
    const double peak = peakDbOf (out, 0, out.l.size());
    INFO ("peak " << peak << " dBFS");
    CHECK (peak <= -1.0);
    CHECK (peak > -4.0); // the trim is the documented -6.02 dB, not more
}

TEST_CASE ("Limiter through the processor: the ceiling holds for the loudest chord, dry and with extreme effects", "[wave4][output]")
{
    const float ceiling = GENERATE (-12.0f, -1.0f, 0.0f);
    const int effects = GENERATE (0, 1);
    INFO ("ceiling " << ceiling << ", effects " << effects);
    Rig rig (48000.0, 512);
    rig.set ("key.mode", 1);
    rig.set ("master.volume", 6.0f);
    rig.set ("master.limiter.ceiling", ceiling);
    if (effects == 1)
        setExtremeEffects (rig);
    rig.prepare();
    const auto out = rig.render (rig.samples (3.0), fullChord(), 512);
    CHECK (allFiniteSamples (out));
    CHECK (peakDbOf (out, 0, out.l.size()) <= ceiling + kCeilingToleranceDb);
    CHECK (peakDbOf (out, out.l.size() / 2, out.l.size()) > ceiling - 1.5); // sustained material sits near the ceiling
    CHECK (rig.proc.getGuardTrips() == 0);
}

TEST_CASE ("Limiter through the processor: distortion of a chord 6 dB over the ceiling, and the transient overshoot", "[wave4][output]")
{
    // The limiter-off output is exactly the limiter's input (same path, no limiter), so the two
    // renders isolate what the limiter does.
    const float ceiling = -1.0f;
    auto render = [&] (bool on, float masterDb) {
        Rig rig (48000.0, 512);
        rig.set ("key.mode", 1);
        rig.set ("master.volume", masterDb);
        rig.set ("master.limiter", on ? 1.0f : 0.0f);
        rig.set ("master.limiter.ceiling", ceiling);
        rig.prepare();
        return rig.render (rig.samples (3.0), fullChord (rig.samples (0.1)), 512);
    };
    const auto probe = render (false, -6.0f);
    const size_t from = 48000, to = probe.l.size(); // the sustained part (1 s ... 3 s)
    const double master = -6.0 + (ceiling + 6.0 - peakDbOf (probe, from, to));
    REQUIRE (master <= 6.0);
    const auto dry = render (false, static_cast<float> (master));
    const auto lim = render (true, static_cast<float> (master));
    const double overIn = peakDbOf (dry, from, to) - ceiling;
    const double pumping = residualDb (dry, lim, from, to);                 // incl. slow level changes
    const double dist = windowedResidualDb (dry, lim, from, to, 960);       // 20 ms windows: distortion
    const double peakOut = peakDbOf (lim, 0, lim.l.size());
    std::printf ("limiter, Dual chord vel 127, input peak %.2f dB over the %.1f dBFS ceiling: distortion (residual, 20 ms fits) %.1f dB, "
                 "residual of one fit over 2 s (incl. level changes) %.1f dB, output peak %.3f dBFS (overshoot %.4f dB incl. the onset), "
                 "level change %.2f dB\n",
                 overIn, static_cast<double> (ceiling), dist, pumping, peakOut, peakOut - ceiling,
                 toDb (rms (lim.l, from, to) / rms (dry.l, from, to)));
    CHECK (std::abs (overIn - 6.0) < 0.05);
    CHECK (dist < -38.0);
    CHECK (peakOut <= ceiling + kCeilingToleranceDb); // zero overshoot, also on the chord's onset
}

TEST_CASE ("Limiter through the processor: block-size independent with ceiling automation", "[wave4][output]")
{
    auto render = [] (int block) {
        Rig rig (48000.0, 512);
        rig.set ("key.mode", 1);
        rig.set ("master.volume", 6.0f);
        rig.prepare();
        std::vector<Rig::Change> ch;
        for (int k = 1; k <= 8; ++k)
            ch.push_back ({ 4000 * k, [k] (Rig& r) { r.set ("master.limiter.ceiling", -1.5f * static_cast<float> (k)); } });
        ch.push_back ({ 40000, [] (Rig& r) { r.set ("master.limiter", 0); } });
        ch.push_back ({ 44000, [] (Rig& r) { r.set ("master.limiter", 1); } });
        return rig.render (60000, fullChord (100), block, ch);
    };
    const auto ref = render (512);
    for (const int block : { 61, 7 })
    {
        INFO ("block " << block);
        const auto got = render (block);
        CHECK (diffDb (ref, got) < -120.0);
    }
}

TEST_CASE ("Limiter through the processor: a ceiling sweep does not click; non-finite values are safe", "[wave4][output]")
{
    auto render = [] (bool sweep, float bad) {
        Rig rig (48000.0, 512);
        rig.set ("key.mode", 1);
        rig.set ("master.volume", 6.0f);
        rig.prepare();
        std::vector<Rig::Change> ch;
        if (sweep)
        {
            // 0 -> -12 dB and back in 0.5 dB steps every 10 ms (an automation lane at block rate).
            for (int k = 0; k <= 48; ++k)
            {
                const float c = -0.5f * static_cast<float> (k <= 24 ? k : 48 - k);
                ch.push_back ({ 24000 + 480 * k, [c] (Rig& r) { r.set ("master.limiter.ceiling", c); } });
            }
            auto* raw = rig.proc.getState().getRawParameterValue ("master.limiter.ceiling");
            auto* sw = rig.proc.getState().getRawParameterValue ("master.limiter");
            ch.push_back ({ 60000, [raw, sw, bad] (Rig&) { raw->store (bad); sw->store (bad); } });
        }
        auto out = rig.render (72000, fullChord (100), 256, ch);
        return std::make_pair (std::move (out), rig.proc.getLimiter().currentCeilingDb());
    };
    const float bad = GENERATE (std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
    const auto [still, c0] = render (false, 0.0f);
    const auto [moved, c1] = render (true, bad);
    CHECK (allFiniteSamples (moved));
    // A click is a sample-to-sample jump larger than the music itself makes.
    auto maxStep = [] (const Stereo& s, size_t from, size_t to) {
        double m = 0.0;
        for (size_t i = from + 1; i < to; ++i)
            m = std::max ({ m, static_cast<double> (std::abs (s.l[i] - s.l[i - 1])), static_cast<double> (std::abs (s.r[i] - s.r[i - 1])) });
        return m;
    };
    const double staticStep = maxStep (still, 24000, 48000);
    const double sweepStep = maxStep (moved, 24000, 48000);
    INFO ("largest step: static " << staticStep << ", during the sweep " << sweepStep);
    CHECK (sweepStep <= staticStep * 1.05);
    // Non-finite host values: the parameter defaults (limiter on, -1 dB).
    CHECK (std::abs (c1 - vs::kDefaultLimiterCeilingDb) < 1.0e-6f);
    CHECK (std::abs (c0 - vs::kDefaultLimiterCeilingDb) < 1.0e-6f);
    CHECK (peakDbOf (moved, 62000, 72000) <= vs::kDefaultLimiterCeilingDb + kCeilingToleranceDb);
}

TEST_CASE ("Report: output peaks of the loudest chord with the trim, limiter on and off", "[.report]")
{
    struct Cfg
    {
        const char* name;
        int fx; // 0 dry, 1 moderate, 2 extreme
    };
    for (const Cfg c : { Cfg { "dry", 0 }, Cfg { "moderate effects (chorus I, delay 0.3, reverb 0.3)", 1 },
                         Cfg { "extreme effects (chorus II full, delay 1.0 at 95 % / 250 ms, reverb 1.0 at 20 s)", 2 } })
        for (const int on : { 0, 1 })
        {
            Rig rig (48000.0, 512);
            rig.set ("key.mode", 1);
            rig.set ("master.limiter", static_cast<float> (on));
            if (c.fx == 1)
            {
                rig.set ("fx.chorus.mode", 1.0f);
                rig.set ("fx.delay.level", 0.3f);
                rig.set ("fx.reverb.level", 0.3f);
            }
            if (c.fx == 2)
                setExtremeEffects (rig);
            rig.prepare();
            const auto out = rig.render (rig.samples (6.0), fullChord(), 512);
            int clamped = 0;
            for (size_t i = 0; i < out.l.size(); ++i)
                clamped += (std::abs (out.l[i]) >= 3.999f || std::abs (out.r[i]) >= 3.999f) ? 1 : 0;
            std::printf ("peak, Dual 32 voices vel 127, master -6 dB, trim -6.02 dB, limiter %s, %s: %.2f dBFS, samples at the clamp %d\n",
                         on ? "on (-1 dB)" : "off", c.name, peakDbOf (out, 0, out.l.size()), clamped);
        }
}
