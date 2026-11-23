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
//   s4gate   the patch of the S4 CPU gate (8.5, 8.18): Model A, pulse (a width write every tick), saw and
//            the triangle+saw combined waveform, about A3..A4, sustaining envelopes
// Each case renders 4 s of chip time in blocks of 64 samples with its writes at their exact cycles
// (renderUntil); the best of 5 runs is reported. Arguments: case names to run only those (default: all).
// S3a (8.19.1) adds the filter and the output stage on their own, per internal sample, over a prepared input
// (three voices' worth of saw-like signal plus their DC, render units), a cutoff write every 64 samples:
//   filterB, filterA   Filter alone (low-pass + band-pass selected, resonance 8)
//   outputB, outputA   OutputStage alone (mixer, volume, board filters)
//   s3aB, s3aA         both together, as a chip uses them (filterBusLevel -> Filter -> OutputStage): the 7 ns
//                      budget of 8.19.1 is s3aB at 96 kHz; Model A has no budget (S4 question) and is broken
//                      down into filterA, outputA and s3aA
//   cutB, cutA         s3aB and s3aA with a new cutoff every sample through Filter::processWithCutoff
// S3b-1 (8.19.4): CHIP_BENCH_BUSES=1 runs every chip case above through Chip::renderBusesUntil (the two buses in
// double) instead of the single sum (CHIP_BENCH_NOBEND=1 also: without Model A's voice bend). The whole chain (ChipVoice: buses -> Filter -> OutputStage, double output), the S4
// gate's patch (s4gate) with oscillators 1 and 2 through the filter (low-pass, cutoff 1024, resonance 8, volume 15):
//   chainB, chainA     a cutoff write every tick (the filter runs process: setCutoff once per write)
//   moveB, moveA       a cutoff write every 64 samples (moving: Filter::processWithCutoff every sample)
//   busesB, busesA     the same patch, Chip::renderBusesUntil alone (the buses' share of chainB / chainA)
// The breakdown per stage: busesX is the renderer with its buses; chainX - busesX the filter, the stage and the voice's
// per-sample bookkeeping.

#include "../../support/Env.h"
#include "bside/chip/Chip.h"
#include "bside/chip/ChipVoice.h"
#include "bside/chip/Filter.h"
#include "bside/chip/OutputStage.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace vs::bside::chip;

namespace
{
constexpr std::uint64_t kTick = 19656;

/** Chip time per run: 4 s, or the environment variable CHIP_BENCH_SECONDS (longer runs for a profiler). */
double chipSeconds()
{
    const auto s = testenv::get ("CHIP_BENCH_SECONDS");
    const double v = s ? std::atof (s->c_str()) : 0.0;
    return v > 0.0 ? v : 4.0;
}

/** CHIP_BENCH_NOBEND=1: Model A's voices are not bent on the buses (the buses' own cost, without the bend's). */
bool noBendWanted()
{
    const auto s = testenv::get ("CHIP_BENCH_NOBEND");
    return s && *s == "1";
}

/** CHIP_BENCH_BUSES=1: the chip cases render the two buses (S3b-1). */
bool busesWanted()
{
    const auto s = testenv::get ("CHIP_BENCH_BUSES");
    return s && *s == "1";
}

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
        if (noBendWanted())
            c.renderBendVoices (false);
        std::vector<float> out (64, 0.0f);
        const auto samples = static_cast<std::uint64_t> (chipSeconds() * rate);
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
        std::vector<double> bus (6 * 64, 0.0);
        const Chip::BusBlock block { bus.data(), bus.data() + 64, bus.data() + 128, bus.data() + 192, bus.data() + 256, bus.data() + 320 };
        const bool buses = busesWanted();
        while (done < samples)
        {
            const int got = buses ? c.renderBusesUntil (block, 64, ticks * kTick) : c.renderUntil (out.data(), 64, ticks * kTick);
            done += static_cast<std::uint64_t> (got);
            if (c.cycle() == ticks * kTick)
                tick (c, ticks++);
        }
        out[0] += static_cast<float> (bus[0]);
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min (best, std::chrono::duration<double, std::nano> (t1 - t0).count() / static_cast<double> (done));
        volatile float sink = out[0];
        (void) sink;
    }
    return best;
}

/** ns per internal sample of the S3a stages (see the header): `which` 0 filter, 1 output stage, 2 both, 3 both with
    a new cutoff every sample (Filter::processWithCutoff). */
double s3aNsPerSample (Model m, double rate, int which)
{
    const auto samples = static_cast<std::size_t> (chipSeconds() * rate);
    std::vector<float> audio (4096), env (4096);
    for (std::size_t i = 0; i < audio.size(); ++i)
    {
        // three saws (incommensurate) in render units, and an envelope sum near three full voices
        const double t = static_cast<double> (i);
        audio[i] = static_cast<float> (std::fmod (t * 0.0123, 1.0) + std::fmod (t * 0.0071, 1.0) + std::fmod (t * 0.0049, 1.0) - 1.5);
        env[i] = 2.5f + 0.0001f * static_cast<float> (i % 7);
    }
    double best = 1e300;
    float sink = 0.0f;
    for (int run = 0; run < 5; ++run)
    {
        Filter f;
        f.prepare (m, rate, 0.5f);
        f.setResonance (8);
        f.setMode (0x3f);
        OutputStage o;
        o.prepare (m, rate, false);
        o.setRouting (0x03);
        o.setModeVolume (0x3f);
        o.setFilterStaticDc (f.staticOutput (o.staticFilterBusDc()));
        float acc = 0.0f;
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t done = 0; done < samples; done += 64)
        {
            f.setCutoff (static_cast<float> (600 + (done / 64) % 1000) + 0.5f);
            const std::size_t base = done % 4096;
            for (std::size_t j = 0; j < 64; ++j)
            {
                const float a = audio[(base + j) & 4095], e = env[(base + j) & 4095];
                if (which == 0)
                    acc += f.process (a);
                else if (which == 1)
                    acc += o.process (a, a, e);
                else if (which == 2)
                    acc += o.process (f.process (o.filterBusLevel (a, e)), a, e);
                else
                    acc += o.process (f.processWithCutoff (o.filterBusLevel (a, e), 600.0f + 0.37f * static_cast<float> ((done + j) % 2000)), a, e);
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min (best, std::chrono::duration<double, std::nano> (t1 - t0).count() / static_cast<double> (samples));
        sink += acc;
    }
    volatile float keep = sink;
    (void) keep;
    return best;
}
/** ns per internal sample of the whole chain (ChipVoice, Per Note) on the S4 gate's patch: `which` 0 chain with a
    cutoff write every tick, 1 a cutoff write every 64 samples (moving), 2 the buses alone. */
double chainNsPerSample (Model m, double rate, int which)
{
    double best = 1e300;
    double sink = 0.0;
    for (int run = 0; run < 5; ++run)
    {
        auto v = std::make_unique<ChipVoice>();
        Config cfg;
        cfg.model = m;
        cfg.internalRate = rate;
        v->prepare (cfg);
        Filter f;
        f.prepare (m, rate, 0.5f);
        v->setFilter (&f);
        auto osc = [&] (int n, double fr, float pw, std::uint8_t control) {
            OscInput in;
            in.frequency = fr;
            in.pulseWidth = pw;
            in.control = control;
            in.attackDecay = 0x09;
            in.sustainRelease = 0xa9;
            v->setOscillator (n, in);
        };
        osc (0, 3740.0, 2048.0f, ctrl::pulse | ctrl::gate);
        osc (1, 7480.0, 0.0f, ctrl::saw | ctrl::gate);
        osc (2, 5600.0, 2048.0f, ctrl::triangle | ctrl::saw | ctrl::gate);
        v->write (0x15, 0);
        v->write (0x16, 0x80);
        v->write (0x17, 0x83);
        v->write (0x18, 0x1f);
        std::vector<double> out (64, 0.0);
        const auto samples = static_cast<std::uint64_t> (chipSeconds() * rate);
        std::uint64_t done = 0, ticks = 1, blocks = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (done < samples)
        {
            const std::uint64_t until = ticks * kTick;
            const int got = which == 2 ? v->renderBusesUntil (64, until) : v->renderUntil (out.data(), 64, until);
            done += static_cast<std::uint64_t> (got);
            if (v->chip().cycle() == until)
            {
                const auto pw = static_cast<unsigned> (1024 + (ticks * 37) % 2048);
                v->write (0x02, static_cast<std::uint8_t> (pw & 0xff));
                v->write (0x03, static_cast<std::uint8_t> (pw >> 8));
                if (which == 0)
                    v->write (0x16, static_cast<std::uint8_t> (0x60 + (ticks * 7) % 64));
                ++ticks;
            }
            if (which == 1 && got == 64)
                v->write (0x16, static_cast<std::uint8_t> (0x60 + (++blocks * 5) % 64));
        }
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min (best, std::chrono::duration<double, std::nano> (t1 - t0).count() / static_cast<double> (done));
        sink += out[0];
    }
    volatile double keep = sink;
    (void) keep;
    return best;
}
} // namespace

int main (int argc, char** argv)
{
    // the cases named on the command line (all when none is)
    auto want = [argc, argv] (const char* name) {
        if (argc < 2)
            return true;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp (argv[i], name) == 0)
                return true;
        return false;
    };
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
    if (want ("typical"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::B, rates[i], typicalSetup, typicalTick);
        std::printf ("%-8s %10.1f %10.1f %10.1f\n", "typical", r[0], r[1], r[2]);
    }
    if (want ("typical"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::B, rates[i], typicalSetup, typicalTick, true);
        std::printf ("%-8s %10.1f %10.1f %10.1f   (digital core only, no audio)\n", "typical", r[0], r[1], r[2]);
    }
    if (want ("worst"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::A, rates[i], worstSetup, worstTick);
        std::printf ("%-8s %10.1f %10.1f %10.1f\n", "worst", r[0], r[1], r[2]);
    }
    if (want ("worst"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::A, rates[i], worstSetup, worstTick, true);
        std::printf ("%-8s %10.1f %10.1f %10.1f   (digital core only, no audio)\n", "worst", r[0], r[1], r[2]);
    }
    if (want ("combined"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::A, rates[i], combinedSetup, noTick);
        std::printf ("%-8s %10.1f %10.1f %10.1f   (Model A combined waveforms: long kernel)\n", "combined", r[0], r[1], r[2]);
    }
    if (want ("noise"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::B, rates[i], noiseSetup, noTick);
        std::printf ("%-8s %10.1f %10.1f %10.1f\n", "noise", r[0], r[1], r[2]);
    }

    // the review's cases (S2b-2): what typical and worst understate
    auto silentSetup = [] (Chip&) {};
    if (want ("silent"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::B, rates[i], silentSetup, noTick);
        std::printf ("%-8s %10.1f %10.1f %10.1f   (silent chip: power-on state, nothing selected)\n", "silent", r[0], r[1], r[2]);
    }
    if (want ("typicalA"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::A, rates[i], typicalSetup, typicalTick);
        std::printf ("%-8s %10.1f %10.1f %10.1f   (the typical patch on Model A)\n", "typicalA", r[0], r[1], r[2]);
    }
    auto sawsSetup = [] (Chip& c) {
        setOsc (c, 0, 7480.0, 2048.0f, ctrl::saw | ctrl::gate);
        setOsc (c, 1, 3740.0, 2048.0f, ctrl::saw | ctrl::gate);
        setOsc (c, 2, 11220.0, 2048.0f, ctrl::saw | ctrl::gate);
    };
    if (want ("sawsA"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::A, rates[i], sawsSetup, noTick);
        std::printf ("%-8s %10.1f %10.1f %10.1f   (Model A, three saws)\n", "sawsA", r[0], r[1], r[2]);
    }
    auto combined12k = [] (Chip& c) {
        const double f = 12000.0 * 16777216.0 / 985248.0; // Smooth allows 12 kHz (8.4.6)
        setOsc (c, 0, f, 1024.0f, ctrl::triangle | ctrl::saw | ctrl::gate);
        setOsc (c, 1, f * 0.98, 1024.0f, ctrl::saw | ctrl::pulse | ctrl::gate);
        setOsc (c, 2, f * 0.96, 2048.0f, ctrl::triangle | ctrl::pulse | ctrl::gate);
    };
    if (want ("comb12kA"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::A, rates[i], combined12k, noTick);
        std::printf ("%-8s %10.1f %10.1f %10.1f   (three combined waveforms at 12 kHz, Model A)\n", "comb12kA", r[0], r[1], r[2]);
    }
    if (want ("comb12kB"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::B, rates[i], combined12k, noTick);
        std::printf ("%-8s %10.1f %10.1f %10.1f   (three combined waveforms at 12 kHz, Model B)\n", "comb12kB", r[0], r[1], r[2]);
    }
    // the second review's cases
    auto noiseSrcSetup = [] (Chip& c) {
        setOsc (c, 0, 3000.0, 2048.0f, ctrl::noise | ctrl::saw | ctrl::gate);           // per-cycle source
        setOsc (c, 1, 7480.0, 2048.0f, ctrl::saw | ctrl::sync | ctrl::gate);            // synced by it
        setOsc (c, 2, 5000.0, 2048.0f, ctrl::triangle | ctrl::ring | ctrl::gate);       // ring from oscillator 2
    };
    if (want ("noisesrc"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::A, rates[i], noiseSrcSetup, noTick);
        std::printf ("%-8s %10.1f %10.1f %10.1f   (noise+saw source syncing and ringing, Model A)\n", "noisesrc", r[0], r[1], r[2]);
    }
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
    if (want ("landA"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::A, rates[i], landSetup, landTick);
        std::printf ("%-8s %10.1f %10.1f %10.1f   (Model A saw combinations at 3 kHz, a control write every tick)\n", "landA", r[0], r[1], r[2]);
    }
    // the S4 gate's patch (8.5, 8.18)
    auto s4Setup = [] (Chip& c) {
        setOsc (c, 0, 3740.0, 2048.0f, ctrl::pulse | ctrl::gate);                   // about 220 Hz
        setOsc (c, 1, 7480.0, 0.0f, ctrl::saw | ctrl::gate);                        // about 440 Hz
        setOsc (c, 2, 5600.0, 2048.0f, ctrl::triangle | ctrl::saw | ctrl::gate);    // about 330 Hz
    };
    if (want ("s4gate"))
    {
        for (int i = 0; i < 3; ++i)
            r[i] = nsPerSample (Model::A, rates[i], s4Setup, typicalTick);
        std::printf ("%-8s %10.1f %10.1f %10.1f   (the S4 gate's patch: Model A pulse, saw, triangle+saw)\n", "s4gate", r[0], r[1], r[2]);
    }
    // S3a: the filter and the output stage (8.19.1)
    struct S3aCase
    {
        const char* name;
        Model model;
        int which;
        const char* what;
    };
    const S3aCase s3a[] { { "filterB", Model::B, 0, "Filter alone, Model B" },       { "filterA", Model::A, 0, "Filter alone, Model A" },
                          { "outputB", Model::B, 1, "OutputStage alone, Model B" },  { "outputA", Model::A, 1, "OutputStage alone, Model A" },
                          { "s3aB", Model::B, 2, "filter + output stage, Model B (budget 7 ns at 96 kHz)" },
                          { "s3aA", Model::A, 2, "filter + output stage, Model A (no budget: S4)" },
                          { "cutB", Model::B, 3, "s3aB with a cutoff every sample (processWithCutoff)" },
                          { "cutA", Model::A, 3, "s3aA with a cutoff every sample (processWithCutoff)" } };
    for (const auto& c : s3a)
        if (want (c.name))
        {
            for (int i = 0; i < 3; ++i)
                r[i] = s3aNsPerSample (c.model, rates[i], c.which);
            std::printf ("%-8s %10.1f %10.1f %10.1f   (%s)\n", c.name, r[0], r[1], r[2], c.what);
        }
    // S3b-1: the whole chain on the S4 gate's patch (8.19.4)
    struct ChainCase
    {
        const char* name;
        Model model;
        int which;
        const char* what;
    };
    const ChainCase chains[] { { "chainB", Model::B, 0, "chain, S4 gate patch, Model B: buses -> filter -> stage" },
                               { "chainA", Model::A, 0, "chain, S4 gate patch, Model A: buses -> filter -> stage" },
                               { "moveB", Model::B, 1, "chainB with a cutoff write every 64 samples (processWithCutoff)" },
                               { "moveA", Model::A, 1, "chainA with a cutoff write every 64 samples (processWithCutoff)" },
                               { "busesB", Model::B, 2, "the buses alone, S4 gate patch, Model B" },
                               { "busesA", Model::A, 2, "the buses alone, S4 gate patch, Model A (voice bend included)" } };
    for (const auto& c : chains)
        if (want (c.name))
        {
            for (int i = 0; i < 3; ++i)
                r[i] = chainNsPerSample (c.model, rates[i], c.which);
            std::printf ("%-8s %10.1f %10.1f %10.1f   (%s)\n", c.name, r[0], r[1], r[2], c.what);
        }
    return 0;
}
