// Measurement helpers for hard sync and Cross Mod (ported from the cross-mod aliasing study,
// branch study/crossmod-aliasing, tests/study_crossmod/measure.h, "method P").
//
// 1. measurePair(): the VCO-2 -> VCO-1 pair exactly as Voice::renderSample runs it (production
//    Oscillator, fast::exp2, the same clamps), at one internal rate, periodic by construction:
//    VCO-2 sits on an FFT bin; with sync the output repeats every VCO-2 period; without sync the
//    carrier's base increment is solved so that VCO-1 completes exactly J cycles in the window
//    (effective VCO-1 / VCO-2 = num / den). Every legitimate component then lies on a multiple of
//    the common fundamental, and anything else in the band is aliasing. Rectangular window.
// 2. spectrumPeaks() / comparePartials() / steadyNullDb(): compare two renders of the same patch
//    (real time vs offline) partial by partial and by a latency-aligned null.
#pragma once

#include "TestHelpers.h"
#include "common/dsp/FastMath.h"
#include "vintage/dsp/Oscillator.h"
#include "common/engine/Oversampler.h"
#include "vintage/engine/Part.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

namespace vs::test::xmod
{
struct PairCfg
{
    dsp::Wave car = dsp::Wave::Saw; // VCO-1 (carrier, sync slave)
    dsp::Wave mod = dsp::Wave::Saw; // VCO-2 (modulator, sync master)
    double rate = 96000.0;          // internal rate of the pair
    double f2 = 1000.0;             // VCO-2 frequency (moved onto a bin)
    bool sync = false;
    double syncRatio = 1.37;        // sync: VCO-1 knob / VCO-2
    int num = 3, den = 2;           // no sync: effective VCO-1 / VCO-2
    float depthOct = 1.0f;          // Cross Mod depth in octaves at full VCO-2 swing
    float width = 0.5f;             // pulse width
    double bandHz = 20000.0;
};

struct PairResult
{
    double worstDb = -300.0; // strongest off-grid component in (0, band], dB re the strongest on-grid one
    double worstHz = 0.0;
};

inline PairResult measurePair (const PairCfg& c)
{
    constexpr float kMinDt = 1.0e-7f, kMaxDt = 0.45f; // as Voice
    const double r = c.rate;
    int log2N = 16, K2 = 0, J = 0, k0 = 0;
    for (;; ++log2N)
    {
        const double bin = r / static_cast<double> (1 << log2N);
        if (c.sync)
        {
            K2 = static_cast<int> (std::lround (c.f2 / bin));
            K2 += (K2 % 2 == 0) ? 1 : 0;
            k0 = K2;
            if (K2 >= 3)
                break;
        }
        else
        {
            const int g = std::gcd (c.num, c.den);
            const int a = c.den / g, b = c.num / g;
            k0 = static_cast<int> (std::lround (c.f2 / (bin * a)));
            if (k0 % 2 == 0)
                k0 += (k0 * bin * a < c.f2) ? 1 : -1;
            if (k0 >= 5)
            {
                K2 = a * k0;
                J = b * k0;
                break;
            }
        }
    }
    const int N = 1 << log2N;
    const int warm = 32768;
    const int total = warm + N;
    const float dt2 = static_cast<float> (K2) / static_cast<float> (N); // exact

    double raw = c.syncRatio * static_cast<double> (dt2);
    if (! c.sync)
    {
        // Solve the base increment from the modulator's gain sequence (float, as rendered).
        dsp::Oscillator m;
        m.setWave (c.mod);
        m.reset();
        std::vector<float> gain (static_cast<size_t> (total));
        for (auto& g : gain)
        {
            float wr = -1.0f;
            const float o2 = m.process (dt2, c.width, -1.0f, wr);
            g = c.depthOct > 0.0f ? dsp::fast::exp2 (c.depthOct * o2) : 1.0f;
        }
        auto cycles = [&] (double rw) {
            double s = 0.0;
            for (size_t i = static_cast<size_t> (warm); i < gain.size(); ++i)
                s += static_cast<double> (std::clamp (static_cast<float> (rw) * gain[i], kMinDt, kMaxDt));
            return s;
        };
        double lo = 0.0, hi = 0.45;
        for (int it = 0; it < 80; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            (cycles (mid) < J ? lo : hi) = mid;
        }
        raw = 0.5 * (lo + hi);
    }

    std::vector<float> y;
    for (int iter = 0; iter < (c.sync ? 1 : 4); ++iter)
    {
        y.assign (static_cast<size_t> (N), 0.0f);
        dsp::Oscillator m, s;
        m.setWave (c.mod);
        s.setWave (c.car);
        m.reset();
        s.reset();
        const float rawF = static_cast<float> (raw);
        long wraps = 0;
        float prev = 0.0f, startPhase = 0.0f;
        for (int i = 0; i < total; ++i)
        {
            float wrap2 = -1.0f, unused = -1.0f;
            const float o2 = m.process (dt2, c.width, -1.0f, wrap2);
            const float dt1 = dsp::clamp (c.depthOct > 0.0f ? rawF * dsp::fast::exp2 (c.depthOct * o2) : rawF, kMinDt, kMaxDt);
            const float o1 = s.process (dt1, c.width, c.sync ? wrap2 : -1.0f, unused);
            if (i == warm - 1)
                startPhase = s.getPhase();
            if (i >= warm)
            {
                y[static_cast<size_t> (i - warm)] = o1;
                if (s.getPhase() < prev)
                    ++wraps;
            }
            prev = s.getPhase();
        }
        if (! c.sync)
        {
            const double cyc = static_cast<double> (wraps) + static_cast<double> (s.getPhase() - startPhase);
            raw *= static_cast<double> (J) / cyc;
        }
    }

    const auto mag = magnitudeSpectrum (y, log2N);
    const double bin = r / static_cast<double> (N);
    double ref = 0.0;
    for (size_t b = static_cast<size_t> (k0); b < mag.size(); b += static_cast<size_t> (k0))
        ref = std::max (ref, static_cast<double> (mag[b]));
    PairResult res;
    for (size_t b = 1; b < mag.size(); ++b)
    {
        const double f = static_cast<double> (b) * bin;
        if (f > c.bandHz)
            break;
        if (b % static_cast<size_t> (k0) == 0)
            continue;
        const double d = toDb (static_cast<double> (mag[b]) / ref);
        if (d > res.worstDb)
            res = { d, f };
    }
    return res;
}

// ------------------------------------------------------------------ comparing two renders

/**
    Complex spectrum of x (power-of-two length) with a 7-term Blackman-Harris window (or none),
    zero-padded `pad` times.
*/
inline std::vector<std::complex<double>> windowedSpectrum (const std::vector<float>& x, int pad = 4, bool window = true)
{
    const size_t n = x.size();
    int order = 0;
    while ((static_cast<size_t> (1) << order) < n * static_cast<size_t> (pad))
        ++order;
    const size_t m = static_cast<size_t> (1) << order;
    static constexpr double a[7] { 0.27105140069342, -0.43329793923448, 0.21812299954311, -0.06592544638803,
                                   0.01081174209837, -0.00077658482522, 0.00001388721735 };
    std::vector<std::complex<float>> in (m), out (m);
    const double twoPi = 6.283185307179586;
    for (size_t i = 0; i < n; ++i)
    {
        double w = window ? 0.0 : 1.0;
        for (int k = 0; k < 7 && window; ++k)
            w += a[k] * std::cos (twoPi * k * static_cast<double> (i) / static_cast<double> (n));
        in[i] = { static_cast<float> (w * static_cast<double> (x[i])), 0.0f };
    }
    juce::dsp::FFT fft (order);
    fft.perform (in.data(), out.data(), false);
    std::vector<std::complex<double>> s (m / 2);
    for (size_t i = 0; i < m / 2; ++i)
        s[i] = { static_cast<double> (out[i].real()), static_cast<double> (out[i].imag()) };
    return s;
}

struct Peak
{
    size_t bin;
    double hz, db; // level re the strongest peak in the band
};

/** Local maxima above floorDb (re the strongest) in [loHz, hiHz]. binHz: width of one (padded) bin. */
inline std::vector<Peak> spectrumPeaks (const std::vector<std::complex<double>>& s, double binHz, double floorDb, double loHz = 20.0,
                                        double hiHz = 20000.0, size_t halfWidth = 12)
{
    std::vector<double> mag (s.size());
    for (size_t i = 0; i < s.size(); ++i)
        mag[i] = std::abs (s[i]);
    const auto lo = static_cast<size_t> (loHz / binHz), hi = std::min (s.size() - 1 - halfWidth, static_cast<size_t> (hiHz / binHz));
    double ref = 0.0;
    for (size_t i = lo; i <= hi; ++i)
        ref = std::max (ref, mag[i]);
    std::vector<Peak> peaks;
    for (size_t i = std::max (lo, halfWidth); i <= hi; ++i)
    {
        bool isMax = mag[i] > 0.0;
        for (size_t j = i - halfWidth; j <= i + halfWidth && isMax; ++j)
            if (j != i && (mag[j] > mag[i] || (mag[j] >= mag[i] && j < i)))
                isMax = false;
        const double db = toDb (mag[i] / ref);
        if (isMax && db > floorDb)
            peaks.push_back ({ i, static_cast<double> (i) * binHz, db });
    }
    return peaks;
}

struct PartialDiff
{
    int count = 0;
    double worstDb = 0.0; // largest |level difference|
    double worstHz = 0.0;
};

/**
    For every peak of a above floorDb (re a's strongest), the level of b at the same partial (the
    largest magnitude within +-tol padded bins), both re their own strongest component.
*/
inline PartialDiff comparePartials (const std::vector<std::complex<double>>& a, const std::vector<std::complex<double>>& b, double binHz,
                                    double floorDb = -60.0, size_t tol = 4)
{
    const auto peaks = spectrumPeaks (a, binHz, floorDb);
    auto strongest = [binHz] (const std::vector<std::complex<double>>& s) {
        double r = 0.0;
        for (size_t i = static_cast<size_t> (20.0 / binHz); i < std::min (s.size(), static_cast<size_t> (20000.0 / binHz)); ++i)
            r = std::max (r, std::abs (s[i]));
        return r;
    };
    const double refA = strongest (a), refB = strongest (b);
    PartialDiff d;
    for (const auto& p : peaks)
    {
        double mb = 0.0;
        for (size_t j = p.bin - tol; j <= p.bin + tol; ++j)
            mb = std::max (mb, std::abs (b[j]));
        const double diff = toDb (mb / refB) - toDb (std::abs (a[p.bin]) / refA);
        ++d.count;
        if (std::abs (diff) > std::abs (d.worstDb))
        {
            d.worstDb = diff;
            d.worstHz = p.hz;
        }
    }
    return d;
}

/**
    Steady-state null of b against a in [20 Hz, hiHz], after the best time alignment: the residual
    power sum |A - B e^{j w tau}|^2 / sum |A|^2 in dB, minimised over tau (samples, |tau| <= maxTau).
    The renders are stationary (held notes), so a delay is a per-bin phase factor.
*/
inline double steadyNullDb (const std::vector<std::complex<double>>& a, const std::vector<std::complex<double>>& b, double binHz,
                            double sampleRate, double hiHz = 20000.0, double maxTau = 24.0, double* bestTau = nullptr)
{
    const auto lo = static_cast<size_t> (20.0 / binHz), hi = std::min (a.size(), static_cast<size_t> (hiHz / binHz));
    double pa = 0.0;
    for (size_t i = lo; i < hi; ++i)
        pa += std::norm (a[i]);
    const double twoPi = 6.283185307179586;
    auto residual = [&] (double tau) {
        double r = 0.0;
        for (size_t i = lo; i < hi; ++i)
        {
            const double w = twoPi * static_cast<double> (i) * binHz / sampleRate;
            r += std::norm (a[i] - b[i] * std::polar (1.0, w * tau));
        }
        return r;
    };
    // Coarse: the cross-correlation at whole (padded-spectrum) lags via one FFT. Residual(tau) =
    // const - 2 Re sum A conj(B) e^(-i w tau), and that sum over bins k is a forward DFT.
    const size_t m = 2 * a.size(); // full transform length of the padded spectrum
    int order = 0;
    while ((static_cast<size_t> (1) << order) < m)
        ++order;
    std::vector<std::complex<float>> in (m), out (m);
    for (size_t i = lo; i < hi; ++i)
    {
        const auto p = a[i] * std::conj (b[i]);
        in[i] = { static_cast<float> (p.real()), static_cast<float> (p.imag()) };
    }
    juce::dsp::FFT (order).perform (in.data(), out.data(), false);
    // Lag index l <-> tau = l * (m / N) samples of the signal, N = m / pad; search lags within maxTau.
    const double lagToSamples = sampleRate / (binHz * static_cast<double> (m));
    double bestRe = -1.0e300, tauBest = 0.0;
    for (size_t l = 0; l < m; ++l)
    {
        const double tau = (l < m / 2 ? static_cast<double> (l) : static_cast<double> (l) - static_cast<double> (m)) * lagToSamples;
        if (std::abs (tau) <= maxTau && static_cast<double> (out[l].real()) > bestRe)
        {
            bestRe = static_cast<double> (out[l].real());
            tauBest = tau;
        }
    }
    double best = residual (tauBest);
    double l = tauBest - lagToSamples, h = tauBest + lagToSamples;
    for (int it = 0; it < 40; ++it)
    {
        const double m1 = l + (h - l) / 3.0, m2 = h - (h - l) / 3.0;
        if (residual (m1) < residual (m2))
            h = m2;
        else
            l = m1;
    }
    tauBest = 0.5 * (l + h);
    best = std::min (best, residual (tauBest));
    if (bestTau != nullptr)
        *bestTau = tauBest;
    return 10.0 * std::log10 (std::max (best, 1.0e-300) / pa);
}

/** Prepares a Part for one of the plugin's configurations (docs/architecture.md 4.4), as the processor does. */
inline void prepareLikePlugin (Part& part, double rate, int block, bool offline, std::uint32_t seed)
{
    part.prepare (rate, block, voiceOversamplingFactor (rate, offline), seed, voiceOversamplingFactor (rate, false));
}
} // namespace vs::test::xmod
