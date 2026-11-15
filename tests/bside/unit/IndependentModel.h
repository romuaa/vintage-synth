#pragma once

// AN INDEPENDENT PER-CYCLE MODEL OF THE CHIP'S DIGITAL CORE (docs/architecture.md 8.9.1, 8.15).
//
// Written from the text of contract 8.15 alone (8.15.1-8.15.5, with 8.4.1 for the register map) by a
// reviewer who had not read source/bside/chip/ or any other model of the chip. It was validated
// before it was committed: 0 mismatches at every checkpoint of every committed d_* and a_* trace and of
// the no-table c_9_* and c_a_* traces, and equal to the chip in every state field over random writes.
//
// RULE: whoever edits source/bside/chip/ must never edit this file. If chip and model disagree, the
// contract and the reference data decide; a change here needs its own reviewer who reads 8.15, not
// the chip. Deliberately simple and slow: one step per call, no closed forms, no shared code with the
// chip (only plain integers). JUCE-free, header-only, test code only.
//
// Combined waveforms: the contract's tables are our own fits (8.15.8) and are not part of this model.
// The model uses a placeholder for a shape made of at least two of triangle, saw and pulse: the AND of
// the selected triangle and saw shapes (0xfff when neither is selected), with pulse and noise entering
// through the masks of 8.15.2 (c) step 3. Noise with only triangle or only saw uses no table.

#include <cstdint>

namespace vs::bside::independent
{
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

/** Rate match positions, rates 0-15 (8.15.4). */
inline constexpr u16 kMatch[16] = { 8, 31, 62, 94, 148, 219, 266, 312, 391, 976, 1953, 3125, 3906, 11719, 19531, 31250 };
enum : int { kAttack = 0, kDecay = 1, kRelease = 2 };

/** Test-only rule changes, to show that the tests using this model can fail. */
struct Faults
{
    bool ringWithoutNot = false; // 8.15.2 (c) step 1: XOR the source's bit 23 instead of XOR NOT
};

struct Osc
{
    u32 acc = 0, F = 0, PW = 0;
    u8 ctrl = 0;
    u32 reg = 0, latch = 0;
    bool firstShift = false;
    int noisePipe = 0;
    bool rise = false;
    u32 fill = 0, fade = 0; // 0 = stopped
    u16 out = 0, rd = 0, noiseOut = 0;
    bool cmp = false;
    u16 btLatch = 0; // Model B triangle/saw read latch
};

struct Env
{
    u8 counter = 0;
    int phase = 0, target = 0;
    int phasePipe = 0, stepPipe = 0, divPipe = 0;
    u32 pos = 0;
    bool pending = false;
    u32 divCnt = 0, period = 0;
    int periodPending = -1; // -1: none
    bool frozen = false;
    u8 A = 0, D = 0, S = 0, R = 0;
    u8 readSample = 0;
    u32 cmpPos = 0;
};

struct Model
{
    bool modelA = true;
    Faults faults {};
    Osc o[3];
    Env e[3];
    u32 testHold = 0, fillInt = 0, fadeHold = 0, fadeInt = 0;

    static u16 noiseOutOf (u32 r)
    {
        static constexpr int taps[8] = { 20, 18, 14, 11, 9, 5, 2, 0 };
        u32 v = 0;
        for (int i = 0; i < 8; ++i)
            v |= ((r >> taps[i]) & 1u) << (11 - i);
        return static_cast<u16> (v);
    }
    static u32 tapMask() { return (1u << 20) | (1u << 18) | (1u << 14) | (1u << 11) | (1u << 9) | (1u << 5) | (1u << 2) | 1u; }
    static u32 spreadOut (u16 out) // out bits 11..4 to the tap positions
    {
        static constexpr int taps[8] = { 20, 18, 14, 11, 9, 5, 2, 0 };
        u32 r = 0;
        for (int i = 0; i < 8; ++i)
            r |= static_cast<u32> ((out >> (11 - i)) & 1u) << taps[i];
        return r;
    }
    bool wbApplies (u8 oldw, u8 neww) const
    {
        if (oldw <= 8)
            return false;
        if (neww < 8)
            return false;
        if (neww == 8 && oldw != 0xf)
            return false;
        if (modelA && (((oldw & 3) == 1 && (neww & 3) == 2) || ((oldw & 3) == 2 && (neww & 3) == 1)))
            return false;
        if (oldw == 0xc || neww == 0xc)
            return false;
        return true;
    }

    void reset (bool isModelA)
    {
        modelA = isModelA;
        testHold = modelA ? 50000u : 986000u;
        fillInt = modelA ? 15000u : 314300u;
        fadeHold = modelA ? 54000u : 800000u;
        fadeInt = modelA ? 1400u : 50000u;
        for (auto& x : o)
        {
            x = Osc {};
            x.acc = 0x555555;
            x.reg = 0x7ffffe;
            x.latch = x.reg;
            x.firstShift = true;
            x.cmp = true;
            x.btLatch = 0x555;
            x.noiseOut = noiseOutOf (x.reg);
        }
        for (auto& v : e)
        {
            v = Env {};
            v.counter = 0xaa;
            v.phase = kRelease;
            v.target = kRelease;
            v.pending = true;
            v.period = 1;
            v.periodPending = -1;
            v.cmpPos = kMatch[0];
        }
    }

    void shift (Osc& x, u8 oldw, u8 neww)
    {
        if (wbApplies (oldw, neww))
            x.latch = (x.latch & ~tapMask()) | spreadOut (x.out);
        const u32 l = x.latch;
        const u32 fb = ((((l >> 22) & 1u) | (x.firstShift ? 1u : 0u)) ^ ((l >> 17) & 1u));
        x.reg = ((l << 1) & 0x7fffffu) | fb;
        x.noiseOut = noiseOutOf (x.reg);
    }

    /** A register write between two steps (address 0x00-0x1f; 0x15 and above have no digital effect). */
    void write (int r, u8 v)
    {
        if (r > 0x14 || r < 0)
            return;
        const int n = r / 7, k = r % 7;
        Osc& x = o[n];
        Env& en = e[n];
        switch (k)
        {
            case 0: x.F = (x.F & 0xff00u) | v; break;
            case 1: x.F = (x.F & 0xffu) | (static_cast<u32> (v) << 8); break;
            case 2: x.PW = (x.PW & 0xf00u) | v; break;
            case 3: x.PW = (x.PW & 0xffu) | (static_cast<u32> (v & 0xfu) << 8); break;
            case 4:
            {
                const u8 old = x.ctrl;
                x.ctrl = v;
                const auto ow = static_cast<u8> (old >> 4), nw = static_cast<u8> (v >> 4);
                if (ow != nw && nw == 0)
                    x.fade = fadeHold;
                const bool ot = (old & 8) != 0, nt = (v & 8) != 0;
                if (! ot && nt)
                {
                    x.acc = 0;
                    x.noisePipe = 0;
                    x.latch = x.reg;
                    x.fill = testHold;
                }
                if (ot && ! nt)
                    shift (x, ow, nw);
                const bool og = (old & 1) != 0, ng = (v & 1) != 0;
                if (! og && ng)
                {
                    en.target = kAttack;
                    en.phasePipe = 2;
                    if (en.pending || en.divPipe == 2)
                        en.stepPipe = (en.period == 1 || en.divPipe == 2) ? 2 : 4;
                    else if (en.divPipe == 1)
                        en.phasePipe = 3;
                }
                else if (og && ! ng)
                {
                    en.target = kRelease;
                    en.phasePipe = en.stepPipe > 0 ? 3 : 2;
                }
                break;
            }
            case 5:
                en.A = static_cast<u8> (v >> 4);
                en.D = static_cast<u8> (v & 15);
                if (en.phase == kAttack)
                    en.cmpPos = kMatch[en.A];
                else if (en.phase == kDecay)
                    en.cmpPos = kMatch[en.D];
                break;
            case 6:
                en.S = static_cast<u8> ((v >> 4) * 0x11);
                en.R = static_cast<u8> (v & 15);
                if (en.phase == kRelease)
                    en.cmpPos = kMatch[en.R];
                break;
            default: break;
        }
    }

    void envStep (Env& v)
    {
        v.readSample = v.counter;
        if (v.periodPending >= 0)
        {
            v.period = static_cast<u32> (v.periodPending);
            v.periodPending = -1;
        }
        if (v.phasePipe > 0)
        {
            --v.phasePipe;
            const int p = v.phasePipe;
            if (v.target == kAttack)
            {
                if (p == 1)
                    v.cmpPos = kMatch[v.D];
                if (p == 0)
                {
                    v.phase = kAttack;
                    v.cmpPos = kMatch[v.A];
                    v.frozen = false;
                }
            }
            else if (v.target == kDecay)
            {
                if (p == 0)
                {
                    v.phase = kDecay;
                    v.cmpPos = kMatch[v.D];
                }
            }
            else
            {
                if ((v.phase == kAttack && p == 0) || (v.phase == kDecay && p == 1))
                {
                    v.phase = kRelease;
                    v.cmpPos = kMatch[v.R];
                }
            }
        }
        bool used = false;
        if (v.stepPipe > 0)
        {
            --v.stepPipe;
            if (v.stepPipe == 0)
            {
                used = true;
                if (! v.frozen)
                {
                    if (v.phase == kAttack)
                    {
                        v.counter = static_cast<u8> (v.counter + 1);
                        if (v.counter == 0xff)
                        {
                            v.target = kDecay;
                            v.phasePipe = 3;
                        }
                    }
                    else
                    {
                        v.counter = static_cast<u8> (v.counter - 1); // 8 bits: 0 wraps to 0xff
                        if (v.counter == 0)
                            v.frozen = true;
                    }
                    switch (v.counter)
                    {
                        case 0xff:
                        case 0x00: v.periodPending = 1; break;
                        case 0x5d: v.periodPending = 2; break;
                        case 0x36: v.periodPending = 4; break;
                        case 0x1a: v.periodPending = 8; break;
                        case 0x0e: v.periodPending = 16; break;
                        case 0x06: v.periodPending = 30; break;
                        default: break;
                    }
                }
            }
        }
        if (! used && v.divPipe > 0)
        {
            --v.divPipe;
            if (v.divPipe == 0)
            {
                used = true;
                v.divCnt = 0;
                if ((v.phase == kDecay && v.counter != v.S) || v.phase == kRelease)
                    v.stepPipe = 1;
            }
        }
        if (! used && v.pending)
        {
            v.pos = 0;
            v.pending = false;
            if (v.phase == kAttack)
            {
                v.divCnt = 0;
                v.stepPipe = 2;
            }
            else if (! v.frozen)
            {
                v.divCnt = (v.divCnt + 1) & 0x7fu;
                if (v.divCnt == v.period)
                    v.divPipe = (v.period == 1) ? 1 : 2;
            }
        }
        if (v.pos != v.cmpPos)
            v.pos = (v.pos + 1) % 32767u;
        else
            v.pending = true;
    }

    /** One step (8.15.2). */
    void step()
    {
        // (a) oscillator advance
        for (auto& x : o)
        {
            if ((x.ctrl & 8) != 0)
            {
                if (x.fill != 0)
                {
                    if (--x.fill == 0)
                    {
                        x.reg = (x.reg | (x.reg << 1) | 1u) & 0x7fffffu;
                        x.latch = x.reg;
                        x.noiseOut = noiseOutOf (x.reg);
                        if (x.reg != 0x7fffffu)
                            x.fill = fillInt;
                    }
                }
                x.firstShift = true;
                x.rise = false; // our chip clears it (8.15.2 (a), 8.15.6)
            }
            else
            {
                const u32 a0 = x.acc;
                x.acc = (x.acc + x.F) & 0xffffffu;
                x.rise = (a0 & 0x800000u) == 0 && (x.acc & 0x800000u) != 0;
                if ((a0 & 0x80000u) == 0 && (x.acc & 0x80000u) != 0)
                    x.noisePipe = 2;
                else if (x.noisePipe > 0)
                {
                    --x.noisePipe;
                    if (x.noisePipe == 1)
                    {
                        x.latch = x.reg;
                        x.firstShift = false;
                    }
                    else if (x.noisePipe == 0)
                    {
                        const auto w = static_cast<u8> (x.ctrl >> 4);
                        shift (x, w, w);
                    }
                }
            }
        }
        // (b) envelopes
        for (auto& v : e)
            envStep (v);
        // (c) outputs 1, 2, 3; ring sources 3, 1, 2
        static constexpr int src[3] = { 2, 0, 1 };
        for (int n = 0; n < 3; ++n)
        {
            Osc& x = o[n];
            const Osc& s = o[src[n]];
            const auto w = static_cast<u8> (x.ctrl >> 4);
            const bool test = (x.ctrl & 8) != 0;
            if (w != 0)
            {
                u32 idx = x.acc >> 12;
                if ((x.ctrl & 4) != 0 && (w & 2) == 0)
                {
                    const u32 srcTop = faults.ringWithoutNot ? (s.acc >> 23) : (~s.acc >> 23);
                    const u32 b = ((x.acc >> 23) ^ srcTop) & 1u;
                    idx = (idx & 0x7ffu) | (b << 11);
                }
                const auto tri = static_cast<u16> ((((idx < 0x800u) ? idx : (idx ^ 0xfffu)) << 1) & 0xfffu);
                const auto saw = static_cast<u16> (idx);
                const int nsh = ((w & 1) != 0 ? 1 : 0) + ((w & 2) != 0 ? 1 : 0) + ((w & 4) != 0 ? 1 : 0);
                u16 shape;
                if (nsh >= 2)
                {
                    shape = 0xfff; // placeholder for the combined table (see the header)
                    if ((w & 1) != 0)
                        shape &= tri;
                    if ((w & 2) != 0)
                        shape &= saw;
                }
                else if ((w & 1) != 0)
                    shape = tri;
                else if ((w & 2) != 0)
                    shape = saw;
                else
                    shape = 0xfff;
                const u16 pm = (w & 4) != 0 ? ((x.cmp || test) ? u16 { 0xfff } : u16 { 0 }) : u16 { 0xfff };
                const u16 nm = (w & 8) != 0 ? x.noiseOut : u16 { 0xfff };
                x.out = static_cast<u16> (shape & pm & nm);
                if (modelA && (w & 2) != 0 && (x.out & 0x800u) == 0)
                {
                    x.acc &= ~0x800000u;
                    x.rise = false;
                }
                if (modelA)
                    x.rd = x.out;
                else if ((w & 3) != 0)
                {
                    x.rd = static_cast<u16> (x.btLatch & pm & nm);
                    x.btLatch = shape;
                }
                else
                    x.rd = x.out;
                if ((w & 8) != 0 && (w & 7) != 0)
                {
                    if (x.noisePipe != 1 && ! test)
                    {
                        x.reg &= ~(tapMask() & ~spreadOut (x.out));
                        x.noiseOut = static_cast<u16> (x.noiseOut & x.out);
                    }
                    else
                        x.noiseOut = x.out;
                }
            }
            else if (x.fade != 0)
            {
                if (--x.fade == 0)
                {
                    x.out = static_cast<u16> (x.out & (x.out >> 1));
                    x.rd = x.out;
                    if (x.out != 0)
                        x.fade = fadeInt;
                }
            }
            x.cmp = (x.acc >> 12) >= x.PW;
        }
        // (d) sync; targets 2, 3, 1
        static constexpr int tgt[3] = { 1, 2, 0 };
        for (int n = 0; n < 3; ++n)
        {
            const Osc& x = o[n];
            if (x.rise && (o[tgt[n]].ctrl & 2) != 0 && ! ((x.ctrl & 2) != 0 && o[src[n]].rise))
                o[tgt[n]].acc = 0;
        }
    }

    /** The 20 fields of a .bsref state line (8.9.3, 8.15.1), in its order. */
    void fields (u32 f[20]) const
    {
        for (int i = 0; i < 3; ++i)
        {
            f[i] = o[i].acc;
            f[3 + i] = o[i].reg;
            f[6 + i] = e[i].counter;
            f[9 + i] = static_cast<u32> (e[i].phase);
            f[12 + i] = e[i].pos;
            f[15 + i] = e[i].divCnt | (e[i].pending ? 0x80u : 0u);
        }
        f[18] = static_cast<u32> (o[2].rd >> 4);
        f[19] = e[2].readSample;
    }
};
} // namespace vs::bside::independent
