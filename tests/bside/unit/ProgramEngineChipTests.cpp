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

TEST_CASE ("chip sink: a steal on the real chip resets the chip but not the engine's time; ticks go on", "[bside][engine][chip]")
{
    Drive d (true);
    d.engine.setHost (defaultHost());
    const auto program = compileText (kTable);
    d.engine.noteOn (0, program, RestartPolicy::clean, false, d.sink);
    d.runTo (3 * kFrame + 100);
    const auto before = d.chip.cycle();
    CHECK (before == 3 * kFrame + 100);
    // steal: the engine is told at its absolute cycle; the chip restarts from 0
    d.engine.noteOn (3 * kFrame + 100, program, RestartPolicy::clean, true, d.sink);
    CHECK (d.chip.cycle() == 0);
    CHECK (d.sink.cycle() == 3 * kFrame + 100);
    d.runTo (8 * kFrame);
    CHECK (d.sink.cycle() == 8 * kFrame);
    CHECK (d.chip.cycle() == 8 * kFrame - (3 * kFrame + 100));
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

TEST_CASE ("chip sink: after a reset the sink's time is the engine's", "[bside][engine][chip]")
{
    chip::Chip c;
    c.prepare (config (true));
    pe::ChipProgramSink sink (c, true);
    c.advanceCycles (5000);
    CHECK (sink.cycle() == 5000);
    sink.resetChip (5000);
    CHECK (c.cycle() == 0);
    CHECK (sink.cycle() == 5000);
    c.advanceCycles (700);
    CHECK (sink.cycle() == 5700);
    sink.resetChip (5700);
    CHECK (sink.cycle() == 5700);
}

TEST_CASE ("chip sink: Clean's direct clear of the envelope rate counters", "[bside][engine][chip]")
{
    SKIP ("chip entry point for clearing the envelope rate counters is not built (ChipProgramSink::clearEnvelopeTimers is a no-op); open item of S2");
}
