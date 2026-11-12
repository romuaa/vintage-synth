#include <catch2/catch_test_macros.hpp>

#include "common/dsp/FastMath.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace vs::dsp;

namespace
{
bool sameBits (float a, float b) { return std::bit_cast<std::uint32_t> (a) == std::bit_cast<std::uint32_t> (b); }
} // namespace

// Measured worst cases: arm64 (FMA contraction) / x64 (no FMA). The bounds hold for both.
//   exp2 1.58e-7 / 1.74e-7 relative; tanPrewarp 3.3e-7 / 4.45e-7 relative;
//   tanh 1.27e-7 / 1.22e-7 absolute.

TEST_CASE ("fast::exp2 relative error <= 2e-7 (0.00035 cent) and exact octaves", "[fastmath]")
{
    double worst = 0.0;
    for (double x = -40.0; x <= 20.0; x += 1.0e-4)
    {
        const auto xf = static_cast<float> (x);
        const double ref = std::exp2 (static_cast<double> (xf));
        worst = std::max (worst, std::abs (static_cast<double> (fast::exp2 (xf)) / ref - 1.0));
    }
    INFO ("max relative error " << worst << " = " << 1200.0 * std::log2 (1.0 + worst) << " cent");
    CHECK (worst <= 2.0e-7);

    for (int octave = -30; octave <= 20; ++octave)
    {
        const auto x = static_cast<float> (octave);
        CHECK (sameBits (fast::exp2 (x), std::exp2 (x)));
    }
    CHECK (std::isfinite (fast::exp2 (1000.0f)));
    CHECK (fast::exp2 (-1000.0f) > 0.0f);
}

TEST_CASE ("fast::tanPrewarp relative error <= 5e-7 up to 0.43 x the sample rate", "[fastmath]")
{
    double worst = 0.0;
    for (double f = 1.0e-5; f <= 0.43; f += 1.0e-6) // fc / fs
    {
        const auto x = static_cast<float> (3.14159265358979323846 * f);
        const double ref = std::tan (static_cast<double> (x));
        worst = std::max (worst, std::abs (static_cast<double> (fast::tanPrewarp (x)) / ref - 1.0));
    }
    INFO ("max relative error " << worst);
    CHECK (worst <= 5.0e-7);
}

TEST_CASE ("fast::tanh is odd, bounded, absolute error <= 2e-7, relative <= 1e-6", "[fastmath]")
{
    double worstAbs = 0.0, worstRel = 0.0;
    bool bounded = true, odd = true;
    for (double x = -20.0; x <= 20.0; x += 1.0e-5)
    {
        const auto xf = static_cast<float> (x);
        const double ref = std::tanh (static_cast<double> (xf));
        const auto y = static_cast<double> (fast::tanh (xf));
        bounded = bounded && std::abs (y) <= 1.0;
        odd = odd && sameBits (fast::tanh (-xf), -fast::tanh (xf));
        worstAbs = std::max (worstAbs, std::abs (y - ref));
        if (std::abs (ref) > 1.0e-30)
            worstRel = std::max (worstRel, std::abs (y / ref - 1.0));
    }
    INFO ("max abs error " << worstAbs << ", max rel error " << worstRel);
    CHECK (bounded);
    CHECK (odd);
    CHECK (worstAbs <= 2.0e-7);
    CHECK (worstRel <= 1.0e-6);
    CHECK (sameBits (fast::tanh (1.0e-30f), 1.0e-30f)); // tiny signals pass unchanged (filter decays to 0)
    CHECK (sameBits (fast::tanh (0.0f), 0.0f));
}

TEST_CASE ("fast math: NaN in gives NaN out, Inf is handled", "[fastmath]")
{
    // NaN must survive so the processor's non-finite output guard sees it (a tanh that turns
    // NaN into 1 would give a stuck full-scale signal instead). Same result on every platform:
    // exp2 never converts NaN to an integer.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    CHECK (std::isnan (fast::exp2 (nan)));
    CHECK (std::isnan (fast::tanh (nan)));
    CHECK (std::isnan (fast::tanPrewarp (nan)));

    CHECK (sameBits (fast::exp2 (inf), std::exp2 (126.0f)));
    CHECK (sameBits (fast::exp2 (-inf), std::exp2 (-126.0f)));
    CHECK (std::abs (fast::tanh (inf) - 1.0f) <= 2.0e-7f);  // within the absolute bound (1 ulp below 1)
    CHECK (std::abs (fast::tanh (-inf) + 1.0f) <= 2.0e-7f);
}
