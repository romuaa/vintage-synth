#include "bside/trace/AnalogMeasure.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace vs::bside::trace
{
namespace
{
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kBins = kSpecBins;
/** A corner needs an octave of plateau below it: bin 12 = 40 Hz. */
constexpr double kMinCornerBin = 12.0;
/** Plateau/corner iteration: at most this many rounds, then 'not converged'. */
constexpr int kMaxRounds = 8;

double medianOf (std::vector<double> v)
{
    if (v.empty())
        return kNaN;
    std::sort (v.begin(), v.end());
    const auto n = v.size();
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double plateauBelow (const Response& r, int lastBin)
{
    std::vector<double> v;
    for (int k = 0; k <= std::min (std::max (5, lastBin), kBins - 1); ++k)
        if (! std::isnan (r[static_cast<std::size_t> (k)]))
            v.push_back (r[static_cast<std::size_t> (k)]);
    return medianOf (v);
}

/** First bin k (>= 1) where s[k] and s[k + 1] are below `level`; -1 if none. */
int firstBelow (const Response& s, double level)
{
    for (int k = 1; k < kBins; ++k)
    {
        const double a = s[static_cast<std::size_t> (k)];
        const double b = k + 1 < kBins ? s[static_cast<std::size_t> (k + 1)] : a;
        if (! std::isnan (a) && ! std::isnan (b) && a < level && b < level)
            return k;
    }
    return -1;
}

/** Least-squares parabola through r over bins [k - 4, k + 4]; the bin position nearest k where it
    crosses `level` (falling). NaN when there is none within the window. */
double curveCrossing (const Response& r, int k, double level)
{
    // Normal equations in x = j - k for y = a + b x + c x^2.
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, t0 = 0, t1 = 0, t2 = 0;
    for (int j = std::max (0, k - 4); j <= std::min (kBins - 1, k + 4); ++j)
    {
        const double y = r[static_cast<std::size_t> (j)];
        if (std::isnan (y))
            continue;
        const double x = j - k;
        s0 += 1;
        s1 += x;
        s2 += x * x;
        s3 += x * x * x;
        s4 += x * x * x * x;
        t0 += y;
        t1 += x * y;
        t2 += x * x * y;
    }
    if (s0 < 5)
        return kNaN;
    // Solve the 3x3 system by Cramer's rule.
    const double m[3][3] = { { s0, s1, s2 }, { s1, s2, s3 }, { s2, s3, s4 } };
    const auto det = [] (const double a[3][3]) {
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
               + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    };
    const double d = det (m);
    if (std::abs (d) < 1e-12)
        return kNaN;
    double coef[3];
    const double t[3] = { t0, t1, t2 };
    for (int c = 0; c < 3; ++c)
    {
        double mc[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                mc[i][j] = j == c ? t[i] : m[i][j];
        coef[c] = det (mc) / d;
    }
    // a + b x + c x^2 = level, the root nearest 0 in [-4, 4] where the curve falls.
    const double a = coef[0] - level, b = coef[1], c = coef[2];
    double best = kNaN;
    const auto consider = [&] (double x) {
        if (x >= -4.0 && x <= 4.0 && b + 2.0 * c * x < 0.0 && (std::isnan (best) || std::abs (x) < std::abs (best)))
            best = x;
    };
    if (std::abs (c) < 1e-12)
    {
        if (std::abs (b) > 1e-12)
            consider (-a / b);
    }
    else
    {
        const double disc = b * b - 4.0 * c * a;
        if (disc >= 0.0)
        {
            consider ((-b + std::sqrt (disc)) / (2.0 * c));
            consider ((-b - std::sqrt (disc)) / (2.0 * c));
        }
    }
    return std::isnan (best) ? kNaN : k + best;
}
} // namespace

double binHz (double k)
{
    return 20.0 * std::pow (2.0, k / 12.0);
}

Response filterResponse (const SpecSegment& filtered, const SpecSegment& bypass)
{
    Response r {};
    for (int k = 0; k < kBins; ++k)
    {
        const auto i = static_cast<std::size_t> (k);
        r[i] = bypass.spectrum[i] <= -1000 ? kNaN : (filtered.spectrum[i] - bypass.spectrum[i]) / 10.0;
    }
    return r;
}

Response smoothResponse (const Response& r)
{
    Response s {};
    for (int k = 0; k < kBins; ++k)
    {
        double sum = 0.0;
        int n = 0;
        for (int j = std::max (0, k - 2); j <= std::min (kBins - 1, k + 2); ++j)
            if (! std::isnan (r[static_cast<std::size_t> (j)]))
            {
                sum += r[static_cast<std::size_t> (j)];
                ++n;
            }
        s[static_cast<std::size_t> (k)] = n > 0 ? sum / n : kNaN;
    }
    return s;
}

Corner lowpassCorner (const Response& r)
{
    Corner c;
    const auto s = smoothResponse (r);
    int kc = kBins - 1;
    for (int round = 1; round <= kMaxRounds; ++round)
    {
        c.rounds = round;
        const double plateau = plateauBelow (r, kc - 12);
        if (std::isnan (plateau))
            return Corner {};
        const int k = firstBelow (s, plateau - 3.0);
        if (k < 0)
            return Corner {};
        const double x = curveCrossing (r, k, plateau - 3.0);
        if (std::isnan (x) || x < kMinCornerBin)
            return Corner {};
        c.hz = binHz (std::min (x, static_cast<double> (kBins - 1)));
        c.plateauDb = plateau;
        if (k == kc)
        {
            c.found = true;
            return c;
        }
        kc = k;
    }
    c.found = false;
    c.converged = false;
    return c;
}

Peak resonancePeak (const Response& r)
{
    const auto c = lowpassCorner (r);
    if (! c.found)
        return Peak {};
    const auto s = smoothResponse (r);
    const int last = std::min (kBins - 1, static_cast<int> (std::ceil (12.0 * std::log2 (c.hz / 20.0))) + 6);
    const int first = std::max (0, static_cast<int> (std::floor (12.0 * std::log2 (c.hz / 20.0))) - 12);
    Peak p;
    double best = -std::numeric_limits<double>::infinity();
    for (int k = first; k <= last; ++k)
        if (! std::isnan (s[static_cast<std::size_t> (k)]) && s[static_cast<std::size_t> (k)] > best)
        {
            best = s[static_cast<std::size_t> (k)];
            p.hz = binHz (k);
        }
    p.found = std::isfinite (best);
    p.plateauDb = c.plateauDb;
    p.db = best - c.plateauDb;
    return p;
}

double toneLevelDb (const SpecSegment& s, double hz)
{
    if (! (hz > 0.0))
        return -120.0;
    const long k = std::lround (12.0 * std::log2 (hz / 20.0));
    if (k < 0 || k >= kBins)
        return -120.0;
    return s.spectrum[static_cast<std::size_t> (k)] / 10.0;
}
} // namespace vs::bside::trace
