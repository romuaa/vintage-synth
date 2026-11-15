#include "bside/chip/CombinedWaveforms.h"

#include "bside/chip/Waveforms.h"

#include <cmath>

namespace vs::bside::chip::combined
{
const ModelParams& fittedParams (Model m) noexcept { return m == Model::A ? kFittedA : kFittedB; }

Evaluation evaluate (const NetworkParams& p, std::uint8_t tspBits, std::uint16_t index) noexcept
{
    // Node equations G V = I of the network (nodes 0..11 the bit lines, node 12 the pulse node).
    constexpr int kLines = 12;
    double g[13][13] {};
    double rhs[13] {};
    const bool pulse = (tspBits & wave::kPulse) != 0;
    const int n = pulse ? 13 : 12;
    const auto triBits = wave::triangle (index);
    const auto sawBits = wave::saw (index);
    const double triUp = std::exp (p.triUp), triDown = std::exp (p.triDown);
    const double sawUp = std::exp (p.sawUp), sawDown = std::exp (p.sawDown);
    const double pulseLine = std::exp (p.pulseLine), pulseSource = std::exp (p.pulseSource);
    for (int i = 0; i < kLines; ++i)
    {
        const double centred = i - 5.5;
        const double drive = std::exp (p.driveSlope * centred);
        auto driver = [&] (bool one, double up, double down) {
            const double c = (one ? up : down) * drive;
            g[i][i] += c;
            if (one)
                rhs[i] += c;
        };
        if ((tspBits & wave::kTri) != 0)
            driver (((triBits >> i) & 1u) != 0, triUp, triDown);
        if ((tspBits & wave::kSaw) != 0)
            driver (((sawBits >> i) & 1u) != 0, sawUp, sawDown);
        if (i + 1 < kLines)
        {
            const double c = std::exp (p.couplingSlope * (i - 5));
            g[i][i] += c;
            g[i + 1][i + 1] += c;
            g[i][i + 1] -= c;
            g[i + 1][i] -= c;
        }
        if (pulse)
        {
            const double c = pulseLine * std::exp (p.pulseSlope * centred);
            g[i][i] += c;
            g[12][12] += c;
            g[i][12] -= c;
            g[12][i] -= c;
        }
    }
    if (pulse)
    {
        g[12][12] += pulseSource;
        rhs[12] += pulseSource; // the pulse is high (the mask of step 3 handles the low part)
    }
    // Symmetric, diagonally dominant: Gaussian elimination without pivoting is exact enough and stable.
    for (int k = 0; k < n; ++k)
        for (int r = k + 1; r < n; ++r)
        {
            if (std::fpclassify (g[r][k]) == FP_ZERO)
                continue;
            const double m = g[r][k] / g[k][k];
            for (int c = k; c < n; ++c)
                g[r][c] -= m * g[k][c];
            rhs[r] -= m * rhs[k];
        }
    double v[13] {};
    for (int k = n - 1; k >= 0; --k)
    {
        double s = rhs[k];
        for (int c = k + 1; c < n; ++c)
            s -= g[k][c] * v[c];
        v[k] = s / g[k][k];
    }
    Evaluation e;
    e.margin = 1.0e300;
    for (int i = 0; i < kLines; ++i)
    {
        const double d = v[i] - (p.threshold + p.thresholdSlope * (i - 5.5));
        if (d >= 0.0)
            e.value = static_cast<std::uint16_t> (e.value | (1u << i));
        e.margin = std::fmin (e.margin, std::fabs (d));
    }
    return e;
}

namespace
{
std::uint16_t singleShape (std::uint8_t waveBits, std::uint16_t ix) noexcept
{
    switch (waveBits)
    {
        case wave::kTri:
        case wave::kTri | wave::kNoise: return wave::triangle (ix);
        case wave::kSaw:
        case wave::kSaw | wave::kNoise: return wave::saw (ix);
        case wave::kPulse:
        case wave::kNoise: return 0xfff;
        default: return 0;
    }
}
} // namespace

void buildTables (const ModelParams& p, ShapeTables& t) noexcept
{
    for (unsigned w = 0; w < 16; ++w)
    {
        const auto bits = static_cast<std::uint8_t> (w);
        auto& row = t.wave[w];
        for (unsigned ix = 0; ix < 4096; ++ix)
        {
            const auto i = static_cast<std::uint16_t> (ix);
            if (bits == 0)
                row[ix] = 0;
            else if (! wave::usesTable (bits))
                row[ix] = singleShape (bits, i);
            else if ((bits & wave::kNoise) != 0)
                row[ix] = static_cast<std::uint16_t> (p.noiseShape[w] & 0xfffu);
            else
                row[ix] = evaluate (p.network[w], bits, i).value;
        }
    }
}

void buildPlaceholderTables (ShapeTables& t) noexcept
{
    for (unsigned w = 0; w < 16; ++w)
    {
        const auto bits = static_cast<std::uint8_t> (w);
        for (unsigned ix = 0; ix < 4096; ++ix)
        {
            const auto i = static_cast<std::uint16_t> (ix);
            std::uint16_t v = 0;
            if (bits == 0)
                v = 0;
            else if (! wave::usesTable (bits))
                v = singleShape (bits, i);
            else
            {
                v = 0xfff;
                if ((bits & wave::kTri) != 0)
                    v &= wave::triangle (i);
                if ((bits & wave::kSaw) != 0)
                    v &= wave::saw (i);
            }
            t.wave[w][ix] = v;
        }
    }
}

namespace
{
struct Built
{
    ShapeTables a, b;
    Built() noexcept
    {
        buildTables (kFittedA, a);
        buildTables (kFittedB, b);
    }
};
struct BuiltPlaceholder
{
    ShapeTables t;
    BuiltPlaceholder() noexcept { buildPlaceholderTables (t); }
};
} // namespace

const ShapeTables& fittedTables (Model m) noexcept
{
    static const Built built; // 256 KiB of static storage, no heap
    return m == Model::A ? built.a : built.b;
}

const ShapeTables& placeholderTables() noexcept
{
    static const BuiltPlaceholder built;
    return built.t;
}

const ShapeTables& tablesFor (Model m, ShapeSet s) noexcept { return s == ShapeSet::placeholder ? placeholderTables() : fittedTables (m); }
} // namespace vs::bside::chip::combined
