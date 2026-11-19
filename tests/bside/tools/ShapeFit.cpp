// bside_shapefit: our combined-waveform fit (source/bside/chip/CombinedWaveforms.h; docs/architecture.md
// 8.4.5, 8.15.8). JUCE-free, deterministic (own random generator, fixed seeds; one thread per table, each
// with its own generator, so the result does not depend on scheduling).
//
//   bside_shapefit gen <dir>                        write the measurement traces (ShapePlan.h) into <dir>
//   bside_shapefit fit <dir> <params.cpp>           fit rounds from the committed parameters until a round
//                                                   changes nothing (a fixed point), then write them as
//                                                   source (source/bside/chip/CombinedWaveformParams.cpp)
//   bside_shapefit verify <dir>                     one fit round from the committed parameters; exit 1 if
//                                                   it changes any parameter (they must be a fixed point)
//   bside_shapefit dump <dir> <plan>                the samples of one plan (for inspection)
//   bside_shapefit report <dir>                     fit errors of the committed parameters, per model,
//                                                   waveform combination and pulse width, and the latch
//                                                   experiment
//
// A round: from the current parameters and 6 000 random starts, pattern search from the best 16, then
// random joint moves; only strict improvements of the number of wrong observed bits are accepted, so a
// round from a fixed point returns it unchanged. The references are made by running the reference
// emulator's adapter on each trace (tests/bside/shapes/MANIFEST); this tool never runs it.

#include "../shapes/ShapeData.h"

#include "bside/chip/CombinedWaveforms.h"

#include <algorithm>
#include <cstdint>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

/** One fitted table: a network combination at an index (wave 3, 5, 6, 7) or pulse with noise (wave 0xc,
    input = the noise output). */
struct Target
{
    chip::Model model = chip::Model::A;
    std::uint8_t wave = 0;
    struct Obs
    {
        std::uint16_t input;
        std::uint8_t value, observed;
    };
    std::vector<Obs> obs;
    bool pulseNoise() const noexcept { return wave == chip::combined::kPulseNoise; }
};

bool active (int k, const Target& t)
{
    const std::uint8_t w = t.pulseNoise() ? std::uint8_t { 6 } : t.wave; // Tpn: the saw slot and the pulse node
    if (k < 2)
        return (w & chip::wave::kTri) != 0;
    if (k < 4)
        return (w & chip::wave::kSaw) != 0;
    if (k < 6 || k == 8)
        return (w & chip::wave::kPulse) != 0;
    return true;
}

double scaleOf (int k) { return k < 6 ? 1.0 : k == 9 ? 0.1 : k == 10 ? 0.01 : 0.2; }

chip::combined::Evaluation evalOne (const NetworkParams& p, const Target& t, std::uint16_t input)
{
    if (t.pulseNoise())
    {
        auto e = chip::combined::evaluatePulseNoise (p, input);
        e.value = static_cast<std::uint16_t> (e.value & input); // step 3: AND the noise output
        return e;
    }
    return chip::combined::evaluate (p, t.wave, input);
}

/** "No limit" for cost(); the value needs 64 bits (long is 32 bits on Windows). */
constexpr std::int64_t kNoLimit = std::int64_t { 1 } << 40;

std::int64_t cost (const Vec& x, const Target& t, std::int64_t limit)
{
    const NetworkParams p = toParams (x);
    std::int64_t c = 0;
    for (const auto& o : t.obs)
    {
        if (c >= limit)
            break;
        const auto v = static_cast<unsigned> (evalOne (p, t, o.input).value >> 4);
        c += std::popcount ((v ^ o.value) & o.observed);
    }
    return c;
}

double minMargin (const Vec& x, const Target& t)
{
    const NetworkParams p = toParams (x);
    double m = 1e300;
    const unsigned n = t.pulseNoise() ? 256 : 4096;
    for (unsigned i = 0; i < n; ++i)
        m = std::min (m, evalOne (p, t, static_cast<std::uint16_t> (t.pulseNoise() ? i << 4 : i)).margin);
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

/** Pattern search, then random joint moves; strict improvements only. */
std::int64_t polish (Vec& x, const Target& t, std::int64_t c, std::uint64_t& rng)
{
    for (int round = 0; round < 3; ++round)
    {
        double step = round == 0 ? 1.0 : 0.25;
        while (step > 1.0e-4)
        {
            bool improved = false;
            for (int k = 0; k < kParams; ++k)
            {
                if (! active (k, t))
                    continue;
                for (const double sign : { -1.0, 1.0 })
                {
                    Vec y = x;
                    y[static_cast<std::size_t> (k)] += sign * step * scaleOf (k);
                    const std::int64_t cy = cost (y, t, c);
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
                if (active (k, t))
                    y[static_cast<std::size_t> (k)] += (uniform (rng) - 0.5) * 0.2 * scaleOf (k);
            const std::int64_t cy = cost (y, t, c);
            if (cy < c)
            {
                c = cy;
                x = y;
            }
        }
    }
    return c;
}

/** One round from `start`. Returns `start` itself unless something strictly better was found (or its
    margin is too small, which a nudge of the threshold at equal cost repairs). */
Vec fitRound (const Target& t, const Vec& start, std::uint64_t seed, std::int64_t& best)
{
    std::uint64_t rng = seed;
    best = cost (start, t, kNoLimit);
    Vec bx = start;
    struct Cand
    {
        std::int64_t c;
        Vec x;
    };
    std::vector<Cand> pool;
    pool.push_back ({ best, start });
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
            if (! active (k, t))
                x[static_cast<std::size_t> (k)] = 0.0;
        pool.push_back ({ cost (x, t, kNoLimit), x });
    }
    std::stable_sort (pool.begin(), pool.end(), [] (const Cand& a, const Cand& b) { return a.c < b.c; });
    for (std::size_t i = 0; i < 16 && i < pool.size(); ++i)
    {
        Vec x = pool[i].x;
        const std::int64_t c = polish (x, t, pool[i].c, rng);
        if (c < best)
        {
            best = c;
            bx = x;
        }
    }
    // Floating-point safety: keep every decision at least 2 x kMinMargin from its threshold.
    const double want = 2.0 * chip::combined::kMinMargin;
    if (minMargin (bx, t) < want)
        for (int j = 1; j < 400; ++j)
        {
            bool done = false;
            for (const double sign : { 1.0, -1.0 })
            {
                Vec y = bx;
                y[9] += sign * j * 1.0e-6;
                if (cost (y, t, best + 1) == best && minMargin (y, t) >= want)
                {
                    bx = y;
                    done = true;
                    break;
                }
            }
            if (done)
                break;
        }
    return bx;
}

/** Noise with two or more of triangle, saw and pulse: one constant shape, each observed bit by majority
    (bits 3..0: 0). */
std::uint16_t fitConstant (const std::vector<shapes::Sample>& samples)
{
    unsigned v = 0;
    for (int b = 0; b < 8; ++b)
    {
        std::int64_t ones = 0, zeros = 0;
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
    {
        for (const std::uint8_t w : chip::combined::kNetworkCombinations)
        {
            Target t;
            t.model = m;
            t.wave = w;
            std::array<int, 4096> slot {};
            slot.fill (-1);
            for (const auto& lp : plans)
                for (const auto& s : lp.samples)
                    if (s.model == m && s.wave == w && s.pulseWidth == 0 && s.pulseHigh)
                    {
                        auto& k = slot[s.index];
                        if (k < 0)
                        {
                            k = static_cast<int> (t.obs.size());
                            t.obs.push_back ({ s.index, s.value, s.observed() });
                        }
                    }
            std::stable_sort (t.obs.begin(), t.obs.end(), [] (const Target::Obs& a, const Target::Obs& b) { return a.input < b.input; });
            v.push_back (t);
        }
        Target t;
        t.model = m;
        t.wave = chip::combined::kPulseNoise;
        for (const auto& lp : plans)
            for (const auto& s : lp.samples)
                if (s.model == m && s.wave == chip::combined::kPulseNoise && s.pulseHigh)
                    t.obs.push_back ({ static_cast<std::uint16_t> (s.noiseMask << 4), s.value, 0xff });
        v.push_back (t);
    }
    return v;
}

const NetworkParams& committed (const Target& t)
{
    const auto& p = chip::combined::fittedParams (t.model);
    return t.pulseNoise() ? p.pulseNoise : p.network[t.wave];
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

/** One round for every target in parallel. */
void roundAll (const std::vector<Target>& ts, std::vector<Vec>& x, std::vector<std::int64_t>& costs)
{
    std::vector<std::thread> threads;
    std::vector<Vec> out (ts.size());
    for (std::size_t i = 0; i < ts.size(); ++i)
        threads.emplace_back ([&, i] { out[i] = fitRound (ts[i], x[i], 0x5ba9e000u + i, costs[i]); });
    for (auto& th : threads)
        th.join();
    x = out;
}

int report (const fs::path& dir)
{
    const auto plans = shapes::loadAll (dir);
    if (! loadOk (plans))
        return 1;
    const auto& ta = chip::combined::fittedTables (chip::Model::A);
    const auto& tb = chip::combined::fittedTables (chip::Model::B);
    const auto errs = shapes::errorsByPlan (plans, ta, tb);
    std::printf ("%-20s %8s %8s %9s %9s %9s %6s\n", "plan", "samples", "exact", "exact%", "bits%", "max", "rms");
    for (const auto& lp : plans)
    {
        const auto& e = errs.at (lp.spec.name);
        std::printf ("%-20s %8zu %8zu %8.2f%% %8.3f%% %9d %6.2f\n", lp.spec.name.c_str(), e.samples, e.exact, 100.0 * e.exactRate(),
                     e.bitsObserved == 0 ? 0.0 : 100.0 * static_cast<double> (e.bitsWrong) / static_cast<double> (e.bitsObserved), e.maxError, e.rms());
    }
    for (const auto& t : targets (plans))
    {
        shapes::FitError e;
        const auto p = fromParams (committed (t));
        for (const auto& o : t.obs)
            e.add (static_cast<std::uint8_t> (evalOne (toParams (p), t, o.input).value >> 4), o.value, o.observed);
        std::printf ("table model %s waveform %x: %zu entries, %zu exact (%.2f%%), bits wrong %zu of %zu (%.3f%%), max %d, rms %.3f, margin %.3g\n",
                     t.model == chip::Model::A ? "A" : "B", t.wave, e.samples, e.exact, 100.0 * e.exactRate(), e.bitsWrong, e.bitsObserved,
                     100.0 * static_cast<double> (e.bitsWrong) / static_cast<double> (std::max<std::size_t> (1, e.bitsObserved)), e.maxError, e.rms(),
                     minMargin (p, t));
    }
    return 0;
}

std::string source (const std::vector<Target>& ts, const std::vector<Vec>& x, const std::vector<std::int64_t>& costs,
                    const std::vector<shapes::LoadedPlan>& plans)
{
    std::string src;
    src += "// Generated by bside_shapefit fit (tests/bside/tools/ShapeFit.cpp) from tests/bside/shapes/: our own fit\n";
    src += "// (docs/architecture.md 8.15.8). Do not edit by hand; refit and review like a golden re-bless.\n";
    src += "// Fields: triUp, triDown, sawUp, sawDown, pulseSource, pulseLine, driveSlope, couplingSlope, pulseSlope,\n";
    src += "// threshold, thresholdSlope (CombinedWaveforms.h). Hexadecimal literals keep every value exact; the\n";
    src += "// parameters are constant-initialised, so no static-initialisation order can see them unset.\n\n";
    src += "#include \"bside/chip/CombinedWaveforms.h\"\n\nnamespace vs::bside::chip::combined\n{\nnamespace\n{\n";
    for (const auto m : { chip::Model::A, chip::Model::B })
    {
        const char* mn = m == chip::Model::A ? "A" : "B";
        src += std::string ("constexpr ModelParams make") + mn + "()\n{\n    ModelParams p;\n";
        for (std::size_t i = 0; i < ts.size(); ++i)
        {
            if (ts[i].model != m)
                continue;
            src += ts[i].pulseNoise() ? std::string ("    p.pulseNoise = {") : "    p.network[" + std::to_string (ts[i].wave) + "] = {";
            for (int k = 0; k < kParams; ++k)
                src += (k == 0 ? " " : ", ") + hexfloat (x[i][static_cast<std::size_t> (k)]);
            src += " }; // " + std::to_string (costs[i]) + " observed bits differ\n";
        }
        for (const std::uint8_t w : chip::combined::kConstantNoiseCombinations)
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
    src += "} // namespace\n\nconstinit const ModelParams kFittedA = makeA();\nconstinit const ModelParams kFittedB = makeB();\n} // namespace vs::bside::chip::combined\n";
    return src;
}

int fit (const fs::path& dir, const fs::path& outFile, bool verifyOnly)
{
    const auto plans = shapes::loadAll (dir);
    if (! loadOk (plans))
        return 1;
    const auto ts = targets (plans);
    std::vector<Vec> x (ts.size());
    std::vector<std::int64_t> costs (ts.size());
    for (std::size_t i = 0; i < ts.size(); ++i)
        x[i] = fromParams (committed (ts[i]));
    for (int round = 1;; ++round)
    {
        const auto before = x;
        roundAll (ts, x, costs);
        bool changed = false;
        for (std::size_t i = 0; i < ts.size(); ++i)
        {
            const bool c = std::memcmp (before[i].data(), x[i].data(), sizeof (Vec)) != 0;
            changed = changed || c;
            std::fprintf (stderr, "round %d model %s waveform %x: %lld observed bits differ%s\n", round, ts[i].model == chip::Model::A ? "A" : "B",
                          ts[i].wave, static_cast<long long> (costs[i]), c ? " (changed)" : "");
        }
        if (verifyOnly)
        {
            std::fprintf (stderr, changed ? "verify: the committed parameters are NOT a fixed point\n" : "verify: the committed parameters are a fixed point\n");
            return changed ? 1 : 0;
        }
        if (! changed)
            break;
        if (round >= 50)
        {
            std::fprintf (stderr, "no fixed point after 50 rounds\n");
            return 1;
        }
    }
    std::ofstream f (outFile, std::ios::binary);
    f << source (ts, x, costs, plans);
    return f ? 0 : 1;
}

/** The samples (or latch events) of one plan, one per line: model, waveform, index, pulse, noise output, value. */
int dump (const fs::path& dir, const std::string& name)
{
    for (const auto& lp : shapes::loadAll (dir))
    {
        if (lp.spec.name != name)
            continue;
        if (! lp.error.empty())
        {
            std::fprintf (stderr, "%s\n", lp.error.c_str());
            return 1;
        }
        for (const auto& s : lp.samples)
            std::printf ("%s %x %03x %d %02x %02x\n", s.model == chip::Model::A ? "A" : "B", s.wave, s.index, s.pulseHigh ? 1 : 0, s.noiseMask, s.value);
        for (const auto& e : lp.latch)
            std::printf ("event %d %03x %02x %02x\n", e.event, e.index, e.read1, e.read2);
        return 0;
    }
    return 1;
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
        return fit (argv[2], argv[3], false);
    if (cmd == "verify" && argc == 3)
        return fit (argv[2], "", true);
    if (cmd == "dump" && argc == 4)
        return dump (argv[2], argv[3]);
    if (cmd == "report" && argc == 3)
        return report (argv[2]);
    std::fprintf (stderr, "usage: bside_shapefit gen <dir> | fit <dir> <params.cpp> | verify <dir> | report <dir>\n");
    return 2;
}
