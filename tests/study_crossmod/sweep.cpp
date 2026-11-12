// Sweeps for the map tables. Usage: sweep <table> [variant opts as key=value ...]
#include "measure.h"
#include <cstring>
#include <functional>

using namespace xm;

static W parseW (const std::string& s)
{
    if (s == "sine") return W::Sine;
    if (s == "tri") return W::Tri;
    if (s == "saw") return W::Saw;
    if (s == "sqr") return W::Square;
    return W::Pulse;
}

static void applyOpt (Cfg& c, const std::string& kv)
{
    const auto eq = kv.find ('=');
    const std::string k = kv.substr (0, eq), v = kv.substr (eq + 1);
    const double x = std::atof (v.c_str());
    if (k == "trap") c.trap = x != 0;
    else if (k == "lin") c.linFM = x != 0;
    else if (k == "naive") c.naiveMod = x != 0;
    else if (k == "hyb") c.hybrid = x != 0;
    else if (k == "curv") c.curv = x != 0;
    else if (k == "eb") c.edgeBlamp = x != 0;
    else if (k == "ssf") c.syncSlopeFix = x != 0;
    else if (k == "prod") c.prodFloat = x != 0;
    else if (k == "fix") { c.prodFloat = x != 0; c.prodFix = x != 0; }
    else if (k == "lpo") c.lpOrder = static_cast<int> (x);
    else if (k == "lphz") c.lpHz = x;
    else if (k == "lpm") c.lpMult = x;
    else if (k == "os") c.os = static_cast<int> (x);
    else if (k == "rate") c.rate = x;
    else if (k == "car") c.car = parseW (v);
    else if (k == "guard") c.guard = static_cast<int> (x);
    else if (k == "win") { c.win = x != 0; if (c.win && c.guard < 8) c.guard = 8; }
    else if (k == "mod") c.mod = parseW (v);
    else { std::fprintf (stderr, "bad opt %s\n", k.c_str()); std::exit (1); }
}

static const double kV[] = { 0.1, 0.25, 0.35, 0.5, 0.7, 0.85, 1.0 }; // slider positions
static double oct (double v) { return 4 * v * v; }
struct Ratio { int n, d; };
static const Ratio kRatios[] = { { 1, 2 }, { 1, 1 }, { 3, 2 }, { 7, 5 }, { 2, 1 }, { 5, 2 } };
static std::vector<double> kJitter = { 1.0, 1.0021, 1.0067, 1.013 };
static const double kSyncRatios[] = { 1.37, 2.71, 4.3 };

struct Agg
{
    double worst = -300, asrWorst = -300;
    std::vector<double> all;
    void add (const Result& r) { worst = std::max (worst, r.worstDb); asrWorst = std::max (asrWorst, r.asrDb); all.push_back (r.worstDb); }
    double median() { auto v = all; std::sort (v.begin(), v.end()); return v.empty() ? -300 : v[v.size() / 2]; }
};

// worst over ratios for one (f2, depth)
static Agg cell (Cfg c, bool sync, double f2, double d, double maxCarrierHz = 1e9)
{
    Agg a;
    c.sync = sync; c.depth = d;
    for (double jit : kJitter) {
    c.f2 = f2 * jit;
    if (sync)
        for (double r : kSyncRatios)
        {
            if (r * f2 > maxCarrierHz) continue;
            c.syncRatio = r;
            a.add (measure (c));
        }
    else
        for (auto r : kRatios)
        {
            if (static_cast<double> (r.n) / r.d * f2 > maxCarrierHz) continue;
            c.num = r.n; c.den = r.d;
            a.add (measure (c));
        }
    }
    return a;
}

static void mapTable (const Cfg& base, bool sync, const char* title, double maxCarrierHz = 1e9)
{
    std::printf ("\n%s  [car=%s mod=%s rate=%.0f os=%d]  worst/median alias dB | worst ASR dB (over %s x 4 f2 offsets), rows f2, cols slider v (octaves)\n",
                 title, wname (base.car), wname (base.mod), base.rate, base.os, sync ? "sync ratios 1.37/2.71/4.3" : "ratios 1/2,1,3/2,7/5,2,5/2");
    std::printf ("%6s", "f2\\v");
    for (double v : kV) std::printf (" %5.2f(%4.2f)", v, oct (v));
    std::printf (" |");
    for (double v : kV) std::printf (" %5.2f", v);
    std::printf ("\n");
    for (double f2 : { 55.0, 110.0, 220.0, 440.0, 1000.0, 2000.0, 5000.0 })
    {
        std::printf ("%6.0f", f2);
        std::vector<double> asr;
        for (double v : kV)
        {
            auto a = cell (base, sync, f2, oct (v), maxCarrierHz);
            std::printf ("  %4.0f/%4.0f", a.worst, a.median());
            asr.push_back (a.asrWorst);
        }
        std::printf (" |");
        for (double x : asr) std::printf (" %5.0f", x);
        std::printf ("\n");
        std::fflush (stdout);
    }
}

int main (int argc, char** argv)
{
    if (argc < 2) return 1;
    const std::string t = argv[1];
    Cfg base;
    for (int i = 2; i < argc; ++i) { if (std::strcmp (argv[i], "nojit") == 0) { kJitter = { 1.0 }; continue; } applyOpt (base, argv[i]); }

    if (t == "map")       { mapTable (base, false, "MAP cross-mod only"); }
    else if (t == "mapsync") { mapTable (base, true, "MAP sync + cross-mod"); }
    else if (t == "waves")
    {
        // carrier x modulator matrix, worst over ratios, at (f2, depth) points
        for (bool sync : { false, true })
            for (auto [f2, v] : { std::pair { 440.0, 0.5 }, std::pair { 2000.0, 0.5 }, std::pair { 440.0, 1.0 } })
            {
                std::printf ("\nWAVES %s f2=%.0f v=%.2f (%.2f oct) rate=%.0f os=%d: rows carrier, cols modulator (worst dB / median dB)\n",
                             sync ? "sync" : "nosync", f2, v, oct (v), base.rate, base.os);
                std::printf ("%6s", "");
                for (W m : { W::Sine, W::Tri, W::Saw, W::Square, W::Pulse }) std::printf ("  %13s", wname (m));
                std::printf ("\n");
                for (W cw : { W::Sine, W::Tri, W::Saw, W::Square, W::Pulse })
                {
                    std::printf ("%6s", wname (cw));
                    for (W m : { W::Sine, W::Tri, W::Saw, W::Square, W::Pulse })
                    {
                        Cfg c = base; c.car = cw; c.mod = m; c.width = 0.25;
                        auto a = cell (c, sync, f2, oct (v));
                        std::printf ("  %6.1f/%6.1f", a.worst, a.median());
                    }
                    std::printf ("\n");
                    std::fflush (stdout);
                }
            }
    }
    else if (t == "rates")
    {
        std::printf ("\nRATES [car=%s mod=%s] worst over ratios; columns rate\n%-28s", wname (base.car), wname (base.mod), "");
        const double rates[] = { 88200, 96000, 176400, 192000, 384000 };
        for (double r : rates) std::printf ("  %8.0f", r);
        std::printf ("\n");
        for (bool sync : { false, true })
            for (auto [f2, v] : { std::pair { 110.0, 0.35 }, std::pair { 440.0, 0.5 }, std::pair { 1000.0, 0.5 }, std::pair { 2000.0, 0.7 }, std::pair { 5000.0, 0.5 }, std::pair { 440.0, 1.0 } })
            {
                char lbl[64];
                std::snprintf (lbl, sizeof lbl, "%s f2=%.0f v=%.2f", sync ? "sync" : "xmod", f2, v);
                std::printf ("%-28s", lbl);
                for (double r : rates)
                {
                    Cfg c = base; c.rate = r;
                    if (r > 150000) c.minLog2N = 17;
                    if (r > 300000) c.minLog2N = 18;
                    auto a = cell (c, sync, f2, oct (v));
                    std::printf ("  %8.1f", a.worst);
                }
                std::printf ("\n");
                std::fflush (stdout);
            }
    }
    else if (t == "lofreq")
    {
        // VCO-2 in Lo Freq mode (1..8 Hz). Integer ratios only (carrier = harmonic of f2).
        std::printf ("\nLO FREQ modulator [car=%s mod=%s rate=%.0f]: worst dB; rows (f2, carrier), cols v\n", wname (base.car), wname (base.mod), base.rate);
        for (double f2 : { 2.0, 8.0 })
            for (int mult : { 55, 220, 1000 })
            {
                std::printf ("f2=%3.0f carrier~%5.0f Hz:", f2, f2 * mult);
                for (double v : kV)
                {
                    Cfg c = base; c.f2 = f2; c.num = mult; c.den = 1; c.depth = oct (v); c.minLog2N = 20;
                    auto r = measure (c);
                    std::printf ("  %7.1f", r.worstDb);
                    std::fflush (stdout);
                }
                std::printf ("\n");
            }
    }
    else if (t == "one")
    {
        auto r = measure (base);
        std::printf ("%.1f %.1f\n", r.worstDb, r.asrDb);
    }
    return 0;
}
