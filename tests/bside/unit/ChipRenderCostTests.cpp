// The render cost step (docs/architecture.md 8.18): the batched path renders exactly as the general path,
// the digital core's skip over repeating envelope states equals stepping, and the regression tests of the
// three defects 8.17 listed.

#include "FuzzSupport.h"
#include "RenderTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
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

TEST_CASE ("render cost: the batched path renders exactly as the general path (random patches, writes, envelopes, blocks)", "[render][batch]")
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

TEST_CASE ("render cost: the batched path, under the state check, finds no mismatch and renders as without it", "[render][batch]")
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

TEST_CASE ("chip: an envelope that cannot change skips its repeating states exactly (frozen, sustain, every divider period)", "[bside][chip][equivalence]")
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

TEST_CASE ("chip: the Model A pull-down in the fast path equals stepping (both shape sets, pulse gates, ring and sync around it)", "[bside][chip][equivalence]")
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

TEST_CASE ("render cost: the batched path renders exactly as the general path around Model A pull-downs and their landings", "[render][batch]")
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
    double aliasDb = -300.0;     // worst non-harmonic component up to 20 kHz, dB re the fundamental (coarse levels)
    double aliasFineDb = -300.0; // the same rendered cell by cell
    double bandDiffDb = -300.0;  // the largest in-band (<= 20 kHz) difference between the two, dB re full scale
};

/** One combined waveform (control c) at fundamental hz (Smooth, above the register's range) on a bin-centred
    frequency: its aliasing with and without the coarse levels, and their difference in the audio band. */
AboveResult measureAbove (Model m, std::uint8_t c, double hz, double rate)
{
    constexpr int n = 1 << 15;
    AboveResult r;
    std::vector<float> y[2];
    for (int coarse = 0; coarse < 2; ++coarse)
    {
        Chip chip = makeChip (m, rate);
        chip.renderCoarseLevelsEnabled (coarse == 1);
        const int bin = oddBin (hz, rate, n);
        setOsc (chip, 0, binFrequency (chip, bin, n), 1024.0f, static_cast<std::uint8_t> (c | ctrl::gate));
        renderSamples (chip, 4000);
        y[coarse] = renderSamples (chip, n);
        const auto a = aliasOf (y[coarse], rate, 20000.0, -1, bin); // (a Model A saw combination pulled at once repeats every half period)
        (coarse == 1 ? r.aliasDb : r.aliasFineDb) = a.worstDb;
    }
    const auto l0 = lowpass (y[0], rate, 20000.0), l1 = lowpass (y[1], rate, 20000.0);
    double worst = 0.0;
    for (std::size_t i = 1000; i + 1000 < l0.size(); ++i)
        worst = std::max (worst, std::abs (l0[i] - l1[i]));
    r.bandDiffDb = 20.0 * std::log10 (worst + 1e-300);
    return r;
}
} // namespace

TEST_CASE ("render cost: combined waveforms above the register's range, coarse levels against cell by cell (report)", "[.render-above]")
{
    // The coarse levels (RenderTables.h, kCoarseLevels) above the register's highest frequency: aliasing up to
    // 20 kHz, with and without them, and how far the audio band moves (Smooth reaches 12 kHz; no budget exists
    // above 3.85 kHz, 8.18).
    std::printf ("%-7s %-5s %9s %8s %12s %12s %14s\n", "model", "wave", "rate", "f (Hz)", "alias (dB)", "cell by cell", "band diff (dB)");
    for (const Model m : { Model::A, Model::B })
        for (const std::uint8_t c : { std::uint8_t { 0x30 }, std::uint8_t { 0x50 }, std::uint8_t { 0x60 }, std::uint8_t { 0x70 } })
            for (const double rate : { 88200.0, 96000.0, 192000.0 })
                for (const double hz : { 4000.0, 5000.0, 7000.0, 9000.0, 12000.0 })
                {
                    const auto r = measureAbove (m, c, hz, rate);
                    std::printf ("%-7s %-5x %9.0f %8.0f %12.1f %12.1f %14.1f\n", m == Model::A ? "A" : "B", c, rate, hz, r.aliasDb, r.aliasFineDb, r.bandDiffDb);
                }
}

TEST_CASE ("render cost: coarse levels only above the register's range, with hysteresis, and close to cell by cell in the band", "[render][levels]")
{
    // At and below the register's highest frequency nothing changes (no level, output bit-identical with the
    // levels off); above it a glide switches a few times, a vibrato across a level limit at most once; and the
    // coarse rendering (switches included) stays close to the cell-by-cell one in the audio band.
    for (const Model m : { Model::A, Model::B })
        for (const std::uint8_t c : { std::uint8_t { 0x30 }, std::uint8_t { 0x60 }, std::uint8_t { 0x70 } })
        {
            const double rate = 96000.0;
            Chip a = makeChip (m, rate), b = makeChip (m, rate);
            b.renderCoarseLevelsEnabled (false);
            // a glide from 1 kHz to the register's top (65535), then on to 12 kHz, a write every 2000 cycles
            const double top = 12000.0 * 16777216.0 / 985248.0;
            std::vector<float> ya, yb;
            std::vector<float> block (64);
            for (int step = 0; step <= 400; ++step)
            {
                const double f = step <= 200 ? 1000.0 * 16777216.0 / 985248.0 + (65535.0 - 1000.0 * 16777216.0 / 985248.0) * step / 200.0
                                             : 65535.0 + (top - 65535.0) * (step - 200) / 200.0;
                const std::uint64_t at = 2000u * static_cast<std::uint64_t> (step + 1);
                for (auto [chip, y] : { std::pair<Chip*, std::vector<float>*> { &a, &ya }, std::pair<Chip*, std::vector<float>*> { &b, &yb } })
                {
                    setOsc (*chip, 0, f, 1024.0f, static_cast<std::uint8_t> (c | ctrl::gate));
                    while (chip->cycle() < at)
                    {
                        std::fill (block.begin(), block.end(), 0.0f);
                        const int done = chip->renderUntil (block.data(), 64, at);
                        y->insert (y->end(), block.begin(), block.begin() + done);
                    }
                }
                if (step == 200)
                {
                    INFO ((m == Model::A ? "Model A" : "Model B") << ", control " << int (c) << ": up to the register's top");
                    CHECK (a.renderLevelSwitches() == 0);
                    CHECK (std::memcmp (ya.data(), yb.data(), ya.size() * sizeof (float)) == 0);
                }
            }
            INFO ((m == Model::A ? "Model A" : "Model B") << ", control " << int (c) << ": " << a.renderLevelSwitches() << " level switches on the glide");
            CHECK (a.renderLevelSwitches() >= 2);
            CHECK (a.renderLevelSwitches() <= 12);
            const auto la = lowpass (ya, rate, 20000.0), lb = lowpass (yb, rate, 20000.0);
            double worst = 0.0;
            for (std::size_t i = 1000; i + 1000 < la.size(); ++i)
                worst = std::max (worst, std::abs (la[i] - lb[i]));
            UNSCOPED_INFO ((m == Model::A ? "Model A" : "Model B") << ", control " << int (c) << ": in-band difference on the glide " << 20.0 * std::log10 (worst + 1e-300) << " dB");
            CHECK (worst < 0.05);
            // a vibrato of +-1 % around 6 kHz: at most one switch
            Chip v = makeChip (m, rate);
            setOsc (v, 0, 6000.0 * 16777216.0 / 985248.0, 1024.0f, static_cast<std::uint8_t> (c | ctrl::gate));
            renderSamples (v, 2000);
            const auto before = v.renderLevelSwitches();
            for (int i = 0; i < 400; ++i)
            {
                const double f = 6000.0 * (1.0 + 0.01 * std::sin (i * 0.3)) * 16777216.0 / 985248.0;
                setOsc (v, 0, f, 1024.0f, static_cast<std::uint8_t> (c | ctrl::gate));
                renderSamples (v, 64);
            }
            CHECK (v.renderLevelSwitches() - before <= 1);
        }
}

TEST_CASE ("chip: a Model A saw combination at frequency 0 or below one accumulator step, both halves (no division by a zero increment)", "[bside][chip][equivalence]")
{
    // The closed form of the pull-down (pullRun) divided by the increment: frequency 0 (a control written
    // before any frequency, a trace) crashed on x86-64. Every saw combination, with and without the pulse, the
    // accumulator in either half, frequency 0 and Smooth increments below one accumulator step; the fast path
    // against stepping, and rendering (the sanitizer build runs this too).
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

TEST_CASE ("render: narrow pulses at small-denominator frequencies, rectangular DFT over whole periods (report)", "[.render-exact]")
{
    // A harness for the next step (render cost review, item 8; no assertion): the narrowest Model B pulses
    // (width 4095 or 1) at fundamentals with a small-denominator ratio to the internal rate, where the edges
    // fall on few sub-sample offsets. The signal is rendered over a whole number of its periods (p periods in q
    // samples, f = rate x p / q) and measured with a rectangular DFT at every bin up to 20 kHz: the harmonics
    // are exact bins, everything else is aliasing, relative to the fundamental. The S2b-2 renderer gives the
    // same figures (-97.4 dB at 88200 x 3 / 73 Hz, width 4095).
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
}
