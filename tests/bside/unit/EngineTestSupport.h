#pragma once

// Helpers of the program engine tests: building programs from table text, a note scenario driven like the
// chip is (render to the next tick, then run it), and the register text the tests compare.

#include "TableTestUtil.h"

#include "bside/chip/ChipClock.h"
#include "bside/engine/ProgramEngine.h"
#include "RegisterRecorder.h"
#include "bside/engine/TickClock.h"
#include "bside/trace/TraceFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace bst
{
namespace eng = vs::bside::engine;

constexpr std::uint32_t kClock50 = 985248;
constexpr std::uint32_t kFrame = 19656;

/** The host parameters of the golden scenarios: oscillator 1 a gated pulse at middle C (note 60), oscillators 2 and
    3 off, the filter at its defaults (cutoff 1024, resonance 0, all routed, low-pass, volume 15). */
inline eng::HostInputs defaultHost()
{
    eng::HostInputs h;
    h.osc[0].enabled = true;
    h.osc[0].waveBits = 0x40;
    h.osc[0].semitones = 60.0;
    return h;
}

inline Program compileText (const std::string& text)
{
    auto r = readTable (text);
    INFO (r.message());
    REQUIRE (r.ok);
    return r.program;
}

/** One event of a scenario at a chip cycle. */
struct Event
{
    enum class Kind { noteOn, noteOff, idle, sync, setHost, setPeriod } kind = Kind::noteOn;
    std::uint64_t cycle = 0;
    eng::RestartPolicy policy = eng::RestartPolicy::clean;
    bool stolen = false;
    eng::HostInputs host;
    std::uint32_t period = 0;
};

inline Event noteOn (std::uint64_t cycle, eng::RestartPolicy p = eng::RestartPolicy::clean, bool stolen = false)
{
    Event e;
    e.kind = Event::Kind::noteOn;
    e.cycle = cycle;
    e.policy = p;
    e.stolen = stolen;
    return e;
}
inline Event noteOff (std::uint64_t cycle)
{
    Event e;
    e.kind = Event::Kind::noteOff;
    e.cycle = cycle;
    return e;
}
inline Event idleAt (std::uint64_t cycle, eng::RestartPolicy p)
{
    Event e;
    e.kind = Event::Kind::idle;
    e.cycle = cycle;
    e.policy = p;
    return e;
}
inline Event syncAt (std::uint64_t cycle)
{
    Event e;
    e.kind = Event::Kind::sync;
    e.cycle = cycle;
    return e;
}
inline Event hostAt (std::uint64_t cycle, const eng::HostInputs& h)
{
    Event e;
    e.kind = Event::Kind::setHost;
    e.cycle = cycle;
    e.host = h;
    return e;
}
inline Event periodAt (std::uint64_t cycle, std::uint32_t p)
{
    Event e;
    e.kind = Event::Kind::setPeriod;
    e.cycle = cycle;
    e.period = p;
    return e;
}

struct Scenario
{
    Program program = vs::bside::program::emptyProgram();
    eng::HostInputs host = defaultHost();
    eng::EngineConfig config { kClock50, true };
    std::uint32_t period = kFrame;
    std::vector<Event> events;
    std::uint64_t endCycle = 0;
};

/** Plays a scenario like the chip's renderer would: before each event, every tick due up to its cycle runs at its own
    cycle; `chunk` (> 0) additionally stops at every multiple of it and calls `sync` there (a block boundary: it must
    change nothing). Returns the recorder's log. */
inline vs::bside::engine::RegisterRecorder play (const Scenario& s, std::uint64_t chunk = 0)
{
    eng::RegisterRecorder rec;
    eng::ProgramEngine e;
    e.prepare (s.config);
    e.setTickPeriod (s.period);
    e.setHost (s.host);
    eng::HostInputs host = s.host;
    std::uint64_t now = 0;
    const auto advanceTo = [&] (std::uint64_t cycle) {
        while (chunk > 0 && now / chunk < cycle / chunk)
        {
            now = (now / chunk + 1) * chunk;
            e.runUntil (now - 1, rec);
            if (e.isRunning())
                e.sync (now, rec);
        }
        e.runUntil (cycle, rec);
        now = cycle;
    };
    for (const auto& ev : s.events)
    {
        advanceTo (ev.cycle);
        switch (ev.kind)
        {
            case Event::Kind::noteOn: e.noteOn (ev.cycle, s.program, ev.policy, ev.stolen, rec); break;
            case Event::Kind::noteOff: e.noteOff (ev.cycle, rec); break;
            case Event::Kind::idle: e.enterIdle (ev.cycle, s.program.restart, ev.policy, rec); break;
            case Event::Kind::sync: e.sync (ev.cycle, rec); break;
            case Event::Kind::setHost: host = ev.host; e.setHost (host); break;
            case Event::Kind::setPeriod: e.setTickPeriod (ev.period); break;
        }
    }
    if (s.endCycle > 0)
    {
        advanceTo (s.endCycle);
        // ticks exactly at endCycle are part of the cycle `endCycle`; the traces end before the next tick
    }
    return rec;
}

/** "cycle reg value" lines of the register writes only (no header, no comments). */
inline std::string writesText (const eng::RegisterRecorder& rec)
{
    static const char* const hexd = "0123456789abcdef";
    std::string out;
    for (const auto& w : rec.log())
        if (w.kind == eng::RegisterRecorder::Kind::write)
        {
            out += std::to_string (w.cycle) + ' ' + hexd[w.reg >> 4] + hexd[w.reg & 15] + ' ' + hexd[w.value >> 4] + hexd[w.value & 15] + '\n';
        }
    return out;
}

/** Normalises an expected block written in a test: drops indentation and blank lines. */
inline std::string expectLines (const std::string& block)
{
    std::istringstream in (block);
    std::string line, out;
    while (std::getline (in, line))
    {
        const auto b = line.find_first_not_of (" \t");
        if (b == std::string::npos)
            continue;
        const auto e = line.find_last_not_of (" \t");
        out += line.substr (b, e - b + 1) + '\n';
    }
    return out;
}

inline std::string readFile (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    std::string out;
    for (char c : s)
        if (c != '\r')
            out += c;
    return out;
}
} // namespace bst
