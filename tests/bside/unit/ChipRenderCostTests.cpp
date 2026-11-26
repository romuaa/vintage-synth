// The render cost step (docs/architecture.md 8.18): the batched path renders exactly as the general path,
// the digital core's skip over repeating envelope states equals stepping, and the regression tests of the
// three defects 8.17 listed.

#include "../../support/Env.h"
#include "FuzzSupport.h"
#include "RenderTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

using namespace bs_render_test;
namespace ctrl = vs::bside::chip::ctrl;
using vs::bside::chip::ShapeSet;

namespace
{
OscInput input (double f, float pw, std::uint8_t control, std::uint8_t ad, std::uint8_t sr)
{
    OscInput in;
    in.frequency = f;
    in.pulseWidth = pw;
    in.control = control;
    in.attackDecay = ad;
    in.sustainRelease = sr;
    return in;
}

/** One random patch and write schedule, rendered in random blocks (render and renderUntil) by `c`; the writes
    come from the generator, so two chips given the same seed see the same inputs at the same cycles. */
std::vector<float> renderRandom (Chip& c, std::uint64_t seed, std::uint64_t total)
{
    bst::Rng rng (seed);
    const bool syncRing = rng.chance (40);
    const bool high = rng.chance (15); // Smooth up to 12 kHz
    double f[3];
    float pw[3];
    std::uint8_t ct[3], ad[3], sr[3];
    auto freq = [&] {
        const int r = rng.range (0, 5);
        double v = r == 0 ? static_cast<double> (rng.below (65536)) : r == 1 ? 60000.0 + rng.below (5536) + 0.37
                   : r == 2 ? 2000.0 + rng.below (30000) * 1.0001 : r == 3 ? rng.below (2000) + 0.5 : r == 4 ? 65535.0 : 0.0;
        return high ? 65535.0 + rng.below (140000) : v;
    };
    auto control = [&] {
        return static_cast<std::uint8_t> ((rng.below (16) << 4) | (syncRing && rng.chance (40) ? ctrl::sync : 0) | (syncRing && rng.chance (25) ? ctrl::ring : 0)
                                          | (rng.chance (85) ? ctrl::gate : 0) | (rng.chance (3) ? ctrl::test : 0));
    };
    auto envelope = [&] { return static_cast<std::uint8_t> (rng.chance (50) ? rng.below (256) : rng.below (16) * 0x11); };
    for (int n = 0; n < 3; ++n)
    {
        ct[n] = control();
        f[n] = freq();
        pw[n] = rng.chance (30) ? static_cast<float> (rng.below (4096)) + 0.5f : static_cast<float> (rng.below (4096));
        ad[n] = envelope();
        sr[n] = envelope();
        c.setOscillator (n, input (f[n], pw[n], ct[n], ad[n], sr[n]));
    }
    std::vector<float> y;
    std::vector<float> block (300);
    std::uint64_t at = 3000;
    const bool dense = rng.chance (33);
    while (c.cycle() < total)
    {
        at += static_cast<std::uint64_t> (dense ? rng.range (30, 600) : rng.range (300, 20000));
        while (c.cycle() < at && c.cycle() < total)
        {
            const int want = 1 + static_cast<int> (rng.below (300));
            std::fill (block.begin(), block.end(), 0.0f);
            int done = 0;
            const double cps = cyclesPerSample (c);
            const int fit = static_cast<int> ((static_cast<double> (at - c.cycle()) - 2.0) / cps);
            if (rng.chance (50) && fit >= 1)
            {
                done = std::min (want, fit);
                c.render (block.data(), done);
            }
            else
                done = c.renderUntil (block.data(), want, at);
            y.insert (y.end(), block.begin(), block.begin() + done);
        }
        if (c.cycle() >= total)
            break;
        const unsigned r = rng.below (100);
        const int n = static_cast<int> (rng.below (3));
        if (r < 20)
            for (int q = 0; q < 3; ++q)
            {
                ct[q] = control();
                c.setOscillator (q, input (f[q], pw[q], ct[q], ad[q], sr[q]));
            }
        else if (r < 35)
        {
            f[n] = rng.chance (20) ? freq() : rng.below (65536) + 0.25;
            c.setOscillator (n, input (f[n], pw[n], ct[n], ad[n], sr[n]));
        }
        else if (r < 50)
        {
            pw[n] = static_cast<float> (rng.below (4096));
            c.setOscillator (n, input (f[n], pw[n], ct[n], ad[n], sr[n]));
        }
        else if (r < 65)
        {
            ct[n] = static_cast<std::uint8_t> (ct[n] ^ ctrl::gate);
            c.setOscillator (n, input (f[n], pw[n], ct[n], ad[n], sr[n]));
        }
        else if (r < 72)
        {
            ad[n] = envelope();
            sr[n] = envelope();
            c.setOscillator (n, input (f[n], pw[n], ct[n], ad[n], sr[n]));
        }
        else if (r < 92)
            c.write (static_cast<std::uint8_t> (7 * n + static_cast<int> (rng.below (7))), static_cast<std::uint8_t> (rng.below (256)));
        else if (r < 96)
            c.clearEnvelopeCounters (n);
        else
            c.powerOnKeepingTime();
    }
    return y;
}
} // namespace

TEST_CASE ("render cost: the batched path renders exactly as the general path (random patches, writes, envelopes, blocks)", "[render][batch][slow]")
{
    // Every output sample and the digital state bit-identical with the batched path on and off, the
    // renderer's diagnostics too; and the batched path is taken (it renders most samples of these patches).
    int trials = 0;
    for (std::uint64_t seed = 1; seed <= 160; ++seed)
    {
        bst::Rng pick (seed * 7919u);
        const Model m = pick.chance (50) ? Model::A : Model::B;
        const double rate = rateConfigs()[pick.below (7)].internalRate;
        Config cfg;
        cfg.model = m;
        cfg.internalRate = rate;
        cfg.shapes = pick.chance (25) ? ShapeSet::placeholder : ShapeSet::fitted;
        cfg.authentic = pick.chance (25);
        Chip a, b;
        a.prepare (cfg);
        b.prepare (cfg);
        b.renderBatchingEnabled (false);
        const auto ya = renderRandom (a, seed, 160000);
        const auto yb = renderRandom (b, seed, 160000);
        INFO ("seed " << seed << ", " << (m == Model::A ? "Model A" : "Model B") << ", " << rate << " Hz");
        REQUIRE (ya.size() == yb.size());
        bool same = true;
        std::size_t first = 0;
        for (std::size_t i = 0; i < ya.size() && same; ++i)
            if (std::memcmp (&ya[i], &yb[i], sizeof (float)) != 0)
            {
                same = false;
                first = i;
            }
        INFO ("first differing sample " << first << ": " << (same ? 0.0f : ya[first]) << " / " << (same ? 0.0f : yb[first]));
        CHECK (same);
        CHECK (a.state() == b.state());
        CHECK (a.renderCorrections() == b.renderCorrections());
        CHECK (a.renderConverterSwitches() == b.renderConverterSwitches());
        ++trials;
    }
    CHECK (trials == 160);
}

TEST_CASE ("render cost: the batched path, under the state check, finds no mismatch and renders as without it", "[render][batch][slow]")
{
    for (std::uint64_t seed = 300; seed < 340; ++seed)
    {
        Config cfg;
        cfg.model = seed % 2 == 0 ? Model::A : Model::B;
        cfg.internalRate = rateConfigs()[seed % 7].internalRate;
        Chip a, b;
        a.prepare (cfg);
        b.prepare (cfg);
        a.renderCheckStates (true);
        const auto ya = renderRandom (a, seed, 120000);
        const auto yb = renderRandom (b, seed, 120000);
        INFO ("seed " << seed);
        REQUIRE (ya.size() == yb.size());
        CHECK (std::memcmp (ya.data(), yb.data(), ya.size() * sizeof (float)) == 0);
        CHECK (a.state() == b.state());
    }
}

TEST_CASE ("chip: an envelope that cannot change skips its repeating states exactly (frozen, sustain, every divider period)", "[bside][chip][equivalence][slow]")
{
    // Frozen at zero after a release, and holding sustain levels whose divider periods are 1 .. 30; every
    // rate; long advances (the skip) against per-cycle stepping, also right after a rate change that leaves
    // the rate counter above its new match (off its cycle) and after clearEnvelopeCounters.
    for (const Model m : { Model::A, Model::B })
        for (int rate = 0; rate < 16; ++rate)
            for (const int sustain : { 0x0, 0x1, 0x2, 0x3, 0x5, 0xa, 0xf })
            {
                Config cfg;
                cfg.model = m;
                Chip c;
                c.prepare (cfg);
                const auto ad = static_cast<std::uint8_t> (rate);
                const auto sr = static_cast<std::uint8_t> ((sustain << 4) | rate);
                c.setOscillator (0, input (3000.0, 2048.0f, ctrl::saw | ctrl::gate, ad, sr));
                c.setOscillator (1, input (0.0, 0.0f, 0x00, 0x00, sr));
                c.setOscillator (2, input (5000.0, 100.0f, ctrl::pulse | ctrl::gate, ad, static_cast<std::uint8_t> (sr & 0xf0)));
                c.advanceCycles (2000000); // decay to sustain; oscillator 2 (no gate) frozen in release
                Chip d = c;
                INFO ((m == Model::A ? "Model A" : "Model B") << ", rate " << rate << ", sustain " << int (sustain));
                c.advanceCycles (400009);
                for (int i = 0; i < 400009; ++i)
                    d.stepCycle();
                REQUIRE (c.state() == d.state());
                // a faster rate leaves the rate counter above its match; a slower one moves the match away
                c.write (0x05, static_cast<std::uint8_t> ((rate + 7) % 16));
                d.write (0x05, static_cast<std::uint8_t> ((rate + 7) % 16));
                c.write (0x0d, 0x00);
                d.write (0x0d, 0x00);
                c.clearEnvelopeCounters (2);
                d.clearEnvelopeCounters (2);
                c.advanceCycles (150001);
                for (int i = 0; i < 150001; ++i)
                    d.stepCycle();
                REQUIRE (c.state() == d.state());
            }
}

TEST_CASE ("chip: the Model A pull-down in the fast path equals stepping (both shape sets, pulse gates, ring and sync around it)", "[bside][chip][equivalence][slow]")
{
    // The fast path follows a pulling oscillator (8.15.2 (c) step 4) in closed form; here against per-cycle
    // stepping with the placeholder shapes too (their top bit stays up for a quarter period), pulse widths
    // around the half, ring and sync on the other oscillators, writes that leave the accumulator in the upper
    // half, and every jump length.
    const std::uint8_t sawCombos[] { 0x30, 0x60, 0x70 };
    for (const ShapeSet set : { ShapeSet::fitted, ShapeSet::placeholder })
        for (std::uint64_t seed = 1; seed <= 12; ++seed)
        {
            bst::Rng r (seed + (set == ShapeSet::fitted ? 0u : 100u));
            Config cfg;
            cfg.model = Model::A;
            cfg.shapes = set;
            cfg.authentic = r.chance (50);
            Chip fast, ref;
            fast.prepare (cfg);
            ref.prepare (cfg);
            for (int w = 0; w < 300; ++w)
            {
                const int n = static_cast<int> (r.below (3));
                std::uint8_t c = r.chance (70) ? sawCombos[r.below (3)] : static_cast<std::uint8_t> (r.below (8) << 4);
                c = static_cast<std::uint8_t> (c | ctrl::gate | (r.chance (25) ? ctrl::sync : 0) | (r.chance (25) ? ctrl::ring : 0) | (r.chance (3) ? ctrl::test : 0));
                const double f = r.chance (20) ? static_cast<double> (r.below (300)) : r.chance (50) ? static_cast<double> (r.below (65536)) : 65535.0 + r.below (140000);
                const float pw = r.chance (50) ? 2048.0f + static_cast<float> (r.range (-40, 40)) : static_cast<float> (r.below (4096));
                const auto in = input (f, pw, c, 0x00, 0xf0);
                if (r.chance (80))
                {
                    fast.setOscillator (n, in);
                    ref.setOscillator (n, in);
                }
                else
                {
                    const auto reg = static_cast<std::uint8_t> (7 * n + static_cast<int> (r.below (5)));
                    const auto value = static_cast<std::uint8_t> (r.below (256));
                    fast.write (reg, value);
                    ref.write (reg, value);
                }
                const std::uint64_t gap = r.chance (40) ? r.below (20) : r.chance (70) ? r.below (3000) : r.below (60000);
                fast.advanceCycles (gap);
                for (std::uint64_t i = 0; i < gap; ++i)
                    ref.stepCycle();
                INFO ((set == ShapeSet::fitted ? "fitted" : "placeholder") << " seed " << seed << ", write " << w);
                REQUIRE (fast.state() == ref.state());
            }
        }
}

TEST_CASE ("chip: the fast path ends a jump on a pull-down as stepping does (oscillator 1 reads oscillator 3 before its pull)", "[bside][chip][equivalence]")
{
    // Short jumps of every length end on every step of a pulling oscillator 3's period: the outputs of the
    // last step (oscillator 1's ring reads oscillator 3's top bit before oscillator 3's pull-down, 8.15.2
    // (c) step 1), the comparison and the rise flag come out as stepping gives them.
    for (const ShapeSet set : { ShapeSet::fitted, ShapeSet::placeholder })
        for (const std::uint8_t combo : { std::uint8_t { 0x30 }, std::uint8_t { 0x60 }, std::uint8_t { 0x70 } })
        {
            Config cfg;
            cfg.model = Model::A;
            cfg.shapes = set;
            Chip fast, ref;
            fast.prepare (cfg);
            ref.prepare (cfg);
            for (Chip* c : { &fast, &ref })
            {
                c->setOscillator (0, input (9000.3, 2048.0f, ctrl::triangle | ctrl::ring | ctrl::gate, 0x00, 0xf0));
                c->setOscillator (1, input (7000.0, 1900.0f, static_cast<std::uint8_t> (0x70 | ctrl::ring | ctrl::gate), 0x00, 0xf0));
                c->setOscillator (2, input (65535.0 + 40000.0, 2040.0f, static_cast<std::uint8_t> (combo | ctrl::gate), 0x00, 0xf0));
            }
            bst::Rng r (combo);
            for (int i = 0; i < 6000; ++i)
            {
                const std::uint64_t gap = 1 + r.below (i % 3 == 0 ? 200u : 9u);
                fast.advanceCycles (gap);
                for (std::uint64_t k = 0; k < gap; ++k)
                    ref.stepCycle();
                INFO ((set == ShapeSet::fitted ? "fitted" : "placeholder") << ", combination " << int (combo) << ", jump " << i);
                REQUIRE (fast.state() == ref.state());
            }
        }
}

TEST_CASE ("render cost: the batched path renders exactly as the general path around Model A pull-downs and their landings", "[render][batch][slow]")
{
    // Writes that switch slow Model A oscillators to saw combinations while their accumulators are in the upper
    // half: the chip pulls them down at a later landing (renderLanding), a time-based event the batched path
    // must find at the same place as the general one (the long run of "[.render-corrections-long]" found three
    // patches where it did not: the landing's time taken against the wrong sweep position)
    const std::uint8_t combos[] { 0x31, 0x61, 0x71, 0x21, 0x41, 0x51 };
    for (const ShapeSet set : { ShapeSet::fitted, ShapeSet::placeholder })
        for (std::uint64_t seed = 1; seed <= 40; ++seed)
        {
            bst::Rng r (seed * 31u + (set == ShapeSet::fitted ? 0u : 7u));
            Config cfg;
            cfg.model = Model::A;
            cfg.shapes = set;
            cfg.internalRate = rateConfigs()[r.below (7)].internalRate;
            Chip a, b;
            a.prepare (cfg);
            b.prepare (cfg);
            b.renderBatchingEnabled (false);
            double f[3];
            float pw[3];
            std::uint8_t ct[3];
            for (int n = 0; n < 3; ++n)
            {
                f[n] = r.chance (50) ? 200.0 + r.below (3000) : 2000.0 + r.below (60000);
                pw[n] = static_cast<float> (r.below (4096));
                ct[n] = combos[r.below (6)];
                for (Chip* c : { &a, &b })
                    c->setOscillator (n, input (f[n], pw[n], ct[n], 0x00, 0xf0));
            }
            std::vector<float> ya, yb, block (256);
            for (int w = 0; w < 60; ++w)
            {
                const std::uint64_t at = a.cycle() + 50 + r.below (3000);
                for (auto [c, y] : { std::pair<Chip*, std::vector<float>*> { &a, &ya }, std::pair<Chip*, std::vector<float>*> { &b, &yb } })
                    while (c->cycle() < at)
                    {
                        std::fill (block.begin(), block.end(), 0.0f);
                        const int done = c->renderUntil (block.data(), 256, at);
                        y->insert (y->end(), block.begin(), block.begin() + done);
                    }
                const int n = static_cast<int> (r.below (3));
                if (r.chance (60))
                    ct[n] = combos[r.below (6)];
                else
                    pw[n] = static_cast<float> (r.below (4096));
                for (Chip* c : { &a, &b })
                    c->setOscillator (n, input (f[n], pw[n], ct[n], 0x00, 0xf0));
            }
            INFO ((set == ShapeSet::fitted ? "fitted" : "placeholder") << " seed " << seed);
            REQUIRE (ya.size() == yb.size());
            CHECK (std::memcmp (ya.data(), yb.data(), ya.size() * sizeof (float)) == 0);
            CHECK (a.state() == b.state());
            CHECK (a.renderCorrections() == b.renderCorrections());
        }
}

namespace
{
/** Everything of x above `band` Hz removed (a brick wall in the spectrum; x zero-padded to a power of two). */
std::vector<double> lowpass (const std::vector<float>& x, double rate, double band)
{
    std::size_t n = 1;
    while (n < x.size())
        n <<= 1;
    std::vector<std::complex<double>> a (n);
    for (std::size_t i = 0; i < x.size(); ++i)
        a[i] = static_cast<double> (x[i]);
    fft (a);
    for (std::size_t b = 0; b < n; ++b)
    {
        const double hz = static_cast<double> (std::min (b, n - b)) * rate / static_cast<double> (n);
        if (hz > band)
            a[b] = 0.0;
    }
    for (auto& c : a)
        c = std::conj (c);
    fft (a);
    std::vector<double> y (x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        y[i] = a[i].real() / static_cast<double> (n);
    return y;
}

struct AboveResult
{
    double aliasDb = -300.0;     // worst non-harmonic component up to 20 kHz, dB re the fundamental (step moments)
    double aliasFineDb = -300.0; // the same with every step emitted on its own
    double bandDiffDb = -300.0;  // the largest in-band (<= 20 kHz) difference between the two, dB re 1
};

/** One combined waveform (control c, width pw; with `synced`, oscillator 2 synced by a 1.1 kHz saw on
    oscillator 1) at fundamental hz (Smooth, above the register's range) on a bin-centred frequency: its
    aliasing with and without the step moments, and their difference in the audio band. */
AboveResult measureAbove (Model m, std::uint8_t c, double hz, double rate, float pw = 1024.0f, bool synced = false)
{
    constexpr int n = 1 << 15;
    AboveResult r;
    std::vector<float> y[2];
    for (int on = 0; on < 2; ++on)
    {
        Chip chip = makeChip (m, rate);
        chip.renderStepMomentsEnabled (on == 1);
        const int bin = oddBin (hz, rate, n);
        if (synced)
        {
            setOsc (chip, 0, 1100.0 * 16777216.0 / 985248.0, 2048.0f, ctrl::saw);
            setOsc (chip, 1, binFrequency (chip, bin, n), pw, static_cast<std::uint8_t> (c | ctrl::sync | ctrl::gate));
        }
        else
            setOsc (chip, 0, binFrequency (chip, bin, n), pw, static_cast<std::uint8_t> (c | ctrl::gate));
        renderSamples (chip, 4000);
        y[on] = renderSamples (chip, n);
        if (! synced)
        {
            const auto a = aliasOf (y[on], rate, 20000.0, -1, bin); // (a Model A saw combination pulled at once repeats every half period)
            (on == 1 ? r.aliasDb : r.aliasFineDb) = a.worstDb;
        }
    }
    const auto l0 = lowpass (y[0], rate, 20000.0), l1 = lowpass (y[1], rate, 20000.0);
    double worst = 0.0;
    for (std::size_t i = 1000; i + 1000 < l0.size(); ++i)
        worst = std::max (worst, std::abs (l0[i] - l1[i]));
    r.bandDiffDb = 20.0 * std::log10 (worst + 1e-300);
    return r;
}

/** A run with writes at given cycles, rendered with and without the step moments (renderUntil to each write);
    returns the in-band difference and the sample index of every write. */
struct Run
{
    std::vector<double> diff;               // |in-band difference| per sample
    std::vector<std::size_t> marks;         // the sample of each write
    std::vector<bool> switched;             // whether that write switched how the steps are emitted
};
Run runWrites (Model m, double rate, const std::function<void (Chip&)>& setup, const std::vector<std::pair<std::uint64_t, std::function<void (Chip&)>>>& writes,
               std::uint64_t total)
{
    Run r;
    std::vector<float> y[2];
    for (int on = 0; on < 2; ++on)
    {
        Chip c = makeChip (m, rate);
        c.renderStepMomentsEnabled (on == 1);
        setup (c);
        std::vector<float> block (64);
        std::size_t w = 0;
        while (c.cycle() < total)
        {
            const std::uint64_t at = w < writes.size() ? writes[w].first : total;
            while (c.cycle() < at)
            {
                std::fill (block.begin(), block.end(), 0.0f);
                const int done = c.renderUntil (block.data(), 64, at);
                y[on].insert (y[on].end(), block.begin(), block.begin() + done);
            }
            if (w < writes.size())
            {
                const auto before = c.renderMethodSwitches();
                writes[w++].second (c);
                if (on == 1)
                {
                    r.marks.push_back (y[on].size());
                    r.switched.push_back (c.renderMethodSwitches() != before);
                }
            }
        }
    }
    const auto a = lowpass (y[0], rate, 20000.0), b = lowpass (y[1], rate, 20000.0);
    r.diff.resize (a.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        r.diff[i] = std::abs (a[i] - b[i]);
    return r;
}

double maxOver (const std::vector<double>& d, std::size_t from, std::size_t to)
{
    double m = 0.0;
    for (std::size_t i = from; i < std::min (to, d.size()); ++i)
        m = std::max (m, d[i]);
    return m;
}

double hzToReg (double hz) { return hz * 16777216.0 / 985248.0; }
} // namespace

TEST_CASE ("render cost: combined waveforms above the register's range, step moments against one by one (report)", "[.render-above]")
{
    // The step moments (RenderTables.h kStepMoments) above the register's highest frequency: aliasing up to
    // 20 kHz with and without them, and how far the audio band moves (Smooth reaches 12 kHz; no budget exists
    // above 3.85 kHz, 8.18). The synced rows: oscillator 2 synced by a 1.1 kHz saw (no alias column).
    std::printf ("%-7s %-5s %6s %9s %8s %12s %12s %14s\n", "model", "wave", "width", "rate", "f (Hz)", "alias (dB)", "one by one", "band diff (dB)");
    for (const Model m : { Model::A, Model::B })
        for (const std::uint8_t c : { std::uint8_t { 0x30 }, std::uint8_t { 0x50 }, std::uint8_t { 0x60 }, std::uint8_t { 0x70 } })
            for (const double rate : { 88200.0, 96000.0, 192000.0 })
                for (const double hz : { 4000.0, 5000.0, 7000.0, 9000.0, 12000.0 })
                    for (const bool synced : { false, true })
                    {
                        const auto r = measureAbove (m, c, hz, rate, 1024.0f, synced);
                        std::printf ("%-7s %-5x %6s %9.0f %8.0f %12.1f %12.1f %14.1f\n", m == Model::A ? "A" : "B", c, synced ? "sync" : "1024", rate, hz, r.aliasDb,
                                     r.aliasFineDb, r.bandDiffDb);
                    }
}

TEST_CASE ("render cost: step moments only above the register's range, entered with hysteresis, close to one by one in the band", "[render][levels][slow]")
{
    // At and below the register's top nothing changes (output bit-identical with the moments off), also on a glide
    // up to it and at 2 % above it when coming from below (the entry hysteresis); from 4 % above it the moments
    // are used. Above, the audio band stays within -60 dB re 1 of the one-by-one rendering (every combination,
    // both models, widths, a synced target, 88.2 and 96 kHz; measured about -78 dB at worst).
    for (const Model m : { Model::A, Model::B })
        for (const std::uint8_t c : { std::uint8_t { 0x30 }, std::uint8_t { 0x50 }, std::uint8_t { 0x60 }, std::uint8_t { 0x70 } })
        {
            for (const double regValue : { 65000.0, 65535.0, 65535.0 * 1.02 })
            {
                Chip a = makeChip (m, 96000.0), b = makeChip (m, 96000.0);
                b.renderStepMomentsEnabled (false);
                for (Chip* x : { &a, &b })
                {
                    setOsc (*x, 0, 60000.0, 1024.0f, static_cast<std::uint8_t> (c | ctrl::gate));
                    renderSamples (*x, 500);
                    setOsc (*x, 0, regValue, 1024.0f, static_cast<std::uint8_t> (c | ctrl::gate));
                }
                const auto ya = renderSamples (a, 6000), yb = renderSamples (b, 6000);
                INFO ((m == Model::A ? "Model A" : "Model B") << ", control " << int (c) << ", frequency register " << regValue);
                CHECK (a.renderMethodSwitches() == 0);
                CHECK (std::memcmp (ya.data(), yb.data(), ya.size() * sizeof (float)) == 0);
            }
            {
                Chip a = makeChip (m, 96000.0), b = makeChip (m, 96000.0);
                b.renderStepMomentsEnabled (false);
                for (Chip* x : { &a, &b })
                    setOsc (*x, 0, 65535.0 * 1.04, 1024.0f, static_cast<std::uint8_t> (c | ctrl::gate));
                const auto ya = renderSamples (a, 3000), yb = renderSamples (b, 3000);
                INFO ((m == Model::A ? "Model A" : "Model B") << ", control " << int (c) << ": 4 % above the register's top");
                CHECK (std::memcmp (ya.data(), yb.data(), ya.size() * sizeof (float)) != 0);
            }
            for (const double rate : { 88200.0, 96000.0 })
                for (const double hz : { 4000.0, 6000.0, 9000.0, 12000.0 })
                    for (const float pw : { 1024.0f, 3000.0f })
                        for (const bool synced : { false, true })
                        {
                            const auto r = measureAbove (m, c, hz, rate, pw, synced);
                            INFO ((m == Model::A ? "Model A" : "Model B") << ", control " << int (c) << ", " << rate << " Hz, " << hz << " Hz, width " << pw
                                                                          << (synced ? ", synced" : "") << ": in-band difference " << r.bandDiffDb << " dB");
                            CHECK (r.bandDiffDb < -60.0);
                        }
        }
}

TEST_CASE ("render cost: switching to and from the step moments makes no click (single writes and glides across the limit)", "[render][levels][slow]")
{
    // 8.18: the switch between the methods must not click. Near a switch (+-40 samples) the in-band difference
    // from the one-by-one rendering may exceed the larger of the two sides' steady differences by at most 6 dB.
    // Single writes across the limit in both directions, glides across it (a write every 2 000 cycles), both
    // models, 88.2 and 96 kHz; a pulse combination, a Model A saw combination (pulled down every period), and a
    // synced target (resets around the writes).
    struct Setup
    {
        const char* name;
        std::uint8_t control;
        bool synced;
    };
    const Setup setups[] { { "triangle+saw", 0x31, false }, { "triangle+saw+pulse", 0x71, false }, { "saw+pulse, synced", 0x63, true } };
    auto set = [] (Chip& c, const Setup& s, double hz) {
        if (s.synced)
            c.setOscillator (1, input (hzToReg (hz), 1800.0f, s.control, 0x00, 0xf0)); // (the gate opens below the half: a Model A saw combination never passes it)
        else
            c.setOscillator (0, input (hzToReg (hz), 1024.0f, s.control, 0x00, 0xf0));
    };
    for (const Model m : { Model::A, Model::B })
        for (const double rate : { 88200.0, 96000.0 })
            for (const auto& s : setups)
            {
                auto setup = [&] (Chip& c) {
                    if (s.synced)
                        setOsc (c, 0, hzToReg (1300.0), 2048.0f, ctrl::saw);
                };
                // single writes across the limit, both directions
                for (const auto& pair : std::vector<std::pair<double, double>> { { 3800.0, 4200.0 }, { 4200.0, 3800.0 }, { 3700.0, 9000.0 }, { 12000.0, 3600.0 }, { 5900.0, 6100.0 } })
                {
                    const Run r = runWrites (
                        m, rate, [&] (Chip& c) { setup (c); set (c, s, pair.first); }, { { 100000u, [&] (Chip& c) { set (c, s, pair.second); } } }, 200000);
                    const std::size_t mk = r.marks[0];
                    const double before = maxOver (r.diff, 3000, mk - 200), after = maxOver (r.diff, mk + 200, r.diff.size() - 3000);
                    const double near = maxOver (r.diff, mk - 40, mk + 41);
                    INFO ((m == Model::A ? "Model A " : "Model B ") << s.name << ", " << rate << " Hz, " << pair.first << " -> " << pair.second << " Hz: steady "
                                                                    << 20.0 * std::log10 (before + 1e-300) << " / " << 20.0 * std::log10 (after + 1e-300)
                                                                    << " dB, near the write " << 20.0 * std::log10 (near + 1e-300) << " dB");
                    CHECK (near <= 2.0 * std::max ({ before, after, 1.0e-7 }));
                }
                // a glide across the limit and back: windows around switching writes against the others
                std::vector<std::pair<std::uint64_t, std::function<void (Chip&)>>> writes;
                for (int i = 0; i < 120; ++i)
                {
                    const double ph = i / 120.0;
                    const double hz = 3600.0 + 1400.0 * (ph < 0.5 ? 2.0 * ph : 2.0 - 2.0 * ph);
                    writes.push_back ({ 20000u + 2000u * static_cast<std::uint64_t> (i), [&s, &set, hz] (Chip& c) { set (c, s, hz); } });
                }
                const Run r = runWrites (m, rate, [&] (Chip& c) { setup (c); set (c, s, 3600.0); }, writes, 20000u + 2000u * 121u);
                double atSwitch = 0.0, elsewhere = 0.0;
                int switches = 0;
                for (std::size_t w = 0; w < r.marks.size(); ++w)
                {
                    const double near = maxOver (r.diff, r.marks[w] - 40, r.marks[w] + 41);
                    if (r.switched[w])
                    {
                        atSwitch = std::max (atSwitch, near);
                        ++switches;
                    }
                    else if (w > 0 && w + 1 < r.marks.size() && ! r.switched[w - 1] && ! r.switched[w + 1])
                        elsewhere = std::max (elsewhere, maxOver (r.diff, r.marks[w] - 40, r.marks[w] + 41));
                }
                INFO ((m == Model::A ? "Model A " : "Model B ") << s.name << ", " << rate << " Hz, glide: " << switches << " switches, near them "
                                                                << 20.0 * std::log10 (atSwitch + 1e-300) << " dB, near other writes " << 20.0 * std::log10 (elsewhere + 1e-300) << " dB");
                CHECK (switches >= 2);
                CHECK (atSwitch <= 2.0 * std::max (elsewhere, 1.0e-7));
            }
}

TEST_CASE ("chip: a Model A saw combination at frequency 0 or below one accumulator step, both halves (no division by a zero increment)", "[bside][chip][equivalence]")
{
    // The closed form of the pull-down (pullRun) divided by the increment: frequency 0 (a control written
    // before any frequency, a trace) crashed on x86-64. Every saw combination, with and without the pulse, the
    // accumulator in either half, frequency 0 and Smooth increments below one accumulator step; the fast path
    // against stepping, and rendering. NOTE: on arm64 an integer division by zero returns 0 without a trap, so
    // in a plain arm64 build this test passes with the guard removed; it proves the guard only under UBSan
    // (scripts/sanitize-targets.sh, halt_on_error) or on x86-64 (render cost review, round 3).
    const std::uint8_t combos[] { 0x31, 0x61, 0x71 };
    for (const ShapeSet set : { ShapeSet::fitted, ShapeSet::placeholder })
        for (const std::uint8_t combo : combos)
            for (const bool upper : { false, true })
                for (const double f : { 0.0, 0.25, 0.999, 1.0e-6 })
                {
                    Config cfg;
                    cfg.model = Model::A;
                    cfg.shapes = set;
                    Chip fast;
                    fast.prepare (cfg);
                    if (upper)
                    {
                        // a triangle until the accumulator is in its upper half, then the frequency
                        fast.setOscillator (0, input (60000.0, 2048.0f, ctrl::triangle | ctrl::gate, 0x00, 0xf0));
                        while ((fast.phase (0) & vs::bside::chip::kPhaseMsb) == 0 || ((fast.accumulator (0) >> 12) & 0x7ffu) < 300u)
                            fast.stepCycle();
                    }
                    Chip ref = fast, rendered = fast;
                    for (Chip* c : { &fast, &ref, &rendered })
                    {
                        if (f <= 0.0)
                        {
                            c->write (0x00, 0x00);
                            c->write (0x01, 0x00);
                            c->write (0x04, combo);
                        }
                        else
                            c->setOscillator (0, input (f, 1000.0f, combo, 0x00, 0xf0));
                    }
                    for (const std::uint64_t gap : { 1u, 2u, 7u, 100u, 4096u, 5000u })
                    {
                        fast.advanceCycles (gap);
                        for (std::uint64_t i = 0; i < gap; ++i)
                            ref.stepCycle();
                        INFO ((set == ShapeSet::fitted ? "fitted" : "placeholder") << ", control " << int (combo) << (upper ? ", upper half" : ", lower half")
                                                                                   << ", frequency " << f << ", jump " << gap);
                        REQUIRE (fast.state() == ref.state());
                    }
                    std::vector<float> y (2000, 0.0f);
                    rendered.render (y.data(), 2000);
                    rendered.advanceCycles (fast.cycle() > rendered.cycle() ? fast.cycle() - rendered.cycle() : 0);
                    bool finite = true;
                    for (const float v : y)
                        finite = finite && std::isfinite (v);
                    CHECK (finite);
                }
}

TEST_CASE ("render: narrow pulses at small-denominator frequencies, rectangular DFT over whole periods (report and assertion)", "[.render-exact]")
{
    // The render cost review's harness (item 8), held since the alias follow-up step (8.20) to -100 dB with
    // 3 dB to spare: the narrowest Model B pulses (width 4095 or 1) at fundamentals with a small-denominator
    // ratio to the internal rate, where the edges fall on few sub-sample offsets. The signal is rendered over a
    // whole number of its periods (p periods in q samples, f = rate x p / q) and measured with a rectangular DFT
    // at every bin up to 20 kHz: the harmonics are exact bins, everything else is aliasing, relative to the
    // fundamental. The S2b-2 and render cost renderers gave -97.4 dB at 88200 x 3 / 73 Hz, width 4095 (the
    // output's float rounding; ChipRenderExactTests.cpp has the full scan).
    struct Case
    {
        double rate;
        int p, q;
        float width;
    };
    std::vector<Case> cases;
    for (const float w : { 4095.0f, 1.0f, 4094.0f, 2.0f })
    {
        for (const auto& pq : std::vector<std::pair<int, int>> { { 3, 73 }, { 3, 70 }, { 3, 71 }, { 2, 47 }, { 3, 69 }, { 1, 23 }, { 2, 45 } })
            cases.push_back ({ 88200.0, pq.first, pq.second, w });
        for (const auto& pq : std::vector<std::pair<int, int>> { { 2, 59 }, { 1, 25 }, { 3, 76 }, { 2, 51 } })
            cases.push_back ({ 96000.0, pq.first, pq.second, w });
    }
    struct Row
    {
        double db, hz, f;
        float width;
        double rate;
    };
    std::vector<Row> rows;
    constexpr double pi = 3.14159265358979323846;
    for (const auto& k : cases)
    {
        const double f = k.rate * k.p / k.q;
        Chip c = makeChip (Model::B, k.rate);
        setOsc (c, 0, f * 16777216.0 / 985248.0, k.width, static_cast<std::uint8_t> (ctrl::pulse | ctrl::gate));
        renderSamples (c, 3000);
        const int periods = std::max (1, 24000 / k.q);
        const int n = periods * k.q;
        const auto y = renderSamples (c, n);
        double mean = 0.0;
        for (const float v : y)
            mean += static_cast<double> (v);
        mean /= n;
        const int maxBin = static_cast<int> (20000.0 * n / k.rate);
        std::vector<double> mag (static_cast<std::size_t> (maxBin + 1));
        for (int b = 1; b <= maxBin; ++b)
        {
            std::complex<double> s = 0.0, ph = 1.0;
            const std::complex<double> rot = std::polar (1.0, -2.0 * pi * b / n);
            for (int i = 0; i < n; ++i)
            {
                s += (static_cast<double> (y[static_cast<std::size_t> (i)]) - mean) * ph;
                ph *= rot;
            }
            mag[static_cast<std::size_t> (b)] = std::abs (s);
        }
        const int fundamental = periods * k.p; // the bin of the fundamental
        double worst = 0.0;
        int at = 0;
        for (int b = 1; b <= maxBin; ++b)
            if (b % fundamental != 0 && mag[static_cast<std::size_t> (b)] > worst)
            {
                worst = mag[static_cast<std::size_t> (b)];
                at = b;
            }
        rows.push_back ({ 20.0 * std::log10 (worst / mag[static_cast<std::size_t> (fundamental)] + 1e-300), at * k.rate / n, f, k.width, k.rate });
    }
    std::sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b) { return a.db > b.db; });
    std::printf ("%10s %10s %8s %12s %10s\n", "rate", "f (Hz)", "width", "worst (dB)", "at (Hz)");
    for (std::size_t i = 0; i < std::min<std::size_t> (rows.size(), 15); ++i)
        std::printf ("%10.0f %10.2f %8.0f %12.2f %10.1f\n", rows[i].rate, rows[i].f, static_cast<double> (rows[i].width), rows[i].db, rows[i].hz);
    for (const auto& r : rows)
    {
        INFO ("width " << r.width << " at " << r.f << " Hz, " << r.rate << " Hz internal: " << r.db << " dB at " << r.hz << " Hz");
        CHECK (r.db <= -103.0); // -100 dB (8.4.5) with the 3 dB margin of 8.17
    }
}

TEST_CASE ("chip: an envelope whose counter step is still pending is not inert (a sustain write in the one-cycle window)", "[bside][chip][equivalence]")
{
    // Decaying at the fastest rate, the sustain register is written to the counter's own level right after
    // every step around a counter step: where that step is already in its pipeline, the counter still moves,
    // so the envelope must not count as inert (its long skip and the batched path's stability depend on it).
    for (const Model m : { Model::A, Model::B })
    {
        Config cfg;
        cfg.model = m;
        Chip base;
        base.prepare (cfg);
        base.advanceCycles (3000000); // the power-on envelopes decay to 0 and freeze: the batched path can run
        REQUIRE (base.envelope (1) == 0);
        REQUIRE (base.envelope (2) == 0);
        base.setOscillator (0, input (3000.0, 2048.0f, ctrl::saw | ctrl::gate, 0x00, 0xf0));
        while (base.envelope (0) != 0xff)
            base.stepCycle();
        base.setOscillator (0, input (3000.0, 2048.0f, ctrl::saw | ctrl::gate, 0x00, 0x00)); // decay towards 0
        int tried = 0, windows = 0;
        for (int i = 0; i < 6000 && tried < 400; ++i)
        {
            base.stepCycle();
            const std::uint8_t counter = base.envelope (0);
            if (counter % 0x11 != 0 || counter == 0)
                continue;
            ++tried;
            Chip fast = base, ref = base;
            const auto sr = static_cast<std::uint8_t> ((counter / 0x11) << 4);
            fast.write (0x06, sr);
            ref.write (0x06, sr);
            const auto written = fast.state();
            const auto& envelopes = written.env;
            windows += envelopes[0].stepPipeline != 0 ? 1 : 0;
            fast.advanceCycles (3000);
            for (int k = 0; k < 3000; ++k)
                ref.stepCycle();
            INFO ((m == Model::A ? "Model A" : "Model B") << ", sustain written at counter " << int (counter) << ", cycle " << base.cycle());
            REQUIRE (fast.state() == ref.state());
        }
        INFO (tried << " writes, " << windows << " of them with the counter step pending");
        CHECK (tried >= 50);
        CHECK (windows >= 3);
    }
    // Rendered: the batched path takes an envelope's stability from envInert. Sample by sample, the same write
    // at a sample boundary that falls in the window, then the batched and the general path over 256 samples.
    int windows = 0;
    for (const auto& rc : rateConfigs())
        for (const std::uint8_t decay : { std::uint8_t { 0 }, std::uint8_t { 1 }, std::uint8_t { 2 }, std::uint8_t { 3 } })
    {
        const double rate = rc.internalRate;
        Chip base = makeChip (Model::B, rate);
        base.advanceCycles (3000000);
        base.setOscillator (0, input (3000.0, 2048.0f, ctrl::saw | ctrl::gate, 0x00, 0xf0));
        std::vector<float> y (256, 0.0f);
        while (base.envelope (0) != 0xff)
            base.render (y.data(), 1);
        base.setOscillator (0, input (3000.0, 2048.0f, ctrl::saw | ctrl::gate, decay, 0x00));
        for (int i = 0; i < 40000 && base.envelope (0) != 0; ++i)
        {
            base.render (y.data(), 1);
            const std::uint8_t counter = base.envelope (0);
            const auto now = base.state();
            const auto& envelopes = now.env;
            if (counter % 0x11 != 0 || counter == 0 || envelopes[0].stepPipeline == 0)
                continue;
            ++windows;
            Chip r1 = base, r2 = base;
            const auto sr = static_cast<std::uint8_t> ((counter / 0x11) << 4);
            r1.write (0x06, sr);
            r2.write (0x06, sr);
            r2.renderBatchingEnabled (false);
            std::vector<float> y1 (256, 0.0f), y2 (256, 0.0f);
            r1.render (y1.data(), 256);
            r2.render (y2.data(), 256);
            INFO (rate << " Hz, decay " << int (decay) << ", sustain written at counter " << int (counter));
            REQUIRE (std::memcmp (y1.data(), y2.data(), y1.size() * sizeof (float)) == 0);
            REQUIRE (r1.state() == r2.state());
        }
    }
    {
        INFO (windows << " sample boundaries in the window");
        CHECK (windows >= 10);
    }
    // ... and in a sample's last cycle (render cost review, round 3): renderUntil to the sample's end, which
    // keeps the sample in progress, where the counter step is one cycle from landing (step pipeline 1), and the
    // sustain written to the counter's level there: the batched path takes over right after the sample. The review's counter-example: Model B, 96 kHz, decay 3, counter 0xaa at cycle 8928. With envInert
    // ignoring the pending step, this case differs from the chip stepped cycle by cycle.
    int lastCycleWindows = 0;
    for (const auto& rc : rateConfigs())
        for (const std::uint8_t decay : { std::uint8_t { 0 }, std::uint8_t { 1 }, std::uint8_t { 2 }, std::uint8_t { 3 } })
            for (const bool settled : { false, true })
        {
            const double rate = rc.internalRate;
            Chip base = makeChip (Model::B, rate);
            if (settled)
                base.advanceCycles (3000000); // the other envelopes frozen at 0: the batched path can run
            base.setOscillator (0, input (3000.0, 2048.0f, ctrl::saw | ctrl::gate, 0x00, 0xf0));
            std::vector<float> y (256, 0.0f);
            while (base.envelope (0) != 0xff)
                base.render (y.data(), 1);
            base.setOscillator (0, input (3000.0, 2048.0f, ctrl::saw | ctrl::gate, decay, 0x00));
            for (int i = 0; i < 40000 && base.envelope (0) != 0; ++i)
            {
                Chip probe = base;
                probe.render (y.data(), 1);
                const std::uint64_t last = probe.cycle(); // the sample's end: renderUntil keeps it in progress
                Chip at = base;
                REQUIRE (at.renderUntil (y.data(), 1, last) == 0);
                REQUIRE (at.cycle() == last);
                const std::uint8_t counter = at.envelope (0);
                const auto now = at.state();
                const auto& envelopes = now.env;
                base.render (y.data(), 1);
                if (counter % 0x11 != 0 || counter == 0 || envelopes[0].stepPipeline != 1)
                    continue;
                ++lastCycleWindows;
                Chip r1 = at, r2 = at, d = at;
                const auto sr = static_cast<std::uint8_t> ((counter / 0x11) << 4);
                for (Chip* x : { &r1, &r2, &d })
                    x->write (0x06, sr);
                r2.renderBatchingEnabled (false);
                std::vector<float> y1 (256, 0.0f), y2 (256, 0.0f);
                r1.render (y1.data(), 256);
                r2.render (y2.data(), 256);
                while (d.cycle() < r1.cycle())
                    d.stepCycle();
                INFO (rate << " Hz, decay " << int (decay) << ", sustain written at counter " << int (counter) << " in the last cycle of a sample, cycle " << last);
                REQUIRE (std::memcmp (y1.data(), y2.data(), y1.size() * sizeof (float)) == 0);
                REQUIRE (r1.state() == d.state());
                REQUIRE (r2.state() == d.state());
            }
        }
    INFO (lastCycleWindows << " last cycles of a sample in the window");
    CHECK (lastCycleWindows >= 4);
}

TEST_CASE ("render: the Model A converter bits rendered as square waves follow the frequency (none above half the rate)", "[render][hysteresis]")
{
    // Taken afresh (a change of waveform), the lowest rendered bit toggles less than once per sample and the one
    // below it would toggle at least once (8.4.5: the bits whose square waves stay below half the internal rate)
    for (const double rate : { 88200.0, 96000.0, 192000.0 })
        for (const std::uint8_t wave : { std::uint8_t { 0x11 }, std::uint8_t { 0x21 } })
            for (double f = 3.0; f < 70000.0; f *= 1.37)
            {
                Chip c = makeChip (Model::A, rate);
                setOsc (c, 0, f, 2048.0f, ctrl::pulse | ctrl::gate);
                renderSamples (c, 4);
                setOsc (c, 0, f, 2048.0f, wave);
                renderSamples (c, 4);
                const double pps = static_cast<double> (c.increment (0)) / 72057594037927936.0 * cyclesPerSample (c);
                const int imin = c.renderLowestConverterBit (0);
                INFO (rate << " Hz, control " << int (wave) << ", frequency " << f << ": lowest bit " << imin << ", periods per sample " << pps);
                REQUIRE (imin >= 0);
                REQUIRE (imin <= 11);
                if (imin < 11)
                    CHECK (pps * std::ldexp (1.0, 12 - imin) < 1.0);
                if (imin > 0)
                    CHECK (pps * std::ldexp (1.0, 13 - imin) >= 1.0);
            }
}

namespace
{
/** One write of a split test's schedule (kinds as in ChipRenderStreamTests' schedules). */
struct SplitWrite
{
    std::uint64_t at = 0;
    int kind = 0, n = 0;
    double f = 0.0;
    float pw = 0.0f;
    std::uint8_t ct = 0, ad = 0, sr = 0, reg = 0, value = 0;
};
struct SplitCase
{
    Config cfg;
    std::array<OscInput, 3> initial {};
    std::vector<SplitWrite> writes;
    std::uint64_t total = 120000;
};

/** A random patch and schedule: both models and shape sets, every internal rate, Authentic in a quarter, sync and
    ring without chains (as the generator writes them), every write kind; in a fifth of the cases frequencies up
    to 12 kHz (Smooth: above the register's top, the step moments). */
SplitCase splitCase (std::uint64_t seed)
{
    bst::Rng rng (seed);
    SplitCase s;
    s.cfg.model = rng.chance (50) ? Model::A : Model::B;
    s.cfg.shapes = rng.chance (33) ? ShapeSet::placeholder : ShapeSet::fitted;
    s.cfg.internalRate = rateConfigs()[rng.below (7)].internalRate;
    s.cfg.authentic = rng.chance (25);
    const bool high = ! s.cfg.authentic && rng.chance (20);
    const bool syncRing = rng.chance (60);
    std::array<std::uint8_t, 3> ct {};
    auto control = [&] (int n) {
        auto c = static_cast<std::uint8_t> ((rng.below (16) << 4) | (syncRing && rng.chance (40) ? ctrl::sync : 0) | (syncRing && rng.chance (30) ? ctrl::ring : 0)
                                            | (rng.chance (85) ? ctrl::gate : 0) | (rng.chance (3) ? ctrl::test : 0));
        // no sync chain: a synced oscillator's source and target are not synced
        const int src = vs::bside::chip::detail::syncSource (n), tgt = vs::bside::chip::detail::syncTarget (n);
        if ((c & ctrl::sync) != 0 && ((ct[static_cast<std::size_t> (src)] & ctrl::sync) != 0 || (ct[static_cast<std::size_t> (tgt)] & ctrl::sync) != 0))
            c = static_cast<std::uint8_t> (c & ~ctrl::sync);
        return c;
    };
    auto freq = [&] {
        if (high)
            return 60000.0 + rng.below (150000);
        const int r = rng.range (0, 4);
        return r == 0 ? static_cast<double> (rng.below (65536)) : r == 1 ? 60000.0 + rng.below (5536) + 0.37 : r == 2 ? 2000.0 + rng.below (30000) * 1.0001
               : r == 3 ? rng.below (2000) + 0.5 : 0.0;
    };
    std::array<double, 3> f {};
    std::array<float, 3> pw {};
    std::array<std::uint8_t, 3> ad {}, sr {};
    for (int n = 0; n < 3; ++n)
    {
        const auto ni = static_cast<std::size_t> (n);
        ct[ni] = control (n);
        f[ni] = freq();
        pw[ni] = static_cast<float> (rng.below (4096));
        ad[ni] = static_cast<std::uint8_t> (rng.below (256));
        sr[ni] = static_cast<std::uint8_t> (rng.below (256));
        s.initial[ni] = input (f[ni], pw[ni], ct[ni], ad[ni], sr[ni]);
    }
    std::uint64_t at = 3000;
    while (true)
    {
        at += static_cast<std::uint64_t> (rng.chance (33) ? rng.range (30, 600) : rng.range (300, 8000));
        if (at + 2000 > s.total)
            break;
        SplitWrite w;
        w.at = at;
        w.n = static_cast<int> (rng.below (3));
        const auto ni = static_cast<std::size_t> (w.n);
        const unsigned r = rng.below (100);
        w.kind = r < 20 ? 0 : r < 35 ? 1 : r < 50 ? 2 : r < 60 ? 3 : r < 85 ? 4 : r < 95 ? 6 : 5;
        if (w.kind == 1)
            f[ni] = rng.chance (20) ? freq() : rng.below (65536) + 0.25;
        else if (w.kind == 2)
            pw[ni] = static_cast<float> (rng.below (4096));
        else if (w.kind == 3)
        {
            ad[ni] = static_cast<std::uint8_t> (rng.below (256));
            sr[ni] = static_cast<std::uint8_t> (rng.below (256));
        }
        else if (w.kind == 0)
            ct[ni] = control (w.n);
        else if (w.kind == 4)
        {
            w.reg = static_cast<std::uint8_t> (rng.chance (85) ? 7 * w.n + static_cast<int> (rng.below (7)) : 0x15 + static_cast<int> (rng.below (4)));
            w.value = static_cast<std::uint8_t> (rng.below (256));
            if (w.reg % 7 == 4 && w.reg < 21)
            {
                // a raw control byte, without a sync chain
                const int src = vs::bside::chip::detail::syncSource (w.n), tgt = vs::bside::chip::detail::syncTarget (w.n);
                if ((w.value & ctrl::sync) != 0 && ((ct[static_cast<std::size_t> (src)] & ctrl::sync) != 0 || (ct[static_cast<std::size_t> (tgt)] & ctrl::sync) != 0))
                    w.value = static_cast<std::uint8_t> (w.value & ~ctrl::sync);
                ct[ni] = w.value;
            }
        }
        w.f = f[ni];
        w.pw = pw[ni];
        w.ct = ct[ni];
        w.ad = ad[ni];
        w.sr = sr[ni];
        s.writes.push_back (w);
    }
    return s;
}

void applySplitWrite (Chip& c, const SplitWrite& w)
{
    if (w.kind == 4)
        c.write (w.reg, w.value);
    else if (w.kind == 5)
        c.powerOnKeepingTime();
    else if (w.kind == 6)
        c.clearEnvelopeCounters (w.n);
    else
        c.setOscillator (w.n, input (w.f, w.pw, w.ct, w.ad, w.sr));
}

/** Renders a case. splitSeed 0: render() up to just before each write and renderUntil to it; else, besides, stops of
    renderUntil at random cycles that are not write cycles (1 to 3000 cycles ahead) and random block sizes. */
std::vector<float> renderSplit (const SplitCase& s, std::uint64_t splitSeed, bool batching = true)
{
    Chip c;
    c.prepare (s.cfg);
    c.renderBatchingEnabled (batching);
    for (int n = 0; n < 3; ++n)
        c.setOscillator (n, s.initial[static_cast<std::size_t> (n)]);
    bst::Rng rng (splitSeed + 1);
    std::vector<float> y;
    std::vector<float> block (512);
    std::size_t w = 0;
    const double cps = cyclesPerSample (c);
    while (c.cycle() < s.total)
    {
        const std::uint64_t at = w < s.writes.size() ? s.writes[w].at : s.total;
        while (c.cycle() < at)
        {
            std::fill (block.begin(), block.end(), 0.0f);
            int done = 0;
            if (splitSeed != 0 && rng.chance (60))
            {
                const std::uint64_t stop = std::min (at, c.cycle() + 1 + rng.below (3000));
                done = c.renderUntil (block.data(), 1 + static_cast<int> (rng.below (512)), stop);
            }
            else
            {
                const int fit = static_cast<int> ((static_cast<double> (at - c.cycle()) - 2.0) / cps);
                if (fit >= 1)
                {
                    done = std::min (fit, 256);
                    c.render (block.data(), done);
                }
                else
                    done = c.renderUntil (block.data(), 512, at);
            }
            y.insert (y.end(), block.begin(), block.begin() + done);
        }
        if (w < s.writes.size())
            applySplitWrite (c, s.writes[w++]);
    }
    return y;
}

/** The first index where two renders differ (or the shorter length), -1 if equal. */
long firstDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    const std::size_t n = std::min (a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        if (std::memcmp (&a[i], &b[i], sizeof (float)) != 0)
            return static_cast<long> (i);
    return a.size() == b.size() ? -1 : static_cast<long> (n);
}

void checkSplits (std::uint64_t firstSeed, int count)
{
    int high = 0;
    for (std::uint64_t seed = firstSeed; seed < firstSeed + static_cast<std::uint64_t> (count); ++seed)
    {
        const SplitCase s = splitCase (seed);
        const auto plain = renderSplit (s, 0);
        const auto split = renderSplit (s, seed * 31u + 7u);
        const long at = firstDifference (plain, split);
        INFO ("seed " << seed << ", " << (s.cfg.model == Model::A ? "Model A" : "Model B") << ", " << s.cfg.internalRate << " Hz, "
                      << (s.cfg.shapes == ShapeSet::fitted ? "fitted" : "placeholder") << (s.cfg.authentic ? ", Authentic" : "") << ": first difference at sample "
                      << at << " (of " << plain.size() << ")");
        CHECK (at == -1);
        high += s.initial[0].frequency > 65535.0 || s.initial[1].frequency > 65535.0 ? 1 : 0;
    }
    CHECK (high >= count / 10);
}
} // namespace

TEST_CASE ("render: the output does not depend on where renderUntil stops without a write (random cases, below and above the register's top)", "[render][batch][split][slow]")
{
    // 8.17: any block split gives the same output. A stop inside a sample that no write follows must change
    // nothing (the program engine stops at every tick); render cost review, items 1 and 2.
    checkSplits (1, 150);
}

TEST_CASE ("render: the output does not depend on where renderUntil stops without a write, long run", "[.render-split-long]")
{
    const auto e = testenv::get ("BS_SPLIT_SEEDS");
    checkSplits (1000, e ? std::atoi (e->c_str()) : 4000);
}

TEST_CASE ("render: split cases the S2b-2 renderer got wrong (stops without a write)", "[render][batch][split]")
{
    // Cases of the generator above whose output changed with renderUntil stops at non-write cycles before the
    // pieces were aligned to the sample start and to writes (render cost review, items 1 and 2): 1071 (Model A,
    // placeholder shapes, a pulling triangle+saw), 6079 (Model B, a synced saw+pulse), 10475 (Model A, 384 kHz,
    // Authentic, a synced triangle, a pulling triangle+saw+pulse) below the register's top; 27, 37, 131, 235 with
    // the step moments above it.
    for (const std::uint64_t seed : { 1071u, 6079u, 10475u, 27u, 37u, 131u, 235u })
    {
        const SplitCase s = splitCase (seed);
        for (const std::uint64_t split : { seed * 31u + 7u, std::uint64_t { 99 }, std::uint64_t { 12345 } })
        {
            INFO ("seed " << seed << ", split " << split);
            CHECK (firstDifference (renderSplit (s, 0), renderSplit (s, split)) == -1);
        }
    }
}

namespace
{
struct AboveAlias
{
    double strongestDb = -300.0; // the strongest line in 0 .. 20 kHz, dB re 1
    double aliasDb = -300.0;     // the worst component off the lines, dB re the strongest line
    double hz = 0.0;
};

/** A combined waveform (control c, width pw) above the register's range on oscillator 2: plain, synced by
    oscillator 1 (variant 1) or as a ring target of it (variant 2); every line a multiple of the source's
    frequency (ring: a third of the target's increment, exactly; sync: the target at 2.71 times the source's
    increment, not an integer ratio: at an exact integer ratio the rendering alternates between source periods,
    an older artefact recorded in docs/decisions.md and reported by the hidden [.render-sync-integer]).
    7-term Blackman-Harris, not bin-centred. */
AboveAlias aboveAlias (Model m, std::uint8_t c, float pw, int variant, double hz, double rate, bool moments = true, double syncRatio = 2.71)
{
    Chip chip = makeChip (m, rate);
    chip.renderStepMomentsEnabled (moments);
    const double reg = std::floor (hz * 16777216.0 / 985248.0 / 3.0) * 3.0 + 0.75; // a multiple of 3 x 0.25
    const double lineReg = variant == 0 ? reg : variant == 1 ? reg / syncRatio : reg / 3.0;
    if (variant != 0)
        setOsc (chip, 0, lineReg, 2048.0f, ctrl::saw);
    const auto control = static_cast<std::uint8_t> (c | ctrl::gate | (variant == 1 ? ctrl::sync : 0) | (variant == 2 ? ctrl::ring : 0));
    setOsc (chip, 1, reg, pw, control);
    renderSamples (chip, 3000);
    const auto y = renderSamples (chip, 1 << 15);
    const auto mag = windowedMagnitudes (y);
    const double lineHz = lineReg * 985248.0 / 16777216.0;
    // the strongest line and the worst component off the lines, both outside DC's main lobe (+-7 bins): a quiet
    // shape's DC offset must not stand in for its strongest line (third review)
    const double binHz = rate / static_cast<double> (y.size());
    constexpr double guard = 9.0;
    double strongest = 0.0;
    AboveAlias a;
    for (std::size_t b = static_cast<std::size_t> (guard) + 1; b < mag.size() && static_cast<double> (b) * binHz <= 20000.0; ++b)
        strongest = std::max (strongest, mag[b]);
    for (std::size_t b = static_cast<std::size_t> (guard) + 1; b < mag.size() && static_cast<double> (b) * binHz <= 20000.0; ++b)
    {
        const double f = static_cast<double> (b) * binHz;
        if (std::abs (f - std::round (f / lineHz) * lineHz) / binHz <= guard)
            continue;
        const double db = 20.0 * std::log10 (mag[b] / strongest + 1e-300);
        if (db > a.aliasDb)
        {
            a.aliasDb = db;
            a.hz = f;
        }
    }
    a.strongestDb = 20.0 * std::log10 (2.0 * strongest / (static_cast<double> (y.size()) * 0.27105140069342) + 1e-300);
    return a;
}

/** The strongest line (dB re 1) above which a combined waveform is held to -80 dB re that line, and the one
    down to which quieter rows are printed and recorded (third review: a Model A triangle+saw synced at 12 kHz,
    88.2 kHz, strongest line about -60 dB re 1, aliases -76 dB re that line from the moments, -136 dB re 1;
    three parts per sample would settle it, at 228 ns for comb12kA, over the 200 ns target). */
constexpr double kAboveAliasAssertedDb = -50.0;
constexpr double kAboveAliasRecordedDb = -62.0;

/** Over a grid: the worst row whose strongest line is above `assertedDb` re 1 (held to -80 dB re that line),
    and the worst of the quieter rows down to kAboveAliasRecordedDb (recorded, not held). */
struct AboveAliasWorst
{
    double asserted = -300.0, quiet = -300.0;
    int rows = 0, quietRows = 0;
};
AboveAliasWorst worstAboveAlias (const std::vector<double>& rates, const std::vector<double>& freqs, const std::vector<float>& widths, double assertedDb, bool print)
{
    AboveAliasWorst w;
    for (const Model m : { Model::A, Model::B })
        for (const std::uint8_t c : { std::uint8_t { 0x30 }, std::uint8_t { 0x50 }, std::uint8_t { 0x60 }, std::uint8_t { 0x70 } })
            for (const float pw : widths)
                for (const int variant : { 0, 1, 2 })
                    for (const double rate : rates)
                        for (const double hz : freqs)
                        {
                            if ((c & ctrl::pulse) == 0 && pw > 1.0f)
                                continue; // the width matters only with the pulse
                            const auto a = aboveAlias (m, c, pw, variant, hz, rate);
                            if (a.strongestDb < kAboveAliasRecordedDb)
                                continue;
                            const bool held = a.strongestDb >= assertedDb;
                            if (print && (a.aliasDb > -85.0 || ! held))
                                std::printf ("%c %02x width %4.0f %-6s %6.0f Hz %7.0f Hz: strongest %6.1f dB, alias %6.1f dB at %7.0f Hz (one by one %6.1f dB)%s\n", m == Model::A ? 'A' : 'B', c,
                                             static_cast<double> (pw), variant == 0 ? "plain" : variant == 1 ? "synced" : "ring", rate, hz, a.strongestDb, a.aliasDb, a.hz,
                                             aboveAlias (m, c, pw, variant, hz, rate, false).aliasDb, held ? "" : " (quiet: recorded)");
                            if (held)
                            {
                                w.asserted = std::max (w.asserted, a.aliasDb);
                                ++w.rows;
                            }
                            else
                            {
                                w.quiet = std::max (w.quiet, a.aliasDb);
                                ++w.quietRows;
                            }
                        }
    return w;
}
} // namespace

TEST_CASE ("render cost: the step moments alias at most -80 dB re the strongest line above the register's top", "[render][levels][alias][slow]")
{
    // The budget row of 8.4.5 for combined waveforms extended upwards (render cost review): combined waveforms
    // alias at most -80 dB re their strongest line in 0 .. 20 kHz, measured not bin-centred; plain, synced (a
    // non-integer ratio) and ring targets alike. A subset of the hidden grid below (both models, every
    // combination). Held where the strongest line is above kAboveAliasAssertedDb (-50 dB re 1); quieter rows
    // down to about -60 dB are printed by the hidden test and recorded in docs/decisions.md.
    const auto w = worstAboveAlias ({ 88200.0, 96000.0 }, { 4100.0, 5300.0, 9300.0, 12000.0 }, { 1.0f, 64.0f, 2048.0f, 4095.0f }, kAboveAliasAssertedDb, false);
    INFO ("worst " << w.asserted << " dB re the strongest line over " << w.rows << " rows; quiet rows (" << w.quietRows << ") " << w.quiet << " dB");
    CHECK (w.rows > 100);
    CHECK (w.asserted <= -80.0);
}

TEST_CASE ("render cost: the step moments' aliasing over the whole grid (report and assertion)", "[.render-above-alias]")
{
    const auto w = worstAboveAlias ({ 88200.0, 96000.0, 192000.0 }, { 4000.0, 5300.0, 7000.0, 9300.0, 12000.0 },
                                    { 1.0f, 64.0f, 1024.0f, 2048.0f, 3000.0f, 4032.0f, 4095.0f }, kAboveAliasAssertedDb, true);
    std::printf ("worst %.1f dB re the strongest line over %d rows (strongest line above %.0f dB re 1); %d quieter rows: worst %.1f dB (recorded)\n", w.asserted, w.rows, kAboveAliasAssertedDb, w.quietRows, w.quiet);
    CHECK (w.asserted <= -80.0);
}

TEST_CASE ("render: a pulling Model A saw combination above the register's top keeps its upper half's cells one by one", "[render][levels][pull]")
{
    // Render cost review, round 3: above the register's top the cell steps go by their moments, except in a
    // pulling oscillator's upper half, where a pull-down may come in any cell (placeholder shapes: the top bit
    // falls a quarter period in). No sync chain: free lines, plain. Emitting that half by moments too passed
    // every other test; here it misses the pull-downs (state mismatches, large corrections).
    int cases = 0;
    for (const ShapeSet shapes : { ShapeSet::placeholder, ShapeSet::fitted })
        for (const std::uint8_t combo : { std::uint8_t { 0x31 }, std::uint8_t { 0x51 }, std::uint8_t { 0x61 }, std::uint8_t { 0x71 } })
            for (const double hz : { 4300.0, 7000.0, 12000.0 })
                for (const double rate : { 88200.0, 96000.0 })
                {
                    Chip c;
                    Config cfg;
                    cfg.model = Model::A;
                    cfg.internalRate = rate;
                    cfg.shapes = shapes;
                    c.prepare (cfg);
                    c.renderCheckStates (true);
                    setOsc (c, 1, hz * 16777216.0 / 985248.0, 1500.0f, static_cast<std::uint8_t> (combo | ctrl::gate));
                    std::vector<float> y (4096, 0.0f);
                    c.render (y.data(), 4096);
                    INFO ((shapes == ShapeSet::fitted ? "fitted" : "placeholder") << ", control " << int (combo) << ", " << hz << " Hz at " << rate
                                                                                   << " Hz: state mismatches " << c.renderStateMismatches() << ", large corrections "
                                                                                   << c.renderLargeCorrections() << ", level switches " << c.renderMethodSwitches());
                    CHECK (c.renderStateMismatches() == 0);
                    CHECK (c.renderLargeCorrections() == 0);
                    ++cases;
                }
    CHECK (cases == 48);
}

TEST_CASE ("render: synced targets at exact integer frequency ratios (report and assertion)", "[.render-sync-integer]")
{
    // Third review of the render cost step: at an exact integer ratio between a sync source and its target, the
    // rendering alternated between source periods (a line at half the source's rate), at and below the
    // register's top too. Fixed by the alias follow-up step (8.20; the reset met the target's own wrap on the
    // same instant and every other reset lost its step, docs/decisions.md); held since then: the sync budgets of
    // 8.4.5 with 3 dB to spare up to 3.85 kHz, the -80 dB of 8.18.1 above it (strongest line above -50 dB re 1).
    // Oscillator 1 a saw with its gate off at the target's register / ratio, oscillator 2 synced, width 2048;
    // ratio 3.0001 and the 2.71 of the asserting test as controls. Prints the worst rows. The full scan of exact
    // ratios is in ChipRenderExactTests.cpp.
    struct Row
    {
        double db;
        char text[160];
    };
    std::vector<Row> rows;
    for (const Model m : { Model::A, Model::B })
        for (const std::uint8_t c : { std::uint8_t { 0x20 }, std::uint8_t { 0x30 }, std::uint8_t { 0x50 }, std::uint8_t { 0x60 }, std::uint8_t { 0x70 } })
            for (const double ratio : { 2.0, 3.0, 4.0, 5.0, 3.0001, 2.71 })
                for (const double rate : { 88200.0, 96000.0 })
                    for (const double hz : { 1000.0, 2000.0, 3000.0, 3500.0, 3848.0, 9300.0 })
                    {
                        const auto a = aboveAlias (m, c, 2048.0f, 1, hz, rate, true, ratio);
                        if (a.strongestDb < -60.0)
                            continue;
                        INFO ((m == Model::A ? "Model A" : "Model B") << " control " << int (c) << " ratio " << ratio << ", " << rate << " Hz internal, " << hz << " Hz: " << a.aliasDb << " dB re the strongest line ("
                                                                      << a.strongestDb << " dB re 1) at " << a.hz << " Hz");
                        if (hz > 3850.0)
                        {
                            if (a.strongestDb >= kAboveAliasAssertedDb)
                                CHECK (a.aliasDb <= -80.0);
                        }
                        else
                            CHECK (a.aliasDb <= (m == Model::B && c == 0x20 ? -100.0 : hz <= 2000.0 ? -96.0 : -80.0) - 3.0);
                        Row r {};
                        r.db = a.aliasDb;
                        std::snprintf (r.text, sizeof r.text, "%c %02x ratio %-6g %6.0f Hz %5.0f Hz: strongest %6.1f dB re 1, alias %6.1f dB re it at %6.0f Hz",
                                       m == Model::A ? 'A' : 'B', c, ratio, rate, hz, a.strongestDb, a.aliasDb, a.hz);
                        rows.push_back (r);
                    }
    std::sort (rows.begin(), rows.end(), [] (const Row& x, const Row& y) { return x.db > y.db; });
    for (std::size_t i = 0; i < rows.size() && i < 30; ++i)
        std::printf ("%s\n", rows[i].text);
    for (const double ratio : { 2.0, 3.0, 4.0, 5.0, 3.0001, 2.71 })
    {
        double worst = -300.0;
        for (const auto& r : rows)
        {
            char tag[32];
            std::snprintf (tag, sizeof tag, "ratio %-6g ", ratio);
            if (std::strstr (r.text, tag) != nullptr)
                worst = std::max (worst, r.db);
        }
        std::printf ("ratio %g: worst %.1f dB re the strongest line\n", ratio, worst);
    }
    SUCCEED();
}
