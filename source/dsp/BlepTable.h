#pragma once

#include <array>
#include <cmath>
#include <vector>

namespace vs::dsp
{
/**
    Windowed-sinc band-limited step (BLEP) and ramp (BLAMP) residual tables.

    The residual is (band-limited edge - naive edge). It is non-zero over
    kZeroCrossings samples on either side of the discontinuity, so oscillators
    using it delay their output by kZeroCrossings samples.

    Layout: one cell per sample offset j in [-Z, Z-1], each with kResolution + 1
    points covering t in (j, j + 1]. Cells use one-sided limits at t = 0, where the
    step residual jumps from +0.5 to -0.5.

    Build once off the audio thread (BlepTable::get() in prepare()).
*/
class BlepTable
{
public:
    static constexpr int kZeroCrossings = 8;  // taps on each side
    static constexpr int kResolution = 512;   // table points per sample
    static constexpr double kCutoff = 0.45;   // of the (oversampled) rate
    static constexpr double kKaiserBeta = 8.0;

    static const BlepTable& get()
    {
        static const BlepTable table;
        return table;
    }

    /** Step residual at t = j + d (j in [-Z, Z-1], d in (0, 1]) for a unit step. */
    float step (int j, float d) const noexcept { return lookup (stepCells, j, d); }

    /** Ramp residual at t = j + d for a unit slope change (per sample). */
    float ramp (int j, float d) const noexcept { return lookup (rampCells, j, d); }

    /**
        STUDY (cross-mod aliasing, experimental): curvature residual at t = j + d for a unit jump
        of the second derivative (per sample^2), the third integral of the same kernel.
    */
    float curve (int j, float d) const noexcept { return lookup (curveCells, j, d); }

private:
    static constexpr int kCells = 2 * kZeroCrossings;
    static constexpr int kCellSize = kResolution + 1;

    BlepTable()
    {
        stepCells.resize (static_cast<size_t> (kCells * kCellSize));
        rampCells.resize (static_cast<size_t> (kCells * kCellSize));
        curveCells.resize (static_cast<size_t> (kCells * kCellSize));

        // Integrate the windowed sinc on a fine grid: BLEP(t) = int h, BLEP2(t) = int BLEP.
        constexpr int fine = 8;
        constexpr int perSample = kResolution * fine;
        constexpr int total = 2 * kZeroCrossings * perSample;
        const double dt = 1.0 / perSample;

        std::vector<double> blep (static_cast<size_t> (total + 1)), blep2 (static_cast<size_t> (total + 1)), blep3 (static_cast<size_t> (total + 1));
        double acc = 0.0, acc2 = 0.0, acc3 = 0.0;
        double prevH = kernel (-kZeroCrossings), prevB = 0.0, prevB2 = 0.0;
        blep[0] = blep2[0] = blep3[0] = 0.0;
        for (int i = 1; i <= total; ++i)
        {
            const double t = -kZeroCrossings + i * dt;
            const double h = kernel (t);
            acc += 0.5 * (h + prevH) * dt;
            blep[static_cast<size_t> (i)] = acc;
            acc2 += 0.5 * (acc + prevB) * dt;
            blep2[static_cast<size_t> (i)] = acc2;
            acc3 += 0.5 * (acc2 + prevB2) * dt;
            blep3[static_cast<size_t> (i)] = acc3;
            prevH = h;
            prevB = acc;
            prevB2 = acc2;
        }
        const double norm = acc; // make BLEP(Z) exactly 1
        for (size_t i = 0; i <= static_cast<size_t> (total); ++i)
        {
            blep[i] /= norm;
            blep2[i] /= norm;
            blep3[i] /= norm;
        }

        for (int j = -kZeroCrossings; j < kZeroCrossings; ++j)
            for (int k = 0; k <= kResolution; ++k)
            {
                const double t = j + static_cast<double> (k) / kResolution;
                const auto idx = static_cast<size_t> ((t + kZeroCrossings) * perSample + 0.5);
                // One-sided limit at t = 0: cell j = -1 ends "just before" the edge.
                const bool afterEdge = j >= 0;
                const auto cell = static_cast<size_t> ((j + kZeroCrossings) * kCellSize + k);
                stepCells[cell] = static_cast<float> (blep[idx] - (afterEdge ? 1.0 : 0.0));
                rampCells[cell] = static_cast<float> (blep2[idx] - (t > 0.0 ? t : 0.0));
                curveCells[cell] = static_cast<float> (blep3[idx] - (t > 0.0 ? 0.5 * t * t : 0.0));
            }
    }

    static double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    static double kernel (double t)
    {
        constexpr double pi = 3.14159265358979323846;
        const double x = 2.0 * kCutoff * t;
        const double sinc = std::abs (x) < 1.0e-12 ? 1.0 : std::sin (pi * x) / (pi * x);
        const double r = t / kZeroCrossings;
        const double w = std::abs (r) >= 1.0 ? 0.0 : besselI0 (kKaiserBeta * std::sqrt (1.0 - r * r)) / besselI0 (kKaiserBeta);
        return 2.0 * kCutoff * sinc * w;
    }

    static float lookup (const std::vector<float>& cells, int j, float d) noexcept
    {
        const float pos = d * static_cast<float> (kResolution);
        int k = static_cast<int> (pos);
        if (k >= kResolution)
            k = kResolution - 1;
        const float frac = pos - static_cast<float> (k);
        const float* c = cells.data() + (j + kZeroCrossings) * kCellSize + k;
        return c[0] + (c[1] - c[0]) * frac;
    }

    std::vector<float> stepCells, rampCells, curveCells;
};
} // namespace vs::dsp
