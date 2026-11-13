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
    lim.reset (ceilingDb, true);
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
        lim.reset (-1.0f, true);
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

namespace
{
/** The loudest chord at master +6 dB (about 10 dB over the default ceiling), with changes. The
    rest of the path is the same with the limiter off, so a limiter-off render is exactly the
    limiter's input. */
Stereo loudRender (std::vector<Rig::Change> ch, bool limiterOn = true, int samples = 72000)
{
    Rig rig (48000.0, 512);
    rig.set ("key.mode", 1);
    rig.set ("master.volume", 6.0f);
    rig.set ("master.limiter", limiterOn ? 1.0f : 0.0f);
    rig.prepare();
    return rig.render (samples, fullChord (100), 256, std::move (ch));
}

/** The gain ratio y / x per sample in dB, where x is loud enough to measure it; the largest
    change between neighbouring samples in [from, to). With x the limiter's input this is the
    applied gain (which also jumps when the instant-attack detector catches a new peak); with x a
    render that differs only in a parameter, the detector moves cancel (they depend on the input
    alone) and only the parameter's own effect on the gain remains. */
double maxGainStepDb (const Stereo& in, const Stereo& out, size_t from, size_t to)
{
    const double floor = 0.05 * std::max (peakAbs (in.l, from, to), peakAbs (in.r, from, to));
    double worst = 0.0;
    for (const auto* ch : { &in.l, &in.r })
    {
        const auto& x = *ch;
        const auto& y = ch == &in.l ? out.l : out.r;
        for (size_t i = from + 1; i < to; ++i)
            if (std::abs (x[i]) > floor && std::abs (x[i - 1]) > floor)
            {
                const double a = toDb (std::abs (y[i]) / std::abs (x[i]));
                const double b = toDb (std::abs (y[i - 1]) / std::abs (x[i - 1]));
                worst = std::max (worst, std::abs (a - b));
            }
    }
    return worst;
}
} // namespace

TEST_CASE ("Limiter through the processor: a ceiling sweep moves the gain smoothly, both directions", "[wave4][output]")
{
    // 0 -> -12 dB in 0.5 dB steps every 10 ms (an automation lane at block rate), then back up.
    std::vector<Rig::Change> ch;
    for (int k = 0; k <= 48; ++k)
    {
        const float c = -0.5f * static_cast<float> (k <= 24 ? k : 48 - k);
        ch.push_back ({ 24000 + 480 * k, [c] (Rig& r) { r.set ("master.limiter.ceiling", c); } });
    }
    const auto still = loudRender ({});
    const auto moved = loudRender (ch);
    CHECK (allFiniteSamples (moved));
    // moved / still: the gain change the ceiling causes, without the detectors' own moves.
    const double down = maxGainStepDb (still, moved, 24000, 24000 + 480 * 24);
    const double up = maxGainStepDb (still, moved, 24000 + 480 * 24, 48000);
    std::printf ("ceiling sweep in 0.5 dB steps, largest per-sample gain change caused by the ceiling: down %.5f dB, "
                 "up %.5f dB (unsmoothed it would be up to 0.5 dB)\n", down, up);
    CHECK (down < 0.01);
    CHECK (up < 0.01);
    CHECK (down > 0.0); // it did move
}

TEST_CASE ("Limiter through the processor: switching it while it reduces the gain is crossfaded, both directions", "[wave4][output]")
{
    // Off at 30000 while it reduces the gain by about 10 dB, on again at 50000.
    const std::vector<Rig::Change> ch { { 30000, [] (Rig& r) { r.set ("master.limiter", 0); } },
                                        { 50000, [] (Rig& r) { r.set ("master.limiter", 1); } } };
    const auto in = loudRender ({}, false);
    const auto still = loudRender ({});
    const auto out = loudRender (ch);
    const double reduction = -toDb (rms (still.l, 25000, 30000) / rms (in.l, 25000, 30000));
    // The applied gain (out / in) across each switch. Switching on restarts the detectors, so
    // after the fade the gain moves the way it does at any loud onset (instant attack, the slow
    // detector charging): measured separately, against the largest move of the always-on render.
    const double off = maxGainStepDb (in, out, 29000, 32000);
    const double onFade = maxGainStepDb (in, out, 49000, 50000 + 241);
    const double onAfter = maxGainStepDb (in, out, 50000 + 241, 54000);
    const double steady = maxGainStepDb (in, still, 20000, 60000);
    const double onset = maxGainStepDb (in, still, 0, 4000); // the chord's own onset, limiter on all along
    std::printf ("limiter switch while reducing %.1f dB: largest per-sample gain change switching off %.4f dB, switching on "
                 "%.4f dB during the fade and %.4f dB in the 80 ms after it (always on: sustained %.4f dB, at the chord's "
                 "onset %.4f dB; the fade alone = %.4f dB per sample; unfaded the step would be the whole reduction)\n",
                 reduction, off, onFade, onAfter, steady, onset, reduction / 240.0);
    REQUIRE (reduction > 6.0);
    CHECK (off < 0.1);                 // the fade alone: reduction / 240 samples
    CHECK (onFade <= onset + 0.05);    // no worse than a loud onset with the limiter on all along
    CHECK (onAfter < 2.0 * steady);
    CHECK (onFade < 1.0);              // far from the unfaded step
    // Fully off after the fade: exactly the limiter-off path.
    const auto from = static_cast<std::ptrdiff_t> (30000 + 240 + 1);
    CHECK (sameBits (std::vector<float> (out.l.begin() + from, out.l.begin() + 50000), std::vector<float> (in.l.begin() + from, in.l.begin() + 50000)));
    CHECK (sameBits (std::vector<float> (out.r.begin() + from, out.r.begin() + 50000), std::vector<float> (in.r.begin() + from, in.r.begin() + 50000)));
    // On again: limited at the ceiling once the fade is over.
    CHECK (peakDbOf (out, 50000 + 241, out.l.size()) <= vs::kDefaultLimiterCeilingDb + kCeilingToleranceDb);
}

TEST_CASE ("Soft limiter: switching below the knee leaves the samples untouched; fully off costs nothing", "[wave4][limiter]")
{
    const double fs = 48000.0;
    const auto in = chordSignal (48000, fs, dbToGain (-1.0 - SoftLimiter::kKneeDb) * 0.999);
    SoftLimiter lim;
    lim.prepare (fs);
    lim.reset (-1.0f, true);
    Stereo s = in;
    for (int pos = 0, k = 0; pos < 48000; pos += 1000, ++k)
    {
        lim.setEnabled (k % 3 != 1); // toggles every few blocks, fades included
        lim.process (s.l.data() + pos, s.r.data() + pos, 1000);
    }
    CHECK (bitIdentical (s, in));
    // Off and faded out: process() returns at once, also for a signal far over the ceiling.
    lim.reset (-1.0f, false);
    auto loud = chordSignal (4800, fs, 8.0);
    const auto copy = loud;
    lim.process (loud.l.data(), loud.r.data(), 4800);
    CHECK (bitIdentical (loud, copy));
    CHECK (lim.isOff());
}

TEST_CASE ("Limiter: non-finite parameter values give the defaults (on, -1 dB)", "[wave4][output]")
{
    const float bad = GENERATE (std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity());
    Rig rig (48000.0, 512);
    rig.set ("key.mode", 1);
    rig.set ("master.volume", 6.0f);
    rig.set ("master.limiter.ceiling", -9.0f);
    rig.prepare();
    auto* raw = rig.proc.getState().getRawParameterValue ("master.limiter.ceiling");
    auto* sw = rig.proc.getState().getRawParameterValue ("master.limiter");
    const std::vector<Rig::Change> ch { { 12000, [raw, sw, bad] (Rig&) { raw->store (bad); sw->store (bad); } } };
    const auto out = rig.render (36000, fullChord (100), 256, ch);
    CHECK (allFiniteSamples (out));
    CHECK (rig.proc.getLimiter().isEnabled());
    CHECK (std::abs (rig.proc.getLimiter().currentCeilingDb() - vs::kDefaultLimiterCeilingDb) < 1.0e-6f);
    CHECK (peakDbOf (out, 24000, 36000) <= vs::kDefaultLimiterCeilingDb + kCeilingToleranceDb);
    CHECK (rig.proc.getGuardTrips() == 0);
}

TEST_CASE ("State: a parameter missing from a loaded session takes its default, not the instance's value", "[wave4][state]")
{
    // A session saved before Wave 4 has no limiter entries. Measured: JUCE 9.0.5 already resets
    // a missing parameter to its default (this test passes with the processor's own completion
    // step disabled); the processor makes it explicit, and this test guards both.
    const juce::File dir (VS_FIXTURE_DIR);
    juce::MemoryBlock old;
    REQUIRE (dir.getChildFile ("state_m5.bin").loadFileAsData (old));
    Rig rig;
    rig.set ("master.limiter", 0);
    rig.set ("master.limiter.ceiling", -12.0f);
    rig.proc.setStateInformation (old.getData(), static_cast<int> (old.getSize()));
    CHECK (rig.get ("master.limiter") > 0.5f);
    CHECK (std::abs (rig.get ("master.limiter.ceiling") - vs::kDefaultLimiterCeilingDb) < 1.0e-6f);
    CHECK (std::abs (rig.get ("master.volume") + 52.7f) < 1.0e-3f); // what the session holds is restored

    // Any parameter, not only the new ones: drop two entries from a saved state.
    juce::MemoryBlock saved;
    rig.set ("lower.vcf.cutoff", 0.123f);
    rig.set ("arp.tempo", 77.0f);
    rig.proc.getStateInformation (saved);
    auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), static_cast<int> (saved.getSize()));
    REQUIRE (xml != nullptr);
    for (const char* id : { "lower.vcf.cutoff", "arp.tempo" })
    {
        auto* e = xml->getChildByAttribute ("id", id);
        REQUIRE (e != nullptr);
        xml->removeChildElement (e, true);
    }
    juce::MemoryBlock trimmed;
    juce::AudioProcessor::copyXmlToBinary (*xml, trimmed);
    Rig other;
    other.set ("lower.vcf.cutoff", 0.9f);
    other.set ("arp.tempo", 200.0f);
    other.set ("upper.vcf.cutoff", 0.9f);
    other.proc.setStateInformation (trimmed.getData(), static_cast<int> (trimmed.getSize()));
    auto def = [&] (const char* id) {
        auto* p = other.proc.getState().getParameter (id);
        return p->convertFrom0to1 (p->getDefaultValue());
    };
    CHECK (std::abs (other.get ("lower.vcf.cutoff") - def ("lower.vcf.cutoff")) < 1.0e-6f);
    CHECK (std::abs (other.get ("arp.tempo") - def ("arp.tempo")) < 1.0e-4f);
    CHECK (std::abs (other.get ("upper.vcf.cutoff") - rig.get ("upper.vcf.cutoff")) < 1.0e-6f); // present: from the session
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
