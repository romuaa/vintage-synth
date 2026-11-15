// Digital unit gate (docs/architecture.md 8.9.1, row "Digital unit gates"): the chip's digital core
// (stepCycle and the fast path advanceCycles) against a naive per-cycle stepper written from the
// per-cycle specification 8.15 (which supersedes 8.4.3 where they differ).
//
// NaiveChip below is deliberately the simplest code that follows the specification's text, sentence
// by sentence: one function per part of a step, plain 24-bit integers, no fast paths, and nothing shared
// with the implementation (only the public constants of ChipTypes.h, the register addresses and the
// DigitalState struct, to compare field by field). It uses register writes only (Authentic values), so
// the accumulator fraction is always 0.
//
// History: the first version of this stepper (written from 8.4.1-8.4.3 before 8.15 existed) carried
// five "readings" for ambiguous sentences (rate-counter wrap, divider latch, sustain equality, attack
// wrap at 0xff, sync chain). 8.15 settled all five (8.15.4 steps 4 and 7, "The counter can wrap from
// 0xff to 0x00", 8.15.2 (d)); their minimal sequences are kept below as regression cases.
//
// The long run is hidden: `bside_unit "[.stepper-long]"` runs 10^8 cycles per model.
// Triage: BSIDE_STEPPER_IGNORE = comma list of field names to skip in the comparison.

#include "bside/chip/Chip.h"
#include "bside/chip/ChipTypes.h"
#include "bside/chip/CombinedWaveforms.h"

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

struct NaiveChip
{
    explicit NaiveChip (Model m) : model (m)
    {
        for (auto& x : o)
            x = O {};
        for (auto& x : e)
            x = E {};
    }

    Model model;
    struct O
    {
        std::uint32_t acc = 0x555555, F = 0;
        std::uint16_t PW = 0;
        std::uint32_t reg = 0x7ffffe, lat = 0x7ffffe;
        std::uint16_t nout = 0xfe0; // the noise output of the power-on register (see the tests below)
        int pipe = 0;
        bool fs = true, rise = false;
        std::uint32_t fill = 0, fade = 0; // 0: stopped
        std::uint16_t out = 0, rv = 0, bl = 0x555;
        bool pc = true;
    } o[3];
    struct E
    {
        int cnt = 0xaa, ph = 2, tgt = 2, pp = 0, sp = 0, dp = 0;
        int pos = 0, cmp = 8;
        bool pend = true;
        int dc = 0, dper = 1, pendper = 0;
        bool frz = false;
        int rs = 0;
    } e[3];
    std::uint8_t regs[0x19] = {};
    std::uint64_t cycle = 0;

    // Coverage counters (what the random driver actually reached).
    struct Cov
    {
        std::uint64_t gateRise = 0, gateFall = 0, testSet = 0, testClear = 0, syncResets = 0, chainSuppressed = 0, ringTriCycles = 0,
                      shifts = 0, interruptedPipelines = 0, fills = 0, filledToOnes = 0, fades = 0, writeBacks = 0, shiftWriteBacks = 0,
                      pullDowns = 0, lockedNoiseCycles = 0, attackDone = 0, lostGateOff = 0, sustainHold = 0, releaseToZero = 0,
                      attackWrap = 0, decayWrap = 0, periodBelowPosition = 0, latchReads = 0, freqZero = 0, freqMax = 0;
        std::array<std::uint64_t, 6> dividerSeen {}; // periods 1,2,4,8,16,30
        std::array<std::uint64_t, 16> waveCombo {};  // control bits 7..4 value, cycles
    } cov;

    static constexpr int rateMatch[16] = { 8, 31, 62, 94, 148, 219, 266, 312, 391, 976, 1953, 3125, 3906, 11719, 19531, 31250 };
    static int src (int i) { return i == 0 ? 2 : i - 1; }
    static int tgt (int i) { return i == 2 ? 0 : i + 1; }
    std::uint8_t ctl (int i) const { return regs[7 * i + 4]; }
    int waveOf (int i) const { return ctl (i) >> 4; }
    bool test (int i) const { return (ctl (i) & 8) != 0; }
    int A (int i) const { return regs[7 * i + 5] >> 4; }
    int D (int i) const { return regs[7 * i + 5] & 15; }
    int S (int i) const { return (regs[7 * i + 6] >> 4) * 0x11; }
    int R (int i) const { return regs[7 * i + 6] & 15; }
    std::uint32_t holdTime() const { return model == Model::A ? 50000u : 986000u; }
    std::uint32_t fillInterval() const { return model == Model::A ? 15000u : 314300u; }
    std::uint32_t fadeHold() const { return model == Model::A ? 54000u : 800000u; }
    std::uint32_t fadeInterval() const { return model == Model::A ? 1400u : 50000u; }

    static int bitOf (std::uint32_t v, int b) { return static_cast<int> ((v >> b) & 1u); }
    static const int* taps()
    {
        static const int t[8] = { 20, 18, 14, 11, 9, 5, 2, 0 }; // output bits 11..4
        return t;
    }
    static std::uint16_t noiseOutputOf (std::uint32_t r)
    {
        unsigned v = 0;
        for (int k = 0; k < 8; ++k)
            v |= static_cast<unsigned> (bitOf (r, taps()[k])) << (11 - k);
        return static_cast<std::uint16_t> (v);
    }

    // ---- 8.15.3 ----
    bool writeBackApplies (int oldW, int newW) const
    {
        if (oldW <= 8) return false;
        if (newW < 8) return false;
        if (newW == 8 && oldW != 0xf) return false;
        if (model == Model::A && (((oldW & 3) == 1 && (newW & 3) == 2) || ((oldW & 3) == 2 && (newW & 3) == 1))) return false;
        if (oldW == 0xc || newW == 0xc) return false;
        return true;
    }
    void shift (int i, int oldW, int newW)
    {
        O& x = o[i];
        if (writeBackApplies (oldW, newW))
        {
            ++cov.shiftWriteBacks;
            for (int k = 0; k < 8; ++k)
            {
                const std::uint32_t m = 1u << taps()[k];
                if (bitOf (x.out, 11 - k)) x.lat |= m; else x.lat &= ~m;
            }
        }
        const int nb = (bitOf (x.lat, 22) | (x.fs ? 1 : 0)) ^ bitOf (x.lat, 17);
        x.reg = ((x.lat << 1) & 0x7fffffu) | static_cast<std::uint32_t> (nb);
        x.nout = noiseOutputOf (x.reg);
        ++cov.shifts;
    }

    // ---- 8.15.2 (a) ----
    void advance (int i)
    {
        O& x = o[i];
        if (test (i))
        {
            if (x.fill != 0)
            {
                --x.fill;
                if (x.fill == 0)
                {
                    x.reg = (x.reg | (x.reg << 1) | 1u) & 0x7fffffu;
                    x.lat = x.reg;
                    x.nout = noiseOutputOf (x.reg);
                    ++cov.fills;
                    if (x.reg != 0x7fffffu) x.fill = fillInterval();
                    else ++cov.filledToOnes;
                }
            }
            x.fs = true;
            x.rise = false; // our chip
            return;
        }
        const std::uint32_t before = x.acc;
        x.acc = (x.acc + x.F) & 0xffffffu;
        x.rise = bitOf (before, 23) == 0 && bitOf (x.acc, 23) == 1;
        if (bitOf (before, 19) == 0 && bitOf (x.acc, 19) == 1)
        {
            if (x.pipe > 0) ++cov.interruptedPipelines;
            x.pipe = 2;
        }
        else if (x.pipe > 0)
        {
            --x.pipe;
            if (x.pipe == 1)
            {
                x.lat = x.reg;
                x.fs = false;
            }
            else
                shift (i, waveOf (i), waveOf (i));
        }
    }

    // ---- 8.15.2 (c) ----
    std::uint16_t combinedShape (int w, std::uint16_t index) const
    {
        // The combined-waveform shapes are data, not logic: the stepper reads the same fitted tables as the
        // chip (CombinedWaveforms.h; their values are tested against the reference by bside_trace). Noise
        // with only the triangle or only the saw uses no table (8.15.2 (c) step 2).
        if (w == 9) return triangleOf (index);
        if (w == 10) return index;
        return vs::bside::chip::combined::fittedTables (model).wave[static_cast<std::size_t> (w)][index];
    }
    static std::uint16_t triangleOf (std::uint16_t index)
    {
        const unsigned t = index < 0x800 ? index : (index ^ 0xfffu);
        return static_cast<std::uint16_t> ((t << 1) & 0xfffu);
    }
    void output (int i)
    {
        O& x = o[i];
        const std::uint8_t c = ctl (i);
        const int w = c >> 4;
        if (w != 0)
        {
            ++cov.waveCombo[static_cast<std::size_t> (w)];
            std::uint16_t index = static_cast<std::uint16_t> (x.acc >> 12);
            const bool ring = (c & 4) != 0, saw = (c & 0x20) != 0;
            if (ring && ! saw)
            {
                const int b = bitOf (x.acc, 23) ^ (1 - bitOf (o[src (i)].acc, 23));
                index = static_cast<std::uint16_t> ((index & 0x7ff) | (b << 11));
                if (c & 0x10) ++cov.ringTriCycles;
            }
            std::uint16_t shape;
            if (w == 1) shape = triangleOf (index);
            else if (w == 2) shape = index;
            else if (w == 4 || w == 8) shape = 0xfff;
            else shape = combinedShape (w, index);
            const std::uint16_t pmask = (c & 0x40) ? ((test (i) || x.pc) ? 0xfff : 0) : 0xfff;
            const std::uint16_t nmask = (c & 0x80) ? x.nout : 0xfff;
            x.out = static_cast<std::uint16_t> (shape & pmask & nmask);
            if (model == Model::A && saw && ! bitOf (x.out, 11))
            {
                if (bitOf (x.acc, 23)) ++cov.pullDowns;
                x.acc &= ~0x800000u;
                x.rise = false;
            }
            if (model == Model::B && (c & 0x30) != 0)
            {
                x.rv = static_cast<std::uint16_t> (x.bl & pmask & nmask);
                x.bl = shape;
                ++cov.latchReads;
            }
            else
                x.rv = x.out;
            if ((c & 0x80) && (c & 0x70))
            {
                if (x.pipe != 1 && ! test (i))
                {
                    ++cov.writeBacks;
                    for (int k = 0; k < 8; ++k)
                        if (! bitOf (x.out, 11 - k)) x.reg &= ~(1u << taps()[k]);
                    x.nout = static_cast<std::uint16_t> (x.nout & x.out);
                    if (x.reg == 0) ++cov.lockedNoiseCycles;
                }
                else
                    x.nout = x.out;
            }
        }
        else if (x.fade != 0)
        {
            --x.fade;
            if (x.fade == 0)
            {
                ++cov.fades;
                x.out = static_cast<std::uint16_t> (x.out & (x.out >> 1));
                x.rv = x.out;
                if (x.out != 0) x.fade = fadeInterval();
            }
        }
        x.pc = static_cast<int> (x.acc >> 12) >= x.PW;
    }

    // ---- 8.15.2 (d) ----
    void sync()
    {
        bool reset[3] = { false, false, false };
        for (int n = 0; n < 3; ++n)
        {
            const int t = tgt (n);
            const bool ownSyncAndSourceRose = (ctl (n) & 2) && o[src (n)].rise;
            if (o[n].rise && (ctl (t) & 2))
            {
                if (ownSyncAndSourceRose) ++cov.chainSuppressed;
                else reset[t] = true;
            }
        }
        for (int t = 0; t < 3; ++t)
            if (reset[t])
            {
                o[t].acc = 0;
                ++cov.syncResets;
            }
    }

    // ---- 8.15.4 ----
    void envelope (int i)
    {
        E& x = e[i];
        x.rs = x.cnt;                                   // 1
        if (x.pendper) { x.dper = x.pendper; x.pendper = 0; } // 2
        if (x.pp > 0)                                   // 3
        {
            --x.pp;
            if (x.tgt == 0)
            {
                if (x.pp == 1) x.cmp = rateMatch[D (i)];
                if (x.pp == 0) { x.ph = 0; x.cmp = rateMatch[A (i)]; x.frz = false; }
            }
            else if (x.tgt == 1)
            {
                if (x.pp == 0) { x.ph = 1; x.cmp = rateMatch[D (i)]; }
            }
            else if ((x.ph == 0 && x.pp == 0) || (x.ph == 1 && x.pp == 1))
            {
                x.ph = 2;
                x.cmp = rateMatch[R (i)];
            }
        }
        bool used = false;                              // 4
        if (x.sp > 0)
        {
            --x.sp;
            used = x.sp == 0;
        }
        if (used && ! x.frz)
        {
            if (x.ph == 0)
            {
                if (x.cnt == 0xff) ++cov.attackWrap;
                x.cnt = (x.cnt + 1) & 0xff;
                if (x.cnt == 0xff)
                {
                    if (x.tgt == 2 && x.pp > 0) ++cov.lostGateOff;
                    x.tgt = 1;
                    x.pp = 3;
                    ++cov.attackDone;
                }
            }
            else
            {
                if (x.cnt == 0) ++cov.decayWrap; // possible when an attack phase cleared the freeze before any step
                x.cnt = (x.cnt - 1) & 0xff;      // 8-bit counter
                if (x.cnt == 0) { x.frz = true; if (x.ph == 2) ++cov.releaseToZero; }
            }
            static const int at[7] = { 0xff, 0x00, 0x5d, 0x36, 0x1a, 0x0e, 0x06 };
            static const int per[7] = { 1, 1, 2, 4, 8, 16, 30 };
            for (int k = 0; k < 7; ++k)
                if (x.cnt == at[k]) x.pendper = per[k];
        }
        if (! used && x.dp > 0)                         // 5
        {
            --x.dp;
            if (x.dp == 0)
            {
                used = true;
                x.dc = 0;
                if ((x.ph == 1 && x.cnt != S (i)) || x.ph == 2) x.sp = 1;
                else if (x.ph == 1) ++cov.sustainHold;
            }
        }
        if (! used && x.pend)                           // 6
        {
            x.pos = 0;
            x.pend = false;
            if (x.ph == 0)
            {
                x.dc = 0;
                x.sp = 2;
            }
            else if (! x.frz)
            {
                x.dc += 1;
                if (x.dc == x.dper)
                {
                    x.dp = x.dper == 1 ? 1 : 2;
                    static const int dv[6] = { 1, 2, 4, 8, 16, 30 };
                    for (int d = 0; d < 6; ++d)
                        if (x.dper == dv[d]) ++cov.dividerSeen[static_cast<std::size_t> (d)];
                }
            }
        }
        if (x.pos != x.cmp) x.pos = (x.pos + 1) % 32767; // 7
        else x.pend = true;
    }

    void step()
    {
        for (int i = 0; i < 3; ++i) advance (i);
        for (int i = 0; i < 3; ++i) envelope (i);
        for (int i = 0; i < 3; ++i) output (i);
        sync();
        ++cycle;
    }

    // ---- writes (8.15.4 "Writes", 8.15.5) ----
    void write (std::uint8_t a, std::uint8_t v)
    {
        a &= 0x1f;
        if (a >= 0x19) return;
        const std::uint8_t old = regs[a];
        regs[a] = v;
        if (a >= 21) return;
        const int i = a / 7, k = a % 7;
        O& x = o[i];
        E& en = e[i];
        if (k <= 1)
        {
            x.F = regs[7 * i] | (static_cast<std::uint32_t> (regs[7 * i + 1]) << 8);
            if (x.F == 0) ++cov.freqZero;
            if (x.F == 0xffff) ++cov.freqMax;
        }
        else if (k <= 3)
            x.PW = static_cast<std::uint16_t> (regs[7 * i + 2] | ((regs[7 * i + 3] & 15) << 8));
        else if (k == 4)
        {
            const int oldW = old >> 4, newW = v >> 4;
            if (oldW != newW && newW == 0) x.fade = fadeHold();
            if (! (old & 8) && (v & 8))
            {
                ++cov.testSet;
                x.acc = 0;
                x.pipe = 0;
                x.lat = x.reg;
                x.fill = holdTime();
            }
            if ((old & 8) && ! (v & 8))
            {
                ++cov.testClear;
                shift (i, oldW, newW);
            }
            if (! (old & 1) && (v & 1))
            {
                ++cov.gateRise;
                en.tgt = 0;
                en.pp = 2;
                if (en.pend || en.dp == 2) en.sp = (en.dper == 1 || en.dp == 2) ? 2 : 4;
                else if (en.dp == 1) en.pp = 3;
            }
            if ((old & 1) && ! (v & 1))
            {
                ++cov.gateFall;
                en.tgt = 2;
                en.pp = en.sp > 0 ? 3 : 2;
            }
        }
        else if (k == 5)
        {
            if (en.ph == 0) en.cmp = rateMatch[A (i)];
            else if (en.ph == 1) en.cmp = rateMatch[D (i)];
            if (en.cmp < en.pos) ++cov.periodBelowPosition;
        }
        else if (k == 6)
        {
            if (en.ph == 2) en.cmp = rateMatch[R (i)];
            if (en.cmp < en.pos) ++cov.periodBelowPosition;
        }
    }

    std::uint8_t read (std::uint8_t a) const
    {
        a &= 0x1f;
        return a == 0x1b ? static_cast<std::uint8_t> (o[2].rv >> 4) : a == 0x1c ? static_cast<std::uint8_t> (e[2].rs) : std::uint8_t { 0 };
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
    if (w < 14) wave = 0;
    else if (w < 40) wave = 1u << r.below (4);          // a single waveform (incl. noise)
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
    else if (g < 990) op.gap = 2000 + r.below (6000);
    else if (g < 996) op.gap = 8000 + r.below (32000);
    else op.gap = 32000 + r.below (90000); // long enough for Model A's test-bit fill and output fade

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
    std::ostringstream os;
    auto skip = [&ignore] (const std::string& name) { return ignore.find ("," + name + ",") != std::string::npos; };
    auto chk = [&] (const std::string& name, std::uint64_t a, std::uint64_t b)
    {
        if (a != b && ! skip (name)) os << name << ": impl " << a << " naive " << b << "; ";
    };
    chk ("cycle", s.cycle, n.cycle);
    for (std::size_t i = 0; i < 3; ++i)
    {
        const std::string p = "osc" + std::to_string (i) + ".";
        const auto& a = s.osc[i];
        const auto& b = n.o[i];
        chk (p + "acc", a.acc, b.acc);
        chk (p + "accFraction", a.accFraction, 0);
        chk (p + "increment", a.increment, static_cast<std::uint64_t> (b.F) << 32);
        chk (p + "pulseWidth", a.pulseWidth, b.PW);
        chk (p + "noise", a.noise, b.reg);
        chk (p + "noiseLatch", a.noiseLatch, b.lat);
        chk (p + "noiseOutput", a.noiseOutput, b.nout);
        chk (p + "noisePipeline", a.noisePipeline, static_cast<std::uint64_t> (b.pipe));
        chk (p + "firstShift", a.firstShift, b.fs);
        chk (p + "rise", a.rise, b.rise);
        chk (p + "fillTimer", a.fillTimer, b.fill);
        chk (p + "fadeTimer", a.fadeTimer, b.fade);
        chk (p + "output", a.output, b.out);
        chk (p + "readValue", a.readValue, b.rv);
        chk (p + "readLatch", a.readLatch, b.bl);
        chk (p + "pulseHigh", a.pulseHigh, b.pc);
        const auto& ea = s.env[i];
        const auto& eb = n.e[i];
        const std::string q = "env" + std::to_string (i) + ".";
        chk (q + "counter", ea.counter, static_cast<std::uint64_t> (eb.cnt));
        chk (q + "phase", ea.phase, static_cast<std::uint64_t> (eb.ph));
        chk (q + "target", ea.target, static_cast<std::uint64_t> (eb.tgt));
        chk (q + "phasePipeline", ea.phasePipeline, static_cast<std::uint64_t> (eb.pp));
        chk (q + "stepPipeline", ea.stepPipeline, static_cast<std::uint64_t> (eb.sp));
        chk (q + "dividerPipeline", ea.dividerPipeline, static_cast<std::uint64_t> (eb.dp));
        chk (q + "position", ea.position, static_cast<std::uint64_t> (eb.pos));
        chk (q + "compare", ea.compare, static_cast<std::uint64_t> (eb.cmp));
        chk (q + "resetPending", ea.resetPending, eb.pend);
        chk (q + "divider", ea.divider, static_cast<std::uint64_t> (eb.dc));
        chk (q + "dividerPeriod", ea.dividerPeriod, static_cast<std::uint64_t> (eb.dper));
        chk (q + "pendingPeriod", ea.pendingPeriod, static_cast<std::uint64_t> (eb.pendper));
        chk (q + "frozen", ea.frozen, eb.frz);
        chk (q + "readSample", ea.readSample, static_cast<std::uint64_t> (eb.rs));
    }
    for (std::size_t r = 0; r < 0x19; ++r)
        chk ("reg" + std::to_string (r), s.registers[r], n.regs[r]);
    chk ("osc3Output", s.osc3Output, n.read (0x1b));
    chk ("env3Output", s.env3Output, n.read (0x1c));
    return os.str();
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
    const char* ev = std::getenv ("BSIDE_STEPPER_IGNORE");
    return ev ? std::string (",") + ev + "," : std::string (",");
}

std::string opsToString (const std::vector<Op>& ops)
{
    std::ostringstream os;
    char buf[64];
    for (const auto& op : ops)
    {
        std::snprintf (buf, sizeof buf, "  +%llu cycles; write 0x%02x <- 0x%02x\n", static_cast<unsigned long long> (op.gap), op.reg, op.val);
        os << buf;
    }
    return os.str();
}

Chip makeChip (Model model)
{
    Config cfg;
    cfg.model = model;
    cfg.authentic = true;
    Chip c;
    c.prepare (cfg);
    return c;
}

/** Replays ops from reset: stepCycle chip compared after every cycle, advanceCycles chip at the end of each
    gap and at a split point, both after every write. Returns the first mismatch ("" if none). */
std::string replay (Model model, const std::vector<Op>& ops, bool dense, const std::string& ignore, std::size_t* failedAt = nullptr)
{
    Chip cs = makeChip (model), ca = makeChip (model);
    NaiveChip n (model);
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
    Chip cs = makeChip (model), ca = makeChip (model);
    NaiveChip n (model);
    std::vector<Op> history;
    std::uint64_t cycles = 0;
    auto fail = [&] (const std::string& what)
    {
        res.error = what;
        res.minimal = minimise (model, history, ignore);
        res.cov = n.cov;
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
                if (! d.empty()) { fail ("advanceCycles(mid-gap) vs naive: " + d); return res; }
            }
            if (dense)
            {
                const auto d = diffAll (cs, n, ignore);
                if (! d.empty()) { fail ("stepCycle vs naive: " + d); return res; }
            }
        }
        ca.advanceCycles (op.gap - half);
        cycles += op.gap;
        std::string d = diffAll (cs, n, ignore);
        if (d.empty()) d = diffAll (ca, n, ignore);
        if (! d.empty()) { fail ("end of gap: " + d); return res; }
        cs.write (op.reg, op.val);
        ca.write (op.reg, op.val);
        n.write (op.reg, op.val);
        ++res.writes;
        d = diffAll (cs, n, ignore);
        if (d.empty()) d = diffAll (ca, n, ignore);
        if (! d.empty()) { fail ("after write: " + d); return res; }
        if (history.size() > 20000 && rng.chance (1)) { /* keep the minimiser's input bounded: restart all three from reset */
            const auto keep = n.cov;
            cs.reset(); ca.reset(); n = NaiveChip (model); n.cov = keep; history.clear();
        }
    }
    res.cov = n.cov;
    res.cycles = cycles;
    return res;
}

std::string covText (const RunResult& r)
{
    const auto& c = r.cov;
    std::ostringstream os;
    os << "cycles " << r.cycles << ", writes " << r.writes << "\n"
       << "gate rise " << c.gateRise << ", gate fall " << c.gateFall << ", test set " << c.testSet << ", test clear " << c.testClear
       << ", sync resets " << c.syncResets << ", chains suppressed " << c.chainSuppressed << ", ring+triangle cycles " << c.ringTriCycles << "\n"
       << "noise shifts " << c.shifts << ", interrupted pipelines " << c.interruptedPipelines << ", fills " << c.fills << " (to all ones "
       << c.filledToOnes << "), fades " << c.fades << ", write-backs " << c.writeBacks << ", shift write-backs " << c.shiftWriteBacks
       << ", noise-locked (0) cycles " << c.lockedNoiseCycles << ", pull-downs " << c.pullDowns << ", Model B latch reads " << c.latchReads << "\n"
       << "attack completed " << c.attackDone << ", lost gate-offs " << c.lostGateOff << ", attack wraps " << c.attackWrap << ", decay/release wraps " << c.decayWrap
       << ", decay held at sustain " << c.sustainHold << ", release reached 0 " << c.releaseToZero << ", compare set below position "
       << c.periodBelowPosition << "\n"
       << "freq 0 writes " << c.freqZero << ", freq 0xFFFF writes " << c.freqMax << "\n";
    os << "divider period reached (1,2,4,8,16,30):";
    for (auto v : c.dividerSeen) os << " " << v;
    os << "\ncycles per control bits 7..4 (1..15):";
    for (std::size_t i = 1; i < 16; ++i) os << " " << c.waveCombo[i];
    return os.str();
}

void requireCoverage (Model model, const RunResult& r)
{
    const auto& c = r.cov;
    CHECK (c.gateRise > 100);
    CHECK (c.gateFall > 100);
    CHECK (c.testSet > 20);
    CHECK (c.testClear > 20);
    CHECK (c.syncResets > 100);
    // chainSuppressed (two sources rising in the same step) is too rare for random writes; the regression
    // sequence below, the row tests and the committed trace d_sync_ring_* pin it.
    CHECK (c.ringTriCycles > 1000);
    CHECK (c.shifts > 1000);
    CHECK (c.shiftWriteBacks > 10);
    CHECK (c.writeBacks > 1000);
    CHECK (c.lockedNoiseCycles > 0);
    if (model == Model::A)
    {
        CHECK (c.fills > 0);
        CHECK (c.fades > 0);
        CHECK (c.pullDowns > 100);
    }
    else
        CHECK (c.latchReads > 1000);
    CHECK (c.attackDone > 10);
    CHECK (c.sustainHold > 10);
    CHECK (c.releaseToZero > 10);
    CHECK (c.periodBelowPosition > 5);
    CHECK (c.freqZero > 5);
    CHECK (c.freqMax > 5);
    for (std::size_t i = 1; i < 16; ++i) CHECK (c.waveCombo[i] > 1000);
    for (auto v : c.dividerSeen) CHECK (v > 0);
}

void runModel (Model model, std::uint64_t cycles, std::uint64_t seed, bool dense)
{
    const auto r = run (model, cycles, seed, dense);
    INFO ("model " << (model == Model::A ? "A" : "B") << " seed " << seed << "\n" << covText (r));
    if (! r.error.empty())
    {
        FAIL (r.error << "\nminimal sequence from reset (" << r.minimal.size() << " writes):\n" << opsToString (r.minimal));
    }
    requireCoverage (model, r);
    std::printf ("[stepper] model %s seed %llu\n%s\n", model == Model::A ? "A" : "B", static_cast<unsigned long long> (seed), covText (r).c_str());
}

using Seq = std::vector<Op>;
} // namespace

TEST_CASE ("stepper: the naive stepper's power-on state is the chip's (8.15.1)", "[bside][chip][stepper]")
{
    for (Model m : { Model::A, Model::B })
    {
        const auto c = makeChip (m);
        const NaiveChip n (m);
        INFO (diffAll (c, n, ","));
        CHECK (diffAll (c, n, ",").empty());
    }
}

TEST_CASE ("stepper: the five formerly ambiguous cases follow 8.15 (regression sequences)", "[bside][chip][stepper][ambiguity]")
{
    // The minimal sequences that once separated the readings of 8.4.3 (see the file header). 8.15 decides
    // each; the chip and the naive stepper must agree on every cycle of each.
    struct Case { const char* name; Seq seq; };
    const std::vector<Case> cases = {
        { "rate counter position after 32 766 (8.15.4 step 7)", { { 0, 0x06, 0x0f }, { 31000, 0x06, 0x00 }, { 1800, 0x19, 0 } } },
        { "divider period is a state set when the counter takes a threshold value (8.15.4 step 4)", { { 0, 0x04, 0x01 }, { 9, 0x19, 0 }, { 3000, 0x04, 0x00 }, { 3000, 0x19, 0 } } },
        { "sustain raised above the counter during decay: decays to 0 (8.15.4 step 5)",
          { { 0, 0x06, 0x30 }, { 0, 0x04, 0x01 }, { 3500, 0x06, 0xf0 }, { 3000, 0x19, 0 } } },
        { "attack started at counter 0xFF wraps to 0 (8.15.4 consequences)",
          { { 0, 0x06, 0xf0 }, { 0, 0x04, 0x01 }, { 2400, 0x04, 0x00 }, { 0, 0x04, 0x01 }, { 30, 0x19, 0 } } },
        { "sync chain: a source reset in the same cycle does not reset its target (8.15.2 (d))",
          { { 0, 0x01, 0x80 }, { 0, 0x0f, 0x80 }, { 0, 0x07, 0x01 }, { 0, 0x0b, 0x02 }, { 0, 0x04, 0x02 }, { 300, 0x19, 0 } } },
    };
    for (const auto& c : cases)
        for (Model m : { Model::A, Model::B })
        {
            const auto r = replay (m, c.seq, true, ",");
            INFO (c.name << "\nsequence from reset:\n" << opsToString (c.seq) << "\nresult: " << (r.empty() ? "equal" : r));
            CHECK (r.empty());
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
