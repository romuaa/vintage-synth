// Band-limited rendering (docs/architecture.md 8.4.5): alias budgets, digital state untouched by rendering,
// block and rate independence, sub-sample timing and latency.

#include "FuzzSupport.h"
#include "RenderTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

using namespace bs_render_test;
using vs::bside::chip::DigitalState;
namespace ctrl = vs::bside::chip::ctrl;

namespace
{
constexpr int kOrder = 15;
constexpr int kN = 1 << kOrder;
constexpr int kWarmup = 4096;

struct Row
{
    const char* name;
    Model model;
    std::uint8_t control;  // oscillator 2's control (gate set by the caller)
    float pulseWidth;
    double ratio;          // oscillator 2's frequency / oscillator 1's (sync, ring); 0: oscillator 2 alone
    bool chain = false;    // a three-oscillator sync chain: 1 -> 2 (silent, 1.37 x) -> 3 (plays, ratio x)
};

/** Oscillator 2 (index 1) plays the row; oscillator 1 (index 0) is its sync / ring source, silent. `hz` is
    the highest oscillator frequency (8.4.5: up to the register maximum). Returns the worst alias relative
    to the fundamental; for sync and ring relative to the strongest legitimate component, as Vintage's sync
    gate does (tests/dsp_unit/SyncCrossModTests.cpp), since their fundamental can be weak. Sync ratios are
    Vintage's (target / source; above 1 the target runs at hz). */
AliasResult measureRow (const Row& r, double hz, const RateConfig& rc)
{
    Chip c = makeChip (r.model, rc.internalRate);
    const bool sync = (r.control & ctrl::sync) != 0 || r.chain;
    const bool ring = (r.control & ctrl::ring) != 0;
    int fund = oddBin (hz, rc.internalRate, kN);
    int base = fund;
    double f2 = binFrequency (c, fund, kN);
    double f1 = 0.0;
    if (sync)
    {
        // `hz` is the highest oscillator frequency (8.4.5: up to the register maximum): the target runs at
        // it, the source (which sets the period, the fundamental) at hz / ratio
        fund = oddBin (r.ratio >= 1.0 ? hz / r.ratio : hz, rc.internalRate, kN);
        base = fund;
        f1 = binFrequency (c, fund, kN);
        f2 = f1 * r.ratio;
    }
    else if (ring)
    {
        // both on multiples of a common odd base bin: everything legitimate lies on its multiples; the
        // target (5 x base) runs at about hz
        base = std::max (1, oddBin (hz / 5.0, rc.internalRate, kN));
        fund = base * 5;
        f2 = binFrequency (c, fund, kN);
        f1 = binFrequency (c, base * 3, kN);
    }
    setOsc (c, 0, f1, 2048.0f, 0x00); // no waveform, gate off: silent
    if (r.chain)
    {
        setOsc (c, 1, f1 * 1.37, 2048.0f, ctrl::sync); // synced by 1, silent, syncs 3
        setOsc (c, 2, f2, r.pulseWidth, static_cast<std::uint8_t> (r.control | ctrl::sync | ctrl::gate));
    }
    else
        setOsc (c, 1, f2, r.pulseWidth, static_cast<std::uint8_t> (r.control | ctrl::gate));
    // settle for at least three periods of the slowest line (the first resets of a chain come late)
    const double slowest = std::max (1.0, static_cast<double> (std::min (fund, base)));
    renderSamples (c, std::max (kWarmup, static_cast<int> (3.0 * kN / slowest)));
    const auto y = renderSamples (c, kN);
    // combined waveforms: on Model A a saw combination pulls its top bit down and may play at 2 f0, so the
    // reference is the strongest legitimate component there too
    const bool combined = vs::bside::chip::wave::isCombined (vs::bside::chip::wave::bits (r.control));
    auto a = aliasOf (y, rc.internalRate, rc.band, ring || sync || combined ? -1 : fund, base);
    a.corrections = c.renderCorrections();
    return a;
}

std::string fmt (double v)
{
    std::ostringstream s;
    s.setf (std::ios::fixed);
    s.precision (1);
    s << v;
    return s.str();
}
} // namespace

namespace
{
/** Runs rows against a budget that depends on the frequency (8.4.5); reports the worst per row. */
void runBudget (const char* title, const std::vector<Row>& rows, double budget2k, double budget385)
{
    std::vector<double> rowWorst (rows.size(), -300.0);
    for (const auto& rc : rateConfigs())
        for (std::size_t i = 0; i < rows.size(); ++i)
            for (const double hz : { 110.0, 1000.0, 2000.0, 3850.0 })
            {
                const auto a = measureRow (rows[i], hz, rc);
                const double budget = hz <= 2000.0 ? budget2k : budget385;
                INFO (title << ", " << rc.internalRate << " Hz internal, " << rows[i].name << " (width " << rows[i].pulseWidth << ", ratio " << rows[i].ratio << ") @ " << hz << " Hz: worst " << a.worstDb
                            << " dB at " << a.worstHz << " Hz (budget " << budget << "; reference component " << a.fundamentalDb << " dB re full scale)");
                CHECK (a.worstDb <= budget);
                CHECK (a.worstDb <= budget - 3.0); // at least 3 dB inside (coordinator, S2b-2 review)
                CHECK (a.corrections == 0);
                rowWorst[i] = std::max (rowWorst[i], a.worstDb - budget);
            }
    for (std::size_t i = 0; i < rows.size(); ++i)
        WARN (title << " " << rows[i].name << " (width " << rows[i].pulseWidth << ", ratio " << rows[i].ratio << "): worst margin to the budget over all rates and frequencies " << fmt (rowWorst[i]) << " dB");
}

std::vector<Row> basicRows (Model m)
{
    std::vector<Row> r {
        { "triangle", m, ctrl::triangle, 2048.0f, 0.0 },
        { "saw", m, ctrl::saw, 2048.0f, 0.0 },
        { "triangle ring", m, ctrl::triangle | ctrl::ring, 2048.0f, 0.0 },
    };
    for (const float w : { 1.0f, 64.0f, 200.0f, 512.0f, 2048.0f, 3072.0f, 4032.0f, 4095.0f })
        r.push_back ({ "pulse", m, ctrl::pulse, w, 0.0 });
    for (const double ratio : { 2.71, 1.9969 })
    {
        r.push_back ({ "saw chain", m, ctrl::saw, 2048.0f, ratio, true });
        r.push_back ({ "triangle chain", m, ctrl::triangle, 2048.0f, ratio, true });
        r.push_back ({ "pulse chain", m, ctrl::pulse, 1300.0f, ratio, true });
    }
    for (const double ratio : { 1.37, 2.71, 0.71, 1.0031, 1.9969 })
    {
        r.push_back ({ "saw sync", m, ctrl::saw | ctrl::sync, 2048.0f, ratio });
        r.push_back ({ "triangle sync", m, ctrl::triangle | ctrl::sync, 2048.0f, ratio });
        r.push_back ({ "pulse sync", m, ctrl::pulse | ctrl::sync, 1300.0f, ratio });
    }
    return r;
}

std::vector<Row> combinedRows (Model m)
{
    std::vector<Row> r { { "triangle+saw", m, ctrl::triangle | ctrl::saw, 2048.0f, 0.0 } };
    for (const float w : { 64.0f, 1024.0f, 2048.0f, 3072.0f })
    {
        r.push_back ({ "triangle+pulse", m, ctrl::triangle | ctrl::pulse, w, 0.0 });
        r.push_back ({ "saw+pulse", m, ctrl::saw | ctrl::pulse, w, 0.0 });
        r.push_back ({ "triangle+saw+pulse", m, ctrl::triangle | ctrl::saw | ctrl::pulse, w, 0.0 });
        r.push_back ({ "triangle+pulse ring", m, ctrl::triangle | ctrl::pulse | ctrl::ring, w, 0.0 });
    }
    r.push_back ({ "triangle+saw chain", m, ctrl::triangle | ctrl::saw, 2048.0f, 2.71, true });
    r.push_back ({ "saw+pulse chain", m, ctrl::saw | ctrl::pulse, 1024.0f, 2.71, true });
    // combined targets of sync
    for (const double ratio : { 1.37, 2.71, 0.71 })
    {
        r.push_back ({ "triangle+saw sync", m, ctrl::triangle | ctrl::saw | ctrl::sync, 2048.0f, ratio });
        r.push_back ({ "triangle+pulse sync", m, ctrl::triangle | ctrl::pulse | ctrl::sync, 2048.0f, ratio });
        r.push_back ({ "saw+pulse sync", m, ctrl::saw | ctrl::pulse | ctrl::sync, 1024.0f, ratio });
        r.push_back ({ "triangle+saw+pulse sync", m, ctrl::triangle | ctrl::saw | ctrl::pulse | ctrl::sync, 1024.0f, ratio });
    }
    return r;
}

} // namespace

TEST_CASE ("render: alias budget, triangle / saw / pulse incl. sync, ring and chains, Model B converter (<= -100 dB to 3.85 kHz)", "[render][alias]")
{
    runBudget ("Model B", basicRows (Model::B), -100.0, -100.0);
}

TEST_CASE ("render: alias budget, Model A converter (<= -96 dB to 2 kHz, <= -80 dB to 3.85 kHz)", "[render][alias]")
{
    runBudget ("Model A", basicRows (Model::A), -96.0, -80.0);
}

TEST_CASE ("render: alias budget, combined waveforms, both models (<= -96 dB to 2 kHz, <= -80 dB to 3.85 kHz)", "[render][alias]")
{
    // Model A's combined waveforms use the long kernel (docs/decisions.md S2b-2); with the short one they
    // missed the budget by up to 10 dB (-85.9 dB)
    runBudget ("Model B combined", combinedRows (Model::B), -96.0, -80.0);
    runBudget ("Model A combined", combinedRows (Model::A), -96.0, -80.0);
}

// Diagnostic (hidden): the stop band of both kernels, computed from the kernels themselves (kernelAt): a
// band-limited step leaks content from above half the internal rate with at least this attenuation, and a
// shape whose spectrum is flat far above the band (single cells of a combined table) aliases at about that
// level relative to its own harmonics.
namespace
{
template <class Table>
void reportStopBand (const char* name)
{
    const auto& b = Table::get();
    constexpr int z = Table::kZeroCrossings;
    constexpr int per = 256;
    double dc = 0.0;
    for (int i = -z * per; i <= z * per; ++i)
        dc += static_cast<double> (b.kernelAt (static_cast<float> (i) / per)) / per;
    double worst = -300.0, worstF = 0.0;
    // content that folds into 0 .. 20 kHz at 96 kHz internal lies at >= 1 - 20/96 of the internal rate
    for (double f = 1.0 - 20.0 / 96.0; f <= 2.0; f += 0.0025)
    {
        double re = 0.0;
        for (int i = -z * per; i <= z * per; ++i)
            re += static_cast<double> (b.kernelAt (static_cast<float> (i) / per)) / per * std::cos (2.0 * 3.14159265358979323846 * f * i / per);
        const double db = 20.0 * std::log10 (std::abs (re) / dc + 1e-300);
        if (db > worst)
        {
            worst = db;
            worstF = f;
        }
    }
    WARN (name << " (" << z << " zero crossings, Kaiser beta " << Table::kKaiserBeta << "): from 0.79 fs, worst " << worst << " dB at " << worstF << " fs");
}
} // namespace

TEST_CASE ("render: diagnostic, the BLEP kernels' stop band", "[.render-diag]")
{
    reportStopBand<Chip::ShortBlep> ("short kernel");
    reportStopBand<Chip::LongBlep> ("long kernel");
}

namespace
{
constexpr std::uint64_t kTick = 19656; // the 50 Hz system's frame (8.4.2)

/** Write discontinuities (8.4.5): oscillator 2 at about hz, its control written at every tick (at exact
    cycles). `mode` 0: the waveform switches every tick (triangle, saw, pulse in turn); 1: a test-bit reset
    every tick (set at the tick, cleared 300 cycles later; saw). The signal repeats exactly every three ticks
    (mode 0, the frequency an exact multiple of 1 / 3 ticks) or every tick (mode 1). */
AliasResult measureWrites (Model m, int mode, double hz, const RateConfig& rc)
{
    Chip c = makeChip (m, rc.internalRate);
    const std::uint64_t period = mode == 0 ? 3 * kTick : kTick;
    const double periods = std::max (1.0, std::round (hz * static_cast<double> (period) / 985248.0));
    const double f = periods * 16777216.0 / static_cast<double> (period);
    setOsc (c, 1, f, 1300.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
    std::uint64_t index = 0;
    const std::uint8_t waves[3] { ctrl::triangle, ctrl::saw, ctrl::pulse };
    auto write = [&] (Chip& ch, std::uint64_t i) {
        if (mode == 0)
            ch.write (0x0b, static_cast<std::uint8_t> (waves[i % 3] | ctrl::gate));
        else
            ch.write (0x0b, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate | ((i % 2) == 0 ? ctrl::test : 0)));
    };
    const std::uint64_t first = 1000;
    const std::uint64_t step = mode == 0 ? kTick : 0;
    // the signal's own lines lie every lineHz; the analysis needs at least 20 bins between two of them
    // (the window's main lobe is +-7 bins)
    const double lineHz = 985248.0 / static_cast<double> (period);
    int n = 1 << 14;
    while (rc.internalRate / n > lineHz / 20.0)
        n <<= 1;
    // mode 1 alternates set (at the tick) and clear (300 cycles later): two writes per tick
    if (mode == 1)
    {
        std::vector<float> y;
        const int total = kWarmup + n;
        y.assign (static_cast<std::size_t> (total), 0.0f);
        int done = 0;
        auto at = [&] (std::uint64_t i) { return first + (i / 2) * kTick + ((i % 2) == 0 ? 0u : 300u); };
        while (done < total)
        {
            done += c.renderUntil (y.data() + done, total - done, at (index));
            if (c.cycle() == at (index))
                write (c, index++);
        }
        y.erase (y.begin(), y.begin() + kWarmup);
        auto r = offGridAlias (windowedMagnitudes (y), rc.internalRate, rc.band, lineHz, 8.0);
        r.corrections = c.renderCorrections();
        return r;
    }
    renderWithWrites (c, kWarmup, first, step, write, index);
    const auto y = renderWithWrites (c, n, first, step, write, index);
    auto r = offGridAlias (windowedMagnitudes (y), rc.internalRate, rc.band, lineHz, 8.0);
    r.corrections = c.renderCorrections();
    return r;
}
} // namespace

TEST_CASE ("render: alias budget, write discontinuities every tick (<= -90 dB to 2 kHz)", "[render][alias][writes]")
{
    for (const Model m : { Model::B, Model::A })
        for (const int mode : { 0, 1 })
        {
            double worst = -300.0;
            for (const auto& rc : rateConfigs())
                for (const double hz : { 110.0, 1000.0, 2000.0 })
                {
                    const auto a = measureWrites (m, mode, hz, rc);
                    INFO ((m == Model::A ? "Model A" : "Model B") << (mode == 0 ? ", waveform switch" : ", test-bit reset") << ", " << rc.internalRate
                                                                << " Hz internal, " << hz << " Hz: worst " << a.worstDb << " dB at " << a.worstHz << " Hz");
                    CHECK (a.worstDb <= -90.0);
                    CHECK (a.worstDb > -299.0); // something was measured: bins exist between the lines
                    CHECK (a.corrections == 0);
                    worst = std::max (worst, a.worstDb);
                }
            WARN ((m == Model::A ? "Model A" : "Model B") << (mode == 0 ? " waveform switch every tick" : " test-bit reset every tick")
                                                        << ": worst alias over all rates " << fmt (worst) << " dB");
        }
}

namespace
{
/** One change of a random write schedule: a register write, a program-engine oscillator input, or a
    restart (power-on keeping time). */
struct Change
{
    std::uint64_t cycle;
    int kind; // 0 write, 1 setOscillator, 2 powerOnKeepingTime
    std::uint8_t reg, value;
    int osc;
    OscInput in;
};

std::vector<Change> randomSchedule (bst::Rng& r, std::uint64_t cycles, int count)
{
    std::vector<Change> v;
    std::uint64_t at = 0;
    for (int i = 0; i < count; ++i)
    {
        at += static_cast<std::uint64_t> (r.chance (20) ? r.range (0, 3) : r.range (1, static_cast<int> (2 * cycles / static_cast<std::uint64_t> (count))));
        Change c {};
        c.cycle = at;
        const int pick = r.range (0, 99);
        if (pick < 80)
        {
            c.kind = 0;
            const int osc = r.range (0, 2);
            const int field = r.range (0, 6);
            c.reg = static_cast<std::uint8_t> (osc * 7 + field);
            c.value = static_cast<std::uint8_t> (r.below (256));
            if (field == 4) // control: mostly musical waveforms, sometimes anything
                c.value = static_cast<std::uint8_t> (r.chance (70) ? (r.below (8) << 4) | r.below (16) : r.below (256));
            if (field == 1)
                c.value = static_cast<std::uint8_t> (r.below (r.chance (50) ? 64 : 256)); // frequency high byte
        }
        else if (pick < 97)
        {
            c.kind = 1;
            c.osc = r.range (0, 2);
            c.in.frequency = static_cast<double> (r.below (60000)) + static_cast<double> (r.below (1000)) / 1000.0;
            c.in.pulseWidth = static_cast<float> (r.below (4096)) + 0.25f;
            c.in.control = static_cast<std::uint8_t> (r.below (256));
            c.in.attackDecay = static_cast<std::uint8_t> (r.below (256));
            c.in.sustainRelease = static_cast<std::uint8_t> (r.below (256));
        }
        else
            c.kind = 2;
        v.push_back (c);
    }
    return v;
}

void apply (Chip& chip, const Change& c)
{
    switch (c.kind)
    {
        case 0: chip.write (c.reg, c.value); break;
        case 1: chip.setOscillator (c.osc, c.in); break;
        default: chip.powerOnKeepingTime(); break;
    }
}

/** Renders the schedule with changes at their exact cycles, in chunks of at most `chunk` samples. */
std::vector<float> renderSchedule (Chip& chip, const std::vector<Change>& sched, std::uint64_t endCycle, int chunk, std::vector<DigitalState>* states = nullptr)
{
    std::vector<float> y;
    std::vector<float> buf (static_cast<std::size_t> (chunk), 0.0f);
    std::size_t next = 0;
    while (chip.cycle() < endCycle || next < sched.size())
    {
        const std::uint64_t target = next < sched.size() ? sched[next].cycle : endCycle;
        std::fill (buf.begin(), buf.end(), 0.0f);
        const int got = chip.renderUntil (buf.data(), chunk, target);
        y.insert (y.end(), buf.begin(), buf.begin() + got);
        if (chip.cycle() == target && next < sched.size())
        {
            apply (chip, sched[next++]);
            if (states != nullptr)
                states->push_back (chip.state());
        }
        else if (chip.cycle() >= endCycle && next >= sched.size())
            break;
    }
    return y;
}
} // namespace

TEST_CASE ("render: the digital state is exactly what stepping gives (random writes, both models, any split)", "[render][state]")
{
    for (const Model m : { Model::A, Model::B })
        for (std::uint64_t seed = 1; seed <= 12; ++seed)
        {
            bst::Rng r (seed * 7919 + (m == Model::A ? 1 : 2));
            const double rate = rateConfigs()[static_cast<std::size_t> (r.below (7))].internalRate;
            const std::uint64_t cycles = 400000;
            const auto sched = randomSchedule (r, cycles, 300);
            const std::uint64_t endCycle = sched.back().cycle + 5000;

            // rendered, with every change at its exact cycle and random chunks
            Chip a = makeChip (m, rate);
            std::vector<DigitalState> rendered;
            const int chunk = r.range (1, 700);
            const auto y = renderSchedule (a, sched, endCycle, chunk, &rendered);

            // digital only
            Chip b = makeChip (m, rate);
            std::vector<DigitalState> stepped;
            for (const auto& c : sched)
            {
                b.advanceCycles (c.cycle - b.cycle());
                apply (b, c);
                stepped.push_back (b.state());
            }
            INFO ((m == Model::A ? "Model A" : "Model B") << ", seed " << seed << ", rate " << rate << ", chunk " << chunk);
            REQUIRE (rendered.size() == stepped.size());
            for (std::size_t i = 0; i < rendered.size(); ++i)
            {
                INFO ("change " << i << " at cycle " << sched[i].cycle);
                REQUIRE (rendered[i] == stepped[i]);
            }
            b.advanceCycles (a.cycle() - b.cycle());
            CHECK (a.state() == b.state());
            bool finite = true;
            float peak = 0.0f;
            for (const float v : y)
            {
                finite = finite && std::isfinite (v);
                peak = std::max (peak, std::abs (v));
            }
            CHECK (finite);
            CHECK (peak < 4.0f); // three voices of at most +-0.5 each, plus band-limiting overshoot
        }
}

TEST_CASE ("render: whole-sample rendering with writes between calls keeps the digital state of stepping", "[render][state]")
{
    for (const Model m : { Model::A, Model::B })
        for (std::uint64_t seed = 1; seed <= 6; ++seed)
        {
            bst::Rng r (seed * 104729 + (m == Model::A ? 3 : 4));
            Chip a = makeChip (m, 96000.0);
            Chip b = makeChip (m, 96000.0);
            const auto sched = randomSchedule (r, 300000, 200);
            std::vector<float> buf (2048);
            for (const auto& c : sched)
            {
                // render whole samples (the write lands at a sample boundary, wherever that is)
                const int n = r.range (0, 300);
                a.render (buf.data(), n);
                b.advanceCycles (a.cycle() - b.cycle());
                apply (a, c);
                apply (b, c);
                REQUIRE (a.state() == b.state());
            }
        }
}

TEST_CASE ("render: audio and digital state do not depend on the block size; the state not on the internal rate", "[render][state]")
{
    for (const Model m : { Model::A, Model::B })
    {
        bst::Rng r (m == Model::A ? 11 : 12);
        const auto sched = randomSchedule (r, 300000, 250);
        const std::uint64_t endCycle = sched.back().cycle + 3000;
        std::vector<std::vector<float>> outs;
        for (const int chunk : { 1, 7, 61, 512 })
        {
            Chip c = makeChip (m, 192000.0);
            outs.push_back (renderSchedule (c, sched, endCycle, chunk));
        }
        for (std::size_t i = 1; i < outs.size(); ++i)
        {
            REQUIRE (outs[i].size() == outs[0].size());
            CHECK (outs[i] == outs[0]); // bit-identical
        }
        std::vector<DigitalState> ref;
        for (const auto& rc : rateConfigs())
        {
            Chip c = makeChip (m, rc.internalRate);
            std::vector<DigitalState> st;
            renderSchedule (c, sched, endCycle, 333, &st);
            if (ref.empty())
                ref = st;
            else
                CHECK (st == ref);
        }
    }
}

TEST_CASE ("render: no allocation while rendering", "[render][rt]")
{
    if (! bst::allocationsAreCounted())
        SKIP ("allocation counting is off in this build (sanitizers)");
    for (const Model m : { Model::A, Model::B })
    {
        bst::Rng r (m == Model::A ? 21 : 22);
        const auto sched = randomSchedule (r, 200000, 200);
        Chip c = makeChip (m, 96000.0); // prepare builds the tables (allowed to allocate)
        std::vector<float> buf (512, 0.0f);
        const auto before = bst::totalAllocations();
        std::size_t next = 0;
        while (next < sched.size())
        {
            c.renderUntil (buf.data(), 512, sched[next].cycle);
            if (c.cycle() == sched[next].cycle)
                apply (c, sched[next++]);
        }
        c.render (buf.data(), 512);
        CHECK (bst::totalAllocations() == before);
    }
}

TEST_CASE ("render: a write's discontinuity lands at its exact cycle; latency is kLatencySamples", "[render][timing]")
{
    // Oscillator 1 frozen (frequency 0, accumulator at its power-on value 0x555555): triangle, saw and pulse
    // are constants there, so each waveform switch is a pure step of known size at the write's cycle.
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 88200.0, 192000.0, 384000.0 })
        {
            Chip c = makeChip (m, rate);
            setOsc (c, 0, 0.0, 256.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
            std::vector<float> y;
            std::vector<std::pair<std::uint64_t, std::uint8_t>> writes;
            bst::Rng r (m == Model::A ? 5 : 6);
            std::uint64_t at = 30000; // the envelope is at 0xff long before
            const std::uint8_t waves[] { ctrl::triangle, ctrl::saw, ctrl::pulse, 0x30 };
            for (int i = 0; i < 24; ++i)
            {
                writes.push_back ({ at, static_cast<std::uint8_t> (waves[i % 4] | ctrl::gate) });
                at += static_cast<std::uint64_t> (r.range (400, 1500));
            }
            std::vector<float> buf (64);
            std::size_t next = 0;
            const std::uint64_t endCycle = at + 2000;
            while (c.cycle() < endCycle)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                const std::uint64_t target = next < writes.size() ? writes[next].first : endCycle;
                const int got = c.renderUntil (buf.data(), 64, target);
                y.insert (y.end(), buf.begin(), buf.begin() + got);
                if (next < writes.size() && c.cycle() == writes[next].first)
                {
                    c.write (0x04, writes[next].second);
                    ++next;
                }
            }
            c.render (buf.data(), 0);
            // the expected output: a sum of band-limited steps at cycle / cycles-per-sample + kLatencySamples
            // the step's shape: the short kernel, the long one where the waveform is (or was) a pulse or a
            // combined one (docs/decisions.md S2b-2)
            auto blepCum = [&] (double t, bool longKernel) {
                const double z = longKernel ? Chip::LongBlep::kZeroCrossings : Chip::ShortBlep::kZeroCrossings;
                if (t <= -z)
                    return 0.0;
                if (t > z)
                    return 1.0;
                const int j = static_cast<int> (std::ceil (t)) - 1;
                const double res = longKernel ? Chip::LongBlep::get().step (j, t - j)
                                              : static_cast<double> (Chip::ShortBlep::get().step (j, static_cast<float> (t - j)));
                return res + (j >= 0 ? 1.0 : 0.0);
            };
            auto isLong = [&] (std::size_t i) {
                auto onLong = [] (std::uint8_t control) {
                    const auto b = vs::bside::chip::wave::bits (control);
                    return b == vs::bside::chip::wave::kPulse || vs::bside::chip::wave::usesTable (b);
                };
                const std::uint8_t before = i == 0 ? static_cast<std::uint8_t> (ctrl::saw) : writes[i - 1].second;
                return onLong (before) || onLong (writes[i].second);
            };
            // The levels are read from the output itself, half way between two writes (the continuous
            // waveform at the frozen accumulator differs from the 12-bit step value by a fraction of a step).
            const double cps = cyclesPerSample (c);
            std::vector<double> levels;
            for (std::size_t i = 0; i <= writes.size(); ++i)
            {
                const double from = i == 0 ? 26000.0 : static_cast<double> (writes[i - 1].first);
                const double to = i < writes.size() ? static_cast<double> (writes[i].first) : static_cast<double> (endCycle);
                levels.push_back (static_cast<double> (y[static_cast<std::size_t> (0.5 * (from + to) / cps) + Chip::kLatencySamples]));
            }
            auto model = [&] (double shiftCycles) {
                double worst = 0.0;
                for (auto s = static_cast<std::size_t> (26000.0 / cps); s < y.size(); ++s)
                {
                    double v = levels[0];
                    for (std::size_t i = 0; i < writes.size(); ++i)
                    {
                        const double pos = (static_cast<double> (writes[i].first) + shiftCycles) / cps + Chip::kLatencySamples;
                        v += (levels[i + 1] - levels[i]) * blepCum (static_cast<double> (s) - pos, isLong (i));
                    }
                    worst = std::max (worst, std::abs (static_cast<double> (y[s]) - v));
                }
                return worst;
            };
            const double exact = model (0.0), early = model (-1.0), late = model (1.0);
            INFO ((m == Model::A ? "Model A" : "Model B") << ", " << rate << " Hz: max deviation " << exact << " (one cycle early " << early << ", late " << late << ")");
            CHECK (exact < 1.0e-5);
            CHECK (early > 100.0 * exact); // the comparison resolves one cycle
            CHECK (late > 100.0 * exact);
        }
}

// Report (hidden): the worst alias per budget row group and internal rate, as a table on stdout.
TEST_CASE ("render: alias report per budget row and internal rate", "[.render-report]")
{
    struct Group
    {
        const char* name;
        std::vector<Row> rows;
    };
    const Group groups[] {
        { "B tri/saw/pulse/sync/ring", basicRows (Model::B) },
        { "A tri/saw/pulse/sync/ring", basicRows (Model::A) },
        { "B combined", combinedRows (Model::B) },
        { "A combined", combinedRows (Model::A) },
    };
    std::printf ("%-30s %9s %12s %12s\n", "rows", "rate", "<=2k worst", "3.85k worst");
    for (const auto& g : groups)
        for (const auto& rc : rateConfigs())
        {
            double low = -300.0, high = -300.0;
            for (const auto& r : g.rows)
                for (const double hz : { 110.0, 1000.0, 2000.0, 3850.0 })
                {
                    const double db = measureRow (r, hz, rc).worstDb;
                    (hz <= 2000.0 ? low : high) = std::max (hz <= 2000.0 ? low : high, db);
                }
            std::printf ("%-30s %9.0f %12.1f %12.1f\n", g.name, rc.internalRate, low, high);
        }
    for (const Model m : { Model::B, Model::A })
        for (const int mode : { 0, 1 })
            for (const auto& rc : rateConfigs())
            {
                double worst = -300.0;
                for (const double hz : { 110.0, 1000.0, 2000.0 })
                    worst = std::max (worst, measureWrites (m, mode, hz, rc).worstDb);
                std::printf ("%-30s %9.0f %12.1f %12s\n", (std::string (m == Model::A ? "A " : "B ") + (mode == 0 ? "waveform switch/tick" : "test-bit reset/tick")).c_str(),
                             rc.internalRate, worst, "-");
            }
}

namespace
{
/** The patches of the shift test: random schedules on both models whose waveforms never use the long
    kernel (no pulse, no combined waveform: a pulse bit is dropped, a combination keeps its saw or its
    triangle; noise combinations stay, they render per cycle on the short kernel). */
std::vector<float> shiftPatch (Model m)
{
    bst::Rng r (m == Model::A ? 4242 : 4243);
    auto sched = randomSchedule (r, 60000, 260);
    auto strip = [] (std::uint8_t v) {
        if ((v & ctrl::noise) == 0)
        {
            v = static_cast<std::uint8_t> (v & ~ctrl::pulse);
            if ((v & ctrl::triangle) != 0 && (v & ctrl::saw) != 0)
                v = static_cast<std::uint8_t> (v & ~ctrl::triangle);
        }
        return v;
    };
    for (auto& c : sched)
    {
        if (c.kind == 0 && c.reg % 7 == 4)
            c.value = strip (c.value);
        if (c.kind == 1)
            c.in.control = strip (c.in.control);
    }
    Chip c = makeChip (m, 96000.0);
    return renderSchedule (c, sched, sched.back().cycle + 3000, 97);
}
} // namespace

// Generator (hidden): writes the shift test's golden (the renderer before the long kernel) to $BS_GOLDEN_OUT.
TEST_CASE ("render: dump the shift golden", "[.render-golden-dump]")
{
    const char* path = std::getenv ("BS_GOLDEN_OUT");
    REQUIRE (path != nullptr);
    std::FILE* f = std::fopen (path, "w");
    REQUIRE (f != nullptr);
    for (const Model m : { Model::B, Model::A })
    {
        const auto y = shiftPatch (m);
        std::fprintf (f, "%s %zu\n", m == Model::A ? "A" : "B", y.size());
        for (const float v : y)
        {
            std::uint32_t bits = 0;
            std::memcpy (&bits, &v, 4);
            std::fprintf (f, "%08x\n", bits);
        }
    }
    std::fclose (f);
}

TEST_CASE ("render: steps on the short kernel come out as before, only delayed to the long kernel's latency", "[render][shift]")
{
    // tests/bside/render/shift-golden.txt is the output of this renderer with every step on the short
    // kernel (the long one switched off while the golden was made; "[.render-golden-dump]"), on patches that
    // never use the long kernel (shiftPatch: no pulse, no combined waveform). Today's output must be that
    // output delayed by kLatencySamples - 8 samples (0: both kernels have 8 zero crossings): bit for bit with
    // the compiler that made the golden (Apple Clang); other compilers may contract floating-point
    // expressions differently, so elsewhere it must agree within float rounding. Pulse and combined
    // waveforms use the long kernel and are not covered by this golden (their own alias and timing tests).
    std::FILE* f = std::fopen (BS_RENDER_DIR "/shift-golden.txt", "r");
    REQUIRE (f != nullptr);
    constexpr int shift = Chip::kLatencySamples - 8;
    for (const Model m : { Model::B, Model::A })
    {
        char name = 0;
        std::size_t count = 0;
        REQUIRE (std::fscanf (f, " %c %zu", &name, &count) == 2);
        REQUIRE (name == (m == Model::A ? 'A' : 'B'));
        std::vector<float> old (count);
        for (auto& v : old)
        {
            unsigned bits = 0;
            REQUIRE (std::fscanf (f, " %x", &bits) == 1);
            const auto b32 = static_cast<std::uint32_t> (bits);
            std::memcpy (&v, &b32, 4);
        }
        const auto now = shiftPatch (m);
        REQUIRE (now.size() == old.size());
        double worst = 0.0;
        std::size_t differing = 0;
        for (std::size_t i = 0; i + shift < now.size(); ++i)
        {
            const double d = std::abs (static_cast<double> (now[i + shift]) - static_cast<double> (old[i]));
            worst = std::max (worst, d);
            differing += d > 0.0 ? 1u : 0u;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t> (shift); ++i)
            CHECK (! (std::abs (now[i]) > 0.0f)); // the longer pre-roll is silent
        WARN ((m == Model::A ? "Model A (no table)" : "Model B") << ": " << now.size() - shift << " samples compared after a shift of " << shift
                                                                  << ": " << differing << " differ, worst " << worst);
#if defined(__APPLE__) && defined(__clang__)
        CHECK (differing == 0);
#else
        CHECK (worst < 1.0e-6);
#endif
    }
    std::fclose (f);
}
