// Linear-phase half-band filters (HalfBand.h; docs/architecture.md 8.19.4, S3b-2). Allocates nothing.

#include "bside/chip/HalfBand.h"

#include <algorithm>
#include <cmath>

namespace vs::bside::chip
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 60; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-17 * sum)
            break;
    }
    return sum;
}

/** The Kaiser half-band taps for P pairs and beta into d (normalised: DC gain 1). */
void kaiserTaps (HalfBandDesign& d, int pairs, double beta) noexcept
{
    d.pairs = pairs;
    d.h.fill (0.0);
    const double half = 2.0 * pairs - 1.0; // the window's half length: the outermost taps' distance
    const double i0b = besselI0 (beta);
    double sum = 0.0;
    for (int j = 0; j < pairs; ++j)
    {
        const double n = 2.0 * j + 1.0;
        const double r = n / (half + 1.0); // the window reaches 0 one sample beyond the outermost taps
        const double w = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
        const double sinc = ((j % 2) == 0 ? 1.0 : -1.0) / (kPi * n); // sin (pi n / 2) / (pi n)
        d.h[static_cast<std::size_t> (j)] = sinc * w;
        sum += sinc * w;
    }
    // the odd taps on both sides sum to 1/2 (with the centre's 1/2: DC gain 1)
    for (int j = 0; j < pairs; ++j)
        d.h[static_cast<std::size_t> (j)] *= 0.25 / sum;
}
} // namespace

double HalfBandDesign::response (double hz) const noexcept
{
    const double w = 2.0 * kPi * hz / rate;
    double y = 0.5;
    for (int j = 0; j < pairs; ++j)
        y += 2.0 * h[static_cast<std::size_t> (j)] * std::cos (w * (2.0 * j + 1.0));
    return y;
}

double HalfBandDesign::passDeviation() const noexcept { return passDeviationOn (400); }

double HalfBandDesign::passDeviationOn (int kGrid) const noexcept
{
    double worst = 0.0;
    for (int i = 0; i <= kGrid; ++i)
        worst = std::max (worst, std::abs (response (passHz * i / kGrid) - 1.0));
    return worst;
}

HalfBandDesign designHalfBand (double rate, double passHz, double deviation) noexcept
{
    // The fewest pairs that meet the deviation: for each count from below Kaiser's estimate, the Kaiser window's
    // parameter is searched (2 .. 12 in 0.05 steps, on a coarse grid of the pass band); the first count whose best parameter meets
    // the deviation on the fine grid is taken (bounded: at most kMaxPairs counts x 201 parameters).
    HalfBandDesign d;
    d.rate = rate > 1000.0 && std::isfinite (rate) ? rate : 96000.0;
    d.passHz = std::clamp (passHz, 1.0, 0.24 * d.rate);
    d.deviation = std::clamp (deviation, 1e-7, 0.1);
    const double atten = -20.0 * std::log10 (d.deviation);
    const double dw = 2.0 * kPi * (0.5 * d.rate - 2.0 * d.passHz) / d.rate;
    const double order = (atten - 7.95) / (2.285 * dw);
    const int start = std::clamp (static_cast<int> (std::ceil ((order + 2.0) / 4.0)) - 3, 1, HalfBandDesign::kMaxPairs);
    for (int pairs = start; pairs <= HalfBandDesign::kMaxPairs; ++pairs)
    {
        double bestBeta = -1.0, bestDev = 1e9;
        for (int i = 0; i <= 200; ++i)
        {
            const double beta = 2.0 + 0.05 * i;
            kaiserTaps (d, pairs, beta);
            const double dev = d.passDeviationOn (96);
            if (dev < bestDev)
            {
                bestDev = dev;
                bestBeta = beta;
            }
        }
        kaiserTaps (d, pairs, bestBeta);
        if (d.passDeviation() <= d.deviation || pairs == HalfBandDesign::kMaxPairs)
            break;
    }
    return d;
}

void HalfBandFir::prepare (const HalfBandDesign& d) noexcept
{
    pairs = std::clamp (d.pairs, 1, HalfBandDesign::kMaxPairs);
    h = d.h;
    reset();
}

void HalfBandFir::reset() noexcept
{
    buf.fill (0.0);
    pos = 0;
    held = 0.0;
    same = 0;
}

void HalfBandFir::prime (double x) noexcept
{
    buf.fill (x);
    held = x;
    same = 4 * pairs - 2;
}

void HalfBandUp::prepare (const HalfBandDesign& d) noexcept
{
    pairs = std::clamp (d.pairs, 1, HalfBandDesign::kMaxPairs);
    for (std::size_t j = 0; j < h2.size(); ++j)
        h2[j] = 2.0 * d.h[j];
    reset();
}

void HalfBandUp::reset() noexcept
{
    buf.fill (0.0);
    pos = 0;
}

void HalfBandUp::prime (double x) noexcept { buf.fill (x); }

void HalfBandDown::prepare (const HalfBandDesign& d) noexcept
{
    pairs = std::clamp (d.pairs, 1, HalfBandDesign::kMaxPairs);
    h = d.h;
    reset();
}

void HalfBandDown::reset() noexcept
{
    fb.fill (0.0);
    sb.fill (0.0);
    pos = 0;
    spos = 0;
}

void HalfBandDown::prime (double x) noexcept
{
    fb.fill (x);
    sb.fill (x);
}
} // namespace vs::bside::chip
