#pragma once

// The chip's digital core (docs/architecture.md 8.4.1, 8.4.7 and the per-cycle specification 8.15,
// which wins over 8.4.3): register file, three oscillators (24-bit accumulators, sync, ring, test bit,
// 23-bit noise registers with their latch, pipeline, fill and write-back), three envelope generators
// (pipelined phase, counter and divider, 32 767-position rate counter, delay quirk, freeze at zero) and
// the two read registers (Model B read latch, fade of a deselected output). JUCE-free; no allocation,
// no exceptions, no locks; everything the audio thread calls is noexcept. The object is trivially
// copyable (it owns no memory), which the tests assert.
//
// ---- The normative definition: one cycle (stepCycle) ----
// One call is one step of 8.15.2 and advances cycle() by 1: (a) oscillator advance 1, 2, 3
// (oscAdvance); (b) envelope step 1, 2, 3 (envStep, 8.15.4); (c) outputs 1, then 2, then 3
// (oscOutput); (d) sync 1, 2, 3 (syncStep). Writes (write / setOscillator) apply between two steps
// (8.15.1, 8.15.4 "Writes", 8.15.5). Power-on (reset) is 8.15.1. Each private step function below quotes
// the rule it implements; the specification text is the reference, this file follows it literally.
// Our chip clears an oscillator's rise flag while its test bit is held (8.15.2 (a), 8.15.6).
//
// ---- The fast path (advanceCycles) ----
// Equal to repeated stepCycle (proven by test, states and events). Per call it steps whole cycles with
// stepCycle while any oscillator writes back into its noise register (noise with another waveform) or,
// on Model A, combines the saw with another waveform (the top-bit pull-down), and for every cycle in
// which an oscillator's top bit rises while its sync target has its sync bit set. In between, the
// oscillators are independent of each other and of the envelopes, so the jump (skip) advances each one
// on its own: accumulator in closed form, bit-19 rises and the noise pipeline event by event, fill and
// fade timers event by event, and the output, read value, Model B latch and pulse comparison computed
// from the last two cycles of the jump. Envelopes jump over runs of pure rate-counter increments and
// call the same envStep for every other cycle. Jumps are at most kMaxSkip cycles.
//
// ---- The seam for S2b (band-limited rendering): time, samples, events ----
// TIME. "The step ending at cycle E" is the stepCycle call that moves cycle() from E - 1 to E; the
// state after it is the state at cycle E, and every event of that step carries E. Chip time t is
// real-valued in cycles; the state at cycle E holds for t in [E, E + 1), so a step's changes happen at
// the instant t = E. A write made while cycle() == C happens at t = C, after the step ending at C.
//
// SAMPLES. Internal sample s starts at T_s = C_s + F_s / 2^32, where C_s = cycle() and
// F_s = sampleClock().fraction(), both read BEFORE takeSampleCycles(). takeSampleCycles() returns
// n_s; the steps of sample s are those ending at E = C_s + 1 .. C_s + n_s (all in (T_s, T_s+1]).
// The sub-sample position of an instant t = E - before (0 <= before < 1) in sample s is
//     p = (E - before - T_s) x 2^32 / sampleClock().increment()        (p in (0, 1] for a step end),
// computed by ChipClock::position (C_s, F_s, increment, E, before).
// ENVELOPE: the envelope value that belongs to internal sample s is envelope(n) read AFTER the
// sample's n_s cycles have been advanced (the state at C_s + n_s, the sample's end); envelope steps
// are applied per sample and not band-limited (8.4.5).
//
// EVENTS. advanceCycles (n, sink) / stepCycle (sink) report what a renderer cannot infer from the
// per-sample state. Events are self-contained: A SINK MUST NOT READ THE CHIP FROM INSIDE A CALLBACK
// (inside a jump the oscillators are advanced one after the other, so the chip's state at callback
// time differs from the state at the event's instant).
//   syncReset (SyncResetEvent)    always reported. The target's accumulator becomes 0 at t = E. The
//       source's top bit crossed 2^23 at t = E - before, before = (sourcePhase - 2^55) / sourceIncrement
//       in [0, 1) (SyncResetEvent::crossingBefore()); targetPhaseBefore is the target's 56-bit phase
//       just before the reset.
//   noiseClock (NoiseClockEvent)  when Sink::kWantsNoiseEvents: one per change of a noise register by a
//       shift (2 cycles after a bit-19 rise; `fill` false) or by a fill step under the test bit (`fill`
//       true), at t = E; carries the register and the noise output right after it.
//   noiseWriteBack (NoiseWriteBackEvent)  when Sink::kWantsNoiseEvents: whenever the write-back of
//       (c) step 6 changed the register; register and noise output after it. Write-back mode is never
//       jumped, so these events come in step order.
//   Changes made by a write (the noise shift when the test bit is cleared, the accumulator reset when it
//   is set) carry no event: a renderer splits the sample at the write and reads the state there. The same
//   holds for powerOnKeepingTime() (accumulators, outputs and envelopes jump to power-on) and
//   clearEnvelopeCounters(): no event; the renderer splits the sample where it calls them.
// ORDER. Within one step: noise clocks (oscillator 1..3, part (a)), write-backs (1..3, part (c)), sync
// resets (by source 1..3, part (d)). A jump contains noise clocks only and reports them OSCILLATOR BY
// OSCILLATOR (each in time order), NOT by time across oscillators. Stable-sorted by cycle, the events
// of advanceCycles equal those of the same cycles stepped one by one (tested).
// STEP EVENTS. A sink that declares `static constexpr bool kWantsStepEvents = true` also receives
// oscStep (OscStepEvent) once per oscillator per step, after that step's sync resets (the replay's
// 8.15.8 exemption tracking); then advanceCycles steps every cycle.
//
// Between events an oscillator is a straight line: phase(n) + k x increment(n) (mod 2^56), from which
// saw wraps, top-bit flips and pulse edges (bits 23..12 crossing PW) follow in closed form.
// pulseWidthSmooth(n) is the continuous pulse width (Smooth), output(n) the 12-bit digital output.

#include "bside/chip/ChipClock.h"
#include "bside/chip/ChipTypes.h"
#include "bside/chip/RenderTables.h"
#include "bside/chip/Waveforms.h"
#include "common/dsp/BlepTable.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

namespace vs::bside::chip
{
/** One oscillator's digital state (8.15.1, 8.15.2). Integer fields only; operator== compares all. */
struct OscState
{
    std::uint32_t acc = kAccPowerOn;      // 24-bit accumulator
    std::uint32_t accFraction = 0;        // below bit 0 (Smooth only)
    std::uint64_t increment = 0;          // F in 32.32 fixed point
    std::uint16_t pulseWidth = 0;         // 12-bit value the pulse comparison uses
    std::uint32_t noise = kNoisePowerOn;  // 23-bit shift register
    std::uint32_t noiseLatch = kNoisePowerOn;
    std::uint16_t noiseOutput = 0;        // 12-bit noise output (bits 3..0 are 0)
    std::uint8_t noisePipeline = 0;       // 2 after a bit-19 rise; 1 latch; 0 shift
    bool firstShift = true;
    bool rise = false;                    // bit 23 rose in this step's addition (after the pull-down)
    std::uint32_t fillTimer = 0;          // 0 = stopped
    std::uint32_t fadeTimer = 0;          // 0 = stopped
    std::uint16_t output = 0;             // 12-bit output of (c)
    std::uint16_t readValue = 0;          // 12-bit read value (0x1B shows oscillator 3's bits 11..4)
    std::uint16_t readLatch = kReadLatchPowerOn; // Model B triangle/saw latch
    bool pulseHigh = true;                // pulse comparison, used in the next step
    bool operator== (const OscState&) const = default;
};

/** One envelope's digital state (8.15.1, 8.15.4). */
struct EnvState
{
    std::uint8_t counter = kEnvPowerOn;
    std::uint8_t phase = static_cast<std::uint8_t> (EnvPhase::release);  // EnvPhase
    std::uint8_t target = static_cast<std::uint8_t> (EnvPhase::release); // EnvPhase
    std::uint8_t phasePipeline = 0, stepPipeline = 0, dividerPipeline = 0;
    std::uint16_t position = 0;           // rate counter, 0 .. 32 766
    std::uint16_t compare = kRateMatch[0];// match position of the current rate
    bool resetPending = true;
    std::uint8_t divider = 0;             // divider counter
    std::uint8_t dividerPeriod = 1;
    std::uint8_t pendingPeriod = 0;       // 0 = no period change pending
    bool frozen = false;
    std::uint8_t readSample = 0;          // the counter at the start of the step (0x1C for envelope 3)
    bool operator== (const EnvState&) const = default;
};

/** The complete digital state; operator== compares every field. */
struct DigitalState
{
    std::uint64_t cycle = 0;
    std::array<OscState, 3> osc {};
    std::array<EnvState, 3> env {};
    std::array<std::uint8_t, reg::kWritable> registers {}; // last written values, 0x00..0x18
    std::uint8_t osc3Output = 0;                             // read register 0x1B
    std::uint8_t env3Output = 0;                             // read register 0x1C
    bool operator== (const DigitalState&) const = default;
};

/** A sync reset (see "EVENTS" above). Oscillators are 0-based. */
struct SyncResetEvent
{
    int target = 0, source = 0;
    std::uint64_t cycle = 0;             // E: the step that reset the target ends here
    std::uint64_t sourcePhase = 0;       // source's 56-bit phase after its add in that step (>= 2^55)
    std::uint64_t sourceIncrement = 0;   // source's increment (32.32), > 0
    std::uint64_t targetPhaseBefore = 0; // target's 56-bit phase just before the reset
    /** How long before E the source crossed its top bit, in cycles, in [0, 1). */
    double crossingBefore() const noexcept
    {
        return static_cast<double> (sourcePhase - kPhaseMsb) / static_cast<double> (sourceIncrement);
    }
    bool operator== (const SyncResetEvent&) const = default;
};

/** A change of a noise register by a shift or a fill step in the step ending at `cycle`. */
struct NoiseClockEvent
{
    int osc = 0;
    std::uint64_t cycle = 0;
    std::uint32_t noiseRegister = 0; // after the change
    std::uint16_t output = 0;        // the noise output after it (wave::noise (noiseRegister))
    bool fill = false;               // a fill step under the test bit (else a shift)
    bool operator== (const NoiseClockEvent&) const = default;
};

/** A change of a noise register by the combined-waveform write-back in the step ending at `cycle`. */
struct NoiseWriteBackEvent
{
    int osc = 0;
    std::uint64_t cycle = 0;
    std::uint32_t noiseRegister = 0; // after the write-back
    std::uint16_t output = 0;        // the noise output after it
    bool operator== (const NoiseWriteBackEvent&) const = default;
};

/** What one oscillator did in one step (for sinks with kWantsStepEvents; the 8.15.8 exemption). */
struct OscStepEvent
{
    int osc = 0;
    std::uint64_t cycle = 0;
    std::uint8_t control = 0;  // the control register during the step
    bool shifted = false;      // (a): a noise shift
    bool shiftWroteBack = false; // (a): that shift replaced the latch's tap bits by the output
    bool filled = false;       // (a): a fill step
    bool filledToOnes = false; // (a): the fill made the register all ones (the timer stopped)
    bool outputComputed = false; // (c): a waveform bit was set
    bool faded = false;        // (c): a fade step of a deselected output
    bool wroteBack = false;    // (c) step 6: the register was ANDed with the output
    bool pulledDown = false;   // (c) step 4: Model A cleared the top bit
};

/** An event sink that ignores everything (the digital-only path). A sink provides
      static constexpr bool kWantsNoiseEvents;            // noiseClock and noiseWriteBack are called only if true
      void syncReset (const SyncResetEvent&);
      void noiseClock (const NoiseClockEvent&);
      void noiseWriteBack (const NoiseWriteBackEvent&);
    and optionally `static constexpr bool kWantsStepEvents = true; void oscStep (const OscStepEvent&);`.
    It must not read the chip from inside a callback. */
struct NullSink
{
    static constexpr bool kWantsNoiseEvents = false;
    void syncReset (const SyncResetEvent&) noexcept {}
    void noiseClock (const NoiseClockEvent&) noexcept {}
    void noiseWriteBack (const NoiseWriteBackEvent&) noexcept {}
};

namespace detail
{
template <class Sink>
constexpr bool wantsStepEvents() noexcept
{
    if constexpr (requires { Sink::kWantsStepEvents; })
        return Sink::kWantsStepEvents;
    else
        return false;
}

/** The period change pending after the counter takes `counter` (8.15.4 step 4), 0 for none. */
constexpr std::uint8_t periodFor (std::uint8_t counter) noexcept
{
    switch (counter)
    {
        case 0xff:
        case 0x00: return 1;
        case 0x5d: return 2;
        case 0x36: return 4;
        case 0x1a: return 8;
        case 0x0e: return 16;
        case 0x06: return 30;
        default: return 0;
    }
}

constexpr int syncTarget (int i) noexcept { return i == 2 ? 0 : i + 1; }
constexpr int syncSource (int i) noexcept { return i == 0 ? 2 : i - 1; }
} // namespace detail

class Chip
{
public:
    /** Longest jump of the fast path (keeps jump x increment inside 64 bits). */
    static constexpr std::uint64_t kMaxSkip = 4096;

    /** Power-on state with the default Config (clock and internal rate included, so a chip that is
        never prepared still advances by samples). */
    Chip() noexcept
    {
        clock.prepare (cfg.clockHz, cfg.internalRate);
        shapes = &combined::tablesFor (cfg.model, cfg.shapes);
        rtab = &render::tablesFor (cfg.model, cfg.shapes);
        blep = &ShortBlep::get();
        blepLong = &LongBlep::get();
        reset();
    }

    /** Model, clock, internal rate and shape set; resets the chip. Allocates nothing (no member owns
        memory): the combined-waveform tables, the renderer's tables (RenderTables.h, about 180 KiB per
        model and shape set, a few milliseconds to build) and both BLEP tables are process-wide and built on
        their first use, which this call (and the constructor) always makes, never a step or a render.
        Construct and prepare a Chip off the audio thread: the first one in a process builds the tables. */
    void prepare (const Config& c) noexcept;
    /** Power-on state at cycle 0 (8.15.1; the configuration is kept). */
    void reset() noexcept;

    /** Power-on state (8.15.1) of every oscillator, envelope, register and the stored filter input, like
        reset(), but cycle() and the sample clock keep running (a stolen voice restarts its chip while the
        program engine keeps absolute cycles). Configuration and shape tables are kept. */
    void powerOnKeepingTime() noexcept;
    /** Clean restart (8.6.4): envelope n's (0..2) rate counter position := 0 and its divider counter (the
        exponential counter) := 0, now, without a register write. Exactly these two fields of EnvState
        (`position`, `divider`) change; the pending rate reset, the divider period and any pending period
        change, the pipelines, phase, counter, freeze and every oscillator field stay as they are. Another n
        is ignored. */
    void clearEnvelopeCounters (int n) noexcept;

    /** A register write at the current cycle (trace replay). The address is taken modulo 32;
        0x19..0x1F are ignored (read-only or not modelled). */
    void write (std::uint8_t reg, std::uint8_t value) noexcept;
    /** Program-engine input of oscillator n (0..2) at the current cycle: frequency and pulse width (with
        the resolution of the configuration), then as if registers 5, 6 and 4 were written in that order. */
    void setOscillator (int n, const OscInput& in) noexcept;
    /** Filter input at the current cycle: stored for the analog stages (no digital effect). */
    void setFilter (const FilterInput& in) noexcept;
    /** 0x1B: oscillator 3's read value bits 11..4; 0x1C: envelope 3's read sample; any other address 0. */
    std::uint8_t read (std::uint8_t reg) const noexcept;

    /** One cycle, exactly as defined at the top of this file. */
    void stepCycle() noexcept
    {
        NullSink s;
        stepCycle (s);
    }
    template <class Sink>
    void stepCycle (Sink& sink) noexcept;

    /** n cycles; equal to n x stepCycle (proven by test), but fast. */
    void advanceCycles (std::uint64_t n) noexcept
    {
        NullSink s;
        advanceCycles (n, s);
    }
    template <class Sink>
    void advanceCycles (std::uint64_t n, Sink& sink) noexcept;

    /** Starts the next internal sample: moves the sample clock and returns the whole cycles the sample
        covers, without advancing the digital state. A renderer then calls advanceCycles for those cycles,
        split at the cycles of any writes inside the sample (S2b). */
    std::uint64_t takeSampleCycles() noexcept { return clock.nextSample(); }

    /** The digital state over n internal samples (no audio, no writes): takeSampleCycles + advanceCycles
        per sample. Returns the cycles advanced. */
    std::uint64_t advanceSamples (int n) noexcept
    {
        std::uint64_t total = 0;
        for (int i = 0; i < n; ++i)
        {
            const auto c = takeSampleCycles();
            advanceCycles (c);
            total += c;
        }
        return total;
    }

    // ---- band-limited rendering (8.4.5; ChipRender.cpp; design at "RENDERING" at the end of this file) ----

    /** Delay of the rendered audio against chip time, in internal samples: output sample m holds the
        signal at the start of internal sample m - kLatencySamples (the BLEP window, 8.4.2; the long kernel's,
        which every step shares: 8). */
    using ShortBlep = vs::dsp::BlepTable;
    using LongBlep = vs::dsp::BasicBlepTable<render::kLongZeroCrossings, render::kLongBetaTenths, double>;
    static constexpr int kLatencySamples = LongBlep::kZeroCrossings;

    /** Renders whole internal samples and ADDS them to out[0 .. n): per oscillator the band-limited
        waveform after the waveform converter (DAC), times the envelope converter, summed over the three
        oscillators (8.4.4 up to the routing; see "RENDERING" for what waits for S3). Advances the digital
        state over the samples' cycles exactly as advanceCycles would. A sample that renderUntil left
        in progress is completed first. */
    void render (float* out, int numInternalSamples) noexcept;

    /** Renders until cycle() == untilCycle or maxSamples samples are complete, whichever comes first;
        ADDS the completed samples to out and returns their count. When untilCycle falls inside a sample
        (or at its last cycle), the sample stays in progress: a write made now lands at its exact cycle,
        and the next render call completes the sample. untilCycle below cycle() renders nothing. */
    int renderUntil (float* out, int maxSamples, std::uint64_t untilCycle) noexcept;

    /** Times the renderer had to move a rendered line back onto the digital phase (a band-limited
        correction; a diagnostic for tests: 0 over the tests' random patches and write schedules; writes a
        few cycles into a sample still give rare ones, almost all below 4e-6 of a period: docs/decisions.md
        S2b-2 row 17). */
    std::uint64_t renderCorrections() const noexcept { return rs.corrections; }
    /** The corrections larger than 1e-4 of a period (the others round the sub-cycle time of a sync reset). */
    std::uint64_t renderLargeCorrections() const noexcept { return rs.largeCorrections; }
    /** Test aid (white-box): when on, every sample end compares the renderer's pulse gate and ring state
        with the chip's own comparison and the ring source's top bit, wherever the digital line is more
        than three cycles from a boundary (wrap, half, width), no write and no prediction is pending; a
        difference is counted. Off by default (one branch per sample). */
    void renderCheckStates (bool on) noexcept { renderCheck = on; }
    std::uint64_t renderStateMismatches() const noexcept { return rs.stateMismatches; }
    /** Times the set of Model A converter bits rendered as square waves changed with the frequency (each a
        small band-limited step; hysteresis keeps a vibrato around a limit from repeating it). */
    std::uint64_t renderConverterSwitches() const noexcept { return rs.converterSwitches; }

    DigitalState state() const noexcept;
    std::uint64_t cycle() const noexcept { return cycleCount; }
    const Config& config() const noexcept { return cfg; }
    const ChipClock& sampleClock() const noexcept { return clock; }

    // ---- read access for S2b and the tests ----
    // Precondition: n is 0, 1 or 2 (asserted in debug builds). Outside that range a release build reads
    // oscillator 1's value (never out of bounds), which is still a caller error.
    std::uint64_t phase (int n) const noexcept { return osc[checked (n)].phase; }
    std::uint64_t increment (int n) const noexcept { return osc[checked (n)].inc; }
    std::uint32_t accumulator (int n) const noexcept { return static_cast<std::uint32_t> (osc[checked (n)].phase >> kFractionBits); }
    std::uint32_t noiseRegister (int n) const noexcept { return osc[checked (n)].noise; }
    std::uint16_t pulseWidth (int n) const noexcept { return osc[checked (n)].pw; }
    float pulseWidthSmooth (int n) const noexcept { return osc[checked (n)].pwSmooth; }
    std::uint8_t control (int n) const noexcept { return ctl (static_cast<int> (checked (n))); }
    std::uint8_t envelope (int n) const noexcept { return env[checked (n)].counter; }
    EnvPhase envelopePhase (int n) const noexcept { return static_cast<EnvPhase> (env[checked (n)].phase); }
    /** The 12-bit digital output of oscillator n (0..2) as the last step computed it (8.15.2 (c); held
        and fading while no waveform is selected). */
    std::uint16_t output (int n) const noexcept { return osc[checked (n)].out; }
    /** The step-2 shape tables in use (the combined-waveform values; S2b renders from the same tables).
        Read a shape only through wave::shape (tables, waveform bits, index, noise output): for pulse with
        noise (waveform bits 0xc) the shape is `pulseNoise[noise output >> 4]` and the row `wave[0xc]` is
        unused zeros. */
    const combined::ShapeTables& shapeTables() const noexcept { return *shapes; }
    float cutoffSmooth() const noexcept { return cutoff; }
    std::uint8_t registerValue (std::uint8_t r) const noexcept { return r < reg::kWritable ? regs[r] : std::uint8_t { 0 }; }

private:
    struct Osc
    {
        std::uint64_t phase = static_cast<std::uint64_t> (kAccPowerOn) << kFractionBits; // accumulator.fraction, 56 bits
        std::uint64_t inc = 0;
        std::uint16_t pw = 0;
        float pwSmooth = 0.0f;
        std::uint32_t noise = kNoisePowerOn;
        std::uint32_t latch = kNoisePowerOn;
        std::uint16_t noiseOut = wave::noise (kNoisePowerOn);
        std::uint8_t noisePipe = 0;
        bool firstShift = true;
        bool rise = false;
        std::uint32_t fillTimer = 0;
        std::uint32_t fadeTimer = 0;
        std::uint16_t out = 0;
        std::uint16_t read = 0;
        std::uint16_t readLatch = kReadLatchPowerOn;
        bool pulseHigh = true;
    };
    using Env = EnvState;

    static std::size_t idx (int n) noexcept { return static_cast<std::size_t> (n); } // internal loops: 0..2 by construction
    static std::size_t checked (int n) noexcept
    {
        assert (n >= 0 && n < 3);
        return static_cast<std::size_t> (n) < 3 ? static_cast<std::size_t> (n) : 0;
    }
    std::uint8_t ctl (int n) const noexcept { return regs[static_cast<std::size_t> (reg::oscStride * n + reg::control)]; }
    std::uint8_t attackDecay (int n) const noexcept { return regs[static_cast<std::size_t> (reg::oscStride * n + reg::attackDecay)]; }
    std::uint8_t sustainRelease (int n) const noexcept { return regs[static_cast<std::size_t> (reg::oscStride * n + reg::sustainRelease)]; }
    std::uint8_t sustainLevel (int n) const noexcept { return static_cast<std::uint8_t> ((sustainRelease (n) >> 4) * 0x11); }
    std::uint16_t attackMatch (int n) const noexcept { return kRateMatch[attackDecay (n) >> 4]; }
    std::uint16_t decayMatch (int n) const noexcept { return kRateMatch[attackDecay (n) & 15u]; }
    std::uint16_t releaseMatch (int n) const noexcept { return kRateMatch[sustainRelease (n) & 15u]; }
    std::uint32_t acc (int n) const noexcept { return static_cast<std::uint32_t> (osc[idx (n)].phase >> kFractionBits); }

    void writeControl (int n, std::uint8_t before) noexcept;
    void writeAttackDecay (int n) noexcept;
    void writeSustainRelease (int n) noexcept;

    // ---- one step, part by part (8.15.2) ----

    /** A noise shift (8.15.3), with the waveform bits before and after (equal for a pipeline shift). */
    bool noiseShift (int n, std::uint8_t oldBits, std::uint8_t newBits) noexcept
    {
        Osc& o = osc[idx (n)];
        const bool writeBack = wave::shiftWritesBack (cfg.model, oldBits, newBits);
        if (writeBack)
            o.latch = (o.latch & ~wave::kTapBits) | wave::taps (o.out);
        const std::uint32_t l = o.latch;
        const std::uint32_t feedback = (((l >> 22) | (o.firstShift ? 1u : 0u)) ^ (l >> 17)) & 1u;
        o.noise = ((l << 1) & kNoiseMask) | feedback;
        o.noiseOut = wave::noise (o.noise);
        return writeBack;
    }

    /** One fill step under the test bit (8.15.2 (a)); returns true when the register became all ones. */
    bool noiseFill (int n) noexcept
    {
        Osc& o = osc[idx (n)];
        o.noise = (o.noise | (o.noise << 1) | 1u) & kNoiseMask;
        o.latch = o.noise;
        o.noiseOut = wave::noise (o.noise);
        if (o.noise != kNoiseMask)
        {
            o.fillTimer = timingFor (cfg.model).fillInterval;
            return false;
        }
        return true;
    }

    /** One pipeline step of the noise register without a bit-19 rise (8.15.2 (a), the "otherwise"). */
    template <class Sink>
    void noisePipelineStep (int n, std::uint64_t now, Sink& sink, OscStepEvent& ev) noexcept
    {
        Osc& o = osc[idx (n)];
        if (o.noisePipe == 0)
            return;
        if (--o.noisePipe == 1)
        {
            o.latch = o.noise;
            o.firstShift = false;
            return;
        }
        const std::uint8_t w = wave::bits (ctl (n));
        ev.shifted = true;
        ev.shiftWroteBack = noiseShift (n, w, w);
        if constexpr (Sink::kWantsNoiseEvents)
            sink.noiseClock (NoiseClockEvent { n, now, o.noise, o.noiseOut, false });
    }

    /** (a) Oscillator advance. */
    template <class Sink>
    void oscAdvance (int n, std::uint64_t now, Sink& sink, OscStepEvent& ev) noexcept
    {
        Osc& o = osc[idx (n)];
        if ((ctl (n) & ctrl::test) != 0)
        {
            if (o.fillTimer != 0 && --o.fillTimer == 0)
            {
                ev.filled = true;
                ev.filledToOnes = noiseFill (n);
                if constexpr (Sink::kWantsNoiseEvents)
                    sink.noiseClock (NoiseClockEvent { n, now, o.noise, o.noiseOut, true });
            }
            o.firstShift = true;
            o.rise = false; // our chip (8.15.2 (a), 8.15.6)
            return;
        }
        const std::uint64_t prev = o.phase;
        const std::uint64_t next = (prev + o.inc) & kPhaseMask;
        o.phase = next;
        o.rise = (~prev & next & kPhaseMsb) != 0;
        if ((~prev & next & kPhaseBit19) != 0)
            o.noisePipe = 2;
        else
            noisePipelineStep (n, now, sink, ev);
    }

    /** The output of (c) steps 1-3 for oscillator n with the given accumulators and inputs. */
    struct OutParts
    {
        std::uint16_t shape, pulseMask, noiseMask;
    };
    OutParts outParts (int n, std::uint32_t ownAcc, std::uint32_t sourceAcc, bool pulsePrev) const noexcept
    {
        const std::uint8_t c = ctl (n);
        const std::uint16_t ix = wave::index (ownAcc, c, sourceAcc);
        OutParts p;
        const std::uint16_t noiseOut = osc[idx (n)].noiseOut;
        p.shape = wave::shape (*shapes, wave::bits (c), ix, noiseOut);
        p.pulseMask = (c & ctrl::pulse) == 0 || (c & ctrl::test) != 0 || pulsePrev ? std::uint16_t { 0xfff } : std::uint16_t { 0 };
        p.noiseMask = (c & ctrl::noise) != 0 ? noiseOut : std::uint16_t { 0xfff };
        return p;
    }

    bool pulseCompare (int n, std::uint32_t a) const noexcept { return ((a >> 12) & 0xfffu) >= osc[idx (n)].pw; }

    /** (c) Output of oscillator n. */
    template <class Sink>
    void oscOutput (int n, std::uint64_t now, Sink& sink, OscStepEvent& ev) noexcept
    {
        Osc& o = osc[idx (n)];
        const std::uint8_t c = ctl (n);
        if (wave::bits (c) != 0)
        {
            ev.outputComputed = true;
            const OutParts p = outParts (n, acc (n), acc (detail::syncSource (n)), o.pulseHigh);
            o.out = static_cast<std::uint16_t> (p.shape & p.pulseMask & p.noiseMask);
            if (cfg.model == Model::A && (c & ctrl::saw) != 0 && (o.out & 0x800u) == 0)
            {
                ev.pulledDown = (o.phase & kPhaseMsb) != 0;
                o.phase &= ~kPhaseMsb;
                o.rise = false;
            }
            if (cfg.model == Model::B && (c & (ctrl::triangle | ctrl::saw)) != 0)
            {
                o.read = static_cast<std::uint16_t> (o.readLatch & p.pulseMask & p.noiseMask);
                o.readLatch = p.shape;
            }
            else
                o.read = o.out;
            if (wave::writesBack (c))
            {
                if (o.noisePipe != 1 && (c & ctrl::test) == 0)
                {
                    ev.wroteBack = true;
                    const std::uint32_t before = o.noise;
                    o.noise &= wave::noiseKeepMask (o.out);
                    o.noiseOut &= o.out;
                    if constexpr (Sink::kWantsNoiseEvents)
                        if (o.noise != before)
                            sink.noiseWriteBack (NoiseWriteBackEvent { n, now, o.noise, o.noiseOut });
                }
                else
                    o.noiseOut = o.out;
            }
        }
        else if (o.fadeTimer != 0 && --o.fadeTimer == 0)
        {
            ev.faded = true;
            fadeStep (n);
        }
        o.pulseHigh = pulseCompare (n, acc (n));
    }

    void fadeStep (int n) noexcept
    {
        Osc& o = osc[idx (n)];
        o.out = static_cast<std::uint16_t> (o.out & (o.out >> 1));
        o.read = o.out;
        if (o.out != 0)
            o.fadeTimer = timingFor (cfg.model).fadeInterval;
    }

    /** (d) Sync. */
    template <class Sink>
    void syncStep (std::uint64_t now, Sink& sink) noexcept
    {
        const std::uint64_t before[3] = { osc[0].phase, osc[1].phase, osc[2].phase };
        for (int n = 0; n < 3; ++n)
        {
            const int t = detail::syncTarget (n);
            const int s = detail::syncSource (n);
            if (osc[idx (n)].rise && (ctl (t) & ctrl::sync) != 0 && ! ((ctl (n) & ctrl::sync) != 0 && osc[idx (s)].rise))
            {
                sink.syncReset (SyncResetEvent { t, n, now, before[n], osc[idx (n)].inc, before[t] });
                osc[idx (t)].phase = 0;
            }
        }
    }

    /** (b) One envelope step (8.15.4 "Per step"). */
    void envStep (int n) noexcept
    {
        Env& e = env[idx (n)];
        // 1.
        e.readSample = e.counter;
        // 2.
        if (e.pendingPeriod != 0)
        {
            e.dividerPeriod = e.pendingPeriod;
            e.pendingPeriod = 0;
        }
        // 3.
        if (e.phasePipeline > 0)
        {
            const std::uint8_t p = --e.phasePipeline;
            switch (static_cast<EnvPhase> (e.target))
            {
                case EnvPhase::attack:
                    if (p == 1)
                        e.compare = decayMatch (n);
                    else if (p == 0)
                    {
                        e.phase = static_cast<std::uint8_t> (EnvPhase::attack);
                        e.compare = attackMatch (n);
                        e.frozen = false;
                    }
                    break;
                case EnvPhase::decaySustain:
                    if (p == 0)
                    {
                        e.phase = static_cast<std::uint8_t> (EnvPhase::decaySustain);
                        e.compare = decayMatch (n);
                    }
                    break;
                case EnvPhase::release:
                    if ((e.phase == static_cast<std::uint8_t> (EnvPhase::attack) && p == 0)
                        || (e.phase == static_cast<std::uint8_t> (EnvPhase::decaySustain) && p == 1))
                    {
                        e.phase = static_cast<std::uint8_t> (EnvPhase::release);
                        e.compare = releaseMatch (n);
                    }
                    break;
            }
        }
        // 4. counter step
        bool used = false;
        if (e.stepPipeline > 0 && --e.stepPipeline == 0)
        {
            used = true;
            if (! e.frozen)
            {
                if (e.phase == static_cast<std::uint8_t> (EnvPhase::attack))
                {
                    ++e.counter;
                    if (e.counter == 0xff)
                    {
                        e.target = static_cast<std::uint8_t> (EnvPhase::decaySustain);
                        e.phasePipeline = 3;
                    }
                }
                else if (--e.counter == 0)
                    e.frozen = true;
                e.pendingPeriod = detail::periodFor (e.counter);
            }
        }
        // 5. divider pipeline
        if (! used && e.dividerPipeline > 0 && --e.dividerPipeline == 0)
        {
            used = true;
            e.divider = 0;
            if ((e.phase == static_cast<std::uint8_t> (EnvPhase::decaySustain) && e.counter != sustainLevel (n))
                || e.phase == static_cast<std::uint8_t> (EnvPhase::release))
                e.stepPipeline = 1;
        }
        // 6. rate reset
        if (! used && e.resetPending)
        {
            e.position = 0;
            e.resetPending = false;
            if (e.phase == static_cast<std::uint8_t> (EnvPhase::attack))
            {
                e.divider = 0;
                e.stepPipeline = 2;
            }
            else if (! e.frozen)
            {
                ++e.divider;
                if (e.divider == e.dividerPeriod)
                    e.dividerPipeline = e.dividerPeriod == 1 ? 1 : 2;
            }
        }
        // 7. rate counter
        if (e.position != e.compare)
            e.position = static_cast<std::uint16_t> (e.position + 1 == kRatePositions ? 0 : e.position + 1);
        else
            e.resetPending = true;
    }

    /** True while an envelope step would only move the rate counter by one (no pipeline, no pending
        change or reset, not at the match position). */
    static bool envQuiet (const Env& e) noexcept
    {
        return e.phasePipeline == 0 && e.stepPipeline == 0 && e.dividerPipeline == 0 && e.pendingPeriod == 0 && ! e.resetPending
               && e.position != e.compare;
    }

    /** j envelope steps of envelope n: runs of pure increments in one go, every other step by envStep. */
    void envAdvance (int n, std::uint64_t j) noexcept
    {
        Env& e = env[idx (n)];
        while (j > 0)
        {
            if (envQuiet (e))
            {
                const std::uint64_t d = (static_cast<std::uint64_t> (e.compare) + kRatePositions - e.position) % kRatePositions;
                const std::uint64_t t = std::min (d, j);
                e.position = static_cast<std::uint16_t> ((e.position + t) % kRatePositions);
                e.readSample = e.counter;
                j -= t;
            }
            else
            {
                envStep (n);
                --j;
            }
        }
    }

    /** Whether the fast path must step every cycle (8.15.2 (c) steps 4 and 6 act every cycle). */
    bool mustStep() const noexcept
    {
        for (int n = 0; n < 3; ++n)
        {
            const std::uint8_t c = ctl (n);
            if (wave::writesBack (c))
                return true;
            if (cfg.model == Model::A && (c & ctrl::saw) != 0 && wave::isCombined (wave::bits (c)))
                return true;
        }
        return false;
    }

    /** Cycles to the next step in which a top bit rises while its sync target has the sync bit set
        (>= 1), at most kMaxSkip + 1. */
    std::uint64_t nextSyncRise() const noexcept
    {
        std::uint64_t e = kMaxSkip + 1;
        for (int n = 0; n < 3; ++n)
        {
            const Osc& o = osc[idx (n)];
            if ((ctl (n) & ctrl::test) != 0 || o.inc == 0 || (ctl (detail::syncTarget (n)) & ctrl::sync) == 0)
                continue;
            const std::uint64_t boundary = o.phase < kPhaseMsb ? kPhaseMsb : kPhaseMsb + (kPhaseMask + 1);
            const std::uint64_t d = boundary - o.phase;
            e = std::min (e, (d + o.inc - 1) / o.inc);
        }
        return e;
    }

    /** j cycles (1 <= j <= kMaxSkip) with no sync reset and no cycle that mustStep() covers. */
    template <class Sink>
    void skip (std::uint64_t j, Sink& sink) noexcept;

    // ---- rendering state and steps (ChipRender.cpp) ----
    // per-oscillator output ring: a power of two >= 2 x kLatencySamples + 2
    static constexpr int kRing = 2 * kLatencySamples + 2 <= 32 ? 32 : 2 * kLatencySamples + 2 <= 64 ? 64 : 128;
    enum RenderMode : std::uint8_t { rmOff, rmTri, rmSaw, rmPulse, rmNoise, rmTable, rmCycle };
    struct RenderOsc
    {
        double u = 0.0;          // rendered phase in periods
        double offU = 0.0;       // rendered minus digital phase, periods (set by a sync reset)
        std::int64_t k = 0;      // boundaries passed: k x sigma <= u (triangle, saw; combined: the index cell)
        int imin = 11;           // lowest index bit rendered as its own square (sigma = 2^imin / 4096); 0 for combined
        std::uint8_t mode = rmOff;
        bool gate = true;        // pulse comparison as rendered
        double gateCheckU = -1.0; // after a phase jump: where the (one cycle late) comparison reads the new line
        bool ring = false;       // the ring fold applies to this oscillator's index
        bool ringFlip = false;   // ... and the source's top bit is low: index bit 11 inverted
        bool longKernel = false; // this oscillator's steps use the long kernel (Model A combined waveform)
        bool pending = false;    // a predicted sync reset the digital core has not confirmed yet
        bool pendingPull = false; // the pending event is a Model A pull-down (renderPullDown, renderLanding)
        std::uint64_t pendingStart = 0; // ... predicted at pendingT cycles after this sample start
        double pendingT = 0.0;
        bool glitchPending = false;     // a ring flip back carried into the next stretch (oscillator 1's source)
        std::uint64_t cmpCycle = ~std::uint64_t { 0 }; // the cycle of the last write, and the pulse comparison
        bool cmpHigh = false;                            // the chip's next step used then
        bool landPull = false;          // a pull-down scheduled at the chip's own landing (renderLanding)
        bool landHold = false;          // ... and no other pull-down until the line leaves the upper half
        std::uint64_t landStart = 0;
        double landT = 0.0;
        bool shownTop = false;          // per-cycle source: the top bit last shown to its ring target
        std::uint64_t glitchStart = 0;
        double glitchT = 0.0;
        std::uint16_t held = 0;  // the digital value shown in noise, per-cycle and off modes
        std::array<double, kRing> buf {}; // double: a nearly silent shape on a large offset keeps its detail
        std::array<float, kRing> env {};
    };
    struct RenderState
    {
        bool active = false;      // render has run since reset (writes then feed the renderer)
        bool inProgress = false;  // a sample has taken its cycles and is not finished
        std::uint64_t sample = 0; // index of the sample in progress (or the next one)
        std::uint64_t start = 0;  // C_s: cycle() when the sample started
        std::uint64_t cycles = 0; // n_s: whole cycles of the sample
        double tStart = 0.0, tEnd = 0.0; // the sample's start and end, in cycles since C_s
        double sweepT = 0.0;      // how far the oscillators' lines have been rendered, cycles since C_s
        double cps = 1.0;         // cycles per internal sample
        std::array<RenderOsc, 3> osc {};
        std::array<double, 3> leftValue {}, leftSlope {}, leftSlopeU {}; // captured before a write
        std::uint64_t corrections = 0; // re-anchorings that moved a line (diagnostic)
        std::uint64_t largeCorrections = 0; // of those, the ones above 1e-4 of a period
        std::uint64_t stateMismatches = 0;  // renderCheckStates: gate or ring state unlike the chip's
        std::uint64_t converterSwitches = 0; // changes of the set of Model A converter bits rendered (diagnostic)
    };
    struct PieceEvents; // ChipRender.cpp

    void renderActivate() noexcept;
    void renderStartSample() noexcept;
    void renderPiece (std::uint64_t cycles, bool toSampleEnd) noexcept;
    float renderFinishSample() noexcept;
    void renderBeforeChange() noexcept;
    void renderAfterChange (int testSetMask) noexcept;
    void renderSyncOsc (int n) noexcept;
    void renderSweep (int n, double tEnd, const PieceEvents* ev) noexcept;
    void renderLines (PieceEvents& ev, double tEnd) noexcept;
    bool renderPullDown (int n, double t, bool atWrite) noexcept;
    int renderPullCell (int n, int cell, double tCross) const noexcept;
    bool renderGateAtRise (int n, double tCross) const noexcept;
    bool renderGateAfterRise (int n, double tCross) const noexcept;
    void renderLanding (int n, double tw) noexcept;
    void renderCheckSample() noexcept;
    bool renderCheck = false;
    void renderEmit (int n, double t, double dv, double ds) noexcept;
    void renderEmitWith (int n, double t, double dv, double ds, bool longKernel) noexcept;
    double renderSlopeU (int n) const noexcept; // periods per cycle
    double renderDigitalU (int n, double t) const noexcept;
    double renderValue (int n, double u) const noexcept;
    double renderDerivative (int n, double u) const noexcept; // per period
    double renderValueAt (int n, double u) noexcept;          // with the boundary state of u itself
    double renderDerivativeAt (int n, double u) noexcept;
    double renderNextLineEvent (int n, double t, double slope, double& uEvent, int& kind) const noexcept;
    std::uint8_t renderMode (int n) const noexcept;

    Config cfg {};
    const combined::ShapeTables* shapes = nullptr; // never null after construction (CombinedWaveforms.h)
    const render::RenderTables* rtab = nullptr;    // never null after construction (RenderTables.h)
    const ShortBlep* blep = nullptr;               // never null after construction
    const LongBlep* blepLong = nullptr;            // never null after construction
    RenderState rs {};
    ChipClock clock {};
    std::array<Osc, 3> osc {};
    std::array<Env, 3> env {};
    std::array<std::uint8_t, reg::kWritable> regs {};
    float cutoff = 0.0f;
    std::uint64_t cycleCount = 0;
};

template <class Sink>
void Chip::stepCycle (Sink& sink) noexcept
{
    const std::uint64_t now = cycleCount + 1;
    OscStepEvent ev[3] {};
    for (int n = 0; n < 3; ++n)
    {
        ev[n].osc = n;
        ev[n].cycle = now;
        ev[n].control = ctl (n);
    }
    for (int n = 0; n < 3; ++n)
        oscAdvance (n, now, sink, ev[n]);
    for (int n = 0; n < 3; ++n)
        envStep (n);
    for (int n = 0; n < 3; ++n)
        oscOutput (n, now, sink, ev[n]);
    syncStep (now, sink);
    if constexpr (detail::wantsStepEvents<Sink>())
        for (int n = 0; n < 3; ++n)
            sink.oscStep (ev[n]);
    cycleCount = now;
}

template <class Sink>
void Chip::skip (std::uint64_t j, Sink& sink) noexcept
{
    const std::uint64_t start = cycleCount;
    OscStepEvent unused {};
    std::uint64_t lastPhase[3] {}, prevPhase[3] {};

    // (a), oscillator by oscillator
    for (int n = 0; n < 3; ++n)
    {
        Osc& o = osc[idx (n)];
        if ((ctl (n) & ctrl::test) != 0)
        {
            std::uint64_t left = j;
            while (o.fillTimer != 0 && o.fillTimer <= left)
            {
                left -= o.fillTimer;
                o.fillTimer = 0;
                noiseFill (n);
                if constexpr (Sink::kWantsNoiseEvents)
                    sink.noiseClock (NoiseClockEvent { n, start + (j - left), o.noise, o.noiseOut, true });
            }
            if (o.fillTimer != 0)
                o.fillTimer -= static_cast<std::uint32_t> (left);
            o.firstShift = true;
            o.rise = false;
            prevPhase[n] = lastPhase[n] = o.phase;
            continue;
        }
        // Bit-19 rises: the phase crosses (2m + 1) x 2^51 for some m, i.e. phase + 2^51 crosses a multiple of 2^52.
        constexpr std::uint64_t half = kPhaseBit19;
        constexpr int periodShift = 52;
        const std::uint64_t end = o.phase + j * o.inc; // < 2^56 + 2^63: no overflow
        const std::uint64_t first = (o.phase + half) >> periodShift;
        const std::uint64_t rises = ((end + half) >> periodShift) - first;
        std::uint64_t at = 0; // steps done
        for (std::uint64_t k = 1; k <= rises; ++k)
        {
            const std::uint64_t boundary = ((first + k) << periodShift) - half;
            const std::uint64_t riseStep = (boundary - o.phase + o.inc - 1) / o.inc; // 1 .. j
            while (at + 1 < riseStep && o.noisePipe > 0)
            {
                ++at;
                noisePipelineStep (n, start + at, sink, unused);
            }
            at = riseStep;
            o.noisePipe = 2;
        }
        while (at < j && o.noisePipe > 0)
        {
            ++at;
            noisePipelineStep (n, start + at, sink, unused);
        }
        prevPhase[n] = (o.phase + (j - 1) * o.inc) & kPhaseMask;
        lastPhase[n] = end & kPhaseMask;
        o.rise = (~prevPhase[n] & lastPhase[n] & kPhaseMsb) != 0;
    }
    for (int n = 0; n < 3; ++n)
        osc[idx (n)].phase = lastPhase[n];

    // (b)
    for (int n = 0; n < 3; ++n)
        envAdvance (n, j);

    // (c): the state after the last step depends only on the last two steps (no pull-down, no write-back here)
    for (int n = 0; n < 3; ++n)
    {
        Osc& o = osc[idx (n)];
        const std::uint8_t c = ctl (n);
        const auto lastAcc = static_cast<std::uint32_t> (lastPhase[n] >> kFractionBits);
        if (wave::bits (c) != 0)
        {
            const int s = detail::syncSource (n);
            const bool pulsePrev = j >= 2 ? pulseCompare (n, static_cast<std::uint32_t> (prevPhase[n] >> kFractionBits)) : o.pulseHigh;
            const OutParts p = outParts (n, lastAcc, static_cast<std::uint32_t> (lastPhase[s] >> kFractionBits), pulsePrev);
            o.out = static_cast<std::uint16_t> (p.shape & p.pulseMask & p.noiseMask);
            if (cfg.model == Model::B && (c & (ctrl::triangle | ctrl::saw)) != 0)
            {
                const std::uint16_t latchBefore = j >= 2 ? outParts (n, static_cast<std::uint32_t> (prevPhase[n] >> kFractionBits),
                                                                     static_cast<std::uint32_t> (prevPhase[s] >> kFractionBits), true)
                                                               .shape
                                                         : o.readLatch;
                o.read = static_cast<std::uint16_t> (latchBefore & p.pulseMask & p.noiseMask);
                o.readLatch = p.shape;
            }
            else
                o.read = o.out;
        }
        else
        {
            std::uint64_t left = j;
            while (o.fadeTimer != 0 && o.fadeTimer <= left)
            {
                left -= o.fadeTimer;
                o.fadeTimer = 0;
                fadeStep (n);
            }
            if (o.fadeTimer != 0)
                o.fadeTimer -= static_cast<std::uint32_t> (left);
        }
        o.pulseHigh = pulseCompare (n, lastAcc);
    }
    cycleCount = start + j;
}

template <class Sink>
void Chip::advanceCycles (std::uint64_t n, Sink& sink) noexcept
{
    if constexpr (detail::wantsStepEvents<Sink>())
    {
        for (; n > 0; --n)
            stepCycle (sink);
        return;
    }
    while (n > 0)
    {
        if (mustStep())
        {
            stepCycle (sink);
            --n;
            continue;
        }
        const std::uint64_t span = std::min (n, kMaxSkip);
        const std::uint64_t e = nextSyncRise();
        const std::uint64_t k = e > span ? span : e - 1;
        if (k == 0)
        {
            stepCycle (sink);
            --n;
            continue;
        }
        skip (k, sink);
        n -= k;
    }
}

// ==== RENDERING (Chip::render, ChipRender.cpp; docs/architecture.md 8.4.5) ====
//
// WHAT IS RENDERED. Per oscillator: the band-limited waveform after the waveform converter, minus the
// converter's mid-scale (d (0) + d (4095)) / 2, times the envelope converter's output for the sample's
// envelope counter; render() adds the sum of the three oscillators. Of 8.4.4 this is "oscillator output ->
// waveform DAC -> x envelope (envelope DAC)" and a plain sum. NOT yet (S3): the routing bits and the
// filter, oscillator 3 off, the mixer's voice DC (chip.volume_dc), the volume, the board filters, the
// output scale; the analog fade of a deselected output is rendered as the digital core holds and fades it.
//
// THE LINES. Every oscillator is a straight line in phase between events: u (t), in periods, through the
// digital accumulator (u = digital phase + offU, offU being non-zero only after a sync reset, see below).
// Each internal sample is processed in pieces of at most 32 digital cycles: the digital core advances
// (advanceCycles, or stepCycle in per-cycle mode) and reports its events; then each line is swept from the
// previous position to the piece's end, stopping at every event in time order. At an event the waveform's
// value and slope just before and after it give a step (BLEP) and a slope change (BLAMP) from the shared
// BlepTable (pulses and combined waveforms: the stronger kernel, RenderTables.h), placed at the event's exact time (a sub-sample position; events up to one sample back are
// allowed and corrected in the samples already written). The naive value is taken at the sample's end,
// the output is the per-oscillator ring buffer kLatencySamples (8) samples later, times the envelope.
// Steps on the short kernel are that kernel's own steps at their exact time, kLatencySamples late (tested
// against an independent sum of its steps: a free-running saw, "[kernel]"; steps at writes, "[timing]").
//
// PER WAVEFORM (the mode follows the control register):
//   triangle, saw   closed form: g x (the continuous 12-bit value) plus, for the Model A converter, every
//                   index bit whose square wave stays below half the internal rate as its own square wave
//                   (e_i x bit_i, edges placed exactly), the faster bits as their mean. Saw wraps and
//                   triangle folds are BLEP/BLAMP events; the 12-bit staircase itself is not rendered.
//   pulse           two converter levels; the comparison is one cycle late (8.15.2 (c)), so the edges lie
//                   one cycle after the line crosses the width (or wraps); Smooth widths are continuous.
//   combined        (two or more of triangle, saw, pulse; no noise) an exact band-limited staircase: every
//                   change of d (shape (index)) between index cells is a BLEP; the pulse gate, the ring
//                   fold (half a table further) and phase jumps are exact too. Model A combinations with
//                   the saw pull their top bit down (8.15.2 (c) step 4) in the first step whose landing
//                   cell has output bit 11 clear: from a rise the line is pulled where it enters such a
//                   cell (the gate as the chip compared it, at whole cycles); after a write or a correction
//                   inside the upper half the chip's own landings are followed (renderLanding) and the
//                   pull-down is scheduled at the landing.
//   noise           sample-and-hold of d (noise output), a step at each noise clock's exact cycle.
//   noise + another (write-back) per-cycle mode: the digital output itself, a step wherever it changes.
//   none            the held, fading digital output (its steps on the long kernel).
// SYNC. The digital core resets a target at the cycle after its source's top bit rose; the rendered reset
// lies at the source's crossing, up to two cycles earlier. It is predicted from the source's line (a joint
// simulation of the three lines per piece) and the digital reset confirms it and sets offU = the target's
// advance between the two. A prediction the digital core does not confirm within three cycles (the
// sync-chain exception of 8.15.2 (d)) is undone by re-anchoring: at every sample end each line is
// re-anchored on the digital phase, and a disagreement is corrected band-limited and counted
// (renderCorrections(): 0 over the tests' random patches and write schedules; with many writes rare,
// almost all below 4e-6 of a period; docs/decisions.md S2b-2 row 17).
// offU is a sub-cycle time held as a phase: a frequency write rescales it (and the line of an unconfirmed
// reset), so rendered and digital line stay less than a cycle apart in time.
// A source in per-cycle mode (noise with another waveform) is not predicted: its top-bit changes come from
// the digital accumulator cycle by cycle, its targets' resets from the digital core. Oscillator 1 reads
// oscillator 3's top bit before oscillator 3's pull-down: it sees such a rise for one cycle (a flip and the
// flip back one cycle later). Two lines crossing at the same instant are both taken.
// The simulation applies the Model A pull-down as the sweep does: a saw combination pulled down in its
// first upper-half cell never rises, so it neither syncs nor shows its top bit (8.15.2 (c) step 4, (d)).
// RING flips the triangle (or the table) where the source's top bit changes, from the same simulation.
// WRITES (write, setOscillator, powerOnKeepingTime) take effect at their exact cycle: renderUntil stops
// inside a sample at the requested cycle, the write captures every line's value before and after, and the
// difference is a BLEP/BLAMP at that cycle. What the chip has not done yet is decided again with the new
// controls: a predicted pull-down ends (the line goes back to the digital phase), a reset whose sync bit
// was cleared is dropped, pull-downs inside an upper half follow the chip's landings, and the ring state is
// taken after the sources' pull-downs. A write right after render() (at a sample boundary) is placed at
// its cycle too, with the values taken at the sample end (less than one cycle apart).
// THE DIGITAL STATE is advanced only by advanceCycles / stepCycle, exactly as without rendering (tested
// for random write sequences, both models, every block split and internal rate).
// REAL TIME: no allocation, no locks, no exceptions; every loop is bounded (32 cycles per piece, 80
// events per oscillator and piece, 4 096 events per sweep as a backstop).
} // namespace vs::bside::chip
