#pragma once

// The measurement traces of our combined-waveform fit (source/bside/chip/CombinedWaveforms.h) and the
// extraction of shape samples from their references. JUCE-free, deterministic, header-only; used by the
// fit tool (tests/bside/tools/ShapeFit.cpp) and by bside_trace (tests/bside/trace/ShapeDataTests.cpp).
//
// Every trace drives oscillator 3 and reads it back through the read register 0x1B (rd1B = output bits
// 11..4: bits 3..0 are never observable through the trace interface). Four kinds:
//   sweep  frequency 0x1000 (the index advances by exactly 1 per cycle) from a test-bit reset, the
//          combination selected throughout, a checkpoint every cycle. Model A reads the output of the same
//          step; Model B the shape of the previous step (its read latch, 8.15.2 (c) step 5). Model A with
//          the saw only up to index 0x7ff: above it the top-bit pull-down (step 4) sends the accumulator
//          back to the lower half.
//   spot   one index per sample, for the Model A saw combinations' upper half and the pulse-width checks:
//          oscillator 2 resets oscillator 3 by sync every 4096 cycles (no write), oscillator 3 climbs with
//          the triangle alone, the combination is selected for the one step that reaches the index (two
//          steps on Model B, for its latch) and deselected again; one sample every second period.
//   noise  noise with other waveforms: the noise register is refilled under the test bit, oscillator 3
//          climbs with noise alone at a random frequency for a random number of steps (so the index and
//          the noise output are not correlated), then the combination is selected for one step (two on
//          Model B with triangle or saw). Samples whose step meets a noise shift are not planned.
// Each sample names the checkpoint whose accumulator gives the index and whose noise register gives the
// noise mask, the checkpoint whose rd1B is read, and the pulse width. extract() checks the design held
// (the accumulator where the plan expects it) before it trusts a sample.

#include "bside/chip/ChipTypes.h"
#include "bside/chip/Waveforms.h"
#include "bside/trace/ReferenceFormat.h"
#include "bside/trace/TraceFormat.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace vs::bside::shapes
{
enum class Kind : std::uint8_t { sweep, spot, noise };

struct PlanSpec
{
    std::string name;     // file stem in tests/bside/shapes/
    chip::Model model = chip::Model::A;
    std::uint8_t wave = 0; // waveform bits 0..15
    Kind kind = Kind::sweep;
    std::uint16_t pulseWidth = 0;
    std::vector<std::uint16_t> indices; // spot: the indices to sample
    int samples = 0;       // noise: samples to plan
    std::uint64_t seed = 0;
};

struct PlannedSample
{
    std::size_t accWrite = 0, readWrite = 0; // spot/noise: after-write checkpoints by write index
    std::uint64_t accCycle = 0, readCycle = 0; // sweep: periodic checkpoints by cycle
    std::uint32_t increment = 0x1000;          // F during the observed step(s)
    int steps = 1;                             // index = ((acc + steps x F) >> 12) & 0xfff
    std::uint32_t expectedAcc = 0;             // spot/noise: the accumulator the plan expects at accWrite
    bool checkAcc = false;
};

struct Plan
{
    std::string text;
    std::vector<PlannedSample> samples;
};

/** One observed shape value: rd1B = (shape(index) AND pulse mask AND noise mask) >> 4. */
struct Sample
{
    chip::Model model = chip::Model::A;
    std::uint8_t wave = 0;
    std::uint16_t index = 0;
    std::uint16_t pulseWidth = 0;
    bool pulseHigh = true;       // the pulse mask of the observed value
    std::uint8_t noiseMask = 0xff; // noise output bits 11..4 (0xff without noise)
    std::uint8_t value = 0;      // rd1B
    /** Bits 11..4 this sample determines (the noise mask hides the others). With the pulse low the whole
        value is known to be 0 (the mask of step 3), which checks the contract's structure. */
    std::uint8_t observed() const noexcept { return noiseMask; }
};

namespace detail
{
inline std::uint64_t splitmix (std::uint64_t& s) noexcept
{
    std::uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

inline std::string hex2 (unsigned v)
{
    char b[4];
    std::snprintf (b, sizeof b, "%02x", v & 0xffu);
    return b;
}

struct Writer
{
    std::string text;
    std::size_t writes = 0;
    std::size_t write (std::uint64_t cycle, unsigned reg, unsigned value)
    {
        text += std::to_string (cycle) + " " + hex2 (reg) + " " + hex2 (value) + "\n";
        return writes++;
    }
};

inline const char* modelName (chip::Model m) noexcept { return m == chip::Model::A ? "A" : "B"; }

/** True when the accumulator's bit 19 rises in step s (s >= 1) of a climb acc(s) = s x F from 0. */
inline bool bit19Rises (std::uint64_t s, std::uint32_t f) noexcept
{
    if (s == 0)
        return false;
    const std::uint64_t a0 = ((s - 1) * f) & 0xffffff, a1 = (s * f) & 0xffffff;
    return (a0 & 0x80000) == 0 && (a1 & 0x80000) != 0;
}
} // namespace detail

constexpr std::uint64_t kRelease = 40; // test-bit release of a sweep

inline bool sawPullsDown (chip::Model m, std::uint8_t wave) noexcept { return m == chip::Model::A && (wave & chip::wave::kSaw) != 0; }

inline Plan buildPlan (const PlanSpec& s)
{
    using detail::hex2;
    Plan p;
    detail::Writer w;
    const unsigned ctl = static_cast<unsigned> (s.wave) << 4;
    std::string head = "bside-trace 1\n";
    head += "# Combined-waveform shape measurement (tests/bside/shapes/ShapePlan.h, kind ";
    head += s.kind == Kind::sweep ? "sweep" : s.kind == Kind::spot ? "spot" : "noise";
    head += "): waveform bits " + hex2 (s.wave) + ", pulse width " + std::to_string (s.pulseWidth) + ".\n";
    head += "# Oscillator 3 is read back through the read register; generated, never edited by hand.\n";
    head += "clock 985248\nmodel ";
    head += detail::modelName (s.model);
    head += "\n";
    std::uint64_t end = 0;
    if (s.kind == Kind::sweep)
    {
        head += "check 1\n";
        w.write (8, 0x12, 0x08);
        w.write (16, 0x0e, 0x00);
        w.write (16, 0x0f, 0x10);
        w.write (16, 0x10, s.pulseWidth & 0xffu);
        w.write (16, 0x11, s.pulseWidth >> 8);
        w.write (kRelease, 0x12, ctl);
        const bool half = sawPullsDown (s.model, s.wave);
        const std::uint64_t count = half ? 0x7ff : 4096; // indices 1..count (4096 = index 0 after the wrap)
        const bool latch = s.model == chip::Model::B;
        for (std::uint64_t k = 1; k <= count; ++k)
        {
            PlannedSample o;
            // Model A: the read of step release + k shows index k; Model B: the read of the next step.
            o.accCycle = kRelease + k - (latch ? 0 : 1);
            o.readCycle = kRelease + k + (latch ? 1 : 0);
            o.steps = latch ? 0 : 1;
            p.samples.push_back (o);
        }
        end = kRelease + count + 2;
    }
    else if (s.kind == Kind::spot)
    {
        // Oscillators 2 and 3 held by the test bit, released together: oscillator 2's top bit then rises
        // at step 0x800 and every 4096 steps, and resets oscillator 3 (sync bit set) after its output.
        w.write (8, 0x0b, 0x08);
        w.write (8, 0x12, 0x08);
        w.write (16, 0x07, 0x00);
        w.write (16, 0x08, 0x10);
        w.write (16, 0x0e, 0x00);
        w.write (16, 0x0f, 0x10);
        w.write (16, 0x10, s.pulseWidth & 0xffu);
        w.write (16, 0x11, s.pulseWidth >> 8);
        w.write (kRelease, 0x0b, 0x00);
        w.write (kRelease, 0x12, 0x12);
        const std::uint64_t firstReset = kRelease + 0x800;
        const bool latch = s.model == chip::Model::B;
        std::uint64_t period = 1;
        for (const auto x : s.indices)
        {
            const std::uint64_t reset = firstReset + 4096 * period; // oscillator 3's index is 0 after this step
            const std::uint64_t on = reset + x - 1;
            PlannedSample o;
            o.accWrite = w.write (on, 0x12, ctl | 0x02);
            o.readWrite = w.write (on + (latch ? 2 : 1), 0x12, 0x12);
            o.expectedAcc = static_cast<std::uint32_t> (((x - 1u) & 0xfffu) << 12);
            o.checkAcc = true;
            p.samples.push_back (o);
            period += 2;
        }
        end = firstReset + 4096 * period;
    }
    else
    {
        const auto timing = chip::timingFor (s.model);
        const std::uint64_t hold = timing.testHold + 3ull * timing.fillInterval + 1;
        const bool latch = s.model == chip::Model::B && (s.wave & (chip::wave::kTri | chip::wave::kSaw)) != 0;
        w.write (8, 0x12, 0x88);
        std::uint64_t c = 16 + hold;
        std::uint64_t rng = s.seed;
        int planned = 0;
        while (planned < s.samples)
        {
            const auto m = static_cast<std::uint32_t> (1 + detail::splitmix (rng) % 15);
            const auto k = 2 + detail::splitmix (rng) % 1200;
            const std::uint32_t f = m << 12;
            if (detail::bit19Rises (k - 2, f) || detail::bit19Rises (k - 1, f) || (latch && detail::bit19Rises (k, f)))
                continue; // a noise shift inside the observed step(s)
            w.write (c, 0x0f, m << 4);
            w.write (c, 0x12, 0x80);
            PlannedSample o;
            o.accWrite = w.write (c + k - 1, 0x12, ctl);
            o.readWrite = w.write (c + k - 1 + (latch ? 2 : 1), 0x12, 0x88);
            o.increment = f;
            o.expectedAcc = static_cast<std::uint32_t> (((k - 1) * f) & 0xffffff);
            o.checkAcc = true;
            p.samples.push_back (o);
            c = c + k - 1 + (latch ? 2 : 1) + hold;
            ++planned;
        }
        end = c + 8;
    }
    p.text = head + w.text + "end " + std::to_string (end) + "\n";
    return p;
}

/** The pulse widths and indices of the pulse-width checks: around the edge and spread over the table. */
inline std::vector<std::uint16_t> pulseWidthIndices (std::uint16_t pw)
{
    std::vector<std::uint16_t> v;
    for (int d = -16; d < 16; ++d)
        v.push_back (static_cast<std::uint16_t> ((pw + d) & 0xfff));
    for (int j = 0; j < 32; ++j)
        v.push_back (static_cast<std::uint16_t> ((j * 128 + 37) & 0xfff));
    for (auto& x : v)
        if (x == 0)
            x = 1;
    return v;
}

/** Every measurement trace (names: s_<kind>_<waveform bits>_<model>[_<pulse width hex>]). */
inline std::vector<PlanSpec> allPlans()
{
    std::vector<PlanSpec> v;
    for (const auto m : { chip::Model::A, chip::Model::B })
    {
        const std::string mn = m == chip::Model::A ? "a" : "b";
        for (const std::uint8_t w : chip::combined::kNetworkCombinations)
        {
            PlanSpec s;
            s.model = m;
            s.wave = w;
            s.kind = Kind::sweep;
            s.name = "s_sweep_" + detail::hex2 (w).substr (1) + "_" + mn;
            v.push_back (s);
            if (sawPullsDown (m, w))
            {
                PlanSpec u = s;
                u.kind = Kind::spot;
                u.name = "s_spot_" + detail::hex2 (w).substr (1) + "_" + mn;
                for (std::uint16_t x = 0x800; x < 0x1000; ++x)
                    u.indices.push_back (x);
                u.indices.push_back (0); // the sweep stops below 0x800 and so never wraps to 0
                v.push_back (u);
            }
            if ((w & chip::wave::kPulse) != 0)
                for (const std::uint16_t pw : { std::uint16_t { 0x400 }, std::uint16_t { 0xa00 } })
                {
                    PlanSpec u = s;
                    u.kind = Kind::spot;
                    u.pulseWidth = pw;
                    u.indices = pulseWidthIndices (pw);
                    u.name = "s_pw_" + detail::hex2 (w).substr (1) + "_" + mn + "_" + detail::hex2 (pw >> 4);
                    v.push_back (u);
                }
        }
        for (const std::uint8_t w : chip::combined::kNoiseCombinations)
        {
            PlanSpec s;
            s.model = m;
            s.wave = w;
            s.kind = Kind::noise;
            s.samples = m == chip::Model::A ? 96 : 48;
            s.seed = 0x5eed0000u + w * 2u + (m == chip::Model::A ? 0u : 1u);
            s.name = "s_noise_" + detail::hex2 (w).substr (1) + "_" + mn;
            v.push_back (s);
        }
    }
    return v;
}

/** The samples of one plan from its reference. Returns false (with `why`) when the reference does not
    fit the plan (schedule, or an accumulator where the design puts another value). */
inline bool extract (const PlanSpec& spec, const Plan& plan, const trace::Trace& t, const trace::Reference& r, std::vector<Sample>& out,
                     std::string& why)
{
    std::vector<trace::Checkpoint> schedule;
    if (! trace::checkpointSchedule (t, schedule) || schedule.size() != r.points.size())
    {
        why = "the reference does not follow the trace's checkpoint schedule";
        return false;
    }
    std::vector<std::size_t> byWrite (t.writes.size(), 0);
    std::vector<std::size_t> byCycle; // periodic checkpoints (check 1: index = cycle - 1)
    for (std::size_t i = 0; i < schedule.size(); ++i)
    {
        if (schedule[i].kind == trace::Checkpoint::Kind::afterWrite)
            byWrite[schedule[i].write] = i;
        else if (schedule[i].kind == trace::Checkpoint::Kind::periodic)
        {
            if (byCycle.size() + 1 != schedule[i].cycle)
            {
                why = "unexpected periodic checkpoint";
                return false;
            }
            byCycle.push_back (i);
        }
    }
    constexpr std::size_t kAcc3 = 2, kNoise3 = 5, kRead = 18;
    for (const auto& o : plan.samples)
    {
        std::size_t a = 0, rd = 0;
        if (spec.kind == Kind::sweep)
        {
            if (o.accCycle == 0 || o.accCycle > byCycle.size() || o.readCycle == 0 || o.readCycle > byCycle.size())
            {
                why = "a sweep sample outside the checkpoints";
                return false;
            }
            a = byCycle[o.accCycle - 1];
            rd = byCycle[o.readCycle - 1];
        }
        else
        {
            a = byWrite[o.accWrite];
            rd = byWrite[o.readWrite];
        }
        // At the release cycle of a sweep the periodic checkpoint precedes the release write: the state
        // after it (accumulator 0) is the one the next step continues from.
        std::uint32_t acc = r.points[a].state.v[kAcc3];
        if (spec.kind == Kind::sweep && o.accCycle == kRelease)
            acc = 0;
        if (o.checkAcc && acc != o.expectedAcc)
        {
            why = "accumulator " + std::to_string (acc) + " where the plan expects " + std::to_string (o.expectedAcc);
            return false;
        }
        Sample smp;
        smp.model = spec.model;
        smp.wave = spec.wave;
        smp.pulseWidth = spec.pulseWidth;
        smp.index = static_cast<std::uint16_t> (((acc + static_cast<std::uint32_t> (o.steps) * o.increment) >> 12) & 0xfffu);
        if ((spec.wave & chip::wave::kPulse) != 0)
        {
            // Model A compares the previous step's accumulator (the state at `a`); Model B's read shows the
            // latched shape with the comparison made in the step that latched it (the sampled index).
            const std::uint32_t cmpIndex = spec.model == chip::Model::A ? (acc >> 12) & 0xfffu : smp.index;
            smp.pulseHigh = cmpIndex >= spec.pulseWidth;
        }
        if ((spec.wave & chip::wave::kNoise) != 0)
            smp.noiseMask = static_cast<std::uint8_t> (chip::wave::noise (r.points[a].state.v[kNoise3]) >> 4);
        smp.value = static_cast<std::uint8_t> (r.points[rd].state.v[kRead]);
        out.push_back (smp);
    }
    return true;
}
} // namespace vs::bside::shapes
