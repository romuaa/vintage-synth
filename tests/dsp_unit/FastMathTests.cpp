#include <catch2/catch_test_macros.hpp>

#include "dsp/FastMath.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

using namespace vs::dsp;

namespace
{
bool sameBits (float a, float b) { return std::bit_cast<std::uint32_t> (a) == std::bit_cast<std::uint32_t> (b); }
} // namespace

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
