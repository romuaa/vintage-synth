// The whole chain (docs/architecture.md 8.19.4, S3b-1; ChipVoice.h): any split bit-identical, no allocation, stability
// as 8.19.1 through the chain, the write-discontinuity budget of 8.4.5 for routing, volume and mode writes on the
// chain's output before the board filters, a volume write with volume DC on as a band-limited step of its full size,
// and the Shared topology against Per Note with real chips.

#include "FuzzSupport.h"
#include "RenderTestSupport.h"

#include "bside/chip/ChipVoice.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>

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
    WriteAlias r;
    const auto magW = windowedMagnitudes (xw);
    r.raw = offGridAlias (magW, rc.internalRate, rc.band, lineHz, 8.0);
    const auto ra = offGridAlias (windowedMagnitudes (xa), rc.internalRate, rc.band, lineHz, 8.0);
    const auto rb = offGridAlias (windowedMagnitudes (xb), rc.internalRate, rc.band, lineHz, 8.0);
    r.baseline = ra.worstDb > rb.worstDb ? ra : rb;
    // the difference relative to the output's own strongest line
    auto magD = windowedMagnitudes (xd);
    double refW = 0.0, refD = 0.0;
    const double binHz = rc.internalRate / static_cast<double> (n);
    for (std::size_t k = 1; k < magW.size() && static_cast<double> (k) * binHz <= rc.band; ++k)
    {
        refW = std::max (refW, magW[k]);
        refD = std::max (refD, magD[k]);
    }
    r.write = offGridAlias (magD, rc.internalRate, rc.band, lineHz, 8.0);
    r.write.worstDb += 20.0 * std::log10 ((refD + 1e-300) / refW);
    return r;
}
} // namespace

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
        osc (x, 0, hz * 16777216.0 / 985248.0, 1500.0f + 100.0f * static_cast<float> (k), static_cast<std::uint8_t> (wave | ctrl::gate));
        osc (x, 1, 1.5 * hz * 16777216.0 / 985248.0, 1500.0f + 100.0f * static_cast<float> (k), static_cast<std::uint8_t> (wave | ctrl::gate));
        if (s.osc3)
            osc (x, 2, 2.25 * hz * 16777216.0 / 985248.0, 0.0f, static_cast<std::uint8_t> (ctrl::saw | ctrl::gate));
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
                for (auto& x : v)
                    x->finishShared (sh.data(), so.data(), sl.data(), ss.data(), s.n, y.data(), want);
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
