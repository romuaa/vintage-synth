// Golden write traces (docs/architecture.md 8.9.6): the program engine's register writes for canonical tables, in the
// trace format, compared as exact text with tests/bside/golden/*.bstrace.
//
// Kinds (also in the first line of every file):
//   hand-derived  snare, arpeggio, pulselead, filterbass at tick.mult 1: written by hand from 8.6 (pitch values with
//                 a calculator) before the engine existed; the independent model reproduced them line by line.
//                 Their m2 and m4 files are the model's output with every cycle divided by the multiplier (checked
//                 equal to the hand file scaled).
//   model         kick, hihat, the restart matrix (3 modes x 3 policies) and the firstwave cases: derived by the
//                 independent model tests/bside/golden/model.py (an interpreter of the table text written
//                 separately from the engine), reviewed by hand. gen.py regenerates every file.
// The scenarios below repeat gen.py's: a divergence of either side fails the comparison.

#include "EngineTestSupport.h"

#include "bside/chip/Chip.h"
#include "bside/engine/ChipProgramSink.h"

#ifndef BS_GOLDEN_DIR
 #error "BS_GOLDEN_DIR must point at tests/bside/golden"
#endif

using namespace bst;
namespace pe = vs::bside::engine;
using vs::bside::engine::RestartPolicy;

namespace
{
const char* kKick = R"(bside-table 1
[restart]
mode phase
lead 2
[wave1]
    adsr 0 8 0 8, wave tri+gate =C3
    pitch =F2
    pitch =C2
    pitch =G1 ticks 3
    wave tri =G1
    end
[filter]
    mode lp, cutoff 600, route 1
    sweep -100 x4
    end
)";
const char* kHihat = R"(bside-table 1
[restart]
mode classic
lead 1
[wave1]
    adsr 0 3 0 3, wave noise+gate =F#7
    wave noise+gate =C8 ticks 2
    gate off
    end
[filter]
    mode hp, res 4, cutoff 1500, route 1
    end
)";

std::string matrixTable (const char* mode, const char* extra)
{
    return std::string ("bside-table 1\n[restart]\nmode ") + mode + "\nlead 2\n" + extra + "[wave1]\n    wave pulse+gate =C4\n    pitch =G4\n    end\n";
}

pe::HostInputs hostWithSaw()
{
    auto h = defaultHost();
    h.osc[1].enabled = true;
    h.osc[1].waveBits = 0x20;
    h.osc[1].semitones = 59.88;
    return h;
}

struct Golden
{
    std::string name;
    std::string table;
    pe::HostInputs host;
    int mult;
    Scenario scenario;
};

constexpr std::uint64_t kMults[3] = { 1, 2, 4 };

Scenario base (const std::string& table, const pe::HostInputs& host, std::uint64_t mult)
{
    Scenario s;
    s.program = compileText (table);
    s.host = host;
    s.period = static_cast<std::uint32_t> (kFrame / mult);
    return s;
}

std::vector<Golden> goldens()
{
    std::vector<Golden> g;
    const auto add = [&] (const std::string& name, Scenario s, std::uint64_t mult) {
        g.push_back ({ name, {}, {}, static_cast<int> (mult), std::move (s) });
    };
    for (const auto m : kMults)
    {
        const std::uint64_t P = kFrame / m;
        {
            auto s = base (kSnare, defaultHost(), m);
            s.events = { noteOn (0, RestartPolicy::clean), noteOff (44312 / m) };
            s.endCycle = 9 * P - 1;
            add ("snare", s, m);
        }
        {
            auto s = base (kArpeggio, defaultHost(), m);
            s.events = { noteOn (0, RestartPolicy::clean), noteOff (44312 / m) };
            s.endCycle = 5 * P - 1;
            add ("arpeggio", s, m);
        }
        {
            auto s = base (kPulseLead, hostWithSaw(), m);
            s.events = { noteOn (0, RestartPolicy::precondition), noteOff (157348 / m) };
            s.endCycle = 15 * P - 1;
            add ("pulselead", s, m);
        }
        {
            auto s = base (kFilterBass, defaultHost(), m);
            s.events = { noteOn (0, RestartPolicy::authentic) };
            s.endCycle = 18 * P - 1;
            add ("filterbass", s, m);
        }
        {
            auto s = base (kKick, defaultHost(), m);
            s.events = { noteOn (0, RestartPolicy::clean), noteOff (2 * P + P / 2) };
            s.endCycle = 10 * P - 1;
            add ("kick", s, m);
        }
        {
            auto s = base (kHihat, defaultHost(), m);
            s.events = { noteOn (0, RestartPolicy::precondition), noteOff (P + P / 4) };
            s.endCycle = 6 * P - 1;
            add ("hihat", s, m);
        }
    }
    const auto matrixEvents = [] (RestartPolicy p) {
        const std::uint64_t P = kFrame;
        return std::vector<Event> { idleAt (0, p),           noteOn (3 * P, p),     noteOff (4 * P + 1000), idleAt (6 * P, p),
                                    noteOn (7 * P, p),       noteOn (9 * P, p),     noteOn (11 * P, p, true) };
    };
    const struct { const char* name; RestartPolicy p; } policies[] = { { "clean", RestartPolicy::clean },
                                                                       { "precondition", RestartPolicy::precondition },
                                                                       { "authentic", RestartPolicy::authentic } };
    for (const char* mode : { "off", "classic", "phase" })
        for (const auto& pol : policies)
        {
            auto s = base (matrixTable (mode, ""), defaultHost(), 1);
            s.events = matrixEvents (pol.p);
            s.endCycle = 13 * kFrame - 1;
            add (std::string ("restart_") + mode + "_" + pol.name, s, 1);
        }
    const struct { const char* mode; const char* policy; RestartPolicy p; } firstwave[] = {
        { "classic", "clean", RestartPolicy::clean }, { "phase", "precondition", RestartPolicy::precondition }, { "phase", "authentic", RestartPolicy::authentic } };
    for (const auto& f : firstwave)
    {
        auto s = base (matrixTable (f.mode, "firstwave tri\n"), defaultHost(), 1);
        s.events = matrixEvents (f.p);
        s.endCycle = 13 * kFrame - 1;
        add (std::string ("firstwave_") + f.mode + "_" + f.policy, s, 1);
    }
    return g;
}

std::string goldenPath (const Golden& g)
{
    return std::string (BS_GOLDEN_DIR) + "/" + g.name + "_m" + std::to_string (g.mult) + ".bstrace";
}

bool hasReset (const pe::RegisterRecorder& r)
{
    for (const auto& e : r.log())
        if (e.kind == pe::RegisterRecorder::Kind::resetChip)
            return true;
    return false;
}

/** The text the golden file must contain: its own first (comment) line, then the recorder's trace text. */
std::string expectedText (const Golden& g, const pe::RegisterRecorder& rec)
{
    const std::string file = readFile (goldenPath (g));
    const auto nl = file.find ('\n');
    REQUIRE (nl != std::string::npos);
    const std::string head = file.substr (0, nl + 1);
    CHECK (head.rfind ("# golden " + g.name + ", tick.mult " + std::to_string (g.mult) + ", Authentic, 50 Hz system, ", 0) == 0);
    return head + rec.toText (kClock50, g.scenario.endCycle);
}
} // namespace

TEST_CASE ("golden traces: every file equals the engine's write trace exactly", "[bside][engine][golden]")
{
    const auto all = goldens();
    REQUIRE (all.size() == 18 + 9 + 3);
    for (const auto& g : all)
    {
        INFO (g.name << " mult " << g.mult);
        const auto rec = play (g.scenario);
        const std::string file = readFile (goldenPath (g));
        REQUIRE_FALSE (file.empty());
        CHECK (expectedText (g, rec) == file);
        // The golden file is itself a valid trace the replay tools read.
        const auto parsed = vs::bside::trace::readTrace (file);
        CHECK (parsed.ok);
    }
}

TEST_CASE ("golden traces: the same writes at the same cycles for any block split", "[bside][engine][golden]")
{
    for (const auto& g : goldens())
    {
        INFO (g.name << " mult " << g.mult);
        const auto reference = play (g.scenario).toText (kClock50, g.scenario.endCycle);
        for (const std::uint64_t chunk : std::initializer_list<std::uint64_t> { 7, 777, 4096, g.scenario.period, std::uint64_t { g.scenario.period } + 1 })
            CHECK (play (g.scenario, chunk).toText (kClock50, g.scenario.endCycle) == reference);
    }
}

TEST_CASE ("golden traces: the live chip and the replayed trace reach the same digital state", "[bside][engine][golden]")
{
    // The same traces are replay inputs (8.9.6): the engine writing to the chip as it runs and a chip fed the trace's
    // writes at their cycles must agree. Scenarios with a chip reset are skipped: a reset also restarts the chip's cycle
    // count, which a plain trace cannot say.
    for (const auto& g : goldens())
    {
        const auto rec = play (g.scenario);
        if (hasReset (rec))
            continue;
        INFO (g.name << " mult " << g.mult);
        vs::bside::chip::Config cfg;
        cfg.model = vs::bside::chip::Model::B;
        cfg.clockHz = kClock50;
        cfg.authentic = true;

        vs::bside::chip::Chip live;
        live.prepare (cfg);
        {
            pe::ChipProgramSink sink (live, true);
            pe::ProgramEngine e;
            e.prepare ({ kClock50, true });
            e.setTickPeriod (g.scenario.period);
            e.setHost (g.scenario.host);
            const auto runTo = [&] (std::uint64_t cycle) {
                while (e.isRunning() && e.nextTickCycle() <= cycle)
                {
                    live.advanceCycles (e.nextTickCycle() - live.cycle());
                    e.runTick (sink);
                }
                if (cycle > live.cycle())
                    live.advanceCycles (cycle - live.cycle());
            };
            for (const auto& ev : g.scenario.events)
            {
                runTo (ev.cycle);
                if (ev.kind == Event::Kind::noteOn) e.noteOn (ev.cycle, g.scenario.program, ev.policy, ev.stolen, sink);
                else if (ev.kind == Event::Kind::noteOff) e.noteOff (ev.cycle, sink);
                else if (ev.kind == Event::Kind::idle) e.enterIdle (ev.cycle, g.scenario.program.restart, ev.policy, sink);
            }
            runTo (g.scenario.endCycle);
        }
        vs::bside::chip::Chip replayed;
        replayed.prepare (cfg);
        for (const auto& w : rec.log())
        {
            if (w.kind != pe::RegisterRecorder::Kind::write)
                continue;
            if (w.cycle > replayed.cycle())
                replayed.advanceCycles (w.cycle - replayed.cycle());
            replayed.write (w.reg, w.value);
        }
        if (g.scenario.endCycle > replayed.cycle())
            replayed.advanceCycles (g.scenario.endCycle - replayed.cycle());
        CHECK (live.cycle() == replayed.cycle());
        CHECK (live.state() == replayed.state());
    }
}
