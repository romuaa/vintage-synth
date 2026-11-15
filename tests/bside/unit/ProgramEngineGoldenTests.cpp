// Golden write traces (docs/architecture.md 8.9.6): the program engine's register writes for canonical tables, in the
// trace format, compared as exact text with tests/bside/golden/*.bstrace.
//
// Kinds (also in the first line of every file):
//   hand-derived  snare, arpeggio, pulselead, filterbass at tick.mult 1: values typed from the contract (pitch
//                 values with a calculator), the three decision changes (gate after note-off, Authentic without the
//                 restart section) applied by hand. They are never overwritten by tools/gen.py, which only checks
//                 that the independent model reproduces them.
//   model-derived every other file (all tick.mult 1, 2, 4): tools/model.py (an interpreter of the table text written
//                 separately from the engine) through tools/gen.py, reviewed by hand. The m2 and m4 files of the four
//                 hand-derived tables are model-derived too.
// The scenarios below repeat gen.py's: a divergence of either side fails the comparison.

#include "EngineTestSupport.h"

#include <algorithm>

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

const char* kPing = R"(bside-table 1
[pulse1]
    set 100
    pingpong 100 130 10
)";
const char* kRepeat = R"(bside-table 1
[wave1]
top:
    pitch =C4
    repeat top 2
    pitch =A3
    jump top
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
    for (const auto m : kMults)
    {
        const std::uint64_t P = kFrame / m;
        {
            auto s2 = base (kPing, defaultHost(), m);
            s2.events = { noteOn (0, RestartPolicy::authentic) };
            s2.endCycle = 12 * P - 1;
            add ("pingpong", s2, m);
            auto h = defaultHost();
            h.macro.sweepDepth = 0.5f;
            auto s3 = base (kPing, h, m);
            s3.events = { noteOn (0, RestartPolicy::authentic) };
            s3.endCycle = 12 * P - 1;
            add ("pingpong_depth", s3, m);
            auto s4 = base (kRepeat, defaultHost(), m);
            s4.events = { noteOn (0, RestartPolicy::authentic) };
            s4.endCycle = 9 * P - 1;
            add ("repeat", s4, m);
        }
        const auto matrixEvents = [&] (RestartPolicy p) {
            return std::vector<Event> { idleAt (0, p),     noteOn (3 * P, p),  noteOff (4 * P + 1000 / m), idleAt (6 * P, p),
                                        noteOn (7 * P, p), noteOn (9 * P, p),  noteOn (11 * P, p, true) };
        };
        const struct { const char* name; RestartPolicy p; } policies[] = { { "clean", RestartPolicy::clean },
                                                                           { "precondition", RestartPolicy::precondition },
                                                                           { "authentic", RestartPolicy::authentic } };
        for (const char* mode : { "off", "classic", "phase" })
            for (const auto& pol : policies)
            {
                auto s2 = base (matrixTable (mode, ""), defaultHost(), m);
                s2.events = matrixEvents (pol.p);
                s2.endCycle = 13 * P - 1;
                add (std::string ("restart_") + mode + "_" + pol.name, s2, m);
            }
        for (const char* mode : { "classic", "phase" })
            for (const auto& pol : policies)
            {
                auto s2 = base (matrixTable (mode, "firstwave tri\n"), defaultHost(), m);
                s2.events = matrixEvents (pol.p);
                s2.endCycle = 13 * P - 1;
                add (std::string ("firstwave_") + mode + "_" + pol.name, s2, m);
            }
    }
    return g;
}

std::string goldenPath (const Golden& g)
{
    return std::string (BS_GOLDEN_DIR) + "/" + g.name + "_m" + std::to_string (g.mult) + ".bstrace";
}

/** The text the golden file must contain: its own first (comment) line, then the recorder's trace text. */
std::string expectedText (const Golden& g, const pe::RegisterRecorder& rec)
{
    const std::string file = readFile (goldenPath (g));
    const auto nl = file.find ('\n');
    REQUIRE (nl != std::string::npos);
    const std::string head = file.substr (0, nl + 1);
    CHECK (head.rfind ("# golden " + g.name + ", tick.mult " + std::to_string (g.mult) + ", Authentic, 50 Hz system, ", 0) == 0);
    const bool hand = (g.name == "snare" || g.name == "arpeggio" || g.name == "pulselead" || g.name == "filterbass") && g.mult == 1;
    CHECK (head.find (hand ? "hand-derived" : "model-derived") != std::string::npos);
    return head + rec.toText (kClock50, g.scenario.endCycle);
}
} // namespace

TEST_CASE ("golden traces: every file equals the engine's write trace exactly", "[bside][engine][golden]")
{
    const auto all = goldens();
    REQUIRE (all.size() == (6 + 3 + 9 + 6) * 3);
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

namespace
{
using vs::bside::chip::DigitalState;

vs::bside::chip::Config chipConfig()
{
    vs::bside::chip::Config cfg;
    cfg.model = vs::bside::chip::Model::B;
    cfg.clockHz = kClock50;
    cfg.authentic = true;
    return cfg;
}

/** The engine writing to a chip as it runs; the chip's state is taken at every checkpoint (before the writes of that
    cycle) and after the last write. */
std::vector<DigitalState> runLive (const Scenario& s, const std::vector<std::uint64_t>& checkpoints, std::uint64_t& maxAdvance)
{
    std::vector<DigitalState> states;
    vs::bside::chip::Chip live;
    live.prepare (chipConfig());
    pe::ChipProgramSink sink (live, true);
    pe::ProgramEngine e;
    e.prepare ({ kClock50, true });
    e.setTickPeriod (s.period);
    e.setHost (s.host);
    std::size_t next = 0;
    const auto advance = [&] (std::uint64_t to) {
        if (to > sink.cycle())
        {
            maxAdvance = std::max (maxAdvance, to - sink.cycle());
            live.advanceCycles (to - sink.cycle());
        }
    };
    const auto runTo = [&] (std::uint64_t target) {
        for (;;)
        {
            const std::uint64_t tick = e.nextTickCycle();
            const std::uint64_t cp = next < checkpoints.size() ? checkpoints[next] : ~std::uint64_t { 0 };
            const std::uint64_t t = std::min (tick, cp);
            if (t > target)
                break;
            advance (t);
            if (cp <= tick)
            {
                states.push_back (live.state());
                ++next;
            }
            else
                e.runTick (sink);
        }
        advance (target);
    };
    for (const auto& ev : s.events)
    {
        runTo (ev.cycle);
        if (ev.kind == Event::Kind::noteOn) e.noteOn (ev.cycle, s.program, ev.policy, ev.stolen, sink);
        else if (ev.kind == Event::Kind::noteOff) e.noteOff (ev.cycle, sink);
        else if (ev.kind == Event::Kind::idle) e.enterIdle (ev.cycle, s.program.restart, ev.policy, sink);
    }
    runTo (s.endCycle);
    advance (s.endCycle + 1);
    states.push_back (live.state()); // after every write of the scenario
    return states;
}

/** A chip fed the recorded writes at their cycles; the markers of a chip reset and of Clean's direct clear are applied too. */
std::vector<DigitalState> replayRecorded (const pe::RegisterRecorder& rec, const std::vector<std::uint64_t>& checkpoints, std::uint64_t end)
{
    std::vector<DigitalState> states;
    vs::bside::chip::Chip c;
    c.prepare (chipConfig());
    std::size_t i = 0;
    const auto advance = [&] (std::uint64_t to) {
        if (to > c.cycle())
            c.advanceCycles (to - c.cycle());
    };
    const auto apply = [&] (std::uint64_t before) { // every entry with a cycle < before
        for (; i < rec.log().size() && rec.log()[i].cycle < before; ++i)
        {
            const auto& e = rec.log()[i];
            advance (e.cycle);
            if (e.kind == pe::RegisterRecorder::Kind::write)
                c.write (e.reg, e.value);
            else if (e.kind == pe::RegisterRecorder::Kind::resetChip)
                c.powerOnKeepingTime();
            else
                for (int n = 0; n < 3; ++n)
                    c.clearEnvelopeCounters (n); // Clean's direct clear, at its marker
        }
    };
    for (const auto cp : checkpoints)
    {
        apply (cp);
        advance (cp);
        states.push_back (c.state());
    }
    apply (end + 1);
    advance (end + 1);
    states.push_back (c.state());
    return states;
}
} // namespace

TEST_CASE ("golden traces: the live chip and the replayed trace agree at every checkpoint, steals included", "[bside][engine][golden]")
{
    // The same traces are replay inputs (8.9.6): the engine writing to the chip as it runs and a chip fed the trace's
    // writes at their cycles must agree. Checkpoints every 10 007 cycles (before the writes of that cycle) and at
    // the end. The replay applies the recording's reset and direct-clear markers like the live sink does.
    for (const auto& g : goldens())
    {
        INFO (g.name << " mult " << g.mult);
        const auto rec = play (g.scenario);
        std::vector<std::uint64_t> checkpoints;
        for (std::uint64_t c = 10007; c <= g.scenario.endCycle; c += 10007)
            checkpoints.push_back (c);
        std::uint64_t maxAdvance = 0;
        const auto live = runLive (g.scenario, checkpoints, maxAdvance);
        const auto replayed = replayRecorded (rec, checkpoints, g.scenario.endCycle);
        REQUIRE (live.size() == replayed.size());
        for (std::size_t k = 0; k < live.size(); ++k)
            CHECK (live[k] == replayed[k]);
        // the chip never has to render more than the longest wait between two events (a few ticks), not a lifetime
        CHECK (maxAdvance <= 3 * kFrame);
    }
}
