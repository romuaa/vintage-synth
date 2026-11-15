// The patch program engine (docs/architecture.md 8.6): every command, the tick clock, ownership of registers,
// macros, note-off, hard restart under the three policies, resolution. The expected writes of these tests are
// derived by hand from the contract (the arithmetic of the pitch values with a calculator); each test names its
// reading where the contract leaves a choice. "Quiet host": nothing the host says differs from the chip's power-on
// registers, so only what the table writes shows in the log.

#include "EngineTestSupport.h"

#include <cmath>

using namespace bst;
namespace pe = vs::bside::engine;
using vs::bside::engine::RestartPolicy;

namespace
{
constexpr RestartPolicy kAuth = RestartPolicy::authentic;

bool near (double a, double b) { return std::fabs (a - b) < 1e-9; }

pe::HostInputs quietHost()
{
    pe::HostInputs h;
    h.osc[0].enabled = false;
    h.osc[0].sustainRelease = 0;
    h.osc[0].pulseWidth = 0.0f;
    h.osc[1] = h.osc[2] = h.osc[0];
    h.filter.cutoff = 0.0f;
    h.filter.routing = 0;
    h.filter.mode = 0;
    h.filter.volume = 0;
    return h;
}

Scenario scenario (const std::string& body, std::uint64_t endCycle, RestartPolicy policy = kAuth)
{
    Scenario s;
    s.program = compileText ("bside-table 1\n" + body);
    s.host = quietHost();
    s.events = { noteOn (0, policy) };
    s.endCycle = endCycle;
    return s;
}

void expectWrites (const Scenario& s, const std::string& expected)
{
    const auto rec = play (s);
    CHECK (writesText (rec) == expectLines (expected));
    // A block boundary every 777 cycles (host parameters unchanged, `sync` at each) must change nothing.
    CHECK (writesText (play (s, 777)) == writesText (rec));
}

constexpr std::uint64_t T (int k) { return static_cast<std::uint64_t> (k) * kFrame; }

/** Records the values (not the registers) the engine hands over, for the Smooth resolution. */
class ValueSink final : public pe::ProgramSink
{
public:
    double lastFrequency[3] = {}, lastPulse[3] = {}, lastCutoff = -1;
    int frequencyCalls = 0;
    void frequency (std::uint64_t, int osc, double v) noexcept override { lastFrequency[osc] = v; ++frequencyCalls; }
    void pulseWidth (std::uint64_t, int osc, double v) noexcept override { lastPulse[osc] = v; }
    void control (std::uint64_t, int, std::uint8_t) noexcept override {}
    void envelope (std::uint64_t, int, std::uint8_t, std::uint8_t) noexcept override {}
    void cutoff (std::uint64_t, double v) noexcept override { lastCutoff = v; }
    void resonanceRouting (std::uint64_t, std::uint8_t) noexcept override {}
    void modeVolume (std::uint64_t, std::uint8_t) noexcept override {}
    void clearEnvelopeTimers (std::uint64_t) noexcept override {}
    void resetChip (std::uint64_t) noexcept override {}
};

std::vector<pe::RegisterRecorder::Kind> directKinds (const pe::RegisterRecorder& r)
{
    std::vector<pe::RegisterRecorder::Kind> k;
    for (const auto& e : r.log())
        if (e.kind != pe::RegisterRecorder::Kind::write)
            k.push_back (e.kind);
    return k;
}
} // namespace

TEST_CASE ("engine: tick period of every rate (8.6.1)", "[bside][engine]")
{
    pe::TickParams p;
    const auto period = [&] { return pe::tickPeriodCycles (p); };
    SECTION ("Frame: 19656 or 17095 cycles divided by the multiplier, rounded")
    {
        CHECK (period() == 19656);
        p.mult = 2;
        CHECK (period() == 9828);
        p.mult = 4;
        CHECK (period() == 4914);
        p.mult = 8;
        CHECK (period() == 2457);
        p.clockHz = 1022727;
        p.mult = 1;
        CHECK (period() == 17095);
        p.mult = 2;
        CHECK (period() == 8548); // 8547.5 rounds up
        p.mult = 4;
        CHECK (period() == 4274); // 4273.75
        p.mult = 8;
        CHECK (period() == 2137); // 2136.875
    }
    SECTION ("Free: clock / (hz x mult)")
    {
        p.rate = pe::TickRate::free;
        CHECK (period() == 19705); // 985248 / 50 = 19704.96
        p.freeHz = 10.0;
        p.mult = 8;
        CHECK (period() == 12316); // 12315.6
        p.freeHz = 400.0;
        CHECK (period() == 308); // 307.9
    }
    SECTION ("Tempo: clock x 60 / (bpm x sync x mult)")
    {
        p.rate = pe::TickRate::tempo;
        CHECK (period() == 20526); // 985248 / 48
        p.sync = 6;
        p.mult = 2;
        CHECK (period() == 41052); // 985248 / 24
        p.bpm = std::nan ("");
        CHECK (period() == 41052); // not finite: 120 bpm
    }
    SECTION ("at least 256 cycles; out-of-range inputs are clamped, never undefined")
    {
        p.rate = pe::TickRate::free;
        p.clockHz = 1000;
        p.freeHz = 400.0;
        p.mult = 8;
        CHECK (period() == 256);
        p.mult = 0; // clamped to 1
        p.freeHz = 1e9;
        CHECK (period() >= 256);
        p.freeHz = std::nan ("");
        p.clockHz = 985248;
        p.mult = 1;
        CHECK (period() == 19705); // 50 Hz
    }
}

TEST_CASE ("engine: wave lane commands wave, pitch and gate (8.6.3)", "[bside][engine]")
{
    // Pitches are relative to the host's note (60): +12 is note 72 (F = 8910 = 0x22ce), -12 note 48 (2228 = 0x08b4).
    expectWrites (scenario (R"(
[wave1]
    wave tri+gate =C4
    pitch +12
    gate off
    gate on, pitch -12
    wave none ~
    end
)", T (6)),
                  R"(
0 00 67
0 01 11
0 04 11
19656 00 ce
19656 01 22
39312 04 10
58968 04 11
58968 00 b4
58968 01 08
78624 04 00
)");
}

TEST_CASE ("engine: adsr and the control bits of wave", "[bside][engine]")
{
    expectWrites (scenario (R"(
[wave1]
    adsr 2 8 f 4, wave pulse+sync+ring+test+gate =C4
    end
)", T (1)),
                  R"(
0 05 28
0 06 f4
0 00 67
0 01 11
0 04 4f
)");
}

TEST_CASE ("engine: absolute pitch ignores the host; relative follows it (bend)", "[bside][engine]")
{
    auto s = scenario (R"(
[wave1]
    wave pulse+gate +0
    end
[wave2]
    wave pulse+gate =C4
    end
)", T (1));
    auto h = quietHost();
    h.osc[0].semitones = 61.0; // wave1: relative +0 -> 61 -> F = 4720 = 0x1270
    h.osc[1].semitones = 61.0; // wave2: absolute C4: stays 60 = 0x1167
    s.host = h;
    expectWrites (s, R"(
0 00 70
0 01 12
0 04 41
0 07 67
0 08 11
0 0b 41
)");
    // Host pitch changes later: relative follows at the next sync, absolute does not.
    s.events.push_back (hostAt (5000, [&] { auto g = h; g.osc[0].semitones = 62.0; g.osc[1].semitones = 62.0; return g; }()));
    s.events.push_back (syncAt (5000));
    const auto text = writesText (play (s));
    CHECK (text.find ("5000 00 ") != std::string::npos);  // wave1 moved to 62 (0x1337: F = 5000.7 -> 5001 = 0x1389)
    CHECK (text.find ("5000 07 ") == std::string::npos);  // wave2 did not
}

TEST_CASE ("engine: pulse lane set, percentage, sweep with clamping", "[bside][engine]")
{
    expectWrites (scenario (R"(
[pulse1]
    set 4000
    sweep +50 x3
    sweep -4095 x1
    end
[pulse2]
    set 25%
)", T (5)),
                  R"(
0 02 a0
0 03 0f
0 09 00
0 0a 04
19656 02 d2
19656 03 0f
39312 02 ff
39312 03 0f
58968 02 ff
58968 03 0f
78624 02 00
78624 03 00
)");
}

TEST_CASE ("engine: ping-pong reverses at both limits and stops at a release jump", "[bside][engine]")
{
    // 100, 110, 120, 130 (limit, reverse), 120, 110, 100 (limit, reverse), 110 ...
    expectWrites (scenario (R"(
[pulse1]
    set 100
    pingpong 100 130 10
)", T (8) - 1),
                  R"(
0 02 64
0 03 00
19656 02 6e
19656 03 00
39312 02 78
39312 03 00
58968 02 82
58968 03 00
78624 02 78
78624 03 00
98280 02 6e
98280 03 00
117936 02 64
117936 03 00
137592 02 6e
137592 03 00
)");
    // A note-off between ticks 2 and 3 makes the lane jump to `release` (the pulse width 7), ending the ping-pong.
    auto s = scenario (R"(
[pulse1]
    set 100
    pingpong 100 130 10
release:
    set 7
    end
)", T (6));
    s.events.push_back (noteOff (T (2) + 100));
    expectWrites (s, R"(
0 02 64
0 03 00
19656 02 6e
19656 03 00
39312 02 78
39312 03 00
39412 02 07
39412 03 00
)");
}

TEST_CASE ("engine: macros are added after the lane value and clamp; the sweep depth scales the steps", "[bside][engine]")
{
    auto s = scenario (R"(
[pulse1]
    set 100
    sweep +100 x2
    end
)", T (3));
    SECTION ("pulse width offset")
    {
        auto h = quietHost();
        h.macro.pulseWidthOffset = 50.0f;
        s.host = h; // 150, 250, 350
        expectWrites (s, R"(
0 02 96
0 03 00
19656 02 fa
19656 03 00
39312 02 5e
39312 03 01
)");
        h.macro.pulseWidthOffset = -200.0f; // 0, 0 (clamped), 100
        s.host = h;
        expectWrites (s, R"(
0 02 00
0 03 00
19656 02 00
19656 03 00
39312 02 64
39312 03 00
)");
    }
    SECTION ("a macro change reaches a held lane value at the next sync")
    {
        auto h = quietHost();
        h.macro.pulseWidthOffset = 20.0f;
        s.events.push_back (hostAt (T (3) + 10, h));
        s.events.push_back (syncAt (T (3) + 10));
        s.endCycle = T (3) + 10;
        const auto text = writesText (play (s));
        // lane value after the sweep is 300; with the offset 320 = 0x140
        CHECK (text.find ("\n58978 02 40\n58978 03 01\n") != std::string::npos);
    }
    SECTION ("sweep depth 0.5 halves every step")
    {
        auto h = quietHost();
        h.macro.sweepDepth = 0.5f;
        s.host = h; // 100, 150, 200
        expectWrites (s, R"(
0 02 64
0 03 00
19656 02 96
19656 03 00
39312 02 c8
39312 03 00
)");
    }
}

TEST_CASE ("engine: filter lane commands, sweep clamping and the oscillator 3 bit", "[bside][engine]")
{
    expectWrites (scenario (R"(
[filter]
    route 13, mode lp+hp, res 15, volume 9, cutoff 2047
    end
)", T (1)),
                  R"(
0 17 05
0 18 50
0 17 f5
0 18 59
0 15 07
0 16 ff
)");
    expectWrites (scenario (R"(
[filter]
    cutoff 100
    sweep -40 x4
    end
)", T (5)),
                  R"(
0 15 04
0 16 0c
19656 15 04
19656 16 07
39312 15 04
39312 16 02
58968 15 00
58968 16 00
78624 15 00
78624 16 00
)");
    auto s = scenario (R"(
[filter]
    mode bp
    end
)", T (1));
    auto h = quietHost();
    h.filter.osc3Off = true; // the host's bit joins the mode and volume byte
    s.host = h;
    expectWrites (s, R"(
0 18 a0
)");
}

TEST_CASE ("engine: jump, repeat and end", "[bside][engine]")
{
    // repeat top 2: the row runs three times, then falls through; the count restarts on the next pass.
    expectWrites (scenario (R"(
[wave1]
top:
    pitch =C4
    repeat top 2
    pitch =A3
    jump top
)", T (8) - 1),
                  R"(
0 00 67
0 01 11
19656 00 67
19656 01 11
39312 00 67
39312 01 11
58968 00 a2
58968 01 0e
78624 00 67
78624 01 11
98280 00 67
98280 01 11
117936 00 67
117936 01 11
137592 00 a2
137592 01 0e
)");
    // end: nothing more is written by the lane; the lane reports it has stopped.
    pe::RegisterRecorder rec;
    pe::ProgramEngine e;
    e.prepare ({ kClock50, true });
    e.setHost (quietHost());
    e.noteOn (0, compileText ("bside-table 1\n[wave1]\n    pitch =C4\n    end\n"), kAuth, false, rec);
    CHECK (e.anyWaveLaneRunning());
    e.runUntil (T (2), rec);
    CHECK_FALSE (e.anyWaveLaneRunning());
    CHECK (writesText (rec) == expectLines ("0 00 67\n0 01 11\n"));
}

TEST_CASE ("engine: a lane that runs past its last row stops like `end`", "[bside][engine]")
{
    expectWrites (scenario (R"(
[wave1]
    pitch =C4
)", T (4)),
                  R"(
0 00 67
0 01 11
)");
}

TEST_CASE ("engine: rows with delay 0 run in the same tick", "[bside][engine]")
{
    expectWrites (scenario (R"(
[wave1]
    pitch =C4 ticks 0
    pitch =A3 ticks 0
    pitch =F3 ticks 2
    pitch =D6
)", T (4)),
                  R"(
0 00 67
0 01 11
0 00 a2
0 01 0e
0 00 9d
0 01 0b
39312 00 23
39312 01 4e
)");
}

TEST_CASE ("engine: note-off goes to the release label; the tick on the same cycle runs first", "[bside][engine]")
{
    const char* body = R"(
[wave1]
    wave pulse+gate =C4
    pitch =A3
    end
release:
    gate off
    end
)";
    SECTION ("on the cycle of a tick: tick first, then release")
    {
        auto s = scenario (body, 2000);
        s.period = 1000;
        s.events.push_back (noteOff (1000));
        expectWrites (s, R"(
0 00 67
0 01 11
0 04 41
1000 00 a2
1000 01 0e
1000 04 40
)");
    }
    SECTION ("one cycle before a tick: release first, then the tick finds `end`")
    {
        auto s = scenario (body, 2000);
        s.period = 1000;
        s.events.push_back (noteOff (999));
        expectWrites (s, R"(
0 00 67
0 01 11
0 04 41
999 04 40
)");
    }
}

TEST_CASE ("engine: a release label may start a sweep: its first step is at the note-off, the rest on the ticks", "[bside][engine]")
{
    auto s = scenario (R"(
[filter]
    cutoff 800
    end
release:
    sweep -80 x3
    end
)", T (4));
    s.events.push_back (noteOff (T (1) + 10));
    expectWrites (s, R"(
0 15 00
0 16 64
19666 15 00
19666 16 5a
39312 15 00
39312 16 50
58968 15 00
58968 16 46
)");
}

TEST_CASE ("engine: a wave lane without release clears its gate at note-off and keeps running", "[bside][engine]")
{
    auto s = scenario (R"(
[wave1]
    wave pulse+gate =C4
    wave pulse+gate =A3
    end
)", T (3));
    s.events.push_back (noteOff (T (0) + 500));
    // The note-off clears the gate; the next row sets it again (rows keep running: a drum hit plays out).
    expectWrites (s, R"(
0 00 67
0 01 11
0 04 41
500 04 40
19656 00 a2
19656 01 0e
19656 04 41
)");
}

TEST_CASE ("engine: no table: the host drives gate, pitch, envelope and filter", "[bside][engine]")
{
    Scenario s;
    s.program = vs::bside::program::emptyProgram();
    s.host = defaultHost();
    s.events = { noteOn (0, kAuth), noteOff (5000) };
    s.endCycle = T (2);
    // Order inside oscillator 1: frequency, envelope, control (gate last); then pulse width and the filter.
    expectWrites (s, R"(
0 00 67
0 01 11
0 05 00
0 06 f6
0 04 41
0 02 00
0 03 08
0 15 00
0 16 80
0 17 07
0 18 1f
5000 04 40
)");
}

TEST_CASE ("engine: smooth keeps fractions, authentic rounds to the register", "[bside][engine]")
{
    auto h = quietHost();
    h.osc[0].enabled = true;
    h.osc[0].semitones = 69.0; // 7492.5045 -> 7493
    h.macro.pulseWidthOffset = 0.25f;
    h.osc[0].pulseWidth = 100.0f;
    const auto program = vs::bside::program::emptyProgram();
    ValueSink smooth, authentic;
    pe::ProgramEngine a, b;
    a.prepare ({ kClock50, false });
    b.prepare ({ kClock50, true });
    a.setHost (h);
    b.setHost (h);
    a.noteOn (0, program, kAuth, false, smooth);
    b.noteOn (0, program, kAuth, false, authentic);
    CHECK (std::fabs (smooth.lastFrequency[0] - 7492.504465880672) < 1e-6);
    CHECK (near (authentic.lastFrequency[0], 7493.0));
    CHECK (std::fabs (smooth.lastPulse[0] - 100.25) < 1e-6);
    CHECK (near (authentic.lastPulse[0], 100.0));

    SECTION ("limits: Authentic 1 .. 65535; Smooth up to 12 kHz")
    {
        h.osc[0].semitones = -100.0;
        a.setHost (h);
        b.setHost (h);
        a.sync (10, smooth);
        b.sync (10, authentic);
        CHECK (near (authentic.lastFrequency[0], 1.0));
        h.osc[0].semitones = 130.0;
        a.setHost (h);
        b.setHost (h);
        a.sync (20, smooth);
        b.sync (20, authentic);
        CHECK (near (authentic.lastFrequency[0], 65535.0));
        CHECK (smooth.lastFrequency[0] < 204342.0);
        CHECK (smooth.lastFrequency[0] > 204340.0); // 12 kHz x 2^24 / 985248
        h.osc[0].semitones = std::nan ("");
        a.setHost (h);
        a.sync (30, smooth);
        CHECK (std::isfinite (smooth.lastFrequency[0]));
    }
}

TEST_CASE ("engine: sync at an unchanged host writes nothing, at any cycle", "[bside][engine]")
{
    pe::RegisterRecorder rec;
    pe::ProgramEngine e;
    e.prepare ({ kClock50, true });
    e.setHost (defaultHost());
    e.noteOn (0, vs::bside::program::emptyProgram(), kAuth, false, rec);
    const auto n = rec.log().size();
    for (std::uint64_t c : { 1ull, 2ull, 777ull, 19655ull })
        e.sync (c, rec);
    CHECK (rec.log().size() == n);
}

TEST_CASE ("engine: tick clock: tick 0 at the note-on cycle, then every P cycles; a period change applies after the scheduled tick", "[bside][engine]")
{
    pe::RegisterRecorder rec;
    pe::ProgramEngine e;
    e.prepare ({ kClock50, true });
    e.setTickPeriod (1000);
    e.setHost (quietHost());
    CHECK (e.nextTickCycle() == ~std::uint64_t { 0 });
    e.noteOn (5000, compileText ("bside-table 1\n[wave1]\n    pitch =C4\n    pitch =A3\n    pitch =F3\n    pitch =D6\n    end\n"), kAuth, false, rec);
    CHECK (e.tickIndex() == 1);
    CHECK (e.nextTickCycle() == 6000);
    e.runUntil (6000, rec);
    e.setTickPeriod (2000); // tick 2 was scheduled at 7000 by tick 1
    CHECK (e.nextTickCycle() == 7000);
    e.runUntil (7000, rec);
    CHECK (e.nextTickCycle() == 9000);
    e.runUntil (9000, rec);
    CHECK (writesText (rec) == expectLines (R"(
5000 00 67
5000 01 11
6000 00 a2
6000 01 0e
7000 00 9d
7000 01 0b
9000 00 23
9000 01 4e
)"));
    e.setTickPeriod (3); // clamped to 256
    CHECK (e.tickPeriod() == 256);
    e.stop();
    CHECK_FALSE (e.isRunning());
}

TEST_CASE ("engine: a table that bypassed validation cannot hang the engine", "[bside][engine]")
{
    using namespace vs::bside::program;
    Program p = emptyProgram();
    p.lanes[0].firstRow = 0;
    p.lanes[0].numRows = 1;
    p.numRows = 1;
    p.numCmds = 1;
    p.rows[0] = { 0, 1, 0 }; // jump to itself with no delay: a zero-delay loop
    p.cmds[0].op = static_cast<std::uint8_t> (Op::jump);
    REQUIRE_FALSE (validate (p));
    pe::RegisterRecorder rec;
    pe::ProgramEngine e;
    e.prepare ({ kClock50, true });
    e.setHost (quietHost());
    e.noteOn (0, p, kAuth, false, rec);
    e.runUntil (T (3), rec);
    CHECK (e.tickIndex() == 4);
    // only what the host drives at tick 0 (the frequency of the active oscillator); the lane itself wrote nothing
    CHECK (writesText (rec) == expectLines ("0 00 67\n0 01 11\n"));
}

// ---- hard restart (8.6.4) --------------------------------------------------------------------------------

namespace
{
const char* restartTable (const char* mode, const char* extra = "")
{
    static std::string t;
    t = std::string ("\n[restart]\nmode ") + mode + "\nlead 2\n" + extra + "[wave1]\n    wave pulse+gate =C4\n    end\n";
    return t.c_str();
}
const char* kR9 = "05 00\n06 00\n04 00\n0c 00\n0d 00\n0b 00\n13 00\n14 00\n12 00\n";
const char* kR9Phase = "05 00\n06 00\n04 08\n0c 00\n0d 00\n0b 08\n13 00\n14 00\n12 08\n";
const char* kWave1 = "00 67\n01 11\n04 41\n";

std::string at (std::uint64_t cycle, const std::string& regs)
{
    std::istringstream in (regs);
    std::string line, out;
    while (std::getline (in, line))
        out += std::to_string (cycle) + " " + line + "\n";
    return out;
}
} // namespace

TEST_CASE ("engine: restart writes at tick 0 when the voice was not preconditioned", "[bside][engine][restart]")
{
    for (auto policy : { RestartPolicy::authentic, RestartPolicy::precondition, RestartPolicy::clean })
    {
        auto s = scenario (restartTable ("classic"), T (1) - 1, policy);
        const auto rec = play (s);
        // gate off and envelope := ad/sr (00 00) on all three oscillators, per oscillator envelope first, then
        // control; then the wave lane's row 0, at the same cycle
        CHECK (writesText (rec) == at (0, std::string (kR9) + kWave1));
        // Clean: not the full lead, so the rate counters are cleared (first thing at the note's cycle)
        const auto direct = directKinds (rec);
        if (policy == RestartPolicy::clean)
        {
            REQUIRE (direct.size() == 1);
            CHECK (direct[0] == pe::RegisterRecorder::Kind::clearEnvelopeTimers);
            CHECK (rec.log().front().kind == pe::RegisterRecorder::Kind::clearEnvelopeTimers);
        }
        else
            CHECK (direct.empty());
    }
}

TEST_CASE ("engine: phase mode sets the test bit in the restart writes and clears it at tick 0", "[bside][engine][restart]")
{
    auto s = scenario (restartTable ("phase"), T (1) - 1, kAuth);
    // tick 0 without preconditioning: restart (test set), then the lane's control write replaces it on oscillator
    // 1, and the sync of the idle oscillators 2 and 3 clears their test bit
    CHECK (writesText (play (s)) == at (0, std::string (kR9Phase) + kWave1 + "0b 00\n12 00\n"));
}

TEST_CASE ("engine: a preconditioned idle voice has the restart state in place; the note starts normally", "[bside][engine][restart]")
{
    SECTION ("Precondition, idle for 3 ticks (the lead is 2)")
    {
        auto s = scenario (restartTable ("classic"), T (3), RestartPolicy::precondition);
        s.events = { idleAt (0, RestartPolicy::precondition), noteOn (T (3), RestartPolicy::precondition) };
        const auto rec = play (s);
        CHECK (writesText (rec) == at (0, kR9) + at (T (3), kWave1));
        CHECK (directKinds (rec).empty());
    }
    SECTION ("Clean with the full lead: no direct clear")
    {
        auto s = scenario (restartTable ("classic"), T (3), RestartPolicy::clean);
        s.events = { idleAt (0, RestartPolicy::clean), noteOn (T (2), RestartPolicy::clean) }; // exactly the lead
        const auto rec = play (s);
        CHECK (writesText (rec) == at (0, kR9) + at (T (2), kWave1));
        CHECK (directKinds (rec).empty());
    }
    SECTION ("Clean sooner than the lead: the rate counters are cleared directly; Precondition leaves them")
    {
        auto s = scenario (restartTable ("classic"), T (3), RestartPolicy::clean);
        s.events = { idleAt (0, RestartPolicy::clean), noteOn (T (1), RestartPolicy::clean) };
        auto rec = play (s);
        CHECK (writesText (rec) == at (0, kR9) + at (T (1), kWave1));
        REQUIRE (directKinds (rec).size() == 1);
        CHECK (rec.log()[rec.log().size() - 4].kind == pe::RegisterRecorder::Kind::clearEnvelopeTimers);
        s.events = { idleAt (0, RestartPolicy::precondition), noteOn (T (1), RestartPolicy::precondition) };
        rec = play (s);
        CHECK (writesText (rec) == at (0, kR9) + at (T (1), kWave1));
        CHECK (directKinds (rec).empty());
    }
    SECTION ("Authentic never preconditions")
    {
        auto s = scenario (restartTable ("classic"), T (4), kAuth);
        s.events = { idleAt (0, kAuth), noteOn (T (3), kAuth) };
        CHECK (writesText (play (s)) == at (T (3), std::string (kR9) + kWave1));
    }
    SECTION ("a voice preconditioned for another restart section does not count")
    {
        auto s = scenario (restartTable ("classic"), T (4), RestartPolicy::precondition);
        auto other = s;
        other.program = compileText (std::string ("bside-table 1") + restartTable ("classic", "ad 0f\n"));
        pe::RegisterRecorder rec;
        pe::ProgramEngine e;
        e.prepare ({ kClock50, true });
        e.setHost (quietHost());
        e.enterIdle (0, other.program.restart, RestartPolicy::precondition, rec);
        e.noteOn (T (3), s.program, RestartPolicy::precondition, false, rec);
        // the restart writes of the idle voice (ad 0f), then at tick 0 the note's own restart writes again
        CHECK (writesText (rec) == at (0, "05 0f\n06 00\n04 00\n0c 0f\n0d 00\n0b 00\n13 0f\n14 00\n12 00\n") + at (T (3), std::string (kR9) + kWave1));
    }
    SECTION ("an idle voice without a restart section (mode off) writes nothing")
    {
        auto s = scenario (restartTable ("off"), T (3), RestartPolicy::precondition);
        s.events = { idleAt (0, RestartPolicy::precondition), noteOn (T (3), RestartPolicy::precondition) };
        CHECK (writesText (play (s)) == at (T (3), kWave1));
    }
    SECTION ("phase: the idle state holds the test bit; tick 0 clears it")
    {
        auto s = scenario (restartTable ("phase"), T (3), RestartPolicy::precondition);
        s.events = { idleAt (0, RestartPolicy::precondition), noteOn (T (3), RestartPolicy::precondition) };
        CHECK (writesText (play (s)) == at (0, kR9Phase) + at (T (3), std::string (kWave1) + "0b 00\n12 00\n"));
    }
}

TEST_CASE ("engine: a stolen voice is reset and restarted (Clean, Precondition) or left alone (Authentic)", "[bside][engine][restart]")
{
    SECTION ("Clean")
    {
        auto s = scenario (restartTable ("classic"), T (2), RestartPolicy::clean);
        s.events = { noteOn (0, RestartPolicy::clean), noteOn (T (1), RestartPolicy::clean, true) };
        const auto rec = play (s);
        // At T(1): reset, restart writes, the direct clear (no lead), then tick 0 writes the wave row (the shadow
        // registers were reset, so nothing the host says is written: quiet host)
        std::vector<pe::RegisterRecorder::Kind> kinds;
        for (const auto& e : rec.log())
            if (e.cycle == T (1))
            {
                kinds.push_back (e.kind);
            }
        REQUIRE (kinds.size() == 1 + 9 + 1 + 3);
        CHECK (kinds[0] == pe::RegisterRecorder::Kind::resetChip);
        CHECK (kinds[10] == pe::RegisterRecorder::Kind::clearEnvelopeTimers);
        CHECK (kinds[11] == pe::RegisterRecorder::Kind::write);
    }
    SECTION ("Authentic: no reset, no direct operation; the restart writes happen at tick 0 only")
    {
        auto s = scenario (restartTable ("classic"), T (2), kAuth);
        s.events = { noteOn (0, kAuth), noteOn (T (1), kAuth, true) };
        const auto rec = play (s);
        CHECK (directKinds (rec).empty());
        // the second restart keeps the waveform bits it finds in the control register (gate off: 0x41 -> 0x40)
        CHECK (writesText (rec) == at (0, std::string (kR9) + kWave1)
                                       + at (T (1), "05 00\n06 00\n04 40\n0c 00\n0d 00\n0b 00\n13 00\n14 00\n12 00\n" + std::string (kWave1)));
    }
}

TEST_CASE ("engine: Clean and Precondition end a gate that is still high before a retrigger without a restart section", "[bside][engine][restart]")
{
    auto s = scenario ("\n[wave1]\n    wave pulse+gate =C4\n    end\n", T (2), RestartPolicy::clean);
    s.events = { noteOn (0, RestartPolicy::clean), noteOn (T (1), RestartPolicy::clean) };
    const auto rec = play (s);
    // second note: the gate (control 0x41) is cleared (0x40) first, then the wave row raises it again
    CHECK (writesText (rec) == at (0, kWave1) + at (T (1), std::string ("04 40\n") + kWave1));
    // Authentic: nothing clears it: the gate stays high and the chip sees no new attack
    s.events = { noteOn (0, kAuth), noteOn (T (1), kAuth) };
    CHECK (writesText (play (s)) == at (0, kWave1) + at (T (1), kWave1));
}

TEST_CASE ("engine: firstwave delays row 0 of the wave lanes to tick 1", "[bside][engine][restart]")
{
    // tick 0: restart writes; oscillator 1 (it has a wave lane) gets control := firstwave (tri) plus gate and the
    // host's frequency; tick 1: row 0
    auto s = scenario (restartTable ("classic", "firstwave tri\n"), T (2) - 1, kAuth);
    CHECK (writesText (play (s)) == at (0, std::string (kR9) + "00 67\n01 11\n04 11\n") + at (T (1), kWave1));
}

TEST_CASE ("engine: restart with mode off does nothing at tick 0", "[bside][engine][restart]")
{
    auto s = scenario (restartTable ("off"), T (1) - 1, kAuth);
    CHECK (writesText (play (s)) == at (0, kWave1));
}
