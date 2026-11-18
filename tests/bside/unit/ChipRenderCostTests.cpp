// The render cost step (docs/architecture.md 8.18): the batched path renders exactly as the general path,
// the digital core's skip over repeating envelope states equals stepping, and the regression tests of the
// three defects 8.17 listed.

#include "FuzzSupport.h"
#include "RenderTestSupport.h"

#include <catch2/catch_test_macros.hpp>

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
