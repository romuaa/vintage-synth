// The measurement side of the .bsspec format (source/bside/trace/SpecAnalysis.h): sample and frame
// counts, the derived segment plan, and the two spectra on signals with a known answer.

#include "bside/trace/SpecAnalysis.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace vs::bside::trace;

namespace
{
Trace makeTrace (std::uint32_t clock, std::uint64_t end, std::vector<std::uint64_t> writeCycles)
{
    Trace t;
    t.clockHz = clock;
    t.model = Model::B;
    for (auto c : writeCycles)
        t.writes.push_back ({ c, 0x18, 0x0f });
    t.endCycle = end;
    return t;
}

std::vector<float> sine (std::size_t n, double hz, double amplitude)
{
    std::vector<float> x (n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = static_cast<float> (amplitude * std::sin (2.0 * 3.14159265358979323846 * hz * static_cast<double> (i) / 48000.0));
    return x;
}
} // namespace

TEST_CASE ("spec analysis: sample, frame and cycle arithmetic", "[spec]")
{
    const auto t = makeTrace (985248, 985248, {});
    CHECK (specSampleCount (t) == 48000);
    CHECK (specFrameCount (t) == 40);
    CHECK (frameOfCycle (t, 0) == 0);
    CHECK (frameOfCycle (t, 24631) == 0);  // 24631.2 cycles per frame
    CHECK (frameOfCycle (t, 24632) == 1);
    const auto u = makeTrace (985248, 1, {});
    CHECK (specSampleCount (u) == 1); // ceil
    CHECK (specFrameCount (u) == 1);
    const auto z = makeTrace (985248, 0, {});
    CHECK (specFrameCount (z) == 1); // a spec has at least one frame
}

TEST_CASE ("spec analysis: the segment plan follows the writes", "[spec]")
{
    const std::uint32_t clock = 985248;
    const auto frameCycle = [&] (std::uint64_t f) { return (f * clock + 39) / 40 + 10; };

    SECTION ("each gap long enough gives one segment from the 9th frame after its write")
    {
        // writes in frames 1, 26, 51; end at frame 76
        const auto t = makeTrace (clock, (76 * clock + 39) / 40, { 8, frameCycle (1), frameCycle (26), frameCycle (51) });
        const auto plan = segmentPlan (t);
        REQUIRE (plan.size() == 3);
        CHECK (plan[0].name == "s00");
        CHECK (plan[0].firstFrame == 10);
        CHECK (plan[0].endFrame == 26);
        CHECK (plan[1].name == "s01");
        CHECK (plan[1].firstFrame == 35);
        CHECK (plan[1].endFrame == 51);
        CHECK (plan[2].firstFrame == 60);
        CHECK (plan[2].endFrame == 76);
    }
    SECTION ("short gaps are not marked; equal cycles are one event")
    {
        const auto t = makeTrace (clock, (100 * clock + 39) / 40, { frameCycle (1), frameCycle (1), frameCycle (20), frameCycle (50) });
        const auto plan = segmentPlan (t);
        REQUIRE (plan.size() == 2); // gap 1 -> 20 leaves 10 frames (< 14): skipped
        CHECK (plan[0].name == "s00");
        CHECK (plan[0].firstFrame == 29);
        CHECK (plan[0].endFrame == 50);
        // 50 -> 100: [59, 100)
        CHECK (plan[1].firstFrame == 59);
        CHECK (plan[1].endFrame == 100);
    }
    SECTION ("at most 64 segments")
    {
        std::vector<std::uint64_t> w;
        for (std::uint64_t k = 0; k < 70; ++k)
            w.push_back (frameCycle (1 + 25 * k));
        const auto t = makeTrace (clock, (1800 * clock + 39) / 40, w);
        const auto plan = segmentPlan (t);
        REQUIRE (plan.size() == 64);
        CHECK (plan.back().name == "s63");
    }
    SECTION ("a trace that does not validate has no plan")
    {
        auto t = makeTrace (clock, 100, { 200 });
        CHECK (segmentPlan (t).empty());
    }
}

TEST_CASE ("spec analysis: a full-scale sine reads -3 dB in its band and its 1/12-octave bin", "[spec]")
{
    const double hz = 20.0 * std::pow (2.0, 60.0 / 12.0); // bin 60 = 640 Hz
    const auto x = sine (48000, hz, 1.0);

    std::vector<std::int16_t> level, bands;
    featureFrames (x.data(), x.size(), 40, level, bands);
    REQUIRE (level.size() == 40);
    CHECK (std::abs (level[20] - (-30)) <= 1);
    // 640 Hz lies in band 8 (lower edges 125 * 2^((b - 1)/3 - 1/6)): 561 .. 707 Hz
    CHECK (std::abs (bands[20 * kSpecBands + 8] - (-30)) <= 1);
    CHECK (bands[20 * kSpecBands + 4] < -800);

    const auto s = twelfthOctaveSpectrum (x.data(), 0, x.size());
    CHECK (std::abs (s[60] - (-30)) <= 1);
    CHECK (s[54] < -800); // half an octave away: window leakage only
    CHECK (s[66] < -800);
}

TEST_CASE ("spec analysis: white noise gains 3 dB per octave in the 1/12-octave bins", "[spec]")
{
    // Deterministic uniform noise: flat density, so band power is proportional to the bandwidth:
    // 12 bins (one octave) apart = 3.01 dB, averaged over several bins.
    std::vector<float> x (48000 * 4);
    std::uint32_t r = 12345;
    for (auto& v : x)
    {
        r = r * 1664525u + 1013904223u;
        v = static_cast<float> ((static_cast<double> (r >> 8) / 16777216.0 - 0.5) * 0.5);
    }
    const auto s = twelfthOctaveSpectrum (x.data(), 0, x.size());
    double lo = 0, hi = 0;
    for (int k = 72; k < 84; ++k)
        lo += s[static_cast<std::size_t> (k)];
    for (int k = 96; k < 108; ++k)
        hi += s[static_cast<std::size_t> (k)];
    CHECK (std::abs ((hi - lo) / 12.0 / 10.0 - 2 * 3.0103) < 0.5);
}

TEST_CASE ("spec analysis: analyseRender builds a spec that validates", "[spec]")
{
    const std::uint32_t clock = 985248;
    const auto t = makeTrace (clock, (60 * clock + 39) / 40, { (1 * clock + 39) / 40 + 10 });
    const auto n = specSampleCount (t);
    const auto x = sine (static_cast<std::size_t> (n), 1000.0, 0.25);
    Spec s;
    std::string why;
    REQUIRE (analyseRender (t, std::string (64, 'a'), x, s, &why));
    CHECK (s.frames == 60);
    REQUIRE (s.segments.size() == 1);
    CHECK (s.segments[0].firstFrame == 10);
    CHECK (s.segments[0].endFrame == 60);
    std::string text;
    CHECK (writeSpec (s, text));

    std::vector<float> wrong (x.begin(), x.end() - 1);
    CHECK_FALSE (analyseRender (t, std::string (64, 'a'), wrong, s, &why));
}

TEST_CASE ("spec analysis: band 22 counts 16 kHz .. 20 kHz only", "[spec]")
{
    std::vector<std::int16_t> level, bands;
    const auto hi = sine (48000, 22000.0, 1.0);
    featureFrames (hi.data(), hi.size(), 40, level, bands);
    CHECK (bands[20 * kSpecBands + 22] < -800);        // above the limit: left out
    CHECK (std::abs (level[20] - (-30)) <= 1);         // the level is not band-limited
    featureFrames (hi.data(), hi.size(), 40, level, bands, 24000.0);
    CHECK (std::abs (bands[20 * kSpecBands + 22] - (-30)) <= 1); // without the limit (harness layout)
    const auto in = sine (48000, 18000.0, 1.0);
    featureFrames (in.data(), in.size(), 40, level, bands);
    CHECK (std::abs (bands[20 * kSpecBands + 22] - (-30)) <= 1); // below the limit: counted
}
