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

    Build once off the audio thread (get() in prepare()).

    One table per (zero crossings, Kaiser beta in tenths): `BlepTable` is the instance Vintage uses (8 zero
    crossings, beta 8.0, unchanged); an instrument that needs more stop-band attenuation instantiates a
    longer one (B-Side's combined waveforms, docs/decisions.md S2b-2) and reports its longer delay.
*/
template <int ZeroCrossings, int KaiserBetaTenths>
class BasicBlepTable
{
public:
    static constexpr int kZeroCrossings = ZeroCrossings;  // taps on each side
    static constexpr int kResolution = 512;   // table points per sample
    static constexpr double kCutoff = 0.45;   // of the (oversampled) rate
    static constexpr double kKaiserBeta = KaiserBetaTenths / 10.0;

    static const BasicBlepTable& get()
    {
        static const BasicBlepTable table;
        return table;
    }

    /** Step residual at t = j + d (j in [-Z, Z-1], d in (0, 1]) for a unit step. */
    float step (int j, float d) const noexcept { return lookup (stepCells, j, d); }

    /** Ramp residual at t = j + d for a unit slope change (per sample). */
    float ramp (int j, float d) const noexcept { return lookup (rampCells, j, d); }

    /**
        The band-limiting kernel itself (the windowed sinc whose integrals are the step and ramp
        residuals, with the same normalisation), at any t in [-Z, Z]; zero outside. Linear
        interpolation in a table of kKernelResolution points per sample (error below 1e-7).
        Used to band-limit corrections that are not polynomial (the sine's hard-sync reset).
    */
    float kernelAt (float t) const noexcept
    {
        const float pos = std::abs (t) * static_cast<float> (kKernelResolution);
        const int k = static_cast<int> (pos);
        if (k >= kZeroCrossings * kKernelResolution)
            return 0.0f;
        const float frac = pos - static_cast<float> (k);
        const float* c = kernelCells.data() + k;
        return c[0] + (c[1] - c[0]) * frac;
    }
    static constexpr int kKernelResolution = 4096;

private:
    static constexpr int kCells = 2 * kZeroCrossings;
    static constexpr int kCellSize = kResolution + 1;

    BasicBlepTable()
    {
        stepCells.resize (static_cast<size_t> (kCells * kCellSize));
        rampCells.resize (static_cast<size_t> (kCells * kCellSize));

        // Integrate the windowed sinc on a fine grid: BLEP(t) = int h, BLEP2(t) = int BLEP.
        constexpr int fine = 8;
        constexpr int perSample = kResolution * fine;
        constexpr int total = 2 * kZeroCrossings * perSample;
        const double dt = 1.0 / perSample;

        std::vector<double> blep (static_cast<size_t> (total + 1)), blep2 (static_cast<size_t> (total + 1));
        double acc = 0.0, acc2 = 0.0;
        double prevH = kernel (-kZeroCrossings), prevB = 0.0;
        blep[0] = blep2[0] = 0.0;
        for (int i = 1; i <= total; ++i)
        {
            const double t = -kZeroCrossings + i * dt;
            const double h = kernel (t);
            acc += 0.5 * (h + prevH) * dt;
            blep[static_cast<size_t> (i)] = acc;
            acc2 += 0.5 * (acc + prevB) * dt;
            blep2[static_cast<size_t> (i)] = acc2;
            prevH = h;
            prevB = acc;
        }
        const double norm = acc; // make BLEP(Z) exactly 1
        for (size_t i = 0; i <= static_cast<size_t> (total); ++i)
        {
            blep[i] /= norm;
            blep2[i] /= norm;
        }

        // The kernel (even), normalised like the residuals: kernelCells[k] = h (k / kKernelResolution) / norm.
        kernelCells.resize (static_cast<size_t> (kZeroCrossings * kKernelResolution + 1));
        for (size_t k = 0; k < kernelCells.size(); ++k)
            kernelCells[k] = static_cast<float> (kernel (static_cast<double> (k) / kKernelResolution) / norm);

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

    std::vector<float> stepCells, rampCells, kernelCells;
};

/** Vintage's table (and B-Side's short kernel): 8 zero crossings, Kaiser beta 8.0. */
using BlepTable = BasicBlepTable<8, 80>;
} // namespace vs::dsp
