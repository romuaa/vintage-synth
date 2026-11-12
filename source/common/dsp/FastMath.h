#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace vs::dsp
{
/**
    Branch-free, inlinable approximations of the transcendental functions on the voice path.
    Plain scalar code: loops over arrays of these vectorise on NEON and SSE without intrinsics.
    Error bounds are checked by tests/dsp_unit/FastMathTests.cpp.
*/
namespace fast
{
/** Round toward minus infinity for |x| < 2^31 (no libm call, vectorises). */
inline float floorFast (float x) noexcept
{
    const float t = static_cast<float> (static_cast<std::int32_t> (x));
    return t > x ? t - 1.0f : t;
}

/**
    2^x. Relative error <= 2e-7 for x in [-126, 126] (measured 1.6e-7 = 0.0003 cent with FMA
    contraction, arm64; 1.7e-7 without, x64);
    exact at integer x (p(0) = 1), so octaves stay exact. Input is clamped to [-126, 126]
    (so +-Inf give 2^+-126); NaN in gives NaN out on every platform (the integer conversion only
    ever sees a clamped, finite value).
    Degree-5 polynomial for 2^f on [0, 1), fitted for minimum relative error.
*/
inline float exp2 (float x) noexcept
{
    // Written so that NaN fails the first comparison and becomes -126 here.
    const float xc = x > -126.0f ? (x < 126.0f ? x : 126.0f) : -126.0f;
    const float fl = floorFast (xc);
    const float f = xc - fl;
    const float p = 1.0f + f * (6.9315131187e-01f
                       + f * (2.4016444971e-01f
                       + f * (5.5799914206e-02f
                       + f * (9.0170292088e-03f
                       + f * 1.8671304683e-03f))));
    const auto bits = static_cast<std::uint32_t> (static_cast<std::int32_t> (fl) + 127) << 23;
    const float r = p * std::bit_cast<float> (bits);
    return std::isnan (x) ? x : r; // a select, not a branch: keeps NaN visible to the output guard
}

/**
    tan(x) for x in [0, 1.35] (prewarping up to 0.43 x the sample rate), from Taylor series of
    sin (to x^11) and cos (to x^12): truncation error < 1e-8, so float rounding dominates:
    relative error <= 5e-7 (measured 3.3e-7 with FMA, arm64; 4.45e-7 without, x64, where the
    cos sum cancels near 1.35). NaN in gives NaN out.
*/
inline float tanPrewarp (float x) noexcept
{
    const float x2 = x * x;
    const float s = x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f
                           + x2 * (1.0f / 362880.0f + x2 * (-1.0f / 39916800.0f))))));
    const float c = 1.0f + x2 * (-0.5f + x2 * (1.0f / 24.0f + x2 * (-1.0f / 720.0f + x2 * (1.0f / 40320.0f
                           + x2 * (-1.0f / 3628800.0f + x2 * (1.0f / 479001600.0f))))));
    return s / c;
}

/**
    tanh(x), odd and bounded to [-1, 1]. Absolute error <= 2e-7 over all x (measured 1.3e-7
    with and without FMA);
    relative error <= 1e-6 (series below |x| = 0.25, exp form above). +-Inf give +-1; NaN in
    gives NaN out, so a NaN upstream reaches the processor's output guard instead of
    becoming a stuck full-scale signal.
*/
inline float tanh (float x) noexcept
{
    const float ax = x < 0.0f ? -x : x;
    const float x2 = x * x;
    // Taylor series to x^9: relative truncation error < 1e-8 for |x| < 0.25.
    const float series = x * (1.0f + x2 * (-1.0f / 3.0f + x2 * (2.0f / 15.0f + x2 * (-17.0f / 315.0f + x2 * (62.0f / 2835.0f)))));
    // 1 - 2 / (e^(2|x|) + 1), clamped where tanh is 1 to float precision.
    const float e = exp2 ((ax < 9.0f ? ax : 9.0f) * 2.8853900817779268f); // 2 / ln 2
    const float big = 1.0f - 2.0f / (e + 1.0f);
    const float r = ax < 0.25f ? series : (x < 0.0f ? -big : big);
    return std::isnan (x) ? x : r;
}
} // namespace fast
} // namespace vs::dsp
