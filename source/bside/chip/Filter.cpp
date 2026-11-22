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
constexpr double kALowScale = 1.10;
constexpr double kAL1 = -0.598556 * kALowScale, kAL2 = -0.127445 * kALowScale, kAL3 = -0.039509 * kALowScale, kAL4 = 0.043390 * kALowScale, kAL5 = -0.006265 * kALowScale;
constexpr double kAB1 = 0.157448, kAB2 = -0.060120, kAB3 = 0.021538;
constexpr double kAH1 = 0.014064, kAH2 = -0.012227;
// the low-pass state is limited to about the range the fit saw (0 .. 3.7: three voices at full level) plus margin
constexpr double kALpMin = -1.5, kALpMax = 4.5;
// band-pass output k0 x bp x kABand x (1 + kABandRes (lowGain - 1)); high-pass kAHigh hp + kAHighBand k0 bp.
// Fitted from the mode responses at FC 612 and 1024 (0.525, -0.537, -0.156), then raised by about 1 dB and the
// band leak by 2 dB to meet a_modes_a (decisions "B-Side S3a", review)
constexpr double kABand = 0.59, kABandRes = 0.42, kAHigh = -0.59, kAHighBand = -0.20;

// ---- Model A, signal-dependent dynamics (Filter.h "MODEL A, SIGNAL-DEPENDENT") ----
// Filter.h "MODEL A, SIGNAL-DEPENDENT"; decisions "B-Side S3a round 3". Every value is our own fit to noise responses
// generated with the reference adapter on fitting traces (never the committed gate traces).
// the rates' floor (Hz) of the band-pass (1) and the low-pass (2) integrator
constexpr double kAIFloor1 = 230.85, kAIFloor2 = 302.10;
// the damping at resonance 0 (1 / Q of the loop when the two rates are equal)
constexpr double kAIDamp0 = 1.44276;
// the input node's factor (Hz per render unit): the high-pass node for integrator 1, the band-pass node for 2
constexpr double kAIIn1 = 26088.1, kAIIn2 = 31798.6;
// how far each control follows its input node (0: not at all, 0.5: a resistance linear in its input)
constexpr double kAIGate1 = 0.23449, kAIGate2 = 0.23059;
// the low-pass integrator's overdrive falls with the low-pass node's level v (render units): kAIOut2 v, plus
// kAIOut2Sq (v - kAIOutKnee)^2 above the knee
constexpr double kAIOut2 = 2426.18, kAIOut2Sq = 4939.49, kAIOutKnee = 1.9576;
// the softness of the threshold (Hz)
constexpr double kAISoft = 262.51;
// integrator 1's control from integrator 2's: scale and offset (Hz)
constexpr double kAICtl1Scale = 0.89731, kAICtl1Offset = -2.64;
// the control of integrator 2 (Hz) against the cutoff converter's value d (piecewise linear between
// these points: one per fitted cutoff, ascending in d)
constexpr std::array<double, 118> kAICtlD { 0.000000, 0.033387, 0.067252, 0.094807, 0.121001, 0.130842, 0.136674, 0.147136,
                                            0.152893, 0.162407, 0.168239, 0.179575, 0.187467, 0.195137, 0.196360, 0.196432,
                                            0.197508, 0.197655, 0.198731, 0.199746, 0.200969, 0.202117, 0.203340, 0.204102,
                                            0.205325, 0.206473, 0.207696, 0.208711, 0.209934, 0.211082, 0.212305, 0.212579,
                                            0.213802, 0.214313, 0.214950, 0.216173, 0.217188, 0.218411, 0.219559, 0.220145,
                                            0.220782, 0.221544, 0.222767, 0.223915, 0.225138, 0.226153, 0.227376, 0.228524,
                                            0.229073, 0.229747, 0.230296, 0.231444, 0.232667, 0.233682, 0.234905, 0.236053,
                                            0.237276, 0.238038, 0.239262, 0.240410, 0.241633, 0.242647, 0.243870, 0.245018,
                                            0.246241, 0.246515, 0.247739, 0.248886, 0.250110, 0.250602, 0.251124, 0.251825,
                                            0.252347, 0.252973, 0.253495, 0.254196, 0.254718, 0.255481, 0.256704, 0.257852,
                                            0.259075, 0.260089, 0.260153, 0.261312, 0.262460, 0.263683, 0.276372, 0.292104,
                                            0.297475, 0.313902, 0.327329, 0.338044, 0.369742, 0.385415, 0.416981, 0.422560,
                                            0.438779, 0.468886, 0.486352, 0.489946, 0.495569, 0.506031, 0.506165, 0.513648,
                                            0.521511, 0.527268, 0.554898, 0.604982, 0.652354, 0.679775, 0.704126, 0.744519,
                                            0.797883, 0.848841, 0.869397, 0.897027, 0.947985, 1.000000 };
constexpr std::array<double, 118> kAICtlHz { -6121.9, -4891.2, -3679.1, -2550.4, -1451.3, -1205.5, -1679.2, -517.3,
                                             -638.2, 647.9, 845.8, 955.7, 1052.3, 1273.9, 1300.5, 1287.7,
                                             1382.2, 1321.3, 1342.6, 1370.0, 1410.1, 1450.8, 1473.0, 1517.7,
                                             1599.0, 1571.8, 1627.3, 1659.5, 1643.1, 1722.6, 1761.9, 1714.7,
                                             1759.8, 1813.6, 1802.2, 1838.3, 1823.5, 1845.3, 1866.0, 1892.2,
                                             1907.0, 1904.1, 1919.0, 1977.1, 1991.9, 1991.0, 2024.6, 2034.8,
                                             2059.9, 2113.3, 2061.9, 2147.1, 2184.8, 2229.9, 2302.2, 2312.4,
                                             2409.1, 2426.5, 2500.4, 2487.2, 2514.1, 2567.9, 2589.6, 2604.9,
                                             2673.9, 2647.8, 2685.6, 2685.5, 2737.6, 2690.2, 2746.2, 2726.0,
                                             2764.4, 2729.0, 2787.2, 2785.0, 2804.1, 2816.0, 2859.8, 2887.1,
                                             2892.3, 2911.0, 2873.2, 2955.9, 2943.8, 2964.9, 3201.9, 4066.5,
                                             4303.9, 4767.9, 5198.3, 5198.0, 6440.4, 7278.7, 8243.3, 8362.8,
                                             9001.3, 10171.7, 10724.3, 10785.7, 11072.9, 11428.4, 11263.4, 11673.9,
                                             11878.5, 11925.6, 13014.8, 14446.9, 15666.4, 16041.8, 17226.8, 18415.8,
                                             19660.9, 20280.3, 20514.0, 20828.2, 21313.1, 21802.1 };
// per resonance step: the factor on Q (the damping is kAIDamp0 / kAIQRes): 1 / (1 - 0.029753 r - 0.0010969 r^2)
constexpr std::array<double, 16> kAIQRes { 1.0000, 1.0318, 1.0683, 1.1100, 1.1582, 1.2139, 1.2788, 1.3550,
                                            1.4456, 1.5543, 1.6870, 1.8519, 2.0618, 2.3374, 2.7140, 3.2584 };
// the loop gain rho = (1 - a r - kAIResCornerSq r^2)^2 for resonance r, with a = kAIResCornerLow where integrator 2's
// control is at or below kAIResCtlLow (Hz) and kAIResCornerHigh at or above kAIResCtlHigh (linear between)
constexpr double kAIResCornerLow = -0.006203, kAIResCornerHigh = 0.005784, kAIResCornerSq = 0.0002439, kAIResCtlLow = 2500.0, kAIResCtlHigh = 4500.0;
// the integrator states are limited to this (render units; a chip's signal stays below 5)
constexpr double kAIStateLimit = 64.0;
// the largest rate the signal may drive an integrator to, as a fraction of the internal rate (see modelARateCaps)
constexpr double kAIDynamicLimit = 0.25;
// band-pass output: kAIBand x bp x (1 + kAIBandRes (lowGain - 1)); high-pass output: kAIHigh hp + kAIHighBand x bp x the
// same resonance factor (fitted on the mode responses of the fitting traces; decisions "B-Side S3a round 3")
constexpr double kAIBand = 0.8250, kAIBandRes = 0.4151, kAIHigh = -0.5469, kAIHighBand = -0.2584;

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
/** Model A: the control of the low-pass integrator (Hz) for a converter value d (piecewise linear, held outside). */
double dynamicControl (double d) noexcept
{
    const double x = std::clamp (d, 0.0, 1.0);
    // the interval that holds x, by bisection (the table is ascending in d; at most 8 steps)
    std::size_t i = 0, hi = kAICtlD.size() - 1;
    while (hi - i > 1)
    {
        const std::size_t mid = (i + hi) / 2;
        if (x >= kAICtlD[mid])
            i = mid;
        else
            hi = mid;
    }
    return kAICtlHz[i] + (kAICtlHz[i + 1] - kAICtlHz[i]) * (x - kAICtlD[i]) / (kAICtlD[i + 1] - kAICtlD[i]);
}

/** Model A: the converter value a register value and a variation give (0..1). */
double modelAConverterValue (double registerValue, double variation) noexcept
{
    return Filter::modelACutoffConverter (registerValue) + (variation - 0.5) * 2.0 * Filter::kVariationShift;
}

/** Model A: the rate above the floor (Hz) of a resistance at overdrive t with nothing across it: the limit of
    (soft (t)^2 - soft (t - e)^2) / (2 e) for e -> 0, soft (t) = (t + sqrt (t^2 + w^2)) / 2. */
double restRate (double t) noexcept
{
    const double s = std::sqrt (t * t + kAISoft * kAISoft);
    return 0.5 * (t + s) * 0.5 * (1.0 + t / s);
}

/** Model A: the low-pass integrator's overdrive for its control and the low-pass node's level. */
double lowOverdrive (double control2, double lp) noexcept
{
    const double over = lp > kAIOutKnee ? lp - kAIOutKnee : 0.0;
    return control2 - kAIOut2 * lp - kAIOut2Sq * over * over;
}

/** Model A: the loop gain rho for a resonance step and the low-pass integrator's control: the corner factor
    1 - a r - b r^2 squared, a between kAIResCornerLow (control at or below kAIResCtlLow) and kAIResCornerHigh (at or
    above kAIResCtlHigh). */
double dynamicLoopGain (int resonance, double control2) noexcept
{
    const double z = std::clamp ((control2 - kAIResCtlLow) / (kAIResCtlHigh - kAIResCtlLow), 0.0, 1.0);
    const double a = kAIResCornerLow + (kAIResCornerHigh - kAIResCornerLow) * z;
    const double r = static_cast<double> (resonance);
    const double f = std::max (0.2, 1.0 - a * r - kAIResCornerSq * r * r);
    return f * f;
}

struct RestRates
{
    double r1, r2;
};

/** Model A: the two rates (Hz) with a constant filter-bus level and nothing sounding. */
RestRates dynamicRestRates (double control1, double control2, double level, double rateMax) noexcept
{
    return { std::min (rateMax, kAIFloor1 + restRate (control1)), std::min (rateMax, kAIFloor2 + restRate (lowOverdrive (control2, level))) };
}

/** Model A: the largest rate (Hz) the signal may drive an integrator to: kAIDynamicLimit x the internal rate, or
    the integrator's own rate at rest where that is higher (a constant rate is stable up to rateMax; rates that jump
    every sample are not, measured: decisions "B-Side S3a round 3"). */
RestRates dynamicRateCaps (double control1, double control2, double internalRate, double rateMax) noexcept
{
    const auto rest = dynamicRestRates (control1, control2, 0.0, rateMax);
    const double dynamic = kAIDynamicLimit * internalRate;
    return { std::max (dynamic, rest.r1), std::max (dynamic, rest.r2) };
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
    restDynamic();
}

void Filter::reset() noexcept
{
    s1 = 0.0;
    s2 = 0.0;
    restDynamic();
}

void Filter::restDynamic() noexcept
{
    // the predictor's memory as after a long rest at the stored cutoff (only processSignalDependent uses it)
    const auto r = dynamicRestRates (ctl1, ctl2, 0.0, rateMax);
    gPrev1 = fastTan (piOverRate * r.r1);
    gPrev2 = fastTan (piOverRate * r.r2);
    dPrev = 1.0 / (1.0 + gPrev1 * (rho * gPrev2 + damp));
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
    piOverRate = kPi / rate;
    rateMax = 0.45 * rate;
    if (mdl == Model::A)
    {
        // the signal-dependent dynamics' controls, loop gain, damping and rate limits for this cutoff and resonance
        ctl2 = dynamicControl (modelAConverterValue (static_cast<double> (fcReg), static_cast<double> (variation)));
        ctl1 = ctl2 * kAICtl1Scale + kAICtl1Offset;
        rho = dynamicLoopGain (res, ctl2);
        damp = kAIDamp0 / kAIQRes[r];
        const auto caps = dynamicRateCaps (ctl1, ctl2, rate, rateMax);
        cap1 = caps.r1;
        cap2 = caps.r2;
    }
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
    // the signal-dependent form: the band-pass state of its two-rate loop peaks at rho / k whatever the cutoff, so
    // its gains are plain
    const double bandResDyn = 1.0 + kAIBandRes * (lowGain - 1.0);
    cBpDyn = (band ? kAIBand * bandResDyn : 0.0) + (high ? kAIHighBand * bandResDyn : 0.0);
    cHpDyn = high ? kAIHigh : 0.0;
    updateStateSpace();
    lpMin = kALpMin;
    lpMax = kALpMax;
}

double Filter::stepA (double x, double control1, double control2, double limit1, double limit2, double loop) noexcept
{
    // 1. predictor: this sample's nodes with the previous sample's rates
    const double fb = loop * gPrev2 + damp;
    const double hpE = (loop * (x - s2) - fb * s1) * dPrev;
    const double bpE = gPrev1 * hpE + s1;
    const double lpE = gPrev2 * bpE + s2;
    // 2. the two rates from the predicted nodes. For a resistance with overdrive t at the integrator's side and
    //    t - e at its input side: rate = (a^2 - b^2) / (2 e), a = soft (t), b = soft (t - e), soft (v) =
    //    (v + sqrt (v^2 + w^2)) / 2, written as (a + b) / 4 x (1 + (2 t - e) / (sa + sb)) with the two square
    //    roots sa, sb: no 0 / 0 at e = 0, and always between 0 and (a + b) / 2. One division serves both.
    const double e1 = kAIIn1 * hpE, e2 = kAIIn2 * bpE;
    const double t1 = control1 + kAIGate1 * e1, t2 = lowOverdrive (control2, lpE) + kAIGate2 * e2;
    const double u1 = t1 - e1, u2 = t2 - e2;
    constexpr double w2 = kAISoft * kAISoft;
    const double sum1 = std::sqrt (t1 * t1 + w2) + std::sqrt (u1 * u1 + w2);
    const double sum2 = std::sqrt (t2 * t2 + w2) + std::sqrt (u2 * u2 + w2);
    const double inv = 1.0 / (sum1 * sum2);
    const double r1 = std::min (limit1, kAIFloor1 + 0.125 * (t1 + u1 + sum1) * (1.0 + (t1 + u1) * sum2 * inv));
    const double r2 = std::min (limit2, kAIFloor2 + 0.125 * (t2 + u2 + sum2) * (1.0 + (t2 + u2) * sum1 * inv));
    // 3. the trapezoidal loop with g1 = tan (pi r1 / rate), g2 likewise (the rational tan of Filter.h as numerator
    //    and denominator, so that one division gives g1, g2 and the solve's reciprocal)
    const double x1 = piOverRate * r1, x2 = piOverRate * r2, z1 = x1 * x1, z2 = x2 * x2;
    const double n1 = x1 * (945.0 - z1 * (105.0 - z1)), d1 = 945.0 - z1 * (420.0 - 15.0 * z1);
    const double n2 = x2 * (945.0 - z2 * (105.0 - z2)), d2 = 945.0 - z2 * (420.0 - 15.0 * z2);
    const double dd = d1 * d2;
    const double m = dd + n1 * (loop * n2 + damp * d2);
    const double rec = 1.0 / (dd * m);
    const double g1 = n1 * d2 * m * rec, g2 = n2 * d1 * m * rec, solve = dd * dd * rec;
    const double hp = (loop * (x - s2) - (loop * g2 + damp) * s1) * solve;
    const double bp = g1 * hp + s1;
    const double lp = g2 * bp + s2;
    // the states, limited (a signal the chip can produce stays far inside; the limit bounds every other input)
    const double q1 = bp + g1 * hp, q2 = lp + g2 * bp;
    s1 = q1 < -kAIStateLimit ? -kAIStateLimit : (q1 > kAIStateLimit ? kAIStateLimit : q1);
    s2 = q2 < -kAIStateLimit ? -kAIStateLimit : (q2 > kAIStateLimit ? kAIStateLimit : q2);
    gPrev1 = g1;
    gPrev2 = g2;
    dPrev = solve;
    settle();
    return cBpDyn * bp + cHpDyn * hp + shape (lp);
}


float Filter::processWithCutoff (float in, float registerValue) noexcept
{
    const double v = std::isfinite (registerValue) ? std::clamp (static_cast<double> (registerValue), 0.0, 2047.0) : 0.0;
    double hz = 0.0;
    if (mdl == Model::B)
        hz = std::max (kBMinHz, ((kBCubic[0] * v + kBCubic[1]) * v + kBCubic[2]) * v + kBCubic[3]) * kBFcRes[static_cast<std::size_t> (res)];
    else
        hz = modelAHz (modelACutoffConverter (v) + (static_cast<double> (variation) - 0.5) * 2.0 * kVariationShift) * kAFcRes[static_cast<std::size_t> (res)];
    const double gg = fastTan (kPi / rate * std::min (hz, 0.45 * rate));
    const double x = guard (in);
    const double kg2 = k + gg;
    const double hp = (x - kg2 * s1 - s2) / (1.0 + gg * kg2);
    const double bp = gg * hp + s1;
    const double lp = gg * bp + s2;
    s1 = bp + gg * hp;
    s2 = lp + gg * bp;
    settle();
    double out = cLp * lp + cBp * bp + cHp * hp;
    if (mdl == Model::A)
        out += shape (lp);
    return static_cast<float> (out);
}

void Filter::process (float* inOut, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        inOut[i] = process (inOut[i]);
}
float Filter::processSignalDependent (float in) noexcept
{
    if (mdl != Model::A)
        return process (in);
    return static_cast<float> (stepA (guard (in), ctl1, ctl2, cap1, cap2, rho));
}

float Filter::processSignalDependentWithCutoff (float in, float registerValue) noexcept
{
    if (mdl != Model::A)
        return processWithCutoff (in, registerValue);
    const double v = std::isfinite (registerValue) ? std::clamp (static_cast<double> (registerValue), 0.0, 2047.0) : 0.0;
    const double c2 = dynamicControl (modelAConverterValue (v, static_cast<double> (variation)));
    const double c1 = c2 * kAICtl1Scale + kAICtl1Offset;
    const auto caps = dynamicRateCaps (c1, c2, rate, rateMax);
    return static_cast<float> (stepA (guard (in), c1, c2, caps.r1, caps.r2, dynamicLoopGain (res, c2)));
}

void Filter::processSignalDependent (float* inOut, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        inOut[i] = processSignalDependent (inOut[i]);
}
} // namespace vs::bside::chip
