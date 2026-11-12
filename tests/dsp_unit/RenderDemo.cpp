// Hidden: renders audition WAVs of M1 features (m1_demo) and M2 polyphony (m2_demo).
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

// =============================================================================================
// M2 audition: polyphony. Output: m2_demo.wav, m2_demo_spectrogram.png, m2_demo_scenes.txt
// =============================================================================================
namespace
{
struct TimedEvent
{
    double t;
    vs::EventType type;
    int note;
    float value;
};

struct PolyScene
{
    const char* name;
    void (*setup) (vs::PartParams&);
    std::vector<TimedEvent> events;
    double length;
    float gain;
    float vintage = 0.0f;
};

using ET = vs::EventType;

void addChord (std::vector<TimedEvent>& ev, double t, double dur, std::initializer_list<int> notes, float vel)
{
    for (const int n : notes)
    {
        ev.push_back ({ t, ET::NoteOn, n, vel });
        ev.push_back ({ t + dur, ET::NoteOff, n, 0.0f });
    }
}

void addNote (std::vector<TimedEvent>& ev, double t, double dur, int note, float vel)
{
    ev.push_back ({ t, ET::NoteOn, note, vel });
    ev.push_back ({ t + dur, ET::NoteOff, note, 0.0f });
}

void renderPolyScene (const PolyScene& s, std::vector<float>& out)
{
    vs::Part part;
    vs::PartParams p;
    s.setup (p);
    part.setParams (p);
    part.prepare (kFs, kBlock, 2, 0x1001u);
    part.setVintage (s.vintage);

    auto events = s.events;
    std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b) { return a.t < b.t; });

    const int total = static_cast<int> (s.length * kFs);
    std::vector<float> buf (kBlock);
    std::vector<vs::NoteEvent> chunk;
    size_t next = 0;
    for (int start = 0; start < total; start += kBlock)
    {
        const int len = std::min (kBlock, total - start);
        chunk.clear();
        while (next < events.size() && static_cast<int> (events[next].t * kFs) < start + len)
        {
            const auto& e = events[next++];
            chunk.push_back ({ std::max (0, static_cast<int> (e.t * kFs) - start), e.type, static_cast<std::uint8_t> (e.note), e.value });
        }
        part.render (buf.data(), len, chunk);
        for (int i = 0; i < len; ++i)
            out.push_back (s.gain * buf[static_cast<size_t> (i)]);
    }
    out.insert (out.end(), static_cast<size_t> (kFs * 0.6), 0.0f); // gap
}

std::vector<PolyScene> m2Scenes()
{
    std::vector<PolyScene> scenes;

    { // 1. Pad: slow attack, wide chords (up to 8 notes held at once), vibrato from the wheel at the end.
        std::vector<TimedEvent> ev;
        addChord (ev, 0.2, 2.3, { 48, 55, 59, 62, 64, 67, 71 }, 0.7f);     // Cmaj9
        addChord (ev, 2.5, 2.3, { 45, 52, 55, 60, 62, 67, 71, 74 }, 0.7f); // Am11
        addChord (ev, 4.8, 2.3, { 41, 48, 52, 57, 60, 64, 69 }, 0.7f);     // Fmaj7
        addChord (ev, 7.1, 3.0, { 43, 50, 55, 60, 62, 67, 74 }, 0.75f);    // Gsus
        for (int k = 0; k <= 20; ++k)
            ev.push_back ({ 8.0 + 0.05 * k, ET::ModWheel, 0, static_cast<float> (k) / 20.0f });
        scenes.push_back ({ "1 pad chords (Poly, up to 8 voices, wheel vibrato in the last chord)", [] (vs::PartParams& p) {
                               p.vco1Wave = 2; p.vco2Wave = 2; p.vco2FineTune = 0.12f;
                               p.vcfCutoff = 0.5f; p.vcfEnvMod = 0.15f; p.vcfResonance = 0.15f;
                               p.env1Attack = 0.65f; p.env1Decay = 0.7f; p.env1Sustain = 0.6f;
                               p.env2Attack = 0.62f; p.env2Decay = 0.7f; p.env2Sustain = 0.9f; p.env2Release = 0.72f;
                               p.lfoRate = 0.55f; p.lfoWave = 0; p.modWheelDepth = 0.25f; p.velToVca = 0.3f; },
                           ev, 11.5, 0.12f, 0.4f });
    }

    { // 2. Fast arpeggio with a long release: more than 16 voices wanted -> steals (oldest release first),
      //    then the pedal holds everything (steals of held voices), velocity accents.
        std::vector<TimedEvent> ev;
        const int pattern[] { 48, 52, 55, 60, 64, 67, 72, 76, 79, 76, 72, 67, 64, 60, 55, 52 };
        double t = 0.2;
        for (int bar = 0; bar < 6; ++bar)
            for (int i = 0; i < 16; ++i)
            {
                const int transpose = (bar % 3 == 1) ? -3 : ((bar % 3 == 2) ? 5 : 0);
                const float vel = i % 4 == 0 ? 1.0f : (i % 2 == 0 ? 0.65f : 0.45f);
                addNote (ev, t, 0.05, pattern[i] + transpose, vel);
                t += 0.07;
            }
        ev.push_back ({ 0.2 + 3 * 16 * 0.07, ET::SustainPedal, 0, 1.0f });
        ev.push_back ({ t + 0.3, ET::SustainPedal, 0, 0.0f });
        scenes.push_back ({ "2 fast arpeggio forcing steals (long release; pedal from bar 4)", [] (vs::PartParams& p) {
                               p.vco1Wave = 4; p.pwmDepth = 0.3f; p.sourceMix = 0.35f; p.vco2Range = 2;
                               p.vcfCutoff = 0.42f; p.vcfEnvMod = 0.45f; p.vcfResonance = 0.35f;
                               p.env1Attack = 0.0f; p.env1Decay = 0.4f; p.env1Sustain = 0.0f; p.env1Release = 0.6f;
                               p.env2Attack = 0.0f; p.env2Decay = 0.55f; p.env2Sustain = 0.0f; p.env2Release = 0.75f;
                               p.velToVca = 0.6f; p.velToVcf = 0.5f; },
                           ev, 8.0, 0.2f });
    }

    { // 3. Unison lead (8 voices) with portamento: Legato mode, so overlapped notes glide and detached ones don't.
        std::vector<TimedEvent> ev;
        struct N { double t, d; int n; };
        const N melody[] { { 0.2, 0.9, 57 }, { 1.0, 0.5, 64 }, { 1.45, 0.5, 62 }, { 1.9, 0.9, 69 }, // legato run
                           { 3.0, 0.3, 67 }, { 3.5, 0.3, 64 },                                      // detached
                           { 4.0, 1.2, 60 }, { 5.1, 1.6, 72 } };                                     // legato leap
        for (const auto& m : melody)
            addNote (ev, m.t, m.d, m.n, 0.9f);
        for (int k = 0; k <= 10; ++k)
            ev.push_back ({ 5.6 + 0.08 * k, ET::ModWheel, 0, static_cast<float> (k) / 10.0f });
        ev.push_back ({ 7.0, ET::ModWheel, 0, 0.0f });
        scenes.push_back ({ "3 unison lead, 8 voices, portamento (Legato mode), wheel vibrato on the last note", [] (vs::PartParams& p) {
                               p.assignMode = 1; p.unisonVoices = 2; p.unisonDetune = 0.45f;
                               p.portaTime = vs::map::portaFromSec (0.25f); p.portaMode = 1;
                               p.vco1Wave = 2; p.vco2Wave = 3; p.vco2Range = 0;
                               p.vcfCutoff = 0.55f; p.vcfEnvMod = 0.3f; p.vcfResonance = 0.3f;
                               p.env1Attack = 0.35f; p.env1Sustain = 0.5f; p.env2Attack = 0.1f; p.env2Sustain = 0.85f; p.env2Release = 0.5f;
                               p.lfoRate = 0.62f; p.lfoWave = 0; p.modWheelDepth = 0.35f; },
                           ev, 7.5, 0.25f, 0.5f });
    }

    { // 4. Sustain pedal: broken chords released at once but held by the pedal; pedal changes per chord.
        std::vector<TimedEvent> ev;
        const int chords[3][5] { { 48, 55, 60, 64, 67 }, { 45, 52, 57, 60, 64 }, { 41, 48, 53, 57, 60 } };
        for (int c = 0; c < 3; ++c)
        {
            const double t0 = 0.2 + 2.0 * c;
            ev.push_back ({ t0 - 0.02, ET::SustainPedal, 0, 0.0f });
            ev.push_back ({ t0 + 0.01, ET::SustainPedal, 0, 1.0f });
            for (int i = 0; i < 5; ++i)
                addNote (ev, t0 + 0.12 * i, 0.15, chords[c][i], 0.55f + 0.08f * static_cast<float> (i));
        }
        ev.push_back ({ 6.4, ET::SustainPedal, 0, 0.0f });
        scenes.push_back ({ "4 pedal-held broken chords (keys up after 150 ms; pedal changes per chord)", [] (vs::PartParams& p) {
                               p.vco1Wave = 1; p.vco2Wave = 0; p.vco2Range = 2; p.sourceMix = 0.4f;
                               p.vcfCutoff = 0.62f; p.vcfEnvMod = 0.25f;
                               p.env1Attack = 0.0f; p.env1Decay = 0.6f; p.env1Sustain = 0.1f;
                               p.env2Attack = 0.0f; p.env2Decay = 0.78f; p.env2Sustain = 0.0f; p.env2Release = 0.62f;
                               p.velToVca = 0.5f; p.velToVcf = 0.3f; },
                           ev, 7.5, 0.25f });
    }

    { // 5. Velocity dynamics: one brass chord, crescendo then decrescendo (velocity -> VCA and VCF).
        std::vector<TimedEvent> ev;
        const float vels[] { 0.12f, 0.25f, 0.4f, 0.55f, 0.7f, 0.85f, 1.0f, 0.75f, 0.5f, 0.3f, 0.15f };
        double t = 0.2;
        for (const float v : vels)
        {
            addChord (ev, t, 0.4, { 50, 57, 62, 65 }, v);
            t += 0.55;
        }
        scenes.push_back ({ "5 velocity dynamics: brass chord crescendo/decrescendo (vel -> VCA 0.8, -> VCF 0.8)", [] (vs::PartParams& p) {
                               p.vco1Wave = 2; p.vco2Wave = 2; p.vco2FineTune = 0.1f;
                               p.vcfCutoff = 0.3f; p.vcfEnvMod = 0.6f; p.vcfResonance = 0.2f;
                               p.env1Attack = 0.25f; p.env1Decay = 0.5f; p.env1Sustain = 0.45f;
                               p.env2Attack = 0.2f; p.env2Sustain = 0.8f; p.env2Release = 0.5f;
                               p.velToVca = 0.8f; p.velToVcf = 0.8f; },
                           ev, 7.0, 0.3f });
    }

    { // 6. Solo bass with portamento Always (glides on every note) and pitch bend range 12.
        std::vector<TimedEvent> ev;
        const int line[] { 36, 36, 48, 43, 41, 41, 53, 46, 36, 39, 43, 31 };
        double t = 0.2;
        for (const int n : line)
        {
            addNote (ev, t, 0.2, n, 0.85f);
            t += 0.25;
        }
        ev.push_back ({ 2.6, ET::PitchBend, 0, -1.0f });
        ev.push_back ({ 3.2, ET::PitchBend, 0, 0.0f });
        scenes.push_back ({ "6 solo bass, portamento Always (60 ms), octave bend at the end", [] (vs::PartParams& p) {
                               p.assignMode = 2; p.portaTime = vs::map::portaFromSec (0.06f); p.bendRange = 12;
                               p.vco1Wave = 3; p.vco2Wave = 2; p.vco1Range = 1;
                               p.vcfCutoff = 0.3f; p.vcfEnvMod = 0.5f; p.vcfResonance = 0.5f;
                               p.env1Attack = 0.0f; p.env1Decay = 0.35f; p.env1Sustain = 0.1f;
                               p.env2Attack = 0.0f; p.env2Sustain = 0.9f; p.env2Release = 0.3f; },
                           ev, 4.0, 0.35f });
    }
    return scenes;
}
} // namespace

TEST_CASE ("Render M2 polyphony demo WAV", "[.render]")
{
    std::vector<float> audio;
    juce::String cues;
    for (const auto& s : m2Scenes())
    {
        cues << juce::String (static_cast<double> (audio.size()) / kFs, 1) << " s  " << s.name << "\n";
        renderPolyScene (s, audio);
    }

    float peak = 0.0f;
    for (const float v : audio)
        peak = std::max (peak, std::abs (v));
    WARN ("rendered " << static_cast<double> (audio.size()) / kFs << " s, peak " << juce::Decibels::gainToDecibels (peak) << " dBFS\n" << cues);
    REQUIRE (peak < 1.0f);

    const auto envDir = juce::SystemStats::getEnvironmentVariable ("VS_RENDER_DIR", {});
    auto dir = envDir.isNotEmpty() ? juce::File (envDir) : juce::File::getCurrentWorkingDirectory().getChildFile ("renders");
    dir.createDirectory();
    auto file = dir.getChildFile ("m2_demo.wav");
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (kFs).withNumChannels (1).withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, options);
    REQUIRE (writer != nullptr);
    const float* ch[] { audio.data() };
    writer->writeFromFloatArrays (ch, 1, static_cast<int> (audio.size()));
    WARN ("wrote " << file.getFullPathName());

    dir.getChildFile ("m2_demo_scenes.txt").replaceWithText (cues);
    writeSpectrogram (audio, dir.getChildFile ("m2_demo_spectrogram.png"));
}
