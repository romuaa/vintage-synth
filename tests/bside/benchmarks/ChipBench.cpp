// chip_bench (docs/architecture.md 8.1.2, 8.9.1): CPU cost of Chip::render, in nanoseconds per internal
// sample per chip, at 48, 96 and 192 kHz internal. A benchmark, not a test (no ctest entry). Cases:
//   typical  Model B: pulse (width swept every tick), saw, triangle around A3..A4, sustaining envelopes
//   worst    Model A: three oscillators at the register maximum (3.85 kHz), each synced to the next, the
//            waveform of every oscillator switched every tick (triangle, saw, pulse, combined shapes)
//   combined Model A: three combined waveforms around A3..A4 (their steps use the long kernel)
//   noise    noise with a second waveform (write-back: the core steps every cycle) on all three
//   silent, typicalA, sawsA, comb12kA/B: the S2b-2 review's cases (a silent chip; the typical patch on Model A;
//            three Model A saws; three combined waveforms at 12 kHz, which Smooth allows)
//   noisesrc, landA: the second review's cases (a noise+saw source in per-cycle mode syncing one target and
//            ringing another; Model A saw combinations at 3 kHz with a control write every tick, which
//            re-decides their pull-downs at the chip's landings)
// Each case renders 4 s of chip time in blocks of 64 samples with its writes at their exact cycles
// (renderUntil); the best of 5 runs is reported.

#include "bside/chip/Chip.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace vs::bside::chip;

namespace
{
constexpr std::uint64_t kTick = 19656;

void setOsc (Chip& c, int n, double f, float pw, std::uint8_t control)
{
    OscInput in;
    in.frequency = f;
    in.pulseWidth = pw;
    in.control = control;
    in.attackDecay = 0x09;
    in.sustainRelease = 0xa9;
    c.setOscillator (n, in);
}

template <class Setup, class Tick>
double nsPerSample (Model m, double rate, Setup&& setup, Tick&& tick, bool digitalOnly = false)
{
    double best = 1e300;
    for (int run = 0; run < 5; ++run)
    {
        Chip c;
        Config cfg;
        cfg.model = m;
        cfg.internalRate = rate;
        c.prepare (cfg);
        setup (c);
        std::vector<float> out (64, 0.0f);
        const auto samples = static_cast<std::uint64_t> (4.0 * rate);
        std::uint64_t done = 0, ticks = 1;
        const auto t0 = std::chrono::steady_clock::now();
        while (done < samples && digitalOnly)
        {
            // the same cycles without audio: takeSampleCycles + advanceCycles per sample (the digital core's share)
            const auto cyc = c.takeSampleCycles();
            const std::uint64_t until = ticks * kTick;
            if (c.cycle() + cyc >= until)
            {
                c.advanceCycles (until - c.cycle());
                tick (c, ticks++);
            }
            else
                c.advanceCycles (cyc);
            ++done;
        }
        while (done < samples)
        {
            const int got = c.renderUntil (out.data(), 64, ticks * kTick);
            done += static_cast<std::uint64_t> (got);
            if (c.cycle() == ticks * kTick)
                tick (c, ticks++);
        }
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min (best, std::chrono::duration<double, std::nano> (t1 - t0).count() / static_cast<double> (done));
        volatile float sink = out[0];
        (void) sink;
    }
    return best;
}
} // namespace

int main()
{
    std::printf ("chip_bench: ns per internal sample per chip (best of 5, 4 s of chip time each)\n");
    std::printf ("%-8s %10s %10s %10s\n", "case", "48 kHz", "96 kHz", "192 kHz");
    const double rates[] { 48000.0, 96000.0, 192000.0 };

    auto typicalSetup = [] (Chip& c) {
        setOsc (c, 0, 7480.0, 2048.0f, ctrl::pulse | ctrl::gate);   // about 440 Hz
        setOsc (c, 1, 3740.0, 0.0f, ctrl::saw | ctrl::gate);        // about 220 Hz
        setOsc (c, 2, 11220.0, 0.0f, ctrl::triangle | ctrl::gate);  // about 660 Hz
    };
    auto typicalTick = [] (Chip& c, std::uint64_t t) {
        const auto pw = static_cast<unsigned> (1024 + (t * 37) % 2048);
        c.write (0x02, static_cast<std::uint8_t> (pw & 0xff));
        c.write (0x03, static_cast<std::uint8_t> (pw >> 8));
    };
    auto worstSetup = [] (Chip& c) {
        for (int n = 0; n < 3; ++n)
            setOsc (c, n, 65535.0 - 977.0 * n, 2048.0f, ctrl::saw | ctrl::sync | ctrl::gate);
    };
    auto worstTick = [] (Chip& c, std::uint64_t t) {
        const std::uint8_t waves[] { ctrl::triangle, ctrl::saw, ctrl::pulse, ctrl::triangle | ctrl::saw, ctrl::saw | ctrl::pulse,
                                     ctrl::triangle | ctrl::pulse | ctrl::ring };
        for (int n = 0; n < 3; ++n)
            c.write (static_cast<std::uint8_t> (7 * n + 4), static_cast<std::uint8_t> (waves[(t + static_cast<std::uint64_t> (n)) % 6] | ctrl::sync | ctrl::gate));
    };
    auto noiseSetup = [] (Chip& c) {
        for (int n = 0; n < 3; ++n)
            setOsc (c, n, 20000.0 + 3000.0 * n, 2048.0f, ctrl::noise | ctrl::pulse | ctrl::gate);
    };
    auto noTick = [] (Chip&, std::uint64_t) {};
    auto combinedSetup = [] (Chip& c) {
        // Model A combined waveforms (the steps on the long kernel), about 440, 220 and 660 Hz
        setOsc (c, 0, 7480.0, 1024.0f, ctrl::triangle | ctrl::saw | ctrl::gate);
        setOsc (c, 1, 3740.0, 1024.0f, ctrl::saw | ctrl::pulse | ctrl::gate);
        setOsc (c, 2, 11220.0, 2048.0f, ctrl::triangle | ctrl::pulse | ctrl::gate);
    };

    double r[3];
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::B, rates[i], typicalSetup, typicalTick);
    std::printf ("%-8s %10.1f %10.1f %10.1f\n", "typical", r[0], r[1], r[2]);
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::B, rates[i], typicalSetup, typicalTick, true);
    std::printf ("%-8s %10.1f %10.1f %10.1f   (digital core only, no audio)\n", "typical", r[0], r[1], r[2]);
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::A, rates[i], worstSetup, worstTick);
    std::printf ("%-8s %10.1f %10.1f %10.1f\n", "worst", r[0], r[1], r[2]);
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::A, rates[i], worstSetup, worstTick, true);
    std::printf ("%-8s %10.1f %10.1f %10.1f   (digital core only, no audio)\n", "worst", r[0], r[1], r[2]);
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::A, rates[i], combinedSetup, noTick);
    std::printf ("%-8s %10.1f %10.1f %10.1f   (Model A combined waveforms: long kernel)\n", "combined", r[0], r[1], r[2]);
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::B, rates[i], noiseSetup, noTick);
    std::printf ("%-8s %10.1f %10.1f %10.1f\n", "noise", r[0], r[1], r[2]);

    // the review's cases (S2b-2): what typical and worst understate
    auto silentSetup = [] (Chip&) {};
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::B, rates[i], silentSetup, noTick);
    std::printf ("%-8s %10.1f %10.1f %10.1f   (silent chip: power-on state, nothing selected)\n", "silent", r[0], r[1], r[2]);
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::A, rates[i], typicalSetup, typicalTick);
    std::printf ("%-8s %10.1f %10.1f %10.1f   (the typical patch on Model A)\n", "typicalA", r[0], r[1], r[2]);
    auto sawsSetup = [] (Chip& c) {
        setOsc (c, 0, 7480.0, 2048.0f, ctrl::saw | ctrl::gate);
        setOsc (c, 1, 3740.0, 2048.0f, ctrl::saw | ctrl::gate);
        setOsc (c, 2, 11220.0, 2048.0f, ctrl::saw | ctrl::gate);
    };
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::A, rates[i], sawsSetup, noTick);
    std::printf ("%-8s %10.1f %10.1f %10.1f   (Model A, three saws)\n", "sawsA", r[0], r[1], r[2]);
    auto combined12k = [] (Chip& c) {
        const double f = 12000.0 * 16777216.0 / 985248.0; // Smooth allows 12 kHz (8.4.6)
        setOsc (c, 0, f, 1024.0f, ctrl::triangle | ctrl::saw | ctrl::gate);
        setOsc (c, 1, f * 0.98, 1024.0f, ctrl::saw | ctrl::pulse | ctrl::gate);
        setOsc (c, 2, f * 0.96, 2048.0f, ctrl::triangle | ctrl::pulse | ctrl::gate);
    };
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::A, rates[i], combined12k, noTick);
    std::printf ("%-8s %10.1f %10.1f %10.1f   (three combined waveforms at 12 kHz, Model A)\n", "comb12kA", r[0], r[1], r[2]);
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::B, rates[i], combined12k, noTick);
    std::printf ("%-8s %10.1f %10.1f %10.1f   (three combined waveforms at 12 kHz, Model B)\n", "comb12kB", r[0], r[1], r[2]);
    // the second review's cases
    auto noiseSrcSetup = [] (Chip& c) {
        setOsc (c, 0, 3000.0, 2048.0f, ctrl::noise | ctrl::saw | ctrl::gate);           // per-cycle source
        setOsc (c, 1, 7480.0, 2048.0f, ctrl::saw | ctrl::sync | ctrl::gate);            // synced by it
        setOsc (c, 2, 5000.0, 2048.0f, ctrl::triangle | ctrl::ring | ctrl::gate);       // ring from oscillator 2
    };
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::A, rates[i], noiseSrcSetup, noTick);
    std::printf ("%-8s %10.1f %10.1f %10.1f   (noise+saw source syncing and ringing, Model A)\n", "noisesrc", r[0], r[1], r[2]);
    auto landSetup = [] (Chip& c) {
        const double f = 3000.0 * 16777216.0 / 985248.0;
        setOsc (c, 0, f, 1024.0f, ctrl::triangle | ctrl::saw | ctrl::gate);
        setOsc (c, 1, f * 1.01, 3000.0f, ctrl::saw | ctrl::pulse | ctrl::gate);
        setOsc (c, 2, f * 0.99, 2048.0f, ctrl::triangle | ctrl::saw | ctrl::pulse | ctrl::gate);
    };
    auto landTick = [] (Chip& c, std::uint64_t t) {
        const std::uint8_t waves[] { ctrl::triangle | ctrl::saw, ctrl::saw | ctrl::pulse, ctrl::triangle | ctrl::saw | ctrl::pulse };
        for (int n = 0; n < 3; ++n)
            c.write (static_cast<std::uint8_t> (7 * n + 4), static_cast<std::uint8_t> (waves[(t + static_cast<std::uint64_t> (n)) % 3] | ctrl::gate));
    };
    for (int i = 0; i < 3; ++i)
        r[i] = nsPerSample (Model::A, rates[i], landSetup, landTick);
    std::printf ("%-8s %10.1f %10.1f %10.1f   (Model A saw combinations at 3 kHz, a control write every tick)\n", "landA", r[0], r[1], r[2]);
    return 0;
}
