// The chip's filter (Filter.h; docs/architecture.md 8.19.1, 8.19.3; every constant: docs/decisions.md "B-Side S3a").
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
constexpr double kLn2 = 0.69314718055994530942;

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
// processWithCutoff's table: a knot every kBCutStep register units, 0 .. 2048
constexpr double kBCutStep = 64.0;

// ---- Model A ----
// The cutoff mapping (register value -> corner at resonance 0) has two parts (docs/architecture.md 8.19.3, owner
// decision O9; decisions "B-Side Model A cutoff steps, scope 512"):
// - codes 0 .. 479: the S3a form, bit for bit: an 11-bit terminated ladder of ratio 2.1756 (the converter value d)
//   and ln fc piecewise linear over 24 points of d. Nothing here depends on the refitted converter below.
// - codes 480 .. 2047: the refitted form: d from a weight per bit (fitted; close to the ladder) and ln fc piecewise
//   linear over 129 points of d, whose slope at each fall-back of the converter sets the corner's step there (within
//   2 percentage points of the reference's per code from 479 -> 480 up to 1600). Codes 480 .. 511 take fitted values
//   of d instead of the sum of their bits (kAJoin): they join the S3a corner at 479 with the reference's step there
//   (-2.2 %, the committed noise walk) and rise evenly (+1.0 % per code at the -3 dB point) to code 511, which lies on
//   the refitted curve, so the fall at 511 -> 512 is the curve's (-15.0 % at the -3 dB point; the committed walk:
//   -15.0 %; decisions S7).
// - a register value between 479 and 480 (Smooth) interpolates ln fc between the two.
constexpr int kAFirstRefitted = 480;
// codes 0 .. 479 (S3a)
constexpr double kALowLadderRatio = 2.1756;
constexpr std::array<double, 24> kALowLogFc { 5.566978, 5.601697, 5.794737, 5.830848, 5.952658, 7.166796, 7.832795, 8.205156,
                                              8.463033, 8.749728, 8.914376, 9.091689, 9.229615, 9.347138, 9.468028, 9.542380,
                                              9.620053, 9.686958, 9.733824, 9.790228, 9.835870, 9.887333, 9.914261, 9.951000 };
// codes 480 .. 2047: the weight per bit (bit 0 first), the converter values of 480 .. 511, ln (fc / Hz) at
// d = i / kALogFcIntervals
constexpr std::array<double, 11> kAConverter { 0.000677536, 0.001300386, 0.002520950, 0.004900252, 0.009531922, 0.017761255, 0.033858608, 0.066655995, 0.128577338, 0.249785711, 0.484430046 };
constexpr std::array<double, 32> kAJoin { 0.240074656, 0.240536530, 0.240935837, 0.241294777, 0.241666672, 0.242034619, 0.242619999, 0.243409451,
    0.244224166, 0.245050991, 0.245866650, 0.246581038, 0.247379271, 0.248182767, 0.248991804, 0.249790746,
    0.250766321, 0.251787836, 0.252868325, 0.253934267, 0.254970039, 0.256039093, 0.257083874, 0.258118306,
    0.259042431, 0.260039855, 0.261015547, 0.261965596, 0.262869040, 0.263844735, 0.264844312, 0.265784232 };
constexpr int kALogFcIntervals = 128;
constexpr std::array<double, 129> kALogFc { 5.568762, 5.574051, 5.579520, 5.585102, 5.590843, 5.597733, 5.613662, 5.649298,
    5.682199, 5.716583, 5.749633, 5.784669, 5.799238, 5.805900, 5.812686, 5.818856,
    5.825178, 5.834360, 5.857475, 5.879546, 5.900385, 5.922892, 5.941360, 6.091184,
    6.310404, 6.519868, 6.735299, 6.941595, 7.168982, 7.329803, 7.469615, 7.632934,
    7.707134, 7.763385, 7.825549, 7.895722, 7.971609, 8.049846, 8.133840, 8.205641,
    8.261674, 8.308356, 8.349665, 8.391052, 8.431115, 8.474731, 8.529889, 8.591215,
    8.647933, 8.694001, 8.733191, 8.769271, 8.802955, 8.836176, 8.869572, 8.900264,
    8.930406, 8.958206, 8.986429, 9.018792, 9.051212, 9.083282, 9.114686, 9.142244,
    9.167545, 9.191416, 9.213670, 9.234926, 9.255931, 9.277678, 9.300422, 9.323396,
    9.346121, 9.368434, 9.389309, 9.408517, 9.426244, 9.443287, 9.460551, 9.478162,
    9.494880, 9.510347, 9.524757, 9.538788, 9.553100, 9.567833, 9.582439, 9.596270,
    9.609154, 9.621597, 9.633743, 9.645440, 9.656640, 9.666661, 9.675656, 9.685001,
    9.696134, 9.704555, 9.712976, 9.721397, 9.729819, 9.739656, 9.749791, 9.759926,
    9.770061, 9.780196, 9.789827, 9.798028, 9.806229, 9.814430, 9.822632, 9.830833,
    9.839762, 9.849009, 9.858256, 9.867504, 9.876751, 9.885423, 9.890262, 9.895100,
    9.899939, 9.904778, 9.909616, 9.915451, 9.922053, 9.928654, 9.935256, 9.941858,
    9.948459 };
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
// processWithCutoff's table: knots at kACutHz0 x 2^(j / kACutPerOctave) of the corner at resonance 0 (on the Q law's
// own knots); the curve spans 261 Hz .. 21 kHz, knots j = 0 .. 26
constexpr double kACutHz0 = 250.0, kACutLog2Hz0 = 7.965784284662087, kACutPerOctave = 4.0; // log2 (250)
constexpr int kACutKnots = 27;

const render::LadderWeights& lowWeights() noexcept
{
    static const render::LadderWeights w = render::ladderWeights (11, kALowLadderRatio, true);
    return w;
}

/** Codes 0 .. 479 (and 480 as the upper neighbour of 479): the S3a converter, summed bit by bit as it always was. */
double lowConverterAt (int v) noexcept
{
    const auto& w = lowWeights();
    double d = 0.0;
    for (std::size_t i = 0; i < 11; ++i)
        if (((v >> i) & 1) != 0)
            d += w[i];
    return d;
}

double lowConverter (double v) noexcept
{
    const int lo = static_cast<int> (std::floor (v));
    const int hi = std::min (2047, lo + 1);
    const double f = v - lo;
    return lowConverterAt (lo) + (lowConverterAt (hi) - lowConverterAt (lo)) * f;
}

double lowLnHz (double d) noexcept
{
    const double x = std::clamp (d, 0.0, 1.0) * 23.0;
    const auto i = static_cast<std::size_t> (std::min (22.0, std::floor (x)));
    const double f = x - static_cast<double> (i);
    return kALowLogFc[i] + (kALowLogFc[i + 1] - kALowLogFc[i]) * f;
}

/** Codes 480 .. 2047: the converter value of every code, built at compile time (a lookup on the audio thread). */
constexpr std::array<double, 2048> kAConverterTable = [] {
    std::array<double, 2048> t {};
    for (std::size_t v = 0; v < 2048; ++v)
        for (std::size_t i = 0; i < 11; ++i)
            if (((v >> i) & 1) != 0)
                t[v] += kAConverter[i];
    for (std::size_t j = 0; j < kAJoin.size(); ++j)
        t[static_cast<std::size_t> (kAFirstRefitted) + j] = kAJoin[j];
    return t;
}();

double converterAt (int v) noexcept { return kAConverterTable[static_cast<std::size_t> (v)]; }

double refittedConverter (double v) noexcept
{
    const int lo = static_cast<int> (std::floor (v));
    const int hi = std::min (2047, lo + 1);
    const double f = v - lo;
    return converterAt (lo) + (converterAt (hi) - converterAt (lo)) * f;
}

double refittedLnHz (double d) noexcept
{
    const double x = std::clamp (d, 0.0, 1.0) * static_cast<double> (kALogFcIntervals);
    const auto i = static_cast<std::size_t> (std::min (static_cast<double> (kALogFcIntervals - 1), std::floor (x)));
    const double f = x - static_cast<double> (i);
    return kALogFc[i] + (kALogFc[i + 1] - kALogFc[i]) * f;
}

/** ln (fc / Hz) of Model A's curve at a (clamped) register value and variation: the two parts above. */
double modelALnBase (double v, double var) noexcept
{
    // each part's expression written exactly as the S3a form wrote it (one expression: the same rounding)
    if (v <= kAFirstRefitted - 1.0)
        return lowLnHz (lowConverter (v) + (var - 0.5) * 2.0 * Filter::kVariationShift);
    if (v >= kAFirstRefitted)
        return refittedLnHz (refittedConverter (v) + (var - 0.5) * 2.0 * Filter::kVariationShift);
    const double a = lowLnHz (lowConverterAt (kAFirstRefitted - 1) + (var - 0.5) * 2.0 * Filter::kVariationShift);
    const double b = refittedLnHz (converterAt (kAFirstRefitted) + (var - 0.5) * 2.0 * Filter::kVariationShift);
    return a + (b - a) * (v - (kAFirstRefitted - 1.0));
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

/** Model B's curve before the floor (the cubic), for any register value. */
double modelBCubic (double v) noexcept { return ((kBCubic[0] * v + kBCubic[1]) * v + kBCubic[2]) * v + kBCubic[3]; }

/** The corner the discrete filter is designed for: never at or above 0.45 x the rate. */
double designCorner (double hz, double sampleRate) noexcept { return std::min (hz, 0.45 * sampleRate); }

/** The roots of b0 z^2 + b1 z + b2 when they are real (false otherwise, or when b0 is 0). */
bool realRoots (double b0, double b1, double b2, double& za, double& zb) noexcept
{
    const double disc = b1 * b1 - 4.0 * b0 * b2;
    if (disc < 0.0 || ! (std::abs (b0) > 0.0))
        return false;
    const double q = -0.5 * (b1 + (b1 < 0.0 ? -1.0 : 1.0) * std::sqrt (disc)); // no cancellation
    za = q / b0;
    zb = std::abs (q) > 0.0 ? b2 / q : 0.0;
    return true;
}

/** PHASE (Filter.h "RATE"): whether the numerator b0 + b1 z^-1 + b2 z^-2, as factored with |b0| >= |b2|, is to be
    replaced by its reflection (b0 and b2 swapped) to put its zeros on the side of the unit circle where the matched
    images of the prototype numerator's zeros lie (cL + cB s + cH s^2, s in units of the corner; w = 2 pi fc / rate).
    Zeros in the right half-plane: none -> keep (both zeros inside or on the circle); two (a pair, or two real) -> swap
    (both outside); one (real, the other real in the left half-plane, at the origin or at infinity) -> of the two
    factors keep the one whose zeros lie nearer the images exp (s w) of the prototype's (infinity: -1), so that only
    the zero of the right-half-plane one is outside. */
bool reflectZeros (double b0, double b1, double b2, double w, double cL, double cB, double cH) noexcept
{
    int rightHalf = 0;
    double sOut = 0.0, tOther = 0.0, logOther = 0.0; // the right-half-plane zero; the matched image of the other one
    if (std::abs (cH) > 0.0)
    {
        if (cL / cH < 0.0) // two real zeros of opposite signs
        {
            const double disc = std::sqrt (cB * cB - 4.0 * cH * cL);
            const double ra = (-cB + disc) / (2.0 * cH), rb = (-cB - disc) / (2.0 * cH);
            rightHalf = 1;
            sOut = std::max (ra, rb);
            logOther = std::max (std::min (ra, rb) * w, -700.0);
            tOther = std::exp (logOther);
        }
        else if (cB / cH < 0.0) // the zeros' sum -cB / cH is positive
        {
            rightHalf = std::abs (cL) > 0.0 ? 2 : 1;
            sOut = -cB / cH;
            tOther = 1.0; // cL = 0: a zero at the origin
        }
    }
    else if (std::abs (cB) > 0.0 && -cL / cB > 0.0)
    {
        rightHalf = 1;
        sOut = -cL / cB;
        tOther = -1.0; // the other zero is at infinity: half the rate
    }
    if (rightHalf != 1)
        return rightHalf == 2;
    double za = 0.0, zb = 0.0, ya = 0.0, yb = 0.0;
    if (! realRoots (b0, b1, b2, za, zb) || ! realRoots (b2, b1, b0, ya, yb))
        return false;
    const double logOut = std::min (sOut * w, 700.0), tOut = std::exp (logOut);
    auto cost = [=] (double x, double y) {
        const double lx = std::log (std::abs (x) + 1e-300), ly = std::log (std::abs (y) + 1e-300);
        auto d = [] (double z, double lz, double t, double lt) { return std::abs (lz - lt) + ((z < 0.0) != (t < 0.0) ? 100.0 : 0.0); };
        return std::min (d (x, lx, tOut, logOut) + d (y, ly, tOther, logOther), d (y, ly, tOut, logOut) + d (x, lx, tOther, logOther));
    };
    return cost (ya, yb) < cost (za, zb);
}
} // namespace

Filter::Filter() noexcept
{
    (void) lowWeights(); // the S3a converter weights are built here, never on the audio thread
    prepare (Model::B, 96000.0, 0.5f);
}

double Filter::modelACutoffConverter (double registerValue) noexcept
{
    // the converter of the part in force: S3a's up to 479, the refitted one from 480 (between 479 and 480 the two
    // values interpolated; it is not one converter there)
    const double v = std::isfinite (registerValue) ? std::clamp (registerValue, 0.0, 2047.0) : 0.0;
    if (v <= kAFirstRefitted - 1.0)
        return lowConverter (v);
    if (v >= kAFirstRefitted)
        return refittedConverter (v);
    const double a = lowConverterAt (kAFirstRefitted - 1), b = converterAt (kAFirstRefitted);
    return a + (b - a) * (v - (kAFirstRefitted - 1.0));
}

double Filter::cutoffCurveHz (Model model, double registerValue, double variationValue) noexcept
{
    const double v = std::isfinite (registerValue) ? std::clamp (registerValue, 0.0, 2047.0) : 0.0;
    if (model == Model::B)
        return std::max (kBMinHz, modelBCubic (v));
    const double var = std::isfinite (variationValue) ? std::clamp (variationValue, 0.0, 1.0) : 0.5;
    return std::exp (modelALnBase (v, var));
}

void Filter::prepare (Model model, double internalRate, float variationValue) noexcept
{
    (void) lowWeights();
    mdl = model;
    rate = internalRate > 1000.0 && std::isfinite (internalRate) ? internalRate : 96000.0;
    variation = std::isfinite (variationValue) ? std::clamp (variationValue, 0.0f, 1.0f) : 0.5f;
    node2 = std::min (20000.0, 0.4 * rate);
    const double s = std::sin (kPi * node2 / rate);
    node2Phi = s * s;
    cutValid = false; // processWithCutoff's table depends on the model, rate, resonance and mode only
    update();
}

void Filter::reset() noexcept
{
    s1 = 0.0;
    s2 = 0.0;
}

void Filter::primeSteady (double in) noexcept
{
    // s = A s + b x at the fixed point: (I - A) s = b x, solved by Cramer's rule (2 x 2)
    const double x = guard (in);
    const double m11 = 1.0 - a11, m12 = -a12, m21 = -a21, m22 = 1.0 - a22;
    const double det = m11 * m22 - m12 * m21;
    if (! (std::abs (det) > 1e-300) || ! std::isfinite (det))
    {
        reset();
        return;
    }
    const double b1 = a1x * x, b2 = a2x * x;
    s1 = (b1 * m22 - m12 * b2) / det;
    s2 = (m11 * b2 - m21 * b1) / det;
    if (! std::isfinite (s1) || ! std::isfinite (s2))
        reset();
    settle();
}

void Filter::setCutoff (float registerValue) noexcept
{
    const float v = std::isfinite (registerValue) ? std::clamp (registerValue, 0.0f, 2047.0f) : 0.0f;
    if (v < fcReg || v > fcReg) // a change of the stored value (no float ==: GCC -Wfloat-equal)
    {
        fcReg = v;
        update();
    }
}

void Filter::setResonance (int value) noexcept
{
    const int r = std::clamp (value, 0, 15);
    if (r != res)
    {
        res = r;
        cutValid = false;
        update();
    }
}

void Filter::setMode (std::uint8_t modeVolume) noexcept
{
    const auto m = static_cast<std::uint8_t> (modeVolume & kModeMask);
    if (m != modeBits)
    {
        modeBits = m;
        cutValid = false;
        update();
    }
}

void Filter::setVariation (float value) noexcept
{
    const float v = std::isfinite (value) ? std::clamp (value, 0.0f, 1.0f) : 0.5f;
    if (v < variation || v > variation)
    {
        variation = v;
        update();
    }
}

std::array<double, 3> Filter::modelAWeights (std::uint8_t mode, int resonance, double dampingAtZero) noexcept
{
    const bool lo = (mode & kLowPass) != 0, band = (mode & kBandPass) != 0, high = (mode & kHighPass) != 0;
    const double lowGain = kALowGain[static_cast<std::size_t> (resonance)];
    const double bandRes = 1.0 + kABandRes * (lowGain - 1.0);
    return { (lo ? kAL1 * lowGain : 0.0) + (band ? kAB1 : 0.0) + (high ? kAH1 : 0.0),
             (band ? kABand * dampingAtZero * bandRes : 0.0) + (high ? kAHighBand * dampingAtZero * bandRes : 0.0),
             high ? kAHigh : 0.0 };
}

Filter::Mapped Filter::mapMatched (double fc, double damping, double cL, double cB, double cH, bool notch, double sampleRate,
                                   double n2, double phi2) noexcept
{
    // POLES (Filter.h "RATE"): s = wc (-k/2 +- sqrt (k^2/4 - 1)) -> exp (s T); P = 1 + a1 + a2, M = 1 - a1 + a2 and
    // 1 - a2 without cancellation (expm1, 1 - cos as 2 sin^2), so a corner of 15 Hz at 384 kHz stays exact
    const double theta = 2.0 * kPi * fc / sampleRate;
    const double sigma = 0.5 * damping * theta;
    const double oneMinusR = -std::expm1 (-sigma);
    const double r = 1.0 - oneMinusR; // exp (-sigma)
    const double disc = 1.0 - 0.25 * damping * damping;
    double a1 = 0.0, pSum = 0.0;
    if (disc >= 0.0)
    {
        const double psi = theta * std::sqrt (disc);
        const double sh = std::sin (0.5 * psi);
        const double oneMinusCos = 2.0 * sh * sh;
        a1 = -2.0 * r * (1.0 - oneMinusCos);
        pSum = oneMinusR * oneMinusR + 2.0 * r * oneMinusCos;
    }
    else // real poles (Q < 0.5): exp (-(sigma -+ psi))
    {
        const double psi = theta * std::sqrt (-disc);
        pSum = std::expm1 (-(sigma + psi)) * std::expm1 (-(sigma - psi));
        a1 = -2.0 * r * std::cosh (psi);
    }
    const double a2 = r * r;
    const double mSum = 1.0 - a1 + a2;
    const double oneMinusA2 = oneMinusR * (1.0 + r); // 1 - exp (-2 sigma)
    Mapped m {};
    m.g = std::sqrt (pSum / mSum);
    m.k = 2.0 * oneMinusA2 / (mSum * m.g);

    // NUMERATOR b0 + b1 z^-1 + b2 z^-2 (over the denominator with a0 = 1), then the output weights of the trapezoidal
    // form realising it: wL = sum b / P, wB = 2 (b0 - b2) / (M g), wH = (b0 - b1 + b2) / M
    double b0 = 0.0, b1 = 0.0, b2 = 0.0;
    if (notch)
    {
        // zeros on the unit circle at the prototype's notch (cL - cH x^2 = 0), DC gain exact
        const double wn = std::min (2.0 * kPi * fc * std::sqrt (cL / cH) / sampleRate, kPi);
        const double sn = std::sin (0.5 * wn);
        const double kk = cL * pSum / (4.0 * sn * sn); // 2 - 2 cos wn = 4 sin^2 (wn / 2)
        b0 = kk;
        b1 = -2.0 * (1.0 - 2.0 * sn * sn) * kk;
        b2 = kk;
    }
    else
    {
        // |B|^2 = B0 phi0 + B1 phi1 + B2 phi2 (phi1 = sin^2 (w/2), phi0 = 1 - phi1, phi2 = 4 phi0 phi1); the same form
        // with A0 = P^2, A1 = M^2, A2 = -4 a2 is the denominator's. B0 from the DC gain, B1 and B2 from the magnitude
        // at the nodes n1 = min (fc, n2 / 2) and n2.
        const double aa0 = pSum * pSum, aa1 = mSum * mSum, aa2 = -4.0 * a2;
        const double bb0 = aa0 * cL * cL;
        auto protoSq = [&] (double hz) {
            const double x = hz / fc, x2 = x * x;
            const double re = cL - cH * x2, den = (1.0 - x2) * (1.0 - x2) + damping * damping * x2;
            return (re * re + cB * cB * x2) / den;
        };
        const double n1 = std::min (fc, 0.5 * n2);
        const double sn1 = std::sin (kPi * n1 / sampleRate);
        const double phiA[2] { sn1 * sn1, phi2 };
        const double hz[2] { n1, n2 };
        double m1[2] {}, m2[2] {}, rhs[2] {};
        for (int i = 0; i < 2; ++i)
        {
            const double f1 = phiA[i], f0 = 1.0 - f1, f2 = 4.0 * f0 * f1;
            m1[i] = f1;
            m2[i] = f2;
            rhs[i] = protoSq (hz[i]) * (aa0 * f0 + aa1 * f1 + aa2 * f2) - bb0 * f0;
        }
        const double det = m1[0] * m2[1] - m2[0] * m1[1]; // 4 phi1 phi1' (phi0' - phi0) != 0: n1 < n2
        const double bb1 = (rhs[0] * m2[1] - m2[0] * rhs[1]) / det;
        const double bb2 = (m1[0] * rhs[1] - m1[1] * rhs[0]) / det;
        // factor: b0 + b1 + b2 = r0, b0 - b1 + b2 = r1 (signs: the prototype's at DC and at half the rate),
        // b0 b2 = -B2 / 4. Of the two factors (b0, b2 swapped: every zero reflected through the unit circle, the same
        // magnitude) the one with |b0| >= |b2| first; then the zeros are put on the prototype's side (zerosSide).
        // A discriminant below 0 (a double zero rounded) is taken as 0.
        const double xn = 0.5 * sampleRate / fc, xn2 = xn * xn;
        const double reNyq = (cL - cH * xn2) * (1.0 - xn2) + cB * damping * xn2;
        const double r0 = (cL < 0.0 ? -1.0 : 1.0) * std::sqrt (std::max (bb0, 0.0));
        const double r1 = (reNyq < 0.0 ? -1.0 : 1.0) * std::sqrt (std::max (bb1, 0.0));
        b1 = 0.5 * (r0 - r1);
        const double sum02 = 0.5 * (r0 + r1);
        const double sq = std::sqrt (std::max (sum02 * sum02 + bb2, 0.0));
        const double t1 = 0.5 * (sum02 + sq), t2 = 0.5 * (sum02 - sq);
        b0 = std::abs (t1) >= std::abs (t2) ? t1 : t2;
        b2 = std::abs (t1) >= std::abs (t2) ? t2 : t1;
        if (reflectZeros (b0, b1, b2, 2.0 * kPi * fc / sampleRate, cL, cB, cH))
            std::swap (b0, b2);
    }
    m.wL = (b0 + b1 + b2) / pSum;
    m.wB = 2.0 * (b0 - b2) / (mSum * m.g);
    m.wH = (b0 - b1 + b2) / mSum;
    return m;
}

Filter::Linear Filter::stateSpace (double g, double damping, double wL, double wB, double wH) noexcept
{
    // hp = hx x + h1 s1 + h2 s2, bp = g hp + s1, lp = g bp + s2 (Filter.h, process); s1' = 2 bp - s1, s2' = 2 lp - s2
    const double h = 1.0 / (1.0 + g * (damping + g));
    const double hx = h, h1 = -h * (damping + g), h2 = -h;
    const double bx = g * hx, b1 = g * h1 + 1.0, b2 = g * h2;
    const double lxx = g * bx, l11 = g * b1, l22 = g * b2 + 1.0;
    Linear l {};
    l.a1x = 2.0 * bx;
    l.a11 = 2.0 * b1 - 1.0;
    l.a12 = 2.0 * b2;
    l.a2x = 2.0 * lxx;
    l.a21 = 2.0 * l11;
    l.a22 = 2.0 * l22 - 1.0;
    l.ox = wL * lxx + wB * bx + wH * hx;
    l.o1 = wL * l11 + wB * b1 + wH * h1;
    l.o2 = wL * l22 + wB * b2 + wH * h2;
    return l;
}

void Filter::update() noexcept
{
    const auto r = static_cast<std::size_t> (res);
    const double base = cutoffCurveHz (mdl, static_cast<double> (fcReg), static_cast<double> (variation));
    if (mdl == Model::B)
    {
        fcHz = base * kBFcRes[r];
        k0 = kBInvQ[0];
        k = kBInvQ[r];
        protoL = (modeBits & kLowPass) != 0 ? kBLow : 0.0;
        protoB = (modeBits & kBandPass) != 0 ? kBBand : 0.0;
        protoH = (modeBits & kHighPass) != 0 ? kBHigh : 0.0;
        p1 = p2 = p3 = p4 = p5 = 0.0;
        lpMin = -1.0e9;
        lpMax = 1.0e9;
    }
    else
    {
        fcHz = base * kAFcRes[r];
        k0 = 1.0 / modelAQ0 (base);
        k = k0 / kAQRes[r];
        const bool lo = (modeBits & kLowPass) != 0, band = (modeBits & kBandPass) != 0, high = (modeBits & kHighPass) != 0;
        const double lowGain = kALowGain[r];
        const auto w = modelAWeights (modeBits, res, k0);
        protoL = w[0];
        protoB = w[1];
        protoH = w[2];
        p1 = w[0];
        p2 = (lo ? kAL2 * lowGain : 0.0) + (band ? kAB2 : 0.0) + (high ? kAH2 : 0.0);
        p3 = (lo ? kAL3 * lowGain : 0.0) + (band ? kAB3 : 0.0);
        p4 = lo ? kAL4 * lowGain : 0.0;
        p5 = lo ? kAL5 * lowGain : 0.0;
        lpMin = kALpMin;
        lpMax = kALpMax;
    }
    const bool notch = mdl == Model::B && modeBits == (kLowPass | kHighPass);
    const Mapped m = mapMatched (designCorner (fcHz, rate), k, protoL, protoB, protoH, notch, rate, node2, node2Phi);
    // Model A: the static functions carry p1 x lp already; the linear row carries the rest of the designed numerator
    const Linear l = stateSpace (m.g, m.k, m.wL - p1, m.wB, m.wH);
    const Linear lpRow = stateSpace (m.g, m.k, 1.0, 0.0, 0.0);
    a1x = l.a1x;
    a11 = l.a11;
    a12 = l.a12;
    a2x = l.a2x;
    a21 = l.a21;
    a22 = l.a22;
    ox = l.ox;
    o1 = l.o1;
    o2 = l.o2;
    lx = lpRow.ox;
    l1 = lpRow.o1;
    l2 = lpRow.o2;
}

void Filter::buildCutTable() noexcept
{
    // processWithCutoff's knots (Filter.h): the mapping at each knot's corner, g' against the rational tan the
    // per-sample code uses, the output weights (Model A: minus the static functions' linear term, which is constant
    // over the cutoff at a resonance and mode)
    const auto r = static_cast<std::size_t> (res);
    const int knots = mdl == Model::B ? kCutKnots : kACutKnots;
    const bool notch = mdl == Model::B && modeBits == (kLowPass | kHighPass);
    for (int j = 0; j < knots; ++j)
    {
        double fc = 0.0, damping = k;
        std::array<double, 3> w { protoL, protoB, protoH };
        if (mdl == Model::B)
            fc = std::max (kBMinHz, modelBCubic (kBCutStep * j)) * kBFcRes[r];
        else
        {
            const double base = kACutHz0 * std::exp2 (j / kACutPerOctave);
            const double dampingAtZero = 1.0 / modelAQ0 (base);
            fc = base * kAFcRes[r];
            damping = dampingAtZero / kAQRes[r];
            w = modelAWeights (modeBits, res, dampingAtZero);
        }
        const double fcc = designCorner (fc, rate);
        const Mapped m = mapMatched (fcc, damping, w[0], w[1], w[2], notch, rate, node2, node2Phi);
        auto& t = cutTable[static_cast<std::size_t> (j)];
        t[0] = m.g / fastTan (kPi * fcc / rate);
        t[1] = m.k;
        t[2] = m.wL - p1;
        t[3] = m.wB;
        t[4] = m.wH;
    }
    cutValid = true;
}

double Filter::cutRegister (float registerValue) noexcept
{
    return std::isfinite (registerValue) ? std::clamp (static_cast<double> (registerValue), 0.0, 2047.0) : 0.0;
}

double Filter::cutBaseLn (double v) const noexcept { return modelALnBase (v, static_cast<double> (variation)); }

double Filter::cornerWithCutoffHz (float registerValue) const noexcept
{
    const double v = cutRegister (registerValue);
    return mdl == Model::B ? std::max (kBMinHz, modelBCubic (v)) : std::exp (cutBaseLn (v));
}

Filter::Mapped Filter::cutAt (float registerValue) const noexcept
{
    const double v = cutRegister (registerValue);
    double fc = 0.0, pos = 0.0;
    int last = kCutKnots - 1;
    if (mdl == Model::B)
    {
        fc = std::max (kBMinHz, modelBCubic (v)) * kBFcRes[static_cast<std::size_t> (res)];
        pos = v * (1.0 / kBCutStep);
    }
    else
    {
        const double lnBase = cutBaseLn (v);
        fc = std::exp (lnBase) * kAFcRes[static_cast<std::size_t> (res)];
        last = kACutKnots - 1;
        pos = std::clamp ((lnBase * (1.0 / kLn2) - kACutLog2Hz0) * kACutPerOctave, 0.0, static_cast<double> (last));
    }
    const int i = std::min (static_cast<int> (pos), last - 1);
    const double f = pos - i;
    const auto& lo = cutTable[static_cast<std::size_t> (i)];
    const auto& hi = cutTable[static_cast<std::size_t> (i + 1)];
    Mapped m {};
    m.g = (lo[0] + (hi[0] - lo[0]) * f) * fastTan (kPi / rate * designCorner (fc, rate));
    m.k = lo[1] + (hi[1] - lo[1]) * f;
    m.wL = lo[2] + (hi[2] - lo[2]) * f;
    m.wB = lo[3] + (hi[3] - lo[3]) * f;
    m.wH = lo[4] + (hi[4] - lo[4]) * f;
    return m;
}

double Filter::processWithCutoffDouble (double in, float registerValue) noexcept
{
    double lp = 0.0;
    const double out = processWithCutoffLinearDouble (in, registerValue, lp);
    return mdl == Model::A ? out + shape (lp) : out;
}

double Filter::processWithCutoffLinearDouble (double in, float registerValue, double& lowPass) noexcept
{
    if (! cutValid)
        buildCutTable(); // after a rate, resonance or mode change: bounded (33 mappings)
    const Mapped c = cutAt (registerValue);
    const double x = guard (in);
    const double kg2 = c.k + c.g;
    const double hp = (x - kg2 * s1 - s2) / (1.0 + c.g * kg2);
    const double bp = c.g * hp + s1;
    const double lp = c.g * bp + s2;
    s1 = bp + c.g * hp;
    s2 = lp + c.g * bp;
    settle();
    lowPass = lp;
    return c.wL * lp + c.wB * bp + c.wH * hp;
}

double Filter::peekWithCutoffLinearDouble (double in, float registerValue, const Filter& stateOf, double& lowPass) noexcept
{
    if (! cutValid)
        buildCutTable();
    const Mapped c = cutAt (registerValue);
    const double x = guard (in);
    const double kg2 = c.k + c.g;
    const double hp = (x - kg2 * stateOf.s1 - stateOf.s2) / (1.0 + c.g * kg2);
    const double bp = c.g * hp + stateOf.s1;
    const double lp = c.g * bp + stateOf.s2;
    lowPass = lp;
    return c.wL * lp + c.wB * bp + c.wH * hp;
}

Filter::Linear Filter::linearPart() const noexcept
{
    Linear l { a11, a12, a21, a22, a1x, a2x, ox, o1, o2 };
    // Model A: its static functions' linear term on the low-pass row
    l.ox += p1 * lx;
    l.o1 += p1 * l1;
    l.o2 += p1 * l2;
    return l;
}

Filter::Linear Filter::linearPartWithCutoff (float registerValue) noexcept
{
    if (! cutValid)
        buildCutTable();
    const Mapped c = cutAt (registerValue);
    return stateSpace (c.g, c.k, c.wL + p1, c.wB, c.wH);
}

std::complex<double> Filter::response (const Linear& l, double hz, double sampleRate) noexcept
{
    return responseAt (l, std::polar (1.0, 2.0 * kPi * hz / sampleRate));
}

std::complex<double> Filter::responseAt (const Linear& l, std::complex<double> z) noexcept
{
    // H (z) = ox + [o1 o2] (z I - A)^-1 [a1x a2x]
    const std::complex<double> d = (z - l.a11) * (z - l.a22) - l.a12 * l.a21;
    const std::complex<double> v1 = ((z - l.a22) * l.a1x + l.a12 * l.a2x) / d;
    const std::complex<double> v2 = (l.a21 * l.a1x + (z - l.a11) * l.a2x) / d;
    return l.ox + l.o1 * v1 + l.o2 * v2;
}

double Filter::spectralRadius (const Linear& l) noexcept
{
    const double tr = l.a11 + l.a22, det = l.a11 * l.a22 - l.a12 * l.a21;
    const double disc = tr * tr - 4.0 * det;
    if (disc >= 0.0)
        return std::max (std::abs (0.5 * (tr + std::sqrt (disc))), std::abs (0.5 * (tr - std::sqrt (disc))));
    return std::sqrt (std::max (det, 0.0));
}

void Filter::process (float* inOut, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        inOut[i] = process (inOut[i]);
}
} // namespace vs::bside::chip
