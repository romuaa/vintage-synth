#include "bside/chip/RenderTables.h"

#include "bside/chip/CombinedWaveforms.h"
#include "bside/chip/Waveforms.h"
#include "common/dsp/BlepTable.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace vs::bside::chip::render
{
namespace
{
Converter makeConverter (const LadderWeights& w, bool ideal)
{
    Converter c;
    c.ideal = ideal;
    c.g = 1.0 / 4095.0;
    for (std::size_t i = 0; i < 12; ++i)
        c.e[i] = ideal ? 0.0 : w[i] - c.g * static_cast<double> (1u << i);
    for (int imin = 0; imin <= 12; ++imin)
    {
        double s = 0.0, t = 0.0;
        for (int i = 0; i < imin; ++i)
        {
            s += 0.5 * c.e[static_cast<std::size_t> (i)];
            if (i + 1 < 12)
                t += 0.5 * c.e[static_cast<std::size_t> (i + 1)];
        }
        c.excludedSaw[static_cast<std::size_t> (imin)] = s;
        c.excludedTri[static_cast<std::size_t> (imin)] = t;
    }
    c.center = 0.5; // d (0) = 0 and d (4095) = 1 by normalisation
    for (unsigned v = 0; v < 4096; ++v)
    {
        double s = 0.0;
        for (std::size_t i = 0; i < 12; ++i)
            if (((v >> i) & 1u) != 0)
                s += c.e[i];
        c.devSaw[v] = s;
    }
    for (unsigned v = 0; v < 2048; ++v)
    {
        double s = 0.0;
        for (std::size_t i = 0; i < 11; ++i)
            if (((v >> i) & 1u) != 0)
                s += c.e[i + 1];
        c.devTri[v] = s;
    }
    for (int imin = 0; imin <= 12; ++imin)
    {
        double s = 0.0;
        for (int i = imin; i < 11; ++i)
            s += c.e[static_cast<std::size_t> (i + 1)];
        c.triTail[static_cast<std::size_t> (imin)] = s;
    }
    return c;
}

using Cx = std::complex<double>;

/** The spectrum of a staircase of n equal cells over one period at harmonic m, per unit DFT of its values:
    the integral of e^(-2 pi i m u) over the first cell (1 / n at m = 0). */
Cx cellResponse (long m, std::size_t n)
{
    if (m == 0)
        return Cx (1.0 / static_cast<double> (n), 0.0);
    constexpr double pi = 3.14159265358979323846;
    const double x = 2.0 * pi * static_cast<double> (m);
    return (Cx (1.0, 0.0) - std::polar (1.0, -x / static_cast<double> (n))) / Cx (0.0, x);
}

double convert (const LadderWeights& w, unsigned v)
{
    double d = 0.0;
    for (std::size_t i = 0; i < w.size(); ++i)
        if (((v >> i) & 1u) != 0)
            d += w[i];
    return d;
}

void buildSteps (const RenderTables& t, const combined::ShapeTables& shapes, std::uint8_t bits, StepTable& st)
{
    for (unsigned ix = 0; ix < 4096; ++ix)
    {
        st.shape[ix] = wave::shape (shapes, bits, static_cast<std::uint16_t> (ix), 0);
        st.value[ix] = t.wave[st.shape[ix]];
    }
    std::uint16_t clear = 4096;
    for (unsigned c = 4096; c-- > 2048;)
    {
        if ((st.shape[c] & 0x800u) == 0)
            clear = static_cast<std::uint16_t> (c);
        st.firstClear[c] = clear;
    }
    for (unsigned c = 0; c < 2048; ++c)
        st.firstClear[c] = st.firstClear[2048];
    st.firstSetUpper = 4096;
    for (unsigned c = 2048; c < 4096; ++c)
        if ((st.shape[c] & 0x800u) != 0)
        {
            st.firstSetUpper = static_cast<std::uint16_t> (c);
            break;
        }
    for (unsigned c = 0; c < 4096; ++c)
    {
        std::uint16_t d = 0;
        for (unsigned j = 1; j < 4096; ++j)
        {
            const float a = st.value[(c + j - 1) & 0xfffu];
            const float b = st.value[(c + j) & 0xfffu];
            if (a < b || b < a)
            {
                d = static_cast<std::uint16_t> (j);
                break;
            }
        }
        st.nextChange[c] = d;
    }
    // The coarse levels: a staircase of `cells` cells whose harmonics below cells / 2 equal the fine staircase's
    // exactly (its values from the fine spectrum divided by the coarse cell's response, transformed back; the
    // harmonic cells / 2 left out). Above the register's range a level is chosen with at least half a coarse
    // cell per sample, so harmonic cells / 2 lies at a quarter of the internal rate or above (22 kHz at 88.2
    // kHz): the audio band is the fine staircase's (cell means instead would fold the shape's high harmonics
    // back into it, 1 dB and more at 8 cells per period). Only the levels from kFirstCoarseLevel on are used
    // (128 cells or fewer: the fine spectrum up to harmonic 64), computed directly, without allocation.
    constexpr double pi = 3.14159265358979323846;
    constexpr std::size_t harmonics = (4096u >> kFirstCoarseLevel) / 2;
    std::array<Cx, harmonics + 1> fine {};
    for (std::size_t m = 0; m <= harmonics; ++m)
    {
        Cx sum (0.0, 0.0), rot = std::polar (1.0, -2.0 * pi * static_cast<double> (m) / 4096.0), ph (1.0, 0.0);
        for (std::size_t c = 0; c < 4096; ++c)
        {
            sum += static_cast<double> (st.value[c]) * ph;
            ph *= rot;
        }
        fine[m] = sum * cellResponse (static_cast<long> (m), 4096); // the fine staircase's coefficient at harmonic m
    }
    for (int level = kFirstCoarseLevel; level <= kCoarseLevels; ++level)
    {
        const unsigned cells = 4096u >> level;
        float* value = st.coarseValue.data() + coarseOffset (level);
        std::uint16_t* next = st.coarseNext.data() + coarseOffset (level);
        for (unsigned ci = 0; ci < cells; ++ci)
        {
            // v_i = (1 / cells) (V0 + 2 Re sum over m of V_m e^(2 pi i m i / cells)), V_m = F_m / response_m
            double sum = (fine[0] / cellResponse (0, cells)).real();
            for (unsigned m = 1; m < cells / 2; ++m)
                sum += 2.0 * (fine[m] / cellResponse (static_cast<long> (m), cells) * std::polar (1.0, 2.0 * pi * m * ci / cells)).real();
            value[ci] = static_cast<float> (sum / static_cast<double> (cells));
        }
        for (unsigned ci = 0; ci < cells; ++ci)
        {
            std::uint16_t d = 0;
            for (unsigned j = 1; j < cells; ++j)
            {
                const float a = value[(ci + j - 1) % cells];
                const float b = value[(ci + j) % cells];
                if (a < b || b < a)
                {
                    d = static_cast<std::uint16_t> (j);
                    break;
                }
            }
            next[ci] = d;
        }
    }
}

void build (Model model, ShapeSet set, RenderTables& t)
{
    const bool ideal = model == Model::B;
    const LadderWeights w12 = ideal ? ladderWeights (12, 2.0, true) : ladderWeights (12, kModelARatio, false);
    const LadderWeights w8 = ideal ? ladderWeights (8, 2.0, true) : ladderWeights (8, kModelARatio, false);
    t.dac = makeConverter (w12, ideal);
    for (unsigned v = 0; v < 4096; ++v)
        t.wave[v] = static_cast<float> ((ideal ? static_cast<double> (v) / 4095.0 : convert (w12, v)) - t.dac.center);
    for (unsigned v = 0; v < 256; ++v)
        t.envelope[v] = static_cast<float> (ideal ? static_cast<double> (v) / 255.0 : convert (w8, v));

    const combined::ShapeTables& shapes = combined::tablesFor (model, set);
    constexpr std::uint8_t combos[kTableCombinations] = { 3, 5, 6, 7 };
    for (std::size_t c = 0; c < kTableCombinations; ++c)
        buildSteps (t, shapes, combos[c], t.steps[c]);
}

/** Built in place (about 320 KiB: never a by-value copy), once per (model, shape set). */
template <Model M, ShapeSet S>
const RenderTables& built()
{
    static const RenderTables& t = [] () -> const RenderTables& {
        static RenderTables storage;
        build (M, S, storage);
        return storage;
    }();
    return t;
}
} // namespace

LadderWeights ladderWeights (int bits, double ratio, bool terminated) noexcept
{
    LadderWeights w {};
    const auto n = static_cast<std::size_t> (std::clamp (bits, 1, 16));
    const double leg = 1.0 / ratio; // conductance of a bit's leg (series resistors have conductance 1)
    for (std::size_t b = 0; b < n; ++b)
    {
        // Tridiagonal nodal equations G v = I (Thomas algorithm); bit b at 1, every other bit at 0.
        std::array<double, 16> diag {}, rhs {}, cp {}, dp {}, v {};
        for (std::size_t i = 0; i < n; ++i)
            diag[i] = leg + (i > 0 ? 1.0 : 0.0) + (i + 1 < n ? 1.0 : 0.0) + (i == 0 && terminated ? leg : 0.0);
        rhs[b] = leg;
        cp[0] = -1.0 / diag[0];
        dp[0] = rhs[0] / diag[0];
        for (std::size_t i = 1; i < n; ++i)
        {
            const double m = diag[i] + cp[i - 1];
            cp[i] = -1.0 / m;
            dp[i] = (rhs[i] + dp[i - 1]) / m;
        }
        v[n - 1] = dp[n - 1];
        for (std::size_t i = n - 1; i-- > 0;)
            v[i] = dp[i] - cp[i] * v[i + 1];
        w[b] = v[n - 1];
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        sum += w[i];
    for (std::size_t i = 0; i < n; ++i)
        w[i] /= sum;
    return w;
}

const KernelTables& kernelTables()
{
    static const KernelTables& t = [] () -> const KernelTables& {
        static KernelTables storage;
        using Short = vs::dsp::BlepTable;
        using Long = vs::dsp::BasicBlepTable<kLongZeroCrossings, kLongBetaTenths, double>;
        static_assert (Short::kZeroCrossings == kLongZeroCrossings && Short::kResolution == kKernelResolution && Long::kResolution == kKernelResolution,
                       "both kernels share the taps and points of the renderer's layout");
        static_assert (Long::kKernelResolution % kKernelResolution == 0, "the long kernel's points lie on its kernel table");
        const Short& s = Short::get();
        const Long& l = Long::get();
        for (int k = 0; k <= kKernelResolution; ++k)
            for (int jj = 0; jj < kKernelTaps; ++jj)
            {
                const int j = jj - kLongZeroCrossings;
                const auto at = static_cast<std::size_t> (k * kKernelTaps + jj);
                // a whole point: step (j, d) interpolates nothing there (d = 1 reads the cell's last point)
                const double d = static_cast<double> (k) / kKernelResolution;
                const bool first = k == 0;
                storage.shortStep[at] = first ? s.step (j, 0.0f) : s.step (j, static_cast<float> (d));
                storage.shortRamp[at] = first ? s.ramp (j, 0.0f) : s.ramp (j, static_cast<float> (d));
                storage.longStep[at] = first ? l.step (j, 0.0) : l.step (j, d);
                storage.longRamp[at] = first ? l.ramp (j, 0.0) : l.ramp (j, d);
                storage.longKernel[at] = l.kernelAt (static_cast<double> (j) + d);
            }
        // the long kernel's step at kFineResolution points: cubic Hermite between its own points with the exact
        // derivatives (h = 1 / 512: step' = kernel)
        constexpr int refine = kFineResolution / kKernelResolution;
        static_assert (kFineResolution % kKernelResolution == 0, "the long kernel's own points lie on the refined ones");
        for (int k = 0; k < kKernelResolution; ++k)
            for (int r = 0; r <= refine; ++r)
            {
                if (r == refine && k + 1 < kKernelResolution)
                    continue; // the next cell's first point
                const double x = static_cast<double> (r) / refine, x2 = x * x, x3 = x2 * x;
                const double h00 = 2.0 * x3 - 3.0 * x2 + 1.0, h01 = 3.0 * x2 - 2.0 * x3;
                const double h10 = (x3 - 2.0 * x2 + x) / kKernelResolution, h11 = (x3 - x2) / kKernelResolution;
                for (int jj = 0; jj < kKernelTaps; ++jj)
                {
                    const auto p0 = static_cast<std::size_t> (k * kKernelTaps + jj), p1 = p0 + kKernelTaps;
                    const auto at = static_cast<std::size_t> ((k * refine + r) * kKernelTaps + jj);
                    storage.longStepFine[at] = h00 * storage.longStep[p0] + h01 * storage.longStep[p1] + h10 * storage.longKernel[p0] + h11 * storage.longKernel[p1];
                }
            }
        return storage;
    }();
    return t;
}

const RenderTables& tablesFor (Model m, ShapeSet s)
{
    if (s == ShapeSet::placeholder)
        return m == Model::A ? built<Model::A, ShapeSet::placeholder>() : built<Model::B, ShapeSet::placeholder>();
    return m == Model::A ? built<Model::A, ShapeSet::fitted>() : built<Model::B, ShapeSet::fitted>();
}
} // namespace vs::bside::chip::render
