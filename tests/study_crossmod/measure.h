// Method P: periodic-by-construction alias measurement.
#pragma once
#include "xm.h"
#include "dsp/StudyCrossModPair.h"
#include <numeric>

namespace xm
{
struct Cfg
{
    W car = W::Saw, mod = W::Saw;
    double rate = 96000;  // the part's internal rate
    int os = 1;           // extra oversampling of the VCO pair (ideal decimator: measured at rate*os in 0..20 kHz)
    double f2 = 1000;     // master / modulator frequency (Hz)
    int num = 1, den = 1; // non-sync: effective carrier / f2 = num/den
    double syncRatio = 1.37; // sync: carrier knob / f2
    double depth = 1.0;   // cross-mod octaves at full modulator swing
    bool sync = false;
    double width = 0.5;   // pulse width for Pulse (both VCOs)
    // switches
    bool trap = false;         // piecewise-linear increment inside each sample
    bool linFM = false;        // 1 + d ln2 m instead of 2^(d m)
    bool naiveMod = false;     // FM from the naive (undelayed) master waveform, edges at exact positions
    bool curv = false;         // third-order corrections (study)
    bool hybrid = false;       // naiveMod only for saw/square/pulse modulators (sine/tri keep the BLEP'd output)
    bool edgeBlamp = false;    // carrier BLAMP at modulator edges (needs naiveMod)
    bool syncSlopeFix = false; // slope-change BLAMP at sync for every wave (production: triangle only)
    bool prodFloat = false;    // production float classes and fast::exp2 (rect, BLEP'd modulator)
    bool prodFix = false;      // with prodFloat: the study branch's Oscillator::processFm path (copy of Voice::renderVcosStudy mode 1)
    int lpOrder = 0;           // one-pole low-passes on the FM modulator
    double lpHz = 0;           // fixed cutoff (Hz) if > 0
    double lpMult = 0;         // tracking cutoff = lpMult * f2 if > 0
    int minLog2N = 16;
    double bandHz = 20000;
    bool noIter = false;
    bool win = false; // 7-term Blackman-Harris window (use guard >= 8)
    int guard = 0; // bins next to wanted lines excluded (close-in phase noise of float phase accumulation)
};

struct Result
{
    double worstDb = -300, worstHz = 0, asrDb = -300; // worst off-grid component (rel. strongest wanted); off-grid / all power in band
    double fEff = 0, f1Knob = 0, f2 = 0;
    double refHz = 0;
    std::vector<double> mag; // spectrum (optional use)
    double binHz = 0;
    int grid = 1;
};

inline double modNaive (W w, double q, double width)
{
    switch (w)
    {
        case W::Sine: return std::sin (kTwoPi * q);
        case W::Tri: return q < 0.5 ? 4 * q - 1 : 3 - 4 * q;
        case W::Saw: return 2 * q - 1;
        case W::Square: return q < 0.5 ? 1.0 : -1.0;
        case W::Pulse: return q < width ? 1.0 : -1.0;
    }
    return 0;
}

inline vs::dsp::Wave toProd (W w)
{
    switch (w)
    {
        case W::Sine: return vs::dsp::Wave::Sine;
        case W::Tri: return vs::dsp::Wave::Triangle;
        case W::Saw: return vs::dsp::Wave::Saw;
        case W::Square: return vs::dsp::Wave::Square;
        case W::Pulse: return vs::dsp::Wave::Pulse;
    }
    return vs::dsp::Wave::Saw;
}

struct Render
{
    std::vector<double> y; // window output
    int N = 0, W = 0;
    double r = 0;
    int grid = 1;
    double f1Knob = 0, fEff = 0;
};

// Renders the steady-state window. If keepAll, y has W+N samples (everything).
inline Render render (const Cfg& c, bool keepAll = false, int extraWarm = 0)
{
    Render out;
    const double r = c.rate * c.os;
    int log2N = c.minLog2N + (c.os == 2 ? 1 : c.os == 4 ? 2 : c.os == 8 ? 3 : 0);
    int K2 = 0, J = 0, k0 = 0;
    long N = 0;
    for (;; ++log2N)
    {
        N = 1L << log2N;
        const double bin = r / static_cast<double> (N);
        if (c.sync)
        {
            K2 = static_cast<int> (std::lround (c.f2 / bin));
            if (K2 % 2 == 0) K2 += 1;
            k0 = K2;
            if (K2 >= (c.win ? 41 : 3)) break;
        }
        else
        {
            const int g = std::gcd (c.num, c.den);
            const int a = c.den / g, b = c.num / g;
            k0 = static_cast<int> (std::lround (c.f2 / (bin * a)));
            if (k0 % 2 == 0) k0 += (k0 * bin * a < c.f2 ? 1 : -1);
            if (k0 >= (c.win ? 41 : 5)) { K2 = a * k0; J = b * k0; break; }
        }
        if (log2N > 22) { std::fprintf (stderr, "N too large\n"); std::exit (1); }
    }
    const double dt2 = static_cast<double> (K2) / static_cast<double> (N);
    int Wm = 32768 * c.os + extraWarm;
    if (c.lpOrder > 0)
    {
        const double fc = c.lpHz > 0 ? c.lpHz : c.lpMult * c.f2;
        Wm = std::max<int> (Wm, static_cast<int> (40.0 * r / (kTwoPi * fc)) + 1024);
    }
    const long total = Wm + N;
    out.N = static_cast<int> (N); out.W = Wm; out.r = r; out.grid = k0;

    // ---------- production float path ----------
    if (c.prodFloat)
    {
        // dt1Raw: from a double pre-run of the BLEP'd modulator (sync: from the knob).
        double dt1Raw = c.syncRatio * dt2;
        if (! c.sync && c.prodFix)
        {
            Cfg cd = c;
            cd.prodFloat = cd.prodFix = false;
            cd.hybrid = cd.naiveMod = cd.trap = cd.edgeBlamp = cd.syncSlopeFix = cd.curv = true;
            dt1Raw = render (cd).f1Knob / r;
        }
        else if (! c.sync)
        {
            vs::dsp::Oscillator m;
            m.setWave (toProd (c.mod));
            m.reset();
            std::vector<double> gsum (static_cast<size_t> (total));
            const float df = static_cast<float> (dt2);
            for (long i = 0; i < total; ++i)
            {
                float wr;
                const float o2 = m.process (df, static_cast<float> (c.width), -1.0f, wr);
                gsum[static_cast<size_t> (i)] = vs::dsp::fast::exp2 (static_cast<float> (c.depth) * o2);
            }
            auto tot = [&] (double raw) {
                double s = 0;
                for (long i = Wm; i < total; ++i) s += std::clamp (static_cast<double> (static_cast<float> (static_cast<float> (raw) * gsum[static_cast<size_t> (i)])), 1e-7, 0.45);
                return s;
            };
            double lo = 0, hi = 0.45;
            for (int it = 0; it < 80; ++it) { const double mid = 0.5 * (lo + hi); (tot (mid) < J ? lo : hi) = mid; }
            dt1Raw = 0.5 * (lo + hi);
        }
        for (int iter = 0; iter < ((! c.sync && ! c.noIter) ? 4 : 1); ++iter)
        {
        out.y.clear();
        double phStart = 0; long wraps = 0; float prevPh = 0;
        vs::dsp::Oscillator m, s;
        m.setWave (toProd (c.mod)); s.setWave (toProd (c.car));
        m.reset(); s.reset();
        const float df = static_cast<float> (dt2), raw = static_cast<float> (dt1Raw), dep = static_cast<float> (c.depth), w = static_cast<float> (c.width);
        out.y.reserve (static_cast<size_t> (keepAll ? total : N));
        vs::dsp::study::FmState fmst;
        for (long i = 0; i < total; ++i)
        {
            float wrap2 = -1, unused = -1;
            float o1;
            if (! c.prodFix)
            {
                const float o2 = m.process (df, w, -1.0f, wrap2);
                const float dt1 = vs::dsp::clamp (c.depth > 0 ? raw * vs::dsp::fast::exp2 (dep * o2) : raw, 1.0e-7f, 0.45f);
                o1 = s.process (dt1, w, c.sync ? wrap2 : -1.0f, unused);
            }
            else
            {
                float o2;
                o1 = vs::dsp::study::crossModPairSample (s, m, fmst, df, raw, w, dep, c.sync, 1.0e-7f, 0.45f, o2);
                if (c.sync && wrap2 < -2.0f) std::abort(); // (unused)
            }
            if (keepAll || i >= Wm) out.y.push_back (o1);
            if (i == Wm - 1) phStart = s.getPhase();
            if (i >= Wm && s.getPhase() < prevPh) ++wraps;
            prevPh = s.getPhase();
        }
        if (! c.sync) { const double tot = wraps + s.getPhase() - phStart; if (std::getenv ("XM_DBG")) std::fprintf (stderr, "iter %d J/tot-1 = %.3g\n", iter, J / tot - 1); dt1Raw *= J / tot; }
        }
        out.f1Knob = dt1Raw * r;
        out.fEff = c.sync ? 0 : J * r / static_cast<double> (N);
        return out;
    }

    // ---------- double, generalised path ----------
    auto G = [&] (double m) { return c.linFM ? 1.0 + c.depth * 0.6931471805599453 * m : std::exp2 (c.depth * m); };
    struct Node { double g0, g1, se, gPre, gPost; double syncD; };
    std::vector<Node> nodes (static_cast<size_t> (total));
    {
        OscT<double> m;
        m.wave = c.mod; m.width = c.width; m.reset();
        double q = 0; // naive master phase (same as m's)
        double lp[4] = { 0, 0, 0, 0 };
        const double fc = c.lpHz > 0 ? c.lpHz : c.lpMult * c.f2;
        const double gtpt = c.lpOrder > 0 ? std::tan (M_PI * fc / r) : 0;
        const double Gc = gtpt / (1 + gtpt);
        double prevG = 1.0;
        bool first = true;
        const bool useNaive = c.naiveMod && ! (c.hybrid && (c.mod == W::Sine || c.mod == W::Tri));
        for (long i = 0; i < total; ++i)
        {
            double wrapS;
            const double o2 = m.process (dt2, -1, wrapS);
            Node nd {};
            nd.syncD = (c.sync && wrapS >= 0) ? 1.0 - wrapS : -1.0;
            nd.se = 2.0;
            if (! useNaive)
            {
                double v = o2;
                for (int k = 0; k < c.lpOrder; ++k)
                {
                    const double vv = Gc * (v - lp[k]);
                    const double yk = vv + lp[k];
                    lp[k] = yk + vv;
                    v = yk;
                }
                const double g = G (v);
                nd.g1 = g;
                nd.g0 = c.trap ? (first ? g : prevG) : g;
                prevG = g;
            }
            else
            {
                const double qPrev = q;
                double qn = q + dt2;
                const double width = c.mod == W::Pulse ? c.width : 0.5;
                const bool pulseLike = c.mod == W::Square || c.mod == W::Pulse;
                double se = 2.0, pre = 0, post = 0;
                if (c.mod == W::Tri && qPrev < 0.5 && qPrev + dt2 >= 0.5) { se = (0.5 - qPrev) / dt2; pre = post = 1; }
                if (qn >= 1.0)
                {
                    const double s = (1.0 - qPrev) / dt2;
                    if (c.mod == W::Tri) { se = s; pre = post = -1; }
                    if (c.mod == W::Saw) { se = s; pre = 1; post = -1; }
                    else if (pulseLike) { se = s; pre = -1; post = 1; }
                    qn -= 1.0;
                }
                if (pulseLike && ((qPrev < width && qPrev + dt2 >= width) || (qn >= width && qPrev + dt2 >= 1.0)))
                {
                    const double s = qPrev < width ? (width - qPrev) / dt2 : (1.0 - qPrev + width) / dt2;
                    // two edges in one sample are impossible for the f2 used here; keep the first
                    if (se > 1.0) { se = s; pre = 1; post = -1; }
                }
                q = qn;
                const double mPrev = modNaive (c.mod, qPrev, c.width), mNow = modNaive (c.mod, q, c.width);
                nd.g0 = c.trap ? (first ? G (mNow) : G (mPrev)) : G (mNow);
                nd.g1 = G (mNow);
                nd.se = se;
                if (se <= 1.0) { nd.gPre = c.trap ? G (pre) : G (mPrev); nd.gPost = G (post); if (! c.trap) nd.g0 = G(mPrev); }
            }
            first = false;
            nodes[static_cast<size_t> (i)] = nd;
        }
    }
    auto clampDt = [] (double v) { return std::clamp (v, 1e-7, 0.45); };
    auto makeSeg = [&] (const Node& nd, double raw) {
        Seg s;
        if (nd.se > 1.0)
        {
            const double a = clampDt (raw * nd.g0), b = clampDt (raw * nd.g1);
            if (c.trap) { s.a1 = a; s.b1 = b - a; }
            else { s.a1 = b; s.b1 = 0; }
        }
        else
        {
            const double se = nd.se;
            const double a0 = clampDt (raw * nd.g0), aPre = clampDt (raw * nd.gPre), aPost = clampDt (raw * nd.gPost), a1 = clampDt (raw * nd.g1);
            if (c.trap)
            {
                // piece 1 linear a0 -> aPre over [0, se], piece 2 aPost -> a1 over [se, 1]
                s.b1 = se > 1e-12 ? (aPre - a0) / se : 0; s.a1 = a0;
                s.b2 = se < 1 - 1e-12 ? (a1 - aPost) / (1 - se) : 0; s.a2 = aPost - s.b2 * se;
            }
            else
            {
                s.a1 = a0; s.b1 = 0; s.a2 = a1; s.b2 = 0; // held values before / after the edge
            }
            s.se = se;
        }
        return s;
    };
    auto integral = [] (const Seg& s) {
        if (s.se > 1.0) return s.a1 + 0.5 * s.b1;
        const double se = s.se;
        return s.a1 * se + 0.5 * s.b1 * se * se + s.a2 * (1 - se) + 0.5 * s.b2 * (1 - se * se);
    };
    double raw = c.syncRatio * dt2;
    if (! c.sync)
    {
        auto tot = [&] (double rw) { double s = 0; for (long i = Wm; i < total; ++i) s += integral (makeSeg (nodes[static_cast<size_t> (i)], rw)); return s; };
        double lo = 0, hi = 0.45;
        for (int it = 0; it < 100; ++it) { const double mid = 0.5 * (lo + hi); (tot (mid) < J ? lo : hi) = mid; }
        raw = 0.5 * (lo + hi);
    }
    if (c.depth <= 0 && ! c.sync) raw = static_cast<double> (J) / static_cast<double> (N);
    OscT<double> s;
    s.wave = c.car; s.width = c.width; s.edgeBlamp = c.edgeBlamp; s.curv = c.curv; s.syncSlopeFix = c.syncSlopeFix; s.reset();
    out.y.reserve (static_cast<size_t> (keepAll ? total : N));
    for (long i = 0; i < total; ++i)
    {
        const auto& nd = nodes[static_cast<size_t> (i)];
        double ws;
        const Seg sg = c.depth > 0 ? makeSeg (nd, raw) : Seg { raw, 0, 2.0, 0, 0 };
        const double o1 = s.processSeg (sg, nd.syncD, ws);
        if (keepAll || i >= Wm) out.y.push_back (o1);
    }
    out.f1Knob = raw * r;
    out.fEff = c.sync ? 0 : J * r / static_cast<double> (N);
    return out;
}

inline Result measure (const Cfg& c)
{
    Render rd = render (c);
    Result res;
    if (c.win) { const double a[7] = { 0.27105140069342, -0.43329793923448, 0.21812299954311, -0.06592544638803, 0.01081174209837, -0.00077658482522, 0.00001388721735 }; const double Nn = rd.y.size(); for (size_t i = 0; i < rd.y.size(); ++i) { double w = 0; for (int k = 0; k < 7; ++k) w += a[k] * std::cos (kTwoPi * k * i / Nn); rd.y[i] *= w; } }
    res.mag = magSpectrum (rd.y);
    const double bin = rd.r / rd.N;
    res.binHz = bin; res.grid = rd.grid;
    double ref = 0;
    for (size_t b = 0; b < res.mag.size(); b += static_cast<size_t> (rd.grid))
        if (b > 0 && res.mag[b] > ref) { ref = res.mag[b]; res.refHz = b * bin; }
    double off = 0, all = 0;
    for (size_t b = 1; b < res.mag.size(); ++b)
    {
        const double f = b * bin;
        if (f > c.bandHz) break;
        const double p = res.mag[b] * res.mag[b];
        all += p;
        { const long g = static_cast<long> (rd.grid), r0 = static_cast<long> (b) % g; if (r0 <= c.guard || g - r0 <= c.guard) continue; }
        off += p;
        const double d = db (res.mag[b] / ref);
        if (d > res.worstDb) { res.worstDb = d; res.worstHz = f; }
    }
    res.asrDb = 10 * std::log10 (std::max (off, 1e-300) / std::max (all, 1e-300));
    res.fEff = rd.fEff; res.f1Knob = rd.f1Knob; res.f2 = c.f2;
    return res;
}
} // namespace xm
