// Hard sync and Cross Mod (wave 3 oscillator fixes):
// 1. the alias gate (<= -100 dB in 0..20 kHz up to a 5 kHz fundamental) for hard sync without
//    Cross Mod, for every VCO-1 waveform, several VCO-1/VCO-2 ratios and every rate configuration;
// 2. a measured regression bound for Classic Cross Mod, which is exempt from the -100 dB gate by
//    owner decision (its aliasing is part of the approved sound);
// 3. offline renders of Cross Mod patches sound like real-time renders (partials and null).
// Hidden: an audition / reference render ("[.xmodrender]") and a sync harmonics dump ("[.diag]").
#include <catch2/catch_test_macros.hpp>

#include "CrossModMeasure.h"
#include "TestHelpers.h"
#include "dsp/FastMath.h"
#include "dsp/Oscillator.h"
#include "engine/Oversampler.h"
#include "engine/Part.h"
#include "params/Mappings.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <vector>

using vs::dsp::Wave;

namespace
{
struct AliasResult
{
    double worstDb;
    double worstHz;
};

/**
    Hard sync as Voice::renderSample does it (no Cross Mod): VCO-2 (saw) is the master at a
    bin-centred frequency, VCO-1 is reset at every master wrap at the exact sub-sample point.
    The output repeats every master period, so every legitimate component sits on a multiple of the
    master frequency; anything else in the audio band is aliasing. Rendered at rate x factor and
    taken back to `rate` through the production path (the voice decimator, nothing at 1x).
    If harmonics is not null, it receives the on-grid levels (dB re the strongest).
*/
AliasResult measureSyncAliasing (Wave slaveWave, float width, double hz, double slaveRatio, double rate, int factor,
                                 std::vector<double>* harmonics = nullptr)
{
    constexpr int order = 15, n = 1 << order, block = 512;
    int k = static_cast<int> (std::lround (hz * n / rate));
    k = (k % 2 == 0) ? k + 1 : k;
    auto os = factor > 1 ? vs::makeVoiceOversampler (factor) : nullptr;
    if (os != nullptr)
        os->initProcessing (block);
    juce::AudioBuffer<float> silence (1, block), out (1, block);
    silence.clear();

    vs::dsp::Oscillator master, slave;
    master.setWave (Wave::Saw);
    slave.setWave (slaveWave);
    master.reset();
    slave.reset();
    const float dt2 = static_cast<float> (k) / static_cast<float> (factor * n);
    const float dt1 = static_cast<float> (static_cast<double> (dt2) * slaveRatio);
    auto sample = [&] {
        float wrap2 = -1.0f, unused = -1.0f;
        master.process (dt2, 0.5f, -1.0f, wrap2);
        return slave.process (std::min (dt1, 0.45f), width, wrap2, unused);
    };

    std::vector<float> captured;
    const int warmup = 2048; // the BLEP ring and the IIR decimator settle within a few hundred samples
    for (int done = 0; done < warmup + n; done += block)
    {
        if (os != nullptr)
        {
            auto up = os->processSamplesUp (juce::dsp::AudioBlock<float> (silence));
            float* p = up.getChannelPointer (0);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
                p[i] = sample();
            juce::dsp::AudioBlock<float> ob (out);
            os->processSamplesDown (ob);
        }
        else
        {
            float* p = out.getWritePointer (0);
            for (int i = 0; i < block; ++i)
                p[i] = sample();
        }
        if (done >= warmup)
            captured.insert (captured.end(), out.getReadPointer (0), out.getReadPointer (0) + block);
    }
    const auto mag = vs::test::magnitudeSpectrum (captured, order);
    const double band = std::min (20000.0, rate * 20.0 / 48.0);
    double ref = 0.0; // the strongest harmonic: the reference level
    for (size_t b = static_cast<size_t> (k); b < mag.size(); b += static_cast<size_t> (k))
        ref = std::max (ref, static_cast<double> (mag[b]));
    AliasResult r { -300.0, 0.0 };
    for (size_t b = 1; b < mag.size(); ++b)
    {
        const double f = static_cast<double> (b) * rate / n;
        if (f > band)
            break;
        const double db = vs::test::toDb (mag[b] / ref);
        if (b % static_cast<size_t> (k) == 0)
        {
            if (harmonics != nullptr)
                harmonics->push_back (db);
            continue;
        }
        if (db > r.worstDb)
            r = { db, f };
    }
    return r;
}

struct RateCfg
{
    double rate;
    int factor;
    const char* name;
};
// Real time: 48 kHz 2x (through the real decimator), 88.2 and 96 kHz 1x (no decimator).
// Offline: 48 kHz 4x, 96 kHz 2x.
constexpr RateCfg kSyncCfgs[] { { 48000.0, 2, "48k 2x (real time)" }, { 88200.0, 1, "88.2k 1x (real time)" },
                                { 96000.0, 1, "96k 1x (real time)" }, { 48000.0, 4, "48k 4x (offline)" },
                                { 96000.0, 2, "96k 2x (offline)" } };

struct SlaveWave
{
    Wave wave;
    float width;
    const char* name;
};
constexpr SlaveWave kSlaveWaves[] { { Wave::Sine, 0.5f, "sine" }, { Wave::Triangle, 0.5f, "triangle" }, { Wave::Saw, 0.5f, "saw" },
                                    { Wave::Square, 0.5f, "square" }, { Wave::Pulse, 0.2f, "pulse20" } };

// VCO-1 / VCO-2: unrelated (1.37, 2.71), below 1 (VCO-1 never completes a cycle), and slightly
// detuned from 1:1 and 2:1, where the slave's own wrap lands just before or after the reset.
// The master period is not a whole number of samples (odd bin), so the reset falls at a
// different sub-sample position every cycle.
constexpr double kSyncRatios[] { 1.37, 2.71, 0.71, 1.0031, 1.9969 };
constexpr double kSyncHz[] { 440.0, 5000.0 };
} // namespace

TEST_CASE ("Alias gate with hard sync (no Cross Mod): every VCO-1 waveform, several ratios, every rate configuration", "[os][alias][sync]")
{
    for (const auto& c : kSyncCfgs)
        for (const auto& w : kSlaveWaves)
        {
            double worst = -300.0;
            for (const double hz : kSyncHz)
                for (const double ratio : kSyncRatios)
                {
                    const auto r = measureSyncAliasing (w.wave, w.width, hz, ratio, c.rate, c.factor);
                    INFO (c.name << ", VCO-1 " << w.name << ", master " << hz << " Hz, ratio " << ratio << ": " << r.worstDb << " dB at "
                                 << r.worstHz << " Hz");
                    worst = std::max (worst, r.worstDb);
                    // One case is above -100 dB without being caused by sync: a 20 % pulse with VCO-1
                    // near 10 kHz at 88.2 kHz 1x. The exact 2:1 ratio (every reset lands on the slave's
                    // own wrap, so the output is the free-running oscillator at 10 kHz) measures the
                    // same: it is the oscillator's own BLEP floor for a 10 kHz pulse (-99.2 dB), above
                    // the 5 kHz the waveform gate covers. Improving it needs a longer BLEP table, which
                    // would change every waveform (the saw under sync must stay bit-identical), so the
                    // sync correction is checked against that floor here instead of being hidden.
                    if (w.wave == Wave::Pulse && c.factor == 1 && c.rate < 90000.0 && hz > 4000.0 && std::abs (ratio - 2.0) < 0.01)
                    {
                        const auto unsynced = measureSyncAliasing (w.wave, w.width, hz, 2.0, c.rate, c.factor);
                        INFO ("oscillator floor at exactly 2:1: " << unsynced.worstDb << " dB");
                        CHECK (r.worstDb <= unsynced.worstDb + 0.1);
                        CHECK (unsynced.worstDb <= -99.0);
                        continue;
                    }
                    CHECK (r.worstDb <= -100.0);
                }
            WARN ("sync alias floor, " << c.name << ", VCO-1 " << w.name << ": worst " << worst << " dB");
        }
}

// Diagnostic (hidden): writes the on-grid (wanted) partial levels of every sync case to
// $VS_RENDER_DIR/sync_harmonics.txt, to compare builds before and after an oscillator change.
TEST_CASE ("Diagnostic: sync harmonics dump", "[.diag]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VS_RENDER_DIR", {});
    REQUIRE (dir.isNotEmpty());
    std::ofstream f (juce::File (dir).getChildFile ("sync_harmonics.txt").getFullPathName().toStdString());
    f.precision (9);
    for (const auto& c : kSyncCfgs)
        for (const auto& w : kSlaveWaves)
            for (const double hz : { 110.0, 440.0, 1000.0, 2000.0, 5000.0 })
                for (const double ratio : { 1.37, 2.71, 0.71, 1.0031, 1.9969, 2.0 })
                {
                    std::vector<double> h;
                    const auto r = measureSyncAliasing (w.wave, w.width, hz, ratio, c.rate, c.factor, &h);
                    f << c.rate << ' ' << c.factor << ' ' << w.name << ' ' << hz << ' ' << ratio << ' ' << r.worstDb;
                    for (const double d : h)
                        f << ' ' << d;
                    f << '\n';
                }
}

// ------------------------------------------------------------------ Classic Cross Mod

namespace
{
struct XmodBound
{
    Wave mod;
    double f2;
    float slider;
    bool sync;
    double rate;
    double measuredDb; // worst alias today (method P), dB re the strongest wanted component
};

// Classic Cross Mod is EXEMPT from the -100 dB alias gate by owner decision (2026-11-12): its
// aliasing is part of the approved real-time sound. These are its levels on main (2026-11-12,
// commit 1549dca; unchanged by the wave 3 oscillator fixes), measured with CrossModMeasure.h
// measurePair: the production VCO pair at the real-time internal rates (96 kHz: 48 kHz 2x and
// 96 kHz 1x; 88.2 kHz: 44.1 kHz 2x and 88.2 kHz 1x), carrier saw, VCO-1 / VCO-2 = 1.37 with sync
// and 3/2 effective without; worst component off the harmonic grid in 0..20 kHz, dB re the
// strongest wanted one. A regression guard, not a quality gate: no case may get more than 3 dB
// worse. Levels below -100 dB are guarded at -97 dB (they meet the gate; below it the method's
// own floor, which depends on float rounding and so on the compiler, decides the exact value).
// Columns: modulator (VCO-2) wave, VCO-2 Hz, Cross Mod slider, sync, internal rate, measured dB.
// clang-format off
constexpr XmodBound kXmodBounds[] {
    { Wave::Saw, 110, 0.25f, false, 96000, -116.9 },
    { Wave::Saw, 110, 0.5f, false, 96000, -93.7 },
    { Wave::Saw, 440, 0.25f, false, 96000, -83.7 },
    { Wave::Saw, 440, 0.5f, false, 96000, -47.7 },
    { Wave::Saw, 1000, 0.25f, false, 96000, -98.1 },
    { Wave::Saw, 1000, 0.5f, false, 96000, -63.2 },
    { Wave::Square, 110, 0.25f, false, 96000, -82.9 },
    { Wave::Square, 110, 0.5f, false, 96000, -59.4 },
    { Wave::Square, 440, 0.25f, false, 96000, -64.4 },
    { Wave::Square, 440, 0.5f, false, 96000, -39.2 },
    { Wave::Square, 1000, 0.25f, false, 96000, -74.0 },
    { Wave::Square, 1000, 0.5f, false, 96000, -63.7 },
    { Wave::Sine, 110, 0.25f, false, 96000, -113.0 },
    { Wave::Sine, 110, 0.5f, false, 96000, -116.2 },
    { Wave::Sine, 440, 0.25f, false, 96000, -106.1 },
    { Wave::Sine, 440, 0.5f, false, 96000, -92.9 },
    { Wave::Sine, 1000, 0.25f, false, 96000, -94.8 },
    { Wave::Sine, 1000, 0.5f, false, 96000, -80.6 },
    { Wave::Saw, 110, 0.25f, true, 96000, -124.1 },
    { Wave::Saw, 110, 0.5f, true, 96000, -92.2 },
    { Wave::Saw, 440, 0.25f, true, 96000, -110.0 },
    { Wave::Saw, 440, 0.5f, true, 96000, -78.6 },
    { Wave::Saw, 1000, 0.25f, true, 96000, -101.5 },
    { Wave::Saw, 1000, 0.5f, true, 96000, -70.9 },
    { Wave::Square, 110, 0.25f, true, 96000, -130.2 },
    { Wave::Square, 110, 0.5f, true, 96000, -86.8 },
    { Wave::Square, 440, 0.25f, true, 96000, -116.8 },
    { Wave::Square, 440, 0.5f, true, 96000, -75.5 },
    { Wave::Square, 1000, 0.25f, true, 96000, -110.1 },
    { Wave::Square, 1000, 0.5f, true, 96000, -72.2 },
    { Wave::Sine, 110, 0.25f, true, 96000, -126.7 },
    { Wave::Sine, 110, 0.5f, true, 96000, -113.4 },
    { Wave::Sine, 440, 0.25f, true, 96000, -111.8 },
    { Wave::Sine, 440, 0.5f, true, 96000, -92.8 },
    { Wave::Sine, 1000, 0.25f, true, 96000, -91.0 },
    { Wave::Sine, 1000, 0.5f, true, 96000, -78.5 },
    { Wave::Saw, 110, 0.25f, false, 88200, -110.7 },
    { Wave::Saw, 110, 0.5f, false, 88200, -88.3 },
    { Wave::Saw, 440, 0.25f, false, 88200, -94.0 },
    { Wave::Saw, 440, 0.5f, false, 88200, -60.2 },
    { Wave::Saw, 1000, 0.25f, false, 88200, -83.8 },
    { Wave::Saw, 1000, 0.5f, false, 88200, -71.2 },
    { Wave::Square, 110, 0.25f, false, 88200, -77.9 },
    { Wave::Square, 110, 0.5f, false, 88200, -69.6 },
    { Wave::Square, 440, 0.25f, false, 88200, -74.3 },
    { Wave::Square, 440, 0.5f, false, 88200, -51.6 },
    { Wave::Square, 1000, 0.25f, false, 88200, -69.8 },
    { Wave::Square, 1000, 0.5f, false, 88200, -73.1 },
    { Wave::Sine, 110, 0.25f, false, 88200, -99.8 },
    { Wave::Sine, 110, 0.5f, false, 88200, -108.2 },
    { Wave::Sine, 440, 0.25f, false, 88200, -94.0 },
    { Wave::Sine, 440, 0.5f, false, 88200, -92.3 },
    { Wave::Sine, 1000, 0.25f, false, 88200, -88.2 },
    { Wave::Sine, 1000, 0.5f, false, 88200, -79.7 },
    { Wave::Saw, 110, 0.25f, true, 88200, -123.8 },
    { Wave::Saw, 110, 0.5f, true, 88200, -90.6 },
    { Wave::Saw, 440, 0.25f, true, 88200, -110.6 },
    { Wave::Saw, 440, 0.5f, true, 88200, -77.5 },
    { Wave::Saw, 1000, 0.25f, true, 88200, -99.2 },
    { Wave::Saw, 1000, 0.5f, true, 88200, -69.4 },
    { Wave::Square, 110, 0.25f, true, 88200, -124.6 },
    { Wave::Square, 110, 0.5f, true, 88200, -85.2 },
    { Wave::Square, 440, 0.25f, true, 88200, -108.4 },
    { Wave::Square, 440, 0.5f, true, 88200, -78.6 },
    { Wave::Square, 1000, 0.25f, true, 88200, -102.5 },
    { Wave::Square, 1000, 0.5f, true, 88200, -71.7 },
    { Wave::Sine, 110, 0.25f, true, 88200, -126.8 },
    { Wave::Sine, 110, 0.5f, true, 88200, -115.8 },
    { Wave::Sine, 440, 0.25f, true, 88200, -108.2 },
    { Wave::Sine, 440, 0.5f, true, 88200, -89.1 },
    { Wave::Sine, 1000, 0.25f, true, 88200, -88.5 },
    { Wave::Sine, 1000, 0.5f, true, 88200, -77.1 },
};
// clang-format on

const char* waveName (Wave w)
{
    switch (w)
    {
        case Wave::Sine: return "sine";
        case Wave::Triangle: return "triangle";
        case Wave::Saw: return "saw";
        case Wave::Square: return "square";
        case Wave::Pulse: return "pulse";
        case Wave::Noise: return "noise";
    }
    return "?";
}
} // namespace

TEST_CASE ("Classic Cross Mod: aliasing is not worse than measured + 3 dB (regression guard; exempt from the -100 dB gate)", "[os][alias][xmod]")
{
    for (const auto& b : kXmodBounds)
    {
        vs::test::xmod::PairCfg c;
        c.car = Wave::Saw;
        c.mod = b.mod;
        c.rate = b.rate;
        c.f2 = b.f2;
        c.sync = b.sync;
        c.depthOct = vs::map::crossModOct (b.slider);
        const auto r = vs::test::xmod::measurePair (c);
        INFO ("modulator " << waveName (b.mod) << ", VCO-2 " << b.f2 << " Hz, slider " << b.slider << (b.sync ? ", sync" : "") << ", "
                           << b.rate << " Hz: " << r.worstDb << " dB at " << r.worstHz << " Hz (measured " << b.measuredDb << ")");
        CHECK (r.worstDb <= std::max (b.measuredDb, -100.0) + 3.0);
    }
}

// Prints the table above from a fresh measurement (hidden).
TEST_CASE ("Diagnostic: measure the Classic Cross Mod table", "[.diag]")
{
    juce::String table;
    for (const double rate : { 96000.0, 88200.0 })
        for (const bool sync : { false, true })
            for (const Wave mod : { Wave::Saw, Wave::Square, Wave::Sine })
                for (const double f2 : { 110.0, 440.0, 1000.0 })
                    for (const float slider : { 0.25f, 0.5f })
                    {
                        vs::test::xmod::PairCfg c;
                        c.mod = mod;
                        c.rate = rate;
                        c.f2 = f2;
                        c.sync = sync;
                        c.depthOct = vs::map::crossModOct (slider);
                        const auto r = vs::test::xmod::measurePair (c);
                        const char* w = mod == Wave::Saw ? "Wave::Saw" : mod == Wave::Square ? "Wave::Square" : "Wave::Sine";
                        table << "    { " << w << ", " << f2 << ", " << slider << "f, " << (sync ? "true" : "false") << ", " << rate << ", "
                              << juce::String (r.worstDb, 1) << " },\n";
                    }
    WARN ("\n" << table);
}

// ------------------------------------------------------------------ real time vs offline

namespace
{
struct Patch
{
    const char* name;
    void (*setup) (vs::PartParams&);
    int note;
    bool crossMod; // the patch uses Cross Mod (the equivalence target); false: plain reference
};

// Held, stationary patches (no LFO, no Vintage, envelopes settled, no VCF envelope).
void basePatch (vs::PartParams& p)
{
    p.vcfEnvMod = 0.0f;
    p.env2Attack = 0.0f;
    p.env2Sustain = 1.0f;
    p.sourceMix = 0.0f;
    p.vcfCutoff = 0.9f;
}

const Patch kPatches[] {
    { "sync + Cross Mod (saw <- saw, 0.35)", [] (vs::PartParams& p) { basePatch (p); p.vcoSync = 1; p.vco1CrossMod = 0.35f; }, 48, true },
    { "sync + Cross Mod harsh (pulse <- saw, 0.9)",
      [] (vs::PartParams& p) { basePatch (p); p.vco1Wave = 4; p.pwmDepth = 0.6f; p.pwmSource = 1; p.vcoSync = 1; p.vco1CrossMod = 0.9f; p.vcfCutoff = 1.0f; p.vcaLevel = 0.6f; },
      60, true },
    { "sync + Cross Mod (sine <- square, 0.5)", [] (vs::PartParams& p) { basePatch (p); p.vco1Wave = 0; p.vco2Wave = 3; p.vcoSync = 1; p.vco1CrossMod = 0.5f; }, 55, true },
    { "Cross Mod bell (sine <- sine 4', 0.55)",
      [] (vs::PartParams& p) { basePatch (p); p.vco1Wave = 0; p.vco2Wave = 0; p.vco2Range = 2; p.vco1CrossMod = 0.55f; p.vco2FineTune = 0.37f; p.vcfCutoff = 0.95f; },
      72, true },
    { "Cross Mod metallic (saw <- square, 0.45)",
      [] (vs::PartParams& p) { basePatch (p); p.vco2Wave = 3; p.vco2Range = 1; p.vco2FineTune = 0.5f; p.vco1CrossMod = 0.45f; p.vcfCutoff = 0.85f; p.vcfResonance = 0.3f; },
      64, true },
    { "Cross Mod high modulator (saw <- saw 2', 0.5)",
      [] (vs::PartParams& p) { basePatch (p); p.vco2Range = 3; p.vco2FineTune = 0.23f; p.vco1CrossMod = 0.5f; p.vcfCutoff = 1.0f; p.vcaLevel = 0.6f; },
      64, true },
    { "plain saw + square (reference)", [] (vs::PartParams& p) { basePatch (p); p.vco2Wave = 3; p.sourceMix = 0.5f; }, 52, false },
    { "plain sync, sine VCO-1 (reference)", [] (vs::PartParams& p) { basePatch (p); p.vco1Wave = 0; p.vcoSync = 1; }, 50, false },
};

/** A held note through a Part prepared like the plugin: the last `capture` samples, and the exact latency. */
std::pair<std::vector<float>, double> renderHeld (const Patch& patch, double rate, bool offline, int capture)
{
    constexpr int block = 256;
    vs::Part part;
    vs::PartParams p;
    patch.setup (p);
    part.setParams (p);
    vs::test::xmod::prepareLikePlugin (part, rate, block, offline, 1234u);
    part.setVintage (0.0f);
    const int settle = static_cast<int> (0.2 * rate);
    std::vector<float> out (static_cast<size_t> (settle + capture));
    const vs::NoteEvent on[] { { 0, vs::EventType::NoteOn, static_cast<std::uint8_t> (patch.note), 0.8f } };
    for (int pos = 0; pos < static_cast<int> (out.size()); pos += block)
    {
        const int len = std::min (block, static_cast<int> (out.size()) - pos);
        part.render (out.data() + pos, len, pos == 0 ? std::span<const vs::NoteEvent> (on) : std::span<const vs::NoteEvent> {});
    }
    return { std::vector<float> (out.end() - capture, out.end()), part.getExactLatency() };
}

struct Equivalence
{
    vs::test::xmod::PartialDiff partials;
    double nullDb, tau, latencyDiff;
};

Equivalence compareRealTimeOffline (const Patch& patch, double rate)
{
    const int capture = 1 << 15; // 0.68 s at 48 kHz, 0.34 s at 96 kHz
    const auto [rt, latencyRt] = renderHeld (patch, rate, false, capture);
    const auto [off, latencyOff] = renderHeld (patch, rate, true, capture);
    const auto a = vs::test::xmod::windowedSpectrum (rt), b = vs::test::xmod::windowedSpectrum (off);
    const double binHz = rate / static_cast<double> (2 * a.size());
    Equivalence e {};
    e.partials = vs::test::xmod::comparePartials (a, b, binHz, -60.0);
    e.nullDb = vs::test::xmod::steadyNullDb (a, b, binHz, rate, std::min (20000.0, rate * 20.0 / 48.0), 24.0, &e.tau);
    e.latencyDiff = latencyOff - latencyRt;
    return e;
}
} // namespace

// An offline bounce of a Cross Mod patch must sound like real-time playback.
// Method: held, stationary notes (no LFO, no Vintage, settled envelopes) through the Part, prepared
// like the plugin in real time and offline at the same host rate (48 kHz: 2x vs 4x; 96 kHz: 1x vs
// 2x); 2^15 samples of steady state, 7-term Blackman-Harris window, 4x zero padding.
// - Partials: every local maximum above -60 dB re the strongest component in 20 Hz..20 kHz of the
//   real-time render, against the offline render's largest magnitude within +-1 bin, both re their
//   own strongest component: within 1 dB (measured: <= 0.014 dB; before the fix up to 55 dB, the
//   Cross Mod partials moved).
// - Null: the residual power of offline vs real time in 20 Hz..20 kHz after the best fractional
//   time alignment, re the real-time power: below -45 dB. What is left is the two decimators'
//   different phase responses (IIR half-bands: 2x vs 4x, or none vs 2x) acting on the patch's
//   high partials: the plain reference patches, which always rendered at the offline factor,
//   null at -45 to -76 dB; the Cross Mod patches now at -49 to -66 dB (before: -2 to -48 dB).
// The best alignment (tau) is printed; it varies by up to +-0.6 sample with the patch's spectrum
// (the decimators' group delay is not flat), so latency is checked by the next test instead.
// The plain references are reported, not asserted: they keep the higher offline factor by design.
TEST_CASE ("Cross Mod: offline renders sound like real-time renders (partials within 1 dB, null below -45 dB)", "[os][xmod][offline]")
{
    for (const double rate : { 48000.0, 96000.0 })
        for (const auto& patch : kPatches)
        {
            if (! patch.crossMod && rate > 60000.0)
                continue; // the references once are enough
            const auto e = compareRealTimeOffline (patch, rate);
            INFO (rate << " Hz, " << patch.name << ": " << e.partials.count << " partials, worst " << e.partials.worstDb << " dB at "
                        << e.partials.worstHz << " Hz; null " << e.nullDb << " dB (tau " << e.tau << ", latency difference "
                        << e.latencyDiff << ")");
            WARN (rate << " Hz, " << patch.name << ": worst partial " << e.partials.worstDb << " dB at " << e.partials.worstHz << " Hz ("
                        << e.partials.count << " partials), null " << e.nullDb << " dB, tau " << e.tau << ", latency difference "
                        << e.latencyDiff);
            if (! patch.crossMod)
                continue;
            CHECK (e.partials.count > 0);
            CHECK (std::abs (e.partials.worstDb) <= 1.0);
            CHECK (e.nullDb <= -45.0);
        }
}

// Offline, notes on the real-time-rate path (Cross Mod up at note start) must sound exactly at the
// reported latency (as the real-time voice does). Method as in the independent latency test
// (OversamplingTests): a 110 Hz sine (VCO-1, filter open), phase at 110 Hz by coherent projection
// over steady state; the offline render's phase delay against the real-time render must equal the
// difference of the exact reported latencies. Cross Mod at 0.02 (0.0016 oct) engages the path
// while leaving the tone a sine.
TEST_CASE ("Cross Mod: offline notes on the real-time-rate path sound at the reported latency", "[os][xmod][offline][latency]")
{
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
        for (const float crossMod : { 0.0f, 0.02f })
        {
            const Patch patch { "sine", [] (vs::PartParams& p) { basePatch (p); p.vco1Wave = 0; p.vcfCutoff = 1.0f; p.vcfResonance = 0.0f; p.vcfKeyFollow = 0.0f; }, 45, true };
            auto render = [&] (bool offline) {
                constexpr int block = 256;
                vs::Part part;
                vs::PartParams p;
                patch.setup (p);
                p.vco1CrossMod = crossMod;
                part.setParams (p);
                vs::test::xmod::prepareLikePlugin (part, rate, block, offline, 777u);
                std::vector<float> out (static_cast<size_t> (rate));
                const vs::NoteEvent on[] { { 100, vs::EventType::NoteOn, 45, 0.8f } };
                for (int pos = 0; pos < static_cast<int> (out.size()); pos += block)
                    part.render (out.data() + pos, std::min (block, static_cast<int> (out.size()) - pos),
                                 pos == 0 ? std::span<const vs::NoteEvent> (on) : std::span<const vs::NoteEvent> {});
                const bool rtPath = part.getVoice (0).usesRealTimePair();
                return std::tuple { out, part.getExactLatency(), rtPath };
            };
            const auto [rt, latencyRt, pathRt] = render (false);
            const auto [off, latencyOff, pathOff] = render (true);
            const double w = 2.0 * juce::MathConstants<double>::pi * 110.0 / rate;
            auto phase = [w] (const std::vector<float>& x) {
                double s = 0.0, c = 0.0;
                for (size_t n = x.size() / 2; n < x.size(); ++n)
                {
                    s += static_cast<double> (x[n]) * std::sin (w * static_cast<double> (n));
                    c += static_cast<double> (x[n]) * std::cos (w * static_cast<double> (n));
                }
                return std::atan2 (c, s);
            };
            double dphi = phase (rt) - phase (off);
            while (dphi > juce::MathConstants<double>::pi)
                dphi -= 2.0 * juce::MathConstants<double>::pi;
            while (dphi < -juce::MathConstants<double>::pi)
                dphi += 2.0 * juce::MathConstants<double>::pi;
            const double delay = dphi / w;
            INFO (rate << " Hz, Cross Mod " << crossMod << ": offline is " << delay << " samples later, reported difference "
                        << latencyOff - latencyRt);
            CHECK (! pathRt);
            CHECK (pathOff == (crossMod > 0.0f && vs::voiceOversamplingFactor (rate, true) == 2 * vs::voiceOversamplingFactor (rate, false)));
            if (pathOff)
            {
                // The real-time voice, delayed by exactly the reported difference.
                CHECK (std::abs (delay - (latencyOff - latencyRt)) < 0.01);
            }
            else
            {
                // Ordinary notes (unchanged behaviour, as on main): one internal sample later (an
                // oscillator outputs the phase at the end of its sample, one sample at either rate)
                // plus the VCF's slightly longer low-frequency delay at the higher rate.
                WARN (rate << " Hz, ordinary note: offline " << delay - (latencyOff - latencyRt) << " samples after the reported latency");
                CHECK (delay - (latencyOff - latencyRt) > 0.0);
                CHECK (delay - (latencyOff - latencyRt) < 2.0 / vs::voiceOversamplingFactor (rate, true) + 0.4);
            }
        }
}

// ------------------------------------------------------------------ audition / reference renders

namespace
{
struct Scene
{
    const char* file;
    const char* name;
    void (*setup) (vs::PartParams&);
    std::vector<std::pair<double, int>> notes; // (start s, note); each lasts step - gap
    double step, gap;
};

void renderScene (const Scene& s, double rate, bool offline, std::vector<float>& out)
{
    constexpr int block = 256;
    vs::Part part;
    vs::PartParams p;
    s.setup (p);
    part.setParams (p);
    vs::test::xmod::prepareLikePlugin (part, rate, block, offline, 99u);
    const double length = s.notes.back().first + s.step + 0.6;
    const int total = static_cast<int> (length * rate);
    out.assign (static_cast<size_t> (total), 0.0f);
    for (int start = 0; start < total; start += block)
    {
        const int len = std::min (block, total - start);
        std::vector<vs::NoteEvent> events;
        for (const auto& [t, note] : s.notes)
        {
            const int on = static_cast<int> (t * rate), off = static_cast<int> ((t + s.step - s.gap) * rate);
            if (on >= start && on < start + len)
                events.push_back ({ on - start, vs::EventType::NoteOn, static_cast<std::uint8_t> (note), 0.8f });
            if (off >= start && off < start + len)
                events.push_back ({ off - start, vs::EventType::NoteOff, static_cast<std::uint8_t> (note), 0.0f });
        }
        std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b) { return a.offset < b.offset; });
        part.render (out.data() + start, len, events);
    }
    for (auto& v : out)
        v *= 0.5f;
}

std::vector<std::pair<double, int>> line (std::initializer_list<int> notes, double step)
{
    std::vector<std::pair<double, int>> out;
    double t = 0.1;
    for (int n : notes)
    {
        out.push_back ({ t, n });
        t += step;
    }
    return out;
}
} // namespace

// Writes every scene at 48 kHz (real time 2x, offline 4x) and 96 kHz (real time 1x, offline 2x) to
// $VS_RENDER_DIR as 24-bit WAV (listening) and raw float32 (null tests between builds), plus
// scenes.txt. Run: VS_RENDER_DIR=<dir> VintageSynthTests "[.xmodrender]"
TEST_CASE ("Audition: Cross Mod and sync scenes, real time and offline", "[.xmodrender]")
{
    const Scene scenes[] {
        { "bell", "bell (sine <- sine 4', Cross Mod 0.55 = 1.2 oct)",
          [] (vs::PartParams& p) { p.vco1Wave = 0; p.vco2Wave = 0; p.vco2Range = 2; p.vco1CrossMod = 0.55f; p.sourceMix = 0.0f; p.vcfCutoff = 0.95f; p.vcfEnvMod = 0.0f;
                                   p.env2Attack = 0.0f; p.env2Decay = 0.75f; p.env2Sustain = 0.0f; p.env2Release = 0.7f; p.vco2FineTune = 0.37f; },
          line ({ 72, 79, 76, 84, 67 }, 0.7), 0.7, 0.1 },
        { "metallic_lead", "metallic lead (saw <- square, Cross Mod 0.45 = 0.8 oct)",
          [] (vs::PartParams& p) { p.vco1Wave = 2; p.vco2Wave = 3; p.vco2Range = 1; p.vco2FineTune = 0.5f; p.vco1CrossMod = 0.45f; p.sourceMix = 0.0f;
                                   p.vcfCutoff = 0.85f; p.vcfResonance = 0.3f; p.vcfEnvMod = 0.2f; p.env2Sustain = 0.9f; p.vcoModLfo = 0.08f; },
          line ({ 57, 60, 64, 69, 72, 76 }, 0.55), 0.55, 0.05 },
        { "sync_sweep_xmod", "sync sweep with Cross Mod (saw <- saw, sync, Cross Mod 0.35 = 0.5 oct, ENV-1 -> VCO-1 pitch)",
          [] (vs::PartParams& p) { p.vco1Wave = 2; p.vco2Wave = 2; p.vcoSync = 1; p.vco1CrossMod = 0.35f; p.vcoModEnv = 0.75f; p.vcoModTarget = 0; p.sourceMix = 0.0f;
                                   p.env1Attack = 0.0f; p.env1Decay = 0.7f; p.env1Sustain = 0.0f; p.vcfCutoff = 0.9f; p.vcfEnvMod = 0.0f; p.env2Sustain = 1.0f; },
          line ({ 48, 55, 60, 52 }, 0.9), 0.9, 0.1 },
        { "harsh_extreme", "harsh extreme (pulse <- saw, sync, Cross Mod 0.9 = 3.2 oct)",
          [] (vs::PartParams& p) { p.vco1Wave = 4; p.vco2Wave = 2; p.pwmDepth = 0.6f; p.pwmSource = 1; p.vcoSync = 1; p.vco1CrossMod = 0.9f; p.sourceMix = 0.0f;
                                   p.vcfCutoff = 1.0f; p.vcfEnvMod = 0.0f; p.env2Sustain = 1.0f; p.vcaLevel = 0.6f; },
          line ({ 60, 67, 72, 79 }, 0.8), 0.8, 0.1 },
        { "xmod_alone", "Cross Mod alone, high modulator (saw <- saw 2', Cross Mod 0.5 = 1 oct)",
          [] (vs::PartParams& p) { p.vco1Wave = 2; p.vco2Wave = 2; p.vco2Range = 3; p.vco2FineTune = 0.23f; p.vco1CrossMod = 0.5f; p.sourceMix = 0.0f;
                                   p.vcfCutoff = 1.0f; p.vcfEnvMod = 0.0f; p.env2Sustain = 1.0f; p.vcaLevel = 0.6f; },
          line ({ 64, 71, 76, 83 }, 0.8), 0.8, 0.1 },
        { "sine_sync", "sine sync sweep (sine <- saw, sync, ENV-1 -> VCO-1 pitch, no Cross Mod)",
          [] (vs::PartParams& p) { p.vco1Wave = 0; p.vco2Wave = 2; p.vcoSync = 1; p.vcoModEnv = 0.8f; p.vcoModTarget = 0; p.sourceMix = 0.0f;
                                   p.env1Attack = 0.0f; p.env1Decay = 0.75f; p.env1Sustain = 0.0f; p.vcfCutoff = 1.0f; p.vcfEnvMod = 0.0f; p.env2Sustain = 1.0f; },
          line ({ 60, 67, 72, 79 }, 0.9), 0.9, 0.1 },
        { "triangle_sync", "triangle sync sweep (triangle <- saw, sync, ENV-1 -> VCO-1 pitch, no Cross Mod)",
          [] (vs::PartParams& p) { p.vco1Wave = 1; p.vco2Wave = 2; p.vcoSync = 1; p.vcoModEnv = 0.8f; p.vcoModTarget = 0; p.sourceMix = 0.0f;
                                   p.env1Attack = 0.0f; p.env1Decay = 0.75f; p.env1Sustain = 0.0f; p.vcfCutoff = 1.0f; p.vcfEnvMod = 0.0f; p.env2Sustain = 1.0f; },
          line ({ 60, 67, 72, 79 }, 0.9), 0.9, 0.1 },
        { "plain_poly", "plain saw + square chords (no sync, no Cross Mod)",
          [] (vs::PartParams& p) { p.vco2Wave = 3; p.vcfResonance = 0.4f; p.vcoModLfo = 0.05f; },
          line ({ 48, 55, 60, 64 }, 0.6), 0.6, 0.05 },
    };

    const auto envDir = juce::SystemStats::getEnvironmentVariable ("VS_RENDER_DIR", {});
    REQUIRE (envDir.isNotEmpty());
    juce::File dir (envDir);
    dir.createDirectory();
    juce::String list, nulls;
    std::vector<float> realTime;
    for (const auto& s : scenes)
        for (const double rate : { 48000.0, 96000.0 })
            for (const bool offline : { false, true })
            {
                std::vector<float> audio;
                renderScene (s, rate, offline, audio);
                float peak = 0.0f;
                for (const float v : audio)
                    peak = std::max (peak, std::abs (v));
                REQUIRE (peak < 1.0f);
                const juce::String base = juce::String (s.file) + "_" + juce::String (static_cast<int> (rate / 1000.0)) + "k_"
                                          + (offline ? "offline" : "realtime");
                juce::File (dir.getChildFile (base + ".f32")).replaceWithData (audio.data(), audio.size() * sizeof (float));
                auto file = dir.getChildFile (base + ".wav");
                file.deleteFile();
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
                const auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (24);
                auto writer = wav.createWriterFor (stream, options);
                REQUIRE (writer != nullptr);
                const float* ch[] { audio.data() };
                writer->writeFromFloatArrays (ch, 1, static_cast<int> (audio.size()));
                list << base << ".wav  " << s.name << ", " << (rate / 1000.0) << " kHz " << (offline ? "offline" : "real time") << " ("
                     << vs::voiceOversamplingFactor (rate, offline) << "x)\n";

                if (! offline)
                {
                    realTime = audio;
                    continue;
                }
                // Whole scene, offline vs real time: residual power after the best time alignment
                // (rectangular window over the whole render, zero-padded: the scenes start and end
                // in silence, so a delay is a phase factor per bin).
                const size_t n = std::bit_ceil (std::max (audio.size(), realTime.size()));
                std::vector<float> a (realTime), b (audio);
                a.resize (n, 0.0f);
                b.resize (n, 0.0f);
                const auto sa = vs::test::xmod::windowedSpectrum (a, 2, false), sb = vs::test::xmod::windowedSpectrum (b, 2, false);
                double tau = 0.0;
                const double nullDb = vs::test::xmod::steadyNullDb (sa, sb, rate / static_cast<double> (2 * sa.size()), rate,
                                                                    std::min (20000.0, rate * 20.0 / 48.0), 24.0, &tau);
                nulls << s.file << " " << (rate / 1000.0) << " kHz: offline vs real time null " << juce::String (nullDb, 1) << " dB (aligned by "
                      << juce::String (tau, 2) << " samples)\n";
            }
    dir.getChildFile ("scenes.txt").replaceWithText (list + "\n" + nulls);
    WARN (nulls);
    WARN ("wrote " << dir.getFullPathName() << "\n" << list);
}
