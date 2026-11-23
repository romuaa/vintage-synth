// The renderer's two buses (docs/architecture.md 8.19.4, S3b-1; Chip.h "BUSES"): each oscillator on the bus its
// routing bit names (oscillator 3 muted on the direct bus by 0x18 bit 7), the buses summing to the single sum, a
// routing write as a band-limited step of the membership at its exact cycle, any split bit-identical, no allocation,
// the alias budgets of 8.4.5 on each bus.

#include "FuzzSupport.h"
#include "RenderTestSupport.h"

#include "bside/chip/OutputStage.h"
#include "bside/chip/RenderTables.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <complex>
#include <numeric>

using namespace bs_render_test;
using vs::bside::chip::DigitalState;
namespace ctrl = vs::bside::chip::ctrl;

namespace
{
struct Buses
{
    std::vector<double> fa, da, fe, de, fv, dv;
    void resize (std::size_t n)
    {
        for (auto* v : { &fa, &da, &fe, &de, &fv, &dv })
            v->assign (n, 0.0);
    }
    std::size_t size() const { return fa.size(); }
    Chip::BusBlock at (std::size_t i)
    {
        return { fa.data() + i, da.data() + i, fe.data() + i, de.data() + i, fv.data() + i, dv.data() + i };
    }
    bool operator== (const Buses& o) const
    {
        auto same = [] (const std::vector<double>& a, const std::vector<double>& b) {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (std::bit_cast<std::uint64_t> (a[i]) != std::bit_cast<std::uint64_t> (b[i]))
                    return false;
            return true;
        };
        return same (fa, o.fa) && same (da, o.da) && same (fe, o.fe) && same (de, o.de) && same (fv, o.fv) && same (dv, o.dv);
    }
};

/** A change of a random schedule: a register write (oscillator registers, and 0x17 / 0x18 for the routing), a
    program-engine input, a restart, or a stop that is not a write (renderUntil stops there; nothing is applied). */
struct Change
{
    std::uint64_t cycle;
    int kind; // 0 write, 1 setOscillator, 2 powerOnKeepingTime, 3 stop only
    std::uint8_t reg, value;
    int osc;
    OscInput in;
};

std::vector<Change> routingSchedule (bst::Rng& r, std::uint64_t cycles, int count, bool mute, bool stops)
{
    std::vector<Change> v;
    std::uint64_t at = 0;
    for (int i = 0; i < count; ++i)
    {
        at += static_cast<std::uint64_t> (r.chance (20) ? r.range (0, 3) : r.range (1, static_cast<int> (2 * cycles / static_cast<std::uint64_t> (count))));
        Change c {};
        c.cycle = at;
        const int pick = r.range (0, 99);
        if (pick < 35)
        {
            // the routing and oscillator 3's mute (and whatever else these registers hold)
            c.kind = 0;
            c.reg = r.chance (50) ? std::uint8_t { 0x17 } : std::uint8_t { 0x18 };
            c.value = static_cast<std::uint8_t> (r.below (256));
            if (c.reg == 0x18 && ! mute)
                c.value &= 0x7f;
        }
        else if (pick < 75)
        {
            c.kind = 0;
            const int osc = r.range (0, 2);
            const int field = r.range (0, 6);
            c.reg = static_cast<std::uint8_t> (osc * 7 + field);
            c.value = static_cast<std::uint8_t> (r.below (256));
            if (field == 4)
                c.value = static_cast<std::uint8_t> (r.chance (70) ? (r.below (8) << 4) | r.below (16) : r.below (256));
            if (field == 1)
                c.value = static_cast<std::uint8_t> (r.below (r.chance (50) ? 64 : 256));
        }
        else if (pick < 85)
        {
            c.kind = 1;
            c.osc = r.range (0, 2);
            c.in.frequency = static_cast<double> (r.below (60000)) + static_cast<double> (r.below (1000)) / 1000.0;
            c.in.pulseWidth = static_cast<float> (r.below (4096)) + 0.25f;
            c.in.control = static_cast<std::uint8_t> (r.below (256));
            c.in.attackDecay = static_cast<std::uint8_t> (r.below (256));
            c.in.sustainRelease = static_cast<std::uint8_t> (r.below (256));
        }
        else if (pick < 87)
            c.kind = 2;
        else
            c.kind = stops ? 3 : 0;
        if (c.kind == 0 && pick >= 87)
        {
            c.reg = 0x17;
            c.value = static_cast<std::uint8_t> (r.below (8));
        }
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
        case 2: chip.powerOnKeepingTime(); break;
        default: break;
    }
}

/** The schedule rendered with renderUntil in chunks of at most `chunk` samples: the single sum (`buses` null) or
    the buses. Returns the single sum (empty for buses). */
std::vector<float> renderSchedule (Chip& chip, const std::vector<Change>& sched, std::uint64_t endCycle, int chunk, Buses* buses)
{
    std::vector<float> y;
    std::vector<float> buf (static_cast<std::size_t> (chunk), 0.0f);
    Buses part;
    part.resize (static_cast<std::size_t> (chunk));
    std::size_t next = 0;
    while (chip.cycle() < endCycle || next < sched.size())
    {
        const std::uint64_t target = next < sched.size() ? sched[next].cycle : endCycle;
        int got = 0;
        if (buses == nullptr)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            got = chip.renderUntil (buf.data(), chunk, target);
            y.insert (y.end(), buf.begin(), buf.begin() + got);
        }
        else
        {
            got = chip.renderBusesUntil (part.at (0), chunk, target);
            auto add = [got] (std::vector<double>& to, const std::vector<double>& from) { to.insert (to.end(), from.begin(), from.begin() + got); };
            add (buses->fa, part.fa);
            add (buses->da, part.da);
            add (buses->fe, part.fe);
            add (buses->de, part.de);
            add (buses->fv, part.fv);
            add (buses->dv, part.dv);
        }
        if (chip.cycle() == target && next < sched.size())
            apply (chip, sched[next++]);
        else if (chip.cycle() >= endCycle && next >= sched.size())
            break;
    }
    return y;
}

Buses renderBuses (Chip& c, int n)
{
    Buses b;
    b.resize (static_cast<std::size_t> (n));
    c.renderBuses (b.at (0), n);
    return b;
}

/** One float step (unit in the last place) at the magnitude of v, at least that of 1 (the shaped rounding's bound is
    in the steps of the values it has carried: 5.7e-7 below 2, RenderTables.h OutputRounding). */
double floatStep (double v)
{
    const auto f = static_cast<float> (std::max (std::abs (v), 1.0));
    return static_cast<double> (std::nextafter (f, 1.0e30f) - f);
}
} // namespace

TEST_CASE ("buses: they sum to the single sum, voices and envelopes included (random schedules with routing writes)", "[render][buses]")
{
    // The single sum (8.17) is the float conversion of the same oscillator contributions the buses carry: within 4.75
    // float steps of their sum (the shaped rounding's bound, RenderTables.h OutputRounding) at every sample, with every
    // routing write moving voices between the buses. Model A with its voice bend switched off (the bend is applied on
    // the buses only). Oscillator 3 is never muted here (then it is on neither bus).
    int checked = 0;
    for (const Model m : { Model::B, Model::A })
        for (std::uint64_t seed = 1; seed <= 8; ++seed)
        {
            bst::Rng r (seed * 6151 + (m == Model::A ? 5 : 6));
            const double rate = rateConfigs()[static_cast<std::size_t> (r.below (7))].internalRate;
            const auto sched = routingSchedule (r, 300000, 260, false, false);
            const std::uint64_t endCycle = sched.back().cycle + 4000;
            Chip a = makeChip (m, rate);
            Chip b = makeChip (m, rate);
            b.renderBendVoices (false);
            const int chunk = r.range (1, 600);
            const auto y = renderSchedule (a, sched, endCycle, chunk, nullptr);
            Buses bus;
            renderSchedule (b, sched, endCycle, chunk, &bus);
            INFO ((m == Model::A ? "Model A" : "Model B") << ", seed " << seed << ", rate " << rate << ", chunk " << chunk);
            REQUIRE (bus.size() == y.size());
            CHECK (a.state() == b.state());
            double worstSteps = 0.0, worstVoices = 0.0;
            for (std::size_t i = 0; i < y.size(); ++i)
            {
                const double sum = bus.fa[i] + bus.da[i];
                worstSteps = std::max (worstSteps, std::abs (static_cast<double> (y[i]) - sum) / floatStep (sum));
                worstVoices = std::max (worstVoices, std::abs (bus.fv[i] + bus.dv[i] - 3.0));
                ++checked;
            }
            CHECK (worstSteps <= 4.75);
            CHECK (worstVoices < 1.0e-12);
        }
    CHECK (checked > 100000);
}

TEST_CASE ("buses: each oscillator on the bus its routing bit names, oscillator 3 muted on the direct bus unless routed", "[render][buses]")
{
    // Every routing byte (three bits) with and without the mute, set before the render. Each voice alone is measured
    // from single sums (a chip given only that voice's setup, minus two thirds of a chip given none: the other voices'
    // identical power-on output); the buses must hold exactly the voices their routing names, with their envelope
    // values and counts. Model B, and Model A without its bend (with it, the bus carries each voice bent: checked too).
    for (const Model m : { Model::B, Model::A })
    {
        const double rate = 96000.0;
        const int n = 6000;
        auto setup = [] (Chip& c, int only) {
            if (only < 0 || only == 0)
                setOsc (c, 0, 7480.0, 2048.0f, ctrl::pulse | ctrl::gate);
            if (only < 0 || only == 1)
                setOsc (c, 1, 3741.0, 0.0f, ctrl::saw | ctrl::gate);
            if (only < 0 || only == 2)
                setOsc (c, 2, 11223.0, 0.0f, ctrl::triangle | ctrl::gate);
        };
        auto single = [&] (int only) {
            Chip c = makeChip (m, rate);
            if (only >= -1)
                setup (c, only);
            return renderSamples (c, n);
        };
        const auto none = single (-2);
        std::array<std::vector<double>, 3> voice;
        for (int k = 0; k < 3; ++k)
        {
            const auto v = single (k);
            voice[static_cast<std::size_t> (k)].resize (static_cast<std::size_t> (n));
            for (std::size_t i = 0; i < v.size(); ++i)
                voice[static_cast<std::size_t> (k)][i] = static_cast<double> (v[i]) - 2.0 / 3.0 * static_cast<double> (none[i]);
        }
        // each voice's envelope value: the filter bus's envelope sum with only that voice routed
        std::array<std::vector<double>, 3> env;
        for (int k = 0; k < 3; ++k)
        {
            Chip c = makeChip (m, rate);
            setup (c, -1);
            c.write (0x17, static_cast<std::uint8_t> (1 << k));
            env[static_cast<std::size_t> (k)] = renderBuses (c, n).fe;
        }
        for (int route = 0; route < 16; ++route)
            for (const bool bend : { false, true })
            {
                if (bend && m == Model::B)
                    continue;
                Chip c = makeChip (m, rate);
                c.renderBendVoices (bend);
                setup (c, -1);
                c.write (0x17, static_cast<std::uint8_t> (0xa0 | (route & 7)));
                c.write (0x18, static_cast<std::uint8_t> ((route & 8) != 0 ? 0x9f : 0x1f));
                const auto b = renderBuses (c, n);
                INFO ((m == Model::A ? "Model A" : "Model B") << ", routing " << (route & 7) << ((route & 8) != 0 ? ", oscillator 3 muted" : "") << (bend ? ", bent" : ""));
                double worst = 0.0, worstEnv = 0.0;
                double nf = 0.0, nd = 0.0;
                for (int k = 0; k < 3; ++k)
                {
                    if (((route >> k) & 1) != 0)
                        nf += 1.0;
                    else if (! (k == 2 && (route & 8) != 0))
                        nd += 1.0;
                }
                for (std::size_t i = 0; i < b.size(); ++i)
                {
                    double ef = 0.0, ed = 0.0, ee = 0.0, eed = 0.0;
                    for (int k = 0; k < 3; ++k)
                    {
                        const auto ki = static_cast<std::size_t> (k);
                        const double e = env[ki][i];
                        const double x = bend ? vs::bside::chip::OutputStage::shapeVoice (m, voice[ki][i], e) : voice[ki][i];
                        if (((route >> k) & 1) != 0)
                        {
                            ef += x;
                            ee += e;
                        }
                        else if (! (k == 2 && (route & 8) != 0))
                        {
                            ed += x;
                            eed += e;
                        }
                    }
                    worst = std::max ({ worst, std::abs (b.fa[i] - ef), std::abs (b.da[i] - ed) });
                    worstEnv = std::max ({ worstEnv, std::abs (b.fe[i] - ee), std::abs (b.de[i] - eed) });
                    REQUIRE (std::abs (b.fv[i] - nf) < 1.0e-15);
                    REQUIRE (std::abs (b.dv[i] - nd) < 1.0e-15);
                }
                CHECK (worst < 2.0e-6); // the single sums' float rounding
                CHECK (worstEnv < 1.0e-12);
            }
    }
}

TEST_CASE ("buses: a routing write moves the oscillator between the buses as a band-limited step one cycle before its cycle, kLatencySamples late", "[render][buses][timing]")
{
    // Oscillator 1 frozen on a constant (frequency 0, saw at the power-on accumulator), its envelope held at full
    // level; oscillators 2 and 3 silent. Its routing is toggled at random cycles; the filter bus's voice count is the
    // band-limited membership: the long kernel's step one cycle before the write's cycle (the reference's convention
    // for the analog registers' writes, Chip::kAnalogWriteLeadCycles, pinned here as the number 1: measured on
    // isolated routing steps against generated traces, ChipVoiceTests.cpp kReferenceLeadCycles), kLatencySamples late
    // (resolved to one cycle), and the filter bus's audio is that membership times the voice's value. The direct bus
    // is its complement at every sample. Changed 2026-11-23: the step was expected at the write's cycle itself (the
    // oscillator writes' convention, about one cycle after the reference's).
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 88200.0, 192000.0, 384000.0 })
        {
            Chip c = makeChip (m, rate);
            c.renderBendVoices (false);
            setOsc (c, 0, 0.0, 2048.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
            Buses all;
            Buses part;
            part.resize (4096);
            auto take = [&] (int got) {
                for (auto [to, from] : { std::pair { &all.fa, &part.fa }, std::pair { &all.da, &part.da }, std::pair { &all.fv, &part.fv }, std::pair { &all.dv, &part.dv } })
                    to->insert (to->end(), from->begin(), from->begin() + got);
                all.fe.resize (all.fa.size());
                all.de.resize (all.fa.size());
            };
            c.renderBuses (part.at (0), 2000);
            take (2000);
            bst::Rng r (m == Model::A ? 31 : 32);
            const double cps = cyclesPerSample (c);
            std::vector<std::uint64_t> writeCycle;
            std::uint8_t routing = 0;
            for (int w = 0; w < 24; ++w)
            {
                const std::uint64_t at = c.cycle() + static_cast<std::uint64_t> (r.range (static_cast<int> (40 * cps), static_cast<int> (90 * cps)));
                while (c.cycle() < at)
                    take (c.renderBusesUntil (part.at (0), 4096, at));
                writeCycle.push_back (c.cycle());
                routing ^= 1;
                c.write (0x17, routing);
            }
            take (c.renderBusesUntil (part.at (0), 400, ~std::uint64_t { 0 }));
            // the expected membership: a sum of the long kernel's band-limited steps (its own table, BlepTable.h) at
            // cycle / cycles-per-sample + kLatencySamples, as ChipRenderTests' "[timing]" models a waveform write
            auto blepCum = [] (double t) {
                constexpr double z = Chip::LongBlep::kZeroCrossings;
                if (t <= -z)
                    return 0.0;
                if (t > z)
                    return 1.0;
                const int j = static_cast<int> (std::ceil (t)) - 1;
                return Chip::LongBlep::get().step (j, t - j) + (j >= 0 ? 1.0 : 0.0);
            };
            auto model = [&] (double shiftCycles) {
                double worst = 0.0;
                for (std::size_t s = 1000; s < all.fv.size(); ++s)
                {
                    double v = 0.0;
                    for (std::size_t w = 0; w < writeCycle.size(); ++w)
                    {
                        const double pos = (static_cast<double> (writeCycle[w]) + shiftCycles) / cps + Chip::kLatencySamples;
                        v += ((w % 2) == 0 ? 1.0 : -1.0) * blepCum (static_cast<double> (s) - pos);
                    }
                    worst = std::max (worst, std::abs (all.fv[s] - v));
                }
                return worst;
            };
            // the voice's constant value: the filter bus after the first write has settled (oscillator 1 alone on it)
            const double x = all.fa[static_cast<std::size_t> (static_cast<double> (writeCycle[0]) / cps) + Chip::kLatencySamples + 12];
            const double exact = model (-1.0), early = model (-2.0), late = model (0.0);
            INFO ((m == Model::A ? "Model A" : "Model B") << ", " << rate << " Hz: max deviation " << exact << " (one cycle early " << early
                                                          << ", at the write's own cycle " << late << ")");
            CHECK (exact < 1.0e-6); // the kernel's Hermite read against the table's own interpolation
            CHECK (early > 100.0 * exact); // the comparison resolves one cycle
            CHECK (late > 100.0 * exact);
            double worstAudio = 0.0, worstSum = 0.0;
            for (std::size_t i = 0; i < all.fa.size(); ++i)
            {
                worstAudio = std::max (worstAudio, std::abs (all.fa[i] - all.fv[i] * x));
                worstSum = std::max (worstSum, std::abs (all.fv[i] + all.dv[i] - 3.0));
            }
            CHECK (std::abs (x) > 0.05); // a value that shows the membership
            CHECK (worstAudio < 1.0e-12);
            CHECK (worstSum < 1.0e-12);
        }
}

TEST_CASE ("buses: bit-identical for any block split, stops that are not writes, and with batching off", "[render][buses][split]")
{
    for (const Model m : { Model::A, Model::B })
        for (std::uint64_t seed = 1; seed <= 6; ++seed)
        {
            bst::Rng r (seed * 3571 + (m == Model::A ? 7 : 8));
            const double rate = rateConfigs()[static_cast<std::size_t> (r.below (7))].internalRate;
            const auto sched = routingSchedule (r, 250000, 220, true, true);
            std::vector<Change> writesOnly;
            for (const auto& c : sched)
                if (c.kind != 3)
                    writesOnly.push_back (c);
            const std::uint64_t endCycle = sched.back().cycle + 3000;
            std::vector<Buses> outs;
            std::vector<DigitalState> states;
            for (const int chunk : { 1, 7, 61, 512 })
                for (const bool stops : { false, true })
                {
                    Chip c = makeChip (m, rate);
                    Buses b;
                    renderSchedule (c, stops ? sched : writesOnly, endCycle, chunk, &b);
                    outs.push_back (b);
                    states.push_back (c.state());
                }
            {
                Chip c = makeChip (m, rate);
                c.renderBatchingEnabled (false);
                Buses b;
                renderSchedule (c, writesOnly, endCycle, 333, &b);
                outs.push_back (b);
                states.push_back (c.state());
            }
            INFO ((m == Model::A ? "Model A" : "Model B") << ", seed " << seed << ", rate " << rate);
            for (std::size_t i = 1; i < outs.size(); ++i)
            {
                INFO ("variant " << i);
                CHECK (outs[i] == outs[0]);
                CHECK (states[i] == states[0]);
            }
        }
}

TEST_CASE ("buses: no allocation while rendering the buses", "[render][buses][rt]")
{
    if (! bst::allocationsAreCounted())
        SKIP ("allocation counting is off in this build (sanitizers)");
    for (const Model m : { Model::A, Model::B })
    {
        bst::Rng r (m == Model::A ? 41 : 42);
        const auto sched = routingSchedule (r, 200000, 200, true, false);
        Chip c = makeChip (m, 96000.0);
        Buses b;
        b.resize (512);
        const auto before = bst::totalAllocations();
        std::size_t next = 0;
        while (next < sched.size())
        {
            c.renderBusesUntil (b.at (0), 512, sched[next].cycle);
            if (c.cycle() == sched[next].cycle)
                apply (c, sched[next++]);
        }
        c.renderBuses (b.at (0), 512);
        CHECK (bst::totalAllocations() == before);
    }
}

namespace
{
/** As ChipRenderTests' measureRow for a lone oscillator 2 (oscillator 1 its silent sync / ring source): the row
    measured on the bus oscillator 2 is routed to (`toFilter`), with oscillator 1 on the other bus. */
AliasResult busRow (Model m, std::uint8_t control, float width, double ratio, double hz, const RateConfig& rc, bool toFilter, bool bend = false)
{
    constexpr int kN = 1 << 15;
    Chip c = makeChip (m, rc.internalRate);
    const bool sync = (control & ctrl::sync) != 0;
    int fund = oddBin (hz, rc.internalRate, kN);
    double f2 = binFrequency (c, fund, kN), f1 = 0.0;
    if (sync)
    {
        fund = oddBin (ratio >= 1.0 ? hz / ratio : hz, rc.internalRate, kN);
        f1 = binFrequency (c, fund, kN);
        f2 = f1 * ratio;
    }
    setOsc (c, 0, f1, 2048.0f, 0x00);
    setOsc (c, 1, f2, width, static_cast<std::uint8_t> (control | ctrl::gate));
    c.renderBendVoices (bend);
    c.write (0x17, toFilter ? std::uint8_t { 0x02 } : std::uint8_t { 0x01 });
    renderBuses (c, 4096);
    const auto b = renderBuses (c, kN);
    std::vector<float> y (static_cast<std::size_t> (kN));
    for (std::size_t i = 0; i < y.size(); ++i)
        y[i] = static_cast<float> (toFilter ? b.fa[i] : b.da[i]); // float only for the FFT's input type: no alias at -100 dB from it
    const bool combined = vs::bside::chip::wave::isCombined (vs::bside::chip::wave::bits (control));
    return aliasOf (y, rc.internalRate, rc.band, sync || combined ? -1 : fund, fund);
}
} // namespace

TEST_CASE ("buses: the alias budgets of 8.4.5 hold on each bus (88.2 and 96 kHz, both buses)", "[render][buses][alias]")
{
    // A subset of ChipRenderTests' budget rows, measured on the bus that carries the row's oscillator: triangle, saw,
    // pulses of several widths and sync on Model B (-100 dB), combined waveforms on Model A (-96 dB up to 2 kHz, -80 dB
    // up to 3.85 kHz), each with the 3 dB margin of 8.17. The bus is double, its float copy for the FFT rounds at about
    // -150 dB re a full-scale line. Model A without its voice bend: the bend is a static curve at the internal rate
    // and folds (-62 to -69 dB at 1 kHz as built), the aliasing of the static curves that S3b-2 removes (8.19.4); the
    // bus as built is reported below.
    struct Row
    {
        Model m;
        std::uint8_t control;
        float width;
        double ratio;
    };
    const Row rows[] { { Model::B, ctrl::triangle, 2048.0f, 0.0 },       { Model::B, ctrl::saw, 2048.0f, 0.0 },
                       { Model::B, ctrl::pulse, 64.0f, 0.0 },            { Model::B, ctrl::pulse, 4032.0f, 0.0 },
                       { Model::B, ctrl::saw | ctrl::sync, 2048.0f, 2.71 }, { Model::A, ctrl::triangle | ctrl::saw, 2048.0f, 0.0 },
                       { Model::A, ctrl::saw | ctrl::pulse, 1024.0f, 0.0 } };
    int measured = 0;
    for (const auto& rc : rateConfigs())
    {
        if (rc.internalRate > 96000.0)
            continue;
        for (const auto& row : rows)
            for (const double hz : { 1000.0, 3850.0 })
                for (const bool toFilter : { true, false })
                {
                    const double budget = row.m == Model::B && ! vs::bside::chip::wave::isCombined (vs::bside::chip::wave::bits (row.control)) ? -100.0 : (hz <= 2000.0 ? -96.0 : -80.0);
                    const auto a = busRow (row.m, row.control, row.width, row.ratio, hz, rc, toFilter);
                    INFO ((row.m == Model::A ? "Model A" : "Model B") << ", control " << static_cast<int> (row.control) << ", width " << row.width << ", " << hz << " Hz, "
                                                                      << rc.internalRate << " Hz internal, " << (toFilter ? "filter bus" : "direct bus") << ": worst "
                                                                      << a.worstDb << " dB at " << a.worstHz << " Hz");
                    CHECK (a.worstDb <= budget - 3.0);
                    ++measured;
                }
    }
    CHECK (measured == 2 * 7 * 2 * 2);
    for (const double hz : { 1000.0, 3850.0 })
    {
        const auto a = busRow (Model::A, ctrl::triangle | ctrl::saw, 2048.0f, 0.0, hz, rateConfigs()[1], true, true);
        WARN ("Model A triangle+saw at " << hz << " Hz, 96 kHz, filter bus with the voice bend (S3b-2's aliasing): worst " << a.worstDb << " dB");
    }
}
