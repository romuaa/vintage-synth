// Property tests of the program engine on random valid tables (the reader's own generator): the values handed to
// the chip stay in range, nothing allocates after prepare, and the writes do not depend on how the caller splits time.

#include "EngineTestSupport.h"
#include "FuzzSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>

using namespace bst;
namespace pe = vs::bside::engine;
using vs::bside::engine::RestartPolicy;

namespace
{
/** Checks every value against the ranges of 8.4.1 / 8.4.6 as it arrives; allocates nothing. */
bool whole (double v) { return std::fabs (v - std::floor (v)) < 1e-12; }

class RangeSink final : public pe::ProgramSink
{
public:
    bool authentic = true;
    long long calls = 0, violations = 0;
    std::uint64_t lastCycle = 0;

    void frequency (std::uint64_t c, int osc, double v) noexcept override
    {
        touch (c);
        check (osc >= 0 && osc < 3 && std::isfinite (v));
        if (authentic)
            check (v >= 1.0 && v <= 65535.0 && whole (v));
        else
            check (v >= 0.0 && v <= 204342.0);
    }
    void pulseWidth (std::uint64_t c, int osc, double v) noexcept override
    {
        touch (c);
        check (osc >= 0 && osc < 3 && std::isfinite (v) && v >= 0.0 && v <= 4095.0);
        if (authentic)
            check (whole (v));
    }
    void control (std::uint64_t c, int osc, std::uint8_t) noexcept override { touch (c); check (osc >= 0 && osc < 3); }
    void envelope (std::uint64_t c, int osc, std::uint8_t, std::uint8_t) noexcept override { touch (c); check (osc >= 0 && osc < 3); }
    void cutoff (std::uint64_t c, double v) noexcept override
    {
        touch (c);
        check (std::isfinite (v) && v >= 0.0 && v <= 2047.0);
        if (authentic)
            check (whole (v));
    }
    void resonanceRouting (std::uint64_t c, std::uint8_t v) noexcept override { touch (c); check ((v & 0x08) == 0); }
    void modeVolume (std::uint64_t c, std::uint8_t) noexcept override { touch (c); }
    void clearEnvelopeTimers (std::uint64_t c) noexcept override { touch (c); }
    void resetChip (std::uint64_t c) noexcept override { touch (c); }

private:
    void touch (std::uint64_t c) noexcept
    {
        ++calls;
        check (c >= lastCycle); // cycles never go backwards
        lastCycle = c;
    }
    void check (bool ok) noexcept { violations += ok ? 0 : 1; }
};

double randomSemitones (Rng& r)
{
    switch (r.below (12))
    {
        case 0: return std::numeric_limits<double>::quiet_NaN();
        case 1: return -1000.0;
        case 2: return 1000.0;
        case 3: return std::numeric_limits<double>::infinity();
        default: return -20.0 + 160.0 * static_cast<double> (r.below (10000)) / 10000.0;
    }
}

pe::HostInputs randomHost (Rng& r)
{
    pe::HostInputs h;
    for (auto& o : h.osc)
    {
        o.semitones = randomSemitones (r);
        o.enabled = r.chance (60);
        o.waveBits = static_cast<std::uint8_t> (r.below (16) << 4);
        o.sync = r.chance (20);
        o.ring = r.chance (20);
        o.pulseWidth = r.chance (10) ? std::numeric_limits<float>::quiet_NaN() : static_cast<float> (r.range (-500, 4700));
        o.attackDecay = static_cast<std::uint8_t> (r.below (256));
        o.sustainRelease = static_cast<std::uint8_t> (r.below (256));
    }
    h.filter.cutoff = static_cast<float> (r.range (-300, 2500));
    h.filter.resonance = static_cast<std::uint8_t> (r.below (16));
    h.filter.routing = static_cast<std::uint8_t> (r.below (8));
    h.filter.mode = static_cast<std::uint8_t> (r.below (8));
    h.filter.osc3Off = r.chance (30);
    h.filter.volume = static_cast<std::uint8_t> (r.below (16));
    h.macro.pulseWidthOffset = static_cast<float> (r.range (-3000, 3000));
    h.macro.cutoffOffset = static_cast<float> (r.range (-3000, 3000));
    h.macro.sweepDepth = r.chance (10) ? std::numeric_limits<float>::infinity() : static_cast<float> (r.range (0, 400)) / 100.0f;
    return h;
}

RestartPolicy randomPolicy (Rng& r)
{
    return static_cast<RestartPolicy> (r.below (3));
}

/** A random timeline of events on a random table; the same timeline is replayed with different chunking. */
Scenario randomScenario (Rng& r, const Program& program)
{
    Scenario s;
    s.program = program;
    s.host = randomHost (r);
    s.config = { kClock50, r.chance (60) };
    s.period = static_cast<std::uint32_t> (r.chance (50) ? kFrame : 256 + r.below (30000));
    const std::uint64_t horizon = static_cast<std::uint64_t> (s.period) * 40;
    std::uint64_t t = 0;
    s.events.push_back (noteOn (0, randomPolicy (r)));
    const int extra = r.range (1, 10);
    for (int i = 0; i < extra; ++i)
    {
        t += 1 + r.below (static_cast<std::uint32_t> (horizon / 6));
        switch (r.below (6))
        {
            case 0: s.events.push_back (noteOff (t)); break;
            case 1: s.events.push_back (noteOn (t, randomPolicy (r), r.chance (40))); break;
            case 2: s.events.push_back (idleAt (t, randomPolicy (r))); break;
            case 3: s.events.push_back (hostAt (t, randomHost (r))); break;
            case 4: s.events.push_back (periodAt (t, 256 + r.below (40000))); break;
            default: s.events.push_back (syncAt (t)); break;
        }
    }
    s.endCycle = t + horizon / 2;
    return s;
}

/** Runs a scenario into a RangeSink, with the allocation count taken around the engine calls only. */
void runChecked (const Scenario& s, RangeSink& sink, std::size_t& allocations)
{
    sink.authentic = s.config.authentic;
    pe::ProgramEngine e;
    e.prepare (s.config); // may allocate: nothing does today, and nothing may after this point
    const auto before = totalAllocations();
    e.setTickPeriod (s.period);
    e.setHost (s.host);
    for (const auto& ev : s.events)
    {
        e.runUntil (ev.cycle, sink);
        switch (ev.kind)
        {
            case Event::Kind::noteOn: e.noteOn (ev.cycle, s.program, ev.policy, ev.stolen, sink); break;
            case Event::Kind::noteOff: e.noteOff (ev.cycle, sink); break;
            case Event::Kind::idle: e.enterIdle (ev.cycle, s.program.restart, ev.policy, sink); break;
            case Event::Kind::sync: e.sync (ev.cycle, sink); break;
            case Event::Kind::setHost: e.setHost (ev.host); break;
            case Event::Kind::setPeriod: e.setTickPeriod (ev.period); break;
        }
    }
    e.runUntil (s.endCycle, sink);
    allocations = totalAllocations() - before;
}

/** Events only at multiples of 1000 cycles, host changes and period changes included (each host change followed by a
    sync at its cycle): the block-rate inputs of a real run, changing between chunks. */
Scenario blockAlignedScenario (Rng& r, const Program& program)
{
    Scenario s;
    s.program = program;
    s.host = randomHost (r);
    s.config = { kClock50, r.chance (60) };
    s.period = static_cast<std::uint32_t> (r.chance (50) ? kFrame : 256 + r.below (30000));
    s.events.push_back (noteOn (0, randomPolicy (r)));
    std::uint64_t t = 0;
    const int extra = r.range (3, 14);
    for (int i = 0; i < extra; ++i)
    {
        t += 1000 * (1 + r.below (60));
        switch (r.below (6))
        {
            case 0: s.events.push_back (noteOff (t)); break;
            case 1: s.events.push_back (noteOn (t, randomPolicy (r), r.chance (40))); break;
            case 2: s.events.push_back (idleAt (t, randomPolicy (r))); break;
            case 3:
            case 4:
                s.events.push_back (hostAt (t, randomHost (r)));
                s.events.push_back (syncAt (t));
                break;
            default: s.events.push_back (periodAt (t, 256 + r.below (40000))); break;
        }
    }
    s.endCycle = t + 20000;
    return s;
}
} // namespace

TEST_CASE ("engine properties: random tables, hosts and event timelines (short run)", "[bside][engine][property]")
{
    constexpr int kCases = 400;
    long long accepted = 0, rejected = 0, totalCalls = 0, totalWrites = 0;
    for (int i = 0; i < kCases; ++i)
    {
        Rng r (static_cast<std::uint64_t> (9000 + i));
        const auto parsed = vs::bside::program::readTable (generateTable (r));
        if (! parsed.ok)
        {
            ++rejected;
            continue;
        }
        ++accepted;
        const auto s = randomScenario (r, parsed.program);
        INFO ("case " << i);

        RangeSink sink;
        std::size_t allocations = 0;
        runChecked (s, sink, allocations);
        CHECK (sink.violations == 0);
        if (allocationsAreCounted())
            CHECK (allocations == 0);
        totalCalls += sink.calls;

        // The recorder turns every call into registers inside 0x00..0x18; same calls, so the same writes at any block split.
        const auto rec = play (s);
        for (const auto& e : rec.log())
            if (e.kind == pe::RegisterRecorder::Kind::write)
            {
                CHECK (e.reg <= vs::bside::trace::kMaxWritableRegister);
                ++totalWrites;
            }
        // Host parameters are block-rate by nature (they take effect at the next sync or tick), so the block-split
        // comparison uses the same timeline without host changes.
        auto steady = s;
        steady.events.erase (std::remove_if (steady.events.begin(), steady.events.end(), [] (const Event& e) { return e.kind == Event::Kind::setHost; }),
                             steady.events.end());
        const std::string reference = writesText (play (steady));
        for (const std::uint64_t chunk : std::initializer_list<std::uint64_t> { 13, 1000, s.period, 20011 })
            CHECK (writesText (play (steady, chunk)) == reference);
    }
    CHECK (accepted > kCases / 4);
    CHECK (totalCalls > 1000);
    INFO ("accepted " << accepted << ", rejected " << rejected);
    CHECK (totalWrites > 1000);
}

TEST_CASE ("engine properties: block-rate changes of host values and tick period between chunks do not depend on the chunking", "[bside][engine][property]")
{
    long long withHostChanges = 0, withPeriodChanges = 0;
    for (int i = 0; i < 250; ++i)
    {
        Rng r (static_cast<std::uint64_t> (31000 + i));
        const auto parsed = vs::bside::program::readTable (generateTable (r));
        if (! parsed.ok)
            continue;
        const auto s = blockAlignedScenario (r, parsed.program);
        for (const auto& e : s.events)
        {
            withHostChanges += e.kind == Event::Kind::setHost ? 1 : 0;
            withPeriodChanges += e.kind == Event::Kind::setPeriod ? 1 : 0;
        }
        INFO ("case " << i);
        const std::string reference = play (s).toText (kClock50, s.endCycle);
        for (const std::uint64_t chunk : std::initializer_list<std::uint64_t> { 100, 250, 500, 1000, 3000 })
            CHECK (play (s, chunk).toText (kClock50, s.endCycle) == reference);
    }
    CHECK (withHostChanges > 100);
    CHECK (withPeriodChanges > 50);
}

namespace
{
struct alignas (64) OverAligned
{
    char bytes[64];
};
} // namespace

TEST_CASE ("engine properties: over-aligned allocations are counted too", "[bside][engine][property]")
{
    if (! allocationsAreCounted())
        SKIP ("operator new is not replaced under AddressSanitizer");
    const auto before = totalAllocations();
    static OverAligned* volatile escape; // keeps the optimiser from eliding the allocation
    escape = new OverAligned;
    OverAligned* p = escape;
    p->bytes[0] = 1;
    const auto after = totalAllocations();
    CHECK (p->bytes[0] == 1);
    CHECK (reinterpret_cast<std::uintptr_t> (p) % 64 == 0);
    delete p;
    CHECK (after > before);
}

TEST_CASE ("engine properties: the allocation counter sees allocations", "[bside][engine][property]")
{
    if (! allocationsAreCounted())
        SKIP ("operator new is not replaced under AddressSanitizer");
    const auto before = totalAllocations();
    std::vector<int> v (1000, 1); // a real allocation the optimiser cannot drop: the sum is used
    long long sum = 0;
    for (int x : v)
        sum += x;
    const auto after = totalAllocations();
    CHECK (sum == 1000);
    CHECK (after > before);
}

TEST_CASE ("engine properties: the longest legal burst runs in one tick", "[bside][engine][property]")
{
    // 64 rows in a lane: 62 rows with delay 0, one with delay 1, and the jump back (delay 0): one tick executes all
    // 64 rows (the most the reader's guarantee allows) and writes 63 frequencies.
    std::string t = "bside-table 1\n[wave1]\ntop:\n";
    for (int i = 0; i < 62; ++i)
        t += "    pitch +" + std::to_string (i % 7) + " ticks 0\n";
    t += "    pitch +1\n    jump top\n";
    const auto parsed = vs::bside::program::readTable (t);
    REQUIRE (parsed.ok);
    pe::RegisterRecorder rec;
    pe::ProgramEngine e;
    e.prepare ({ kClock50, true });
    e.noteOn (0, parsed.program, RestartPolicy::authentic, false, rec);
    e.runUntil (kFrame, rec);
    long long atTick0 = 0, atTick1 = 0;
    for (const auto& w : rec.log())
        if (w.kind == pe::RegisterRecorder::Kind::write && w.reg == 0x00)
            (w.cycle == 0 ? atTick0 : atTick1) += 1;
    CHECK (atTick0 == 63);
    CHECK (atTick1 == 63);
}

TEST_CASE ("engine cost per tick (hidden: prints ns per tick)", "[.engine-bench]")
{
    const auto parsed = vs::bside::program::readTable (bst::kPulseLead);
    REQUIRE (parsed.ok);
    RangeSink sink;
    pe::ProgramEngine e;
    e.prepare ({ kClock50, true });
    e.setHost (defaultHost());
    e.noteOn (0, parsed.program, RestartPolicy::clean, false, sink);
    constexpr long long kTicks = 2000000;
    const auto t0 = std::chrono::steady_clock::now();
    for (long long i = 0; i < kTicks; ++i)
        e.runTick (sink);
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano> (t1 - t0).count() / static_cast<double> (kTicks);
    INFO ("calls " << sink.calls);
    WARN ("program engine: " << ns << " ns per tick (pulse lead table, sink checks included)");
    CHECK (sink.violations == 0);
}
