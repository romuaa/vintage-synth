// The program engine driving the real chip through ChipProgramSink: a steal on the real chip, the Smooth path, and the
// one operation the chip cannot do yet.

#include "EngineTestSupport.h"

#include "bside/chip/Chip.h"
#include "bside/engine/ChipProgramSink.h"

#include <cmath>

using namespace bst;
namespace pe = vs::bside::engine;
namespace chip = vs::bside::chip;
using vs::bside::engine::RestartPolicy;

namespace
{
chip::Config config (bool authentic)
{
    chip::Config c;
    c.model = chip::Model::B;
    c.clockHz = kClock50;
    c.authentic = authentic;
    return c;
}

/** Renders the chip up to each tick like a part would: `engine.nextTickCycle() - sink.cycle()` cycles at a time. */
struct Drive
{
    chip::Chip chip;
    pe::ChipProgramSink sink;
    pe::ProgramEngine engine;
    std::uint64_t maxStep = 0;

    explicit Drive (bool authentic) : sink (chip, authentic)
    {
        chip.prepare (config (authentic));
        engine.prepare ({ kClock50, authentic });
    }
    void advance (std::uint64_t to)
    {
        if (to > sink.cycle())
        {
            maxStep = std::max (maxStep, to - sink.cycle());
            chip.advanceCycles (to - sink.cycle());
        }
    }
    void runTo (std::uint64_t cycle)
    {
        while (engine.isRunning() && engine.nextTickCycle() <= cycle)
        {
            advance (engine.nextTickCycle());
            engine.runTick (sink);
        }
        advance (cycle);
    }
};

const char* kTable = R"(bside-table 1
[restart]
mode classic
lead 2
[wave1]
    wave pulse+gate =C4
    pitch =G4
    end
)";
} // namespace

TEST_CASE ("chip sink: a steal on the real chip restarts the chip but not the time; ticks go on", "[bside][engine][chip]")
{
    Drive d (true);
    d.engine.setHost (defaultHost());
    const auto program = compileText (kTable);
    d.engine.noteOn (0, program, RestartPolicy::clean, false, d.sink);
    d.runTo (3 * kFrame + 100);
    const auto before = d.chip.cycle();
    CHECK (before == 3 * kFrame + 100);
    // steal: the chip returns to power-on state, its cycle count keeps running
    d.engine.noteOn (3 * kFrame + 100, program, RestartPolicy::clean, true, d.sink);
    CHECK (d.chip.cycle() == 3 * kFrame + 100);
    CHECK (d.sink.cycle() == 3 * kFrame + 100);
    d.runTo (8 * kFrame);
    CHECK (d.sink.cycle() == 8 * kFrame);
    CHECK (d.chip.cycle() == 8 * kFrame);
    // the new note's registers are in the chip: pulse gate on at tick 0, G4 (6675 = 0x1a13) from tick 1
    CHECK (d.chip.control (0) == 0x41);
    CHECK (d.chip.registerValue (0x00) == 0x13);
    CHECK (d.chip.registerValue (0x01) == 0x1a);
    // no single render step was longer than a tick plus the odd offset (never a lifetime)
    CHECK (d.maxStep <= kFrame);
}

TEST_CASE ("chip sink: Smooth hands the engine's real values to the chip", "[bside][engine][chip]")
{
    Drive d (false);
    auto h = defaultHost();
    h.osc[0].semitones = 69.0;      // 7492.5045 register units: kept with its fraction
    h.osc[0].pulseWidth = 100.25f;
    h.osc[0].attackDecay = 0x29;
    h.osc[0].sustainRelease = 0xc4;
    h.filter.cutoff = 1000.5f;
    h.filter.resonance = 9;
    h.filter.routing = 5;
    h.filter.mode = 4;
    h.filter.volume = 11;
    d.engine.setHost (h);
    d.engine.noteOn (0, vs::bside::program::emptyProgram(), RestartPolicy::clean, false, d.sink);
    CHECK (d.chip.control (0) == 0x41);
    CHECK (d.chip.registerValue (0x05) == 0x29);
    CHECK (d.chip.registerValue (0x06) == 0xc4);
    CHECK (std::fabs (static_cast<double> (d.chip.increment (0)) / 4294967296.0 - 7492.504465880672) < 1e-6);
    CHECK (std::fabs (static_cast<double> (d.chip.pulseWidthSmooth (0)) - 100.25) < 1e-4);
    CHECK (std::fabs (static_cast<double> (d.chip.cutoffSmooth()) - 1000.5) < 1e-3);
    CHECK (d.chip.registerValue (0x18) == ((4 << 4) | 11));

    // a later host change reaches the chip through sync, still with its fraction
    h.osc[0].semitones = 70.5;
    d.engine.setHost (h);
    d.runTo (1000);
    d.engine.sync (1000, d.sink);
    CHECK (std::fabs (static_cast<double> (d.chip.increment (0)) / 4294967296.0 - 8170.6340570718485) < 1e-6);

    // Authentic rounds the same values to the registers
    Drive a (true);
    a.engine.setHost (defaultHost());
    auto h2 = h;
    h2.osc[0].semitones = 69.0;
    a.engine.setHost (h2);
    a.engine.noteOn (0, vs::bside::program::emptyProgram(), RestartPolicy::clean, false, a.sink);
    CHECK (a.chip.increment (0) == (std::uint64_t { 7493 } << 32));
    CHECK (std::fabs (static_cast<double> (a.chip.pulseWidthSmooth (0)) - 100.0) < 1e-6);
}

TEST_CASE ("chip sink: a reset keeps the chip's time and clears its state", "[bside][engine][chip]")
{
    chip::Chip c;
    c.prepare (config (true));
    pe::ChipProgramSink sink (c, true);
    sink.control (0, 0, 0x41);
    c.advanceCycles (5000);
    CHECK (sink.cycle() == 5000);
    sink.resetChip (5000);
    CHECK (c.cycle() == 5000);
    CHECK (sink.cycle() == 5000);
    CHECK (c.control (0) == 0);
    c.advanceCycles (700);
    CHECK (sink.cycle() == 5700);
}

namespace
{
/** The same sink without Clean's direct clear (to show what the clear changes). */
class NoClearSink final : public pe::ProgramSink
{
public:
    explicit NoClearSink (pe::ChipProgramSink& s) : inner (s) {}
    void frequency (std::uint64_t c, int o, double v) noexcept override { inner.frequency (c, o, v); }
    void pulseWidth (std::uint64_t c, int o, double v) noexcept override { inner.pulseWidth (c, o, v); }
    void control (std::uint64_t c, int o, std::uint8_t v) noexcept override { inner.control (c, o, v); }
    void envelope (std::uint64_t c, int o, std::uint8_t a, std::uint8_t r) noexcept override { inner.envelope (c, o, a, r); }
    void cutoff (std::uint64_t c, double v) noexcept override { inner.cutoff (c, v); }
    void resonanceRouting (std::uint64_t c, std::uint8_t v) noexcept override { inner.resonanceRouting (c, v); }
    void modeVolume (std::uint64_t c, std::uint8_t v) noexcept override { inner.modeVolume (c, v); }
    void clearEnvelopeTimers (std::uint64_t) noexcept override {}
    void resetChip (std::uint64_t c) noexcept override { inner.resetChip (c); }

private:
    pe::ChipProgramSink& inner;
};

struct ClearResult
{
    std::uint32_t position = 0, divider = 0;
    int firstStep = -1; // cycles from the note until the first oscillator's counter first changes
};

/** Idle-time restart at 0 (ad/sr 99: both rates 977 cycles), a note on the same voice `noteAt` cycles later. */
ClearResult attack (bool withClear, std::uint64_t noteAt)
{
    const auto program = compileText ("bside-table 1\n[restart]\nmode classic\nlead 2\nad 99\nsr 99\n");
    auto h = defaultHost();
    h.osc[0].attackDecay = 0x90; // attack period 977 cycles
    h.osc[0].sustainRelease = 0xf0;
    chip::Chip c;
    c.prepare (config (true));
    pe::ChipProgramSink real (c, true);
    NoClearSink noClear (real);
    pe::ProgramSink& sink = withClear ? static_cast<pe::ProgramSink&> (real) : static_cast<pe::ProgramSink&> (noClear);
    pe::ProgramEngine e;
    e.prepare ({ kClock50, true });
    e.setHost (h);
    e.enterIdle (0, program.restart, RestartPolicy::clean, sink);
    c.advanceCycles (noteAt);
    e.noteOn (noteAt, program, RestartPolicy::clean, false, sink); // not the full lead: Clean clears the counters
    ClearResult r;
    const auto s = c.state();
    r.position = s.env[0].position;
    r.divider = s.env[0].divider;
    const auto counter = c.envelope (0);
    for (int i = 1; i <= 3000 && r.firstStep < 0; ++i)
    {
        c.advanceCycles (1);
        if (c.envelope (0) != counter)
            r.firstStep = i;
    }
    return r;
}
} // namespace

TEST_CASE ("chip sink: Clean's direct clear starts the attack from a cleared rate counter", "[bside][engine][chip]")
{
    // The restart state has been in place for a short lead (under 2 ticks); the rate counter has run on meanwhile.
    bool differed = false;
    for (const std::uint64_t noteAt : { 3000ull, 3111ull, 4321ull, 7777ull })
    {
        const auto cleared = attack (true, noteAt);
        const auto left = attack (false, noteAt);
        INFO ("note at " << noteAt);
        CHECK (cleared.position == 0);
        CHECK (cleared.divider == 0);
        CHECK (cleared.firstStep > 0);
        if (left.position != 0)
        {
            // without the clear the counter is wherever the idle voice left it, so the first step comes sooner
            differed = true;
            CHECK (left.firstStep < cleared.firstStep);
        }
    }
    CHECK (differed);
}

// ---- Clean never waits (docs/architecture.md 8.21.2) -----------------------------------------------------------------

namespace
{
std::string hatTable (const char* mode, int lead)
{
    return std::string ("bside-table 1\n[restart]\nmode ") + mode + "\nlead " + std::to_string (lead)
           + "\n[wave1]\n    adsr 0 6 0 6, wave noise+gate =E7\n    end\n";
}

/** Oscillator 1's envelope around one note on a voice that has held the restart state since cycle 0. */
struct WaitProbe
{
    chip::EnvState atNote;       // at the note's cycle, before the engine's writes
    chip::EnvState afterWrites;  // after the note's writes (the same cycle)
    chip::EnvState step1, step2; // after the first and the second cycle
    std::int64_t wait = -1;      // cycles from the note to the envelope counter's first step
};

WaitProbe probeNote (chip::Model m, bool withClear, int lead, std::uint64_t noteAt)
{
    auto cfg = config (true);
    cfg.model = m;
    chip::Chip c;
    c.prepare (cfg);
    pe::ChipProgramSink real (c, true);
    NoClearSink noClear (real);
    pe::ProgramSink& sink = withClear ? static_cast<pe::ProgramSink&> (real) : static_cast<pe::ProgramSink&> (noClear);
    pe::ProgramEngine e;
    e.prepare ({ kClock50, true });
    e.setTickPeriod (kFrame);
    e.setHost (defaultHost());
    const auto program = compileText (hatTable ("classic", lead));
    e.enterIdle (0, program.restart, RestartPolicy::clean, sink);
    c.advanceCycles (noteAt);
    WaitProbe p;
    p.atNote = c.state().env[0];
    e.noteOn (noteAt, program, RestartPolicy::clean, false, sink);
    p.afterWrites = c.state().env[0];
    for (std::int64_t i = 1; i <= 40000 && p.wait < 0; ++i)
    {
        if (e.isRunning() && e.nextTickCycle() == c.cycle())
            e.runTick (sink);
        c.advanceCycles (1);
        if (i == 1)
            p.step1 = c.state().env[0];
        if (i == 2)
            p.step2 = c.state().env[0];
        if (c.envelope (0) != 0)
            p.wait = i;
    }
    return p;
}
} // namespace

TEST_CASE ("Clean never waits: the mechanism of the 32 768-cycle wait on a voice that had its full lead, and the direct clear that ends it",
           "[bside][engine][chip][restart]")
{
    // The restart state (sr 00: release rate 0, match 8) keeps the idle rate counter cycling over positions 0..8 for as long as
    // the voice is idle, whatever the lead: a lead makes the restart state old, it never puts the counter anywhere fixed.
    // The hat's note (attack 0: match 8; decay 6: match 266) raises the gate, and the gate edge's phase pipeline sets the
    // compare to the DECAY match for one step, then to the attack match (Chip::envStep, step 3). When the note's cycle finds
    // the counter AT 8 with its reset not yet pending, the counter steps to 9 under the decay compare and is past the attack
    // match (8) when that arrives: it runs through all 32 767 positions first. Without a direct clear this happens on one
    // phase of the counter's loop, for every lead; with Clean's direct clear at the note's cycle it never happens.
    for (const auto model : { chip::Model::B, chip::Model::A })
        for (const int lead : { 2, 4, 8 })
        {
            int waited = 0;
            for (std::uint64_t k = 0; k < 20; ++k)
            {
                const std::uint64_t noteAt = static_cast<std::uint64_t> (lead + 3) * kFrame + k; // well past the full lead
                const auto left = probeNote (model, false, lead, noteAt);
                const auto cleared = probeNote (model, true, lead, noteAt);
                INFO ("model " << (model == chip::Model::A ? "A" : "B") << ", lead " << lead << ", note at " << noteAt << ": position at the note "
                               << left.atNote.position << " (compare " << left.atNote.compare << ", reset pending " << left.atNote.resetPending
                               << "), wait without the clear " << left.wait << ", with it " << cleared.wait);
                // the idle counter is in the restart state's loop, never above its match
                CHECK (left.atNote.compare == 8);
                CHECK (left.atNote.position <= 8);
                if (left.wait > 1000)
                {
                    ++waited;
                    // exactly the phase described above, and the two steps that put the counter behind the attack match
                    CHECK (left.atNote.position == 8);
                    CHECK_FALSE (left.atNote.resetPending);
                    CHECK (left.afterWrites.phasePipeline == 2);
                    CHECK (left.step1.compare == 266); // the decay match (decay 6), for one step
                    CHECK (left.step1.position == 9);
                    CHECK (left.step2.compare == 8); // the attack match: the counter (10) is past it
                    CHECK (left.step2.position == 10);
                    CHECK (left.wait >= 32760);
                    CHECK (left.wait <= 32780);
                    if (k < 10)
                        std::printf ("Clean mechanism: model %s, lead %d: counter at %u (reset not pending) at the note; after 1 cycle %u / compare %u, after 2 "
                                     "cycles %u / compare %u; the attack's first step %lld cycles after the note without the clear, %lld with it\n",
                                     model == chip::Model::A ? "A" : "B", lead, static_cast<unsigned> (left.atNote.position), static_cast<unsigned> (left.step1.position),
                                     static_cast<unsigned> (left.step1.compare), static_cast<unsigned> (left.step2.position), static_cast<unsigned> (left.step2.compare),
                                     static_cast<long long> (left.wait), static_cast<long long> (cleared.wait));
                }
                else
                {
                    CHECK (left.wait > 0);
                    CHECK (left.wait < 16);
                }
                // Clean: cleared at the note's cycle, before the gate's pipeline runs; the attack never waits
                CHECK (cleared.afterWrites.position == 0);
                CHECK (cleared.wait > 0);
                CHECK (cleared.wait < 16);
            }
            CHECK (waited == 2); // one phase of the loop (10 cycles: positions 0..8, then 8 again with the reset pending), twice in 20
        }
}

namespace
{
/** The onsets (ms from each note-on) of `hits` notes `spacing` seconds apart on one voice under Clean, at 48 kHz. The engine and the
    chip are driven like a part (render to each tick; the idle rule of 8.5 after the note-off); the level is the chip's envelope
    counter, the multiplier of everything the voice outputs, so a hit's onset is the first sample at or above -30 dB of that hit's
    peak counter. Digital only: no audio is rendered. */
std::vector<double> cleanOnsets (chip::Model m, const Program& program, const pe::HostInputs& host, double spacing, int hits)
{
    constexpr double rate = 48000.0;
    auto cfg = config (true);
    cfg.model = m;
    chip::Chip c;
    c.prepare (cfg);
    pe::ChipProgramSink sink (c, true);
    pe::ProgramEngine e;
    e.prepare ({ kClock50, true });
    e.setTickPeriod (kFrame);
    e.setHost (host);
    auto cycleOf = [] (std::int64_t s) { return static_cast<std::uint64_t> (static_cast<double> (s) * kClock50 / rate); };
    std::vector<std::int64_t> on, off;
    for (int h = 0; h < hits; ++h)
    {
        on.push_back (std::llround ((0.05 + h * spacing) * rate));
        off.push_back (on.back() + std::llround (0.4 * spacing * rate));
    }
    const std::int64_t end = on.back() + std::llround (spacing * rate);
    std::vector<std::uint8_t> level (static_cast<size_t> (end), 0);
    bool held = false, idle = true;
    size_t nextOn = 0, nextOff = 0;
    for (std::int64_t s = 0; s < end; ++s)
    {
        const std::uint64_t cycle = cycleOf (s);
        while (e.isRunning() && e.nextTickCycle() <= cycle)
        {
            c.advanceCycles (e.nextTickCycle() - c.cycle());
            e.runTick (sink);
        }
        c.advanceCycles (cycle - c.cycle());
        if (nextOff < off.size() && off[nextOff] == s)
        {
            e.noteOff (cycle, sink);
            held = false;
            ++nextOff;
        }
        if (nextOn < on.size() && on[nextOn] == s)
        {
            e.noteOn (cycle, program, RestartPolicy::clean, false, sink);
            held = true;
            idle = false;
            ++nextOn;
        }
        if (! held && ! idle && ! e.anyWaveLaneRunning())
        {
            bool silent = true;
            for (int n = 0; n < 3 && silent; ++n)
                silent = c.envelope (n) == 0 && c.envelopePhase (n) == chip::EnvPhase::release;
            if (silent)
            {
                e.enterIdle (cycle, program.restart, RestartPolicy::clean, sink);
                idle = true;
            }
        }
        level[static_cast<size_t> (s)] = c.envelope (0);
    }
    std::vector<double> onsets;
    for (size_t h = 0; h < on.size(); ++h)
    {
        const auto from = static_cast<size_t> (on[h]);
        const auto to = h + 1 < on.size() ? static_cast<size_t> (on[h + 1]) : level.size();
        int peak = 0;
        for (size_t i = from; i < to; ++i)
            peak = std::max (peak, static_cast<int> (level[i]));
        const double threshold = peak * std::pow (10.0, -30.0 / 20.0);
        double onset = -1.0;
        for (size_t i = from; i < to && onset < 0.0; ++i)
            if (peak > 0 && level[i] >= threshold)
                onset = 1000.0 * static_cast<double> (i - from) / rate;
        onsets.push_back (onset);
    }
    return onsets;
}
} // namespace

TEST_CASE ("Clean never waits: every hit starts as soon as the first, for restart modes off, classic and phase, leads 1 to 8, spacings 0.125 to 1 s, both models",
           "[bside][engine][chip][restart][slow]")
{
    // 8.21.2 "Clean never waits": under Clean no note's envelope start waits, whatever the restart mode, the lead or the spacing.
    // Two tones with attack 0 (the case the wait needs): the hat of B3 (its own envelope, noise) and a host-envelope pulse (ad 0a, sr 0a).
    auto hostTone = defaultHost();
    hostTone.osc[0].attackDecay = 0x0a;
    hostTone.osc[0].sustainRelease = 0x0a;
    int cases = 0;
    double worst = 0.0;
    for (const auto model : { chip::Model::B, chip::Model::A })
        for (const char* mode : { "off", "classic", "phase" })
            for (int lead = 1; lead <= 8; ++lead)
                for (const double spacing : { 0.125, 0.25, 0.35, 0.5, 0.7, 1.0 })
                    for (int tone = 0; tone < 2; ++tone)
                    {
                        const auto program = compileText (tone == 0 ? hatTable (mode, lead)
                                                                    : std::string ("bside-table 1\n[restart]\nmode ") + mode + "\nlead " + std::to_string (lead) + "\n");
                        const auto onsets = cleanOnsets (model, program, tone == 0 ? defaultHost() : hostTone, spacing, 5);
                        std::string row;
                        for (const double o : onsets)
                            row += std::to_string (o) + " ";
                        INFO ("model " << (model == chip::Model::A ? "A" : "B") << ", mode " << mode << ", lead " << lead << ", spacing " << spacing
                                       << " s, tone " << tone << ": onsets (ms) " << row);
                        REQUIRE (onsets.size() == 5);
                        for (const double o : onsets)
                        {
                            CHECK (o >= 0.0);
                            CHECK (std::abs (o - onsets[0]) <= 1.0);
                            worst = std::max (worst, std::abs (o - onsets[0]));
                        }
                        ++cases;
                    }
    std::printf ("Clean never waits: %d cases, the largest onset difference from the first hit %.3f ms\n", cases, worst);
}
