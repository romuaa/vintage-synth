// Wanted-partial comparison: how much does a variant change the legitimate spectrum?
// For each patch: on-grid partial levels (dB re strongest) of A and B; report max |delta| over partials
// above -40 dB and above -60 dB, the level-weighted RMS delta, and the spectral centroid (0..20 kHz).
#include "measure.h"
using namespace xm;

struct Spec { std::vector<double> lv; double centroid; };

static Spec partials (const Cfg& c)
{
    Render rd = render (c);
    auto mag = magSpectrum (rd.y);
    const double bin = rd.r / rd.N;
    double ref = 0;
    for (size_t b = rd.grid; b < mag.size(); b += rd.grid) ref = std::max (ref, mag[b]);
    Spec s;
    double num = 0, den = 0;
    for (size_t b = rd.grid; b < mag.size() && b * bin <= 20000; b += rd.grid)
    {
        s.lv.push_back (db (mag[b] / ref));
        num += b * bin * mag[b] * mag[b];
        den += mag[b] * mag[b];
    }
    s.centroid = num / den;
    return s;
}

static void cmp (const char* label, const Spec& a, const Spec& b)
{
    double m40 = 0, m60 = 0, w = 0, ws = 0;
    for (size_t i = 0; i < std::min (a.lv.size(), b.lv.size()); ++i)
    {
        const double d = std::abs (a.lv[i] - b.lv[i]);
        if (a.lv[i] > -40) m40 = std::max (m40, d);
        if (a.lv[i] > -60) m60 = std::max (m60, d);
        const double p = std::pow (10.0, a.lv[i] / 10.0);
        w += p * d * d; ws += p;
    }
    std::printf ("   %-22s max|d| >-40dB %5.2f  >-60dB %5.2f  power-weighted rms %5.3f dB  centroid %6.0f -> %6.0f Hz\n",
                 label, m40, m60, std::sqrt (w / ws), a.centroid, b.centroid);
}

int main()
{
    struct P { const char* name; W car, mod; double f2; int num, den; double sr; bool sync; double v; };
    const P patches[] = {
        { "bell: sine<-sine 7/5, v0.55", W::Sine, W::Sine, 440, 7, 5, 0, false, 0.55 },
        { "bell: tri<-sine 7/2, v0.6", W::Tri, W::Sine, 220, 7, 2, 0, false, 0.6 },
        { "metallic: saw<-sqr 3/2, v0.45", W::Saw, W::Square, 220, 3, 2, 0, false, 0.45 },
        { "growl: saw<-saw 1/1, v0.5", W::Saw, W::Saw, 110, 1, 1, 0, false, 0.5 },
        { "sync lead: saw<-saw sync 2.71, v0.35", W::Saw, W::Saw, 220, 0, 0, 2.71, true, 0.35 },
        { "sync lead: sqr<-saw sync 1.9, v0.5", W::Square, W::Saw, 440, 0, 0, 1.9, true, 0.5 },
        { "harsh: saw<-saw sync 3.3, v0.85", W::Saw, W::Saw, 330, 0, 0, 3.3, true, 0.85 },
        { "harsh: pls<-pls 5/2, v0.8", W::Pulse, W::Pulse, 330, 5, 2, 0, false, 0.8 },
    };
    for (const auto& p : patches)
    {
        Cfg c;
        c.car = p.car; c.mod = p.mod; c.f2 = p.f2; c.num = p.num; c.den = p.den; c.syncRatio = p.sr; c.sync = p.sync;
        c.depth = 4 * p.v * p.v; c.width = 0.3;
        Cfg fix = c; fix.hybrid = fix.naiveMod = fix.trap = fix.edgeBlamp = fix.syncSlopeFix = fix.curv = true;
        Cfg c192 = c; c192.rate = 192000; c192.minLog2N = 17;
        Cfg f192 = fix; f192.rate = 192000; f192.minLog2N = 17;
        Cfg lp = c; lp.lpOrder = 1; lp.lpMult = 4;
        Cfg os2 = c; os2.os = 2;
        const auto A = partials (c), B = partials (fix), A2 = partials (c192), B2 = partials (f192), L = partials (lp), O = partials (os2);
        std::printf ("%s (f2 %.0f Hz, %.2f oct)\n", p.name, p.f2, c.depth);
        cmp ("prod96 -> fix96", A, B);
        cmp ("prod96 -> prod192", A, A2);
        cmp ("fix96 -> fix192", B, B2);
        cmp ("prod96 -> lp(4f2)96", A, L);
        cmp ("prod96 -> os2 (192)", A, O);
    }
}
