// The shared analog estimator (source/bside/trace/AnalogMeasure.h) on synthetic responses with a known
// answer: an ideal two-pole low-pass at a known corner and Q, with an offset plateau and noise.

#include "bside/trace/AnalogMeasure.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace vs::bside::trace;

namespace
{
/** |H|^2 of a two-pole low-pass in dB at f. */
double twoPoleDb (double f, double fc, double q)
{
    const double x = f / fc;
    return -10.0 * std::log10 ((1.0 - x * x) * (1.0 - x * x) + (x / q) * (x / q));
}

/** A filtered/bypass pair of segments whose response is `offset + twoPole + noise`. */
void makePair (double fc, double q, double offsetDb, double noiseDb, SpecSegment& filtered, SpecSegment& bypass)
{
    std::uint32_t r = 7;
    for (int k = 0; k < kSpecBins; ++k)
    {
        r = r * 1664525u + 1013904223u;
        const double n = noiseDb * (static_cast<double> (r >> 8) / 8388608.0 - 1.0);
        const auto i = static_cast<std::size_t> (k);
        bypass.spectrum[i] = -400; // -40 dB
        const double db = std::max (-115.0, -40.0 + offsetDb + twoPoleDb (binHz (k), fc, q) + n);
        filtered.spectrum[i] = static_cast<std::int16_t> (std::lround (10.0 * db));
    }
}
} // namespace

TEST_CASE ("analog measure: the response is filtered minus bypass, NaN at the floor", "[measure]")
{
    SpecSegment f, b;
    f.spectrum.fill (-300);
    b.spectrum.fill (-200);
    b.spectrum[5] = -1000;
    const auto r = filterResponse (f, b);
    CHECK (std::abs (r[0] + 10.0) < 1e-9);
    CHECK (std::isnan (r[5]));
}

TEST_CASE ("analog measure: corner of an ideal low-pass within 1 %", "[measure]")
{
    for (double fc : { 120.0, 950.0, 4000.0, 11000.0 })
    {
        SpecSegment f, b;
        makePair (fc, 0.7071, -2.6, 0.0, f, b);
        const auto c = lowpassCorner (filterResponse (f, b));
        INFO ("fc " << fc << " found " << c.hz);
        REQUIRE (c.found);
        CHECK (std::abs (c.hz / fc - 1.0) < 0.01);
        CHECK (std::abs (c.plateauDb - (-2.6)) < 0.1);
    }
}

TEST_CASE ("analog measure: corner within 3 % with +-1.5 dB noise per bin", "[measure]")
{
    SpecSegment f, b;
    makePair (2500.0, 0.7071, 0.0, 1.5, f, b);
    const auto c = lowpassCorner (filterResponse (f, b));
    REQUIRE (c.found);
    CHECK (std::abs (c.hz / 2500.0 - 1.0) < 0.03);
}

TEST_CASE ("analog measure: resonance peak of a two-pole low-pass", "[measure]")
{
    for (double q : { 1.0, 2.0, 4.0 })
    {
        SpecSegment f, b;
        makePair (3000.0, q, -1.0, 0.0, f, b);
        const auto p = resonancePeak (filterResponse (f, b));
        REQUIRE (p.found);
        // peak of |H| for Q > 0.707: Q / sqrt (1 - 1/(4 Q^2)); the 1/12-octave bins and the 5-bin mean
        // round it off a little at high Q
        const double expect = 20.0 * std::log10 (q / std::sqrt (1.0 - 1.0 / (4.0 * q * q)));
        INFO ("Q " << q << " peak " << p.db << " expected " << expect);
        CHECK (std::abs (p.db - expect) < (q > 3.0 ? 2.5 : 0.6));
        // the peak lies at fc * sqrt (1 - 1/(2 Q^2)) for Q > 0.707
        CHECK (std::abs (p.hz / (3000.0 * std::sqrt (1.0 - 1.0 / (2.0 * q * q))) - 1.0) < 0.06);
    }
}

TEST_CASE ("analog measure: no corner when the response never falls 3 dB or falls at once", "[measure]")
{
    SpecSegment f, b;
    f.spectrum.fill (-200);
    b.spectrum.fill (-200);
    CHECK_FALSE (lowpassCorner (filterResponse (f, b)).found);
    makePair (15.0, 0.7071, 0.0, 0.0, f, b);
    CHECK_FALSE (lowpassCorner (filterResponse (f, b)).found);
}

TEST_CASE ("analog measure: tone level reads the nearest bin", "[measure]")
{
    SpecSegment s;
    s.spectrum.fill (-1200);
    s.spectrum[60] = -55; // 640 Hz
    CHECK (std::abs (toneLevelDb (s, 640.0) + 5.5) < 1e-9);
    CHECK (std::abs (toneLevelDb (s, 655.0) + 5.5) < 1e-9);
    CHECK (std::abs (toneLevelDb (s, 5.0) + 120.0) < 1e-9);
}
