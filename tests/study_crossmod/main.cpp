#include "measure.h"
#include <cstring>
#include <functional>
#include <map>

using namespace xm;

static W parseW (const std::string& s)
{
    if (s == "sine") return W::Sine;
    if (s == "tri") return W::Tri;
    if (s == "saw") return W::Saw;
    if (s == "sqr") return W::Square;
    return W::Pulse;
}

static void apply (Cfg& c, const std::string& kv)
{
    const auto eq = kv.find ('=');
    const std::string k = kv.substr (0, eq), v = kv.substr (eq + 1);
    const double x = std::atof (v.c_str());
    if (k == "car") c.car = parseW (v);
    else if (k == "mod") c.mod = parseW (v);
    else if (k == "rate") c.rate = x;
    else if (k == "os") c.os = static_cast<int> (x);
    else if (k == "f2") c.f2 = x;
    else if (k == "num") c.num = static_cast<int> (x);
    else if (k == "den") c.den = static_cast<int> (x);
    else if (k == "sr") c.syncRatio = x;
    else if (k == "d") c.depth = x;
    else if (k == "sync") c.sync = x != 0;
    else if (k == "width") c.width = x;
    else if (k == "trap") c.trap = x != 0;
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
    else if (k == "n") c.minLog2N = static_cast<int> (x);
    else if (k == "guard") c.guard = static_cast<int> (x);
    else if (k == "win") { c.win = x != 0; if (c.win && c.guard < 8) c.guard = 8; }
    else { std::fprintf (stderr, "unknown key %s\n", k.c_str()); std::exit (1); }
}

static void printRes (const Cfg& c, const Result& r)
{
    std::printf ("car=%s mod=%s rate=%.0f os=%d f2=%.1f %s d=%.3f -> worst %7.1f dB @ %7.0f Hz  ASR %7.1f dB  (fEff %.1f f1knob %.1f grid %d ref@%.0f)\n",
                 wname (c.car), wname (c.mod), c.rate, c.os, c.f2,
                 c.sync ? ("sync r=" + std::to_string (c.syncRatio)).c_str() : (std::to_string (c.num) + "/" + std::to_string (c.den)).c_str(),
                 c.depth, r.worstDb, r.worstHz, r.asrDb, r.fEff, r.f1Knob, r.grid, r.refHz);
}

int main (int argc, char** argv)
{
    Cfg c;
    bool dump = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "dump") == 0) { dump = true; continue; }
        apply (c, argv[i]);
    }
    const auto r = measure (c);
    printRes (c, r);
    if (dump)
    {
        // top 12 off-grid components in band
        std::vector<std::pair<double, double>> v;
        for (size_t b = 1; b < r.mag.size(); ++b)
        {
            if (b * r.binHz > c.bandHz) break;
            v.push_back ({ r.mag[b], b * r.binHz * (b % (size_t) r.grid == 0 ? -1 : 1) });
        }
        std::sort (v.begin(), v.end(), [] (auto a, auto b) { return a.first > b.first; });
        double ref = 0; for (size_t b = r.grid; b < r.mag.size(); b += r.grid) ref = std::max (ref, r.mag[b]);
        for (int i = 0; i < 24 && i < (int) v.size(); ++i)
            std::printf ("  %s %9.1f Hz %7.1f dB\n", v[i].second < 0 ? "want " : "ALIAS", std::abs (v[i].second), db (v[i].first / ref));
    }
}
