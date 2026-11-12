// STUDY (cross-mod aliasing, branch study/crossmod-aliasing). Hidden audition render:
//   VS_RENDER_DIR=<dir> build/mac-arm64/tests/VintageSynthTests "[.study]"
// Writes crossmod_study.wav (mono, 48 kHz, 2x internal) and crossmod_study_scenes.txt: every scene
// is played twice, first with the production cross-mod path, then with the proposed one (study mode 1).
#include <catch2/catch_test_macros.hpp>

#include "engine/Part.h"
#include "engine/Voice.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 256;

struct StudyScene
{
    const char* name;
    void (*setup) (vs::PartParams&);
    std::vector<std::tuple<double, int, bool>> notes;
    double length;
};

std::vector<std::tuple<double, int, bool>> line (std::initializer_list<int> notes, double step, double gap)
{
    std::vector<std::tuple<double, int, bool>> out;
    double t = 0.1;
    for (int n : notes)
    {
        out.push_back ({ t, n, true });
        out.push_back ({ t + step - gap, n, false });
        t += step;
    }
    return out;
}

void renderStudyScene (const StudyScene& s, int mode, std::vector<float>& out)
{
    vs::study::crossModMode = mode;
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
                events.push_back ({ pos - start, on ? vs::EventType::NoteOn : vs::EventType::NoteOff, static_cast<std::uint8_t> (note), on ? 0.8f : 0.0f });
        }
        std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b) { return a.offset < b.offset; });
        part.render (buf.data(), len, events);
        for (int i = 0; i < len; ++i)
            out.push_back (0.5f * buf[static_cast<size_t> (i)]);
    }
    out.insert (out.end(), static_cast<size_t> (kFs * 0.6), 0.0f);
    vs::study::crossModMode = 0;
}
} // namespace

TEST_CASE ("Study: cross-mod audition, production path vs proposed path", "[.study]")
{
    const StudyScene scenes[] = {
        { "bell (sine <- sine 4', cross-mod 0.55 = 1.2 oct)",
          [] (vs::PartParams& p) { p.vco1Wave = 0; p.vco2Wave = 0; p.vco2Range = 2; p.vco1CrossMod = 0.55f; p.sourceMix = 0.0f; p.vcfCutoff = 0.95f; p.vcfEnvMod = 0.0f;
                                   p.env2Attack = 0.0f; p.env2Decay = 0.75f; p.env2Sustain = 0.0f; p.env2Release = 0.7f; p.vco2FineTune = 0.37f; },
          line ({ 72, 79, 76, 84, 67 }, 0.7, 0.1), 3.9 },
        { "metallic lead (saw <- square, cross-mod 0.45 = 0.8 oct)",
          [] (vs::PartParams& p) { p.vco1Wave = 2; p.vco2Wave = 3; p.vco2Range = 1; p.vco2FineTune = 0.5f; p.vco1CrossMod = 0.45f; p.sourceMix = 0.0f;
                                   p.vcfCutoff = 0.85f; p.vcfResonance = 0.3f; p.vcfEnvMod = 0.2f; p.env2Sustain = 0.9f; p.vcoModLfo = 0.08f; },
          line ({ 57, 60, 64, 69, 72, 76 }, 0.55, 0.05), 3.6 },
        { "sync sweep with cross-mod (saw <- saw, sync, cross-mod 0.35 = 0.5 oct, ENV-1 -> VCO-1 pitch)",
          [] (vs::PartParams& p) { p.vco1Wave = 2; p.vco2Wave = 2; p.vcoSync = 1; p.vco1CrossMod = 0.35f; p.vcoModEnv = 0.75f; p.vcoModTarget = 0; p.sourceMix = 0.0f;
                                   p.env1Attack = 0.0f; p.env1Decay = 0.7f; p.env1Sustain = 0.0f; p.vcfCutoff = 0.9f; p.vcfEnvMod = 0.0f; p.env2Sustain = 1.0f; },
          line ({ 48, 55, 60, 52 }, 0.9, 0.1), 3.8 },
        { "harsh extreme (pulse <- saw, sync, cross-mod 0.9 = 3.2 oct)",
          [] (vs::PartParams& p) { p.vco1Wave = 4; p.vco2Wave = 2; p.pwmDepth = 0.6f; p.pwmSource = 1; p.vcoSync = 1; p.vco1CrossMod = 0.9f; p.sourceMix = 0.0f;
                                   p.vcfCutoff = 1.0f; p.vcfEnvMod = 0.0f; p.env2Sustain = 1.0f; p.vcaLevel = 0.6f; },
          line ({ 60, 67, 72, 79 }, 0.8, 0.1), 3.4 },
        { "cross-mod alone, high modulator (saw <- saw 2', cross-mod 0.5 = 1 oct)",
          [] (vs::PartParams& p) { p.vco1Wave = 2; p.vco2Wave = 2; p.vco2Range = 3; p.vco2FineTune = 0.23f; p.vco1CrossMod = 0.5f; p.sourceMix = 0.0f;
                                   p.vcfCutoff = 1.0f; p.vcfEnvMod = 0.0f; p.env2Sustain = 1.0f; p.vcaLevel = 0.6f; },
          line ({ 64, 71, 76, 83 }, 0.8, 0.1), 3.4 },
    };

    std::vector<float> audio;
    juce::String list;
    for (const auto& s : scenes)
        for (int mode : { 0, 1 })
        {
            list << juce::String (static_cast<double> (audio.size()) / kFs, 2) << " s  " << s.name << (mode == 0 ? "  [before: production]" : "  [after: proposed]") << "\n";
            renderStudyScene (s, mode, audio);
        }

    float peak = 0.0f;
    for (const float v : audio)
        peak = std::max (peak, std::abs (v));
    WARN ("rendered " << static_cast<double> (audio.size()) / kFs << " s, peak " << juce::Decibels::gainToDecibels (peak) << " dBFS");
    REQUIRE (peak < 1.0f);

    const auto envDir = juce::SystemStats::getEnvironmentVariable ("VS_RENDER_DIR", {});
    auto dir = envDir.isNotEmpty() ? juce::File (envDir) : juce::File::getCurrentWorkingDirectory().getChildFile ("renders");
    dir.createDirectory();
    auto file = dir.getChildFile ("crossmod_study.wav");
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (kFs).withNumChannels (1).withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, options);
    REQUIRE (writer != nullptr);
    const float* ch[] { audio.data() };
    writer->writeFromFloatArrays (ch, 1, static_cast<int> (audio.size()));
    dir.getChildFile ("crossmod_study_scenes.txt").replaceWithText (list);
    WARN ("wrote " << file.getFullPathName() << "\n" << list);
}
