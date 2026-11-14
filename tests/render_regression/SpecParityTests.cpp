// The B-Side spec features (source/bside/trace/SpecAnalysis.h, JUCE-free, used by the reference adapter
// and our replay tests) must measure like this harness's extractFeatures (docs/bside-oracle-adapter.md:
// "the feature extraction is the same one the repository's render-regression harness uses"). Both
// implement the same algorithm; they differ only in the FFT (JUCE float here, double there), so every
// cell may differ by at most one 0.1 dB step (a rounding boundary).
#include <catch2/catch_test_macros.hpp>

#include "RenderHarness.h"

#include "bside/trace/SpecAnalysis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;
}

TEST_CASE ("B-Side spec features equal the render-regression features", "[render][bside][spec]")
{
    // 3.01 s at 48 kHz (a partial last frame): a decaying chord, a sweep and noise bursts, so that bands
    // span 0 .. -120 dB.
    const std::size_t n = 48000 * 3 + 517;
    std::vector<float> x (n);
    std::uint32_t r = 1;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) / 48000.0;
        double v = 0.3 * std::exp (-t) * (std::sin (2 * kPi * 220.0 * t) + 0.5 * std::sin (2 * kPi * 1375.0 * t) + 0.25 * std::sin (2 * kPi * 7040.0 * t));
        v += 0.1 * std::sin (2 * kPi * (50.0 + 3000.0 * t) * t);
        r = r * 1664525u + 1013904223u;
        if (static_cast<int> (t * 4.0) % 3 == 1)
            v += 0.05 * (static_cast<double> (r >> 8) / 16777216.0 - 0.5);
        if (t > 2.5)
            v *= 1e-4; // a quiet tail
        x[i] = static_cast<float> (v);
    }

    const auto ref = vs::rr::extractFeatures (x, 48000.0);
    std::vector<std::int16_t> level, bands;
    vs::bside::trace::featureFrames (x.data(), x.size(), ref.frames, level, bands, 24000.0); // no band limit: the harness's layout
    REQUIRE (ref.frames == static_cast<int> ((n + 1199) / 1200));

    int worst = 0, differing = 0;
    for (int f = 0; f < ref.frames; ++f)
    {
        const int a = static_cast<int> (std::lround (ref.levelDb[static_cast<std::size_t> (f)] * 10.0f));
        worst = std::max (worst, std::abs (a - level[static_cast<std::size_t> (f)]));
        differing += a != level[static_cast<std::size_t> (f)] ? 1 : 0;
        for (int b = 0; b < vs::rr::kNumBands; ++b)
        {
            const int c = static_cast<int> (std::lround (ref.band (f, b) * 10.0f));
            const int d = bands[static_cast<std::size_t> (f * vs::bside::trace::kSpecBands + b)];
            worst = std::max (worst, std::abs (c - d));
            differing += c != d ? 1 : 0;
        }
    }
    INFO ("cells differing by one 0.1 dB step: " << differing << " of " << ref.frames * 24);
    CHECK (worst <= 1);
    CHECK (differing * 100 <= ref.frames * 24); // rounding-boundary cases only: at most 1 %
}
