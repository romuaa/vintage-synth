// The independent per-cycle model (IndependentModel.h, written from contract 8.15 alone by a reviewer)
// against the committed reference data and against the chip (docs/architecture.md 8.9.1, 8.15).
// Like IndependentModel.h, this file is not edited by whoever edits source/bside/chip/.

#include "IndependentModel.h"

#include "bside/chip/Chip.h"
#include "bside/trace/ReferenceFormat.h"
#include "bside/trace/TraceFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace vs::bside;
namespace im = vs::bside::independent;

namespace
{
/** tests/bside/reference, found from this file's own path (bside_unit has no data-directory define). */
fs::path referenceDir() { return fs::path (__FILE__).parent_path().parent_path() / "reference"; }

std::string slurp (const fs::path& p)
{
    std::ifstream f (p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

struct ReplayOutcome
{
    bool readOk = false;
    std::size_t checkpoints = 0, mismatched = 0;
    std::string first; // the first difference, for the failure message
};

/** Replays `name` with the model; every field of every checkpoint is compared exactly. */
ReplayOutcome replayModel (const std::string& name, bool modelA, im::Faults faults = {})
{
    ReplayOutcome r;
    const auto traceText = slurp (referenceDir() / (name + ".bstrace"));
    const auto tr = trace::readTrace (traceText);
    const auto rr = trace::readReference (slurp (referenceDir() / (name + ".bsref")));
    std::vector<trace::Checkpoint> schedule;
    if (! tr.ok || ! rr.ok || ! trace::checkpointSchedule (tr.trace, schedule) || schedule.size() != rr.reference.points.size())
        return r;
    r.readOk = true;
    im::Model m;
    m.reset (modelA);
    m.faults = faults;
    std::uint64_t cycle = 0;
    for (std::size_t i = 0; i < schedule.size(); ++i)
    {
        const auto& cp = schedule[i];
        const std::uint64_t at = cp.kind == trace::Checkpoint::Kind::afterWrite ? tr.trace.writes[cp.write].cycle : cp.cycle;
        for (; cycle < at; ++cycle)
            m.step();
        if (cp.kind == trace::Checkpoint::Kind::afterWrite)
        {
            const auto& w = tr.trace.writes[cp.write];
            m.write (w.reg & 0x1f, w.value);
        }
        std::uint32_t f[20] {};
        m.fields (f);
        ++r.checkpoints;
        bool differs = false;
        for (std::size_t k = 0; k < 20; ++k)
            if (f[k] != rr.reference.points[i].state.v[k])
            {
                if (r.first.empty())
                    r.first = "checkpoint " + std::to_string (i) + " cycle " + std::to_string (cp.cycle) + " field "
                              + std::string (trace::kStateFieldNames[k]) + " model " + std::to_string (f[k]) + " reference "
                              + std::to_string (rr.reference.points[i].state.v[k]);
                differs = true;
            }
        if (differs)
            ++r.mismatched;
    }
    return r;
}

/** The committed traces the model replays exactly: d_*, a_*, and the no-table combined traces c_9_*, c_a_*. */
std::vector<std::string> exactTraces()
{
    std::vector<std::string> names;
    if (! fs::is_directory (referenceDir()))
        return names;
    for (const auto& e : fs::directory_iterator (referenceDir()))
    {
        if (e.path().extension() != ".bstrace")
            continue;
        const auto n = e.path().stem().string();
        if (n.rfind ("d_", 0) == 0 || n.rfind ("a_", 0) == 0 || n.rfind ("c_9_", 0) == 0 || n.rfind ("c_a_", 0) == 0)
            names.push_back (n);
    }
    std::sort (names.begin(), names.end());
    return names;
}

bool traceIsModelA (const std::string& name)
{
    const auto tr = trace::readTrace (slurp (referenceDir() / (name + ".bstrace")));
    return tr.ok && tr.trace.model == trace::Model::A;
}

/** Compares every state field the model and the chip both hold. */
bool sameState (const chip::Chip& c, const im::Model& m, std::string& why)
{
    const chip::DigitalState s = c.state();
    for (std::size_t n = 0; n < 3; ++n)
    {
        const auto& o = s.osc[n];
        const auto& x = m.o[n];
        const auto& e = s.env[n];
        const auto& y = m.e[n];
        const int pp = y.periodPending < 0 ? 0 : y.periodPending;
        const bool ok = o.acc == x.acc && o.noise == x.reg && o.noiseLatch == x.latch && o.noiseOutput == x.noiseOut
                        && o.noisePipeline == x.noisePipe && o.firstShift == x.firstShift && o.rise == x.rise && o.fillTimer == x.fill
                        && o.fadeTimer == x.fade && o.output == x.out && o.readValue == x.rd && o.readLatch == x.btLatch
                        && o.pulseHigh == x.cmp && e.counter == y.counter && e.phase == y.phase && e.target == y.target
                        && e.phasePipeline == y.phasePipe && e.stepPipeline == y.stepPipe && e.dividerPipeline == y.divPipe
                        && e.position == y.pos && e.compare == y.cmpPos && e.resetPending == y.pending && e.divider == y.divCnt
                        && e.dividerPeriod == y.period && e.pendingPeriod == pp && e.frozen == y.frozen && e.readSample == y.readSample;
        if (! ok)
        {
            why = "oscillator/envelope " + std::to_string (n + 1) + " differs at cycle " + std::to_string (s.cycle);
            return false;
        }
    }
    return true;
}

/** Random writes at random cycles (many inside pipelines, some long gaps for fill, fade and the delay
    quirk); the model, chip.stepCycle and chip.advanceCycles (random chunks) are compared in full after
    every gap. Returns the cycles run; `why` is set at the first difference. */
std::uint64_t modelAgainstChip (chip::Model model, std::uint64_t cycles, std::uint64_t seed, std::string& why)
{
    std::mt19937_64 rng (seed);
    auto R = [&rng] (std::uint32_t n) { return static_cast<std::uint32_t> (rng() % n); };
    chip::Config cfg;
    cfg.model = model;
    cfg.authentic = true;
    chip::Chip stepped, fast;
    stepped.prepare (cfg);
    fast.prepare (cfg);
    im::Model m;
    m.reset (model == chip::Model::A);
    std::uint64_t cyc = 0;
    while (cyc < cycles)
    {
        const auto g = R (1000);
        const std::uint64_t gap = g < 400 ? R (4) : g < 850 ? R (300) : g < 995 ? R (20000) : R (1200000);
        for (std::uint64_t left = gap; left > 0;)
        {
            const std::uint64_t ch = std::min<std::uint64_t> (left, 1 + R (R (2) != 0 ? 5000 : 7));
            fast.advanceCycles (ch);
            left -= ch;
        }
        for (std::uint64_t k = 0; k < gap; ++k)
        {
            stepped.stepCycle();
            m.step();
        }
        cyc += gap;
        if (! sameState (stepped, m, why))
            return cyc;
        if (! (fast.state() == stepped.state()))
        {
            why = "advanceCycles differs from stepCycle at cycle " + std::to_string (cyc);
            return cyc;
        }
        const int writes = 1 + static_cast<int> (R (3));
        for (int k = 0; k < writes; ++k)
        {
            std::uint8_t r;
            const auto p = R (100);
            if (p < 45)
                r = static_cast<std::uint8_t> (7 * R (3) + 4);
            else if (p < 65)
                r = static_cast<std::uint8_t> (7 * R (3) + (R (2) != 0 ? 5 : 6));
            else if (p < 90)
                r = static_cast<std::uint8_t> (7 * R (3) + R (4));
            else
                r = static_cast<std::uint8_t> (R (0x20));
            auto v = static_cast<std::uint8_t> (R (256));
            if (r < 21 && r % 7 == 4 && R (4) != 0)
                v = static_cast<std::uint8_t> (v & ~0x08u); // the test bit in a quarter of the control writes
            if (r < 21 && r % 7 == 1 && R (3) == 0)
                v = static_cast<std::uint8_t> (R (2) != 0 ? 0xff : R (4));
            stepped.write (r, v);
            fast.write (r, v);
            m.write (r & 0x1f, v);
        }
    }
    return cyc;
}
} // namespace

TEST_CASE ("independent model: replays every committed d_*, a_* and no-table c_9_*/c_a_* trace exactly", "[bside][chip][independent]")
{
    const auto names = exactTraces();
    REQUIRE (names.size() >= 50);
    std::size_t checkpoints = 0;
    for (const auto& name : names)
    {
        // The envelope traces set no waveform bit and hold for both models (docs/bside-oracle-adapter.md).
        std::vector<bool> models { traceIsModelA (name) };
        if (name.rfind ("d_env_", 0) == 0)
            models.push_back (! models.front());
        for (const bool a : models)
        {
            const auto r = replayModel (name, a);
            INFO (name << " as model " << (a ? "A" : "B") << ": " << r.first);
            REQUIRE (r.readOk);
            CHECK (r.checkpoints > 0);
            CHECK (r.mismatched == 0);
            checkpoints += r.checkpoints;
        }
    }
    CHECK (checkpoints > 20000);
}

TEST_CASE ("independent model: a deliberate rule change makes the replay fail", "[bside][chip][independent]")
{
    // The model can fail: ring with XOR instead of XOR NOT (8.15.2 (c) step 1) must break the sync/ring traces.
    im::Faults wrong;
    wrong.ringWithoutNot = true;
    std::size_t failing = 0;
    for (const auto& name : { std::string ("d_sync_ring_a"), std::string ("d_sync_ring_b") })
    {
        const auto good = replayModel (name, traceIsModelA (name));
        const auto bad = replayModel (name, traceIsModelA (name), wrong);
        REQUIRE (good.readOk);
        REQUIRE (bad.readOk);
        CHECK (good.mismatched == 0);
        if (bad.mismatched > 0)
            ++failing;
    }
    CHECK (failing == 2);
}

TEST_CASE ("independent model: equals the chip (stepCycle and advanceCycles) in every state field, 2 x 10^7 random cycles per model", "[bside][chip][independent]")
{
    for (const auto model : { chip::Model::A, chip::Model::B })
    {
        std::string why;
        const auto ran = modelAgainstChip (model, 20000000, model == chip::Model::A ? 11u : 12u, why);
        INFO ((model == chip::Model::A ? "model A: " : "model B: ") << why);
        CHECK (why.empty());
        CHECK (ran >= 20000000);
    }
}

TEST_CASE ("independent model: equals the chip, long run (10^8 cycles per model and seed)", "[.independent-long]")
{
    for (const std::uint64_t seed : { 21u, 22u, 23u })
        for (const auto model : { chip::Model::A, chip::Model::B })
        {
            std::string why;
            modelAgainstChip (model, 100000000, seed * 2 + (model == chip::Model::A ? 0u : 1u), why);
            INFO ("seed " << seed << (model == chip::Model::A ? " model A: " : " model B: ") << why);
            CHECK (why.empty());
        }
}
