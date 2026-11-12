// Hidden: renders the M3 audition WAV (m3_demo, stereo) through the real processor.
//   build/mac/tests/VintageSynthTests "[.render]"
// Output: $VS_RENDER_DIR (if set) or <working dir>/renders: m3_demo.wav + m3_demo_scenes.txt
#include <catch2/catch_test_macros.hpp>

#include "PluginProcessor.h"
#include "params/ParameterLayout.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 256;

struct Ev
{
    double t;
    int note;     // -1: parameter change
    int velocity; // 0 = note-off
    const char* id = nullptr;
    float value = 0.0f;
};

struct M3Scene
{
    const char* name;
    std::vector<std::pair<const char*, float>> setup; // parameter id, plain value
    std::vector<Ev> events;
    double length;
};

void note (std::vector<Ev>& ev, double t, double dur, int n, int vel = 100)
{
    ev.push_back ({ t, n, vel });
    ev.push_back ({ t + dur, n, 0 });
}

void param (std::vector<Ev>& ev, double t, const char* id, float v) { ev.push_back ({ t, -1, 0, id, v }); }

void setParam (vs::VintageSynthProcessor& proc, const char* id, float plain)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
    REQUIRE (p != nullptr);
    p->setValueNotifyingHost (p->convertTo0to1 (plain));
}

void renderScene (const M3Scene& s, std::vector<float>& left, std::vector<float>& right)
{
    vs::VintageSynthProcessor proc;
    for (const auto& [id, v] : s.setup)
        setParam (proc, id, v);
    proc.setPlayConfigDetails (0, 2, kFs, kBlock);
    proc.prepareToPlay (kFs, kBlock);

    auto events = s.events;
    std::stable_sort (events.begin(), events.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; });
    const int total = static_cast<int> (s.length * kFs);
    juce::AudioBuffer<float> buffer (2, kBlock);
    size_t next = 0;
    for (int pos = 0; pos < total;)
    {
        // Parameter changes due now apply before the block; the next one ends the block.
        while (next < events.size() && events[next].note < 0 && static_cast<int> (events[next].t * kFs) <= pos)
        {
            setParam (proc, events[next].id, events[next].value);
            ++next;
        }
        int end = std::min (pos + kBlock, total);
        juce::MidiBuffer midi;
        for (size_t k = next; k < events.size(); ++k)
        {
            const int at = static_cast<int> (events[k].t * kFs);
            if (at >= end)
                break;
            if (events[k].note < 0)
            {
                end = std::max (pos + 1, at);
                break;
            }
            const auto msg = events[k].velocity > 0 ? juce::MidiMessage::noteOn (1, events[k].note, static_cast<juce::uint8> (events[k].velocity))
                                                    : juce::MidiMessage::noteOff (1, events[k].note);
            midi.addEvent (msg, std::max (0, at - pos));
            next = k + 1;
        }
        const int len = end - pos;
        buffer.setSize (2, len, false, false, true);
        buffer.clear();
        proc.processBlock (buffer, midi);
        left.insert (left.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + len);
        right.insert (right.end(), buffer.getReadPointer (1), buffer.getReadPointer (1) + len);
        pos = end;
    }
    left.insert (left.end(), static_cast<size_t> (kFs * 0.6), 0.0f); // gap
    right.insert (right.end(), static_cast<size_t> (kFs * 0.6), 0.0f);
}

// Shared sounds. Lower: a slow, warm saw pad; Upper: a brighter square lead an octave up.
const std::vector<std::pair<const char*, float>> kPadAndLead {
    { "lower.vcf.cutoff", 0.42f }, { "lower.vcf.resonance", 0.35f }, { "lower.env2.attack", 0.42f },
    { "lower.env2.release", 0.6f }, { "lower.vco2.fine_tune", 0.12f }, { "lower.mix.pan", -0.45f },
    { "upper.vco1.waveform", 3.0f }, { "upper.vco2.waveform", 4.0f }, { "upper.vco1.range", 3.0f },
    { "upper.vco2.range", 2.0f }, { "upper.vcf.cutoff", 0.66f }, { "upper.vcf.resonance", 0.25f },
    { "upper.vcomod.lfo_mod", 0.12f }, { "upper.lfo.delay", 0.35f }, { "upper.lfo.rate", 0.62f },
    { "upper.env2.release", 0.4f }, { "upper.mix.pan", 0.45f }, { "upper.vca.level", 0.62f },
    { "global.vintage", 0.4f },
};

// Lower: a plucky Solo bass; Upper: a lead with portamento.
const std::vector<std::pair<const char*, float>> kBassAndLead {
    { "lower.assign.mode", 2.0f }, { "lower.vco1.range", 1.0f }, { "lower.vco2.range", 0.0f },
    { "lower.vcf.cutoff", 0.3f }, { "lower.vcf.resonance", 0.45f }, { "lower.vcf.env_mod", 0.5f },
    { "lower.env1.decay", 0.35f }, { "lower.env1.sustain", 0.1f }, { "lower.env2.decay", 0.5f },
    { "lower.env2.sustain", 0.6f }, { "lower.env2.release", 0.3f }, { "lower.mix.pan", -0.2f },
    { "upper.vco1.waveform", 4.0f }, { "upper.vco1.range", 3.0f }, { "upper.vco2.range", 2.0f },
    { "upper.vcf.cutoff", 0.62f }, { "upper.vcomod.lfo_mod", 0.1f }, { "upper.lfo.delay", 0.3f },
    { "upper.porta.time", 0.12f }, { "upper.porta.mode", 1.0f }, { "upper.env2.release", 0.45f },
    { "upper.mix.pan", 0.35f }, { "upper.vca.level", 0.6f }, { "global.vintage", 0.4f },
};

std::vector<M3Scene> m3Scenes()
{
    std::vector<M3Scene> scenes;

    { // 1. Dual layer
        M3Scene s { "Dual layer: warm saw pad (Lower, panned left) + square/pulse lead an octave up (Upper, right)", kPadAndLead, {}, 9.0 };
        s.setup.push_back ({ vs::ParamID::keyMode, 1.0f });
        const int chords[4][3] { { 48, 55, 64 }, { 45, 52, 60 }, { 41, 48, 57 }, { 43, 50, 59 } };
        for (int c = 0; c < 4; ++c)
            for (const int n : chords[c])
                note (s.events, 0.2 + 2.0 * c, 1.8, n, 80);
        param (s.events, 4.2, vs::ParamID::mixBalance, 0.5f); // more lead in the second half
        scenes.push_back (s);
    }
    { // 2. Split: bass below, lead above
        M3Scene s { "Split at C4: Solo bass below (Lower), gliding lead above (Upper)", kBassAndLead, {}, 9.0 };
        s.setup.push_back ({ vs::ParamID::keyMode, 2.0f });
        s.setup.push_back ({ vs::ParamID::keySplitPoint, 60.0f });
        const int bass[] { 36, 36, 43, 36, 41, 41, 43, 46 };
        for (int i = 0; i < 16; ++i)
            note (s.events, 0.2 + 0.5 * i, 0.32, bass[i % 8], 100);
        const int lead[] { 72, 74, 76, 79, 76, 74, 72, 67, 69, 72 };
        const double at[] { 0.2, 1.0, 1.5, 2.2, 3.0, 3.6, 4.2, 5.2, 6.0, 6.8 };
        for (int i = 0; i < 10; ++i)
            note (s.events, at[i], (i == 9 ? 1.6 : (i + 1 < 10 ? at[i + 1] - at[i] + 0.05 : 0.6)), lead[i], 96); // legato: overlaps glide
        scenes.push_back (s);
    }
    { // 3. Split-point move while notes are held
        M3Scene s { "Split-point move while held: lead E4 held while the split moves C4 -> A4; the next E4 plays the bass", kBassAndLead, {}, 7.0 };
        s.setup.push_back ({ vs::ParamID::keyMode, 2.0f });
        s.setup.push_back ({ vs::ParamID::keySplitPoint, 60.0f });
        note (s.events, 0.2, 3.0, 64, 96); // lead, held across the move ...
        for (int i = 0; i < 5; ++i)
            note (s.events, 0.2 + 0.6 * i, 0.3, 36, 100);
        param (s.events, 1.6, vs::ParamID::keySplitPoint, 69.0f); // ... the split moves under it
        for (int i = 0; i < 4; ++i)
            note (s.events, 3.6 + 0.6 * i, 0.3, i % 2 == 0 ? 64 : 62, 100); // now Lower (bass sound, an octave down)
        note (s.events, 3.6, 2.2, 74, 90); // above the new split: lead
        scenes.push_back (s);
    }
    { // 4. Key-mode change while playing
        M3Scene s { "Key-mode change while playing: Split -> Dual at 3 s (held notes release) -> Whole at 6 s", kBassAndLead, {}, 9.0 };
        s.setup.push_back ({ vs::ParamID::keyMode, 2.0f });
        s.setup.push_back ({ vs::ParamID::keySplitPoint, 60.0f });
        for (int i = 0; i < 17; ++i)
            note (s.events, 0.2 + 0.5 * i, 0.3, i % 4 == 3 ? 43 : 36, 100);
        note (s.events, 0.2, 3.4, 72, 90); // held across the change to Dual: released by it
        param (s.events, 3.0, vs::ParamID::keyMode, 1.0f);
        note (s.events, 3.5, 1.2, 67, 90); // Dual: both sounds
        note (s.events, 4.7, 1.2, 69, 90);
        param (s.events, 6.0, vs::ParamID::keyMode, 0.0f);
        note (s.events, 6.2, 1.5, 72, 90); // Whole: the bass sound only
        scenes.push_back (s);
    }
    return scenes;
}
} // namespace

TEST_CASE ("Render M3 bi-timbral demo WAV", "[.render]")
{
    juce::ScopedJuceInitialiser_GUI init;
    std::vector<float> left, right;
    juce::String cues;
    for (const auto& s : m3Scenes())
    {
        cues << juce::String (static_cast<double> (left.size()) / kFs, 1) << " s  " << s.name << "\n";
        renderScene (s, left, right);
    }

    float peak = 0.0f;
    for (size_t i = 0; i < left.size(); ++i)
        peak = std::max ({ peak, std::abs (left[i]), std::abs (right[i]) });
    WARN ("rendered " << static_cast<double> (left.size()) / kFs << " s, peak " << juce::Decibels::gainToDecibels (peak) << " dBFS\n" << cues);
    REQUIRE (peak < 1.0f);
    REQUIRE (peak > 0.05f);

    const auto envDir = juce::SystemStats::getEnvironmentVariable ("VS_RENDER_DIR", {});
    auto dir = envDir.isNotEmpty() ? juce::File (envDir) : juce::File::getCurrentWorkingDirectory().getChildFile ("renders");
    dir.createDirectory();
    auto file = dir.getChildFile ("m3_demo.wav");
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (kFs).withNumChannels (2).withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, options);
    REQUIRE (writer != nullptr);
    const float* ch[] { left.data(), right.data() };
    writer->writeFromFloatArrays (ch, 2, static_cast<int> (left.size()));
    WARN ("wrote " << file.getFullPathName());
    dir.getChildFile ("m3_demo_scenes.txt").replaceWithText (cues);
}
