// Digital unit gate (docs/architecture.md 8.9.1, row "Digital unit gates"): the chip's closed-form
// digital core against a naive per-cycle stepper written by the test agent from 8.4.1-8.4.3 alone.
//
// NaiveChip below is deliberately the simplest code that follows the contract text: one function
// advances one cycle, there are no fast paths, and nothing is shared with the implementation (only
// the public types of ChipTypes.h and the DigitalState struct are used, to compare field by field).
//
// The readings below R1..R8 are the literal ones; five of them are overridden by the "reference-style"
// readings that the committed gate uses by default (enum Reading below) because the sentences are
// ambiguous, rows are marked "confirm", and the public documentation of the chip supports them.
// CONTRACT READINGS (where 8.4.1-8.4.3 is silent or ambiguous; the choice is the one the text
// supports best; each is a candidate "ambiguity" in the report):
//  R1  Cycle order: per oscillator add F (test bit set: accumulator = 0, noise register = all ones),
//      detect rising edges of accumulator bit 23 and bit 19 on the natural sum against the value left
//      at the end of the previous cycle, clock the noise register on a bit-19 edge, then apply sync
//      resets (target sync bit set and source natural bit-23 edge; all edges are taken before any
//      reset, so chains see natural edges), then step the three envelopes, then write combined
//      noise output back, then cycle++.
//  R2  Writes take effect immediately (no delay is given in the contract): frequency, pulse width and
//      register values update at the write; a gate rising edge sets phase attack and a falling edge
//      release; the test bit clears the accumulator and resets the noise register at the write.
//  R3  Rate counter: 15 bits, +1 per cycle modulo 2^15 (0x7fff -> 0; the wrap1 reading selects 1),
//      reset to 0 on equality with the period. The period is the nibble of the current phase.
//  R4  Envelope step on a match: attack: counter + 1 (no divider; divider counter reset), phase
//      becomes decay when the counter reaches 0xFF (an attack that starts at 0xFF goes to decay at
//      its first match without wrapping). Decay/sustain and release: divider counter + 1; when it
//      equals the divider period it is reset and the counter drops by 1 unless it is 0 or (decay)
//      not above the sustain level (so raising sustain never raises the counter and never makes it
//      fall below the new level).
//  R5  Divider period is a function of the counter, applied whenever the counter changes: 1 above
//      0x5D, 2 at or below 0x5D, 4 at or below 0x36, 8 at or below 0x1A, 16 at or below 0x0E, 30 at or
//      below 0x06 (also at 0). Power-on value 1 is the DigitalState default.
//  R6  Combined waveforms: bitwise AND of the selected waveform outputs (the contract defers the
//      shapes to a fit). No waveform selected: 0. Write-back (noise plus at least one other waveform,
//      test bit clear): combined output bits 11..4 replace register bits 20,18,14,11,9,5,2,0 every
//      cycle, after the envelopes. Read register 0x1B is computed from the state after the write-back.
//  R7  Read registers are pure functions of the state, recomputed after every cycle and every write.
//  R8  Written register values are stored as written (0x00..0x18); the address is taken modulo 32
//      and 0x19..0x1F are ignored (declared in Chip.h).

#include "bside/chip/Chip.h"
#include "bside/chip/ChipTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using namespace vs::bside::chip;

// The five ambiguous sentences (see "Ambiguities" in the S2a stepper report). Each bit selects the
// reading that the implementation (and the public documentation of the chip family) uses; clear it for
// the literal reading of the contract text that this stepper was first written with (R3, R4, R5, R1).
//   wrap1       rate counter wraps 0x7fff -> 1 (text: -> 0, modulo 2^15)
//   divlatch    divider period latched at the exact counter values (text: a function of the counter)
//   sustaineq   decay stops only at counter == sustain level (text: at counter <= level)
//   attackwrap  attack from 0xFF wraps to 0 (text: attack at 0xFF goes straight to decay)
//   chainsuppress  a target is not reset when its source is itself being synced in the same cycle
//               (text: every natural top-bit edge resets its target)
// Triage: BSIDE_STEPPER_READING = comma list of the names above ("text" or empty: none; unset: all).
enum Reading : unsigned { kWrap1 = 1, kDivLatch = 2, kSustainEq = 4, kAttackWrap = 8, kChainSuppress = 16, kAll = 31 };
unsigned initialReadings()
{
    const char* e = std::getenv ("BSIDE_STEPPER_READING");
    if (e == nullptr) return kAll;
    const std::string s = std::string (",") + e + ",";
    unsigned r = 0;
    if (s.find (",wrap1,") != std::string::npos) r |= kWrap1;
    if (s.find (",divlatch,") != std::string::npos) r |= kDivLatch;
    if (s.find (",sustaineq,") != std::string::npos) r |= kSustainEq;
    if (s.find (",attackwrap,") != std::string::npos) r |= kAttackWrap;
    if (s.find (",chainsuppress,") != std::string::npos) r |= kChainSuppress;
    return r;
}
unsigned gReadings = initialReadings();
unsigned readings() { return gReadings; }
struct ScopedReadings
{
    unsigned saved = gReadings;
    explicit ScopedReadings (unsigned r) { gReadings = r; }
    ~ScopedReadings() { gReadings = saved; }
};

struct NaiveChip
{
    struct O
    {
        std::uint64_t phase = 0, inc = 0;
        std::uint32_t noise = 0x7fffff;
        std::uint16_t pw = 0;
    };
    struct E
    {
        std::uint8_t counter = 0, phase = 2;
        std::uint16_t rate = 0, period = 9;
        std::uint8_t div = 0, divPeriod = 1;
    };

    O o[3];
    E e[3];
    std::uint8_t regs[0x19] = {};
    std::uint64_t cycle = 0;
    std::uint8_t r1b = 0, r1c = 0;

    // Coverage counters (what the random driver actually reached).
    struct Cov
    {
        std::uint64_t gateRise = 0, gateFall = 0, testSet = 0, testClear = 0, syncResets = 0, ringTriCycles = 0,
                      noiseClocks = 0, writeBackCycles = 0, lockedNoiseCycles = 0, attackDone = 0, sustainHold = 0,
                      releaseToZero = 0, sustainLowered = 0, sustainRaised = 0, freqZero = 0, freqMax = 0,
                      periodBelowCount = 0, rateWraps = 0;
        std::array<std::uint64_t, 16> attackStep {}, decayStep {}, releaseStep {};
        std::array<std::uint64_t, 6> dividerSeen {}; // periods 1,2,4,8,16,30
        std::array<std::uint64_t, 16> waveCombo {};   // control bits 7..4 value, cycles
    } cov;

    static std::uint8_t dividerFor (std::uint8_t c)
    {
        if (c > 0x5d) return 1;
        if (c > 0x36) return 2;
        if (c > 0x1a) return 4;
        if (c > 0x0e) return 8;
        if (c > 0x06) return 16;
        return 30;
    }
    static void latchDivider (E& en)
    {
        if (! (readings() & kDivLatch)) { en.divPeriod = dividerFor (en.counter); return; }
        switch (en.counter)
        {
            case 0xff: case 0x00: en.divPeriod = 1; break;
            case 0x5d: en.divPeriod = 2; break;
            case 0x36: en.divPeriod = 4; break;
            case 0x1a: en.divPeriod = 8; break;
            case 0x0e: en.divPeriod = 16; break;
            case 0x06: en.divPeriod = 30; break;
            default: break;
        }
    }
    std::uint8_t ctl (int i) const { return regs[7 * i + 4]; }
    std::uint8_t sustainLevel (int i) const { return static_cast<std::uint8_t> ((regs[7 * i + 6] >> 4) * 0x11); }
    std::uint32_t acc (int i) const { return static_cast<std::uint32_t> (o[i].phase >> 32); }
    static int src (int i) { return i == 0 ? 2 : i - 1; }
    static int tgt (int i) { return i == 2 ? 0 : i + 1; }

    void refreshPeriod (int i)
    {
        const std::uint8_t ad = regs[7 * i + 5], sr = regs[7 * i + 6];
        const unsigned nib = e[i].phase == 0 ? (ad >> 4) : e[i].phase == 1 ? (ad & 15) : (sr & 15);
        e[i].period = kRatePeriods[nib & 15];
    }

    std::uint16_t triOut (int i) const
    {
        const bool ring = (ctl (i) & 4) != 0;
        bool top = ((acc (i) >> 23) & 1) != 0;
        if (ring) top = top != (((acc (src (i)) >> 23) & 1) != 0);
        const std::uint32_t v = acc (i) ^ (top ? 0xffffffu : 0u);
        return static_cast<std::uint16_t> ((v >> 11) & 0xfff);
    }
    std::uint16_t sawOut (int i) const { return static_cast<std::uint16_t> ((acc (i) >> 12) & 0xfff); }
    std::uint16_t pulseOut (int i) const
    {
        if (ctl (i) & 8) return 0xfff;
        return static_cast<std::uint16_t> ((acc (i) >> 12) >= o[i].pw ? 0xfff : 0);
    }
    std::uint16_t noiseOut (int i) const
    {
        static const int bits[8] = { 20, 18, 14, 11, 9, 5, 2, 0 };
        unsigned v = 0;
        for (int k = 0; k < 8; ++k)
            v |= ((o[i].noise >> bits[k]) & 1u) << (11 - k);
        return static_cast<std::uint16_t> (v);
    }
    std::uint16_t out (int i) const
    {
        const std::uint8_t c = ctl (i);
        if ((c & 0xf0) == 0) return 0;
        std::uint16_t v = 0xfff;
        if (c & 0x10) v &= triOut (i);
        if (c & 0x20) v &= sawOut (i);
        if (c & 0x40) v &= pulseOut (i);
        if (c & 0x80) v &= noiseOut (i);
        return v;
    }
    void refreshReads()
    {
        r1b = static_cast<std::uint8_t> (out (2) >> 4);
        r1c = e[2].counter;
    }

    void write (std::uint8_t a, std::uint8_t v)
    {
        a &= 0x1f;
        if (a >= 0x19) return;
        const std::uint8_t old = regs[a];
        regs[a] = v;
        if (a < 21)
        {
            const int i = a / 7, k = a % 7;
            if (k <= 1)
            {
                const unsigned f = regs[7 * i] | (static_cast<unsigned> (regs[7 * i + 1]) << 8);
                o[i].inc = static_cast<std::uint64_t> (f) << 32;
                if (f == 0) ++cov.freqZero;
                if (f == 0xffff) ++cov.freqMax;
            }
            else if (k <= 3)
                o[i].pw = static_cast<std::uint16_t> (regs[7 * i + 2] | ((regs[7 * i + 3] & 15) << 8));
            else if (k == 4)
            {
                if (! (old & 1) && (v & 1)) { e[i].phase = 0; ++cov.gateRise; }
                if ((old & 1) && ! (v & 1)) { e[i].phase = 2; ++cov.gateFall; }
                if ((v & 8) && ! (old & 8)) ++cov.testSet;
                if (! (v & 8) && (old & 8)) ++cov.testClear;
                if (v & 8) { o[i].phase = 0; o[i].noise = 0x7fffff; }
            }
            else if (k == 6)
            {
                const std::uint8_t nl = static_cast<std::uint8_t> ((v >> 4) * 0x11), ol = static_cast<std::uint8_t> ((old >> 4) * 0x11);
                if (e[i].phase == 1 && nl < e[i].counter && nl < ol) ++cov.sustainLowered;
                if (e[i].phase == 1 && nl > ol) ++cov.sustainRaised;
            }
            const std::uint16_t before = e[i].period;
            refreshPeriod (i);
            if ((k == 5 || k == 6) && e[i].period < e[i].rate && e[i].period != before) ++cov.periodBelowCount;
        }
        refreshReads();
    }

    void step()
    {
        bool rise23[3], rise19[3];
        for (int i = 0; i < 3; ++i)
        {
            const std::uint64_t prev = o[i].phase;
            if (ctl (i) & 8)
            {
                o[i].phase = 0;
                o[i].noise = 0x7fffff;
            }
            else
                o[i].phase = (prev + o[i].inc) & ((std::uint64_t { 1 } << 56) - 1);
            rise23[i] = ! ((prev >> 55) & 1) && ((o[i].phase >> 55) & 1);
            rise19[i] = ! ((prev >> 51) & 1) && ((o[i].phase >> 51) & 1);
            if (rise19[i])
            {
                const std::uint32_t r = o[i].noise;
                o[i].noise = ((r << 1) | (((r >> 22) ^ (r >> 17)) & 1u)) & 0x7fffff;
                ++cov.noiseClocks;
            }
        }
        for (int i = 0; i < 3; ++i)
            if ((ctl (i) & 2) && rise23[src (i)]
                && ! ((readings() & kChainSuppress) && (ctl (src (i)) & 2) && rise23[src (src (i))]))
            {
                o[i].phase = 0;
                ++cov.syncResets;
            }
        for (int i = 0; i < 3; ++i)
        {
            E& en = e[i];
            refreshPeriod (i);
            en.rate = static_cast<std::uint16_t> ((en.rate + 1) & 0x7fff);
            if (en.rate == 0) ++cov.rateWraps;
            if (en.rate == 0 && (readings() & kWrap1)) en.rate = 1;
            if (en.rate != en.period) continue;
            en.rate = 0;
            const unsigned nibIdx = en.phase == 0 ? (regs[7 * i + 5] >> 4) : en.phase == 1 ? (regs[7 * i + 5] & 15) : (regs[7 * i + 6] & 15);
            if (en.phase == 0)
            {
                ++cov.attackStep[nibIdx];
                en.div = 0;
                if (readings() & kAttackWrap) ++en.counter;
                else if (en.counter != 0xff) ++en.counter;
                if (en.counter == 0xff) { en.phase = 1; ++cov.attackDone; }
                latchDivider (en);
            }
            else
            {
                (en.phase == 1 ? cov.decayStep : cov.releaseStep)[nibIdx]++;
                ++en.div;
                if (en.div != en.divPeriod) continue;
                en.div = 0;
                const bool floorHit = en.phase == 1 ? ((readings() & kSustainEq) ? (en.counter == sustainLevel (i) || en.counter == 0) : en.counter <= sustainLevel (i)) : en.counter == 0;
                if (! floorHit)
                {
                    --en.counter;
                    if (en.phase == 2 && en.counter == 0) ++cov.releaseToZero;
                }
                else if (en.phase == 1)
                    ++cov.sustainHold;
                latchDivider (en);
                static const std::uint8_t dv[6] = { 1, 2, 4, 8, 16, 30 };
                for (int d = 0; d < 6; ++d)
                    if (en.divPeriod == dv[d]) ++cov.dividerSeen[static_cast<std::size_t> (d)];
            }
            refreshPeriod (i);
        }
        for (int i = 0; i < 3; ++i)
        {
            const std::uint8_t c = ctl (i);
            ++cov.waveCombo[static_cast<std::size_t> (c >> 4)];
            if ((c & 4) && (c & 0x10)) ++cov.ringTriCycles;
            if ((c & 0x80) && (c & 0x70) && ! (c & 8))
            {
                ++cov.writeBackCycles;
                const std::uint16_t v = out (i);
                static const int bits[8] = { 20, 18, 14, 11, 9, 5, 2, 0 };
                for (int k = 0; k < 8; ++k)
                {
                    const std::uint32_t m = std::uint32_t { 1 } << bits[k];
                    if ((v >> (11 - k)) & 1) o[i].noise |= m; else o[i].noise &= ~m;
                }
                if (o[i].noise == 0) ++cov.lockedNoiseCycles;
            }
        }
        ++cycle;
        refreshReads();
    }

    std::uint8_t read (std::uint8_t a) const
    {
        a &= 0x1f;
        return a == 0x1b ? r1b : a == 0x1c ? r1c : std::uint8_t { 0 };
    }
};

struct Rng
{
    std::uint64_t s;
    explicit Rng (std::uint64_t seed) : s (seed) {}
    std::uint64_t next()
    {
        std::uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    unsigned below (unsigned n) { return static_cast<unsigned> (next() % n); }
    bool chance (unsigned percent) { return below (100) < percent; }
};

struct Op
{
    std::uint64_t gap = 0;
    std::uint8_t reg = 0, val = 0;
};

std::uint8_t randomControl (Rng& r)
{
    unsigned v = 0;
    if (r.chance (55)) v |= 1;   // gate
    if (r.chance (30)) v |= 2;   // sync
    if (r.chance (30)) v |= 4;   // ring
    if (r.chance (12)) v |= 8;   // test
    const unsigned w = r.below (100);
    unsigned wave;
    if (w < 8) wave = 0;
    else if (w < 38) wave = 1u << r.below (4);          // a single waveform (incl. noise)
    else if (w < 60) wave = 0x8u | (1u << r.below (3)); // noise + one other
    else if (w < 75) wave = 8u | (1 + r.below (7));     // noise + any combination
    else wave = r.below (16);
    return static_cast<std::uint8_t> (v | (wave << 4));
}

Op randomOp (Rng& r)
{
    Op op;
    const unsigned g = r.below (1000);
    if (g < 450) op.gap = 0;
    else if (g < 750) op.gap = 1 + r.below (16);
    else if (g < 950) op.gap = 17 + r.below (2000);
    else if (g < 995) op.gap = 2000 + r.below (6000);
    else if (g < 998) op.gap = 8000 + r.below (32000);
    else op.gap = 32000 + r.below (40000);

    const unsigned c = r.below (100);
    const unsigned osc = r.below (3);
    const auto base = static_cast<std::uint8_t> (7 * osc);
    if (c < 30) { op.reg = static_cast<std::uint8_t> (base + 4); op.val = randomControl (r); }
    else if (c < 40)
    {
        op.reg = static_cast<std::uint8_t> (base + (r.chance (50) ? 0 : 1));
        const unsigned k = r.below (10);
        if (k == 0) op.val = 0;
        else if (k == 1) op.val = 0xff;
        else if (k < 4 && (op.reg % 7) == 1) op.val = 0;                              // high byte 0: tiny frequencies
        else op.val = static_cast<std::uint8_t> (r.below (256));
    }
    else if (c < 46) { op.reg = static_cast<std::uint8_t> (base + (r.chance (50) ? 2 : 3)); op.val = static_cast<std::uint8_t> (r.below (256)); }
    else if (c < 60) // attack / decay
    {
        op.reg = static_cast<std::uint8_t> (base + 5);
        auto nib = [&r]() { return r.chance (70) ? r.below (10) : r.below (16); };
        op.val = static_cast<std::uint8_t> ((nib() << 4) | nib());
    }
    else if (c < 80) // sustain / release (sustain changes during decay are common on purpose)
    {
        op.reg = static_cast<std::uint8_t> (base + 6);
        auto nib = [&r]() { return r.chance (70) ? r.below (10) : r.below (16); };
        op.val = static_cast<std::uint8_t> ((r.below (16) << 4) | nib());
    }
    else if (c < 95) { op.reg = static_cast<std::uint8_t> (0x15 + r.below (4)); op.val = static_cast<std::uint8_t> (r.below (256)); }
    else { op.reg = static_cast<std::uint8_t> (0x19 + r.below (7) + (r.chance (30) ? 0x20 * (1 + r.below (6)) : 0)); op.val = static_cast<std::uint8_t> (r.below (256)); }
    return op;
}

/** Field-by-field comparison; empty string when equal. Names listed in `ignore` (triage only) are skipped. */
std::string diff (const DigitalState& s, const NaiveChip& n, const std::string& ignore)
{
    std::ostringstream o;
    auto skip = [&ignore] (const char* name) { return ignore.find (std::string (",") + name + ",") != std::string::npos; };
    auto chk = [&] (const char* name, std::uint64_t a, std::uint64_t b)
    {
        if (a != b && ! skip (name)) o << name << ": impl " << a << " naive " << b << "; ";
    };
    chk ("cycle", s.cycle, n.cycle);
    for (std::size_t i = 0; i < 3; ++i)
    {
        const std::string p = "osc" + std::to_string (i) + ".";
        const auto& a = s.osc[i];
        const auto& b = n.o[i];
        chk ((p + "acc").c_str(), a.acc, static_cast<std::uint32_t> (b.phase >> 32));
        chk ((p + "accFraction").c_str(), a.accFraction, static_cast<std::uint32_t> (b.phase & 0xffffffffu));
        chk ((p + "increment").c_str(), a.increment, b.inc);
        chk ((p + "noise").c_str(), a.noise, b.noise);
        chk ((p + "pulseWidth").c_str(), a.pulseWidth, b.pw);
        const auto& ea = s.env[i];
        const auto& eb = n.e[i];
        const std::string q = "env" + std::to_string (i) + ".";
        chk ((q + "counter").c_str(), ea.counter, eb.counter);
        chk ((q + "phase").c_str(), ea.phase, eb.phase);
        chk ((q + "rateCounter").c_str(), ea.rateCounter, eb.rate);
        chk ((q + "ratePeriod").c_str(), ea.ratePeriod, eb.period);
        chk ((q + "divider").c_str(), ea.divider, eb.div);
        chk ((q + "dividerPeriod").c_str(), ea.dividerPeriod, eb.divPeriod);
    }
    for (std::size_t r = 0; r < 0x19; ++r)
        chk (("reg" + std::to_string (r)).c_str(), s.registers[r], n.regs[r]);
    chk ("osc3Output", s.osc3Output, n.r1b);
    chk ("env3Output", s.env3Output, n.r1c);
    return o.str();
}

std::string diffAll (const Chip& c, const NaiveChip& n, const std::string& ignore)
{
    std::string d = diff (c.state(), n, ignore);
    if (c.read (0x1b) != n.read (0x1b) && ignore.find (",osc3Output,") == std::string::npos) d += "read(0x1B) differs; ";
    if (c.read (0x1c) != n.read (0x1c) && ignore.find (",env3Output,") == std::string::npos) d += "read(0x1C) differs; ";
    if (c.read (0x5b) != n.read (0x5b) && ignore.find (",osc3Output,") == std::string::npos) d += "read(0x5B) differs; ";
    if (c.read (0x00) != 0) d += "read(0x00) nonzero; ";
    return d;
}

std::string ignoreList()
{
    const char* e = std::getenv ("BSIDE_STEPPER_IGNORE");
    return e ? std::string (",") + e + "," : std::string (",");
}

std::string opsToString (const std::vector<Op>& ops)
{
    std::ostringstream o;
    char buf[64];
    for (const auto& op : ops)
    {
        std::snprintf (buf, sizeof buf, "  +%llu cycles; write 0x%02x <- 0x%02x\n", static_cast<unsigned long long> (op.gap), op.reg, op.val);
        o << buf;
    }
    return o.str();
}

/** Replays ops from reset: stepCycle chip compared after every cycle, advanceCycles chip at the end of each
    gap and at a split point, both after every write. Returns the first mismatch ("" if none). */
std::string replay (Model model, const std::vector<Op>& ops, bool dense, const std::string& ignore, std::size_t* failedAt = nullptr)
{
    Config cfg;
    cfg.model = model;
    Chip cs, ca;
    cs.prepare (cfg);
    ca.prepare (cfg);
    NaiveChip n;
    std::string d = diffAll (cs, n, ignore);
    if (! d.empty()) return "after reset: " + d;
    std::size_t k = 0;
    for (const auto& op : ops)
    {
        if (failedAt) *failedAt = k;
        ++k;
        const std::uint64_t half = op.gap / 2;
        ca.advanceCycles (half);
        for (std::uint64_t i = 0; i < op.gap; ++i)
        {
            n.step();
            cs.stepCycle();
            if (i + 1 == half)
            {
                // advanceCycles chip at a mid point: needs a naive snapshot at that cycle
                d = diffAll (ca, n, ignore);
                if (! d.empty()) return "advanceCycles(mid-gap) vs naive: " + d;
            }
            if (dense)
            {
                d = diffAll (cs, n, ignore);
                if (! d.empty()) return "stepCycle vs naive: " + d;
            }
        }
        ca.advanceCycles (op.gap - half);
        d = diffAll (cs, n, ignore);
        if (! d.empty()) return "stepCycle vs naive: " + d;
        d = diffAll (ca, n, ignore);
        if (! d.empty()) return "advanceCycles vs naive: " + d;
        cs.write (op.reg, op.val);
        ca.write (op.reg, op.val);
        n.write (op.reg, op.val);
        d = diffAll (cs, n, ignore);
        if (! d.empty()) return "stepCycle chip after write vs naive: " + d;
        d = diffAll (ca, n, ignore);
        if (! d.empty()) return "advanceCycles chip after write vs naive: " + d;
    }
    return {};
}

/** Reduces a failing op list: shortest suffix from reset, then single-op removal, then gap halving. */
std::vector<Op> minimise (Model model, std::vector<Op> ops, const std::string& ignore)
{
    auto fails = [&] (const std::vector<Op>& v) { return ! replay (model, v, true, ignore).empty(); };
    for (std::size_t keep = 1; keep < ops.size(); keep *= 2)
    {
        std::vector<Op> tail (ops.end() - static_cast<std::ptrdiff_t> (keep), ops.end());
        if (fails (tail)) { ops = tail; break; }
    }
    for (bool changed = true; changed;)
    {
        changed = false;
        for (std::size_t i = 0; i < ops.size(); ++i)
        {
            auto t = ops;
            t.erase (t.begin() + static_cast<std::ptrdiff_t> (i));
            if (fails (t)) { ops = t; changed = true; --i; }
        }
        for (auto& op : ops)
            while (op.gap > 0)
            {
                auto t = ops;
                const std::uint64_t old = op.gap;
                op.gap = old / 2;
                if (fails (ops)) { changed = true; continue; }
                op.gap = old - 1;
                if (fails (ops)) { changed = true; continue; }
                op.gap = old;
                break;
            }
    }
    return ops;
}

struct RunResult
{
    std::string error;
    std::vector<Op> minimal;
    NaiveChip::Cov cov;
    std::uint64_t cycles = 0, writes = 0;
};

RunResult run (Model model, std::uint64_t totalCycles, std::uint64_t seed, bool denseStep)
{
    RunResult res;
    const std::string ignore = ignoreList();
    Rng rng (seed);
    Config cfg;
    cfg.model = model;
    Chip cs, ca;
    cs.prepare (cfg);
    ca.prepare (cfg);
    NaiveChip n;
    std::vector<Op> history;
    std::uint64_t cycles = 0;
    auto fail = [&] (const std::string& what)
    {
        res.error = what;
        res.minimal = minimise (model, history, ignore);
    };
    while (cycles < totalCycles)
    {
        Op op = randomOp (rng);
        history.push_back (op);
        const std::uint64_t half = rng.chance (50) ? op.gap / 2 : (op.gap ? rng.next() % (op.gap + 1) : 0);
        ca.advanceCycles (half);
        const bool dense = denseStep || op.gap <= 64;
        for (std::uint64_t i = 0; i < op.gap; ++i)
        {
            n.step();
            cs.stepCycle();
            if (i + 1 == half)
            {
                const auto d = diffAll (ca, n, ignore);
                if (! d.empty()) { fail ("advanceCycles(mid-gap) vs naive: " + d); res.cov = n.cov; return res; }
            }
            if (dense)
            {
                const auto d = diffAll (cs, n, ignore);
                if (! d.empty()) { fail ("stepCycle vs naive: " + d); res.cov = n.cov; return res; }
            }
        }
        ca.advanceCycles (op.gap - half);
        cycles += op.gap;
        std::string d = diffAll (cs, n, ignore);
        if (d.empty()) d = diffAll (ca, n, ignore);
        if (! d.empty()) { fail ("end of gap: " + d); res.cov = n.cov; return res; }
        cs.write (op.reg, op.val);
        ca.write (op.reg, op.val);
        n.write (op.reg, op.val);
        ++res.writes;
        d = diffAll (cs, n, ignore);
        if (d.empty()) d = diffAll (ca, n, ignore);
        if (! d.empty()) { fail ("after write: " + d); res.cov = n.cov; return res; }
        if (history.size() > 200000 && rng.chance (1)) { /* keep memory bounded: restart all three from reset */
            cs.reset(); ca.reset(); n = NaiveChip(); history.clear();
        }
    }
    res.cov = n.cov;
    res.cycles = cycles;
    return res;
}

std::string covText (const RunResult& r)
{
    const auto& c = r.cov;
    std::ostringstream o;
    o << "cycles " << r.cycles << ", writes " << r.writes << "\n"
      << "gate rise " << c.gateRise << ", gate fall " << c.gateFall << ", test set " << c.testSet << ", test clear " << c.testClear
      << ", sync resets " << c.syncResets << ", ring+triangle cycles " << c.ringTriCycles << "\n"
      << "noise clocks " << c.noiseClocks << ", write-back cycles " << c.writeBackCycles << ", noise-locked (0) cycles "
      << c.lockedNoiseCycles << "\n"
      << "attack completed " << c.attackDone << ", decay held at sustain " << c.sustainHold << ", release reached 0 "
      << c.releaseToZero << "\n"
      << "sustain lowered during decay " << c.sustainLowered << ", raised " << c.sustainRaised << ", period set below count "
      << c.periodBelowCount << ", rate counter wraps " << c.rateWraps << "\n"
      << "freq 0 writes " << c.freqZero << ", freq 0xFFFF writes " << c.freqMax << "\n";
    o << "env steps by nibble (attack/decay/release):";
    for (std::size_t i = 0; i < 16; ++i) o << " " << i << ":" << c.attackStep[i] << "/" << c.decayStep[i] << "/" << c.releaseStep[i];
    o << "\ndivider period reached (1,2,4,8,16,30):";
    for (auto v : c.dividerSeen) o << " " << v;
    o << "\ncycles per control bits 7..4 (0..15):";
    for (auto v : c.waveCombo) o << " " << v;
    return o.str();
}

void requireCoverage (const RunResult& r)
{
    const auto& c = r.cov;
    CHECK (c.gateRise > 100);
    CHECK (c.gateFall > 100);
    CHECK (c.testSet > 20);
    CHECK (c.syncResets > 100);
    CHECK (c.ringTriCycles > 1000);
    CHECK (c.noiseClocks > 1000);
    CHECK (c.writeBackCycles > 1000);
    CHECK (c.lockedNoiseCycles > 0);
    CHECK (c.attackDone > 10);
    CHECK (c.sustainHold > 10);
    CHECK (c.releaseToZero > 10);
    CHECK (c.sustainLowered > 5);
    CHECK (c.sustainRaised > 5);
    CHECK (c.periodBelowCount > 5);
    CHECK (c.freqZero > 5);
    CHECK (c.freqMax > 5);
    for (auto v : c.waveCombo) CHECK (v > 1000);
}

void runModel (Model model, std::uint64_t cycles, std::uint64_t seed, bool dense)
{
    const auto r = run (model, cycles, seed, dense);
    INFO ("model " << (model == Model::A ? "A" : "B") << " seed " << seed << "\n" << covText (r));
    if (! r.error.empty())
    {
        FAIL (r.error << "\nminimal sequence from reset (" << r.minimal.size() << " writes):\n" << opsToString (r.minimal));
    }
    requireCoverage (r);
    std::printf ("[stepper] model %s seed %llu\n%s\n", model == Model::A ? "A" : "B", static_cast<unsigned long long> (seed), covText (r).c_str());
}
} // namespace

namespace
{
using Seq = std::vector<Op>;
/** Runs a minimal sequence under both readings; returns {referenceStyleResult, literalTextResult}. */
std::pair<std::string, std::string> bothReadings (Model m, const Seq& s, unsigned mask)
{
    std::string a, b;
    {
        ScopedReadings g (kAll);
        a = replay (m, s, true, ",");
    }
    {
        ScopedReadings g (kAll & ~mask);
        b = replay (m, s, true, ",");
    }
    return { a, b };
}
} // namespace

TEST_CASE ("ambiguities: the implementation follows the reference-style reading; the literal text reading differs", "[bside][chip][stepper][ambiguity]")
{
    struct Case { const char* name; unsigned mask; Seq seq; };
    const std::vector<Case> cases = {
        { "wrap: rate counter after 0x7fff (8.4.3 row 11)", kWrap1,
          { { 0, 0x06, 0x0f }, { 31000, 0x06, 0x00 }, { 1800, 0x19, 0 } } },
        { "divider period at counter values not on a threshold (row 10)", kDivLatch,
          { { 0, 0x04, 0x01 }, { 9, 0x19, 0 } } },
        { "sustain raised above the counter during decay (row 10)", kSustainEq,
          { { 0, 0x06, 0x30 }, { 0, 0x04, 0x01 }, { 3500, 0x06, 0xf0 }, { 3000, 0x19, 0 } } },
        { "attack started at counter 0xFF (row 10)", kAttackWrap,
          { { 0, 0x06, 0xf0 }, { 0, 0x04, 0x01 }, { 2400, 0x04, 0x00 }, { 0, 0x04, 0x01 }, { 30, 0x19, 0 } } },
        { "sync chain: source reset in the same cycle (row 6)", kChainSuppress,
          { { 0, 0x01, 0x80 }, { 0, 0x0f, 0x80 }, { 0, 0x07, 0x01 }, { 0, 0x0b, 0x02 }, { 0, 0x04, 0x02 }, { 300, 0x19, 0 } } },
    };
    for (const auto& c : cases)
        for (Model m : { Model::A, Model::B })
        {
            const auto r = bothReadings (m, c.seq, c.mask);
            INFO (c.name << "\nsequence from reset:\n" << opsToString (c.seq) << "\nreference-style reading: " << (r.first.empty() ? "equal" : r.first)
                         << "\nliteral text reading:    " << (r.second.empty() ? "equal" : r.second));
            CHECK (r.first.empty());
            CHECK (! r.second.empty());
        }
}

TEST_CASE ("chip digital core equals the naive per-cycle stepper (10^7 cycles per model)", "[bside][chip][stepper]")
{
    runModel (Model::A, 10'000'000, 0x5eed0001, true);
    runModel (Model::B, 10'000'000, 0x5eed0002, true);
}

TEST_CASE ("chip digital core equals the naive per-cycle stepper (10^8 cycles per model)", "[.stepper-long]")
{
    runModel (Model::A, 100'000'000, 0x10c0de01, false);
    runModel (Model::B, 100'000'000, 0x10c0de02, false);
}
