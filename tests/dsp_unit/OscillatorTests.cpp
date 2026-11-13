#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "TestHelpers.h"
#include "vintage/dsp/Oscillator.h"
#include "common/engine/Oversampler.h"

using vs::dsp::Oscillator;
using vs::dsp::Wave;

namespace
{
constexpr double kBaseRate = 48000.0;
constexpr int kFftOrder = 15;
constexpr int kN = 1 << kFftOrder;

struct AliasResult
{
    double worstDb;
    double worstHz;
};

/**
    Renders an oscillator at 2x through the production decimator, then measures the
    strongest non-harmonic component in 0..20 kHz relative to the fundamental.
    The frequency is bin-centred (k odd) so harmonics and aliases fall on exact,
    distinct bins, which allows a rectangular window with no leakage.
*/
AliasResult measureAliasing (Wave wave, int k, float width = 0.5f)
{
    constexpr int block = 512;
    auto os = vs::makeVoiceOversampler (2);
    os->initProcessing (block);

    juce::AudioBuffer<float> silence (1, block), out (1, block);
    silence.clear();

    Oscillator osc;
    osc.setWave (wave);
    osc.reset();
    const float dt = static_cast<float> (k) / static_cast<float> (2 * kN); // exact in float

    std::vector<float> captured;
    const int warmup = 16384;
    for (int done = 0; done < warmup + kN; done += block)
    {
        auto up = os->processSamplesUp (juce::dsp::AudioBlock<float> (silence));
        float* p = up.getChannelPointer (0);
        float wrap = 0.0f;
        for (size_t i = 0; i < up.getNumSamples(); ++i)
            p[i] = osc.process (dt, width, -1.0f, wrap);

        juce::dsp::AudioBlock<float> ob (out);
        os->processSamplesDown (ob);
        if (done >= warmup)
            captured.insert (captured.end(), out.getReadPointer (0), out.getReadPointer (0) + block);
    }

    const auto mag = vs::test::magnitudeSpectrum (captured, kFftOrder);
    const double fundamental = mag[static_cast<size_t> (k)];
    AliasResult r { -300.0, 0.0 };
    for (size_t b = 1; b < mag.size(); ++b)
    {
        const double hz = static_cast<double> (b) * kBaseRate / kN;
        if (hz > 20000.0)
            break;
        if (b % static_cast<size_t> (k) == 0)
            continue;
        const double db = vs::test::toDb (mag[b] / fundamental);
        if (db > r.worstDb)
            r = { db, hz };
    }
    return r;
}

int binFor (double hz)
{
    int k = static_cast<int> (std::lround (hz * kN / kBaseRate));
    return (k % 2 == 0) ? k + 1 : k;
}
} // namespace

TEST_CASE ("VCO aliasing stays below threshold", "[osc][alias]")
{
    struct Case
    {
        Wave wave;
        float width;
        const char* name;
    };
    const Case wave = GENERATE (Case { Wave::Saw, 0.5f, "saw" },
                                Case { Wave::Square, 0.5f, "square" },
                                Case { Wave::Pulse, 0.2f, "pulse20" },
                                Case { Wave::Triangle, 0.5f, "triangle" });

    // Plan target: worst alias in 0..20 kHz below -90 dB up to a 5 kHz fundamental.
    // The windowed-sinc BLEP measures about -113 dB; the gate keeps 10 dB of margin to the target.
    for (const double hz : { 110.0, 440.0, 1000.0, 2000.0, 5000.0 })
    {
        const auto r = measureAliasing (wave.wave, binFor (hz), wave.width);
        const double limit = -100.0;
        INFO (wave.name << " @ " << hz << " Hz: worst alias " << r.worstDb << " dB at " << r.worstHz << " Hz");
        CHECK (r.worstDb < limit);
    }
}

TEST_CASE ("VCO pitch is accurate", "[osc]")
{
    const Wave wave = GENERATE (Wave::Saw, Wave::Square, Wave::Triangle, Wave::Sine);
    const double fs = 96000.0;
    for (const double hz : { 55.0, 440.0, 3520.0 })
    {
        Oscillator osc;
        osc.setWave (wave);
        osc.reset (0.3f);
        std::vector<float> x (static_cast<size_t> (fs));
        float wrap = 0.0f;
        for (auto& s : x)
            s = osc.process (static_cast<float> (hz / fs), 0.5f, -1.0f, wrap);

        const double measured = vs::test::measureFrequency (x, fs);
        const double cents = 1200.0 * std::log2 (measured / hz);
        INFO ("wave " << static_cast<int> (wave) << " target " << hz << " measured " << measured);
        CHECK (std::abs (cents) < 0.5);
    }
}

TEST_CASE ("Hard sync locks the slave to the master frequency", "[osc][sync]")
{
    const double fs = 96000.0;
    Oscillator master, slave;
    master.setWave (Wave::Saw);
    slave.setWave (Wave::Saw);
    master.reset();
    slave.reset();

    const float dtMaster = static_cast<float> (220.0 / fs);
    const float dtSlave = static_cast<float> (587.0 / fs); // unrelated ratio

    std::vector<float> x (static_cast<size_t> (fs));
    float wrapMaster = -1.0f, unused = -1.0f;
    int syncs = 0;
    for (auto& s : x)
    {
        master.process (dtMaster, 0.5f, -1.0f, wrapMaster);
        if (wrapMaster >= 0.0f)
            ++syncs;
        s = slave.process (dtSlave, 0.5f, wrapMaster, unused);
        REQUIRE (std::isfinite (s));
        REQUIRE (std::abs (s) < 1.6f);
    }
    CHECK (std::abs (syncs - 220) <= 1);

    // The synced waveform repeats at the master period: compare consecutive cycles.
    // 11 periods of 220 Hz at 96 kHz are exactly 4800 samples.
    constexpr size_t elevenPeriods = 4800;
    double diff = 0.0;
    for (size_t i = 48000; i < 48000 + 437; ++i)
        diff = std::max (diff, static_cast<double> (std::abs (x[i] - x[i + elevenPeriods])));
    CHECK (diff < 0.05);
}

TEST_CASE ("VCO stays bounded under extreme audio-rate FM", "[osc]")
{
    const Wave wave = GENERATE (Wave::Saw, Wave::Square, Wave::Pulse, Wave::Triangle, Wave::Sine, Wave::Noise);
    Oscillator osc;
    osc.setWave (wave);
    vs::dsp::Random rng (42);
    float wrap = 0.0f;
    for (int i = 0; i < 200000; ++i)
    {
        const float dt = 1.0e-6f + 0.449f * rng.unipolar();
        const float w = 0.05f + 0.9f * rng.unipolar();
        const float s = osc.process (dt, w, (i % 7 == 0) ? rng.unipolar() : -1.0f, wrap);
        REQUIRE (std::isfinite (s));
        REQUIRE (std::abs (s) < 3.0f);
    }
}

// Diagnostic (hidden; run with "[.diag]"): aliasing of the raw oscillator at the
// oversampled rate, before decimation. Separates oscillator aliasing from decimator leakage.
TEST_CASE ("Diagnostic: raw oscillator aliasing at 2x rate", "[.diag]")
{
    for (const Wave wave : { Wave::Saw, Wave::Square })
        for (const double hz : { 440.0, 1000.0, 2000.0, 5000.0 })
        {
            const int order = kFftOrder + 1;
            const int n = 1 << order;
            const double osRate = 2.0 * kBaseRate;
            const int k = binFor (hz); // same k: at 2N points and 2x rate this is the same Hz
            Oscillator osc;
            osc.setWave (wave);
            float wrap = 0.0f;
            const float dt = static_cast<float> (k) / static_cast<float> (n);
            for (int i = 0; i < 4096; ++i)
                osc.process (dt, 0.5f, -1.0f, wrap);
            std::vector<float> x (static_cast<size_t> (n));
            for (auto& s : x)
                s = osc.process (dt, 0.5f, -1.0f, wrap);
            const auto mag = vs::test::magnitudeSpectrum (x, order);
            double worst = -300.0, worstHz = 0.0;
            for (size_t b = 1; b < mag.size(); ++b)
            {
                const double f = static_cast<double> (b) * osRate / n;
                if (f > 20000.0)
                    break;
                if (b % static_cast<size_t> (k) == 0)
                    continue;
                const double db = vs::test::toDb (mag[b] / mag[static_cast<size_t> (k)]);
                if (db > worst)
                {
                    worst = db;
                    worstHz = f;
                }
            }
            WARN ("raw wave " << static_cast<int> (wave) << " @ " << hz << ": " << worst << " dB at " << worstHz);
        }
}
