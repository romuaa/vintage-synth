// S3a (docs/architecture.md 8.19.1): the chip's Filter and OutputStage on their own. The reference comparisons
// (corners, peaks, distortion, voice DC, modes) are in tests/bside/trace/; here: stability at every setting and
// internal rate of 4.4, writes every sample, decay after a loud input (no limit cycle, no denormal), block-size
// independence, no allocation, and the behaviours the contract names (signs, notch, resonance, the cutoff
// converter's steps, variation, volume DC). The closing step (8.19.3): the response's independence of the internal
// rate against the analog prototype ([rate]), processWithCutoff against setCutoff, every pole inside the unit circle.

#include "FuzzSupport.h"

#include "bside/chip/Filter.h"
#include "bside/chip/OutputStage.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace vs::bside::chip;

namespace
{
constexpr double kRates[] { 88200.0, 96000.0, 128000.0, 176400.0, 192000.0, 352800.0, 384000.0 };
constexpr double kPi = 3.14159265358979323846;

/** Uniform in [lo, hi). */
float uniform (bst::Rng& r, float lo, float hi)
{
    return lo + (hi - lo) * static_cast<float> (r.below (1u << 24)) / static_cast<float> (1u << 24);
}

/** Full scale at the filter's input (render units): three voices of +-0.5 plus their DC (OutputStage::filterBusLevel,
    Model A about 0.71 per voice at full envelope) and a margin. */
constexpr float kInLo = -2.0f, kInHi = 4.0f;

bool finiteAndBounded (float v, float bound) { return std::isfinite (v) && std::abs (v) < bound; }

/** Gain of the filter at `hz` (steady-state sine, after a settling time), relative to the input amplitude. */
double sineGain (Filter& f, double rate, double hz, double dc = 0.5)
{
    f.reset();
    const int settle = static_cast<int> (rate * 0.2), n = static_cast<int> (rate * 0.1);
    double s = 0.0, c = 0.0, mean = 0.0;
    std::vector<double> y (static_cast<std::size_t> (n));
    for (int i = 0; i < settle + n; ++i)
    {
        const double x = dc + 0.01 * std::sin (2.0 * kPi * hz * i / rate);
        const double o = f.process (static_cast<float> (x));
        if (i >= settle)
        {
            y[static_cast<std::size_t> (i - settle)] = o;
            mean += o;
        }
    }
    mean /= n;
    for (int i = 0; i < n; ++i)
    {
        const double ph = 2.0 * kPi * hz * (i + settle) / rate;
        s += (y[static_cast<std::size_t> (i)] - mean) * std::sin (ph);
        c += (y[static_cast<std::size_t> (i)] - mean) * std::cos (ph);
    }
    return 2.0 * std::sqrt (s * s + c * c) / n / 0.01;
}

/** The analog prototype of a filter's present setting (Filter.h "WHAT IT IS"): corner, Q and weights as the class
    reports them; the corner as designed (below 0.45 x the rate). */
std::complex<double> prototype (const Filter& f, double rate, double hz)
{
    const double fc = std::min (f.cutoffHz(), 0.45 * rate);
    const auto w = f.prototypeWeights();
    const std::complex<double> s (0.0, hz / fc);
    return (w[2] * s * s + w[1] * s + w[0]) / (s * s + s / f.q() + 1.0);
}

/** The gate band of 8.19.3: 20 Hz .. 20 kHz, 161 points spaced evenly in log frequency. */
const std::vector<double>& gateGrid()
{
    static const std::vector<double> g = [] {
        std::vector<double> v;
        for (int i = 0; i <= 160; ++i)
            v.push_back (20.0 * std::pow (1000.0, i / 160.0));
        return v;
    }();
    return g;
}

struct Worst
{
    double db = 0.0;
    double hz = 0.0;
};

/** One setting of the rate gate: the prototype on the gate grid, which points are gated (the prototype above -20 dB
    of its largest value: DC, infinity, and around the corner at 1/8 octave, +-4 octaves), and the deviation of a
    linear part (Filter::responseAt) from the prototype or from another linear part there. */
struct GateSetting
{
    GateSetting (const Filter& f, double rate, const std::vector<std::complex<double>>& z) : unit (z)
    {
        const double floor = 0.1 * largest (f, rate);
        const auto& grid = gateGrid();
        for (std::size_t i = 0; i < grid.size(); ++i)
        {
            const auto p = prototype (f, rate, grid[i]);
            if (std::abs (p) > floor)
            {
                index.push_back (i);
                proto.push_back (p);
            }
        }
    }
    static double largest (const Filter& f, double rate)
    {
        const auto w = f.prototypeWeights();
        double m = std::max (std::abs (w[0]), std::abs (w[2]));
        const double fc = std::min (f.cutoffHz(), 0.45 * rate);
        for (int i = -32; i <= 32; ++i)
            m = std::max (m, std::abs (prototype (f, rate, fc * std::exp2 (i / 8.0))));
        return m;
    }
    /** |20 log10 (|H| / |reference|)|, worst over the gated points; reference: the prototype, or `other`. */
    Worst deviation (const Filter::Linear& l, const Filter::Linear* other = nullptr) const
    {
        Worst w;
        for (std::size_t j = 0; j < index.size(); ++j)
        {
            const auto z = unit[index[j]];
            const double ref = other != nullptr ? std::abs (Filter::responseAt (*other, z)) : std::abs (proto[j]);
            const double d = std::abs (20.0 * std::log10 (std::abs (Filter::responseAt (l, z)) / ref));
            if (! (d <= w.db)) // NaN counts as the worst
            {
                w.db = std::isfinite (d) ? d : 1e9;
                w.hz = gateGrid()[index[j]];
            }
        }
        return w;
    }
    const std::vector<std::complex<double>>& unit;
    std::vector<std::size_t> index;
    std::vector<std::complex<double>> proto;
};

/** The response of a filter to an impulse of amplitude `amp` (process, or processWithCutoff at `cutoff` >= 0) at
    `hz`, from n samples (the tail beyond n is below the double rounding for the settings the tests use). */
std::complex<double> impulseResponse (Filter& f, double rate, double hz, int n, float amp, float cutoff)
{
    f.reset();
    std::complex<double> acc = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float x = i == 0 ? amp : 0.0f;
        const float y = cutoff >= 0.0f ? f.processWithCutoff (x, cutoff) : f.process (x);
        acc += static_cast<double> (y) * std::polar (1.0, -2.0 * kPi * hz * i / rate);
    }
    return acc / static_cast<double> (amp);
}

const char* modeName (int mode)
{
    static const char* const names[] { "none", "low", "band", "low+band", "high", "low+high", "band+high", "all" };
    return names[mode];
}
} // namespace

TEST_CASE ("filter: finite and bounded at every setting and internal rate, with full-scale noise", "[filter][stability]")
{
    // every resonance, every mode combination, cutoffs from 0 to 2047 (Model A: across its converter's steps),
    // variation 0 / 0.5 / 1, every internal rate of 4.4: 3000 samples of full-scale noise each. Bound: 60 render
    // units (the largest output seen is about 13: Model B, resonance 15, band-pass, at its corner).
    constexpr float kBound = 60.0f;
    float largest = 0.0f;
    bst::Rng r (31);
    for (const Model m : { Model::A, Model::B })
        for (const double rate : kRates)
            for (const float var : { 0.0f, 0.5f, 1.0f })
            {
                if (m == Model::B && var > 0.0f)
                    continue; // variation is Model A's
                Filter f;
                f.prepare (m, rate, var);
                for (int res = 0; res < 16; ++res)
                    for (const float fc : { 0.0f, 1.0f, 200.5f, 511.0f, 512.0f, 1023.0f, 1024.0f, 1600.0f, 2047.0f })
                        for (std::uint8_t mode = 0; mode < 8; ++mode)
                        {
                            f.reset();
                            f.setCutoff (fc);
                            f.setResonance (res);
                            f.setMode (static_cast<std::uint8_t> (mode << 4));
                            for (int i = 0; i < 3000; ++i)
                            {
                                const float y = f.process (uniform (r, kInLo, kInHi));
                                largest = std::max (largest, std::abs (y));
                                if (! finiteAndBounded (y, kBound))
                                {
                                    INFO ("model " << (m == Model::A ? "A" : "B") << " rate " << rate << " variation " << var << " res " << res << " cutoff "
                                                   << fc << " mode " << int (mode) << " sample " << i << ": " << y);
                                    FAIL();
                                }
                            }
                        }
            }
    WARN ("largest filter output over all settings with full-scale noise: " << largest << " (bound " << kBound << ")");
}

TEST_CASE ("filter and output stage: a cutoff, resonance, mode or routing write every sample stays finite and bounded", "[filter][stability]")
{
    bst::Rng r (32);
    for (const Model m : { Model::A, Model::B })
        for (const double rate : kRates)
        {
            Filter f;
            f.prepare (m, rate, 0.5f);
            OutputStage o;
            o.prepare (m, rate, r.chance (50));
            for (int i = 0; i < 40000; ++i)
            {
                f.setCutoff (uniform (r, -10.0f, 2100.0f)); // also out of range: clamped
                if (r.chance (30))
                    f.setResonance (r.range (-1, 16));
                const auto mv = static_cast<std::uint8_t> (r.below (256));
                if (r.chance (20))
                {
                    f.setMode (mv);
                    o.setModeVolume (mv);
                    o.setRouting (static_cast<std::uint8_t> (r.below (256)));
                    o.setFilterStaticDc (f.staticOutput (o.staticFilterBusDc()));
                }
                const float y = f.process (o.filterBusLevel (uniform (r, -1.5f, 1.5f), uniform (r, 0.0f, 3.0f)));
                const float out = o.process (y, uniform (r, -1.5f, 1.5f), uniform (r, 0.0f, 3.0f));
                INFO ("model " << (m == Model::A ? "A" : "B") << " rate " << rate << " sample " << i);
                REQUIRE (finiteAndBounded (y, 60.0f));
                REQUIRE (finiteAndBounded (out, 10.0f));
            }
        }
}

TEST_CASE ("filter: after a loud input, silence decays below -120 dB with no limit cycle and no denormal", "[filter][stability]")
{
    // 0.1 s of full-scale noise, then a constant input (0, or the voices' DC): the output must settle on the
    // constant's static output (Filter::staticOutput) to within 1e-6 (-120 dB re 1); no output sample on the way
    // is a subnormal number. No limit cycle: with input 0 the output ends exactly at 0 (the states are flushed
    // below 1e-15), with a constant input it ends on the static output within the float output's rounding and
    // moves by less than 1e-12 (-240 dB; what is left is the rounding of the double states at high resonance). Lowest and highest cutoffs (slowest decay: Model B at 15 Hz, resonance
    // 15) at every rate; 3 s is enough for every setting (measured below).
    bst::Rng r (33);
    double slowest = 0.0;
    for (const Model m : { Model::A, Model::B })
        for (const double rate : kRates)
            for (const int res : { 0, 15 })
                for (const float fc : { 0.0f, 2047.0f })
                    for (std::uint8_t mode = 1; mode < 8; ++mode)
                        for (const float dc : { 0.0f, 0.7f })
                        {
                            Filter f;
                            f.prepare (m, rate, 0.5f);
                            f.setCutoff (fc);
                            f.setResonance (res);
                            f.setMode (static_cast<std::uint8_t> (mode << 4));
                            for (int i = 0; i < static_cast<int> (rate * 0.1); ++i)
                                f.process (uniform (r, kInLo, kInHi));
                            const double target = f.staticOutput (dc);
                            const int n = static_cast<int> (rate * 3.0);
                            int settledAt = -1;
                            float last = 0.0f;
                            for (int i = 0; i < n; ++i)
                            {
                                const float y = f.process (dc);
                                REQUIRE (std::fpclassify (y) != FP_SUBNORMAL);
                                const bool within = std::abs (static_cast<double> (y) - target) < 1e-6;
                                if (within && settledAt < 0)
                                    settledAt = i;
                                if (! within)
                                    settledAt = -1;
                                last = y;
                            }
                            INFO ("model " << (m == Model::A ? "A" : "B") << " rate " << rate << " res " << res << " cutoff " << fc << " mode " << int (mode)
                                           << " dc " << dc << ": last " << last << ", target " << target);
                            REQUIRE (settledAt >= 0);
                            slowest = std::max (slowest, settledAt / rate);
                            // no limit cycle
                            double tail = 0.0;
                            float lo = 1e30f, hi = -1e30f;
                            for (int i = 0; i < 1000; ++i)
                            {
                                const float y = f.process (dc);
                                lo = std::min (lo, y);
                                hi = std::max (hi, y);
                                tail = std::max (tail, std::abs (static_cast<double> (y) - target));
                            }
                            if (dc > 0.0f)
                            {
                                CHECK (tail < 2e-7);     // the float output's own rounding of the target
                                CHECK (hi - lo < 1e-12f); // measured at most 1e-15: rounding of the double states (-300 dB), no cycle
                            }
                            else
                                CHECK (! (tail > 0.0)); // exactly 0: the states are flushed
                        }
    WARN ("slowest decay to within 1e-6 of the static output: " << slowest << " s (allowed 3 s)");
}

TEST_CASE ("filter: every resonance and cutoff law is pinned (a wrong constant fails)", "[filter]")
{
    // Model B's floor at register 0
    CHECK (std::abs (Filter::cutoffCurveHz (Model::B, 0.0, 0.5) - 15.0) < 1e-9);
    // Model A: the corner falls with resonance (0.871 x at 15) and the low-pass gain rises (1.196 x at 15)
    Filter a, b;
    a.prepare (Model::A, 192000.0, 0.5f);
    b.prepare (Model::A, 192000.0, 0.5f);
    for (Filter* f : { &a, &b })
    {
        f->setCutoff (1024.0f);
        f->setMode (Filter::kLowPass);
    }
    b.setResonance (15);
    CHECK (std::abs (b.cutoffHz() / a.cutoffHz() - 0.8710) < 1e-3);
    const double g0 = sineGain (a, 192000.0, 200.0, 0.0), g15 = sineGain (b, 192000.0, 200.0, 0.0);
    WARN ("Model A low-pass gain at 200 Hz, resonance 15 against 0: " << g15 / g0);
    CHECK (std::abs (g15 / g0 - 1.1958) < 0.02);
    // a cutoff-only write updates the band-pass gain (it follows Q at resonance 0): the same as a fresh filter
    Filter c, d;
    c.prepare (Model::A, 96000.0, 0.5f);
    d.prepare (Model::A, 96000.0, 0.5f);
    c.setMode (Filter::kBandPass);
    c.setCutoff (1500.0f);
    c.setCutoff (400.0f);
    d.setCutoff (400.0f);
    d.setMode (Filter::kBandPass); // d's outputs are computed at 400; c's must have followed its cutoff writes
    float diff = 0.0f;
    for (int i = 0; i < 2000; ++i)
    {
        const auto x = static_cast<float> (std::sin (i * 0.05));
        diff = std::max (diff, std::abs (c.process (x) - d.process (x)));
    }
    CHECK (! (diff > 0.0f));
}

TEST_CASE ("filter: the rational tan, and a cutoff every sample (processWithCutoff)", "[filter]")
{
    // fastTan: relative error below 1e-8 up to 0.75 (cutoffs to 0.24 x the rate), below 3e-5 up to 1.42 (0.45 x)
    double worstLow = 0.0, worstHigh = 0.0;
    for (int i = 1; i <= 14200; ++i)
    {
        const double x = i * 1e-4;
        const double e = std::abs (Filter::fastTan (x) / std::tan (x) - 1.0);
        (x <= 0.75 ? worstLow : worstHigh) = std::max (x <= 0.75 ? worstLow : worstHigh, e);
    }
    CHECK (worstLow < 1e-8);
    CHECK (worstHigh < 3e-5);
    // In time: a cutoff swept every sample through processWithCutoff against setCutoff + process every sample, noise at
    // full scale (Model A's static functions at work), on a filter whose stored cutoff stays elsewhere (8.19.3: the
    // damping, band weights and variation of the cutoff given). The difference's rms against the output's: below
    // 10^(0.1/20) - 1 (the 0.1 dB of 8.19.3). Model A at variation 0, 0.5 and 1 and at resonance 0 and 12.
    bst::Rng r (37);
    double worstRel = 0.0;
    for (const Model m : { Model::B, Model::A })
        for (const float var : { 0.0f, 0.5f, 1.0f })
            for (const int res : { 0, 12 })
                for (const std::uint8_t mode : { std::uint8_t { 0x10 }, std::uint8_t { 0x20 }, std::uint8_t { 0x70 } })
                {
                    if (m == Model::B && var > 0.0f)
                        continue;
                    Filter a, b;
                    a.prepare (m, 96000.0, var);
                    b.prepare (m, 96000.0, var);
                    b.setCutoff (1800.0f); // the stored cutoff, far from the swept one
                    for (Filter* f : { &a, &b })
                    {
                        f->setMode (mode);
                        f->setResonance (res);
                    }
                    double num = 0.0, den = 0.0;
                    for (int i = 0; i < 40000; ++i)
                    {
                        const float fc = 700.0f + 600.0f * static_cast<float> (std::sin (i * 0.0007));
                        const float x = m == Model::A ? uniform (r, 0.0f, 2.1f) : uniform (r, -1.0f, 1.0f);
                        a.setCutoff (fc);
                        const double ya = a.process (x);
                        const double yb = b.processWithCutoff (x, fc);
                        if (i >= 2000)
                        {
                            num += (ya - yb) * (ya - yb);
                            den += ya * ya;
                        }
                    }
                    const double rel = std::sqrt (num / den);
                    INFO ("model " << (m == Model::A ? "A" : "B") << " variation " << var << " resonance " << res << " mode " << int (mode));
                    CHECK (rel < 0.0116);
                    worstRel = std::max (worstRel, rel);
                }
    WARN ("processWithCutoff against setCutoff + process, swept cutoff and noise: largest relative rms difference " << worstRel
                                                                                                                     << " (allowed 0.0116)");
}

namespace
{
// For every internal rate of 4.4, mode combination 1..7, resonance 0..15, cutoff 0, 4, ..., 2044, 2047 (Model A at
// variation 0, 0.5 and 1): the magnitude of the filter's linear part (Filter::linearPart, the per-sample code's own
// coefficients; that this is the class's response is the next test's) against its analog prototype (corner, Q and
// weights as the class reports them), 20 Hz .. 20 kHz where the prototype is above -20 dB of its largest value:
// at most 0.5 dB. processWithCutoff's linear part (Filter::linearPartWithCutoff) at the same cutoff, on a filter
// whose stored cutoff is elsewhere, against setCutoff's: at most 0.1 dB. Prints the worst per model, rate and mode.
void rateGate (Model m)
{
    constexpr double kAllowed = 0.5, kAllowedCut = 0.1;
    double worstAll = 0.0, worstCutAll = 0.0;
    for (const double rate : kRates)
    {
        std::vector<std::complex<double>> unit;
        for (const double hz : gateGrid())
            unit.push_back (std::polar (1.0, 2.0 * kPi * hz / rate));
        for (int mode = 1; mode < 8; ++mode)
        {
            Worst worst, worstCut;
            std::string at, atCut;
            for (const float var : { 0.0f, 0.5f, 1.0f })
            {
                if (m == Model::B && var > 0.0f)
                    continue;
                Filter f, other;
                f.prepare (m, rate, var);
                other.prepare (m, rate, var);
                f.setMode (static_cast<std::uint8_t> (mode << 4));
                other.setMode (static_cast<std::uint8_t> (mode << 4));
                other.setCutoff (1500.0f);
                for (int res = 0; res < 16; ++res)
                {
                    f.setResonance (res);
                    other.setResonance (res);
                    for (int v = 0; v <= 2048; v += 4)
                    {
                        const auto fc = static_cast<float> (std::min (v, 2047));
                        f.setCutoff (fc);
                        const auto lin = f.linearPart();
                        const auto cut = other.linearPartWithCutoff (fc);
                        const GateSetting gs (f, rate, unit);
                        const auto d = gs.deviation (lin);
                        const auto c = gs.deviation (cut, &lin);
                        char buf[96];
                        if (d.db > worst.db)
                        {
                            worst = d;
                            std::snprintf (buf, sizeof buf, "FC %g res %d var %g at %.0f Hz", static_cast<double> (fc), res, static_cast<double> (var), d.hz);
                            at = buf;
                        }
                        if (c.db > worstCut.db)
                        {
                            worstCut = c;
                            std::snprintf (buf, sizeof buf, "FC %g res %d var %g at %.0f Hz", static_cast<double> (fc), res, static_cast<double> (var), c.hz);
                            atCut = buf;
                        }
                    }
                }
            }
            INFO ("model " << (m == Model::A ? "A" : "B") << " rate " << rate << " mode " << modeName (mode) << ": " << worst.db << " dB at " << at
                           << "; processWithCutoff " << worstCut.db << " dB at " << atCut);
            CHECK (worst.db <= kAllowed);
            CHECK (worstCut.db <= kAllowedCut);
            worstAll = std::max (worstAll, worst.db);
            worstCutAll = std::max (worstCutAll, worstCut.db);
            std::printf ("[rate] model %s %6.0f Hz %-9s worst %.3f dB (%s); processWithCutoff %.3f dB (%s)\n", m == Model::A ? "A" : "B", rate,
                         modeName (mode), worst.db, at.c_str(), worstCut.db, atCut.c_str());
        }
    }
    std::printf ("[rate] model %s, worst over every setting: %.3f dB (allowed %.1f); processWithCutoff against setCutoff %.3f dB (allowed %.1f)\n",
                 m == Model::A ? "A" : "B", worstAll, kAllowed, worstCutAll, kAllowedCut);
}
} // namespace

TEST_CASE ("filter: Model B's response does not depend on the internal rate: within 0.5 dB of the analog prototype (8.19.3)", "[filter][rate]")
{
    rateGate (Model::B);
}

TEST_CASE ("filter: Model A's linear response does not depend on the internal rate: within 0.5 dB of the analog prototype (8.19.3)", "[filter][rate]")
{
    rateGate (Model::A);
}

TEST_CASE ("filter: the linear part the rate gate measures is the class's own response (impulse at sample points)", "[filter][rate]")
{
    // Filter::response (linearPart ()) and (linearPartWithCutoff (FC)) against the DFT of the class's impulse response
    // through process and processWithCutoff (amplitude 1e-6 on Model A, where its static functions are linear to
    // 1e-7), at 6 frequencies, random settings over every rate, mode, resonance and the register range (corners from
    // 15 Hz: 1 s of response, -150 dB of decay at the slowest). Relative to the largest magnitude: below 1e-4.
    bst::Rng r (41);
    double worst = 0.0;
    for (int t = 0; t < 48; ++t)
    {
        const Model m = t % 2 == 0 ? Model::A : Model::B;
        const double rate = kRates[r.below (7)];
        const float var = static_cast<float> (r.below (3)) * 0.5f;
        const auto mode = static_cast<std::uint8_t> ((1 + r.below (7)) << 4);
        const int res = r.range (0, 15);
        const float fc = t < 4 ? static_cast<float> (t) : uniform (r, 0.0f, 2047.0f);
        const float stored = uniform (r, 0.0f, 2047.0f);
        Filter f;
        f.prepare (m, rate, var);
        f.setMode (mode);
        f.setResonance (res);
        f.setCutoff (fc);
        const auto lin = f.linearPart();
        Filter g;
        g.prepare (m, rate, var);
        g.setMode (mode);
        g.setResonance (res);
        g.setCutoff (stored);
        const auto cut = g.linearPartWithCutoff (fc);
        const float amp = m == Model::A ? 1e-6f : 1.0f;
        const int n = static_cast<int> (rate);
        double largest = 0.0, err = 0.0, errCut = 0.0;
        for (const double hz : { 20.0, 200.0, 1000.0, 4000.0, 12000.0, 20000.0 })
            largest = std::max ({ largest, std::abs (Filter::response (lin, hz, rate)), std::abs (Filter::response (cut, hz, rate)) });
        for (const double hz : { 20.0, 200.0, 1000.0, 4000.0, 12000.0, 20000.0 })
        {
            err = std::max (err, std::abs (impulseResponse (f, rate, hz, n, amp, -1.0f) - Filter::response (lin, hz, rate)));
            errCut = std::max (errCut, std::abs (impulseResponse (g, rate, hz, n, amp, fc) - Filter::response (cut, hz, rate)));
        }
        INFO ("model " << (m == Model::A ? "A" : "B") << " rate " << rate << " variation " << var << " mode " << int (mode) << " res " << res << " FC " << fc);
        CHECK (err < 1e-4 * largest);
        CHECK (errCut < 1e-4 * largest);
        worst = std::max ({ worst, err / largest, errCut / largest });
    }
    WARN ("largest relative difference between the closed-form response and the impulse response: " << worst << " (allowed 1e-4)");
}

TEST_CASE ("filter: processWithCutoff follows a mode, resonance, rate or variation change after its table was built", "[filter]")
{
    // Its table is built at the first processWithCutoff after such a change (Filter.h): a filter that has used it and is
    // then changed must give exactly what a fresh filter with the new setting gives.
    for (const Model m : { Model::A, Model::B })
    {
        auto same = [] (const Filter::Linear& x, const Filter::Linear& y) {
            return ! (x.a11 < y.a11 || x.a11 > y.a11 || x.a12 < y.a12 || x.a12 > y.a12 || x.ox < y.ox || x.ox > y.ox || x.o1 < y.o1 || x.o1 > y.o1
                      || x.o2 < y.o2 || x.o2 > y.o2);
        };
        auto fresh = [m] (double rate, float var, std::uint8_t mode, int res) {
            Filter f;
            f.prepare (m, rate, var);
            f.setMode (mode);
            f.setResonance (res);
            return f.linearPartWithCutoff (900.0f);
        };
        Filter f;
        f.prepare (m, 96000.0, 0.5f);
        f.setMode (0x10);
        (void) f.linearPartWithCutoff (900.0f); // builds the table
        f.setMode (0x60);
        INFO ("model " << (m == Model::A ? "A" : "B"));
        CHECK (same (f.linearPartWithCutoff (900.0f), fresh (96000.0, 0.5f, 0x60, 0)));
        f.setResonance (13);
        CHECK (same (f.linearPartWithCutoff (900.0f), fresh (96000.0, 0.5f, 0x60, 13)));
        f.prepare (m, 192000.0, 0.5f);
        CHECK (same (f.linearPartWithCutoff (900.0f), fresh (192000.0, 0.5f, 0x60, 13)));
        f.setVariation (0.0f);
        CHECK (same (f.linearPartWithCutoff (900.0f), fresh (192000.0, 0.0f, 0x60, 13)));
        f.setCutoff (300.0f); // the stored cutoff does not enter
        CHECK (same (f.linearPartWithCutoff (900.0f), fresh (192000.0, 0.0f, 0x60, 13)));
    }
}

TEST_CASE ("filter: every pole inside the unit circle at every register value, resonance, variation and rate", "[filter][stability]")
{
    // The spectral radius of the state matrix of setCutoff's mapping and of processWithCutoff's (at every register
    // value and half-way between), Model A at variation 0, 0.5 and 1: below 1. Printed: the largest (the slowest decay:
    // 15 Hz at 384 kHz).
    double largest = 0.0;
    for (const Model m : { Model::A, Model::B })
        for (const double rate : kRates)
            for (const float var : { 0.0f, 0.5f, 1.0f })
            {
                if (m == Model::B && var > 0.0f)
                    continue;
                Filter f;
                f.prepare (m, rate, var);
                f.setMode (0x70);
                for (int res = 0; res < 16; ++res)
                {
                    f.setResonance (res);
                    for (int v = 0; v < 2048; ++v)
                    {
                        f.setCutoff (static_cast<float> (v));
                        const double a = Filter::spectralRadius (f.linearPart());
                        const double b = Filter::spectralRadius (f.linearPartWithCutoff (static_cast<float> (v)));
                        const double c = Filter::spectralRadius (f.linearPartWithCutoff (static_cast<float> (v) + 0.5f));
                        const double worst = std::max ({ a, b, c });
                        if (! (worst < 1.0))
                        {
                            INFO ("model " << (m == Model::A ? "A" : "B") << " rate " << rate << " variation " << var << " res " << res << " FC " << v
                                           << ": spectral radius " << a << " / " << b << " / " << c);
                            FAIL();
                        }
                        largest = std::max (largest, worst);
                    }
                }
            }
    WARN ("largest spectral radius: " << largest);
}

TEST_CASE ("output stage: volume DC off, a volume or routing write with held voices does not move the output's DC", "[output]")
{
    // 8.4.4 (orchestrator's reading): with volumeDc off a volume write with held voices changes the DC at the stage's
    // output by less than -60 dBFS, on both paths, and so does a routing write with silent voices. A held triangle
    // (110 Hz, full envelope, through the voice bend) sounds on the direct path or through the low-pass; the DC is the
    // mean over ten periods, 3 s after the voice starts (the board high-pass has settled) and right after the write. With volumeDc on the same write does step the DC (the reference's behaviour; the gates compare it).
    for (const Model m : { Model::A, Model::B })
        for (const bool routed : { false, true })
            for (const bool vdc : { false, true })
            {
                const double rate = 96000.0;
                Filter f;
                f.prepare (m, rate, 0.5f);
                f.setCutoff (1500.0f);
                OutputStage o;
                o.prepare (m, rate, vdc);
                auto setRegs = [&] (std::uint8_t routing, std::uint8_t mv) {
                    f.setMode (mv);
                    o.setRouting (routing);
                    o.setModeVolume (mv);
                    o.setFilterStaticDc (f.staticOutput (o.staticFilterBusDc()));
                };
                setRegs (routed ? 0x01 : 0x00, 0x1f);
                double phase = 0.0;
                auto run = [&] (double seconds, bool measure) {
                    double sum = 0.0;
                    const int n = static_cast<int> (seconds * rate);
                    for (int i = 0; i < n; ++i)
                    {
                        phase += 110.0 / rate;
                        phase -= std::floor (phase);
                        const float u = static_cast<float> (2.0 * std::abs (phase - 0.5) - 0.5); // triangle +-0.5
                        const float v = OutputStage::shapeVoice (m, u, 1.0f);
                        const float y = o.process (f.process (o.filterBusLevel (routed ? v : 0.0f, routed ? 1.0f : 0.0f)), routed ? 0.0f : v,
                                                   routed ? 0.0f : 1.0f);
                        sum += y;
                    }
                    return measure ? sum / n : 0.0;
                };
                // windows of exactly ten periods (90.9 ms): the triangle's own mean is 0 there, so what is left is DC
                run (3.0, false);
                const double before = run (10.0 / 110.0, true);
                setRegs (routed ? 0x01 : 0x00, 0x10); // volume 15 -> 0
                const double after = run (10.0 / 110.0, true); // right after the write: a DC step would show here
                const double stepDb = 20.0 * std::log10 (std::abs (after - before) + 1e-12);
                INFO ("model " << (m == Model::A ? "A" : "B") << (routed ? " low-pass" : " direct") << " volume DC " << vdc << ": DC step "
                               << stepDb << " dBFS");
                if (! vdc)
                    CHECK (stepDb < -60.0);
            }
    // routing writes with silent voices, volume DC off
    for (const Model m : { Model::A, Model::B })
    {
        Filter f;
        f.prepare (m, 96000.0, 0.5f);
        f.setCutoff (1500.0f);
        OutputStage o;
        o.prepare (m, 96000.0, false);
        double worst = -300.0;
        double previous = 0.0;
        bool first = true;
        for (const int routingInt : { 0x00, 0x01, 0x03, 0x07, 0x00, 0x04 })
        {
            const auto routing = static_cast<std::uint8_t> (routingInt);
            f.setMode (0x1f);
            o.setRouting (routing);
            o.setModeVolume (0x1f);
            o.setFilterStaticDc (f.staticOutput (o.staticFilterBusDc()));
            double sum = 0.0;
            for (int i = 0; i < 96000; ++i)
            {
                const float y = o.process (f.process (o.filterBusLevel (0.0f, 0.0f)), 0.0f, 0.0f);
                if (i >= 48000)
                    sum += y;
            }
            const double dc = sum / 48000.0;
            if (! first)
                worst = std::max (worst, 20.0 * std::log10 (std::abs (dc - previous) + 1e-12));
            first = false;
            previous = dc;
        }
        INFO ("model " << (m == Model::A ? "A" : "B") << ": largest DC change after a routing write " << worst << " dBFS");
        CHECK (worst < -60.0);
    }
}

TEST_CASE ("filter and output stage: the Shared topology stays meaningful for 1 to 16 chips", "[filter][output]")
{
    // OutputStage.h "SHARED": one filter, its input the static DC of one chip plus every chip's filterBusAc; each
    // chip's stage gets the shared output / n. Identical notes on n chips: Model B (its filter linear, its mixer curve
    // nearly so) gives n times one chip within 1 %; Model A stays finite, does not fall silent and grows less than n times (its summed
    // distortion, 8.5).
    for (const Model m : { Model::A, Model::B })
    {
        double single = 0.0;
        for (const int n : { 1, 2, 4, 8, 16 })
        {
            const double rate = 96000.0;
            Filter f;
            f.prepare (m, rate, 0.5f);
            f.setCutoff (900.0f);
            f.setMode (0x1f);
            std::vector<OutputStage> stages (static_cast<std::size_t> (n));
            for (auto& o : stages)
            {
                o.prepare (m, rate, false);
                o.setRouting (0x01);
                o.setModeVolume (0x1f);
                o.setFilterStaticDc (f.staticOutput (o.staticFilterBusDc()) / n);
            }
            double phase = 0.0, energy = 0.0;
            bool finite = true;
            const int len = static_cast<int> (rate);
            for (int i = 0; i < len; ++i)
            {
                phase += 220.0 / rate;
                phase -= std::floor (phase);
                const float u = static_cast<float> (phase - 0.5);
                const float v = OutputStage::shapeVoice (m, u, 1.0f);
                float in = static_cast<float> (stages[0].staticFilterBusDc());
                for (auto& o : stages)
                    in += o.filterBusAc (v, 1.0f);
                const float shared = f.process (in) / static_cast<float> (n);
                double y = 0.0;
                for (auto& o : stages)
                    y += o.process (shared, 0.0f, 0.0f);
                finite = finite && std::isfinite (y);
                if (i > len / 2)
                    energy += y * y;
            }
            const double rms = std::sqrt (energy / (len / 2));
            if (n == 1)
                single = rms;
            INFO ("model " << (m == Model::A ? "A" : "B") << " chips " << n << ": rms " << rms << " (one chip " << single << ")");
            CHECK (finite);
            if (m == Model::B)
                CHECK (std::abs (rms / (single * n) - 1.0) < 0.01); // measured at most 0.5 %: the mixer curve is not quite linear
            else
            {
                CHECK (rms > 0.3 * single);
                CHECK (rms < 1.2 * single * n);
            }
        }
    }
}

TEST_CASE ("output stage: silence after a loud input decays below -120 dB through the board high-pass", "[output][stability]")
{
    bst::Rng r (34);
    for (const Model m : { Model::A, Model::B })
        for (const bool vdc : { false, true })
            for (const double rate : { 88200.0, 384000.0 })
            {
                OutputStage o;
                o.prepare (m, rate, vdc);
                o.setRouting (0);
                o.setModeVolume (0x0f);
                o.setFilterStaticDc (0.0);
                for (int i = 0; i < static_cast<int> (rate * 0.1); ++i)
                    o.process (0.0f, uniform (r, -1.5f, 1.5f), uniform (r, 0.0f, 3.0f));
                float last = 1.0f;
                for (int i = 0; i < static_cast<int> (rate * 3.0); ++i)
                {
                    last = o.process (0.0f, 0.0f, 0.0f);
                    REQUIRE (std::fpclassify (last) != FP_SUBNORMAL);
                }
                INFO ("model " << (m == Model::A ? "A" : "B") << " volume DC " << vdc << " rate " << rate);
                CHECK (std::abs (last) < 1e-6f);
            }
}

TEST_CASE ("filter and output stage: a NaN or infinite input does not poison the state", "[filter][output][stability]")
{
    for (const Model m : { Model::A, Model::B })
        for (const float bad : { std::nanf (""), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 1e30f })
        {
            Filter f;
            f.prepare (m, 96000.0, 0.5f);
            f.setCutoff (1000.0f);
            f.setMode (0x70);
            OutputStage o;
            o.prepare (m, 96000.0, true);
            o.setModeVolume (0x7f);
            for (int i = 0; i < 100; ++i)
                o.process (f.process (0.3f), 0.1f, 1.0f);
            (void) o.process (f.process (bad), bad, 1.0f);
            float y = 0.0f, z = 0.0f;
            for (int i = 0; i < 1000; ++i)
            {
                z = f.process (0.3f);
                y = o.process (z, 0.1f, 1.0f);
            }
            INFO ("model " << (m == Model::A ? "A" : "B") << " input " << bad);
            CHECK (std::isfinite (z));
            CHECK (std::isfinite (y));
        }
}

TEST_CASE ("filter and output stage: bit-identical for any block split", "[filter][output][split]")
{
    // The same input and the same parameter writes (at fixed samples) processed in blocks of 1, 7, 61, 512 and
    // random sizes, with Filter::process (block) and per sample.
    bst::Rng gen (35);
    const int n = 20000;
    std::vector<float> in (static_cast<std::size_t> (n)), d (static_cast<std::size_t> (n)), e (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        in[static_cast<std::size_t> (i)] = uniform (gen, -1.5f, 1.5f);
        d[static_cast<std::size_t> (i)] = uniform (gen, -1.5f, 1.5f);
        e[static_cast<std::size_t> (i)] = uniform (gen, 0.0f, 3.0f);
    }
    for (const Model m : { Model::A, Model::B })
    {
        auto run = [&] (int split) {
            bst::Rng sizes (99);
            Filter f;
            f.prepare (m, 96000.0, 0.3f);
            OutputStage o;
            o.prepare (m, 96000.0, true);
            std::vector<float> out (static_cast<std::size_t> (n));
            std::vector<float> buf;
            int i = 0;
            while (i < n)
            {
                int len = split > 0 ? split : 1 + static_cast<int> (sizes.below (300));
                // parameter writes land at multiples of 500 samples, also inside a block of another split
                const int nextWrite = (i / 500 + 1) * 500;
                len = std::min ({ len, n - i, nextWrite - i });
                if (i % 500 == 0)
                {
                    const int k = i / 500;
                    f.setCutoff (static_cast<float> ((k * 397) % 2048) + 0.25f);
                    f.setResonance (k % 16);
                    const auto mv = static_cast<std::uint8_t> (((k % 8) << 4) | (15 - k % 16) | (k % 3 == 0 ? 0x80 : 0));
                    f.setMode (mv);
                    o.setModeVolume (mv);
                    o.setRouting (static_cast<std::uint8_t> (k % 8));
                    o.setFilterStaticDc (f.staticOutput (o.staticFilterBusDc()));
                }
                buf.assign (in.begin() + i, in.begin() + i + len);
                for (int j = 0; j < len; ++j)
                    buf[static_cast<std::size_t> (j)] = o.filterBusLevel (buf[static_cast<std::size_t> (j)], e[static_cast<std::size_t> (i + j)]);
                if (split == 1)
                    for (auto& v : buf)
                        v = f.process (v);
                else
                    f.process (buf.data(), len);
                for (int j = 0; j < len; ++j)
                    out[static_cast<std::size_t> (i + j)] = o.process (buf[static_cast<std::size_t> (j)], d[static_cast<std::size_t> (i + j)], e[static_cast<std::size_t> (i + j)]);
                i += len;
            }
            return out;
        };
        const auto ref = run (1);
        for (const int split : { 7, 61, 512, 0 })
        {
            const auto o = run (split);
            int diff = 0;
            for (int i = 0; i < n; ++i)
                diff += (o[static_cast<std::size_t> (i)] < ref[static_cast<std::size_t> (i)] || o[static_cast<std::size_t> (i)] > ref[static_cast<std::size_t> (i)]) ? 1 : 0;
            INFO ("model " << (m == Model::A ? "A" : "B") << " split " << split);
            CHECK (diff == 0);
        }
    }
}

TEST_CASE ("filter and output stage: no allocation in prepare, the setters or processing", "[filter][output][rt]")
{
    if (! bst::allocationsAreCounted())
        SKIP ("allocation counting is off in this build (sanitizers)");
    Filter warm; // the converter weights' one-time initialisation
    const auto before = bst::totalAllocations();
    for (const Model m : { Model::A, Model::B })
    {
        Filter f;
        f.prepare (m, 96000.0, 0.5f);
        OutputStage o;
        o.prepare (m, 96000.0, false);
        float acc = 0.0f;
        for (int i = 0; i < 10000; ++i)
        {
            f.setCutoff (static_cast<float> (i % 2048));
            f.setResonance (i % 16);
            f.setMode (static_cast<std::uint8_t> (i));
            f.setVariation (static_cast<float> (i % 10) / 10.0f);
            o.setModeVolume (static_cast<std::uint8_t> (i));
            o.setRouting (static_cast<std::uint8_t> (i / 3));
            o.setVolumeDc ((i & 64) != 0);
            o.setFilterStaticDc (f.staticOutput (o.staticFilterBusDc()));
            acc += o.process (f.process (o.filterBusLevel (0.1f, 0.5f)), 0.2f, 1.0f);
        }
        CHECK (std::isfinite (acc));
    }
    CHECK (bst::totalAllocations() == before);
}

TEST_CASE ("filter: outputs, signs and the notch (8.19.1)", "[filter]")
{
    for (const Model m : { Model::A, Model::B })
    {
        Filter f;
        f.prepare (m, 192000.0, 0.5f);
        f.setCutoff (1024.0f);
        f.setResonance (0);
        INFO ("model " << (m == Model::A ? "A" : "B"));
        // low-pass inverted against the direct path (DC gain about -1), band- and high-pass block DC
        f.setMode (Filter::kLowPass);
        CHECK (f.staticOutput (0.5) < -0.3);
        if (m == Model::B)
        {
            CHECK (std::abs (f.staticOutput (0.5) + 0.5) < 0.01);
            f.setMode (Filter::kBandPass);
            CHECK (std::abs (f.staticOutput (0.5)) < 1e-12);
            f.setMode (Filter::kHighPass);
            CHECK (std::abs (f.staticOutput (0.5)) < 1e-12);
        }
        // low + high is a notch at the corner (band-pass passes there)
        const double fc = f.cutoffHz();
        f.setMode (Filter::kLowPass | Filter::kHighPass);
        const double notch = sineGain (f, 192000.0, fc);
        const double below = sineGain (f, 192000.0, fc / 8.0);
        f.setMode (Filter::kBandPass);
        const double band = sineGain (f, 192000.0, fc);
        f.setMode (0);
        const double none = sineGain (f, 192000.0, fc);
        WARN ("notch at the corner " << 20.0 * std::log10 (notch / below) << " dB below the low band; band-pass at the corner " << band);
        // Model B's outputs are ideal: a deep notch; Model A's are not (gains 1.0 / 0.59 and the leaks of Filter.h):
        // the reference's own Model A notch is about 7 dB deep (a_modes_a s03 at 3 kHz against s00), ours 9.8
        CHECK (notch < (m == Model::B ? 0.05 : 0.5) * below);
        CHECK (band > 0.3);
        CHECK (none < 1e-9);
    }
}

TEST_CASE ("filter: resonance raises the peak at every step, cutoff curves and Model A's converter steps", "[filter]")
{
    for (const Model m : { Model::A, Model::B })
    {
        Filter f;
        f.prepare (m, 192000.0, 0.5f);
        f.setCutoff (1024.0f);
        f.setMode (Filter::kLowPass);
        double previous = 0.0;
        for (int res = 0; res < 16; ++res)
        {
            f.setResonance (res);
            const double q = f.q();
            INFO ("model " << (m == Model::A ? "A" : "B") << " resonance " << res);
            CHECK (q > previous);
            previous = q;
        }
        CHECK (previous > 2.0); // Q at resonance 15: Model A 2.17, Model B 2.58 (fitted)
    }
    // Model B: close to linear in the register; Model A: steps down at the converter's bit boundaries
    CHECK (Filter::cutoffCurveHz (Model::B, 1024.0, 0.5) > 7000.0);
    CHECK (Filter::cutoffCurveHz (Model::B, 1024.0, 0.5) < 7400.0);
    CHECK (Filter::cutoffCurveHz (Model::B, 2047.0, 0.5) > 14000.0);
    const double a1023 = Filter::cutoffCurveHz (Model::A, 1023.0, 0.5), a1024 = Filter::cutoffCurveHz (Model::A, 1024.0, 0.5);
    WARN ("Model A cutoff at 1023: " << a1023 << " Hz, at 1024: " << a1024 << " Hz");
    CHECK (a1024 < 0.97 * a1023);
    CHECK (Filter::modelACutoffConverter (2047.0) > 0.999);
    CHECK (Filter::modelACutoffConverter (0.0) < 1e-12);
    // variation shifts the Model A curve (up with variation), not Model B's
    CHECK (Filter::cutoffCurveHz (Model::A, 512.0, 1.0) > 1.2 * Filter::cutoffCurveHz (Model::A, 512.0, 0.5));
    CHECK (Filter::cutoffCurveHz (Model::A, 512.0, 0.0) < 0.8 * Filter::cutoffCurveHz (Model::A, 512.0, 0.5));
    CHECK (! (Filter::cutoffCurveHz (Model::B, 512.0, 1.0) < Filter::cutoffCurveHz (Model::B, 512.0, 0.5)
              || Filter::cutoffCurveHz (Model::B, 512.0, 1.0) > Filter::cutoffCurveHz (Model::B, 512.0, 0.5)));
    // clock-independent: nothing in the filter takes a clock; the same register gives the same Hz at every rate
    Filter x, y;
    x.prepare (Model::A, 88200.0, 0.5f);
    y.prepare (Model::A, 384000.0, 0.5f);
    x.setCutoff (700.0f);
    y.setCutoff (700.0f);
    CHECK (std::abs (x.cutoffHz() - y.cutoffHz()) < 1e-9);
}

TEST_CASE ("output stage: volume DC on steps the output on a volume write, off it does not", "[output]")
{
    for (const Model m : { Model::A, Model::B })
    {
        const double expectStep = m == Model::A ? 0.35 : 0.06; // the reference's 0 -> 15 step (a_voice_dc_*)
        for (const bool vdc : { true, false })
        {
            OutputStage o;
            o.prepare (m, 96000.0, vdc);
            o.setRouting (0);
            o.setModeVolume (0x00);
            o.setFilterStaticDc (0.0);
            const double a = o.mixerOutput (0.0, 0.0, 0.0);
            o.setModeVolume (0x0f);
            const double b = o.mixerOutput (0.0, 0.0, 0.0);
            INFO ("model " << (m == Model::A ? "A" : "B") << " volume DC " << vdc << ": step " << b - a);
            if (vdc)
                CHECK (std::abs (std::abs (b - a) - expectStep) < 0.15 * expectStep);
            else
                CHECK (std::abs (b - a) < 1e-12);
        }
        // oscillator 3 off removes voice 3 from the direct path only
        OutputStage o;
        o.prepare (m, 96000.0, true);
        o.setRouting (0);
        o.setModeVolume (0x8f);
        CHECK (o.directVoices() == 2);
        CHECK (o.filterVoices() == 0);
        o.setRouting (4);
        CHECK (o.directVoices() == 2);
        CHECK (o.filterVoices() == 1);
        o.setModeVolume (0x0f);
        o.setRouting (3);
        CHECK (o.directVoices() == 1);
        CHECK (o.filterVoices() == 2);
    }
}
