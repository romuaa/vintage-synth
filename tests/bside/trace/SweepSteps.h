#pragma once

// The audible step of a cutoff sweep (docs/architecture.md 8.19.3, "Model A's cutoff curve at the converter's
// boundaries"; docs/decisions.md "B-Side Model A cutoff steps"): at every cutoff write of a sweep, the level of one band
// over one waveform period right after the write (from 0.5 ms on) minus its level over the period right before it,
// minus the median of the 11 steps around it (the sweep's own trend). The band is a zero-phase 4th-order Butterworth
// band-pass (high-pass at its lower edge, low-pass at its upper edge, run forward and backward). The same function
// measured the reference's figures (its 48 kHz render, outside the repository) and measures ours. JUCE-free; offline
// test code (allocates).

#include "bside/trace/TraceFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vs::bside::test
{
struct SweepStep
{
    int from = 0, to = 0;  // the cutoff register before and after the write
    double excessDb = 0.0; // the band's step minus the sweep's trend
};

namespace detail
{
struct Biquad
{
    double b0, b1, b2, a1, a2;
};

/** A 2nd-order Butterworth section (bilinear, prewarped) at `hz`, quality q; high-pass or low-pass. */
inline Biquad butterSection (double hz, double q, double rate, bool highPass)
{
    const double k = std::tan (3.14159265358979323846 * hz / rate), k2 = k * k, n = 1.0 / (1.0 + k / q + k2);
    if (highPass)
        return { n, -2.0 * n, n, 2.0 * (k2 - 1.0) * n, (1.0 - k / q + k2) * n };
    return { k2 * n, 2.0 * k2 * n, k2 * n, 2.0 * (k2 - 1.0) * n, (1.0 - k / q + k2) * n };
}

inline void runBiquad (const Biquad& s, std::vector<double>& x, bool backward)
{
    double z1 = 0.0, z2 = 0.0;
    const std::size_t n = x.size();
    for (std::size_t j = 0; j < n; ++j)
    {
        double& v = x[backward ? n - 1 - j : j];
        const double y = s.b0 * v + z1;
        z1 = s.b1 * v - s.a1 * y + z2;
        z2 = s.b2 * v - s.a2 * y;
        v = y;
    }
}

/** Zero-phase band-pass lo .. hi Hz (4th-order Butterworth edges, forward and backward). */
inline std::vector<double> bandPass (const std::vector<float>& in, double lo, double hi, double rate)
{
    std::vector<double> x (in.begin(), in.end());
    constexpr std::array<double, 2> kQ { 0.54119610014619698, 1.3065629648763766 };
    for (const bool backward : { false, true })
        for (const double q : kQ)
        {
            runBiquad (butterSection (lo, q, rate, true), x, backward);
            runBiquad (butterSection (hi, q, rate, false), x, backward);
        }
    return x;
}
} // namespace detail

/** The steps of a 48 kHz render of a sweep trace in the band lo .. hi Hz (see above), for the writes of register 0x16
    between t0 and t1 seconds that change the cutoff; `periodS` is the waveform's period. */
inline std::vector<SweepStep> sweepSteps (const trace::Trace& t, const std::vector<float>& mono48, double periodS, double lo, double hi,
                                          double t0 = 0.45, double t1 = 5.6)
{
    struct Event
    {
        double sec;
        int code;
    };
    std::vector<Event> ev;
    int low = 0;
    for (const auto& w : t.writes)
    {
        if (w.reg == 0x15)
            low = w.value & 7;
        else if (w.reg == 0x16)
        {
            const double sec = static_cast<double> (w.cycle) / static_cast<double> (t.clockHz);
            if (sec > t0 && sec < t1)
                ev.push_back ({ sec, (static_cast<int> (w.value) << 3) | low });
        }
    }
    const auto y = detail::bandPass (mono48, lo, hi, 48000.0);
    const auto n = static_cast<std::size_t> (std::lround (periodS * 48000.0));
    auto level = [&] (std::size_t a, std::size_t b) {
        double s = 0.0;
        for (std::size_t k = a; k < b && k < y.size(); ++k)
            s += y[k] * y[k];
        return 10.0 * std::log10 (s / static_cast<double> (b - a) + 1e-30);
    };
    std::vector<SweepStep> st;
    for (std::size_t k = 1; k < ev.size(); ++k)
    {
        if (ev[k].code == ev[k - 1].code)
            continue;
        const auto a = static_cast<std::size_t> (ev[k].sec * 48000.0);
        if (a < n)
            continue;
        st.push_back ({ ev[k - 1].code, ev[k].code, level (a + 24, a + 24 + n) - level (a - n, a) });
    }
    std::vector<double> raw;
    for (const auto& s : st)
        raw.push_back (s.excessDb);
    for (std::size_t i = 0; i < st.size(); ++i)
    {
        std::vector<double> around (raw.begin() + static_cast<std::ptrdiff_t> (i >= 5 ? i - 5 : 0),
                                    raw.begin() + static_cast<std::ptrdiff_t> (std::min (raw.size(), i + 6)));
        std::sort (around.begin(), around.end());
        const std::size_t m = around.size();
        const double median = m % 2 == 1 ? around[m / 2] : 0.5 * (around[m / 2 - 1] + around[m / 2]);
        st[i].excessDb = raw[i] - median;
    }
    return st;
}
} // namespace vs::bside::test
