// Verbatim copy of Model A's cutoff law of the S3a form (commit 44bc753, source/bside/chip/Filter.cpp: kALadderRatio,
// kALogFc, cutoffWeights, converterAt, modelALnHz, Filter::modelACutoffConverter, Filter::cutoffCurveHz and the corner
// of Filter::cutAt). See S3aCutoffLaw.h. Test code only.

#include "S3aCutoffLaw.h"

#include "bside/chip/Filter.h"
#include "bside/chip/RenderTables.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vs::bside::test::s3a
{
namespace
{
using namespace vs::bside::chip;

constexpr double kALadderRatio = 2.1756; // the cutoff converter: 11-bit terminated ladder (fitted)
// ln (fc / Hz) at d = i / 23, i = 0 .. 23 (piecewise linear in d)
constexpr std::array<double, 24> kALogFc { 5.566978, 5.601697, 5.794737, 5.830848, 5.952658, 7.166796, 7.832795, 8.205156,
                                           8.463033, 8.749728, 8.914376, 9.091689, 9.229615, 9.347138, 9.468028, 9.542380,
                                           9.620053, 9.686958, 9.733824, 9.790228, 9.835870, 9.887333, 9.914261, 9.951000 };
constexpr double kVariationShift = Filter::kVariationShift;

const render::LadderWeights& cutoffWeights() noexcept
{
    static const render::LadderWeights w = render::ladderWeights (11, kALadderRatio, true);
    return w;
}

double converterAt (int v) noexcept
{
    const auto& w = cutoffWeights();
    double d = 0.0;
    for (std::size_t i = 0; i < 11; ++i)
        if (((v >> i) & 1) != 0)
            d += w[i];
    return d;
}

/** ln (fc / Hz) of Model A's curve at a converter value d (variation included). */
double modelALnHz (double d) noexcept
{
    const double x = std::clamp (d, 0.0, 1.0) * 23.0;
    const auto i = static_cast<std::size_t> (std::min (22.0, std::floor (x)));
    const double f = x - static_cast<double> (i);
    return kALogFc[i] + (kALogFc[i + 1] - kALogFc[i]) * f;
}

double modelACutoffConverter (double registerValue) noexcept
{
    const double v = std::isfinite (registerValue) ? std::clamp (registerValue, 0.0, 2047.0) : 0.0;
    const int lo = static_cast<int> (std::floor (v));
    const int hi = std::min (2047, lo + 1);
    const double f = v - lo;
    return converterAt (lo) + (converterAt (hi) - converterAt (lo)) * f;
}
} // namespace

double cutoffCurveHzA (double registerValue, double variationValue) noexcept
{
    const double v = std::isfinite (registerValue) ? std::clamp (registerValue, 0.0, 2047.0) : 0.0;
    const double var = std::isfinite (variationValue) ? std::clamp (variationValue, 0.0, 1.0) : 0.5;
    return std::exp (modelALnHz (modelACutoffConverter (v) + (var - 0.5) * 2.0 * kVariationShift));
}

double cornerWithCutoffHzA (float registerValue, float variation) noexcept
{
    const double v = std::isfinite (registerValue) ? std::clamp (static_cast<double> (registerValue), 0.0, 2047.0) : 0.0;
    const double lnBase = modelALnHz (modelACutoffConverter (v) + (static_cast<double> (variation) - 0.5) * 2.0 * kVariationShift);
    return std::exp (lnBase);
}
} // namespace vs::bside::test::s3a
