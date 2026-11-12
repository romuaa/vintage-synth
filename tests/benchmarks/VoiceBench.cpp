// CPU benchmark (polyphony baseline). Reports cost as % of one core in real time for:
//   1. one Part with one held voice (2x / 4x oversampling; the original measurement),
//   2. N vs::Voice objects driven directly at the oversampled rate (no Part),
//   3. the derived per-part overhead and a projection against the 15 % budget.
// Run: build/mac-arm64/tests/voice_bench
#include "dsp/Lfo.h"
#include "engine/Part.h"
#include "engine/Voice.h"
#include "params/Mappings.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
constexpr double fs = 48000.0;
constexpr int block = 256;
constexpr int seconds = 5; // audio per timed run
constexpr int repeats = 3; // best of

vs::PartParams benchParams()
{
    vs::PartParams p;
    p.vcoSync = 1;
    p.vco1CrossMod = 0.3f;
    p.vcfResonance = 0.6f;
    p.vcoModLfo = 0.1f;
    p.env2Sustain = 1.0f;
    return p;
}

template <typename Fn>
double bestOf (Fn&& run)
{
    double best = 1.0e9;
    for (int r = 0; r < repeats; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        run();
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min (best, std::chrono::duration<double> (t1 - t0).count());
    }
    return 100.0 * best / seconds; // % of one core
}

volatile float gSink = 0.0f; // keeps the work observable

// One Part, one held voice, rendered through Part::render.
double benchPart (int factor)
{
    return bestOf ([factor] {
        juce::ScopedNoDenormals noDenormals;
        vs::Part part;
        part.setParams (benchParams());
        part.prepare (fs, block, factor, 1u);
        part.setVintage (0.5f);

        const vs::NoteEvent on[] { { 0, vs::EventType::NoteOn, 48, 0.8f } };
        std::vector<float> out (block);

        double sum = 0.0;
        const int blocks = static_cast<int> (fs) * seconds / block;
        for (int b = 0; b < blocks; ++b)
        {
            part.render (out.data(), block, b == 0 ? std::span<const vs::NoteEvent> (on) : std::span<const vs::NoteEvent> {});
            sum += static_cast<double> (out[0]);
        }
        gSink = static_cast<float> (sum);
    });
}

// N voices driven directly at the oversampled rate. The ModFrame is built per sample like
// Part::renderOversampled (parameters are constant, so no smoothing cost is included).
double benchVoices (int factor, int n)
{
    return bestOf ([factor, n] {
        juce::ScopedNoDenormals noDenormals;
        const float osRate = static_cast<float> (fs) * static_cast<float> (factor);
        const vs::PartParams p = benchParams();
        const float vintage = 0.5f;

        vs::dsp::Lfo lfo;
        lfo.prepare (osRate, 1u ^ 0x5bd1e995u);
        lfo.setWave (static_cast<vs::dsp::LfoWave> (p.lfoWave));
        lfo.setRate (vs::map::lfoRateHz (p.lfoRate));
        lfo.setDelay (vs::map::lfoDelaySec (p.lfoDelay));
        lfo.trigger();

        std::vector<vs::Voice> voices (static_cast<size_t> (n));
        for (int v = 0; v < n; ++v)
        {
            auto& voice = voices[static_cast<size_t> (v)];
            voice.prepare (osRate, 1u + static_cast<std::uint32_t> (v) * 7919u);
            voice.setBlockParams (p, vintage);
            const int note = n == 1 ? 48 : 36 + (v * 48) / (n - 1);
            voice.noteOn (note, 0.8f);
        }

        const float vcaLevel = vs::map::vcaGain (p.vcaLevel);
        double sum = 0.0;
        const long total = static_cast<long> (fs) * seconds * factor;
        for (long i = 0; i < total; ++i)
        {
            const float lfoValue = lfo.process();
            const float tremolo = 1.0f - vs::map::vcaLfoDepth (p.vcaLfoMod) * 0.5f * (1.0f - lfoValue);
            const vs::ModFrame frame { p,
                                       lfoValue,
                                       0.0f,
                                       vs::dsp::tptG (vs::map::hpfHz (p.hpfCutoff), osRate),
                                       vcaLevel * tremolo,
                                       vintage };
            float mix = 0.0f;
            for (auto& voice : voices)
                mix += voice.process (frame);
            sum += static_cast<double> (mix);
        }
        gSink = static_cast<float> (sum);
    });
}
} // namespace

int main()
{
    // 1. Original measurement (output line format unchanged).
    double part1[2] {};
    for (int k = 0; k < 2; ++k)
    {
        const int factor = k == 0 ? 2 : 4;
        part1[k] = benchPart (factor);
        std::printf ("%dx oversampling: 1 voice = %.3f%% of one core (%.1f voices per 10%%)\n",
                     factor, part1[k], 10.0 / part1[k]);
    }

    // 2. Voices only.
    std::printf ("\nVoices only (no Part: shared ModFrame + LFO per sample, no param smoothing, no decimator)\n");
    constexpr int counts[3] { 1, 16, 32 };
    double v[2][3] {};
    for (int k = 0; k < 2; ++k)
    {
        const int factor = k == 0 ? 2 : 4;
        for (int c = 0; c < 3; ++c)
        {
            v[k][c] = benchVoices (factor, counts[c]);
            std::printf ("%dx, %2d voices: %.3f%% of one core, %.3f%% per voice\n",
                         factor, counts[c], v[k][c], v[k][c] / counts[c]);
        }
        std::printf ("%dx, marginal cost per extra voice ((32 voices - 1 voice) / 31): %.3f%%\n",
                     factor, (v[k][2] - v[k][0]) / 31.0);
    }

    // 3. Overhead and projection.
    std::printf ("\nPer-part overhead = (Part with 1 voice) - (1 voice only) = param smoothing + decimator + MIDI/block handling\n");
    constexpr double budget = 15.0;
    for (int k = 0; k < 2; ++k)
    {
        const int factor = k == 0 ? 2 : 4;
        const double overhead = part1[k] - v[k][0];
        const double perVoice = (v[k][2] - v[k][0]) / 31.0;
        const double partFixed = part1[k] - perVoice; // everything in a Part that is not one marginal voice
        std::printf ("%dx: overhead = %.3f%% - %.3f%% = %.3f%%; per-part fixed cost (Part with 1 voice - marginal voice) = %.3f%%\n",
                     factor, part1[k], v[k][0], overhead, partFixed);
        const double c16 = 16.0 * perVoice + partFixed;
        const double c32one = 32.0 * perVoice + partFixed;
        const double c32two = 32.0 * perVoice + 2.0 * partFixed;
        std::printf ("%dx projection (voices*marginal + parts*fixed) vs %.0f%% budget: "
                     "16 voices/1 Part = %.2f%% (%s), 32 voices/1 Part = %.2f%% (%s), 32 voices/2 Parts = %.2f%% (%s)\n",
                     factor, budget, c16, c16 <= budget ? "ok" : "OVER",
                     c32one, c32one <= budget ? "ok" : "OVER", c32two, c32two <= budget ? "ok" : "OVER");
    }
    return 0;
}
