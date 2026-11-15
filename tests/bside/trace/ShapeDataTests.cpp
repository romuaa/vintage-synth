// Our combined-waveform shapes (source/bside/chip/CombinedWaveforms.h) against the committed measurement
// data (tests/bside/shapes/, ShapePlan.h; docs/architecture.md 8.15.8): the data's integrity (MANIFEST,
// every trace equal to its plan, every reference belonging to its trace), the structure the contract
// assumes (the pulse mask), and the fit quality per plan within the tolerances below. The tables are
// computed from the fitted parameters; nothing here can make them equal the data by construction.

#include "../shapes/ShapeData.h"

#include "bside/chip/CombinedWaveforms.h"
#include "bside/trace/Sha256.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace vs::bside;

namespace
{
const fs::path kShapes { BS_SHAPES_DIR };

const std::vector<shapes::LoadedPlan>& loaded()
{
    static const auto plans = shapes::loadAll (kShapes);
    return plans;
}

/** Fit tolerances per plan: the least share of samples whose observed bits all agree, and the most
    observed bits that may differ (fractions). Set just below the fit as committed (docs/decisions.md),
    so a refit that gets worse fails; the pulse-with-noise plans document the limit of the table form. */
struct Tolerance
{
    double minExact, maxBits;
};
const std::map<std::string, Tolerance> kTolerance {
    { "s_sweep_3_a", { 0.995, 0.001 } }, { "s_spot_3_a", { 0.995, 0.001 } },  { "s_sweep_5_a", { 0.965, 0.006 } },
    { "s_pw_5_a_40", { 0.95, 0.01 } },   { "s_pw_5_a_a0", { 0.85, 0.02 } },   { "s_sweep_6_a", { 0.975, 0.004 } },
    { "s_spot_6_a", { 0.895, 0.016 } },  { "s_pw_6_a_40", { 0.95, 0.01 } },   { "s_pw_6_a_a0", { 0.95, 0.01 } },
    { "s_sweep_7_a", { 0.99, 0.002 } },  { "s_spot_7_a", { 0.97, 0.004 } },   { "s_pw_7_a_40", { 0.95, 0.01 } },
    { "s_pw_7_a_a0", { 0.95, 0.01 } },   { "s_noise_b_a", { 1.0, 0.0 } },     { "s_noise_c_a", { 0.20, 0.31 } },
    { "s_noise_d_a", { 1.0, 0.0 } },     { "s_noise_e_a", { 1.0, 0.0 } },     { "s_noise_f_a", { 1.0, 0.0 } },
    { "s_sweep_3_b", { 0.985, 0.003 } }, { "s_sweep_5_b", { 0.91, 0.013 } },  { "s_pw_5_b_40", { 0.95, 0.01 } },
    { "s_pw_5_b_a0", { 0.80, 0.025 } },  { "s_sweep_6_b", { 0.915, 0.012 } }, { "s_pw_6_b_40", { 0.95, 0.01 } },
    { "s_pw_6_b_a0", { 0.95, 0.01 } },   { "s_sweep_7_b", { 0.975, 0.003 } }, { "s_pw_7_b_40", { 0.95, 0.01 } },
    { "s_pw_7_b_a0", { 0.95, 0.01 } },   { "s_noise_b_b", { 1.0, 0.0 } },     { "s_noise_c_b", { 0.45, 0.21 } },
    { "s_noise_d_b", { 0.95, 0.03 } },   { "s_noise_e_b", { 1.0, 0.0 } },     { "s_noise_f_b", { 1.0, 0.0 } },
};
} // namespace

TEST_CASE ("shape data: the MANIFEST lists every file with its SHA-256; every trace is its plan, every reference its trace's", "[trace][shapes]")
{
    std::map<std::string, std::string> listed;
    std::istringstream in (shapes::slurp (kShapes / "MANIFEST"));
    std::string line;
    while (std::getline (in, line))
    {
        std::istringstream s (line);
        std::string word, hash, name;
        s >> word;
        if (word == "file")
        {
            s >> hash >> name;
            listed[name] = hash;
        }
    }
    std::set<std::string> present;
    for (const auto& e : fs::directory_iterator (kShapes))
        if (e.is_regular_file() && (e.path().extension() == ".bstrace" || e.path().extension() == ".bsref"))
            present.insert (e.path().filename().string());
    REQUIRE_FALSE (present.empty());
    for (const auto& name : present)
    {
        INFO (name);
        REQUIRE (listed.count (name) == 1);
        CHECK (listed[name] == trace::sha256Hex (shapes::slurp (kShapes / name)));
    }
    CHECK (listed.size() == present.size());
    // Every plan has its two files and loads (trace = plan text, reference of that trace, design held).
    for (const auto& lp : loaded())
    {
        INFO (lp.spec.name << ": " << lp.error);
        CHECK (lp.error.empty());
        CHECK_FALSE (lp.samples.empty());
    }
    CHECK (present.size() == 2 * loaded().size());
}

TEST_CASE ("shape data: a low pulse masks the whole output (the structure of 8.15.2 (c) step 3)", "[trace][shapes]")
{
    std::size_t low = 0;
    for (const auto& lp : loaded())
        for (const auto& s : lp.samples)
            if (! s.pulseHigh)
            {
                ++low;
                INFO (lp.spec.name << " index " << s.index);
                CHECK (s.value == 0);
            }
    CHECK (low > 100);
}

TEST_CASE ("shape data: pulse with noise depends on the noise output itself, not on the index (outside the table form)", "[trace][shapes]")
{
    // Evidence for the finding reported against 8.15.2 (c): for pulse + noise the read value is a function of
    // the noise output alone (samples with the same noise output agree, at unrelated indices), and a noise
    // output with more bits set keeps upper bits that a subset of it loses (the bits interact), which an
    // AND of the noise output with an index-addressed shape cannot do at one index. Our table form keeps a
    // constant shape for this combination (CombinedWaveforms.h); the fit tolerance below records its error.
    bool nonMonotone = false;
    for (const auto& lp : loaded())
    {
        if (lp.spec.wave != 0xc)
            continue;
        std::map<int, int> byMask;
        for (const auto& s : lp.samples)
        {
            if (! s.pulseHigh)
                continue;
            const auto [it, inserted] = byMask.emplace (s.noiseMask, s.value);
            INFO (lp.spec.name << " noise output " << int (s.noiseMask));
            CHECK (it->second == s.value);
        }
        for (const auto& [m1, v1] : byMask)
            for (const auto& [m2, v2] : byMask)
                if ((m1 & m2) == m1 && m1 != m2 && ((v2 & m1) & ~v1) != 0)
                    nonMonotone = true; // with a common shape, v2 restricted to m1 would equal v1
    }
    CHECK (nonMonotone);
}

TEST_CASE ("shape data: the fitted tables against the measurements, within the stated tolerances", "[trace][shapes]")
{
    const auto errs = shapes::errorsByPlan (loaded(), chip::combined::fittedTables (chip::Model::A), chip::combined::fittedTables (chip::Model::B));
    for (const auto& lp : loaded())
    {
        const auto& e = errs.at (lp.spec.name);
        const double bits = e.bitsObserved == 0 ? 0.0 : static_cast<double> (e.bitsWrong) / static_cast<double> (e.bitsObserved);
        std::printf ("[shapes] %-14s %5zu samples, %5zu exact (%6.2f%%), observed bits wrong %.3f%%, max %3d, rms %6.2f (rd1B steps)\n",
                     lp.spec.name.c_str(), e.samples, e.exact, 100.0 * e.exactRate(), 100.0 * bits, e.maxError, e.rms());
        INFO (lp.spec.name);
        REQUIRE (kTolerance.count (lp.spec.name) == 1);
        const auto& t = kTolerance.at (lp.spec.name);
        CHECK (e.exactRate() >= t.minExact);
        CHECK (bits <= t.maxBits);
    }
    CHECK (kTolerance.size() == loaded().size());
}
