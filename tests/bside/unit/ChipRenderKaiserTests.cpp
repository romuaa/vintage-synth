// The second, independent alias measurement (S2b-2 review): not bin-centred. Kaiser window (beta 20), the
// mean removed first (the large DC of a quiet shape would otherwise leak past the guard), 2x zero padding,
// the signal's own lines taken from the accumulators' actual increments, every bin more than 8 bins from
// a line counted; 20 fundamentals from 60 Hz to 3.85 kHz (10 for sync and ring). Every row must lie at
// least 3 dB inside its budget of 8.4.5. Internal rates 88.2 kHz (the lowest of 4.4, the hardest) and
// 192 kHz; the full matrix (all rates, 7 800 cases) was run once with the reviewer's harness (decisions).

#include "RenderTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <string>

using namespace bs_render_test;
namespace ctrl = vs::bside::chip::ctrl;

namespace
{
double i0 (double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 200; ++k)
    {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
        if (t < 1e-20 * s)
            break;
    }
    return s;
}

struct Measured
{
    double rel = -400.0, worstHz = 0.0, refDb = -400.0;
};

Measured kaiserAlias (const std::vector<float>& y, double fs, double band, double lineHz, double fundHz, bool againstStrongest)
{
    const std::size_t n = y.size();
    std::vector<std::complex<double>> a (2 * n, 0.0);
    double mean = 0.0;
    for (const float v : y)
        mean += static_cast<double> (v);
    mean /= static_cast<double> (n);
    const double beta = 20.0, ib = i0 (beta);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double r = 2.0 * static_cast<double> (i) / static_cast<double> (n - 1) - 1.0;
        a[i] = (static_cast<double> (y[i]) - mean) * i0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / ib;
    }
    fft (a);
    const double binHz = fs / static_cast<double> (a.size());
    std::vector<double> mag (a.size() / 2);
    for (std::size_t i = 0; i < mag.size(); ++i)
        mag[i] = std::abs (a[i]);
    auto peakNear = [&] (double hz) {
        const double b = hz / binHz;
        double m = 0.0;
        for (long k = static_cast<long> (std::floor (b)) - 3; k <= static_cast<long> (std::ceil (b)) + 3; ++k)
            if (k >= 0 && static_cast<std::size_t> (k) < mag.size())
                m = std::max (m, mag[static_cast<std::size_t> (k)]);
        return m;
    };
    double ref = peakNear (fundHz);
    if (againstStrongest)
        for (double h = lineHz; h <= band; h += lineHz)
            ref = std::max (ref, peakNear (h));
    Measured r;
    r.refDb = 20.0 * std::log10 (ref + 1e-300);
    double worst = 0.0;
    for (std::size_t b = 1; b < mag.size(); ++b)
    {
        const double hz = static_cast<double> (b) * binHz;
        if (hz > band)
            break;
        const double nearest = std::round (hz / lineHz) * lineHz;
        if (std::abs (hz - nearest) / binHz <= 16.0) // 8 bins of the unpadded transform
            continue;
        if (mag[b] > worst)
        {
            worst = mag[b];
            r.worstHz = hz;
        }
    }
    r.rel = 20.0 * std::log10 (worst / ref + 1e-300);
    return r;
}

struct Spec
{
    std::string name;
    Model model;
    std::uint8_t control; // oscillator 2
    float width;
    int kind;            // 0 alone, 1 sync (ratio), 2 ring (p, q)
    double ratio;
    int p, q;
};

/** Renders a spec at about hz (the highest oscillator frequency) and measures it. */
Measured run (const Spec& s, double rate, double hz, std::uint64_t& corrections)
{
    Chip c = makeChip (s.model, rate);
    const double clock = 985248.0;
    auto reg = [&] (double f) { return f * 16777216.0 / clock; };
    double f1 = 0.0, f2 = hz;
    if (s.kind == 1)
    {
        f1 = s.ratio >= 1.0 ? hz / s.ratio : hz;
        f2 = f1 * s.ratio;
    }
    else if (s.kind == 2)
    {
        const double b = hz / std::max (s.p, s.q);
        f1 = s.p * b;
        f2 = s.q * b;
    }
    setOsc (c, 0, reg (f1), 2048.0f, 0x00);
    setOsc (c, 1, reg (f2), s.width, static_cast<std::uint8_t> (s.control | ctrl::gate));
    const auto st = c.state();
    const double a1 = static_cast<double> (st.osc[0].increment) / 72057594037927936.0 * clock;
    const double a2 = static_cast<double> (st.osc[1].increment) / 72057594037927936.0 * clock;
    double lineHz = a2, fundHz = a2;
    if (s.kind == 1)
        lineHz = fundHz = a1;
    if (s.kind == 2)
        lineHz = a1 / s.p;
    const double fs = clock / cyclesPerSample (c);
    std::size_t n = 1 << 15;
    while (fs / static_cast<double> (n) > lineHz / 64.0 && n < (1u << 19))
        n <<= 1;
    renderSamples (c, 8192);
    const auto y = renderSamples (c, static_cast<int> (n));
    corrections = c.renderCorrections();
    const bool plain = s.kind == 0 && ! vs::bside::chip::wave::isCombined (vs::bside::chip::wave::bits (s.control));
    return kaiserAlias (y, fs, std::min (20000.0, fs * 20.0 / 48.0), lineHz, fundHz, ! plain);
}

std::vector<Spec> specs (Model m)
{
    std::vector<Spec> v;
    v.push_back ({ "triangle", m, ctrl::triangle, 2048.0f, 0, 0.0, 0, 0 });
    v.push_back ({ "saw", m, ctrl::saw, 2048.0f, 0, 0.0, 0, 0 });
    for (const float w : { 1.0f, 64.0f, 200.0f, 2048.0f, 3072.0f, 4032.0f, 4095.0f })
        v.push_back ({ "pulse", m, ctrl::pulse, w, 0, 0.0, 0, 0 });
    v.push_back ({ "triangle+saw", m, 0x30, 2048.0f, 0, 0.0, 0, 0 });
    for (const std::uint8_t cb : { std::uint8_t { 0x50 }, std::uint8_t { 0x60 }, std::uint8_t { 0x70 } })
        for (const float w : { 256.0f, 1024.0f, 2048.0f, 3072.0f })
            v.push_back ({ "combined " + std::to_string (cb >> 4), m, cb, w, 0, 0.0, 0, 0 });
    for (const double ratio : { 1.41421356, 2.71828183, 0.70710678, 1.0031 })
        for (const std::uint8_t w : { ctrl::saw, ctrl::triangle, ctrl::pulse, std::uint8_t { 0x30 }, std::uint8_t { 0x60 } })
            v.push_back ({ "sync " + std::to_string (w >> 4), m, static_cast<std::uint8_t> (w | ctrl::sync), 1300.0f, 1, ratio, 0, 0 });
    for (const auto& pq : { std::pair { 3, 5 }, std::pair { 13, 8 } })
        for (const std::uint8_t w : { ctrl::triangle, std::uint8_t { 0x50 } })
            v.push_back ({ "ring " + std::to_string (w >> 4), m, static_cast<std::uint8_t> (w | ctrl::ring), 1300.0f, 2, 0.0, pq.first, pq.second });
    return v;
}
} // namespace

TEST_CASE ("render: alias budgets, second measurement (Kaiser window, 20 fundamentals, not bin-centred)", "[render][alias][kaiser]")
{
    std::vector<double> all, few;
    for (int i = 0; i < 20; ++i)
        all.push_back (std::min (3850.0, 60.0 * std::pow (3850.0 / 60.0, i / 19.0) * (1.0 + 0.0037 * std::sin (i * 7.31 + 0.4))));
    for (std::size_t i = 0; i < all.size(); i += 2)
        few.push_back (all[i]);
    few.push_back (all.back());
    for (const double rate : { 88200.0, 192000.0 })
        for (const Model m : { Model::B, Model::A })
        {
            double worstMargin = -400.0;
            std::string worstWhat;
            for (const auto& s : specs (m))
                for (const double hz : s.kind == 0 ? all : few)
                {
                    std::uint64_t corrections = 0;
                    const auto r = run (s, rate, hz, corrections);
                    if (r.refDb < -150.0)
                        continue; // a silent output (Model A saw+pulse at wide widths: pulled down, gated off)
                    const bool combined = vs::bside::chip::wave::isCombined (vs::bside::chip::wave::bits (s.control)) || s.name.find ("ring 5") == 0;
                    const double budget = m == Model::B && ! combined ? -100.0 : (hz <= 2000.0 ? -96.0 : -80.0);
                    INFO ((m == Model::A ? "Model A " : "Model B ") << s.name << " width " << s.width << " ratio " << s.ratio << " ring " << s.p << "/" << s.q
                                                                     << ", " << rate << " Hz internal, " << hz << " Hz: " << r.rel << " dB at " << r.worstHz
                                                                     << " Hz (budget " << budget << ")");
                    CHECK (r.rel <= budget - 3.0);
                    CHECK (corrections == 0);
                    if (r.rel - budget > worstMargin)
                    {
                        worstMargin = r.rel - budget;
                        worstWhat = s.name + " width " + std::to_string (static_cast<int> (s.width)) + " at " + std::to_string (static_cast<int> (hz)) + " Hz";
                    }
                }
            WARN ("Kaiser measurement, " << (m == Model::A ? "Model A" : "Model B") << ", " << rate << " Hz internal: worst " << worstMargin
                                         << " dB re budget (" << worstWhat << ")");
        }
}
