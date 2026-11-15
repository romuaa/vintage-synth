// bside_shapefit: our combined-waveform fit (source/bside/chip/CombinedWaveforms.h; docs/architecture.md
// 8.4.5, 8.15.8). JUCE-free, deterministic (own random generator, fixed seeds; one thread per table, each
// with its own generator, so the result does not depend on scheduling).
//
//   bside_shapefit gen <dir>                write the measurement traces (ShapePlan.h) into <dir>
//   bside_shapefit fit <dir> <params.cpp>   fit the parameters to <dir>'s traces and references and write
//                                           them as source (source/bside/chip/CombinedWaveformParams.cpp)
//   bside_shapefit report <dir>             fit errors of the committed parameters, per model, waveform
//                                           combination and pulse width
//
// The references are made by running the reference emulator's adapter on each trace (see
// tests/bside/shapes/MANIFEST for the command); this tool never runs it.

#include "../shapes/ShapeData.h"

#include "bside/chip/CombinedWaveforms.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace vs::bside;
using chip::combined::NetworkParams;

namespace
{
constexpr int kParams = 11;
using Vec = std::array<double, kParams>;

NetworkParams toParams (const Vec& x)
{
    NetworkParams p;
    p.triUp = x[0];
    p.triDown = x[1];
    p.sawUp = x[2];
    p.sawDown = x[3];
    p.pulseSource = x[4];
    p.pulseLine = x[5];
    p.driveSlope = x[6];
    p.couplingSlope = x[7];
    p.pulseSlope = x[8];
    p.threshold = x[9];
    p.thresholdSlope = x[10];
    return p;
}

Vec fromParams (const NetworkParams& p)
{
    return { p.triUp, p.triDown, p.sawUp, p.sawDown, p.pulseSource, p.pulseLine, p.driveSlope, p.couplingSlope, p.pulseSlope, p.threshold, p.thresholdSlope };
}

bool active (int k, std::uint8_t w)
{
    if (k < 2)
        return (w & chip::wave::kTri) != 0;
    if (k < 4)
        return (w & chip::wave::kSaw) != 0;
    if (k < 6 || k == 8)
        return (w & chip::wave::kPulse) != 0;
    return true;
}

/** Search scale of each parameter. */
double scaleOf (int k) { return k < 6 ? 1.0 : k == 9 ? 0.1 : k == 10 ? 0.01 : 0.2; }

/** The full-table target of one model and combination: observed bits 11..4 per index. */
struct Target
{
    chip::Model model = chip::Model::A;
    std::uint8_t wave = 0;
    std::array<std::uint8_t, 4096> value {}, observed {};
};

long cost (const Vec& x, const Target& t, long limit)
{
    const NetworkParams p = toParams (x);
    long c = 0;
    for (unsigned ix = 0; ix < 4096 && c < limit; ++ix)
    {
        if (t.observed[ix] == 0)
            continue;
        const auto v = static_cast<unsigned> (chip::combined::evaluate (p, t.wave, static_cast<std::uint16_t> (ix)).value >> 4);
        c += std::popcount ((v ^ t.value[ix]) & t.observed[ix]);
    }
    return c;
}

double minMargin (const Vec& x, std::uint8_t w)
{
    const NetworkParams p = toParams (x);
    double m = 1e300;
    for (unsigned ix = 0; ix < 4096; ++ix)
        m = std::min (m, chip::combined::evaluate (p, w, static_cast<std::uint16_t> (ix)).margin);
    return m;
}

std::uint64_t splitmix (std::uint64_t& s)
{
    std::uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
double uniform (std::uint64_t& s) { return static_cast<double> (splitmix (s) >> 11) * (1.0 / 9007199254740992.0); }

/** Pattern search from x: every active parameter up and down by its scale x step, step halved when
    nothing improves; then random joint moves at the final scale. */
long polish (Vec& x, const Target& t, long c, std::uint64_t& rng)
{
    for (int round = 0; round < 3; ++round)
    {
        double step = round == 0 ? 1.0 : 0.25;
        while (step > 1.0e-4)
        {
            bool improved = false;
            for (int k = 0; k < kParams; ++k)
            {
                if (! active (k, t.wave))
                    continue;
                for (const double sign : { -1.0, 1.0 })
                {
                    Vec y = x;
                    y[static_cast<std::size_t> (k)] += sign * step * scaleOf (k);
                    const long cy = cost (y, t, c);
                    if (cy < c)
                    {
                        c = cy;
                        x = y;
                        improved = true;
                    }
                }
            }
            if (! improved)
                step *= 0.5;
        }
        for (int tries = 0; tries < 400; ++tries)
        {
            Vec y = x;
            for (int k = 0; k < kParams; ++k)
                if (active (k, t.wave))
                    y[static_cast<std::size_t> (k)] += (uniform (rng) - 0.5) * 0.2 * scaleOf (k);
            const long cy = cost (y, t, c + 1);
            if (cy <= c)
            {
                c = cy;
                x = y;
            }
        }
    }
    return c;
}

Vec fitOne (const Target& t, const Vec& start, std::uint64_t seed, long& best)
{
    std::uint64_t rng = seed;
    struct Cand
    {
        long c;
        Vec x;
    };
    std::vector<Cand> pool;
    pool.push_back ({ cost (start, t, 1L << 40), start });
    for (int i = 0; i < 6000; ++i)
    {
        Vec x {};
        for (int k = 0; k < 6; ++k)
            x[static_cast<std::size_t> (k)] = uniform (rng) * 12.0 - 6.0;
        x[6] = uniform (rng) - 0.5;
        x[7] = uniform (rng) - 0.5;
        x[8] = uniform (rng) - 0.5;
        x[9] = 0.3 + 0.69 * uniform (rng);
        x[10] = (uniform (rng) - 0.5) * 0.05;
        for (int k = 0; k < kParams; ++k)
            if (! active (k, t.wave))
                x[static_cast<std::size_t> (k)] = 0.0;
        pool.push_back ({ cost (x, t, 1L << 40), x });
    }
    std::stable_sort (pool.begin(), pool.end(), [] (const Cand& a, const Cand& b) { return a.c < b.c; });
    best = 1L << 40;
    Vec bx = start;
    for (std::size_t i = 0; i < 16 && i < pool.size(); ++i)
    {
        Vec x = pool[i].x;
        const long c = polish (x, t, pool[i].c, rng);
        if (c < best)
        {
            best = c;
            bx = x;
        }
    }
    // Floating-point safety: keep every decision away from its threshold (kMinMargin).
    if (minMargin (bx, t.wave) < 1.0e3 * chip::combined::kMinMargin)
        for (int j = 1; j < 200; ++j)
            for (const double sign : { 1.0, -1.0 })
            {
                Vec y = bx;
                y[9] += sign * j * 1.0e-6;
                if (cost (y, t, best + 1) == best && minMargin (y, t.wave) >= 1.0e3 * chip::combined::kMinMargin)
                {
                    bx = y;
                    j = 200;
                    break;
                }
            }
    return bx;
}

/** Noise combinations: one constant shape, each observed bit by majority (bits 3..0: 0). */
std::uint16_t fitConstant (const std::vector<shapes::Sample>& samples)
{
    unsigned v = 0;
    for (int b = 0; b < 8; ++b)
    {
        long ones = 0, zeros = 0;
        for (const auto& s : samples)
            if (s.pulseHigh && ((s.observed() >> b) & 1u) != 0)
                (((s.value >> b) & 1u) != 0 ? ones : zeros) += 1;
        if (ones > zeros)
            v |= 1u << (b + 4);
    }
    return static_cast<std::uint16_t> (v);
}

std::string hexfloat (double d)
{
    char b[64];
    std::snprintf (b, sizeof b, "%a", d);
    return b;
}

std::vector<Target> targets (const std::vector<shapes::LoadedPlan>& plans)
{
    std::vector<Target> v;
    for (const auto m : { chip::Model::A, chip::Model::B })
        for (const std::uint8_t w : chip::combined::kNetworkCombinations)
        {
            Target t;
            t.model = m;
            t.wave = w;
            for (const auto& lp : plans)
                for (const auto& s : lp.samples)
                    if (s.model == m && s.wave == w && s.pulseWidth == 0 && s.pulseHigh)
                    {
                        t.value[s.index] = s.value;
                        t.observed[s.index] = s.observed();
                    }
            v.push_back (t);
        }
    return v;
}

bool loadOk (const std::vector<shapes::LoadedPlan>& plans)
{
    bool ok = true;
    for (const auto& lp : plans)
        if (! lp.error.empty())
        {
            std::fprintf (stderr, "%s: %s\n", lp.spec.name.c_str(), lp.error.c_str());
            ok = false;
        }
    return ok;
}

int report (const fs::path& dir)
{
    const auto plans = shapes::loadAll (dir);
    if (! loadOk (plans))
        return 1;
    const auto errs = shapes::errorsByPlan (plans, chip::combined::fittedTables (chip::Model::A), chip::combined::fittedTables (chip::Model::B));
    std::printf ("%-20s %8s %8s %9s %9s %9s %6s\n", "plan", "samples", "exact", "exact%", "bits%", "max", "rms");
    for (const auto& lp : plans)
    {
        const auto& e = errs.at (lp.spec.name);
        std::printf ("%-20s %8zu %8zu %8.2f%% %8.3f%% %9d %6.2f\n", lp.spec.name.c_str(), e.samples, e.exact, 100.0 * e.exactRate(),
                     e.bitsObserved == 0 ? 0.0 : 100.0 * static_cast<double> (e.bitsWrong) / static_cast<double> (e.bitsObserved), e.maxError, e.rms());
    }
    // Per model and combination over the full tables (pulse width 0), and the margins.
    for (const auto& t : targets (plans))
    {
        shapes::FitError e;
        const auto& tab = chip::combined::fittedTables (t.model);
        for (unsigned ix = 0; ix < 4096; ++ix)
            e.add (static_cast<std::uint8_t> (tab.wave[t.wave][ix] >> 4), t.value[ix], t.observed[ix]);
        const auto& p = chip::combined::fittedParams (t.model).network[t.wave];
        std::printf ("table model %s waveform %x: %zu entries, %zu exact (%.2f%%), bits wrong %zu of %zu (%.3f%%), max %d, rms %.3f, margin %.3g\n",
                     t.model == chip::Model::A ? "A" : "B", t.wave, e.samples, e.exact, 100.0 * e.exactRate(), e.bitsWrong, e.bitsObserved,
                     100.0 * static_cast<double> (e.bitsWrong) / static_cast<double> (std::max<std::size_t> (1, e.bitsObserved)), e.maxError, e.rms(),
                     minMargin (fromParams (p), t.wave));
    }
    return 0;
}

int fit (const fs::path& dir, const fs::path& outFile)
{
    const auto plans = shapes::loadAll (dir);
    if (! loadOk (plans))
        return 1;
    auto ts = targets (plans);
    std::vector<Vec> result (ts.size());
    std::vector<long> costs (ts.size());
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < ts.size(); ++i)
        threads.emplace_back ([&, i] {
            const auto& start = chip::combined::fittedParams (ts[i].model).network[ts[i].wave];
            result[i] = fitOne (ts[i], fromParams (start), 0x5ba9e000u + i, costs[i]);
        });
    for (auto& th : threads)
        th.join();
    std::string src;
    src += "// Generated by bside_shapefit fit (tests/bside/tools/ShapeFit.cpp) from tests/bside/shapes/: our own fit\n";
    src += "// (docs/architecture.md 8.15.8). Do not edit by hand; refit and review like a golden re-bless.\n";
    src += "// Fields: triUp, triDown, sawUp, sawDown, pulseSource, pulseLine, driveSlope, couplingSlope, pulseSlope,\n";
    src += "// threshold, thresholdSlope (CombinedWaveforms.h). Hexadecimal literals keep every value exact.\n\n";
    src += "#include \"bside/chip/CombinedWaveforms.h\"\n\nnamespace vs::bside::chip::combined\n{\nnamespace\n{\n";
    for (const auto m : { chip::Model::A, chip::Model::B })
    {
        const char* mn = m == chip::Model::A ? "A" : "B";
        src += std::string ("ModelParams make") + mn + "()\n{\n    ModelParams p;\n";
        for (std::size_t i = 0; i < ts.size(); ++i)
        {
            if (ts[i].model != m)
                continue;
            src += "    p.network[" + std::to_string (ts[i].wave) + "] = {";
            for (int k = 0; k < kParams; ++k)
                src += (k == 0 ? " " : ", ") + hexfloat (result[i][static_cast<std::size_t> (k)]);
            src += " }; // " + std::to_string (costs[i]) + " observed bits differ\n";
            std::fprintf (stderr, "model %s waveform %x: %ld observed bits differ\n", mn, ts[i].wave, costs[i]);
        }
        for (const std::uint8_t w : chip::combined::kNoiseCombinations)
        {
            std::vector<shapes::Sample> samples;
            for (const auto& lp : plans)
                for (const auto& s : lp.samples)
                    if (s.model == m && s.wave == w)
                        samples.push_back (s);
            char b[16];
            std::snprintf (b, sizeof b, "0x%03x", fitConstant (samples));
            src += "    p.noiseShape[" + std::to_string (w) + "] = " + b + ";\n";
        }
        src += "    return p;\n}\n";
    }
    src += "} // namespace\n\nconst ModelParams kFittedA = makeA();\nconst ModelParams kFittedB = makeB();\n} // namespace vs::bside::chip::combined\n";
    std::ofstream f (outFile, std::ios::binary);
    f << src;
    return f ? 0 : 1;
}

int gen (const fs::path& dir)
{
    fs::create_directories (dir);
    for (const auto& spec : shapes::allPlans())
    {
        const auto plan = shapes::buildPlan (spec);
        const auto tr = trace::readTrace (plan.text);
        if (! tr.ok)
        {
            std::fprintf (stderr, "%s: %s\n", spec.name.c_str(), tr.message().c_str());
            return 1;
        }
        std::ofstream f (dir / (spec.name + ".bstrace"), std::ios::binary);
        f << plan.text;
        std::printf ("%s\n", spec.name.c_str());
    }
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "gen" && argc == 3)
        return gen (argv[2]);
    if (cmd == "fit" && argc == 4)
        return fit (argv[2], argv[3]);
    if (cmd == "report" && argc == 3)
        return report (argv[2]);
    std::fprintf (stderr, "usage: bside_shapefit gen <dir> | fit <dir> <params.cpp> | report <dir>\n");
    return 2;
}
