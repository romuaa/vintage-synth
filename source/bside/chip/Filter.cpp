// The chip's filter (Filter.h; docs/architecture.md 8.19.1; every constant: docs/decisions.md "B-Side S3a").
// All constants are our own fits to data generated with the reference adapter (noise through the filter
// and its bypass twin, a two-pole response fitted per setting; static levels for the output functions).

#include "bside/chip/Filter.h"

#include "bside/chip/RenderTables.h"

#include <algorithm>

namespace vs::bside::chip
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

// ---- Model B ----
// fc (Hz) = c3 FC^3 + c2 FC^2 + c1 FC + c0 (fitted over FC 64 .. 2047, largest error 0.22 %), at least kBMinHz.
constexpr double kBCubic[4] { 5.25089373e-09, 9.52640664e-05, 6.91631996, -1.13988415 };
constexpr double kBMinHz = 15.0; // FC 0: the reference's corner lies below the measurement's reach (judgement call)
// 1 / Q per resonance step (mean of the fits at FC 256, 1024, 1792)
constexpr std::array<double, 16> kBInvQ { 1.4121, 1.2927, 1.1832, 1.0851, 1.0043, 0.9202, 0.8436, 0.7748,
                                          0.7032, 0.6444, 0.5904, 0.5420, 0.5027, 0.4604, 0.4232, 0.3883 };
// cutoff factor per resonance step (the reference's corner falls slightly with resonance)
constexpr std::array<double, 16> kBFcRes { 1.0000, 0.9983, 0.9969, 0.9952, 0.9946, 0.9929, 0.9922, 0.9914,
                                           0.9903, 0.9894, 0.9891, 0.9881, 0.9875, 0.9868, 0.9867, 0.9858 };
// outputs (render units, against the direct path): low-pass and high-pass inverted
constexpr double kBLow = -0.997, kBBand = 1.011, kBHigh = -1.025;

// ---- Model A ----
constexpr double kALadderRatio = 2.1756; // the cutoff converter: 11-bit terminated ladder (fitted)
// ln (fc / Hz) at d = i / 23, i = 0 .. 23 (piecewise linear in d)
constexpr std::array<double, 24> kALogFc { 5.566978, 5.601697, 5.794737, 5.830848, 5.952658, 7.166796, 7.832795, 8.205156,
                                           8.463033, 8.749728, 8.914376, 9.091689, 9.229615, 9.347138, 9.468028, 9.542380,
                                           9.620053, 9.686958, 9.733824, 9.790228, 9.835870, 9.887333, 9.914261, 9.951000 };
// Q at resonance 0 against log2 (fc), piecewise linear between these points (held outside)
constexpr std::array<double, 8> kAQ0Hz { 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0, 21000.0 };
constexpr std::array<double, 8> kAQ0 { 0.79, 0.62, 0.55, 0.55, 0.651, 0.745, 0.764, 0.77 };
// per resonance step: Q factor, cutoff factor, low-pass gain (mean of the fits at FC 1024 and 1536)
constexpr std::array<double, 16> kAQRes { 1.0000, 1.0355, 1.0735, 1.1122, 1.1510, 1.2076, 1.2697, 1.3393,
                                          1.4313, 1.5222, 1.6307, 1.7792, 1.9363, 2.1375, 2.4440, 2.8952 };
constexpr std::array<double, 16> kAFcRes { 1.0000, 0.9944, 0.9814, 0.9792, 0.9746, 0.9672, 0.9571, 0.9526,
                                           0.9397, 0.9322, 0.9257, 0.9117, 0.9067, 0.8981, 0.8847, 0.8710 };
constexpr std::array<double, 16> kALowGain { 1.0000, 1.0087, 1.0170, 1.0263, 1.0397, 1.0483, 1.0587, 1.0711,
                                             1.0803, 1.0925, 1.1118, 1.1231, 1.1414, 1.1638, 1.1806, 1.1958 };
// static output functions of the low-pass state v (render units): low-pass l1 v + ... + l5 v^5 (monotonic),
// band-pass leak b1 v + b2 v^2 + b3 v^3, high-pass leak h1 v + h2 v^2
constexpr double kAL1 = -1.012862, kAL2 = 0.360214, kAL3 = -0.237858, kAL4 = 0.090060, kAL5 = -0.011228;
constexpr double kAB1 = 0.173416, kAB2 = -0.073527, kAB3 = 0.023741;
constexpr double kAH1 = -0.014301, kAH2 = -0.007776;
// the low-pass state is limited to about the range the fit saw (0 .. 3.7: three voices at full level) plus margin
constexpr double kALpMin = -1.5, kALpMax = 4.5;
// band-pass output k0 x bp x kABand x (1 + kABandRes (lowGain - 1)); high-pass kAHigh hp + kAHighBand k0 bp.
// Fitted from the mode responses at FC 612 and 1024 (0.525, -0.537, -0.156), then raised by about 1 dB and the
// band leak by 2 dB to meet a_modes_a (decisions "B-Side S3a", review)
constexpr double kABand = 0.59, kABandRes = 0.42, kAHigh = -0.59, kAHighBand = -0.20;

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

double modelAHz (double d) noexcept
{
    const double x = std::clamp (d, 0.0, 1.0) * 23.0;
    const auto i = static_cast<std::size_t> (std::min (22.0, std::floor (x)));
    const double f = x - static_cast<double> (i);
    return std::exp (kALogFc[i] + (kALogFc[i + 1] - kALogFc[i]) * f);
}

double modelAQ0 (double hz) noexcept
{
    if (hz <= kAQ0Hz.front())
        return kAQ0.front();
    if (hz >= kAQ0Hz.back())
        return kAQ0.back();
    std::size_t i = 0;
    while (i + 2 < kAQ0Hz.size() && hz >= kAQ0Hz[i + 1])
        ++i;
    const double f = std::log2 (hz / kAQ0Hz[i]) / std::log2 (kAQ0Hz[i + 1] / kAQ0Hz[i]);
    return kAQ0[i] + (kAQ0[i + 1] - kAQ0[i]) * f;
}
} // namespace

Filter::Filter() noexcept
{
    (void) cutoffWeights(); // the converter weights are built here, never on the audio thread
    prepare (Model::B, 96000.0, 0.5f);
}

double Filter::modelACutoffConverter (double registerValue) noexcept
{
    const double v = std::isfinite (registerValue) ? std::clamp (registerValue, 0.0, 2047.0) : 0.0;
    const int lo = static_cast<int> (std::floor (v));
    const int hi = std::min (2047, lo + 1);
    const double f = v - lo;
    return converterAt (lo) + (converterAt (hi) - converterAt (lo)) * f;
}

double Filter::cutoffCurveHz (Model model, double registerValue, double variationValue) noexcept
{
    const double v = std::isfinite (registerValue) ? std::clamp (registerValue, 0.0, 2047.0) : 0.0;
    if (model == Model::B)
        return std::max (kBMinHz, ((kBCubic[0] * v + kBCubic[1]) * v + kBCubic[2]) * v + kBCubic[3]);
    const double var = std::isfinite (variationValue) ? std::clamp (variationValue, 0.0, 1.0) : 0.5;
    return modelAHz (modelACutoffConverter (v) + (var - 0.5) * 2.0 * kVariationShift);
}

void Filter::prepare (Model model, double internalRate, float variationValue) noexcept
{
    (void) cutoffWeights();
    mdl = model;
    rate = internalRate > 1000.0 && std::isfinite (internalRate) ? internalRate : 96000.0;
    variation = std::isfinite (variationValue) ? std::clamp (variationValue, 0.0f, 1.0f) : 0.5f;
    updateCoefficients();
    updateOutputs();
}

void Filter::reset() noexcept
{
    s1 = 0.0;
    s2 = 0.0;
}

void Filter::setCutoff (float registerValue) noexcept
{
    const float v = std::isfinite (registerValue) ? std::clamp (registerValue, 0.0f, 2047.0f) : 0.0f;
    if (v < fcReg || v > fcReg) // a change of the stored value (no float ==: GCC -Wfloat-equal)
    {
        fcReg = v;
        updateCoefficients();
        if (mdl == Model::A)
            updateOutputs(); // the band-pass gain follows Q at resonance 0
    }
}

void Filter::setResonance (int value) noexcept
{
    const int r = std::clamp (value, 0, 15);
    if (r != res)
    {
        res = r;
        updateCoefficients();
        updateOutputs();
    }
}

void Filter::setMode (std::uint8_t modeVolume) noexcept
{
    const auto m = static_cast<std::uint8_t> (modeVolume & kModeMask);
    if (m != modeBits)
    {
        modeBits = m;
        updateOutputs();
    }
}

void Filter::setVariation (float value) noexcept
{
    const float v = std::isfinite (value) ? std::clamp (value, 0.0f, 1.0f) : 0.5f;
    if (v < variation || v > variation)
    {
        variation = v;
        updateCoefficients();
        updateOutputs();
    }
}

void Filter::updateCoefficients() noexcept
{
    const auto r = static_cast<std::size_t> (res);
    const double base = cutoffCurveHz (mdl, static_cast<double> (fcReg), static_cast<double> (variation));
    if (mdl == Model::B)
    {
        fcHz = base * kBFcRes[r];
        k0 = kBInvQ[0];
        k = kBInvQ[r];
    }
    else
    {
        fcHz = base * kAFcRes[r];
        k0 = 1.0 / modelAQ0 (base);
        k = k0 / kAQRes[r];
    }
    // never at or above a quarter of the rate's Nyquist margin (tan grows without bound at rate / 2)
    const double f = std::min (fcHz, 0.45 * rate);
    g = std::tan (kPi * f / rate);
    kg = k + g;
    h = 1.0 / (1.0 + g * kg);
    updateStateSpace();
}

void Filter::updateStateSpace() noexcept
{
    // hp = hx x + h1 s1 + h2 s2, bp = g hp + s1, lp = g bp + s2 (Filter.h, process)
    const double hx = h, h1 = -h * kg, h2 = -h;
    const double bx = g * hx, b1 = g * h1 + 1.0, b2 = g * h2;
    lx = g * bx;
    l1 = g * b1;
    l2 = g * b2 + 1.0;
    a1x = 2.0 * bx;
    a11 = 2.0 * b1 - 1.0;
    a12 = 2.0 * b2;
    a2x = 2.0 * lx;
    a21 = 2.0 * l1;
    a22 = 2.0 * l2 - 1.0;
    ox = cLp * lx + cBp * bx + cHp * hx;
    o1 = cLp * l1 + cBp * b1 + cHp * h1;
    o2 = cLp * l2 + cBp * b2 + cHp * h2;
}

void Filter::updateOutputs() noexcept
{
    const bool lo = (modeBits & kLowPass) != 0, band = (modeBits & kBandPass) != 0, high = (modeBits & kHighPass) != 0;
    if (mdl == Model::B)
    {
        cLp = lo ? kBLow : 0.0;
        cBp = band ? kBBand : 0.0;
        cHp = high ? kBHigh : 0.0;
        p1 = p2 = p3 = p4 = p5 = 0.0;
        lpMin = -1.0e9;
        lpMax = 1.0e9;
        updateStateSpace();
        return;
    }
    const double lowGain = kALowGain[static_cast<std::size_t> (res)];
    const double bandRes = 1.0 + kABandRes * (lowGain - 1.0);
    cLp = 0.0;
    cBp = (band ? kABand * k0 * bandRes : 0.0) + (high ? kAHighBand * k0 * bandRes : 0.0);
    cHp = high ? kAHigh : 0.0;
    p1 = (lo ? kAL1 * lowGain : 0.0) + (band ? kAB1 : 0.0) + (high ? kAH1 : 0.0);
    p2 = (lo ? kAL2 * lowGain : 0.0) + (band ? kAB2 : 0.0) + (high ? kAH2 : 0.0);
    p3 = (lo ? kAL3 * lowGain : 0.0) + (band ? kAB3 : 0.0);
    p4 = lo ? kAL4 * lowGain : 0.0;
    p5 = lo ? kAL5 * lowGain : 0.0;
    updateStateSpace();
    lpMin = kALpMin;
    lpMax = kALpMax;
}

void Filter::process (float* inOut, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        inOut[i] = process (inOut[i]);
}
} // namespace vs::bside::chip
