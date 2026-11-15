#include "bside/chip/RenderTables.h"

#include "bside/chip/CombinedWaveforms.h"
#include "bside/chip/Waveforms.h"

#include <algorithm>
#include <cmath>

namespace vs::bside::chip::render
{
namespace
{
Converter makeConverter (const std::vector<double>& w, bool ideal)
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
    return c;
}

double convert (const std::vector<double>& w, unsigned v)
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
}

RenderTables build (Model model, ShapeSet set)
{
    RenderTables t;
    const bool ideal = model == Model::B;
    const std::vector<double> w12 = ideal ? ladderWeights (12, 2.0, true) : ladderWeights (12, kModelARatio, false);
    const std::vector<double> w8 = ideal ? ladderWeights (8, 2.0, true) : ladderWeights (8, kModelARatio, false);
    t.dac = makeConverter (w12, ideal);
    for (unsigned v = 0; v < 4096; ++v)
        t.wave[v] = static_cast<float> ((ideal ? static_cast<double> (v) / 4095.0 : convert (w12, v)) - t.dac.center);
    for (unsigned v = 0; v < 256; ++v)
        t.envelope[v] = static_cast<float> (ideal ? static_cast<double> (v) / 255.0 : convert (w8, v));

    const combined::ShapeTables& shapes = combined::tablesFor (model, set);
    constexpr std::uint8_t combos[kTableCombinations] = { 3, 5, 6, 7 };
    for (std::size_t c = 0; c < kTableCombinations; ++c)
        buildSteps (t, shapes, combos[c], t.steps[c]);
    return t;
}
} // namespace

std::vector<double> ladderWeights (int bits, double ratio, bool terminated)
{
    const auto n = static_cast<std::size_t> (std::clamp (bits, 1, 16));
    std::vector<double> w (n, 0.0);
    const double leg = 1.0 / ratio; // conductance of a bit's leg (series resistors have conductance 1)
    for (std::size_t b = 0; b < n; ++b)
    {
        // Tridiagonal nodal equations G v = I (Thomas algorithm); bit b at 1, every other bit at 0.
        std::vector<double> diag (n), rhs (n, 0.0), cp (n), dp (n);
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
        std::vector<double> v (n);
        v[n - 1] = dp[n - 1];
        for (std::size_t i = n - 1; i-- > 0;)
            v[i] = dp[i] - cp[i] * v[i + 1];
        w[b] = v[n - 1];
    }
    double sum = 0.0;
    for (const double x : w)
        sum += x;
    for (double& x : w)
        x /= sum;
    return w;
}

const RenderTables& tablesFor (Model m, ShapeSet s)
{
    static const RenderTables fittedA = build (Model::A, ShapeSet::fitted);
    static const RenderTables fittedB = build (Model::B, ShapeSet::fitted);
    if (s == ShapeSet::placeholder)
    {
        static const RenderTables placeholderA = build (Model::A, ShapeSet::placeholder);
        static const RenderTables placeholderB = build (Model::B, ShapeSet::placeholder);
        return m == Model::A ? placeholderA : placeholderB;
    }
    return m == Model::A ? fittedA : fittedB;
}
} // namespace vs::bside::chip::render
