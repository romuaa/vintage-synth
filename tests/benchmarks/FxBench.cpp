// CPU benchmark for the M5 effect blocks. Reports cost as % of one core in real time
// (stereo, 256-sample blocks, best of 3) for chorus, delay, reverb and all three in series at
// 48 kHz, all three at 192 kHz, and a worst case that toggles every effect off/on on every block
// (so the wake and fade paths run constantly). Budget (docs/architecture.md 5.3): all three
// together <= 1.5 % at 48 kHz.
// Run: build/mac-arm64/tests/fx_bench
#include "fx/Chorus.h"
#include "fx/Delay.h"
#include "fx/Reverb.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
constexpr int block = 256;
constexpr int seconds = 10; // audio per timed run
constexpr int repeats = 3;  // best of

volatile float gSink = 0.0f;

struct Chain
{
    bool chorus = false, delay = false, reverb = false;
};

double bench (Chain which, double fs = 48000.0, bool toggleEveryBlock = false)
{
    // Pad-like input: band-limited noise, fresh for every block (identical across runs).
    std::vector<float> srcL (static_cast<size_t> (fs)), srcR (srcL.size());
    std::uint32_t st = 1u;
    float lpL = 0.0f, lpR = 0.0f;
    for (size_t i = 0; i < srcL.size(); ++i)
    {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        lpL += 0.1f * (static_cast<float> (static_cast<std::int32_t> (st)) * 4.6e-10f - lpL);
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        lpR += 0.1f * (static_cast<float> (static_cast<std::int32_t> (st)) * 4.6e-10f - lpR);
        srcL[i] = lpL;
        srcR[i] = lpR;
    }

    double best = 1.0e9;
    for (int r = 0; r < repeats; ++r)
    {
        juce::ScopedNoDenormals noDenormals;
        vs::fx::Chorus chorus;
        vs::fx::Delay delay;
        vs::fx::Reverb reverb;
        chorus.prepare (fs, block);
        delay.prepare (fs, block);
        reverb.prepare (fs, block);
        const vs::fx::ChorusParams cp { vs::fx::ChorusMode::I, 1.0f };
        vs::fx::DelayParams dp;
        dp.timeSeconds = 0.375f;
        dp.feedback = 0.5f;
        dp.damping = 0.3f;
        dp.pingPong = true;
        dp.level = 0.4f;
        vs::fx::ReverbParams rp;
        rp.decaySeconds = 2.5f;
        rp.damping = 0.5f;
        rp.preDelaySeconds = 0.02f;
        rp.level = 0.4f;

        std::vector<float> l (block), rch (block);
        double sum = 0.0;
        const int blocks = static_cast<int> (fs) * seconds / block;
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
        {
            const auto off = static_cast<size_t> ((b * block) % (static_cast<int> (fs) - block));
            std::copy_n (srcL.begin() + static_cast<std::ptrdiff_t> (off), block, l.begin());
            std::copy_n (srcR.begin() + static_cast<std::ptrdiff_t> (off), block, rch.begin());
            const bool on = ! toggleEveryBlock || b % 2 == 0;
            auto dpb = dp;
            auto rpb = rp;
            dpb.level = on ? dp.level : 0.0f;
            rpb.level = on ? rp.level : 0.0f;
            const vs::fx::ChorusParams cpb { on ? cp.mode : vs::fx::ChorusMode::Off, cp.mix };
            if (which.chorus) { chorus.setParams (cpb); chorus.process (l.data(), rch.data(), block); }
            if (which.delay) { delay.setParams (dpb); delay.process (l.data(), rch.data(), block); }
            if (which.reverb) { reverb.setParams (rpb); reverb.process (l.data(), rch.data(), block); }
            sum += static_cast<double> (l[0] + rch[block - 1]);
        }
        const auto t1 = std::chrono::steady_clock::now();
        gSink = static_cast<float> (sum);
        best = std::min (best, std::chrono::duration<double> (t1 - t0).count());
    }
    return 100.0 * best / seconds;
}
} // namespace

int main()
{
    const double c = bench ({ true, false, false });
    const double d = bench ({ false, true, false });
    const double r = bench ({ false, false, true });
    const double all = bench ({ true, true, true });
    const double all192 = bench ({ true, true, true }, 192000.0);
    const double toggle = bench ({ true, true, true }, 48000.0, true);
    constexpr double budget = 1.5;
    std::printf ("chorus (mode I):                 %.3f%% of one core\n", c);
    std::printf ("delay (ping-pong, fb 0.5):       %.3f%% of one core\n", d);
    std::printf ("reverb (2.5 s, 16-line FDN):     %.3f%% of one core\n", r);
    std::printf ("all three in series:             %.3f%% of one core (budget %.1f%%: %s)\n",
                 all, budget, all <= budget ? "ok" : "OVER");
    std::printf ("all three at 192 kHz:            %.3f%% of one core\n", all192);
    std::printf ("all three, off/on every block:   %.3f%% of one core (48 kHz; wake + fade paths)\n", toggle);
    return 0;
}
