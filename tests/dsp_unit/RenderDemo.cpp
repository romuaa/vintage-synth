// Hidden: renders an audition WAV of M1 features.
//   build/mac/tests/VintageSynthTests "[.render]"
// Output: $VS_RENDER_DIR (if set) or <working dir>/renders: m1_demo.wav + m1_demo_spectrogram.png
#include <catch2/catch_test_macros.hpp>

#include "engine/Part.h"
#include "params/Mappings.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_graphics/juce_graphics.h>

#include <algorithm>

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 256;

struct Scene
{
    const char* name;
    void (*setup) (vs::PartParams&);
    std::vector<std::tuple<double, int, bool>> notes; // seconds, note, on
    double length;
    void (*automate) (vs::PartParams&, double t) = nullptr;
};

void renderScene (const Scene& s, std::vector<float>& out)
{
    vs::Part part;
    vs::PartParams p;
    s.setup (p);
    part.setParams (p);
    part.prepare (kFs, kBlock, 2, 99u);

    const int total = static_cast<int> (s.length * kFs);
    std::vector<float> buf (kBlock);
    for (int start = 0; start < total; start += kBlock)
    {
        const int len = std::min (kBlock, total - start);
        std::vector<vs::NoteEvent> events;
        for (const auto& [t, note, on] : s.notes)
        {
            const int pos = static_cast<int> (t * kFs);
            if (pos >= start && pos < start + len)
                events.push_back ({ pos - start, on ? vs::EventType::NoteOn : vs::EventType::NoteOff,
                                    static_cast<std::uint8_t> (note), on ? 0.8f : 0.0f });
        }
        std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b) { return a.offset < b.offset; });
        if (s.automate != nullptr)
            s.automate (p, start / kFs);
        part.setParams (p);
        part.render (buf.data(), len, events);
        for (int i = 0; i < len; ++i)
            out.push_back (0.5f * buf[static_cast<size_t> (i)]);
    }
    out.insert (out.end(), static_cast<size_t> (kFs * 0.5), 0.0f); // gap
}

/** Spectrogram PNG (log-magnitude, -130..0 dB, 0..24 kHz linear) for visual review. */
void writeSpectrogram (const std::vector<float>& audio, const juce::File& file)
{
    constexpr int order = 11, size = 1 << order, hop = 512, height = 512;
    const int frames = static_cast<int> (audio.size() - size) / hop;
    juce::Image img (juce::Image::RGB, frames, height, true);
    juce::dsp::FFT fft (order);
    juce::dsp::WindowingFunction<float> window (size, juce::dsp::WindowingFunction<float>::blackmanHarris, false);
    std::vector<float> buf (2 * size);

    for (int f = 0; f < frames; ++f)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        std::copy_n (audio.begin() + f * hop, size, buf.begin());
        window.multiplyWithWindowingTable (buf.data(), size);
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        for (int y = 0; y < height; ++y)
        {
            const int bin0 = y * (size / 2) / height, bin1 = (y + 1) * (size / 2) / height;
            float m = 0.0f;
            for (int b = bin0; b < bin1; ++b)
                m = std::max (m, buf[static_cast<size_t> (b)]);
            const float db = juce::Decibels::gainToDecibels (m / (size * 0.36f), -200.0f);
            const float v = juce::jlimit (0.0f, 1.0f, (db + 130.0f) / 130.0f);
            img.setPixelAt (f, height - 1 - y, juce::Colour::fromHSV (0.7f - 0.7f * v, 0.9f, v, 1.0f));
        }
    }
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat().writeImageToStream (img, out);
}

std::vector<std::tuple<double, int, bool>> riff (std::initializer_list<int> notes, double step, double gate)
{
    std::vector<std::tuple<double, int, bool>> ev;
    double t = 0.1;
    for (const int n : notes)
    {
        ev.emplace_back (t, n, true);
        ev.emplace_back (t + gate, n, false);
        t += step;
    }
    return ev;
}
} // namespace

TEST_CASE ("Render M1 demo WAV", "[.render]")
{
    const std::vector<Scene> scenes {
        { "1 default patch (2 saws, 24 dB, env)", [] (vs::PartParams&) {},
          riff ({ 36, 48, 43, 46, 36, 48, 51, 50 }, 0.3, 0.2), 2.8 },
        { "2 resonant sweep -24 dB", [] (vs::PartParams& p) { p.vcfResonance = 0.85f; p.vcfEnvMod = 0.0f; p.env2Sustain = 1.0f; },
          { { 0.1, 36, true }, { 3.9, 36, false } }, 4.5,
          [] (vs::PartParams& p, double t) { p.vcfCutoff = 0.25f + 0.6f * static_cast<float> (std::sin (t * 1.2)) * static_cast<float> (std::sin (t * 1.2)); } },
        { "3 resonant sweep -12 dB", [] (vs::PartParams& p) { p.vcfSlope = 0; p.vcfResonance = 0.85f; p.vcfEnvMod = 0.0f; p.env2Sustain = 1.0f; },
          { { 0.1, 36, true }, { 3.9, 36, false } }, 4.5,
          [] (vs::PartParams& p, double t) { p.vcfCutoff = 0.25f + 0.6f * static_cast<float> (std::sin (t * 1.2)) * static_cast<float> (std::sin (t * 1.2)); } },
        { "4 hard sync + ENV pitch sweep", [] (vs::PartParams& p) { p.vcoSync = 1; p.sourceMix = 0.0f; p.vcoModTarget = 0; p.vcoModEnv = 0.75f; p.env1Decay = 0.6f; p.env1Sustain = 0.0f; p.vcfCutoff = 0.8f; },
          riff ({ 40, 40, 43, 45, 40, 40, 47, 45 }, 0.35, 0.3), 3.2 },
        { "5 PWM (LFO)", [] (vs::PartParams& p) { p.vco1Wave = 4; p.vco2Wave = 4; p.pwmDepth = 0.8f; p.lfoRate = 0.45f; p.vcfCutoff = 0.7f; p.env2Sustain = 1.0f; },
          { { 0.1, 48, true }, { 3.5, 48, false } }, 4.0 },
        { "6 cross mod bell", [] (vs::PartParams& p) { p.vco1Wave = 0; p.vco2Wave = 0; p.vco2Range = 2; p.vco1CrossMod = 0.55f; p.sourceMix = 0.0f; p.vcfCutoff = 0.9f; p.env2Decay = 0.75f; p.env2Sustain = 0.0f; p.env2Release = 0.7f; },
          riff ({ 72, 79, 76, 84 }, 0.6, 0.1), 3.5 },
        { "7 delayed vibrato", [] (vs::PartParams& p) { p.vcoModLfo = 0.22f; p.lfoDelay = 0.6f; p.lfoWave = 0; p.vcfCutoff = 0.65f; p.env2Sustain = 1.0f; },
          { { 0.1, 57, true }, { 3.5, 57, false } }, 4.0 },
        { "8 self-oscillating filter, keyboard tracked", [] (vs::PartParams& p) { p.sourceMix = 0.0f; p.vcaLevel = 0.6f; p.vcfResonance = 1.0f; p.vcfKeyFollow = 1.0f; p.vcfCutoff = 0.45f; p.vcfEnvMod = 0.0f; p.env2Sustain = 1.0f; p.hpfCutoff = 1.0f; },
          riff ({ 60, 64, 67, 72 }, 0.5, 0.45), 2.5 },
    };

    std::vector<float> audio;
    for (const auto& s : scenes)
        renderScene (s, audio);

    float peak = 0.0f;
    for (const float v : audio)
        peak = std::max (peak, std::abs (v));
    WARN ("rendered " << static_cast<double> (audio.size()) / kFs << " s, peak " << juce::Decibels::gainToDecibels (peak) << " dBFS");
    REQUIRE (peak < 1.0f);

    const auto envDir = juce::SystemStats::getEnvironmentVariable ("VS_RENDER_DIR", {});
    auto dir = envDir.isNotEmpty() ? juce::File (envDir) : juce::File::getCurrentWorkingDirectory().getChildFile ("renders");
    dir.createDirectory();
    auto file = dir.getChildFile ("m1_demo.wav");
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (kFs).withNumChannels (1).withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, options);
    REQUIRE (writer != nullptr);
    const float* ch[] { audio.data() };
    writer->writeFromFloatArrays (ch, 1, static_cast<int> (audio.size()));
    WARN ("wrote " << file.getFullPathName());

    writeSpectrogram (audio, dir.getChildFile ("m1_demo_spectrogram.png"));
}
