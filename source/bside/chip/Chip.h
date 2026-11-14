#pragma once

// The chip's digital core (docs/architecture.md 8.4.1-8.4.3, 8.4.7): register file, three
// oscillators (24-bit accumulators, sync, ring, test bit, 23-bit noise registers with the combined-
// waveform write-back), three envelope generators (15-bit rate counters, exponential divider, delay
// quirk, freeze at zero, gate edges) and the two read registers. JUCE-free; no allocation, no
// exceptions, no locks; everything the audio thread calls is noexcept. The object is trivially
// copyable (it owns no memory), which the tests assert.
//
// ---- The normative definition: one cycle (stepCycle) ----
// One call advances cycle() by 1. In this order:
//   1. Oscillators 1..3. In test mode (control bit 3): the accumulator stays 0, no edges. Otherwise
//      acc += F (mod 2^24; with Smooth resolution the fraction below bit 0 is carried along); "top bit
//      rose" = bit 23 was 0 and is 1; when bit 19 was 0 and is 1, the noise register shifts once:
//      reg = ((reg << 1) | (bit 22 XOR bit 17)) & 0x7fffff.
//   2. Sync, from the edges of step 1 (before any reset): for each source s with target t = s + 1
//      (1 -> 2, 2 -> 3, 3 -> 1): if s's top bit rose, t has its sync bit and t is not in test mode,
//      t's accumulator (and fraction) becomes 0 -- except when s is itself synced in this cycle
//      (s has its sync bit and s's own source's top bit rose): then t is not reset (the documented
//      chain case; contract question, 8.4.3 row 6 "confirm").
//   3. Write-back: an oscillator with noise and at least one other waveform selected and not in test
//      mode ANDs its combined 12-bit output (wave::combined) back into the noise register: output
//      bits 11..4 clear register bits 20, 18, 14, 11, 9, 5, 2, 0 where they are 0.
//   4. Envelopes 1..3: rate counter + 1 (after 0x7fff it continues at 1). If it now equals the rate
//      period: it becomes 0; outside attack the divider counter + 1, and unless it now equals the
//      divider period nothing else happens; otherwise the divider counter becomes 0 and the
//      envelope counter steps: attack + 1 (8-bit, wraps; at 0xff -> decay/sustain), decay - 1 unless
//      it equals the sustain level (nibble x 0x11) or 0, release - 1 unless 0. Then the divider
//      period is latched from the counter's value: 0xff -> 1, 0x5d -> 2, 0x36 -> 4, 0x1a -> 8,
//      0x0e -> 16, 0x06 -> 30, 0x00 -> 1, any other value keeps it.
// A register write (write / setOscillator / setFilter) takes effect at once, between two cycles:
// frequency and pulse width from the next cycle; test bit set -> accumulator 0, noise register
// 0x7fffff (held while set); gate 0 -> 1 -> attack, 1 -> 0 -> release (rate period switched, counters
// kept); a rate nibble write switches the current phase's rate period at once (the delay quirk follows
// from the counter running past a smaller period). Power-on (reset): everything 0, noise 0x7fffff,
// envelopes in release at 0, rate period 9, divider period 1.
//
// ---- The fast path (advanceCycles) ----
// Equal to repeated stepCycle by construction: it jumps over runs of cycles in which nothing but
// "accumulator + F, rate counter + 1, noise shifts on bit-19 rises" happens (computed in closed form)
// and calls stepCycle itself for every other cycle (a top-bit rise feeding a sync target, a rate
// counter match of an envelope that is not frozen, every cycle of an oscillator in write-back mode).
// A frozen envelope (counter 0, or in decay at the sustain level) is also jumped: its matches only
// reset the rate counter and count the divider. Tests compare both for 10^7 random cycles per model.
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
// (inside a closed-form jump the oscillators are advanced one after the other, so the chip's state
// at callback time differs from the state at the event's instant).
//   syncReset (SyncResetEvent)    always reported. The target's accumulator becomes 0 at t = E. The
//       source's top bit crossed 2^23 at t = E - before, before = (sourcePhase - 2^55) / sourceIncrement
//       in [0, 1) (SyncResetEvent::crossingBefore()); targetPhaseBefore is the target's 56-bit phase
//       (accumulator.fraction) after its own add in that step, just before the reset.
//   noiseClock (NoiseClockEvent)  when Sink::kWantsNoiseEvents: one per shift, at t = E (bit 19 rose in
//       that step); carries the register and its 12-bit noise output right after the shift (before
//       any write-back of the same step).
//   noiseWriteBack (NoiseWriteBackEvent)  when Sink::kWantsNoiseEvents: whenever the write-back of
//       step 3 changed the register; register and 12-bit noise output after it. Write-back mode is
//       never jumped: advanceCycles steps every cycle while any oscillator is in it, so these events
//       come in step order.
// ORDER. Within one step: noise clocks (oscillator 1..3), sync resets (target 1..3), write-backs
// (oscillator 1..3). A closed-form jump contains noise clocks only and reports them OSCILLATOR BY
// OSCILLATOR (each in time order), NOT by time across oscillators. Stable-sorted by cycle, the events
// of advanceCycles equal those of the same cycles stepped one by one (tested).
//
// Between events an oscillator is a straight line: phase(n) + k x increment(n) (mod 2^56), from which
// saw wraps, top-bit flips and pulse edges (bits 23..12 crossing PW) follow in closed form.
// pulseWidthSmooth(n) is the continuous pulse width (Smooth), output(n) the 12-bit digital output.
// Writes happen between cycles; the renderer splits the sample at the write's cycle.

#include "bside/chip/ChipClock.h"
#include "bside/chip/ChipTypes.h"
#include "bside/chip/Waveforms.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

namespace vs::bside::chip
{
/** Everything 8.4.3 lists, plus the inputs that determine it; operator== compares every field. */
struct DigitalState
{
    struct Osc
    {
        std::uint32_t acc = 0;          // 24-bit accumulator
        std::uint32_t accFraction = 0;  // below bit 0 (Smooth only; 0 in Authentic and with register writes)
        std::uint64_t increment = 0;    // F in 32.32 fixed point
        std::uint32_t noise = kNoiseReset; // 23-bit shift register
        std::uint16_t pulseWidth = 0;   // 12-bit value the digital pulse compares with
        bool operator== (const Osc&) const = default;
    };
    struct Env
    {
        std::uint8_t counter = 0;       // 8-bit envelope counter
        std::uint8_t phase = 2;         // EnvPhase
        std::uint16_t rateCounter = 0;  // 15 bits
        std::uint16_t ratePeriod = 9;
        std::uint8_t divider = 0;       // exponential divider counter
        std::uint8_t dividerPeriod = 1;
        bool operator== (const Env&) const = default;
    };
    std::uint64_t cycle = 0;
    std::array<Osc, 3> osc {};
    std::array<Env, 3> env {};
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

/** One shift of a noise register (bit 19 rose in the step ending at `cycle`). */
struct NoiseClockEvent
{
    int osc = 0;
    std::uint64_t cycle = 0;
    std::uint32_t noiseRegister = 0; // after the shift
    std::uint16_t output = 0;        // wave::noise (noiseRegister)
    bool operator== (const NoiseClockEvent&) const = default;
};

/** A change of a noise register by the combined-waveform write-back in the step ending at `cycle`. */
struct NoiseWriteBackEvent
{
    int osc = 0;
    std::uint64_t cycle = 0;
    std::uint32_t noiseRegister = 0; // after the write-back
    std::uint16_t output = 0;        // wave::noise (noiseRegister)
    bool operator== (const NoiseWriteBackEvent&) const = default;
};

/** An event sink that ignores everything (the digital-only path). A sink provides
      static constexpr bool kWantsNoiseEvents;            // noiseClock and noiseWriteBack are called only if true
      void syncReset (const SyncResetEvent&);
      void noiseClock (const NoiseClockEvent&);
      void noiseWriteBack (const NoiseWriteBackEvent&);
    and must not read the chip from inside a callback. */
struct NullSink
{
    static constexpr bool kWantsNoiseEvents = false;
    void syncReset (const SyncResetEvent&) noexcept {}
    void noiseClock (const NoiseClockEvent&) noexcept {}
    void noiseWriteBack (const NoiseWriteBackEvent&) noexcept {}
};

namespace detail
{
constexpr std::uint8_t latchDivider (std::uint8_t counter, std::uint8_t current) noexcept
{
    switch (counter)
    {
        case 0xff: return 1;
        case 0x5d: return 2;
        case 0x36: return 4;
        case 0x1a: return 8;
        case 0x0e: return 16;
        case 0x06: return 30;
        case 0x00: return 1;
        default: return current;
    }
}

/** Cycles until the rate counter next equals `period` (>= 1): directly, or after the wrap (the delay quirk). */
constexpr std::uint64_t rateDistance (std::uint16_t rate, std::uint16_t period) noexcept
{
    return rate < period ? static_cast<std::uint64_t> (period - rate) : static_cast<std::uint64_t> (kRateCounterMax - rate) + period;
}

/** The rate counter after j cycles without a match (at most one wrap, 0x7fff -> 1). */
constexpr std::uint16_t rateAdvance (std::uint16_t rate, std::uint64_t j) noexcept
{
    std::uint64_t r = rate + j;
    if (r > kRateCounterMax)
        r -= kRateCounterMax;
    return static_cast<std::uint16_t> (r);
}

constexpr std::uint32_t shiftNoise (std::uint32_t r) noexcept
{
    return ((r << 1) | (((r >> 22) ^ (r >> 17)) & 1u)) & kNoiseMask;
}

constexpr int syncTarget (int i) noexcept { return i == 2 ? 0 : i + 1; }
constexpr int syncSource (int i) noexcept { return i == 0 ? 2 : i - 1; }
} // namespace detail

class Chip
{
public:
    /** Longest closed-form jump (keeps jump x increment inside 64 bits). */
    static constexpr std::uint64_t kMaxSkip = 4096;

    /** Power-on state with the default Config (clock and internal rate included, so a chip that is
        never prepared still advances by samples). */
    Chip() noexcept
    {
        clock.prepare (cfg.clockHz, cfg.internalRate);
        reset();
    }

    /** Model, clock and internal rate; resets the chip. Allocates nothing (no member owns memory). */
    void prepare (const Config& c) noexcept;
    /** Power-on state at cycle 0 (the configuration is kept). */
    void reset() noexcept;

    /** A register write at the current cycle (trace replay). The address is taken modulo 32;
        0x19..0x1F are ignored (read-only or not modelled). */
    void write (std::uint8_t reg, std::uint8_t value) noexcept;
    /** Program-engine input of oscillator n (0..2) at the current cycle: frequency and pulse width (with
        the resolution of the configuration), then as if registers 5, 6 and 4 were written in that order. */
    void setOscillator (int n, const OscInput& in) noexcept;
    /** Filter input at the current cycle: stored for the analog stages (no digital effect). */
    void setFilter (const FilterInput& in) noexcept;
    /** 0x1B: oscillator 3's output bits 11..4; 0x1C: envelope 3's counter; any other address 0. */
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
    EnvPhase envelopePhase (int n) const noexcept { return env[checked (n)].phase; }
    /** The 12-bit digital output of oscillator n (0..2) for its selected waveforms (Waveforms.h). */
    std::uint16_t output (int n) const noexcept;
    float cutoffSmooth() const noexcept { return cutoff; }
    std::uint8_t registerValue (std::uint8_t r) const noexcept { return r < reg::kWritable ? regs[r] : std::uint8_t { 0 }; }

private:
    struct Osc
    {
        std::uint64_t phase = 0; // accumulator.fraction, 56 bits
        std::uint64_t inc = 0;
        std::uint32_t noise = kNoiseReset;
        std::uint16_t pw = 0;
        float pwSmooth = 0.0f;
    };
    struct Env
    {
        std::uint8_t counter = 0;
        EnvPhase phase = EnvPhase::release;
        std::uint16_t rate = 0;
        std::uint16_t ratePeriod = kRatePeriods[0];
        std::uint8_t div = 0;
        std::uint8_t divPeriod = 1;
    };

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

    void applyControl (int n, std::uint8_t before) noexcept;
    void updateRatePeriod (int n) noexcept;

    bool envFrozen (int n) const noexcept
    {
        const Env& e = env[idx (n)];
        if (e.phase == EnvPhase::attack)
            return false;
        if (! (e.counter == 0 || (e.phase == EnvPhase::decaySustain && e.counter == sustainLevel (n))))
            return false;
        return e.div < e.divPeriod && detail::latchDivider (e.counter, e.divPeriod) == e.divPeriod;
    }

    void envMatch (int n) noexcept
    {
        Env& e = env[idx (n)];
        e.rate = 0;
        if (e.phase != EnvPhase::attack)
        {
            ++e.div;
            if (e.div != e.divPeriod)
                return;
        }
        e.div = 0;
        switch (e.phase)
        {
            case EnvPhase::attack:
                ++e.counter; // 8-bit: an attack started at 0xff wraps to 0 (contract question)
                if (e.counter == 0xff)
                {
                    e.phase = EnvPhase::decaySustain;
                    e.ratePeriod = kRatePeriods[attackDecay (n) & 15u];
                }
                break;
            case EnvPhase::decaySustain:
                if (e.counter != sustainLevel (n) && e.counter != 0)
                    --e.counter;
                break;
            case EnvPhase::release:
                if (e.counter != 0)
                    --e.counter;
                break;
        }
        e.divPeriod = detail::latchDivider (e.counter, e.divPeriod);
    }

    bool anyWriteBack() const noexcept { return wave::writesBack (ctl (0)) || wave::writesBack (ctl (1)) || wave::writesBack (ctl (2)); }

    /** Cycles to the next cycle that needs stepCycle (>= 1), at most kMaxSkip + 1. */
    std::uint64_t nextEvent() const noexcept
    {
        std::uint64_t e = kMaxSkip + 1;
        for (int n = 0; n < 3; ++n)
            if (! envFrozen (n))
                e = std::min (e, detail::rateDistance (env[idx (n)].rate, env[idx (n)].ratePeriod));
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

    /** j cycles (1 <= j <= kMaxSkip) in which nextEvent() promised no event and no write-back is active. */
    template <class Sink>
    void skip (std::uint64_t j, Sink& sink) noexcept
    {
        constexpr std::uint64_t half = kPhaseBit19;     // offset that puts the bit-19 rises on multiples of 2^52
        constexpr int periodShift = 52;
        for (int n = 0; n < 3; ++n)
        {
            Osc& o = osc[idx (n)];
            if ((ctl (n) & ctrl::test) != 0 || o.inc == 0)
                continue;
            const std::uint64_t end = o.phase + j * o.inc; // < 2^56 + 2^63: no overflow
            const std::uint64_t first = (o.phase + half) >> periodShift;
            const std::uint64_t rises = ((end + half) >> periodShift) - first;
            for (std::uint64_t k = 1; k <= rises; ++k)
            {
                o.noise = detail::shiftNoise (o.noise);
                if constexpr (Sink::kWantsNoiseEvents)
                {
                    const std::uint64_t boundary = ((first + k) << periodShift) - half;
                    sink.noiseClock (NoiseClockEvent { n, cycleCount + (boundary - o.phase + o.inc - 1) / o.inc, o.noise, wave::noise (o.noise) });
                }
            }
            o.phase = end & kPhaseMask;
        }
        for (int n = 0; n < 3; ++n)
        {
            Env& e = env[idx (n)];
            if (! envFrozen (n))
            {
                e.rate = detail::rateAdvance (e.rate, j);
                continue;
            }
            const std::uint64_t d0 = detail::rateDistance (e.rate, e.ratePeriod);
            if (j < d0)
                e.rate = detail::rateAdvance (e.rate, j);
            else
            {
                const std::uint64_t after = j - d0;
                const std::uint64_t matches = 1 + after / e.ratePeriod;
                e.rate = static_cast<std::uint16_t> (after % e.ratePeriod);
                e.div = static_cast<std::uint8_t> ((e.div + matches) % e.divPeriod);
            }
        }
        cycleCount += j;
    }

    Config cfg {};
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
    bool rose[3] = { false, false, false };

    // 1. accumulators, top-bit edges, noise clocks
    for (int n = 0; n < 3; ++n)
    {
        if ((ctl (n) & ctrl::test) != 0)
            continue;
        Osc& o = osc[idx (n)];
        const std::uint64_t prev = o.phase;
        const std::uint64_t next = (prev + o.inc) & kPhaseMask;
        o.phase = next;
        rose[n] = (~prev & next & kPhaseMsb) != 0;
        if ((~prev & next & kPhaseBit19) != 0)
        {
            o.noise = detail::shiftNoise (o.noise);
            if constexpr (Sink::kWantsNoiseEvents)
                sink.noiseClock (NoiseClockEvent { n, now, o.noise, wave::noise (o.noise) });
        }
    }

    // 2. sync, from the edges above (evaluated before any reset)
    bool resetTarget[3] = { false, false, false };
    for (int s = 0; s < 3; ++s)
    {
        const int t = detail::syncTarget (s);
        const bool sourceSynced = (ctl (s) & ctrl::sync) != 0 && rose[detail::syncSource (s)];
        if (rose[s] && (ctl (t) & ctrl::sync) != 0 && (ctl (t) & ctrl::test) == 0 && ! sourceSynced)
            resetTarget[t] = true;
    }
    const std::uint64_t phaseAfterAdd[3] = { osc[0].phase, osc[1].phase, osc[2].phase };
    for (int t = 0; t < 3; ++t)
        if (resetTarget[t])
        {
            const int src = detail::syncSource (t);
            sink.syncReset (SyncResetEvent { t, src, now, phaseAfterAdd[src], osc[idx (src)].inc, phaseAfterAdd[t] });
            osc[idx (t)].phase = 0;
        }

    // 3. combined-waveform write-back into the noise register
    for (int n = 0; n < 3; ++n)
        if (wave::writesBack (ctl (n)))
        {
            Osc& o = osc[idx (n)];
            const std::uint32_t before = o.noise;
            o.noise &= wave::noiseKeepMask (output (n));
            if constexpr (Sink::kWantsNoiseEvents)
                if (o.noise != before)
                    sink.noiseWriteBack (NoiseWriteBackEvent { n, now, o.noise, wave::noise (o.noise) });
        }

    // 4. envelopes
    for (int n = 0; n < 3; ++n)
    {
        Env& e = env[idx (n)];
        e.rate = static_cast<std::uint16_t> (e.rate + 1);
        if (e.rate > kRateCounterMax)
            e.rate = 1;
        if (e.rate == e.ratePeriod)
            envMatch (n);
    }

    cycleCount = now;
}

template <class Sink>
void Chip::advanceCycles (std::uint64_t n, Sink& sink) noexcept
{
    while (n > 0)
    {
        if (anyWriteBack())
        {
            stepCycle (sink);
            --n;
            continue;
        }
        const std::uint64_t span = std::min (n, kMaxSkip);
        const std::uint64_t e = nextEvent();
        if (e > span)
        {
            skip (span, sink);
            n -= span;
        }
        else
        {
            if (e > 1)
                skip (e - 1, sink);
            stepCycle (sink);
            n -= e;
        }
    }
}

inline std::uint16_t Chip::output (int n) const noexcept
{
    const Osc& o = osc[idx (n)];
    const std::uint8_t c = ctl (n);
    const auto acc = static_cast<std::uint32_t> (o.phase >> kFractionBits);
    const auto src = static_cast<std::uint32_t> (osc[idx (detail::syncSource (n))].phase >> kFractionBits);
    return wave::combined (cfg.model, c, wave::triangle (acc, (c & ctrl::ring) != 0, src), wave::saw (acc), wave::pulse (acc, o.pw, (c & ctrl::test) != 0),
                           wave::noise (o.noise));
}
} // namespace vs::bside::chip
