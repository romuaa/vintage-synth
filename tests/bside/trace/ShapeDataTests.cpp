// Our combined-waveform shapes (source/bside/chip/CombinedWaveforms.h) against the committed measurement
// data (tests/bside/shapes/, ShapePlan.h; docs/architecture.md 8.15.8): the data's integrity (MANIFEST,
// every trace equal to its plan, every reference belonging to its trace), the structure the contract
// assumes (the pulse mask), and the fit quality per plan within the tolerances below. The tables are
// computed from the fitted parameters; nothing here can make them equal the data by construction.

#include "../shapes/ShapeData.h"

#include "bside/chip/CombinedWaveforms.h"
#include "bside/trace/ChipReplay.h"
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
    observed bits that may differ (fractions), at the committed fit (docs/decisions.md): plans that agree
    fully are pinned at 1.0, plans of at most 128 samples at the achieved count, the sweeps and spot
    plans 0.2 % below their exact rate; the bit fraction just above the achieved one. A refit that gets
    worse anywhere fails. */
struct Tolerance
{
    double minExact, maxBits;
};
const std::map<std::string, Tolerance> kTolerance {
    { "s_sweep_3_a", { 0.995, 0.0004 } },
    { "s_spot_3_a", { 0.996, 0.0004 } },
    { "s_sweep_5_a", { 0.970, 0.0039 } },
    { "s_pw_5_a_40", { 63.0 / 64.0, 0.0021 } },
    { "s_pw_5_a_a0", { 62.0 / 64.0, 0.0042 } },
    { "s_sweep_6_a", { 0.975, 0.0057 } },
    { "s_spot_6_a", { 0.936, 0.0093 } },
    { "s_pw_6_a_40", { 1.0, 0.0 } },
    { "s_pw_6_a_a0", { 1.0, 0.0 } },
    { "s_sweep_7_a", { 0.995, 0.0009 } },
    { "s_spot_7_a", { 0.974, 0.0036 } },
    { "s_pw_7_a_40", { 1.0, 0.0 } },
    { "s_pw_7_a_a0", { 1.0, 0.0 } },
    { "s_noise_b_a", { 1.0, 0.0 } },
    { "s_noise_c_a", { 91.0 / 96.0, 0.0129 } },
    { "s_noise_d_a", { 1.0, 0.0 } },
    { "s_noise_e_a", { 1.0, 0.0 } },
    { "s_noise_f_a", { 1.0, 0.0 } },
    { "s_pn_noise_a", { 1.0, 0.0 } },
    { "s_pn_index_a", { 61.0 / 64.0, 0.0123 } },
    { "s_sweep_3_b", { 0.985, 0.0022 } },
    { "s_sweep_5_b", { 0.917, 0.0127 } },
    { "s_pw_5_b_40", { 62.0 / 64.0, 0.0042 } },
    { "s_pw_5_b_a0", { 53.0 / 64.0, 0.0247 } },
    { "s_sweep_6_b", { 0.922, 0.0108 } },
    { "s_pw_6_b_40", { 62.0 / 64.0, 0.0042 } },
    { "s_pw_6_b_a0", { 63.0 / 64.0, 0.0021 } },
    { "s_sweep_7_b", { 0.978, 0.0029 } },
    { "s_pw_7_b_40", { 1.0, 0.0 } },
    { "s_pw_7_b_a0", { 1.0, 0.0 } },
    { "s_noise_b_b", { 1.0, 0.0 } },
    { "s_noise_c_b", { 44.0 / 48.0, 0.0209 } },
    { "s_noise_d_b", { 46.0 / 48.0, 0.0295 } },
    { "s_noise_e_b", { 1.0, 0.0 } },
    { "s_noise_f_b", { 1.0, 0.0 } },
    { "s_pn_noise_b", { 1.0, 0.0 } },
    { "s_pn_index_b", { 59.0 / 64.0, 0.0244 } },
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
        CHECK_FALSE ((lp.samples.empty() && lp.latch.empty()));
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
    std::size_t fitted = 0;
    for (const auto& lp : loaded())
    {
        if (lp.spec.kind == shapes::Kind::latch)
            continue; // no shape samples: the latch tests below
        ++fitted;
        const auto& e = errs.at (lp.spec.name);
        REQUIRE (e.samples > 0);
        const double bits = e.bitsObserved == 0 ? 0.0 : static_cast<double> (e.bitsWrong) / static_cast<double> (e.bitsObserved);
        std::printf ("[shapes] %-14s %5zu samples, %5zu exact (%6.2f%%), observed bits wrong %.3f%%, max %3d, rms %6.2f (rd1B steps)\n",
                     lp.spec.name.c_str(), e.samples, e.exact, 100.0 * e.exactRate(), 100.0 * bits, e.maxError, e.rms());
        INFO (lp.spec.name);
        REQUIRE (kTolerance.count (lp.spec.name) == 1);
        const auto& t = kTolerance.at (lp.spec.name);
        CHECK (e.exactRate() >= t.minExact);
        CHECK (bits <= t.maxBits);
    }
    CHECK (kTolerance.size() == fitted);
}

namespace
{
/** rd1B of our chip (fitted tables) at every checkpoint of a committed trace, in schedule order. */
std::vector<std::uint8_t> chipReads (const std::string& name)
{
    const auto tr = trace::readTrace (shapes::slurp (kShapes / (name + ".bstrace")));
    REQUIRE (tr.ok);
    std::vector<trace::Checkpoint> schedule;
    REQUIRE (trace::checkpointSchedule (tr.trace, schedule));
    // stepCycle is normative; a second chip on advanceCycles must equal it at every checkpoint.
    chip::Chip c, fast;
    trace::setupChip (c, tr.trace);
    trace::setupChip (fast, tr.trace);
    std::vector<std::uint8_t> reads;
    std::size_t fastDiffs = 0;
    for (const auto& cp : schedule)
    {
        const std::uint64_t to = cp.kind == trace::Checkpoint::Kind::afterWrite ? tr.trace.writes[cp.write].cycle : cp.cycle;
        while (c.cycle() < to)
            c.stepCycle();
        fast.advanceCycles (to - fast.cycle());
        if (cp.kind == trace::Checkpoint::Kind::afterWrite)
        {
            const auto& w = tr.trace.writes[cp.write];
            c.write (w.reg, w.value);
            fast.write (w.reg, w.value);
        }
        fastDiffs += fast.state() == c.state() ? 0u : 1u;
        reads.push_back (c.read (chip::reg::osc3Output));
    }
    CHECK (fastDiffs == 0);
    return reads;
}

const shapes::LoadedPlan& planNamed (const std::string& name)
{
    for (const auto& lp : loaded())
        if (lp.spec.name == name)
            return lp;
    FAIL ("no plan " << name);
    return loaded().front();
}
} // namespace

TEST_CASE ("shape data: the Model B read latch across waveform changes (8.15.2 (c) step 5)", "[trace][shapes]")
{
    // s_latch_b: triangle + saw (3) at frequency 0x1000; at index X a switch for one step to saw + pulse
    // (event 1), to pulse alone and then saw + pulse (event 2) or to saw alone (event 3). The measured
    // shapes T3 and T6 come from the sweeps. Findings, pinned so a regeneration that changes them fails:
    //   event 1: 30 of 40 reads show T3(X) (the latched shape), 1 shows T6(X) (a latched index), 9 neither;
    //   event 2: pulse alone reads 0xff (its own output) and leaves the latch: 30 of 40 then show T3(X);
    //   event 3: all 40 reads show saw(X) AND (saw(X) << 1), neither T3(X) (5 equal it only where the two
    //            values coincide) nor the saw at X.
    // Neither wording of step 5 covers events 1 to 3 fully; reported to the contract owner.
    std::map<int, int> t3, t6;
    const shapes::LoadedPlan* latch = nullptr;
    for (const auto& lp : loaded())
    {
        if (lp.spec.name == "s_latch_b")
            latch = &lp;
        if (lp.spec.model == chip::Model::B && lp.spec.kind == shapes::Kind::sweep && (lp.spec.wave == 3 || lp.spec.wave == 6))
            for (const auto& s : lp.samples)
                (lp.spec.wave == 3 ? t3 : t6)[s.index] = s.value;
    }
    REQUIRE (latch != nullptr);
    REQUIRE (latch->latch.size() == 120);
    int shape1 = 0, index1 = 0, pulseOwn = 0, shape2 = 0, andShift3 = 0, shape3 = 0;
    for (const auto& e : latch->latch)
    {
        const int x = e.index;
        if (e.event == 1)
        {
            shape1 += e.read1 == t3.at (x) ? 1 : 0;
            index1 += e.read1 != t3.at (x) && e.read1 == t6.at (x) ? 1 : 0;
        }
        else if (e.event == 2)
        {
            pulseOwn += e.read1 == 0xff ? 1 : 0;
            shape2 += e.read2 == t3.at (x) ? 1 : 0;
        }
        else
        {
            andShift3 += e.read1 == (((x & (x << 1)) >> 4) & 0xff) ? 1 : 0;
            shape3 += e.read1 == t3.at (x) ? 1 : 0;
        }
    }
    std::printf ("[latch] event 1: shape %d, index %d of 40; event 2: pulse reads 0xff %d, shape %d of 40; event 3: saw AND saw<<1 %d, shape %d of 40\n",
                 shape1, index1, pulseOwn, shape2, andShift3, shape3);
    CHECK (shape1 == 30);
    CHECK (index1 == 1);
    CHECK (pulseOwn == 40);
    CHECK (shape2 == 30);
    CHECK (andShift3 == 40);
    CHECK (shape3 == 5);

    // Our chip on the same trace (fitted tables, shape latch): per switch kind, how many of the reads that
    // follow it equal the reference. An honest count: low where the data contradict the shape latch
    // (event 3) and where our fitted T3 or T6 differ from the measured shapes.
    const auto ours = chipReads ("s_latch_b");
    REQUIRE (ours.size() > latch->latch.back().read1Point);
    int chip1 = 0, chip2a = 0, chip2b = 0, chip3 = 0;
    for (const auto& e : latch->latch)
    {
        const bool r1 = ours[e.read1Point] == e.read1;
        if (e.event == 1)
            chip1 += r1 ? 1 : 0;
        else if (e.event == 2)
        {
            chip2a += r1 ? 1 : 0;
            chip2b += ours[e.read2Point] == e.read2 ? 1 : 0;
        }
        else
            chip3 += r1 ? 1 : 0;
    }
    std::printf ("[latch] our chip reproduces: event 1 %d, event 2 pulse step %d and return %d, event 3 %d (of 40 each)\n", chip1, chip2a,
                 chip2b, chip3);
    CHECK (chip1 == 29);
    CHECK (chip2a == 40);
    CHECK (chip2b == 31);
    CHECK (chip3 == 5);
}

TEST_CASE ("shape data: pulse with noise leaves the Model B read latch alone (our chip against s_latch_pn_b)", "[trace][shapes]")
{
    // s_latch_pn_b: triangle + saw (3) at frequency 0x1000; at index X one step (event 4) or two steps
    // (event 5) of pulse with noise, then triangle + saw again; one step later a probe shows the read of
    // that return step: the latch. Pulse with noise has no triangle or saw bit, so the latch must still
    // hold the triangle + saw shape latched at X. The reference: every return read equals the measured
    // T3(X). Our chip must reproduce every return read where its fitted T3 agrees with the measured one,
    // and give exactly its own T3(X) at all of them (a chip whose pulse-with-noise step touched the latch
    // fails both).
    std::map<int, int> t3;
    for (const auto& s : planNamed ("s_sweep_3_b").samples)
        t3[s.index] = s.value;
    const auto& lp = planNamed ("s_latch_pn_b");
    INFO (lp.error);
    REQUIRE (lp.error.empty());
    REQUIRE (lp.latch.size() == 60);
    const auto ours = chipReads ("s_latch_pn_b");
    const auto& table = chip::combined::fittedTables (chip::Model::B);
    int refLatched = 0, chipAgrees = 0, chipOwnShape = 0, fitAgrees = 0;
    for (const auto& e : lp.latch)
    {
        const auto fittedT3 = static_cast<int> (table.wave[3][e.index] >> 4);
        refLatched += e.read2 == t3.at (e.index) ? 1 : 0;
        fitAgrees += fittedT3 == t3.at (e.index) ? 1 : 0;
        chipAgrees += ours[e.read2Point] == e.read2 ? 1 : 0;
        chipOwnShape += ours[e.read2Point] == fittedT3 ? 1 : 0;
    }
    std::printf ("[latch] pulse with noise: reference keeps the latch %d of 60; our fitted T3 equals the measured %d; our chip reproduces %d, "
                 "reads its own T3 %d\n",
                 refLatched, fitAgrees, chipAgrees, chipOwnShape);
    CHECK (refLatched == 60);
    CHECK (chipOwnShape == 60);
    CHECK (chipAgrees == fitAgrees);
}
