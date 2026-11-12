// Cross-mod aliasing study harness (scratch, not production).
// OscT<T>: a copy of vs::dsp::Oscillator generalised to
//  - a phase increment that is piecewise linear inside a sample (exact quadratic crossing times),
//  - a jump of the increment at a sub-sample position (modulator edge) with a carrier BLAMP,
//  - a slope-change BLAMP at the sync instant for every waveform.
// With one constant piece and no extras it computes what the production oscillator computes.
#pragma once
#include "dsp/BlepTable.h"
#include "dsp/DspUtil.h"
#include "dsp/FastMath.h"
#include "dsp/Oscillator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace xm
{
enum class W { Sine, Tri, Saw, Square, Pulse };
inline const char* wname (W w)
{
    switch (w) { case W::Sine: return "sine"; case W::Tri: return "tri"; case W::Saw: return "saw"; case W::Square: return "sqr"; case W::Pulse: return "pls"; }
    return "?";
}
constexpr double kTwoPi = 6.283185307179586476925;

// Increment inside one sample: piece 1 on [0, se), piece 2 on [se, 1]. dt(s) = a + b s (s from sample start).
struct Seg
{
    double a1 = 0, b1 = 0; // piece 1
    double se = 2.0;       // > 1: no edge
    double a2 = 0, b2 = 0; // piece 2 (only if se <= 1)
};


// Third integral of the same windowed sinc (Z = 8, cutoff 0.45, Kaiser 8): residual for a unit jump of
// the second derivative (per sample^2). Study-only; production BlepTable has step and ramp.
class Blep3
{
public:
    static constexpr int Z = 8, R = 512;
    static const Blep3& get() { static const Blep3 t; return t; }
    double at (int j, double d) const
    {
        const double pos = d * R;
        int k = static_cast<int> (pos);
        if (k >= R) k = R - 1;
        const double f = pos - k;
        const double* c = cells.data() + (j + Z) * (R + 1) + k;
        return c[0] + (c[1] - c[0]) * f;
    }
private:
    Blep3()
    {
        constexpr int fine = 8, per = R * fine, total = 2 * Z * per;
        const double dt = 1.0 / per;
        auto I0 = [] (double x) { double s = 1, t = 1; for (int k = 1; k < 50; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } return s; };
        auto kern = [&] (double t) {
            const double x = 2.0 * 0.45 * t;
            const double sinc = std::abs (x) < 1e-12 ? 1.0 : std::sin (M_PI * x) / (M_PI * x);
            const double r = t / Z;
            const double w = std::abs (r) >= 1.0 ? 0.0 : I0 (8.0 * std::sqrt (1.0 - r * r)) / I0 (8.0);
            return 2.0 * 0.45 * sinc * w;
        };
        std::vector<double> b1 (total + 1), b2 (total + 1), b3 (total + 1);
        double a1 = 0, a2 = 0, a3 = 0, ph = kern (-Z), p1 = 0, p2 = 0;
        for (int i = 1; i <= total; ++i)
        {
            const double t = -Z + i * dt, h = kern (t);
            a1 += 0.5 * (h + ph) * dt; a2 += 0.5 * (a1 + p1) * dt; a3 += 0.5 * (a2 + p2) * dt;
            b1[i] = a1; b2[i] = a2; b3[i] = a3; ph = h; p1 = a1; p2 = a2;
        }
        const double norm = a1;
        cells.resize ((2 * Z) * (R + 1));
        for (int j = -Z; j < Z; ++j)
            for (int k = 0; k <= R; ++k)
            {
                const double t = j + static_cast<double> (k) / R;
                const auto idx = static_cast<size_t> ((t + Z) * per + 0.5);
                cells[(j + Z) * (R + 1) + k] = b3[idx] / norm - (t > 0 ? 0.5 * t * t : 0.0);
            }
    }
    std::vector<double> cells;
};

template <typename T>
class OscT
{
public:
    static constexpr int kLatency = vs::dsp::BlepTable::kZeroCrossings;
    W wave = W::Saw;
    double width = 0.5;
    bool syncSlopeFix = true;  // add the slope-change BLAMP at sync for all waves (production: triangle only)
    bool edgeBlamp = false;    // BLAMP for the increment jump at seg.se
    bool curv = false;         // third-order (curvature) corrections at edges, sync and triangle corners

    OscT() : table (&vs::dsp::BlepTable::get()) {}
    void reset (double p = 0) { phase = static_cast<T> (p); ring.fill (0); n = 0; }
    T getPhase() const { return phase; }

    // Rect convenience (production semantics).
    T process (T dt, double syncS, double& wrapS)
    {
        Seg s; s.a1 = dt; s.b1 = 0;
        return processSeg (s, syncS, wrapS);
    }

    // syncS: sync instant as a fraction of the sample from its start (0..1], or < 0.
    // wrapS out: own wrap position (fraction from start), < 0 if none.
    T processSeg (const Seg& g, double syncS, double& wrapS)
    {
        wrapS = -1;
        const bool hasEdge = g.se <= 1.0;
        // Boundaries in order
        double cur = 0;
        T p = phase;
        auto dtAt = [&] (double s, bool after) -> double {
            if (hasEdge && (s > g.se || (after && s >= g.se))) return g.a2 + g.b2 * s;
            return g.a1 + g.b1 * s;
        };
        struct Ev { double s; int kind; }; // 0 edge, 1 sync
        Ev ev[2]; int ne = 0;
        if (hasEdge) ev[ne++] = { g.se, 0 };
        if (syncS >= 0) ev[ne++] = { 1.0 - syncS + 0.0, 1 }; // syncS given as d (time to sample point)
        if (ne == 2 && std::abs (ev[1].s - ev[0].s) < 1e-9) ev[1].s = ev[0].s; // coincident: edge first, then sync
        else if (ne == 2 && ev[1].s < ev[0].s) std::swap (ev[0], ev[1]);
        for (int e = 0; e <= ne; ++e)
        {
            const double end = e < ne ? ev[e].s : 1.0;
            const bool inPiece2 = hasEdge && cur >= g.se;
            const double a = inPiece2 ? g.a2 : g.a1, b = inPiece2 ? g.b2 : g.b1;
            if (end > cur) p = advance (p, cur, end, a, b, wrapS);
            cur = end;
            if (e == ne) break;
            if (ev[e].kind == 0)
            {
                // Edge: increment jumps from dtBefore to dtAfter. If a sync falls on the same instant, the sync handler sees it.
                const bool syncHere = (e + 1 < ne && ev[e + 1].kind == 1 && std::abs (ev[e + 1].s - ev[e].s) < 1e-12);
                if (edgeBlamp && ! syncHere)
                {
                    const double before = g.a1 + g.b1 * g.se, after = g.a2 + g.b2 * g.se;
                    const double m = dWdp (p) * (after - before);
                    addKink (m, 1.0 - g.se);
                    if (curv)
                        addCurv (d2Wdp2 (p) * (after * after - before * before) + dWdp (p) * (g.b2 - g.b1), 1.0 - g.se);
                }
            }
            else
            {
                const double d = 1.0 - ev[e].s;
                const double before = dtAt (ev[e].s, false), after = dtAt (ev[e].s, true);
                const double jump = naive (0) - naive (p);
                if (std::abs (jump) > 1e-12) addStep (jump, d);
                double m;
                if (syncSlopeFix)
                    m = dWdp (0) * after - dWdp (p) * before;
                else
                    m = wave == W::Tri ? (dWdp (0) - dWdp (p)) * before : 0.0; // production: triangle only, same dt
                if (std::abs (m) > 0) addKink (m, d);
                if (curv)
                {
                    const bool p2 = hasEdge && ev[e].s >= g.se;
                    const bool p2b = hasEdge && ev[e].s > g.se;
                    const double cb = p2b ? g.b2 : g.b1, ca = p2 ? g.b2 : g.b1;
                    addCurv (d2Wdp2 (0) * after * after + dWdp (0) * ca - d2Wdp2 (p) * before * before - dWdp (p) * cb, d);
                }
                p = 0;
            }
        }
        phase = p;
        ring[n & kMask] += static_cast<T> (naive (phase));
        auto& slot = ring[(n - kLatency) & kMask];
        const T out = slot;
        slot = 0;
        ++n;
        return out;
    }

    double naive (double p) const
    {
        switch (wave)
        {
            case W::Sine: return std::sin (kTwoPi * p);
            case W::Tri: return p < 0.5 ? 4 * p - 1 : 3 - 4 * p;
            case W::Saw: return 2 * p - 1;
            case W::Square: case W::Pulse: return p < (wave == W::Pulse ? width : 0.5) ? 1.0 : -1.0;
        }
        return 0;
    }
    double d2Wdp2 (double p) const { return wave == W::Sine ? -kTwoPi * kTwoPi * std::sin (kTwoPi * p) : 0.0; }
    double dWdp (double p) const
    {
        switch (wave)
        {
            case W::Sine: return kTwoPi * std::cos (kTwoPi * p);
            case W::Tri: return p < 0.5 ? 4.0 : -4.0;
            case W::Saw: return 2.0;
            default: return 0.0;
        }
    }

private:
    // Solve a (s - s0) + b/2 (s^2 - s0^2) = target for s in [s0, s1].
    static double solve (double s0, double a, double b, double target)
    {
        if (std::abs (b) < 1e-14 * std::max (1e-30, std::abs (a)))
            return s0 + target / a;
        // with u = s - s0: (b/2) u^2 + (a + b s0) u - target = 0
        const double A = 0.5 * b, B = a + b * s0;
        const double disc = std::max (0.0, B * B + 4 * A * target);
        return s0 + 2 * target / (B + std::sqrt (disc));
    }

    T advance (T p0, double s0, double s1, double a, double b, double& wrapS)
    {
        const double w = wave == W::Pulse ? width : 0.5;
        const bool pulseLike = wave == W::Square || wave == W::Pulse;
        double p = p0;
        double s = s0;
        const double total = a * (s1 - s0) + 0.5 * b * (s1 * s1 - s0 * s0);
        double remaining = total;
        for (int guard = 0; guard < 4; ++guard)
        {
            double th = 1.0;
            int kind = 2; // 0 pulse fall, 1 tri peak, 2 wrap
            if (pulseLike && p < w) { th = w; kind = 0; }
            else if (wave == W::Tri && p < 0.5) { th = 0.5; kind = 1; }
            if (p + remaining < th) { p += remaining; break; }
            const double need = th - p;
            const double sc = std::min (s1, solve (s, a, b, need));
            const double d = 1.0 - sc;
            const double v = a + b * sc;
            if (kind == 0) addStep (-2.0, d);
            else if (kind == 1) { addKink (-8.0 * v, d); if (curv) addCurv (-8.0 * b, d); }
            else
            {
                switch (wave)
                {
                    case W::Saw: addStep (-2.0, d); break;
                    case W::Square: case W::Pulse: addStep (2.0, d); break;
                    case W::Tri: addKink (8.0 * v, d); if (curv) addCurv (8.0 * b, d); break;
                    default: break;
                }
                if (wrapS < 0) wrapS = sc;
            }
            remaining -= need;
            p = kind == 2 ? 0.0 : th;
            s = sc;
        }
        return static_cast<T> (p);
    }

    void addStep (double h, double d)
    {
        const float df = static_cast<float> (std::clamp (d, 0.0, 1.0));
        for (int j = -kLatency; j < kLatency; ++j)
            ring[(n + static_cast<unsigned> (j)) & kMask] += static_cast<T> (h * table->step (j, df));
    }
    void addKink (double m, double d)
    {
        const float df = static_cast<float> (std::clamp (d, 0.0, 1.0));
        for (int j = -kLatency; j < kLatency; ++j)
            ring[(n + static_cast<unsigned> (j)) & kMask] += static_cast<T> (m * table->ramp (j, df));
    }

    void addCurv (double c, double d)
    {
        if (c == 0.0) return;
        const auto& t3 = Blep3::get();
        d = std::clamp (d, 0.0, 1.0);
        for (int j = -kLatency; j < kLatency; ++j)
            ring[(n + static_cast<unsigned> (j)) & kMask] += static_cast<T> (c * t3.at (j, d));
    }
    static constexpr unsigned kMask = 2 * kLatency - 1;
    const vs::dsp::BlepTable* table;
    std::array<T, 2 * kLatency> ring {};
    unsigned n = 0;
    T phase = 0;
};

// ---------------- FFT ----------------
inline void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -kTwoPi / static_cast<double> (len);
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1);
            for (size_t j = 0; j < len / 2; ++j)
            {
                // recompute twiddle periodically for accuracy
                if ((j & 63) == 0) w = std::polar (1.0, ang * static_cast<double> (j));
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}
inline std::vector<double> magSpectrum (const std::vector<double>& x)
{
    std::vector<std::complex<double>> a (x.begin(), x.end());
    fft (a);
    std::vector<double> m (x.size() / 2 + 1);
    for (size_t i = 0; i < m.size(); ++i) m[i] = std::abs (a[i]) * 2.0 / static_cast<double> (x.size());
    return m;
}
inline double db (double x) { return 20.0 * std::log10 (std::max (x, 1e-15)); }
} // namespace xm
