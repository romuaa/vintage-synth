#pragma once

// Helpers of the rendering tests (ChipRenderTests.cpp, chip_bench): an FFT, coherent alias measurement as
// Vintage measures it (tests/dsp_unit/OversamplingTests.cpp: worst non-harmonic bin in 0 .. band relative
// to the fundamental, bin-centred so harmonics and aliases fall on exact bins, rectangular window), and the
// rate configurations of docs/architecture.md 4.4. JUCE-free.

#include "bside/chip/Chip.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace bs_render_test
{
using vs::bside::chip::Chip;
using vs::bside::chip::Config;
using vs::bside::chip::Model;
using vs::bside::chip::OscInput;

inline void fft (std::vector<std::complex<double>>& a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * pi / static_cast<double> (len);
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (std::size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto x = a[i + k];
                const auto y = a[i + k + len / 2] * w;
                a[i + k] = x + y;
                a[i + k + len / 2] = x - y;
                w *= wl;
            }
        }
    }
}

inline std::vector<double> magnitudes (const std::vector<float>& x)
{
    std::vector<std::complex<double>> a (x.begin(), x.end());
    fft (a);
    std::vector<double> m (a.size() / 2);
    for (std::size_t i = 0; i < m.size(); ++i)
        m[i] = std::abs (a[i]);
    return m;
}

/** The internal rates of docs/architecture.md 4.4 (host rate x factor, real time and offline) with the
    widest alias band each one has: min (20 kHz, 20/48 x host rate). A narrower band at the same internal
    rate (88.2 kHz from a 22.05 kHz host, 9.19 kHz) is contained in the wider one. */
struct RateConfig
{
    double internalRate, band;
};
inline std::vector<RateConfig> rateConfigs()
{
    return { { 88200.0, 20000.0 }, { 96000.0, 20000.0 }, { 128000.0, 32000.0 * 20.0 / 48.0 }, { 176400.0, 20000.0 },
             { 192000.0, 20000.0 }, { 352800.0, 20000.0 }, { 384000.0, 20000.0 } };
}

inline double cyclesPerSample (const Chip& c) { return static_cast<double> (c.sampleClock().increment()) / 4294967296.0; }

/** Register frequency (Smooth, register units) that puts the fundamental exactly on bin k of an n-point
    FFT at the chip's internal rate. */
inline double binFrequency (const Chip& c, double k, int n) { return k / static_cast<double> (n) * 16777216.0 / cyclesPerSample (c); }

inline int oddBin (double hz, double rate, int n)
{
    int k = static_cast<int> (std::lround (hz * n / rate));
    return k % 2 == 0 ? k + 1 : k;
}

inline Chip makeChip (Model m, double rate, std::uint32_t clock = 985248)
{
    Chip c;
    Config cfg;
    cfg.model = m;
    cfg.internalRate = rate;
    cfg.clockHz = clock;
    cfg.authentic = false;
    c.prepare (cfg);
    return c;
}

/** Oscillator n: frequency F (register units), pulse width, control; attack 0, decay 0, sustain 15. */
inline void setOsc (Chip& c, int n, double f, float pw, std::uint8_t control)
{
    OscInput in;
    in.frequency = f;
    in.pulseWidth = pw;
    in.control = control;
    in.attackDecay = 0x00;
    in.sustainRelease = 0xf0;
    c.setOscillator (n, in);
}

inline std::vector<float> renderSamples (Chip& c, int n)
{
    std::vector<float> y (static_cast<std::size_t> (n), 0.0f);
    c.render (y.data(), n);
    return y;
}

struct AliasResult
{
    double worstDb = -300.0;
    double worstHz = 0.0;
    double fundamentalDb = -300.0;
    std::uint64_t corrections = 0; // Chip::renderCorrections() after the render
};

/** Worst non-harmonic component of `y` (n samples at `rate`) in 1 .. band Hz relative to bin `fundamental`
    (or, with fundamental < 0, to the strongest multiple of `base`: a ring-modulated signal's base is not
    one of its components); every multiple of `base` is a harmonic. */
inline AliasResult aliasOf (const std::vector<float>& y, double rate, double band, int fundamental, int base)
{
    const auto mag = magnitudes (y);
    AliasResult r;
    double ref = 0.0;
    if (fundamental >= 0)
        ref = mag[static_cast<std::size_t> (fundamental)];
    else
        for (std::size_t b = static_cast<std::size_t> (base); b < mag.size(); b += static_cast<std::size_t> (base))
            ref = std::max (ref, mag[b]);
    r.fundamentalDb = 20.0 * std::log10 (2.0 * ref / static_cast<double> (y.size()) + 1e-300); // amplitude, dB re 1
    const double n = static_cast<double> (y.size());
    for (std::size_t b = 1; b < mag.size(); ++b)
    {
        const double hz = static_cast<double> (b) * rate / n;
        if (hz > band)
            break;
        if (b % static_cast<std::size_t> (base) == 0)
            continue;
        const double db = 20.0 * std::log10 (mag[b] / ref + 1e-300);
        if (db > r.worstDb)
            r = { db, hz, r.fundamentalDb, 0 };
    }
    return r;
}
/** 7-term Blackman-Harris window (side lobes far below -150 dB, main lobe +-7 bins): for signals whose
    period is not a whole number of samples (writes at chip cycles). */
inline std::vector<double> windowedMagnitudes (const std::vector<float>& x)
{
    constexpr double a[7] { 0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803, 0.01081174209837, 0.00077658482522, 0.00001388721735 };
    constexpr double pi = 3.14159265358979323846;
    const double n = static_cast<double> (x.size());
    std::vector<std::complex<double>> c (x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        double w = 0.0;
        for (int k = 0; k < 7; ++k)
            w += (k % 2 == 0 ? 1.0 : -1.0) * a[k] * std::cos (2.0 * pi * k * static_cast<double> (i) / n);
        c[i] = static_cast<double> (x[i]) * w;
    }
    fft (c);
    std::vector<double> m (c.size() / 2);
    for (std::size_t i = 0; i < m.size(); ++i)
        m[i] = std::abs (c[i]);
    return m;
}

/** Worst component of a windowed spectrum in 1 .. band Hz that lies more than `guard` bins from every
    multiple of `lineHz` (the signal's own lines), relative to the strongest line. */
inline AliasResult offGridAlias (const std::vector<double>& mag, double rate, double band, double lineHz, double guard = 9.0)
{
    const double n = 2.0 * static_cast<double> (mag.size());
    const double binHz = rate / n;
    double ref = 0.0;
    for (std::size_t b = 1; b < mag.size(); ++b)
        if (static_cast<double> (b) * binHz <= band)
            ref = std::max (ref, mag[b]);
    AliasResult r;
    r.fundamentalDb = 0.0;
    for (std::size_t b = 1; b < mag.size(); ++b)
    {
        const double hz = static_cast<double> (b) * binHz;
        if (hz > band)
            break;
        const double nearest = std::round (hz / lineHz) * lineHz;
        if (std::abs (hz - nearest) / binHz <= guard)
            continue;
        const double db = 20.0 * std::log10 (mag[b] / ref + 1e-300);
        if (db > r.worstDb)
        {
            r.worstDb = db;
            r.worstHz = hz;
        }
    }
    return r;
}

/** Renders n samples while making `write (chip, index)` calls at cycles first, first + period, ... (each
    at its exact cycle, through renderUntil). */
template <class WriteFn>
std::vector<float> renderWithWrites (Chip& c, int n, std::uint64_t first, std::uint64_t period, WriteFn&& write, std::uint64_t& nextIndex)
{
    std::vector<float> y (static_cast<std::size_t> (n), 0.0f);
    int done = 0;
    std::uint64_t at = first + nextIndex * period;
    while (done < n)
    {
        done += c.renderUntil (y.data() + done, n - done, at);
        if (c.cycle() == at)
        {
            write (c, nextIndex);
            ++nextIndex;
            at = first + nextIndex * period;
        }
    }
    return y;
}
} // namespace bs_render_test
