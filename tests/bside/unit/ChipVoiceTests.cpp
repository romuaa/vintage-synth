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
    cycle (kLatencySamples late): what remains is what the writes add. */
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
    double cps = 1.0;
    auto render = [&] (int mode) { // 0 writes, 1 held at a, 2 held at b
        PerNote p (m, rc.internalRate, vdc);
        ChipVoice& v = *p.v;
        v.boardFiltersEnabled (false);
        cps = static_cast<double> (v.chip().sampleClock().increment()) / 4294967296.0;
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
        while (w0 + 1 < writes.size() && static_cast<double> (writes[w0 + 1]) / cps + ChipVoice::kLatencySamples + 9.0 < static_cast<double> (s))
            ++w0;
        double sw = 0.0;
        for (std::size_t k = w0 > 2 ? w0 - 2 : 0; k < writes.size() && k < w0 + 4; ++k)
        {
            const double pos = static_cast<double> (writes[k]) / cps + ChipVoice::kLatencySamples;
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
    // times the long kernel's band-limited step at W (kLatencySamples late): its full size (nothing smoothed away),
    // at its cycle (one cycle early or late is far worse), band-limited. Both models, three rates.
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
                const double pos = (static_cast<double> (w) + shiftCycles) / cps + ChipVoice::kLatencySamples;
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
                                                          << ", late " << late << ")");
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
            std::array<double, ChipVoice::kBlock> so {}, y {};
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
                        double in = v[0]->staticFilterBusDc (i);
                        for (auto& x : v)
                            in += x->filterInputAc (i);
                        so[static_cast<std::size_t> (i)] = sf.processDouble (in);
                    }
                    const double fso = sf.staticOutput (v[0]->staticFilterBusDc (0));
                    for (auto& x : v)
                        x->finishShared (so.data(), n, fso, y.data(), want);
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
