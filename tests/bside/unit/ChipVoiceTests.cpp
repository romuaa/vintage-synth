// The whole chain (docs/architecture.md 8.19.4, S3b-1; ChipVoice.h): any split bit-identical, no allocation, stability
// as 8.19.1 through the chain, the write-discontinuity budget of 8.4.5 for routing, volume and mode writes on the
// chain's output before the board filters, a volume write with volume DC on as a band-limited step of its full size,
// and the Shared topology against Per Note with real chips.

#include "FuzzSupport.h"
#include "RenderTestSupport.h"

#include "bside/chip/ChipVoice.h"
#include "bside/chip/HalfBand.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace bs_render_test;
using vs::bside::chip::ChipVoice;
using vs::bside::chip::Filter;
using vs::bside::chip::FilterInput;
using vs::bside::chip::OutputStage;
namespace ctrl = vs::bside::chip::ctrl;

namespace
{
constexpr std::uint64_t kTick = 19656;

/** The write-timing convention the reference shows (Chip::kAnalogWriteLeadCycles, pinned here as a number): a routing,
    mute, volume or mode write takes effect ONE cycle before an oscillator write at the same cycle would. Measured
    2026-11-23 on isolated steps of generated traces (the reference's debug output against ours through renderChain,
    the midpoint of each step, 16 steps per trace): with the oscillator writes' convention these writes sat +0.9 / +1.0
    (volume), +0.9 to +1.2 (routing), +0.9 / +1.0 (mute) and +0.9 / +1.0 (mode) cycles late on average at 192 / 384 kHz,
    both models, while pulse-width writes sat within -0.1 .. 0.0 cycles. The committed reference data has no such
    trace (it is not regenerated here); the step traces are proposed for the next regeneration (docs/decisions.md
    "B-Side S3b-1"). */
constexpr double kReferenceLeadCycles = 1.0;

/** A voice with its own filter (Per Note); heap-allocated (a ChipVoice holds its blending copies). */
struct PerNote
{
    std::unique_ptr<ChipVoice> v = std::make_unique<ChipVoice>();
    std::unique_ptr<Filter> f = std::make_unique<Filter>();
    PerNote (Model m, double rate, bool volumeDc, float variation = 0.5f)
    {
        vs::bside::chip::Config cfg;
        cfg.model = m;
        cfg.internalRate = rate;
        cfg.volumeDc = volumeDc;
        cfg.variation = variation;
        v->prepare (cfg);
        f->prepare (m, rate, variation);
        v->setFilter (f.get());
    }
};

void osc (ChipVoice& v, int n, double f, float pw, std::uint8_t control, std::uint8_t ad = 0x00, std::uint8_t sr = 0xf0)
{
    OscInput in;
    in.frequency = f;
    in.pulseWidth = pw;
    in.control = control;
    in.attackDecay = ad;
    in.sustainRelease = sr;
    v.setOscillator (n, in);
}

struct Change
{
    std::uint64_t cycle;
    int kind; // 0 write, 1 setOscillator, 2 setFilterInput, 3 powerOnKeepingTime, 4 stop only
    std::uint8_t reg, value;
    int osc;
    OscInput in;
    FilterInput fi;
};

std::vector<Change> chainSchedule (bst::Rng& r, std::uint64_t cycles, int count, bool stops)
{
    std::vector<Change> v;
    std::uint64_t at = 0;
    for (int i = 0; i < count; ++i)
    {
        at += static_cast<std::uint64_t> (r.chance (25) ? r.range (0, 3) : r.range (1, static_cast<int> (2 * cycles / static_cast<std::uint64_t> (count))));
        Change c {};
        c.cycle = at;
        const int pick = r.range (0, 99);
        if (pick < 45)
        {
            c.kind = 0;
            c.reg = static_cast<std::uint8_t> (0x15 + r.range (0, 3));
            c.value = static_cast<std::uint8_t> (r.below (256));
        }
        else if (pick < 75)
        {
            c.kind = 0;
            const int o = r.range (0, 2), field = r.range (0, 6);
            c.reg = static_cast<std::uint8_t> (o * 7 + field);
            c.value = static_cast<std::uint8_t> (field == 4 ? (r.below (8) << 4) | (r.below (16) & 0x9u) : r.below (256));
            if (field == 1)
                c.value = static_cast<std::uint8_t> (r.below (64));
        }
        else if (pick < 85)
        {
            c.kind = 1;
            c.osc = r.range (0, 2);
            c.in = { static_cast<double> (r.below (40000)) + 0.5, static_cast<float> (r.below (4096)), static_cast<std::uint8_t> ((r.below (8) << 4) | 1u),
                     static_cast<std::uint8_t> (r.below (256)), static_cast<std::uint8_t> (r.below (256)) };
        }
        else if (pick < 93)
        {
            c.kind = 2;
            c.fi = { static_cast<float> (r.below (2048)) + 0.37f, static_cast<std::uint8_t> (r.below (16)), static_cast<std::uint8_t> (r.below (8)),
                     static_cast<std::uint8_t> (r.below (256)) };
        }
        else if (pick < 95)
            c.kind = 3;
        else
            c.kind = stops ? 4 : 0;
        if (c.kind == 0 && pick >= 95)
        {
            c.reg = 0x18;
            c.value = static_cast<std::uint8_t> (r.below (256));
        }
        v.push_back (c);
    }
    return v;
}

void apply (ChipVoice& v, const Change& c)
{
    switch (c.kind)
    {
        case 0: v.write (c.reg, c.value); break;
        case 1: v.setOscillator (c.osc, c.in); break;
        case 2: v.setFilterInput (c.fi); break;
        case 3: v.powerOnKeepingTime(); break;
        default: break;
    }
}

std::vector<double> renderSchedule (ChipVoice& v, const std::vector<Change>& sched, std::uint64_t endCycle, int chunk)
{
    std::vector<double> y, buf (static_cast<std::size_t> (chunk));
    std::size_t next = 0;
    while (v.chip().cycle() < endCycle || next < sched.size())
    {
        const std::uint64_t target = next < sched.size() ? sched[next].cycle : endCycle;
        std::fill (buf.begin(), buf.end(), 0.0);
        const int got = v.renderUntil (buf.data(), chunk, target);
        y.insert (y.end(), buf.begin(), buf.begin() + got);
        if (v.chip().cycle() == target && next < sched.size())
            apply (v, sched[next++]);
        else if (v.chip().cycle() >= endCycle && next >= sched.size())
            break;
    }
    return y;
}

bool sameBits (const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::bit_cast<std::uint64_t> (a[i]) != std::bit_cast<std::uint64_t> (b[i]))
            return false;
    return true;
}
} // namespace

TEST_CASE ("chain: bit-identical for any block split and stops that are not writes (random schedules of every write kind)", "[chain][split]")
{
    for (const Model m : { Model::B, Model::A })
        for (std::uint64_t seed = 1; seed <= 4; ++seed)
        {
            bst::Rng r (seed * 7727 + (m == Model::A ? 1 : 2));
            const double rate = rateConfigs()[static_cast<std::size_t> (r.below (7))].internalRate;
            const bool vdc = r.chance (50);
            const auto sched = chainSchedule (r, 300000, 300, true);
            std::vector<Change> writesOnly;
            for (const auto& c : sched)
                if (c.kind != 4)
                    writesOnly.push_back (c);
            const std::uint64_t endCycle = sched.back().cycle + 5000;
            std::vector<std::vector<double>> outs;
            for (const int chunk : { 1, 13, 64, 777 })
                for (const bool stops : { false, true })
                {
                    PerNote p (m, rate, vdc);
                    outs.push_back (renderSchedule (*p.v, stops ? sched : writesOnly, endCycle, chunk));
                }
            INFO ((m == Model::A ? "Model A" : "Model B") << ", seed " << seed << ", rate " << rate << ", volume DC " << vdc);
            bool finite = true;
            for (const double y : outs[0])
                finite = finite && std::isfinite (y) && std::abs (y) < 10.0;
            CHECK (finite);
            for (std::size_t i = 1; i < outs.size(); ++i)
            {
                INFO ("variant " << i);
                CHECK (sameBits (outs[i], outs[0]));
            }
        }
}

namespace
{
/** Bursts of combined writes: every 1500 cycles a resonance, volume, mode, cutoff and routing write at the same cycle
    and at neighbouring ones (offsets 0, 1, 2, 7 and 40 cycles), each burst's values varied. */
std::vector<Change> combinedBursts (int bursts)
{
    std::vector<Change> v;
    for (int b = 0; b < bursts; ++b)
    {
        const std::uint64_t base = 3000 + static_cast<std::uint64_t> (b) * 1500;
        const int o = b % 5;
        const std::uint64_t offs[] { 0, 1, 2, 7, 40 };
        auto put = [&] (std::uint64_t at, std::uint8_t reg, std::uint8_t value) {
            Change c;
            c.cycle = at;
            c.kind = 0;
            c.reg = reg;
            c.value = value;
            v.push_back (c);
        };
        put (base + offs[(o + 0) % 5], 0x17, static_cast<std::uint8_t> (((b * 5) % 16) << 4 | ((b % 3) == 0 ? 0x01 : 0x03)));
        put (base + offs[(o + 1) % 5], 0x18, static_cast<std::uint8_t> (0x10 << (b % 3) | (8 + b % 8)));
        put (base + offs[(o + 2) % 5], 0x16, static_cast<std::uint8_t> (0x40 + (b * 37) % 0x80));
        put (base + offs[(o + 3) % 5], 0x17, static_cast<std::uint8_t> (((b * 3) % 16) << 4 | 0x01));
        put (base + offs[(o + 4) % 5], 0x18, static_cast<std::uint8_t> (0x10 << ((b + 1) % 3) | 15));
    }
    std::stable_sort (v.begin(), v.end(), [] (const Change& a, const Change& b) { return a.cycle < b.cycle; });
    return v;
}

/** Per Note with a switch to Shared (setFilter (nullptr), this owner running a filter of its own) for output samples
    [s1, s2) and back (setFilter (the voice's filter)), the schedule's writes at their cycles, `chunk` samples per call. */
std::vector<double> renderSwitching (Model m, double rate, const std::vector<Change>& sched, int total, int s1, int s2, int chunk)
{
    PerNote p (m, rate, true);
    ChipVoice& v = *p.v;
    v.boardFiltersEnabled (false);
    osc (v, 0, 2000.0 * 16777216.0 / 985248.0, 2048.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
    osc (v, 1, 3000.0 * 16777216.0 / 985248.0, 1024.0f, static_cast<std::uint8_t> (ctrl::pulse | ctrl::gate));
    v.write (0x16, 0x80);
    v.write (0x17, 0x03);
    v.write (0x18, 0x1f);
    Filter owner;
    owner.prepare (m, rate, 0.5f);
    owner.setCutoff (1024.0f);
    owner.setMode (0x1f);
    std::vector<double> y;
    std::size_t next = 0;
    while (static_cast<int> (y.size()) < total)
    {
        const int at = static_cast<int> (y.size());
        if (at == s1)
            v.setFilter (nullptr);
        if (at == s2)
            v.setFilter (p.f.get());
        const bool shared = at >= s1 && at < s2;
        const int limit = std::min (total, at < s1 ? s1 : (at < s2 ? s2 : total)) - at;
        const int want = std::min ({ chunk, limit, ChipVoice::kBlock });
        const std::uint64_t until = next < sched.size() ? sched[next].cycle : ~std::uint64_t { 0 };
        std::array<double, ChipVoice::kBlock> out {};
        int got = 0;
        if (! shared)
            got = v.renderUntil (out.data(), want, until);
        else
        {
            got = v.renderBusesUntil (want, until);
            std::array<double, ChipVoice::kBlock> so {}, sl {}, ss {};
            std::array<Filter::Shape, ChipVoice::kBlock> sh {};
            for (int i = 0; i < got; ++i)
            {
                const double dc = v.staticFilterBusDc (i);
                so[static_cast<std::size_t> (i)] = owner.processLinearDouble (v.filterInputAc (i) + dc, sl[static_cast<std::size_t> (i)]);
                ss[static_cast<std::size_t> (i)] = owner.staticOutput (dc);
                sh[static_cast<std::size_t> (i)] = owner.outputShapeCoefficients();
            }
            v.finishShared (sh.data(), so.data(), sl.data(), ss.data(), 1, out.data(), got);
        }
        y.insert (y.end(), out.begin(), out.begin() + got);
        if (next < sched.size() && v.chip().cycle() == sched[next].cycle)
            apply (v, sched[next++]);
    }
    return y;
}
} // namespace

TEST_CASE ("chain: combined writes at the same and neighbouring cycles: any block split bit-identical, also across setFilter (nullptr) and back inside open windows", "[chain][split][shared]")
{
    // 8.19.4 after the third review: every write lands where the contract says whatever other writes are pending, and
    // setFilter is safe at any time. Bursts of resonance, volume, mode, cutoff and routing writes at the same and
    // neighbouring cycles (combinedBursts), rendered in blocks of 1, 3, 64 and 777 samples: bit for bit the same. Then
    // the same with a switch to Shared and back placed inside the windows of a burst (and after the third review's
    // crash case, a switch while a resonance change waits for its place): no crash, every sample finite, bit for bit the
    // same for every block size. Both models, 88.2 and 96 kHz.
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 88200.0, 96000.0 })
        {
            const auto sched = combinedBursts (24);
            const std::uint64_t endCycle = sched.back().cycle + 4000;
            std::vector<std::vector<double>> outs;
            for (const int chunk : { 1, 3, 64, 777 })
            {
                PerNote p (m, rate, true);
                osc (*p.v, 0, 2000.0 * 16777216.0 / 985248.0, 2048.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
                outs.push_back (renderSchedule (*p.v, sched, endCycle, chunk));
            }
            INFO ((m == Model::A ? "Model A " : "Model B ") << rate << " Hz");
            for (std::size_t i = 1; i < outs.size(); ++i)
                CHECK (sameBits (outs[i], outs[0]));
            // the switch to Shared and back at samples inside bursts' windows (a burst every 1500 cycles: about 137
            // samples at 88.2 kHz; the third burst's writes start at cycle 6000), and (the third review's crash) at the
            // first sample of a later burst at which a configuration is open (found with 1-sample blocks)
            const double cps = 985248.0 / rate;
            const int total = static_cast<int> (static_cast<double> (endCycle) / cps);
            int open = -1;
            {
                PerNote p (m, rate, true);
                osc (*p.v, 0, 2000.0 * 16777216.0 / 985248.0, 2048.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
                std::size_t nx = 0;
                double one[1];
                for (int i = 0; i < total && open < 0;)
                {
                    const std::uint64_t until = nx < sched.size() ? sched[nx].cycle : ~std::uint64_t { 0 };
                    one[0] = 0.0;
                    i += p.v->renderUntil (one, 1, until);
                    if (nx < sched.size() && p.v->chip().cycle() == sched[nx].cycle)
                        apply (*p.v, sched[nx++]);
                    if (p.v->chip().cycle() > 15000 && p.v->configsActive() >= 2)
                        open = i;
                }
            }
            REQUIRE (open > 0);
            // the third review's case exactly: resonance writes only, the switch at the first sample with the write's
            // configuration open (a resonance change waiting for its place)
            std::vector<Change> resOnly;
            for (int k = 0; k < 6; ++k)
            {
                Change c;
                c.cycle = 3000 + static_cast<std::uint64_t> (k) * 1500;
                c.kind = 0;
                c.reg = 0x17;
                c.value = static_cast<std::uint8_t> ((k % 2) == 0 ? 0xf3 : 0x03);
                resOnly.push_back (c);
            }
            int openRes = -1;
            {
                PerNote p (m, rate, true);
                osc (*p.v, 0, 2000.0 * 16777216.0 / 985248.0, 2048.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
                p.v->write (0x16, 0x80);
                p.v->write (0x17, 0x03);
                p.v->write (0x18, 0x1f);
                std::size_t nx = 0;
                double one[1];
                for (int i = 0; i < total && openRes < 0;)
                {
                    const std::uint64_t until = nx < resOnly.size() ? resOnly[nx].cycle : ~std::uint64_t { 0 };
                    one[0] = 0.0;
                    i += p.v->renderUntil (one, 1, until);
                    if (nx < resOnly.size() && p.v->chip().cycle() == resOnly[nx].cycle)
                        apply (*p.v, resOnly[nx++]);
                    if (nx > 0 && p.v->configsActive() >= 2)
                        openRes = i;
                }
            }
            REQUIRE (openRes > 0);
            struct Sw
            {
                const std::vector<Change>* sched;
                int s1, s2;
            };
            for (const Sw& sw0 : { Sw { &sched, static_cast<int> (6001.0 / cps) + 9, static_cast<int> (9002.0 / cps) + 11 },
                                   Sw { &sched, static_cast<int> (12000.0 / cps) + 12, static_cast<int> (12000.0 / cps) + 14 }, Sw { &sched, open, open + 64 },
                                   Sw { &resOnly, openRes, openRes + 64 } })
            {
                const int s1 = sw0.s1, s2 = sw0.s2;
                std::vector<std::vector<double>> sw;
                for (const int chunk : { 1, 3, 64, 777 })
                    sw.push_back (renderSwitching (m, rate, *sw0.sched, total, s1, s2, chunk));
                bool finite = true;
                for (const double v : sw[0])
                    finite = finite && std::isfinite (v) && std::abs (v) < 10.0;
                INFO ("Shared over output samples " << s1 << " .. " << s2);
                CHECK (finite);
                for (std::size_t i = 1; i < sw.size(); ++i)
                    CHECK (sameBits (sw[i], sw[0]));
            }
        }
}

TEST_CASE ("chain: no allocation while rendering, writing or blending", "[chain][rt]")
{
    if (! bst::allocationsAreCounted())
        SKIP ("allocation counting is off in this build (sanitizers)");
    for (const Model m : { Model::A, Model::B })
    {
        bst::Rng r (m == Model::A ? 51 : 52);
        const auto sched = chainSchedule (r, 200000, 250, false);
        PerNote p (m, 96000.0, true);
        std::vector<double> buf (512, 0.0);
        std::vector<float> fbuf (512, 0.0f);
        const auto before = bst::totalAllocations();
        std::size_t next = 0;
        while (next < sched.size())
        {
            if ((next % 2) == 0)
                p.v->renderUntil (buf.data(), 512, sched[next].cycle);
            else
                p.v->renderUntil (fbuf.data(), 512, sched[next].cycle);
            if (p.v->chip().cycle() == sched[next].cycle)
                apply (*p.v, sched[next++]);
        }
        p.v->render (buf.data(), 512);
        CHECK (bst::totalAllocations() == before);
    }
}

TEST_CASE ("chain: stable at every setting through the chain, a write every sample, silence after a loud input decays below -120 dB", "[chain][stability]")
{
    // 8.19.1's stability through the whole chain: three noise voices at full level routed to the filter, every mode
    // sum, resonance 0 and 15, cutoffs 0, 1024 and 2047, variation 0 / 0.5 / 1 (Model A), the lowest and highest
    // internal rates: finite and bounded; then a cutoff and a mode write every sample for 2000 samples; then the
    // voices released (release 0): the output falls below -120 dB (1e-6 in the reference's units) within 3 s and stays
    // there, with no limit cycle (the last 0.1 s below 1e-6 too).
    int runs = 0;
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 88200.0, 384000.0 })
            for (const int res : { 0, 15 })
                for (const std::uint8_t mode : { std::uint8_t { 0x10 }, std::uint8_t { 0x20 }, std::uint8_t { 0x40 }, std::uint8_t { 0x70 } })
                    for (const float var : { 0.0f, 0.5f, 1.0f })
                    {
                        if (m == Model::B && var < 0.4f)
                            continue;
                        if (var > 0.6f && (rate > 100000.0 || res == 0))
                            continue; // variation: one corner of the grid
                        for (const int fc : { 0, 1024, 2047 })
                        {
                            PerNote p (m, rate, true, var);
                            ChipVoice& v = *p.v;
                            for (int n = 0; n < 3; ++n)
                                osc (v, n, 60000.0 - 7000.0 * n, 2048.0f, ctrl::noise | ctrl::gate, 0x00, 0xf0);
                            v.write (0x15, static_cast<std::uint8_t> (fc & 7));
                            v.write (0x16, static_cast<std::uint8_t> (fc >> 3));
                            v.write (0x17, static_cast<std::uint8_t> ((res << 4) | 0x07));
                            v.write (0x18, static_cast<std::uint8_t> (mode | 0x0f));
                            std::vector<double> y (static_cast<std::size_t> (rate * 0.2), 0.0);
                            v.render (y.data(), static_cast<int> (y.size()));
                            double peak = 0.0;
                            bool finite = true;
                            for (const double s : y)
                            {
                                finite = finite && std::isfinite (s);
                                peak = std::max (peak, std::abs (s));
                            }
                            // a cutoff and a mode write every sample
                            std::vector<double> one (1, 0.0);
                            bst::Rng r (static_cast<std::uint64_t> (fc + res * 31 + mode));
                            for (int i = 0; i < 2000; ++i)
                            {
                                v.write (0x16, static_cast<std::uint8_t> (r.below (256)));
                                v.write (0x18, static_cast<std::uint8_t> (0x0f | (r.below (8) << 4)));
                                one[0] = 0.0;
                                v.render (one.data(), 1);
                                finite = finite && std::isfinite (one[0]);
                                peak = std::max (peak, std::abs (one[0]));
                            }
                            v.write (0x16, static_cast<std::uint8_t> (fc >> 3));
                            v.write (0x18, static_cast<std::uint8_t> (mode | 0x0f));
                            for (int n = 0; n < 3; ++n)
                                v.write (static_cast<std::uint8_t> (7 * n + 4), ctrl::noise); // gate off: release 0
                            std::vector<double> tail (static_cast<std::size_t> (rate * 3.0), 0.0);
                            v.render (tail.data(), static_cast<int> (tail.size()));
                            double last = 0.0;
                            for (std::size_t i = tail.size() - static_cast<std::size_t> (rate * 0.1); i < tail.size(); ++i)
                                last = std::max (last, std::abs (tail[i]));
                            INFO ((m == Model::A ? "Model A" : "Model B") << ", " << rate << " Hz, resonance " << res << ", mode " << static_cast<int> (mode)
                                                                          << ", cutoff " << fc << ", variation " << var << ": peak " << peak << ", last 0.1 s " << last);
                            CHECK (finite);
                            CHECK (peak < 10.0);
                            CHECK (last < 1.0e-6);
                            ++runs;
                        }
                    }
    CHECK (runs > 100);
}

namespace
{
struct WriteAlias
{
    AliasResult raw;      // the chain's output with the writes
    AliasResult baseline; // the worse of the two held states without writes (the static curves' own aliasing)
    AliasResult write;    // what the writes add: the output minus the two held states switched by the ideal band-limited step
};

/** The write budget (8.4.5) on the chain's output before the board filters: a voice at about hz (an exact multiple
    of 1 / (2 ticks)), a write at every tick alternating between two values (the signal repeats every two ticks: its
    lines lie every lineHz); the worst component off those lines in 1 Hz .. band relative to the strongest line of the
    output with the writes. The static curves (mixer, Model A's voice bend) alias on their own at the internal rate
    (S3b-2's work, 8.19.4); to measure the writes alone, the same chain is rendered held in each state, and the output
    with the writes is compared with those two switched by the long kernel's ideal band-limited step at each write's
    cycle (the chain's latency late, kReferenceLeadCycles early): what remains is what the writes add. */
WriteAlias chainWrites (Model m, const RateConfig& rc, double hz, bool vdc, std::uint8_t reg, std::uint8_t a, std::uint8_t b, std::uint8_t control,
                        std::uint8_t route, std::uint8_t modeVolume, int oscIndex)
{
    const std::uint64_t period = 2 * kTick;
    const double periods = std::max (1.0, std::round (hz * static_cast<double> (period) / 985248.0));
    const double lineHz = 985248.0 / static_cast<double> (period);
    int n = 1 << 14;
    while (rc.internalRate / n > lineHz / 20.0)
        n <<= 1;
    const int warm = 3 * static_cast<int> (rc.internalRate);
    const int total = warm + n;
    std::vector<std::uint64_t> writes;
    double cps = 1.0, latency = 0.0;
    auto render = [&] (int mode) { // 0 writes, 1 held at a, 2 held at b
        PerNote p (m, rc.internalRate, vdc);
        ChipVoice& v = *p.v;
        v.boardFiltersEnabled (false);
        cps = static_cast<double> (v.chip().sampleClock().increment()) / 4294967296.0;
        latency = static_cast<double> (v.latencySamples());
        osc (v, oscIndex, periods * 16777216.0 / static_cast<double> (period), 2048.0f, static_cast<std::uint8_t> (control | ctrl::gate));
        v.write (0x15, 0);
        v.write (0x16, 0x80); // cutoff 1024
        v.write (0x17, static_cast<std::uint8_t> (0x40 | route));
        v.write (0x18, modeVolume);
        if (mode != 0)
            v.write (reg, mode == 1 ? a : b);
        std::vector<double> y (static_cast<std::size_t> (total), 0.0);
        int done = 0;
        std::uint64_t index = 0, at = 1000;
        while (done < total)
        {
            done += v.renderUntil (y.data() + done, total - done, mode == 0 ? at : ~std::uint64_t { 0 });
            if (mode == 0 && v.chip().cycle() == at)
            {
                v.write (reg, (index++ % 2) == 0 ? b : a);
                writes.push_back (at);
                at += kTick;
            }
        }
        return y;
    };
    const auto yw = render (0), ya = render (1), yb = render (2);
    auto blepCum = [] (double t) {
        constexpr double z = Chip::LongBlep::kZeroCrossings;
        if (t <= -z)
            return 0.0;
        if (t > z)
            return 1.0;
        const int j = static_cast<int> (std::ceil (t)) - 1;
        return Chip::LongBlep::get().step (j, t - j) + (j >= 0 ? 1.0 : 0.0);
    };
    std::vector<float> xw (static_cast<std::size_t> (n)), xa (xw.size()), xb (xw.size()), xd (xw.size());
    std::size_t w0 = 0;
    for (std::size_t i = 0; i < xw.size(); ++i)
    {
        const std::size_t s = static_cast<std::size_t> (warm) + i;
        // the state switch at sample s: b after every even write, a after every odd one, band-limited
        while (w0 + 1 < writes.size() && (static_cast<double> (writes[w0 + 1]) - kReferenceLeadCycles) / cps + latency + 9.0 < static_cast<double> (s))
            ++w0;
        double sw = 0.0;
        for (std::size_t k = w0 > 2 ? w0 - 2 : 0; k < writes.size() && k < w0 + 4; ++k)
        {
            const double pos = (static_cast<double> (writes[k]) - kReferenceLeadCycles) / cps + latency;
            const double h = blepCum (static_cast<double> (s) - pos);
            sw += (k % 2) == 0 ? h : -h;
        }
        // before the writes in the window: the state left by the earlier ones
        const std::size_t firstK = w0 > 2 ? w0 - 2 : 0;
        if ((firstK % 2) == 1)
            sw += 1.0;
        xw[i] = static_cast<float> (yw[s]);
        xa[i] = static_cast<float> (ya[s]);
        xb[i] = static_cast<float> (yb[s]);
        xd[i] = static_cast<float> (yw[s] - (ya[s] + (yb[s] - ya[s]) * sw));
    }
    // every figure relative to the output's strongest line other than DC (8.19.4 after the round over phases: the main
    // lobe of the mean, +-8 bins, is not a line of the signal; with volume DC on the output's mean, the mixer's offset,
    // lies tens of dB above the audio and was taken as the reference before)
    const double binHz = rc.internalRate / static_cast<double> (n);
    const auto magW = windowedMagnitudes (xw);
    double refW = 0.0;
    for (std::size_t k = 9; k < magW.size() && static_cast<double> (k) * binHz <= rc.band; ++k)
        refW = std::max (refW, magW[k]);
    auto relative = [&] (const std::vector<double>& mag) {
        AliasResult res = offGridAlias (mag, rc.internalRate, rc.band, lineHz, 8.0);
        double own = 0.0;
        for (std::size_t k = 1; k < mag.size() && static_cast<double> (k) * binHz <= rc.band; ++k)
            own = std::max (own, mag[k]);
        res.worstDb += 20.0 * std::log10 ((own + 1e-300) / (refW + 1e-300));
        return res;
    };
    WriteAlias r;
    r.raw = relative (magW);
    const auto ra = relative (windowedMagnitudes (xa));
    const auto rb = relative (windowedMagnitudes (xb));
    r.baseline = ra.worstDb > rb.worstDb ? ra : rb;
    r.write = relative (windowedMagnitudes (xd));
    return r;
}
} // namespace

namespace
{
/** One write per tick: at `offset` cycles after the tick, register `reg` takes `even` on even ticks and `odd` on odd
    ones (0x16 is the cutoff's high byte, 0x17 resonance and routing, 0x18 mode and volume). */
struct TickWrite
{
    int offset;
    std::uint8_t reg, even, odd;
};

/** The starting registers of a write case. */
struct WriteStart
{
    std::uint8_t cutoffHi = 0x80, resRoute = 0x01, modeVolume = 0x1f;
};

struct WriteExcess
{
    AliasResult ours, ideal, excess, idealFloor;
};

constexpr std::uint64_t kWriteStart = 1000;

/** The centre of a write's blend weights (the long kernel's unit step at the write, Chip::unitStepTaps) in filter
    samples: the place 8.19.4 gives a write. */
double switchRuleCentre (Chip& c, const Chip::EventTime& t)
{
    std::array<double, vs::bside::chip::render::kKernelTaps> taps {};
    int first = 0;
    c.unitStepTaps (t.base, taps, first);
    const auto s0 = static_cast<std::int64_t> (t.sample);
    auto weight = [&] (std::int64_t q) {
        const std::int64_t k = q - (s0 + first);
        return (q >= s0 ? 1.0 : 0.0) + (k >= 0 && k < vs::bside::chip::render::kKernelTaps ? taps[static_cast<std::size_t> (k)] : 0.0);
    };
    double centre = 0.0;
    for (std::int64_t q = s0 + first; q <= s0 + first + vs::bside::chip::render::kKernelTaps; ++q)
        centre += (weight (q) - weight (q - 1)) * static_cast<double> (q);
    return centre;
}

/** Cascaded half-band interpolators (x factor, 120 dB): one input sample gives `factor` outputs. */
struct Upsampler
{
    std::array<vs::bside::chip::HalfBandUp, 6> stage {};
    int stages = 0;
    double delayF = 0.0; // in output samples
    void prepare (double rate, int factor)
    {
        stages = 0;
        delayF = 0.0;
        for (int f = 2; f <= factor; f *= 2)
        {
            stage[static_cast<std::size_t> (stages)].prepare (vs::bside::chip::designHalfBand (rate * f, 20000.0, 1.0e-6));
            delayF += static_cast<double> (stage[static_cast<std::size_t> (stages)].delay2x()) * (factor / f);
            ++stages;
        }
    }
    void push (double x, double* out)
    {
        std::array<double, 64> a {}, b {};
        a[0] = x;
        int n = 1;
        for (int s = 0; s < stages; ++s)
        {
            for (int i = 0; i < n; ++i)
                stage[static_cast<std::size_t> (s)].process (a[static_cast<std::size_t> (i)], b[static_cast<std::size_t> (2 * i)], b[static_cast<std::size_t> (2 * i + 1)]);
            n *= 2;
            a = b;
        }
        for (int i = 0; i < n; ++i)
            out[i] = a[static_cast<std::size_t> (i)];
    }
};

/** Cascaded half-band decimators: `factor` inputs give one output. */
struct Downsampler
{
    std::array<vs::bside::chip::HalfBandDown, 6> stage {};
    int stages = 0;
    double delayF = 0.0; // in input samples
    void prepare (double rate, int factor)
    {
        stages = 0;
        delayF = 0.0;
        for (int f = factor; f >= 2; f /= 2)
        {
            stage[static_cast<std::size_t> (stages)].prepare (vs::bside::chip::designHalfBand (rate * f, 20000.0, 1.0e-6));
            delayF += static_cast<double> (stage[static_cast<std::size_t> (stages)].delay2x()) * (factor / f);
            ++stages;
        }
    }
    double push (const double* in, int factor)
    {
        std::array<double, 64> a {};
        for (int i = 0; i < factor; ++i)
            a[static_cast<std::size_t> (i)] = in[i];
        int n = factor;
        for (int s = 0; s < stages; ++s)
        {
            for (int i = 0; i < n / 2; ++i)
                a[static_cast<std::size_t> (i)] = stage[static_cast<std::size_t> (s)].process (a[static_cast<std::size_t> (2 * i)], a[static_cast<std::size_t> (2 * i + 1)]);
            n /= 2;
        }
        return a[0];
    }
};

/** The chain's output (board filters off) with the case's writes every tick, a saw at `oscF` (register units). */
std::vector<double> oursWithWrites (Model m, double rate, bool vdc, double oscF, const WriteStart& w0, const std::vector<TickWrite>& ws, int total,
                                    std::uint64_t tickLen = kTick)
{
    PerNote p (m, rate, vdc);
    ChipVoice& v = *p.v;
    v.boardFiltersEnabled (false);
    osc (v, 0, oscF, 2048.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
    v.write (0x15, 0);
    v.write (0x16, w0.cutoffHi);
    v.write (0x17, w0.resRoute);
    v.write (0x18, w0.modeVolume);
    std::vector<double> y (static_cast<std::size_t> (total), 0.0);
    int done = 0;
    std::uint64_t tick = 0;
    std::size_t next = 0;
    while (done < total)
    {
        const std::uint64_t at = kWriteStart + tick * tickLen + static_cast<std::uint64_t> (ws[next].offset);
        done += v.renderUntil (y.data() + done, total - done, at);
        if (v.chip().cycle() == at)
        {
            const auto& w = ws[next];
            v.write (w.reg, (tick % 2) == 0 ? w.even : w.odd);
            if (++next == ws.size())
            {
                next = 0;
                ++tick;
            }
        }
    }
    return y;
}

/** The same writes rendered ideally (8.19.4 after the third review): the same chain at `factor` times the internal
    rate. The chip's buses (writes to it at their cycles; the bus pre-filters of Model A at the internal rate as in the
    chain), interpolated (120 dB half-bands); the filter and the stage at the high rate, each resonance, mode and volume
    write applied at the high-rate sample nearest its place (the centre of the chain's blend weights, Chip::unitStepTaps)
    and each cutoff write at the start of its own internal sample (the contract: "from the write's own sample on");
    Model B's mixer input through the chain's own pre-filter at the internal rate (decimated, filtered, interpolated
    again; the post-curve coefficients follow with that delay); the curves at the high rate; decimated back. */
std::vector<double> idealWithWrites (Model m, double rate, bool vdc, double oscF, const WriteStart& w0, const std::vector<TickWrite>& ws, int total, int factor,
                                     std::uint64_t tickLen = kTick, double placeShift = 0.0)
{
    // the buses and each write's place (in filter samples of the internal rate)
    Chip c = makeChip (m, rate);
    c.renderBendVoices (true);
    {
        OscInput in;
        in.frequency = oscF;
        in.pulseWidth = 2048.0f;
        in.control = static_cast<std::uint8_t> (ctrl::saw | ctrl::gate);
        in.attackDecay = 0x00;
        in.sustainRelease = 0xf0;
        c.setOscillator (0, in);
    }
    c.write (0x15, 0);
    c.write (0x16, w0.cutoffHi);
    c.write (0x17, w0.resRoute);
    c.write (0x18, w0.modeVolume);
    struct Ev
    {
        double place;   // filter sample (fractional) of the internal rate
        std::uint8_t reg, value;
    };
    std::vector<Ev> evs;
    const auto z = static_cast<std::size_t> (total);
    std::vector<double> fa (z), da (z), fe (z), de (z), fv (z), dv (z);
    const Chip::BusBlock all { fa.data(), da.data(), fe.data(), de.data(), fv.data(), dv.data() };
    int done = 0;
    std::uint64_t tick = 0;
    std::size_t next = 0;
    while (done < total)
    {
        const std::uint64_t at = kWriteStart + tick * tickLen + static_cast<std::uint64_t> (ws[next].offset);
        done += c.renderBusesUntil (all.from (done), total - done, at);
        if (c.cycle() == at)
        {
            const auto& w = ws[next];
            const std::uint8_t value = (tick % 2) == 0 ? w.even : w.odd;
            c.write (w.reg, value);
            const Chip::EventTime t = c.renderEventTime();
            double place = static_cast<double> (t.sample);
            if (w.reg != 0x16)
            {
                std::array<double, vs::bside::chip::render::kKernelTaps> taps {};
                int first = 0;
                c.unitStepTaps (t.base, taps, first);
                double centre = 0.0, prev = 0.0;
                for (int k = -1; k <= vs::bside::chip::render::kKernelTaps; ++k)
                {
                    const std::int64_t q = static_cast<std::int64_t> (t.sample) + first + k;
                    const std::int64_t kk = q - (static_cast<std::int64_t> (t.sample) + first);
                    const double wq = (q >= static_cast<std::int64_t> (t.sample) ? 1.0 : 0.0)
                                      + (kk >= 0 && kk < vs::bside::chip::render::kKernelTaps ? taps[static_cast<std::size_t> (kk)] : 0.0);
                    if (k >= 0)
                        centre += (wq - prev) * static_cast<double> (q);
                    prev = wq;
                }
                place = centre + placeShift;
            }
            evs.push_back ({ place, w.reg, value });
            if (++next == ws.size())
            {
                next = 0;
                ++tick;
            }
        }
    }
    // the 1x streams into the filter and the direct path
    OutputStage o1;
    o1.prepare (m, rate, vdc);
    o1.setRouting (static_cast<std::uint8_t> (w0.resRoute & 7));
    o1.setModeVolume (w0.modeVolume);
    const auto pre = vs::bside::chip::designHalfBand (rate, ChipVoice::kAntiAliasPassHz, ChipVoice::kAntiAliasDeviation);
    vs::bside::chip::HalfBandFir preAc, preDc, preDi, preMix;
    preAc.prepare (pre);
    preDc.prepare (pre);
    preDi.prepare (pre);
    preMix.prepare (pre);
    const double preDelay = m == Model::A ? static_cast<double> (preAc.delay()) : 0.0;
    // the high-rate parts
    const double rf = rate * factor;
    Filter f;
    f.prepare (m, rf, 0.5f);
    f.setCutoff (static_cast<float> (w0.cutoffHi << 3));
    f.setResonance (w0.resRoute >> 4);
    f.setMode (w0.modeVolume);
    OutputStage oG, oP; // the stage before the curve (gain) and after it (post coefficients; Model B: later)
    oG.prepare (m, rf, vdc);
    oP.prepare (m, rf, vdc);
    for (auto* o : { &oG, &oP })
    {
        o->setRouting (static_cast<std::uint8_t> (w0.resRoute & 7));
        o->setModeVolume (w0.modeVolume);
    }
    const double staticDc = m == Model::A ? OutputStage::voiceDc (Model::A) * fv[0] : oG.staticFilterBusDc();
    auto refreshDc = [&] (OutputStage& o, const Filter& fl) { o.setFilterStaticDc (fl.staticOutput (staticDc)); };
    Filter fPost = f; // the filter's static output as the post stage sees it (Model B: delayed with the mixer input)
    refreshDc (oG, f);
    refreshDc (oP, fPost);
    Upsampler upF, upD, upX;
    upF.prepare (rate, factor);
    upD.prepare (rate, factor);
    upX.prepare (rate, factor);
    Downsampler dnX, dnOut;
    dnX.prepare (rate, factor);
    dnOut.prepare (rate, factor);
    // high-rate sample of filter sample t: the buses show it lag samples later; the interpolator adds its delay
    const double lag = static_cast<double> (Chip::kLatencySamples + 1) + preDelay;
    const double postLag = m == Model::B ? dnX.delayF + factor * static_cast<double> (preMix.delay()) + upX.delayF : 0.0;
    // each event at its high-rate place: a cutoff at the start of its internal sample (an integer high-rate sample); the
    // others between two high-rate samples: the sample `at` is taken partly (alpha) with the settings before and partly
    // with those after, outputs and the filter's state alike (exact to first order in 1 / factor of a sample)
    struct HiEv
    {
        std::int64_t at;
        double alpha;
        std::uint8_t reg, value;
        bool post;
    };
    std::vector<HiEv> hi;
    for (const auto& e : evs)
    {
        const double base = factor * (e.place + lag) + upF.delayF;
        if (e.reg == 0x16)
        {
            const auto at = static_cast<std::int64_t> (std::floor (base + 0.5));
            hi.push_back ({ at, 0.0, e.reg, e.value, false });
            hi.push_back ({ static_cast<std::int64_t> (std::floor (base + postLag + 0.5)), 0.0, e.reg, e.value, true });
            continue;
        }
        for (const bool post : { false, true })
        {
            const double pl = base + (post ? postLag : 0.0) + 0.5; // the high-rate sample at.. at + 1 spans pl
            const double fl = std::floor (pl);
            hi.push_back ({ static_cast<std::int64_t> (fl), pl - fl, e.reg, e.value, post });
        }
    }
    std::stable_sort (hi.begin(), hi.end(), [] (const HiEv& a, const HiEv& b) { return a.at < b.at; });
    auto applyEv = [&] (const HiEv& e, Filter& fl, OutputStage& o) {
        if (e.reg == 0x16)
            fl.setCutoff (static_cast<float> (e.value << 3));
        else if (e.reg == 0x17)
            fl.setResonance (e.value >> 4);
        else
        {
            fl.setMode (e.value);
            o.setModeVolume (e.value);
        }
        refreshDc (o, fl);
    };
    std::size_t hn = 0;
    std::int64_t k = 0;
    // the post-curve coefficients (scale, offset, gain): memoryless, so applied after the decimation, each change as an
    // ideal band-limited step (a windowed sine-integral, +-256 samples) at its exact place in output samples
    struct PostStep
    {
        double place;
        std::array<double, 3> before, after;
    };
    std::vector<PostStep> postSteps;
    {
        OutputStage o = oP;
        Filter fl = fPost;
        auto coef = [&] () { return std::array<double, 3> { o.postScale(), o.postOffset(), o.postGain() }; };
        for (const auto& e : evs)
        {
            const auto before = coef();
            if (e.reg == 0x16)
                fl.setCutoff (static_cast<float> (e.value << 3));
            else if (e.reg == 0x17)
                fl.setResonance (e.value >> 4);
            else
            {
                fl.setMode (e.value);
                o.setModeVolume (e.value);
            }
            refreshDc (o, fl);
            const auto after = coef();
            const double placeOut = e.reg == 0x16 ? std::floor (e.place) : e.place;
            postSteps.push_back ({ placeOut + lag + (upF.delayF + postLag + dnOut.delayF) / factor, before, after });
        }
    }
    std::vector<double> y (z, 0.0);
    std::array<double, 64> inF {}, inD {}, xs {}, xs2 {}, ms {};
    for (std::size_t i = 0; i < z; ++i)
    {
        double a = 0.0, d = 0.0;
        if (m == Model::A)
        {
            a = preAc.process (o1.filterInputAc (fa[i], fe[i])) + preDc.process (OutputStage::voiceDc (Model::A) * fv[i]);
            d = preDi.process (o1.directSumWith (da[i], de[i], dv[i]));
        }
        else
        {
            a = o1.filterInput (fa[i], fe[i]);
            d = o1.directSumWith (da[i], de[i], dv[i]);
        }
        upF.push (a, inF.data());
        upD.push (d, inD.data());
        std::array<double, 64> postAlpha {};
        std::array<bool, 64> postMixed {};
        std::vector<std::pair<int, OutputStage>> postOld; // (sample j, the post stage before) for the mixed post samples
        postOld.clear();
        for (int j = 0; j < factor; ++j, ++k)
        {
            postMixed[static_cast<std::size_t> (j)] = false;
            double alphaG = 0.0;
            bool mixG = false;
            Filter fOld;
            OutputStage oGOld;
            while (hn < hi.size() && hi[hn].at <= k)
            {
                const auto& e = hi[hn++];
                if (e.post)
                {
                    if (e.reg != 0x16 && e.alpha > 0.0 && ! postMixed[static_cast<std::size_t> (j)] && e.at == k)
                    {
                        postOld.push_back ({ j, oP });
                        postMixed[static_cast<std::size_t> (j)] = true;
                        postAlpha[static_cast<std::size_t> (j)] = e.alpha;
                    }
                    applyEv (e, fPost, oP);
                    continue;
                }
                if (e.reg != 0x16 && e.alpha > 0.0 && ! mixG && e.at == k)
                {
                    fOld = f;
                    oGOld = oG;
                    mixG = true;
                    alphaG = e.alpha;
                }
                applyEv (e, f, oG);
            }
            auto sampleX = [&] (Filter& fl, const OutputStage& o) {
                double lp = 0.0;
                const double lin = fl.processLinearDouble (inF[static_cast<std::size_t> (j)], lp);
                const double g = o.gainFactor();
                double x = g * (lin + inD[static_cast<std::size_t> (j)]);
                if (m == Model::A)
                    x += g * fl.outputShape (lp);
                return x;
            };
            if (! mixG)
                xs[static_cast<std::size_t> (j)] = sampleX (f, oG);
            else
            {
                // the sample from one state: alphaG of it with the settings before, the rest with those after
                const double xOld = sampleX (fOld, oGOld);
                const double xNew = sampleX (f, oG);
                xs[static_cast<std::size_t> (j)] = xNew + alphaG * (xOld - xNew);
                const auto sn = f.state(), so = fOld.state();
                f.setState ({ sn[0] + alphaG * (so[0] - sn[0]), sn[1] + alphaG * (so[1] - sn[1]) });
            }
        }
        if (m == Model::B)
        {
            // the chain's pre-filter on the mixer input, at the internal rate
            upX.push (preMix.process (dnX.push (xs.data(), factor)), xs2.data());
        }
        else
            xs2 = xs;
        for (int j = 0; j < factor; ++j)
            ms[static_cast<std::size_t> (j)] = oP.mixerCurve (xs2[static_cast<std::size_t> (j)]); // the curve; its post coefficients below
        y[i] = dnOut.push (ms.data(), factor);
    }
    // the post coefficients per output sample: the first ones, plus each change's band-limited step
    std::vector<std::array<double, 3>> coef (z);
    {
        OutputStage o0;
        o0.prepare (m, rf, vdc);
        o0.setRouting (static_cast<std::uint8_t> (w0.resRoute & 7));
        o0.setModeVolume (w0.modeVolume);
        Filter f0;
        f0.prepare (m, rf, 0.5f);
        f0.setCutoff (static_cast<float> (w0.cutoffHi << 3));
        f0.setResonance (w0.resRoute >> 4);
        f0.setMode (w0.modeVolume);
        refreshDc (o0, f0);
        const std::array<double, 3> c0 { o0.postScale(), o0.postOffset(), o0.postGain() };
        for (auto& cc : coef)
            cc = c0;
        constexpr double pi = 3.14159265358979323846;
        constexpr int half = 256;
        std::vector<double> g (2 * half + 2);
        for (const auto& ps : postSteps)
        {
            // the band-limited unit step at ps.place: the running sum of a Hann-windowed sinc centred there, normalised
            const auto k0 = static_cast<std::int64_t> (std::floor (ps.place)) - half;
            double sum = 0.0;
            for (int kk = 0; kk < 2 * half + 2; ++kk)
            {
                const double x = static_cast<double> (k0 + kk) - ps.place;
                const double sinc = std::abs (x) < 1e-12 ? 1.0 : std::sin (pi * x) / (pi * x);
                const double win = std::abs (x) < half ? 0.5 + 0.5 * std::cos (pi * x / half) : 0.0;
                g[static_cast<std::size_t> (kk)] = sinc * win;
                sum += g[static_cast<std::size_t> (kk)];
            }
            double h = 0.0;
            for (std::int64_t q = std::max<std::int64_t> (0, k0); q < static_cast<std::int64_t> (z); ++q)
            {
                const std::int64_t kq = q - k0;
                if (kq < 2 * half + 2)
                    h += g[static_cast<std::size_t> (kq)] / sum;
                else
                    h = 1.0;
                auto& cq = coef[static_cast<std::size_t> (q)];
                for (std::size_t kk = 0; kk < 3; ++kk)
                    cq[kk] += h * (ps.after[kk] - ps.before[kk]);
            }
        }
    }
    for (std::size_t i = 0; i < z; ++i)
        y[i] = (coef[i][0] * y[i] + coef[i][1]) * coef[i][2];
    return y;
}

/** Off-line figures (8.4.5's measure on a signal of period two ticks) of ours, the ideal render and their difference
    (the ideal's level and offset fitted, aligned by the best integer shift), each relative to ours' strongest line
    other than DC. */
struct Diff
{
    AliasResult a, b, d;
};
Diff offLine (const std::vector<double>& yw, const std::vector<double>& yi, double rate, int warm, int n)
{
    const std::uint64_t period = 2 * kTick;
    const double lineHz = 985248.0 / static_cast<double> (period);
    const double band = 20000.0;
    const int total = warm + n;
    auto mismatch = [&] (int shift) {
        double e = 0.0;
        for (int i = warm; i < total; i += 7)
        {
            const int k = i - shift;
            if (k < 0 || k >= total)
                continue;
            const double d = yw[static_cast<std::size_t> (i)] - yi[static_cast<std::size_t> (k)];
            e += d * d;
        }
        return e;
    };
    int best = 0;
    double be = mismatch (0);
    for (int sft = -200; sft <= 200; ++sft)
        if (mismatch (sft) < be)
        {
            be = mismatch (sft);
            best = sft;
        }
    // the analysed window of ours, [start, start + n), with its match yi[i - best] inside yi: a negative shift (the
    // ideal later than ours) moves the window that much earlier (the signal is periodic; warm is far longer than any shift)
    const int start = warm - std::max (0, -best);
    double sw = 0.0, si = 0.0, sii = 0.0, swi = 0.0;
    for (int i = start; i < start + n; ++i)
    {
        const double w = yw[static_cast<std::size_t> (i)], v = yi[static_cast<std::size_t> (i - best)];
        sw += w;
        si += v;
        sii += v * v;
        swi += w * v;
    }
    const double cnt = static_cast<double> (n);
    const double den = sii - si * si / cnt;
    const double alpha = den > 0.0 ? (swi - sw * si / cnt) / den : 1.0;
    const double beta = (sw - alpha * si) / cnt;
    std::vector<float> xw (static_cast<std::size_t> (n)), xi (xw.size()), xd (xw.size());
    for (std::size_t i = 0; i < xw.size(); ++i)
    {
        const auto s = static_cast<std::size_t> (start) + i;
        const double ideal = alpha * yi[static_cast<std::size_t> (static_cast<std::int64_t> (s) - best)] + beta;
        xw[i] = static_cast<float> (yw[s]);
        xi[i] = static_cast<float> (ideal);
        xd[i] = static_cast<float> (yw[s] - ideal);
    }
    const double binHz = rate / static_cast<double> (n);
    const auto magW = windowedMagnitudes (xw);
    double refW = 0.0;
    for (std::size_t q = 9; q < magW.size() && static_cast<double> (q) * binHz <= band; ++q)
        refW = std::max (refW, magW[q]);
    auto relative = [&] (const std::vector<double>& mag) {
        AliasResult res = offGridAlias (mag, rate, band, lineHz, 8.0);
        double own = 0.0;
        for (std::size_t q = 1; q < mag.size() && static_cast<double> (q) * binHz <= band; ++q)
            own = std::max (own, mag[q]);
        res.worstDb += 20.0 * std::log10 ((own + 1e-300) / (refW + 1e-300));
        return res;
    };
    return { relative (magW), relative (windowedMagnitudes (xi)), relative (windowedMagnitudes (xd)) };
}

/** A write case against its ideal render at `factor` (and at half of it, for the ideal's own placement error: the
    difference of the two, less 6 dB, since that error halves with each doubling). */
WriteExcess writeExcess (Model m, double rate, double hz, bool vdc, const WriteStart& w0, const std::vector<TickWrite>& ws, int factor)
{
    const std::uint64_t period = 2 * kTick;
    const double periods = std::max (1.0, std::round (hz * static_cast<double> (period) / 985248.0));
    const double lineHz = 985248.0 / static_cast<double> (period);
    int n = 1 << 14;
    while (rate / n > lineHz / 20.0)
        n <<= 1;
    const int warm = static_cast<int> (0.4 * rate);
    const int total = warm + n;
    const double oscF = periods * 16777216.0 / static_cast<double> (period);
    const auto yw = oursWithWrites (m, rate, vdc, oscF, w0, ws, total);
    const auto yi = idealWithWrites (m, rate, vdc, oscF, w0, ws, total, factor);
    const auto yh = idealWithWrites (m, rate, vdc, oscF, w0, ws, total, factor / 2);
    const Diff d = offLine (yw, yi, rate, warm, n);
    const Diff f = offLine (yi, yh, rate, warm, n);
    WriteExcess r;
    r.ours = d.a;
    r.ideal = d.b;
    r.excess = d.d;
    r.idealFloor = f.d;
    r.idealFloor.worstDb -= 6.0;
    return r;
}

/** The write cases of the gate: resonance alone; resonance followed by a volume, a mode or a cutoff write `gap` cycles
    later, and the reverse order. Resonance 0 <-> 15 (the low-pass at cutoff 1024, a mode write to the band-pass). */
struct WriteCase
{
    std::string name;
    WriteStart start;
    std::vector<TickWrite> writes;
    bool vdc = true;
};
std::vector<WriteCase> writeCases (bool all)
{
    std::vector<WriteCase> v;
    const TickWrite res { 0, 0x17, 0xf1, 0x01 };
    v.push_back ({ "resonance 0 <-> 15", {}, { res } });
    // resonance alone through every output (8.19.4 after the round over phases: 0 <-> 15 and 4 <-> 12, low- and
    // band-pass, volume DC on and off; the fourth review added high-pass and band + high-pass)
    // (std::uint8_t spelled out per value: MSVC warns about the int inside std::pair's converting constructor)
    using ModeCase = std::pair<const char*, std::uint8_t>;
    for (const auto& [mname, mv] : { ModeCase { "low-pass", std::uint8_t { 0x1f } }, ModeCase { "band-pass", std::uint8_t { 0x2f } },
                                     ModeCase { "high-pass", std::uint8_t { 0x4f } }, ModeCase { "band + high-pass", std::uint8_t { 0x6f } } })
        for (const bool vdc : { true, false })
            for (const bool wide : { true, false })
            {
                if (mv == 0x1f && vdc && wide)
                    continue; // the first case above
                if (! all && (! vdc || (! wide && mv != 0x6f)))
                    continue; // the normal run: 0 <-> 15 through every output, 4 <-> 12 through band + high-pass
                WriteStart st;
                st.modeVolume = mv;
                st.resRoute = wide ? 0x01 : 0x41;
                const TickWrite w = wide ? TickWrite { 0, 0x17, 0xf1, 0x01 } : TickWrite { 0, 0x17, 0xc1, 0x41 };
                v.push_back ({ std::string (wide ? "resonance 0 <-> 15, " : "resonance 4 <-> 12, ") + mname + (vdc ? "" : ", volume DC off"), st, { w }, vdc });
            }
    for (const int gap : all ? std::vector<int> { 1, 10, 40, 200 } : std::vector<int> { 10 })
    {
        const TickWrite vol { gap, 0x18, 0x1c, 0x1f }, mode { gap, 0x18, 0x2f, 0x1f }, cut { gap, 0x16, 0xa0, 0x80 };
        for (const auto& [what, w] : { std::pair<const char*, TickWrite> { "volume", vol }, { "mode", mode }, { "cutoff", cut } })
        {
            TickWrite later = w, first = res;
            v.push_back ({ std::string ("resonance, then ") + what + " " + std::to_string (gap) + " cycles later", {}, { res, later } });
            TickWrite lead = w;
            lead.offset = 0;
            first.offset = gap;
            v.push_back ({ std::string (what) + ", then resonance " + std::to_string (gap) + " cycles later", {}, { lead, first } });
        }
    }
    return v;
}

void runWriteGate (bool all, const std::vector<double>& rates, const std::vector<double>& hzs, int factor)
{
    int gated = 0, reported = 0;
    for (const Model m : { Model::B, Model::A })
        for (const double rate : rates)
            for (const auto& wc : writeCases (all))
            {
                double ours = -300.0, ideal = -300.0, excess = -300.0, floorDb = -300.0;
                for (const double hz : hzs)
                {
                    const auto r = writeExcess (m, rate, hz, wc.vdc, wc.start, wc.writes, factor);
                    ours = std::max (ours, r.ours.worstDb);
                    ideal = std::max (ideal, r.ideal.worstDb);
                    excess = std::max (excess, r.excess.worstDb);
                    floorDb = std::max (floorDb, r.idealFloor.worstDb);
                }
                const bool gate = floorDb <= -96.0; // the ideal's own placement error 6 dB below the gate
                std::printf ("[write-excess] Model %s %6.0f Hz %-52s ours %7.1f, ideal %7.1f, ideal's own floor %7.1f, excess %7.1f dB%s\n",
                             m == Model::A ? "A" : "B", rate, wc.name.c_str(), ours, ideal, floorDb, excess, gate ? "" : "  (reported: the ideal's floor)");
                INFO ((m == Model::A ? "Model A " : "Model B ") << rate << " Hz, " << wc.name << ": excess " << excess << " dB, the ideal's floor " << floorDb);
                if (gate)
                {
                    CHECK (excess <= -90.0);
                    ++gated;
                }
                else
                    ++reported;
            }
    std::printf ("[write-excess] %d rows gated, %d reported (the ideal render's floor above -96 dB)\n", gated, reported);
    CHECK (gated > 0);
}
} // namespace

TEST_CASE ("chain: writes every tick against an ideal render: the excess within the write budget (<= -90 dB to 2 kHz), resonance alone and with a volume, mode or cutoff write 10 cycles apart (96 kHz)", "[chain][alias][writes]")
{
    // 8.19.4 after the third review: the full set (resonance alone 0 <-> 15 and 4 <-> 12 through the low-, band-, high-
    // and band + high-pass with volume DC on and off; with a volume, mode or cutoff write at 1, 10, 40 and 200 cycles,
    // both orders; 88.2, 96 and 192 kHz; voices at 1 and 2 kHz) runs by its tag "[.chain-writes-full]"; here, at 96 kHz,
    // a voice at 2 kHz, both models, against the ideal render at 32 times the rate: resonance 0 <-> 15 through every
    // output, 4 <-> 12 through band + high-pass, and the gap of 10 cycles (a player routine's usual spacing).
    runWriteGate (false, { 96000.0 }, { 2000.0 }, 32);
}

TEST_CASE ("chain: writes every tick against an ideal render, the full set (hidden; minutes)", "[.chain-writes-full]")
{
    runWriteGate (true, { 88200.0, 96000.0, 192000.0 }, { 1000.0, 2000.0 }, 32);
}

namespace
{
/** The place of a resonance write's dynamics, measured on the filter by itself. A voice (Per Note) renders a saw at
    2 kHz through the low-pass at 1024 with the case's writes, one output sample at a time, recording the filter's input
    and output (ChipVoice::lastFilterInput / lastFilterOutput) and each write's place (the centre of its blend weights,
    Chip::unitStepTaps, in filter samples; a cutoff write: its own sample). A twin Filter runs the same input from the
    same start, the cutoff from its own sample on, the resonance's dynamics changed at the place + delta (the step that
    contains it mixed from the old and the new resonance, as 8.19.4 defines a change between samples), its output with
    the new parameters from the start of the write's window (where the voice's newest configuration takes them); the rms
    of the twin's output minus the voice's from the window's start to 600 samples after the resonance write, for delta on
    a grid of 0.05 samples from -4 to +4. Returned: the best delta and the rms there, relative to the write's change. */
struct PlaceFit
{
    double delta = 0.0, rmsAtBest = 0.0, rmsAtZero = 0.0, rmsAtTwentieth = 0.0, rmsAtOne = 0.0, change = 0.0;
};
PlaceFit placeFit (Model m, double rate, const std::vector<TickWrite>& ws)
{
    PerNote p (m, rate, true);
    ChipVoice& v = *p.v;
    v.boardFiltersEnabled (false);
    const double oscF = 2000.0 * 16777216.0 / 985248.0;
    osc (v, 0, oscF, 2048.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
    v.write (0x15, 0);
    v.write (0x16, 0x80);
    v.write (0x17, 0x01);
    v.write (0x18, 0x1f);
    const int total = static_cast<int> (0.03 * rate) + 2000;
    std::vector<double> in (static_cast<std::size_t> (total)), out (in.size());
    struct Ev
    {
        double place;
        std::uint8_t reg, value;
    };
    std::vector<Ev> evs;
    double windowStart = 0.0;
    std::size_t next = 0;
    double sink[1];
    const std::int64_t lag = v.filterLagSamples();
    for (int i = 0; i < total;)
    {
        const std::uint64_t at = next < ws.size() ? kWriteStart + 2000 + static_cast<std::uint64_t> (ws[next].offset) : ~std::uint64_t { 0 };
        sink[0] = 0.0;
        const int got = v.renderUntil (sink, 1, at);
        if (got == 1)
        {
            in[static_cast<std::size_t> (i)] = v.lastFilterInput();
            out[static_cast<std::size_t> (i)] = v.lastFilterOutput();
            ++i;
        }
        if (next < ws.size() && v.chip().cycle() == at)
        {
            const auto& w = ws[next++];
            v.write (w.reg, w.even);
            const Chip::EventTime t = v.chip().renderEventTime();
            double place = static_cast<double> (t.sample);
            if (w.reg == 0x17)
            {
                place = switchRuleCentre (v.chip(), t);
                std::array<double, vs::bside::chip::render::kKernelTaps> taps {};
                int first = 0;
                v.chip().unitStepTaps (t.base, taps, first);
                windowStart = static_cast<double> (static_cast<std::int64_t> (t.sample) + first + lag);
            }
            // in the recorded sequence: output sample i ran the filter at filter time i - lag
            evs.push_back ({ place + static_cast<double> (lag), w.reg, w.even });
        }
    }
    double resAt = 0.0;
    for (const auto& e : evs)
        if (e.reg == 0x17)
            resAt = e.place;
    auto twin = [&] (double delta) {
        // two filters switched at the samples either side of the place; their linear outputs and low-pass values mixed by
        // where the place lies between them (the state is linear, so this is the mixed step of the rule); the output
        // function of the new parameters on the mixed low-pass value
        Filter fA, fB;
        for (Filter* f : { &fA, &fB })
        {
            f->prepare (m, rate, 0.5f);
            f->setCutoff (1024.0f);
            f->setResonance (0);
            f->setMode (0x1f);
        }
        double alpha = 0.0;
        int newRes = -1;
        for (const auto& e : evs)
            if (e.reg == 0x17)
                newRes = e.value >> 4;
        // the output equation of the new parameters at a filter's state (a copy given the new resonance; nothing of
        // the filter advances): the voice's output from the write's window on
        auto newOut = [&] (const Filter& f, double x, double& lp) {
            Filter c = f;
            c.setResonance (newRes);
            return c.processLinearDouble (x, lp);
        };
        std::vector<double> y (in.size());
        for (int k = 0; k < total; ++k)
        {
            for (const auto& e : evs)
            {
                if (e.reg == 0x16 && static_cast<int> (std::llround (e.place)) == k)
                {
                    fA.setCutoff (static_cast<float> (e.value << 3));
                    fB.setCutoff (static_cast<float> (e.value << 3));
                }
                if (e.reg == 0x17)
                {
                    const double pl = e.place + delta;
                    if (static_cast<int> (std::floor (pl)) == k)
                    {
                        fA.setResonance (e.value >> 4);
                        alpha = pl - std::floor (pl);
                    }
                    if (static_cast<int> (std::floor (pl)) + 1 == k)
                        fB.setResonance (e.value >> 4);
                }
            }
            double lpA = 0.0, lpB = 0.0;
            const double x = in[static_cast<std::size_t> (k)];
            if (static_cast<double> (k) >= windowStart && newRes >= 0 && (fA.resonance() != newRes || fB.resonance() != newRes))
            {
                // inside the window, before both filters carry the new resonance: the new output equation at each state
                const double oA = newOut (fA, x, lpA), oB = newOut (fB, x, lpB);
                const double lin = oA + alpha * (oB - oA), lp = lpA + alpha * (lpB - lpA);
                Filter c = fB;
                c.setResonance (newRes);
                y[static_cast<std::size_t> (k)] = lin + c.outputShape (lp);
                double unused = 0.0;
                fA.processLinearDouble (x, unused);
                fB.processLinearDouble (x, unused);
                continue;
            }
            const double linA = fA.processLinearDouble (x, lpA);
            const double linB = fB.processLinearDouble (x, lpB);
            const double lin = linA + alpha * (linB - linA), lp = lpA + alpha * (lpB - lpA);
            y[static_cast<std::size_t> (k)] = lin + fB.outputShape (lp);
        }
        return y;
    };
    const int from = static_cast<int> (windowStart), to = static_cast<int> (resAt) + 600;
    auto rms = [&] (const std::vector<double>& y) {
        double e = 0.0;
        for (int k = from; k < to; ++k)
        {
            const double d = y[static_cast<std::size_t> (k)] - out[static_cast<std::size_t> (k)];
            e += d * d;
        }
        return std::sqrt (e / (to - from));
    };
    PlaceFit f;
    {
        // the change the write makes: the twin without the resonance's dynamics change against the voice
        double e = 0.0;
        const auto y = twin (1e6);
        for (int k = from; k < to; ++k)
            e += (y[static_cast<std::size_t> (k)] - out[static_cast<std::size_t> (k)]) * (y[static_cast<std::size_t> (k)] - out[static_cast<std::size_t> (k)]);
        f.change = std::sqrt (e / (to - from));
    }
    f.rmsAtZero = rms (twin (0.0));
    f.rmsAtTwentieth = rms (twin (0.05));
    f.rmsAtOne = rms (twin (1.0));
    double best = 1e300;
    for (int d = -80; d <= 80; ++d)
    {
        const double delta = 0.05 * d;
        const double r = rms (twin (delta));
        if (r < best)
        {
            best = r;
            f.delta = delta;
        }
    }
    f.rmsAtBest = best;
    return f;
}
} // namespace

TEST_CASE ("chain: the place of a resonance write's dynamics, alone and with a cutoff write 30 cycles before, at or after it (0.1 samples)", "[chain][alias][writes][place]")
{
    // 8.19.4 after the third review: the off-line measure cannot see a place error that repeats at every write, so the
    // place is measured by itself (placeFit, from the write's window on, so a cutoff missing from the newest output
    // parameters inside the window fails too): the twin's best delta must be within 0.1 samples of 0 (the grid is 0.05),
    // and at delta 0 the twin must equal the voice to 1e-9 of the write's change (the rule exactly). One sample late
    // (delta 1) leaves a mismatch of the order of the change: fails both. A cutoff write (1024 -> 1280) 30 cycles before,
    // at or 30 cycles after the resonance write lands from its own sample on in the twin; the voice must agree (the
    // third review found it up to 8 samples late inside a resonance window). Both models, 88.2, 96 and 192 kHz.
    struct C
    {
        const char* name;
        std::vector<TickWrite> ws;
    };
    const C cases[] { { "resonance alone", { { 0, 0x17, 0xf1, 0xf1 } } },
                      { "cutoff 30 cycles before", { { 0, 0x16, 0xa0, 0xa0 }, { 30, 0x17, 0xf1, 0xf1 } } },
                      { "cutoff at the same cycle", { { 0, 0x16, 0xa0, 0xa0 }, { 0, 0x17, 0xf1, 0xf1 } } },
                      { "cutoff 30 cycles after", { { 0, 0x17, 0xf1, 0xf1 }, { 30, 0x16, 0xa0, 0xa0 } } } };
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 88200.0, 96000.0, 192000.0 })
            for (const auto& c : cases)
            {
                const auto f = placeFit (m, rate, c.ws);
                std::printf ("[res-place] Model %s %6.0f Hz %-26s best delta %+.2f samples; rms at 0 %.1e, at 0.05 %.1e, at 1 %.1e; the change %.2e\n", m == Model::A ? "A" : "B", rate,
                             c.name, f.delta, f.rmsAtZero, f.rmsAtTwentieth, f.rmsAtOne, f.change);
                INFO ((m == Model::A ? "Model A " : "Model B ") << rate << " Hz, " << c.name << ": best delta " << f.delta << ", rms at 0 " << f.rmsAtZero << " of a change " << f.change);
                CHECK (std::abs (f.delta) <= 0.1);
                CHECK (f.rmsAtZero <= 1e-9 * f.change);
                CHECK (f.change > 1e-4);
            }
}

namespace
{
/** A write at an absolute chip cycle (dynamics twin tests). */
struct AtWrite
{
    std::uint64_t cycle;
    std::uint8_t reg, value;
};

/** The documented capacities (ChipVoice.h "TIMING"), restated here so a change of either is a change of this test. */
constexpr int kDocPending = 32, kDocStepMix = 4;

struct TwinResult
{
    double maxDiff = 0.0, peak = 0.0;
    int changes = 0, maxPending = 0, overflowed = 0, maxInOneSample = 0;
    std::uint64_t queueOverflows = 0;
};

/** The voice (Per Note, a saw at 2 kHz through the low-pass at 1024, board filters off) with resonance writes at the
    given cycles, rendered one sample at a time (ChipVoice::lastFilterInput / lastFilterOutput), against a twin that runs
    the documented rule on the recorded input: each resonance change begins at the first sample of its write's window
    (Chip::unitStepTaps; unchanged values begin nothing) and waits for its place (the centre of the window's weights);
    at most kDocPending wait, and a further one first applies the oldest at once; the step of a sample holding places is
    mixed from the parameter sets in turn (at most kDocStepMix; beyond, the last kept one takes the newest resonance);
    the output is the newest begun resonance's output equation on the one state. */
TwinResult dynamicsTwin (Model m, double rate, const std::vector<AtWrite>& ws, int samples)
{
    PerNote p (m, rate, true);
    ChipVoice& v = *p.v;
    v.boardFiltersEnabled (false);
    osc (v, 0, 2000.0 * 16777216.0 / 985248.0, 2048.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
    v.write (0x15, 0);
    v.write (0x16, 0x80);
    v.write (0x17, 0x01);
    v.write (0x18, 0x1f);
    std::vector<double> in (static_cast<std::size_t> (samples)), out (in.size());
    struct Ev
    {
        std::int64_t begin;
        double place;
        int res;
    };
    std::vector<Ev> evs;
    const std::int64_t lag = v.filterLagSamples();
    std::size_t next = 0;
    double one[1];
    for (int i = 0; i < samples;)
    {
        const std::uint64_t at = next < ws.size() ? ws[next].cycle : ~std::uint64_t { 0 };
        one[0] = 0.0;
        if (v.renderUntil (one, 1, at) == 1)
        {
            in[static_cast<std::size_t> (i)] = v.lastFilterInput();
            out[static_cast<std::size_t> (i)] = v.lastFilterOutput();
            ++i;
        }
        while (next < ws.size() && v.chip().cycle() == ws[next].cycle)
        {
            const auto& w = ws[next++];
            v.write (w.reg, w.value);
            const Chip::EventTime t = v.chip().renderEventTime();
            std::array<double, vs::bside::chip::render::kKernelTaps> taps {};
            int first = 0;
            v.chip().unitStepTaps (t.base, taps, first);
            // in the recorded sequence: output sample i ran the filter at filter time i - lag
            evs.push_back ({ static_cast<std::int64_t> (t.sample) + first + lag, switchRuleCentre (v.chip(), t) + static_cast<double> (lag), w.value >> 4 });
        }
    }
    TwinResult r;
    r.queueOverflows = v.queueOverflows();
    Filter dyn;
    dyn.prepare (m, rate, 0.5f);
    dyn.setCutoff (1024.0f);
    dyn.setResonance (0);
    dyn.setMode (0x1f);
    Filter outP = dyn;
    int begun = 0; // the newest begun resonance
    struct Pending
    {
        std::int64_t at;
        double alpha;
        int res;
    };
    std::vector<Pending> pending;
    std::size_t nb = 0;
    for (int k = 0; k < samples; ++k)
    {
        while (nb < evs.size() && evs[nb].begin <= k)
        {
            const auto& e = evs[nb++];
            if (e.res == begun)
                continue;
            begun = e.res;
            outP.setResonance (begun);
            if (static_cast<int> (pending.size()) == kDocPending)
            {
                dyn.setResonance (pending.front().res);
                pending.erase (pending.begin());
                ++r.overflowed;
            }
            const double fl = std::floor (e.place);
            pending.push_back ({ static_cast<std::int64_t> (fl), e.place - fl, e.res });
            ++r.changes;
            r.maxPending = std::max (r.maxPending, static_cast<int> (pending.size()));
        }
        const double x = in[static_cast<std::size_t> (k)];
        double lp = 0.0;
        const double lin = outP.peekLinearDouble (x, dyn, lp);
        const double y = lin + outP.outputShape (lp);
        r.maxDiff = std::max (r.maxDiff, std::abs (y - out[static_cast<std::size_t> (k)]));
        r.peak = std::max (r.peak, std::abs (y));
        std::vector<double> alpha;
        std::vector<int> res;
        int inSample = 0;
        while (! pending.empty() && pending.front().at <= k)
        {
            const auto pe = pending.front();
            pending.erase (pending.begin());
            ++inSample;
            if (pe.at < k)
                dyn.setResonance (pe.res);
            else if (static_cast<int> (alpha.size()) == kDocStepMix)
                res.back() = pe.res;
            else
            {
                alpha.push_back (pe.alpha);
                res.push_back (pe.res);
            }
        }
        r.maxInOneSample = std::max (r.maxInOneSample, inSample);
        if (alpha.empty())
            dyn.processLinearDouble (x, lp);
        else
        {
            std::array<double, 2> mix { 0.0, 0.0 };
            Filter f = dyn;
            double before = 0.0;
            for (std::size_t j = 0; j <= alpha.size(); ++j)
            {
                if (j > 0)
                    f.setResonance (res[j - 1]);
                Filter g = f;
                const double until = j < alpha.size() ? std::max (alpha[j], before) : 1.0;
                g.processLinearDouble (x, lp);
                const auto sj = g.state();
                mix[0] += (until - before) * sj[0];
                mix[1] += (until - before) * sj[1];
                before = until;
            }
            dyn.setResonance (res.back());
            dyn.setState (mix);
        }
    }
    return r;
}
} // namespace

TEST_CASE ("chain: resonance changes in combination follow the documented rule exactly: several in one sample, two writes 0 to 10 cycles apart, the pending capacity at and beyond its limit", "[chain][writes][place][capacity]")
{
    // ChipVoice.h "TIMING" (8.19.4 after the third review) states the rule for resonance changes in combination; a twin
    // (dynamicsTwin) runs it on the voice's own filter input and must equal the voice's filter output to 1e-12 of its
    // peak, sample by sample. Cases (each also checked to exercise what it names):
    // - two resonance writes 0, 1, 3 and 10 cycles apart: the same value twice, two different values, and back to the
    //   old value (the fourth review's M8: a wrong final resonance after two changes in one sample passed every test);
    // - k changes placed in one sample with distinct places, k = 4 (the limit kMaxStepMix: every one mixed) and 5, 6
    //   (beyond: the last kept one takes the newest resonance);
    // - kMaxPending changes waiting at once (32, the limit: none applied early) and 33 (beyond: the oldest applied at once
    //   when the 33rd begins), Model B (on Model A the write queue of the same size fills first at that density: its
    //   bus pre-filter keeps writes waiting 21 samples longer; the write queue's own rule is tested in "a full write
    //   queue").
    auto check = [] (Model m, double rate, const std::vector<AtWrite>& ws, const char* what, int samples = 1500) {
        const auto r = dynamicsTwin (m, rate, ws, samples);
        INFO ((m == Model::A ? "Model A " : "Model B ") << rate << " Hz, " << what << ": max |voice - twin| " << r.maxDiff << " of peak " << r.peak << ", changes "
                                                       << r.changes << ", most waiting " << r.maxPending << ", applied early " << r.overflowed << ", most in one sample "
                                                       << r.maxInOneSample);
        CHECK (r.maxDiff <= 1e-12 * r.peak);
        CHECK (r.queueOverflows == 0u);
        return r;
    };
    const std::uint64_t c0 = 6000;
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 88200.0, 96000.0, 192000.0 })
            for (const std::uint64_t gap : { 0u, 1u, 3u, 10u })
            {
                check (m, rate, { { c0, 0x17, 0xf1 }, { c0 + gap, 0x17, 0xf1 } }, "two writes, the same value");
                const auto d = check (m, rate, { { c0, 0x17, 0x81 }, { c0 + gap, 0x17, 0xf1 } }, "two writes, two values");
                CHECK (d.changes == 2);
                const auto b = check (m, rate, { { c0, 0x17, 0xf1 }, { c0 + gap, 0x17, 0x01 } }, "two writes, back to the old value");
                CHECK (b.changes == 2);
                if (gap == 0)
                    CHECK (b.maxInOneSample == 2);
            }
    // k changes placed in one sample, distinct places: writes 2 cycles apart at 88.2 kHz (11.2 cycles per sample),
    // shifted until all k places fall into one sample
    for (const Model m : { Model::B, Model::A })
        for (const int k : { 4, 5, 6 })
        {
            int found = 0;
            for (std::uint64_t shift = 0; shift < 12 && found == 0; ++shift)
            {
                std::vector<AtWrite> ws;
                for (int j = 0; j < k; ++j)
                    ws.push_back ({ c0 + shift + 2 * static_cast<std::uint64_t> (j), 0x17, static_cast<std::uint8_t> (((3 * j + 2) & 15) << 4 | 1) });
                const auto r = dynamicsTwin (m, 88200.0, ws, 1500);
                if (r.maxInOneSample == k)
                {
                    found = k;
                    check (m, 88200.0, ws, k == 4 ? "four changes in one sample (the limit)" : "more changes in one sample than the limit");
                }
            }
            INFO ((m == Model::A ? "Model A, " : "Model B, ") << k << " changes: a shift placing them all in one sample");
            CHECK (found == k);
        }
    // the pending capacity: Model B, 96 kHz, a change every 2 cycles (all begun before the first one's place)
    for (const int count : { kDocPending, kDocPending + 1 })
    {
        std::vector<AtWrite> ws;
        for (int j = 0; j < count; ++j)
            ws.push_back ({ c0 + 2 * static_cast<std::uint64_t> (j), 0x17, static_cast<std::uint8_t> ((j % 2) == 0 ? 0xf1 : 0x01) });
        const auto r = check (Model::B, 96000.0, ws, count == kDocPending ? "32 changes waiting (the limit)" : "33 changes (one beyond)");
        CHECK (r.maxPending == kDocPending);
        CHECK (r.overflowed == (count > kDocPending ? 1 : 0));
    }
}

TEST_CASE ("chain: routing, volume and mode writes every tick meet the write budget before the board filters (<= -90 dB to 2 kHz)", "[chain][alias][writes]")
{
    struct Row
    {
        const char* name;
        bool vdc;
        std::uint8_t reg, a, b, control, route, modeVolume;
    };
    const Row rows[] { { "routing filter <-> direct (saw)", true, 0x17, 0x40, 0x41, ctrl::saw, 0x00, 0x1f },
                       { "routing filter <-> direct (pulse)", false, 0x17, 0x40, 0x41, ctrl::pulse, 0x00, 0x1f },
                       { "oscillator 3 mute (saw on 3)", true, 0x18, 0x1f, 0x9f, 0, 0x00, 0x1f },
                       { "volume 15 <-> 6, volume DC on (the chip's step)", true, 0x18, 0x1f, 0x16, ctrl::saw, 0x01, 0x1f },
                       { "volume 15 <-> 6, volume DC off", false, 0x18, 0x1f, 0x16, ctrl::saw, 0x01, 0x1f },
                       { "mode low-pass <-> high-pass", true, 0x18, 0x1f, 0x4f, ctrl::saw, 0x01, 0x1f },
                       { "mode low-pass <-> band-pass, volume DC off", false, 0x18, 0x1f, 0x2f, ctrl::triangle, 0x01, 0x1f } };
    // (resonance writes: the next test, against an ideal render; 8.19.4 after the round over phases)
    for (const Model m : { Model::B, Model::A })
        for (const auto& row : rows)
        {
            double worst = -300.0, worstRaw = -300.0, worstBase = -300.0;
            for (const RateConfig rc : { RateConfig { 88200.0, 20000.0 }, RateConfig { 96000.0, 20000.0 }, RateConfig { 192000.0, 20000.0 } })
            {
                for (const double hz : { 110.0, 1000.0, 2000.0 })
                {
                    // the mute row plays a saw on oscillator 3 (routed direct), the others on oscillator 1
                    const auto a = chainWrites (m, rc, hz, row.vdc, row.reg, row.a, row.b, row.control == 0 ? std::uint8_t { ctrl::saw } : row.control, row.route,
                                                row.modeVolume, row.control == 0 ? 2 : 0);
                    INFO ((m == Model::A ? "Model A, " : "Model B, ") << row.name << ", " << rc.internalRate << " Hz internal, " << hz << " Hz: the writes add "
                                                                     << a.write.worstDb << " dB at " << a.write.worstHz << " Hz (output " << a.raw.worstDb
                                                                     << " dB, held states " << a.baseline.worstDb << " dB)");
                    CHECK (a.write.worstDb <= -90.0);
                    CHECK (a.raw.worstDb > -299.0); // something was measured: bins exist between the lines
                    worst = std::max (worst, a.write.worstDb);
                    worstRaw = std::max (worstRaw, a.raw.worstDb);
                    worstBase = std::max (worstBase, a.baseline.worstDb);
                }
            }
            WARN ((m == Model::A ? "Model A " : "Model B ") << row.name << " every tick: the writes add at worst " << worst << " dB (budget -90); the output "
                                                            << worstRaw << " dB, held without writes " << worstBase << " dB (the static curves, S3b-2)");
        }
}

TEST_CASE ("chain: a volume write with volume DC on is the chip's own step: band-limited, at its cycle, of its full size", "[chain][writes]")
{
    // Silent voices (sample playback, 8.4.4): the only signal is the mixer's DC, which the volume scales. Before the
    // board filters, the output with a volume write at cycle W must be the output with the old volume plus (new - old)
    // times the long kernel's band-limited step at W - kReferenceLeadCycles (the reference's convention, see there;
    // the latency late): its full size (nothing smoothed away), at that cycle (one cycle earlier, or the oscillator
    // writes' convention one cycle later, is far worse), band-limited. Both models, three rates. Changed 2026-11-23:
    // the step was expected at W itself (the first build's convention, about one cycle after the reference's).
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 88200.0, 96000.0, 192000.0 })
        {
            auto run = [&] (std::uint8_t from, std::uint8_t to, std::uint64_t w) {
                PerNote p (m, rate, true);
                p.v->boardFiltersEnabled (false);
                p.v->write (0x18, from);
                const int total = static_cast<int> (rate * 0.05);
                std::vector<double> y (static_cast<std::size_t> (total), 0.0);
                int done = p.v->renderUntil (y.data(), total, w);
                p.v->write (0x18, to);
                done += p.v->renderUntil (y.data() + done, total - done, ~std::uint64_t { 0 });
                return y;
            };
            const std::uint64_t w = 20011;
            const auto before = run (0x1f, 0x1f, w), after = run (0x16, 0x16, w), step = run (0x1f, 0x16, w);
            const double latency = static_cast<double> (PerNote (m, rate, true).v->latencySamples());
            const double cps = static_cast<double> (makeChip (m, rate).sampleClock().increment()) / 4294967296.0;
            auto blepCum = [] (double t) {
                constexpr double z = Chip::LongBlep::kZeroCrossings;
                if (t <= -z)
                    return 0.0;
                if (t > z)
                    return 1.0;
                const int j = static_cast<int> (std::ceil (t)) - 1;
                return Chip::LongBlep::get().step (j, t - j) + (j >= 0 ? 1.0 : 0.0);
            };
            auto deviation = [&] (double shiftCycles) {
                double worst = 0.0;
                const double pos = (static_cast<double> (w) - kReferenceLeadCycles + shiftCycles) / cps + latency;
                for (std::size_t s = 100; s < step.size(); ++s)
                {
                    const double expected = before[s] + (after[s] - before[s]) * blepCum (static_cast<double> (s) - pos);
                    worst = std::max (worst, std::abs (step[s] - expected));
                }
                return worst;
            };
            const double size = after[after.size() - 1] - before[before.size() - 1];
            const double exact = deviation (0.0), early = deviation (-1.0), late = deviation (1.0);
            INFO ((m == Model::A ? "Model A" : "Model B") << ", " << rate << " Hz: step " << size << ", deviation " << exact << " (one cycle early " << early
                                                          << ", late (the oscillator writes' convention) " << late << ")");
            CHECK (std::abs (size) > 0.01);
            CHECK (exact < 1.0e-6 * std::abs (size) + 1.0e-12);
            CHECK (early > 100.0 * exact);
            CHECK (late > 100.0 * exact);
        }
}

namespace
{
struct SharedResult
{
    double dcStep = 0.0, rms = 0.0;
};

/** n identical voices (oscillator 1 a 110 Hz triangle at full envelope, routed to the filter, cutoff 900, all three
    modes), Per Note (each its own filter) or Shared (one filter, OutputStage::sharedShare): the summed output's DC
    step on a volume write 0 -> 15 (volume DC on) and the rms of its audio at volume 15, after 1 s. */
SharedResult sharedChips (Model m, int n, bool shared)
{
    const double rate = 96000.0;
    SharedResult res;
    for (const bool audio : { false, true })
    {
        std::vector<std::unique_ptr<ChipVoice>> v;
        std::vector<std::unique_ptr<Filter>> f;
        for (int k = 0; k < n; ++k)
        {
            vs::bside::chip::Config cfg;
            cfg.model = m;
            cfg.internalRate = rate;
            cfg.volumeDc = true;
            v.push_back (std::make_unique<ChipVoice>());
            v.back()->prepare (cfg);
            if (! shared || k == 0)
            {
                f.push_back (std::make_unique<Filter>());
                f.back()->prepare (m, rate, 0.5f);
            }
            if (! shared)
                v.back()->setFilter (f.back().get());
        }
        Filter& sf = *f[0];
        sf.setCutoff (900.0f);
        sf.setMode (0x70);
        for (auto& x : v)
        {
            osc (*x, 0, 110.0 * 16777216.0 / 985248.0, 2048.0f, ctrl::triangle | ctrl::gate);
            x->write (0x15, 900 & 7);
            x->write (0x16, 900 >> 3);
            x->write (0x17, 0x01);
            x->write (0x18, audio ? 0x7f : 0x70);
        }
        std::vector<double> out;
        auto run = [&] (int samples) {
            std::array<double, ChipVoice::kBlock> so {}, sl {}, ss {}, y {};
            std::array<Filter::Shape, ChipVoice::kBlock> sh {};
            int done = 0;
            while (done < samples)
            {
                const int want = std::min (ChipVoice::kBlock, samples - done);
                y.fill (0.0);
                if (! shared)
                    for (auto& x : v)
                        x->renderUntil (y.data(), want, ~std::uint64_t { 0 });
                else
                {
                    for (auto& x : v)
                        x->renderBusesUntil (want, ~std::uint64_t { 0 });
                    for (int i = 0; i < want; ++i)
                    {
                        const double dc = v[0]->staticFilterBusDc (i);
                        double in = dc;
                        for (auto& x : v)
                            in += x->filterInputAc (i);
                        so[static_cast<std::size_t> (i)] = sf.processLinearDouble (in, sl[static_cast<std::size_t> (i)]);
                        ss[static_cast<std::size_t> (i)] = sf.staticOutput (dc);
                        sh[static_cast<std::size_t> (i)] = sf.outputShapeCoefficients();
                    }
                    for (auto& x : v)
                        x->finishShared (sh.data(), so.data(), sl.data(), ss.data(), n, y.data(), want);
                }
                out.insert (out.end(), y.begin(), y.begin() + want);
                done += want;
            }
        };
        run (static_cast<int> (rate));
        out.clear();
        const int span = static_cast<int> (10.0 / 110.0 * rate);
        run (span);
        double mean = 0.0, sq = 0.0;
        for (const double s : out)
        {
            mean += s;
            sq += s * s;
        }
        mean /= static_cast<double> (out.size());
        if (audio)
        {
            res.rms = std::sqrt (sq / static_cast<double> (out.size()) - mean * mean);
            continue;
        }
        for (auto& x : v)
            x->write (0x18, 0x7f);
        out.clear();
        run (span);
        double after = 0.0;
        for (const double s : out)
            after += s;
        res.dcStep = after / static_cast<double> (out.size()) - mean;
    }
    return res;
}
} // namespace

TEST_CASE ("chain: the Shared topology with 1, 2 and 16 real chips against Per Note (Model B equal, Model A as OutputStage.h pins it)", "[chain][shared]")
{
    // OutputStage.h "SHARED" through ChipVoice with real chips (8.19.4): Model B, n Shared chips give the output of n Per
    // Note chips (the DC step of a volume write within 0.05 dB, the audio's rms within 1 %), n = 1, 2, 16; Model A, the
    // pinned property of OutputStage.h (n = 2 / 4 / 8 / 16: DC step -0.1 / +0.6 / +1.9 / +2.7 dB +-0.3 from Per Note,
    // rms 1.01 / 0.72 / 0.37 / 0.20 +-0.05 of it); n = 1 is Per Note exactly for both.
    for (const Model m : { Model::B, Model::A })
        for (const int n : { 1, 2, 4, 8, 16 })
        {
            if (m == Model::B && (n == 4 || n == 8))
                continue;
            const auto pn = sharedChips (m, n, false), sh = sharedChips (m, n, true);
            const double dcDb = 20.0 * std::log10 (sh.dcStep / pn.dcStep), rmsRatio = sh.rms / pn.rms;
            std::printf ("[shared] chain model %s chips %2d: DC step per note %+.4f, shared %+.4f (%+.3f dB); rms ratio %.4f\n", m == Model::A ? "A" : "B", n,
                         pn.dcStep, sh.dcStep, dcDb, rmsRatio);
            INFO ((m == Model::A ? "Model A" : "Model B") << ", " << n << " chips: DC step " << dcDb << " dB, rms ratio " << rmsRatio);
            REQUIRE (std::abs (pn.dcStep) > 1e-3);
            REQUIRE (pn.rms > 1e-3);
            if (n == 1 || m == Model::B)
            {
                CHECK (std::abs (dcDb) < 0.05);
                CHECK (std::abs (rmsRatio - 1.0) < 0.01);
            }
            else
            {
                const double documented = n == 2 ? -0.1 : (n == 4 ? 0.6 : (n == 8 ? 1.9 : 2.7));
                const double documentedRms = n == 2 ? 1.01 : (n == 4 ? 0.72 : (n == 8 ? 0.37 : 0.20));
                CHECK (std::abs (dcDb - documented) <= 0.3);
                CHECK (std::abs (rmsRatio - documentedRms) <= 0.05);
            }
        }
}

// ---- after the independent review of S3b-1 (2026-11-23; docs/architecture.md 8.19.4 "After the independent review") ----

namespace
{
struct PartWrite
{
    int afterSample; // written to every chip once this many output samples are complete
    std::uint8_t reg, value;
};

struct PartSetup
{
    Model m = Model::B;
    bool vdc = true;
    int n = 1;
    bool osc3 = true;        // oscillator 3 sounds too (a saw, on the direct bus unless routed)
    std::uint8_t route = 0x43; // 0x17 of every chip: resonance 4, oscillators 1 and 2 to the filter
    std::vector<PartWrite> writes;
    int samples = 16000;
    // Shared through finishSharedWeighted (docs/architecture.md 8.21.1 after the review): chip k's weight at output
    // sample i (null: finishShared with the count n)
    double (*weight) (int chip, int chips, int sample) = nullptr;
    bool othersSilent = false; // chips 1 .. n - 1 keep their gates closed (no sound of their own)
};

/** n chips at 96 kHz, each on its own notes (chip k: oscillator 1 a saw, triangle or pulse a fifth-spaced step above
    110 Hz, oscillator 2 the same waveform at 1.5 x, oscillator 3 a saw at 2.25 x), low-pass at 900, resonance 4,
    volume 15; Per Note (each chip its own Filter) or Shared (one Filter run by this owner as ChipVoice.h "TOPOLOGY"
    says: per bus sample one chip's static DC plus every chip's filterInputAc in, and the static output of that DC
    handed over per sample), rendered in owner blocks of blockLen samples; the summed output. */
std::vector<double> renderPart (const PartSetup& s, bool shared, int blockLen)
{
    const double rate = 96000.0;
    std::vector<std::unique_ptr<ChipVoice>> v;
    std::vector<std::unique_ptr<Filter>> f;
    for (int k = 0; k < s.n; ++k)
    {
        vs::bside::chip::Config cfg;
        cfg.model = s.m;
        cfg.internalRate = rate;
        cfg.volumeDc = s.vdc;
        v.push_back (std::make_unique<ChipVoice>());
        v.back()->prepare (cfg);
        if (! shared || k == 0)
        {
            f.push_back (std::make_unique<Filter>());
            f.back()->prepare (s.m, rate, 0.5f);
        }
        if (! shared)
            v.back()->setFilter (f.back().get());
    }
    Filter& sf = *f[0];
    if (shared)
    {
        sf.setCutoff (900.0f);
        sf.setResonance (4);
        sf.setMode (0x1f);
    }
    for (int k = 0; k < s.n; ++k)
    {
        ChipVoice& x = *v[static_cast<std::size_t> (k)];
        const double hz = 110.0 * std::pow (2.0, static_cast<double> (k * 5 % 19) / 12.0);
        const auto wave = static_cast<std::uint8_t> (k % 3 == 0 ? ctrl::saw : (k % 3 == 1 ? ctrl::triangle : ctrl::pulse));
        const std::uint8_t gate = s.othersSilent && k > 0 ? std::uint8_t { 0 } : ctrl::gate;
        osc (x, 0, hz * 16777216.0 / 985248.0, 1500.0f + 100.0f * static_cast<float> (k), static_cast<std::uint8_t> (wave | gate));
        osc (x, 1, 1.5 * hz * 16777216.0 / 985248.0, 1500.0f + 100.0f * static_cast<float> (k), static_cast<std::uint8_t> (wave | gate));
        if (s.osc3)
            osc (x, 2, 2.25 * hz * 16777216.0 / 985248.0, 0.0f, static_cast<std::uint8_t> (ctrl::saw | gate));
        x.write (0x15, 900 & 7);
        x.write (0x16, 900 >> 3);
        x.write (0x17, s.route);
        x.write (0x18, 0x1f);
    }
    std::vector<double> out;
    std::array<double, ChipVoice::kBlock> so {}, sl {}, ss {}, y {};
            std::array<Filter::Shape, ChipVoice::kBlock> sh {};
    std::size_t next = 0;
    int done = 0;
    while (done < s.samples)
    {
        int want = std::min ({ blockLen, ChipVoice::kBlock, s.samples - done });
        if (next < s.writes.size())
            want = std::min (want, s.writes[next].afterSample - done);
        y.fill (0.0);
        if (want > 0)
        {
            if (! shared)
                for (auto& x : v)
                    x->renderUntil (y.data(), want, ~std::uint64_t { 0 });
            else
            {
                for (auto& x : v)
                    x->renderBusesUntil (want, ~std::uint64_t { 0 });
                for (int i = 0; i < want; ++i)
                {
                    const double dc = v[0]->staticFilterBusDc (i);
                    double in = dc;
                    for (auto& x : v)
                        in += x->filterInputAc (i);
                    so[static_cast<std::size_t> (i)] = sf.processLinearDouble (in, sl[static_cast<std::size_t> (i)]);
                    ss[static_cast<std::size_t> (i)] = sf.staticOutput (dc);
                    sh[static_cast<std::size_t> (i)] = sf.outputShapeCoefficients();
                }
                if (s.weight == nullptr)
                    for (auto& x : v)
                        x->finishShared (sh.data(), so.data(), sl.data(), ss.data(), s.n, y.data(), want);
                else
                    for (int k = 0; k < s.n; ++k)
                    {
                        std::array<double, ChipVoice::kBlock> w {};
                        for (int i = 0; i < want; ++i)
                            w[static_cast<std::size_t> (i)] = s.weight (k, s.n, done + i);
                        v[static_cast<std::size_t> (k)]->finishSharedWeighted (sh.data(), so.data(), sl.data(), ss.data(), w.data(), y.data(), want);
                    }
            }
            out.insert (out.end(), y.begin(), y.begin() + want);
            done += want;
        }
        while (next < s.writes.size() && s.writes[next].afterSample == done)
        {
            for (auto& x : v)
                x->write (s.writes[next].reg, s.writes[next].value);
            ++next;
        }
    }
    return out;
}

double maxAbsDiff (const std::vector<double>& a, const std::vector<double>& b)
{
    double w = a.size() == b.size() ? 0.0 : 1.0e300;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        w = std::max (w, std::abs (a[i] - b[i]));
    return w;
}
} // namespace

TEST_CASE ("chain: Shared is exact per sample (one chip is Per Note bit for bit; any owner block length; routing, mute and volume writes)", "[chain][shared]")
{
    // 8.19.4 "After the independent review": the static output the stages need is taken per bus sample. Routing
    // writes (oscillator 1 to the direct bus and back, oscillators 1 and 3 to the filter), oscillator 3's mute on and
    // off and a volume write, each inside an owner block (the output is the latency later); volume DC on and off; both
    // models. n = 1: Shared equals Per Note bit for bit for owner blocks of 64, 17 and 1 samples. n = 2 (different
    // notes): Shared is the same bits for every owner block length. (The first build took one static output per owner
    // block: n = 1 was off by up to 0.03 (Model B) and 0.15 (Model A) after a routing write, volume DC off.)
    const std::vector<PartWrite> writes { { 3000, 0x17, 0x41 }, { 5017, 0x18, 0x9f }, { 7033, 0x17, 0x45 },
                                          { 9001, 0x18, 0x1f }, { 11000, 0x18, 0x19 }, { 13000, 0x17, 0x43 } };
    for (const Model m : { Model::B, Model::A })
        for (const bool vdc : { false, true })
        {
            PartSetup s;
            s.m = m;
            s.vdc = vdc;
            s.writes = writes;
            s.n = 1;
            const auto pn = renderPart (s, false, 64);
            PartSetup quiet = s;
            quiet.writes.clear();
            INFO ((m == Model::A ? "Model A" : "Model B") << ", volume DC " << (vdc ? "on" : "off"));
            CHECK (maxAbsDiff (pn, renderPart (quiet, false, 64)) > 1.0e-3); // the writes change the output
            for (const int block : { 64, 17, 1 })
            {
                const auto sh = renderPart (s, true, block);
                INFO ("n = 1, owner block " << block << ": largest difference from Per Note " << maxAbsDiff (sh, pn));
                CHECK (sameBits (sh, pn));
            }
            s.n = 2;
            const auto a = renderPart (s, true, 64);
            for (const int block : { 17, 1 })
            {
                const auto b = renderPart (s, true, block);
                INFO ("n = 2, owner block " << block << " against 64: largest difference " << maxAbsDiff (a, b));
                CHECK (sameBits (a, b));
            }
        }
}

TEST_CASE ("chain: Shared by weight (finishSharedWeighted): weight 1 is Per Note bit for bit, 1 / n is finishShared, 0 is the chip's own output", "[chain][shared]")
{
    // The part's entry point for shares that move when a chip joins or leaves the sum (docs/architecture.md 8.21.1 after
    // the independent review): the owner gives each chip its weight per bus sample instead of the whole count.
    const std::vector<PartWrite> writes { { 3000, 0x17, 0x41 }, { 5017, 0x18, 0x9f }, { 7033, 0x17, 0x45 },
                                          { 9001, 0x18, 0x1f }, { 11000, 0x18, 0x19 }, { 13000, 0x17, 0x43 } };
    for (const Model m : { Model::B, Model::A })
        for (const bool vdc : { false, true })
        {
            INFO ((m == Model::A ? "Model A" : "Model B") << ", volume DC " << (vdc ? "on" : "off"));
            PartSetup s;
            s.m = m;
            s.vdc = vdc;
            s.writes = writes;
            // (1) one chip, weight 1: Per Note bit for bit (and so finishShared (..., 1, ...)), any owner block
            s.n = 1;
            const auto pn = renderPart (s, false, 64);
            PartSetup one = s;
            one.weight = [] (int, int, int) { return 1.0; };
            for (const int block : { 64, 17, 1 })
                CHECK (sameBits (renderPart (one, true, block), pn));
            // (2) n chips at weight 1 / n: finishShared with the count n (a multiplication in place of a division: the
            // same to rounding)
            for (const int n : { 2, 16 })
            {
                PartSetup counted = s;
                counted.n = n;
                PartSetup weighted = counted;
                weighted.weight = [] (int, int chips, int) { return 1.0 / static_cast<double> (chips); };
                const auto a = renderPart (counted, true, 64), b = renderPart (weighted, true, 64);
                INFO ("n = " << n << ": weights 1 / n against the count: largest difference " << maxAbsDiff (a, b));
                CHECK (maxAbsDiff (a, b) <= 1.0e-11);
                // a weighted render does not depend on the owner's block length either
                CHECK (sameBits (renderPart (weighted, true, 17), b));
            }
        }
    // (3) weight 0 is the chip's own output, nothing of the shared filter's signal: two chips, the second with its
    // gates closed (no sound of its own; volume DC off, so its static level is 0 at the output). Chip 1 at weight 1 and
    // chip 2 at weight 0 give what chip 1 gives alone; with the output shared half and half they do not (each stage's
    // curve sees half the signal). And a share that moves from one chip to the other over 288 samples (the part's 3 ms
    // at 96 kHz) with the weights adding up to 1 stays between the two.
    for (const Model m : { Model::B, Model::A })
    {
        PartSetup alone;
        alone.m = m;
        alone.vdc = false;
        alone.samples = 72000;
        PartSetup two = alone;
        two.n = 2;
        two.othersSilent = true;
        PartSetup all = two, halves = two, moving = two;
        all.weight = [] (int chip, int, int) { return chip == 0 ? 1.0 : 0.0; };
        halves.weight = [] (int, int, int) { return 0.5; };
        moving.weight = [] (int chip, int, int sample) {
            const double t = std::clamp ((static_cast<double> (sample) - 60000.0) / 288.0, 0.0, 1.0); // 0 .. 1 over 288 samples
            return chip == 0 ? 1.0 - 0.5 * t : 0.5 * t;
        };
        const auto ref = renderPart (alone, true, 64);
        const auto a = renderPart (all, true, 64), h = renderPart (halves, true, 64), mv = renderPart (moving, true, 64);
        // from 0.5 s on: the second chip's power-on envelopes have run out and its board high-pass has let go of them
        // (a 133 ms time constant: what is left is the tolerance of the "own output" checks below)
        auto diff = [&] (const std::vector<double>& x, const std::vector<double>& y, std::size_t from, std::size_t to) {
            double w = 0.0;
            for (std::size_t i = from; i < to; ++i)
                w = std::max (w, std::abs (x[i] - y[i]));
            return w;
        };
        const double own = diff (a, ref, 48000, 60000), shared = diff (h, ref, 48000, 60000);
        const double before = diff (mv, ref, 48000, 60000), during = diff (mv, ref, 60000, 60000 + 288 + 64), after = diff (mv, h, 60000 + 288 + 64, 72000);
        double peak = 0.0;
        for (std::size_t i = 48000; i < ref.size(); ++i)
            peak = std::max (peak, std::abs (ref[i]));
        std::printf ("[shared] weights, Model %s: (1, 0) differs from one chip by %.3g, (1/2, 1/2) by %.3g; a share moving over 288 samples: %.3g before, %.3g during, "
                     "%.3g from the halves after (peak %.3g)\n", m == Model::A ? "A" : "B", own, shared, before, during, after, peak);
        INFO ((m == Model::A ? "Model A" : "Model B") << ": weights (1, 0) differ from one chip by " << own << ", halves by " << shared << "; moving: before " << before
                                                     << ", during " << during << ", after (from the halves) " << after << "; peak " << peak);
        REQUIRE (peak > 0.05);
        CHECK (shared > 1.0e-3 * peak); // sharing is not nothing: the test can see a share
        CHECK (own <= 0.01 * shared);
        CHECK (before <= 0.01 * shared);
        CHECK (during <= 1.2 * shared); // on its way from one to the other, never beyond
        // afterwards it is the half-and-half render (the board high-pass's memory of the way there decays)
        CHECK (diff (mv, h, 70000, 72000) <= 0.2 * shared);
    }
}

TEST_CASE ("chain: the Shared topology with different notes differs from Per Note by the documented amount (Model B, n = 2)", "[chain][shared]")
{
    // ChipVoice.h "TOPOLOGY" / OutputStage.h "SHARED": Model B's Shared equals Per Note for identical voices only. With
    // different notes each chip's mixer curve sees its share of the summed filter output: two chips, oscillators 1 and
    // 2 on different notes and waveforms, all to the filter, volume DC on; the rms of (Shared - Per Note) over the
    // second half of 0.5 s against Per Note's audio rms is pinned at the documented -38.8 dB (+-1 dB): a property of
    // the topology, not an error to shrink, and a change of it is a change of the chain.
    PartSetup s;
    s.m = Model::B;
    s.n = 2;
    s.osc3 = false;
    s.samples = 48000;
    const auto pn = renderPart (s, false, 64), sh = renderPart (s, true, 64);
    const std::size_t from = pn.size() / 2;
    double mean = 0.0;
    for (std::size_t i = from; i < pn.size(); ++i)
        mean += pn[i];
    mean /= static_cast<double> (pn.size() - from);
    double e = 0.0, p = 0.0;
    for (std::size_t i = from; i < pn.size(); ++i)
    {
        e += (sh[i] - pn[i]) * (sh[i] - pn[i]);
        p += (pn[i] - mean) * (pn[i] - mean);
    }
    const double db = 10.0 * std::log10 (e / p);
    std::printf ("[shared] chain model B chips 2, different notes: rms (Shared - Per Note) %.2f dB re Per Note's audio (documented -38.8)\n", db);
    REQUIRE (p > 1.0e-6);
    CHECK (std::abs (db - (-38.8)) <= 1.0);
}

TEST_CASE ("chain: with volume DC off the volume scales the output behind the board high-pass", "[chain][volume]")
{
    // OutputStage.h "volumeDc off": the volume multiplies the board high-pass's output (postGain). Two voices, the same
    // held saw on the direct bus, volume 15 and 6 set before the render: the outputs (after the board filters) are in
    // the ratio of the volume levels, Model B 6 / 15, Model A 0.4423 / 1 (decisions "B-Side S3a", its fitted table),
    // to rounding at every sample.
    for (const Model m : { Model::B, Model::A })
    {
        auto run = [m] (std::uint8_t volume) {
            PerNote p (m, 96000.0, false);
            osc (*p.v, 0, 220.0 * 16777216.0 / 985248.0, 2048.0f, ctrl::saw | ctrl::gate);
            p.v->write (0x17, 0x00);
            p.v->write (0x18, static_cast<std::uint8_t> (0x10 | volume));
            std::vector<double> y (28800, 0.0);
            p.v->render (y.data(), static_cast<int> (y.size()));
            return y;
        };
        const auto y15 = run (15), y6 = run (6);
        const double ratio = m == Model::B ? 6.0 / 15.0 : 0.4423;
        double worst = 0.0, peak = 0.0;
        for (std::size_t i = 0; i < y15.size(); ++i)
        {
            worst = std::max (worst, std::abs (y6[i] - ratio * y15[i]));
            peak = std::max (peak, std::abs (y15[i]));
        }
        INFO ((m == Model::A ? "Model A" : "Model B") << ": peak " << peak << ", largest deviation from the volume ratio " << worst);
        CHECK (peak > 0.01);
        CHECK (worst <= 1.0e-12 * peak);
    }
}

TEST_CASE ("chain: the static DC follows the voice counts with volume DC off (no step left behind a routing write)", "[chain][volume]")
{
    // OutputStage.h "volumeDc off": the static DC (mixer DC, the voices' DC on both paths) is subtracted at the mixer
    // curve's operating point, so the mixer's output before the board filters is 0 for silent voices (envelopes at 0)
    // once the filter has settled, whatever the routing. Three silent voices, routing 0 -> 1 -> 3 -> 7 -> 2 -> 0 every
    // 0.1 s (written between samples): the last 10 ms of each routing within 1e-9 of 0. Both models, low-pass at 1024.
    for (const Model m : { Model::B, Model::A })
    {
        PerNote p (m, 96000.0, false);
        ChipVoice& v = *p.v;
        v.boardFiltersEnabled (false);
        for (int n = 0; n < 3; ++n)
            osc (v, n, 3000.0 + 1000.0 * n, 2048.0f, ctrl::saw); // no gate: the envelopes stay at 0
        v.write (0x15, 0);
        v.write (0x16, 0x80);
        v.write (0x18, 0x1f);
        double worst = 0.0, worstStep = 0.0;
        double previous = 0.0;
        bool first = true;
        for (const std::uint8_t route : { std::uint8_t { 0 }, std::uint8_t { 1 }, std::uint8_t { 3 }, std::uint8_t { 7 }, std::uint8_t { 2 }, std::uint8_t { 0 } })
        {
            v.write (0x17, route);
            std::vector<double> y (9600, 0.0);
            v.render (y.data(), static_cast<int> (y.size()));
            for (std::size_t i = y.size() - 960; i < y.size(); ++i)
                worst = std::max (worst, std::abs (y[i]));
            if (! first)
                worstStep = std::max (worstStep, std::abs (y.back() - previous));
            previous = y.back();
            first = false;
        }
        INFO ((m == Model::A ? "Model A" : "Model B") << ": largest settled mixer output " << worst << ", largest change between routings " << worstStep);
        CHECK (worst <= 1.0e-9);
    }
}

TEST_CASE ("chain: the cutoff runs per sample while it moves and through setCutoff while it stands still; cutoff writes land at their own sample", "[chain][cutoff]")
{
    // A reference computation of the filter from the voice's own filter input at every sample (ChipVoice test aids), the
    // same Filter class, and the rule of ChipVoice.h "TIMING" written out here with its numbers: a cutoff or resonance
    // write made while the chip's sample s is in progress applies from the filter's sample for chip sample s on (output
    // sample s + filterLagSamples ()); a cutoff change less than 256 samples after the previous one runs
    // Filter::processWithCutoff (the cutoff given every sample) from that sample until the cutoff has stood still for
    // 256 samples, then setCutoff + process. One isolated change, a sweep (a change every 32 samples), a last isolated
    // change. (S3b-2 review: resonance writes are no longer here: they blend two configurations over the write's
    // window like volume and mode writes, and are pinned where they land by "every write lands where latencySamples ()
    // says ..." in ChipVoiceAliasTests.cpp.) To 1e-12 (bit for bit on Clang; GCC differed by one
    // rounding on Model A), both models (volume DC on, no 0x18 write: one configuration). Both paths are taken. Changed in S3b-2: the comparison is on the filter's output, not the
    // chain's (what follows the filter, the anti-alias stages, is pinned by its own tests), and the lag is the filter's
    // (the renderer's latency + 1, plus Model A's bus pre-filter), no longer latencySamples () + 1, which now also counts
    // the stages after the filter.
    for (const Model m : { Model::B, Model::A })
    {
        const double rate = 96000.0;
        vs::bside::chip::Config cfg;
        cfg.model = m;
        cfg.internalRate = rate;
        cfg.volumeDc = true;
        cfg.variation = 0.5f;
        PerNote p (m, rate, true);
        ChipVoice& v = *p.v;
        auto both = [&] (std::uint8_t reg, std::uint8_t value) { v.write (reg, value); };
        OscInput in;
        in.frequency = 220.0 * 16777216.0 / 985248.0;
        in.control = ctrl::saw | ctrl::gate;
        in.sustainRelease = 0xf0;
        v.setOscillator (0, in);
        both (0x15, 0);
        both (0x16, 0x40); // cutoff 512
        both (0x17, 0x81); // resonance 8, oscillator 1 to the filter
        both (0x18, 0x1f);
        const double cps = static_cast<double> (v.chip().sampleClock().increment()) / 4294967296.0;
        struct W
        {
            std::uint64_t cycle;
            std::uint8_t reg, value;
            std::uint64_t sample = 0;
        };
        std::vector<W> w;
        auto at = [cps] (double sample) { return static_cast<std::uint64_t> (sample * cps + 0.37 * cps); };
        w.push_back ({ at (2000.0), 0x16, 0x60 });
        for (int k = 0; k < 60; ++k)
        {
            w.push_back ({ at (4000.0 + 32.0 * k), 0x16, static_cast<std::uint8_t> (0x30 + (k * 23) % 160) });
        }
        w.push_back ({ at (11000.0), 0x16, 0x90 });
        const int total = 14000;
        // the voice, sample by sample: its filter's input and output at every output sample
        std::vector<double> y (static_cast<std::size_t> (total), 0.0), fin (y.size(), 0.0), fout (y.size(), 0.0);
        int done = 0;
        auto renderTo = [&] (std::uint64_t cycle) {
            while (done < total)
            {
                if (v.renderUntil (y.data() + done, 1, cycle) == 0)
                    break;
                fin[static_cast<std::size_t> (done)] = v.lastFilterInput();
                fout[static_cast<std::size_t> (done)] = v.lastFilterOutput();
                ++done;
            }
        };
        for (auto& x : w)
        {
            renderTo (x.cycle);
            REQUIRE (v.chip().cycle() == x.cycle);
            x.sample = v.chip().renderSampleIndex();
            both (x.reg, x.value);
        }
        renderTo (~std::uint64_t { 0 });
        REQUIRE (done == total);
        // the reference computation
        Filter rf;
        rf.prepare (m, rate, 0.5f);
        rf.setCutoff (512.0f);
        rf.setResonance (8);
        rf.setMode (0x1f);
        const std::int64_t delay = v.filterLagSamples();
        std::uint8_t lo = 0, hi = 0x40, resonance = 0x80;
        float cutoff = 512.0f;
        bool changed = false, moving = false;
        std::int64_t last = 0;
        int movingSamples = 0, stillSamples = 0;
        std::size_t next = 0;
        std::vector<double> expected (y.size(), 0.0);
        for (std::size_t b = 0; b < y.size(); ++b)
        {
            const std::int64_t q = static_cast<std::int64_t> (b) - delay;
            while (next < w.size() && static_cast<std::int64_t> (w[next].sample) <= q)
            {
                const W& x = w[next++];
                if (x.reg == 0x17)
                {
                    const auto r = static_cast<std::uint8_t> (x.value & 0xf0);
                    if (r != resonance)
                    {
                        resonance = r;
                        rf.setResonance (r >> 4);
                    }
                    continue;
                }
                (x.reg == 0x15 ? lo : hi) = x.value;
                const auto c = static_cast<float> ((lo & 7) | (hi << 3));
                if (! (c < cutoff || c > cutoff))
                    continue;
                cutoff = c;
                if (changed && q - last < 256)
                    moving = true;
                last = q;
                changed = true;
                if (! moving)
                    rf.setCutoff (cutoff);
            }
            if (moving && q - last >= 256)
            {
                moving = false;
                rf.setCutoff (cutoff);
            }
            expected[b] = moving ? rf.processWithCutoffDouble (fin[b], cutoff) : rf.processDouble (fin[b]);
            (moving ? movingSamples : stillSamples) += 1;
        }
        INFO ((m == Model::A ? "Model A" : "Model B") << ": " << movingSamples << " samples per sample (moving), " << stillSamples
                                                      << " still; largest difference from the reference computation " << maxAbsDiff (fout, expected));
        CHECK (movingSamples > 2000);
        CHECK (stillSamples > 8000);
        CHECK (maxAbsDiff (fout, expected) <= 1.0e-12); // (not bit for bit: GCC may contract the output function's last
                                                         // multiply-add differently in the two inlined copies; a rule
                                                         // broken by one sample differs by 1e-6 or more)
    }
}

TEST_CASE ("chain: at most kMaxConfigs + 1 configurations are blended; a further mode, volume or resonance write in the window retires the oldest at once", "[chain][queue]")
{
    // ChipVoice.h "TIMING" (VOLUME and MODE; RESONANCE): every 0x18 write that changes a mode or the volume, and every
    // resonance write that changes it, opens a configuration over the write's window (the long kernel, 16 samples);
    // kMaxConfigs (4) earlier configurations plus the current one are blended at once, and one more such write while
    // all are open retires the oldest at once. Six changing writes at one cycle (the second review's case: resonance;
    // here mode and resonance alike, both models): the blend reaches kMaxConfigs + 1 = 5 configurations and no more,
    // every output sample is finite, the windows close (1 configuration) and the voice ends in the last write's
    // state: its held output equals that of a voice given only the last write.
    for (const Model m : { Model::B, Model::A })
        for (const bool resonance : { false, true })
        {
            auto run = [&] (bool all, int& most) {
                PerNote p (m, 96000.0, true);
                ChipVoice& v = *p.v;
                v.boardFiltersEnabled (false);
                osc (v, 0, 0.0, 2048.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::test | ctrl::gate)); // a held voice
                v.write (0x15, 0);
                v.write (0x16, 0x80);
                v.write (0x17, 0x01);
                v.write (0x18, 0x1f);
                std::vector<double> y (6000, 0.0);
                int done = v.renderUntil (y.data(), 6000, 40000);
                const std::uint8_t res[] { 0x31, 0xf1, 0x51, 0xd1, 0x71, 0xb1 }, mode[] { 0x4f, 0x2f, 0x1e, 0x4d, 0x2c, 0x1b };
                for (int k = all ? 0 : 5; k < 6; ++k)
                    v.write (resonance ? 0x17 : 0x18, resonance ? res[k] : mode[k]);
                most = 1;
                while (done < 6000)
                {
                    done += v.renderUntil (y.data() + done, 1, ~std::uint64_t { 0 });
                    most = std::max (most, v.configsActive());
                    REQUIRE (std::isfinite (y[static_cast<std::size_t> (done - 1)]));
                }
                CHECK (v.configsActive() == 1);
                return y;
            };
            int most = 0, mostOne = 0;
            const auto y6 = run (true, most);
            const auto y1 = run (false, mostOne);
            INFO ((m == Model::A ? "Model A, " : "Model B, ") << (resonance ? "resonance" : "mode and volume") << ": at most " << most
                                                             << " configurations blended (one write: " << mostOne << "); held output after the windows "
                                                             << y6.back() << " against the last write alone " << y1.back());
            CHECK (most == ChipVoice::kMaxConfigs + 1);
            CHECK (mostOne == 2);
            CHECK (std::abs (y6.back() - y1.back()) <= 1.0e-9 * std::max (1.0, std::abs (y1.back())));
        }
}

TEST_CASE ("chain: a full write queue applies its oldest entry at once, keeping the order and every value", "[chain][queue]")
{
    // ChipVoice.h "QUEUE": a cutoff write every cycle at 88.2 kHz (about 11 cycles per sample; a write waits about
    // the filter's lag, filterLagSamples (), samples) fills the queue of 32. The rule, written out here: when write k
    // (made while sample s_k is in progress) finds 32 writes pending, the oldest pending one takes effect at once, i.e.
    // from the filter's sample s_k - lag on (the next output sample's); every other write from the filter's sample s_j
    // on. The cutoff in effect at every output sample is the value of the latest write in effect. Model B; then the same
    // burst with a 0x18 write at one cycle 40 times: bounded, finite, the blending ends. (S3b-2: the lag is the filter's,
    // filterLagSamples (); latencySamples () + 1 was the same number before the anti-alias stages followed the filter.)
    PerNote p (Model::B, 88200.0, true);
    ChipVoice& v = *p.v;
    osc (v, 0, 3000.0, 2048.0f, ctrl::saw | ctrl::gate);
    v.write (0x15, 0);
    v.write (0x16, 0x40);
    v.write (0x17, 0x01);
    v.write (0x18, 0x1f);
    const std::int64_t delay = v.filterLagSamples();
    struct W
    {
        std::int64_t sample;
        float value;
        std::int64_t effective;
    };
    std::vector<W> w;
    std::vector<std::size_t> pending;
    bst::Rng r (77);
    std::vector<double> buf (1, 0.0);
    std::int64_t outSamples = 0;
    int mismatches = 0, checked = 0;
    auto expectedAt = [&] (std::int64_t q) {
        float value = 512.0f;
        for (const auto& x : w)
            if (x.effective <= q)
                value = x.value;
        return value;
    };
    auto renderTo = [&] (std::uint64_t cycle) {
        while (true)
        {
            buf[0] = 0.0;
            const int got = v.renderUntil (buf.data(), 1, cycle);
            if (got == 0)
                break;
            const std::int64_t q = outSamples++ - delay;
            ++checked;
            if (! (std::abs (v.cutoffInEffect() - expectedAt (q)) < 0.25f))
                ++mismatches;
        }
    };
    const std::uint64_t start = 50000;
    for (std::uint64_t c = start; c < start + 600; ++c)
    {
        renderTo (c);
        const auto s = static_cast<std::int64_t> (v.chip().renderSampleIndex());
        // what the voice has applied by now: ring samples up to s - 1 - delay
        std::vector<std::size_t> still;
        for (const std::size_t j : pending)
            if (w[j].sample > s - 1 - delay)
                still.push_back (j);
        pending = still;
        if (pending.size() == 32)
        {
            w[pending.front()].effective = s - delay;
            pending.erase (pending.begin());
        }
        const auto value = static_cast<std::uint8_t> (r.below (256));
        w.push_back ({ s, static_cast<float> (value << 3), s });
        pending.push_back (w.size() - 1);
        v.write (0x16, value);
    }
    renderTo (start + 3000);
    INFO (v.queueOverflows() << " overflows, " << mismatches << " of " << checked << " samples with another cutoff than the rule's");
    CHECK (v.queueOverflows() > 100);
    CHECK (checked > 200);
    CHECK (mismatches == 0);
    CHECK (std::abs (v.cutoffInEffect() - w.back().value) < 0.25f);
    // 0x18 forty times at one cycle
    for (int k = 0; k < 40; ++k)
        v.write (0x18, static_cast<std::uint8_t> ((k % 2) == 0 ? 0x1f : 0x2a));
    std::vector<double> y (2000, 0.0);
    v.render (y.data(), static_cast<int> (y.size()));
    bool finite = true;
    for (const double s : y)
        finite = finite && std::isfinite (s) && std::abs (s) < 10.0;
    CHECK (finite);
    CHECK (v.configsActive() == 1);
}


TEST_CASE ("chain: the ideal render's convergence: its own placement error per write case at 8, 16, 32 and 64 times the rate (hidden)", "[.chain-writes-convergence]")
{
    // the ideal render's own error (writeExcess: the off-line difference of the render at a factor and at half of it, less
    // 6 dB) and ours against it, per factor: the gate's 32 times leaves the ideal's error at or below -112 dB on every row
    // of the normal run (96 kHz, voice at 2 kHz)
    const auto cases = writeCases (false);
    for (const Model m : { Model::B, Model::A })
        for (std::size_t ci = 0; ci < cases.size(); ++ci)
            for (const int f : { 8, 16, 32, 64 })
            {
                const auto r = writeExcess (m, 96000.0, 2000.0, true, cases[ci].start, cases[ci].writes, f);
                std::printf ("[write-convergence] Model %s %-42s x%2d: ours %.1f, ideal %.1f, the ideal's own error %.1f, excess %.1f dB\n", m == Model::A ? "A" : "B",
                             cases[ci].name.c_str(), f, r.ours.worstDb, r.ideal.worstDb, r.idealFloor.worstDb, r.excess.worstDb);
            }
}
