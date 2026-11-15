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
