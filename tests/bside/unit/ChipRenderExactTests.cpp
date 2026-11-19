// The alias follow-up step (docs/architecture.md 8.20): the two defects of 8.18.1 the bin-centred and windowed
// gates did not see, as gates. Narrow pulses measured exactly (a rectangular transform over whole periods at
// frequencies with a small-denominator ratio to the internal rate); synced targets at exact integer and
// small-rational ratios to their source, measured not bin-centred. A fast subset in the normal run, the full
// scans hidden ("[.render-exact]", "[.render-sync-integer]"). docs/decisions.md "B-Side alias follow-up step".

#include "RenderTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numeric>
#include <vector>

using namespace bs_render_test;
namespace ctrl = vs::bside::chip::ctrl;

namespace
{
constexpr double kPi = 3.14159265358979323846;

/** The exact DFT of x (any length n, the mean removed) at bins 0 .. maxBin, by Bluestein's algorithm on the
    power-of-two FFT: equal to the plain sum over the n samples (rectangular window) up to rounding. */
std::vector<double> exactMagnitudes (const std::vector<float>& x, int maxBin)
{
    const std::size_t n = x.size();
    std::size_t m = 1;
    while (m < 2 * n)
        m <<= 1;
    std::vector<std::complex<double>> w (n);
    for (std::size_t k = 0; k < n; ++k)
    {
        // exp (-i pi k^2 / n), k^2 reduced modulo 2n exactly (k < 2^31)
        const std::uint64_t kk = (static_cast<std::uint64_t> (k) * static_cast<std::uint64_t> (k)) % (2u * static_cast<std::uint64_t> (n));
        w[k] = std::polar (1.0, -kPi * static_cast<double> (kk) / static_cast<double> (n));
    }
    double mean = 0.0;
    for (const float v : x)
        mean += static_cast<double> (v);
    mean /= static_cast<double> (n);
    std::vector<std::complex<double>> a (m), b (m);
    for (std::size_t k = 0; k < n; ++k)
        a[k] = (static_cast<double> (x[k]) - mean) * w[k];
    b[0] = std::conj (w[0]);
    for (std::size_t k = 1; k < n; ++k)
        b[k] = b[m - k] = std::conj (w[k]);
    fft (a);
    fft (b);
    for (std::size_t i = 0; i < m; ++i)
        a[i] = std::conj (a[i] * b[i]);
    fft (a); // the inverse transform as the conjugate of the forward one
    std::vector<double> mag (static_cast<std::size_t> (maxBin) + 1);
    for (std::size_t k = 0; k <= static_cast<std::size_t> (maxBin); ++k)
        mag[k] = std::abs (std::conj (a[k]) * w[k]) / static_cast<double> (m);
    return mag;
}

struct ExactResult
{
    double worstDb = -300.0; // the worst non-harmonic bin in 1 .. band, dB re the fundamental
    double worstHz = 0.0;
};

/** A narrow pulse (or any width) on oscillator 1 at f = rate x p / q, rendered over a whole number of its
    periods (p periods in every q samples; about 24 000 samples) and measured with a rectangular DFT at every
    bin up to the band: the harmonics are exact bins, everything else is aliasing (8.20, "the exact
    measurement"). The warm-up lets the kernel's window and the output's rounding settle. */
ExactResult pulseExact (Model m, double rate, double band, int p, int q, float width)
{
    const double f = rate * p / q;
    Chip c = makeChip (m, rate);
    setOsc (c, 0, f * 16777216.0 / 985248.0, width, static_cast<std::uint8_t> (ctrl::pulse | ctrl::gate));
    renderSamples (c, 3000);
    const int periods = std::max (1, 24000 / q);
    const int n = periods * q;
    const auto y = renderSamples (c, n);
    const int maxBin = static_cast<int> (band * n / rate);
    const auto mag = exactMagnitudes (y, maxBin);
    const int fundamental = periods * p;
    ExactResult r;
    for (int b = 1; b <= maxBin; ++b)
        if (b % fundamental != 0)
        {
            const double db = 20.0 * std::log10 (mag[static_cast<std::size_t> (b)] / mag[static_cast<std::size_t> (fundamental)] + 1e-300);
            if (db > r.worstDb)
                r = { db, b * rate / n };
        }
    return r;
}

/** The budget of 8.4.5 for a pulse (Model B -100 dB; Model A's converter -96 dB to 2 kHz, -80 dB above). */
double pulseBudget (Model m, double hz) { return m == Model::B ? -100.0 : hz <= 2000.0 ? -96.0 : -80.0; }

struct ExactCase
{
    double rate, band;
    int p, q;
};

/** Every p / q in lowest terms with q <= maxQ whose frequency lies in [lo, hi] Hz. */
std::vector<ExactCase> smallDenominators (const RateConfig& rc, int maxQ, double lo, double hi)
{
    std::vector<ExactCase> v;
    for (int q = 2; q <= maxQ; ++q)
        for (int p = 1; p < q; ++p)
        {
            const double f = rc.internalRate * p / q;
            if (std::gcd (p, q) == 1 && f >= lo && f <= hi)
                v.push_back ({ rc.internalRate, rc.band, p, q });
        }
    return v;
}

struct ExactWorst
{
    double margin = -300.0; // worst - budget, dB
    double db = -300.0, hz = 0.0, f = 0.0, rate = 0.0;
    float width = 0.0f;
    Model model = Model::B;
    int rows = 0;
};

void account (ExactWorst& w, const ExactResult& r, Model m, const ExactCase& k, float width)
{
    const double f = k.rate * k.p / k.q;
    const double margin = r.worstDb - pulseBudget (m, f);
    ++w.rows;
    if (margin > w.margin)
        w = { margin, r.worstDb, r.worstHz, f, k.rate, width, m, w.rows };
}
} // namespace

TEST_CASE ("render: narrow pulses meet -100 dB by the exact measurement at small-denominator frequencies", "[render][alias][exact]")
{
    // The fast subset of the hidden scan below: the rows the render cost step's report found worst (-97.3 dB at
    // 88200 x 3 / 73 Hz, width 4095, before this step), rows where a weaker shaping of the output's rounding (a
    // first-order loop) brings the error up from nothing (88200 / 31, 96000 / 26: -101 dB), and one
    // small-denominator row near the top of the range at every other internal rate; widths 1 and 4095 (the
    // narrowest: the float rounding of an output half a converter range off zero against a fundamental at
    // about -66 dB re 1, docs/decisions.md).
    std::vector<ExactCase> cases;
    for (const auto& pq : std::vector<std::pair<int, int>> { { 3, 73 }, { 3, 70 }, { 3, 71 }, { 4, 99 }, { 2, 49 }, { 1, 31 }, { 1, 23 } })
        cases.push_back ({ 88200.0, 20000.0, pq.first, pq.second });
    for (const auto& pq : std::vector<std::pair<int, int>> { { 2, 59 }, { 2, 55 }, { 2, 57 }, { 1, 26 } })
        cases.push_back ({ 96000.0, 20000.0, pq.first, pq.second });
    for (const auto& rc : rateConfigs())
        if (rc.internalRate > 96000.0)
        {
            const auto all = smallDenominators (rc, 140, 3500.0, 3850.0);
            REQUIRE (! all.empty());
            cases.push_back (all.front()); // the smallest denominator there
        }
    ExactWorst w;
    for (const auto& k : cases)
        for (const float width : { 1.0f, 4095.0f })
        {
            const auto r = pulseExact (Model::B, k.rate, k.band, k.p, k.q, width);
            INFO ("Model B pulse width " << width << " at " << k.rate << " x " << k.p << " / " << k.q << " Hz (" << k.rate * k.p / k.q << " Hz): worst " << r.worstDb << " dB at " << r.worstHz << " Hz");
            CHECK (r.worstDb <= -100.0);
            CHECK (r.worstDb <= -103.0); // the 3 dB margin of 8.17
            account (w, r, Model::B, k, width);
        }
    UNSCOPED_INFO ("worst " << w.db << " dB (width " << w.width << ", " << w.f << " Hz at " << w.rate << " Hz internal)");
    CHECK (w.rows == static_cast<int> (cases.size()) * 2);
}

TEST_CASE ("render: narrow pulses by the exact measurement, every internal rate, full scan (report and assertion)", "[.render-exact]")
{
    // Every frequency p / q x rate in lowest terms with q <= 100 (140 at 352.8 and 384 kHz, where 3.85 kHz needs
    // q >= 92) from 300 Hz to 3.85 kHz, both models, the four narrowest widths and two more; then every width
    // class at the worst frequencies of each rate. Held to the budget of 8.4.5 with 3 dB to spare.
    std::vector<ExactWorst> perRate;
    for (const auto& rc : rateConfigs())
    {
        ExactWorst w;
        const auto cases = smallDenominators (rc, rc.internalRate > 300000.0 ? 140 : 100, 300.0, 3850.0);
        struct Seen
        {
            double db;
            ExactCase k;
        };
        std::vector<Seen> worstCases;
        for (const Model m : { Model::B, Model::A })
            for (const float width : { 1.0f, 2.0f, 4094.0f, 4095.0f, 64.0f, 4032.0f })
                for (const auto& k : cases)
                {
                    const auto r = pulseExact (m, k.rate, k.band, k.p, k.q, width);
                    const double margin = r.worstDb - pulseBudget (m, k.rate * k.p / k.q);
                    if (margin > -3.0)
                        std::printf ("over: %c width %4.0f at %.0f x %d / %d Hz: %.2f dB at %.1f Hz\n", m == Model::A ? 'A' : 'B', static_cast<double> (width), k.rate, k.p, k.q, r.worstDb, r.worstHz);
                    CHECK (margin <= -3.0);
                    account (w, r, m, k, width);
                    if (m == Model::B)
                        worstCases.push_back ({ r.worstDb, k });
                }
        std::sort (worstCases.begin(), worstCases.end(), [] (const Seen& a, const Seen& b) { return a.db > b.db; });
        for (std::size_t i = 0; i < std::min<std::size_t> (worstCases.size(), 4); ++i)
            for (const float width : { 3.0f, 4.0f, 8.0f, 16.0f, 256.0f, 1024.0f, 2048.0f, 3072.0f, 3840.0f, 4064.0f, 4088.0f, 4092.0f, 4093.0f })
            {
                const auto& k = worstCases[i].k;
                const auto r = pulseExact (Model::B, k.rate, k.band, k.p, k.q, width);
                CHECK (r.worstDb <= -103.0);
                account (w, r, Model::B, k, width);
            }
        std::printf ("%6.0f Hz internal: %5d rows, worst %7.2f dB (%c width %4.0f at %8.2f Hz), margin to the budget %6.2f dB\n", rc.internalRate, w.rows, w.db, w.model == Model::A ? 'A' : 'B',
                     static_cast<double> (w.width), w.f, -w.margin);
        perRate.push_back (w);
    }
    CHECK (perRate.size() == 7);
}

namespace
{
struct SyncRow
{
    Model model;
    std::uint8_t control; // the target's waveform bits
    float width;
    int num, den; // the target's frequency / the source's, exact in the increments
    double ratio() const { return static_cast<double> (num) / static_cast<double> (den); }
};

struct SyncResult
{
    double strongestDb = -300.0; // the strongest line in 0 .. band, dB re 1
    double worstDb = -300.0;     // the worst component off the source's lines, dB re the strongest line
    double worstHz = 0.0;
    std::uint64_t corrections = 0;
};

/** Oscillator 2 synced by oscillator 1 (a saw with its gate off: silent), the faster of the two at about `hz`,
    at an exact ratio num / den: the registers are num x M and den x M with M a quarter-integer, so the
    increments are exact multiples of one another. Every legitimate component is a multiple of the source's
    frequency. 7-term Blackman-Harris, 2^16 samples, not
    bin-centred; the strongest line and the worst component outside DC's main lobe and more than 9 bins from
    every line. */
SyncResult syncExact (const SyncRow& r, double hz, double rate, double band)
{
    Chip c = makeChip (r.model, rate);
    const double fastest = hz * 16777216.0 / 985248.0;
    const double unit = std::floor (fastest / static_cast<double> (std::max (r.num, r.den))) + 0.25;
    setOsc (c, 0, unit * r.den, 2048.0f, ctrl::saw);
    setOsc (c, 1, unit * r.num, r.width, static_cast<std::uint8_t> (r.control | ctrl::sync | ctrl::gate));
    REQUIRE (c.increment (1) * static_cast<std::uint64_t> (r.den) == c.increment (0) * static_cast<std::uint64_t> (r.num));
    const double lineHz = static_cast<double> (c.increment (0)) / 72057594037927936.0 * 985248.0;
    renderSamples (c, std::max (4096, static_cast<int> (4.0 * rate / lineHz)));
    const auto y = renderSamples (c, 1 << 16);
    const auto mag = windowedMagnitudes (y);
    const double binHz = rate / static_cast<double> (y.size());
    constexpr double guard = 9.0;
    double strongest = 0.0;
    for (std::size_t b = static_cast<std::size_t> (guard) + 1; b < mag.size() && static_cast<double> (b) * binHz <= band; ++b)
        strongest = std::max (strongest, mag[b]);
    SyncResult s;
    s.strongestDb = 20.0 * std::log10 (2.0 * strongest / (static_cast<double> (y.size()) * 0.27105140069342) + 1e-300);
    for (std::size_t b = static_cast<std::size_t> (guard) + 1; b < mag.size() && static_cast<double> (b) * binHz <= band; ++b)
    {
        const double f = static_cast<double> (b) * binHz;
        if (std::abs (f - std::round (f / lineHz) * lineHz) / binHz <= guard)
            continue;
        const double db = 20.0 * std::log10 (mag[b] / strongest + 1e-300);
        if (db > s.worstDb)
        {
            s.worstDb = db;
            s.worstHz = f;
        }
    }
    s.corrections = c.renderLargeCorrections();
    return s;
}

/** The sync budgets of 8.4.5: triangle, saw and pulse on Model B -100 dB; Model A's converter and combined
    waveforms -96 dB to 2 kHz, -80 dB to 3.85 kHz. */
double syncBudget (const SyncRow& r, double hz)
{
    const bool combined = vs::bside::chip::wave::isCombined (vs::bside::chip::wave::bits (r.control));
    if (r.model == Model::B && ! combined)
        return -100.0;
    return hz <= 2000.0 ? -96.0 : -80.0;
}

std::vector<SyncRow> syncRows (const std::vector<std::pair<int, int>>& ratios, bool wide)
{
    std::vector<SyncRow> rows;
    for (const Model m : { Model::B, Model::A })
        for (const auto& [num, den] : ratios)
        {
            rows.push_back ({ m, ctrl::saw, 2048.0f, num, den });
            rows.push_back ({ m, ctrl::triangle, 2048.0f, num, den });
            rows.push_back ({ m, ctrl::pulse, 1300.0f, num, den });
            rows.push_back ({ m, ctrl::triangle | ctrl::saw, 2048.0f, num, den });
            rows.push_back ({ m, ctrl::triangle | ctrl::saw | ctrl::pulse, 1024.0f, num, den });
            if (wide)
            {
                rows.push_back ({ m, ctrl::pulse, 64.0f, num, den });
                rows.push_back ({ m, ctrl::pulse, 4032.0f, num, den });
                rows.push_back ({ m, ctrl::triangle | ctrl::pulse, 1300.0f, num, den });
                rows.push_back ({ m, ctrl::saw | ctrl::pulse, 1300.0f, num, den });
            }
        }
    return rows;
}

const char* waveName (std::uint8_t c)
{
    switch (c)
    {
        case ctrl::saw: return "saw";
        case ctrl::triangle: return "triangle";
        case ctrl::pulse: return "pulse";
        case ctrl::triangle | ctrl::saw: return "triangle+saw";
        case ctrl::triangle | ctrl::pulse: return "triangle+pulse";
        case ctrl::saw | ctrl::pulse: return "saw+pulse";
        default: return "triangle+saw+pulse";
    }
}

struct SyncWorst
{
    double margin = -300.0;
    int rows = 0, silent = 0;
};

void checkSync (const SyncRow& r, double hz, const RateConfig& rc, SyncWorst& w, bool print)
{
    const auto s = syncExact (r, hz, rc.internalRate, rc.band);
    if (s.strongestDb < -120.0)
    {
        // a silent target (reset before its pulse opens, or before a combined shape with the pulse leaves its
        // zero cells: ratios below 1): nothing to measure, as in the Kaiser measurement (docs/decisions.md S2b-2
        // row 18)
        CHECK ((r.control & ctrl::pulse) != 0);
        CHECK (r.num < r.den);
        ++w.silent;
        return;
    }
    const double budget = syncBudget (r, hz);
    INFO ((r.model == Model::A ? "Model A " : "Model B ") << waveName (r.control) << " width " << r.width << " synced at ratio " << r.num << " / " << r.den << ", " << hz << " Hz, " << rc.internalRate
                                                         << " Hz internal: worst " << s.worstDb << " dB re the strongest line at " << s.worstHz << " Hz (budget " << budget << ")");
    CHECK (s.worstDb <= budget);
    CHECK (s.worstDb <= budget - 3.0);
    CHECK (s.corrections == 0);
    if (print && s.worstDb - budget > w.margin)
        std::printf ("  new worst: %c %-18s width %4.0f ratio %-7.4g %5.0f Hz at %6.0f Hz: %7.1f dB re the strongest line at %7.0f Hz (budget %.0f)\n", r.model == Model::A ? 'A' : 'B',
                     waveName (r.control), static_cast<double> (r.width), r.ratio(), hz, rc.internalRate, s.worstDb, s.worstHz, budget);
    w.margin = std::max (w.margin, s.worstDb - budget);
    ++w.rows;
}
} // namespace

TEST_CASE ("render: synced targets at exact integer and small-rational ratios meet the sync budgets (not bin-centred)", "[render][alias][sync-exact]")
{
    // The fast subset of the hidden scan below. At these ratios a sync reset falls on the same instant as one of
    // the target's own boundaries (its wrap or fold, or a cell of a combined waveform); before this step every
    // other reset lost its step or slope change there (docs/decisions.md "B-Side alias follow-up step").
    SyncWorst w;
    for (const auto& rc : rateConfigs())
    {
        if (rc.internalRate > 96000.0)
            continue; // the hidden scan covers the others
        for (const auto& r : syncRows ({ { 1, 1 }, { 2, 1 }, { 3, 1 }, { 5, 1 }, { 1, 2 }, { 3, 2 }, { 5, 2 } }, false))
            for (const double hz : { 1000.0, 3848.0 })
                checkSync (r, hz, rc, w, false);
    }
    UNSCOPED_INFO ("worst margin to the budget " << w.margin << " dB over " << w.rows << " rows");
    CHECK (w.rows == 2 * 2 * 2 * 7 * 5);
    CHECK (w.silent == 0);
}

TEST_CASE ("render: synced targets at exact ratios, every internal rate, full scan (report and assertion)", "[.render-sync-integer]")
{
    SyncWorst all;
    for (const auto& rc : rateConfigs())
    {
        SyncWorst w;
        for (const auto& r : syncRows ({ { 1, 1 }, { 2, 1 }, { 3, 1 }, { 4, 1 }, { 5, 1 }, { 6, 1 }, { 8, 1 }, { 1, 2 }, { 1, 3 }, { 1, 4 }, { 3, 2 }, { 5, 2 }, { 7, 2 }, { 4, 3 }, { 5, 3 }, { 3, 4 }, { 5, 4 } }, true))
            for (const double hz : { 110.0, 1000.0, 2000.0, 3000.0, 3500.0, 3848.0 })
                checkSync (r, hz, rc, w, true);
        std::printf ("%6.0f Hz internal: %5d rows, worst margin to the budget %6.1f dB (%d silent targets skipped)\n", rc.internalRate, w.rows, w.margin, w.silent);
        all.margin = std::max (all.margin, w.margin);
        all.rows += w.rows;
        all.silent += w.silent;
    }
    std::printf ("all rates: %d rows, worst margin to the budget %.1f dB (%d silent targets skipped)\n", all.rows, all.margin, all.silent);
    CHECK (all.rows > 10000);
}
