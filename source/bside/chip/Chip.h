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
// ---- The seam for S2b (band-limited rendering) ----
// S2b renders per internal sample: ChipClock::nextSample() gives the whole cycles of the sample;
// advanceCycles (n, sink) advances the digital state and reports, with their exact cycle, the events
// a renderer cannot infer from the per-sample state: Sink::syncReset (osc, cycle, accBefore) and, when
// Sink::kWantsNoiseClocks, Sink::noiseClock (osc, cycle) (the cycle is the one whose step caused the
// event, i.e. cycle() right after it). Order: per oscillator in cycle order; across oscillators a
// closed-form jump reports oscillator by oscillator (stable-sorted by cycle, the list equals what
// stepCycle reports, which a test proves). Between events an oscillator is a straight
// line: phase(n) + k x increment(n) (mod 2^56), from which saw wraps, top-bit flips and pulse edges
// (bits 23..12 crossing PW) follow in closed form. pulseWidthSmooth(n) is the continuous pulse width
// (Smooth), output(n) the 12-bit digital output (for checks and the combined shapes), envelope(n)
// the 8-bit counter applied per internal sample (8.4.5). Writes happen between cycles; the renderer
// splits the sample at the write's cycle (the chip has no notion of samples beyond ChipClock).

#include "bside/chip/ChipClock.h"
#include "bside/chip/ChipTypes.h"
#include "bside/chip/Waveforms.h"

#include <algorithm>
#include <array>
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

/** An event sink that ignores everything (the digital-only path). */
struct NullSink
{
    static constexpr bool kWantsNoiseClocks = false;
    void syncReset (int, std::uint64_t, std::uint32_t) noexcept {}
    void noiseClock (int, std::uint64_t) noexcept {}
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

    Chip() noexcept { reset(); }

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

    // ---- read access for S2b and the tests (n = 0..2) ----
    std::uint64_t phase (int n) const noexcept { return osc[idx (n)].phase; }
    std::uint64_t increment (int n) const noexcept { return osc[idx (n)].inc; }
    std::uint32_t accumulator (int n) const noexcept { return static_cast<std::uint32_t> (osc[idx (n)].phase >> kFractionBits); }
    std::uint32_t noiseRegister (int n) const noexcept { return osc[idx (n)].noise; }
    std::uint16_t pulseWidth (int n) const noexcept { return osc[idx (n)].pw; }
    float pulseWidthSmooth (int n) const noexcept { return osc[idx (n)].pwSmooth; }
    std::uint8_t control (int n) const noexcept { return regs[static_cast<std::size_t> (reg::oscStride * n + reg::control)]; }
    std::uint8_t envelope (int n) const noexcept { return env[idx (n)].counter; }
    EnvPhase envelopePhase (int n) const noexcept { return env[idx (n)].phase; }
    /** The 12-bit digital output of oscillator n for its selected waveforms (Waveforms.h). */
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

    static std::size_t idx (int n) noexcept { return static_cast<std::size_t> (n); }
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
                if constexpr (Sink::kWantsNoiseClocks)
                {
                    const std::uint64_t boundary = ((first + k) << periodShift) - half;
                    sink.noiseClock (n, cycleCount + (boundary - o.phase + o.inc - 1) / o.inc);
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
            sink.noiseClock (n, now);
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
    for (int t = 0; t < 3; ++t)
        if (resetTarget[t])
        {
            sink.syncReset (t, now, accumulator (t));
            osc[idx (t)].phase = 0;
        }

    // 3. combined-waveform write-back into the noise register
    for (int n = 0; n < 3; ++n)
        if (wave::writesBack (ctl (n)))
            osc[idx (n)].noise &= wave::noiseKeepMask (output (n));

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
