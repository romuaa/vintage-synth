// Hidden: renders the M5 audition WAV (m5_demo, stereo, 48 kHz / 24 bit) through the real processor:
// effects on the bus and the arpeggiator/sequencer in front of the parts (docs/architecture.md 6a.3
// item 6). This is the product owner's M5 listening gate.
//   VS_RENDER_DIR=$TMPDIR/renders build/mac-arm64/tests/VintageSynthTests "Render m5_demo*"
// Output: $VS_RENDER_DIR, or the system temp directory if it is not set (never the repository):
// m5_demo.wav and m5_demo_scenes.txt (start time of every scene).
#include <catch2/catch_test_macros.hpp>

#include "vintage/PluginProcessor.h"
#include "common/params/Mappings.h"
#include "vintage/params/ParameterLayout.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <optional>
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
    juce::String id {};
    float value = 0.0f;
};

struct Scene
{
    juce::String name;
    std::vector<std::pair<juce::String, float>> setup; // parameter id, plain value
    std::vector<Ev> events;
    double length;
    std::optional<double> hostBpm {}; // host transport playing from beat 0 at this tempo
};

void note (std::vector<Ev>& ev, double t, double dur, int n, int vel = 100)
{
    ev.push_back ({ t, n, vel });
    ev.push_back ({ t + dur, n, 0 });
}

void setParam (vs::VintageSynthProcessor& proc, const juce::String& id, float plain)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
    REQUIRE (p != nullptr);
    p->setValueNotifyingHost (p->convertTo0to1 (plain));
}

struct DemoPlayHead final : public juce::AudioPlayHead
{
    std::optional<double> bpm;
    long long now = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (bpm.has_value());
        if (bpm.has_value())
        {
            info.setBpm (*bpm);
            info.setPpqPosition (static_cast<double> (now) * *bpm / (60.0 * kFs));
        }
        return info;
    }
};

/** Renders a scene; returns how many note-ons each part received (arpeggio/sequence check). */
std::pair<int, int> renderScene (const Scene& s, std::vector<float>& left, std::vector<float>& right)
{
    std::pair<int, int> noteOns { 0, 0 };
    vs::VintageSynthProcessor proc;
    DemoPlayHead playHead;
    playHead.bpm = s.hostBpm;
    proc.setPlayHead (&playHead);
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
        playHead.now = pos;
        proc.processBlock (buffer, midi);
        for (int p = 0; p < 2; ++p)
            for (const auto& e : proc.getLastPartEvents (p))
                (p == 0 ? noteOns.first : noteOns.second) += e.type == vs::EventType::NoteOn ? 1 : 0;
        left.insert (left.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + len);
        right.insert (right.end(), buffer.getReadPointer (1), buffer.getReadPointer (1) + len);
        pos = end;
    }
    left.insert (left.end(), static_cast<size_t> (kFs * 0.6), 0.0f); // gap between scenes
    right.insert (right.end(), static_cast<size_t> (kFs * 0.6), 0.0f);
    return noteOns;
}

// A slow, warm saw pad (Whole mode, centre).
const std::vector<std::pair<juce::String, float>> kPad {
    { "lower.vcf.cutoff", 0.45f }, { "lower.vcf.resonance", 0.3f }, { "lower.env2.attack", 0.4f },
    { "lower.env2.release", 0.6f }, { "lower.vco2.fine_tune", 0.12f }, { "global.vintage", 0.4f },
};

// A singing pulse lead (Whole mode, Solo).
const std::vector<std::pair<juce::String, float>> kLead {
    { "lower.assign.mode", 2.0f }, { "lower.vco1.waveform", 4.0f }, { "lower.vco2.waveform", 3.0f },
    { "lower.vco1.range", 3.0f }, { "lower.vco2.range", 2.0f }, { "lower.vcf.cutoff", 0.62f },
    { "lower.vcf.resonance", 0.25f }, { "lower.vcomod.lfo_mod", 0.1f }, { "lower.lfo.delay", 0.35f },
    { "lower.env2.release", 0.35f }, { "lower.vca.level", 0.7f }, { "global.vintage", 0.3f },
};

void padChords (std::vector<Ev>& ev, double start, double chordLength)
{
    const int chords[2][4] { { 48, 55, 60, 64 }, { 45, 52, 57, 60 } };
    for (int c = 0; c < 2; ++c)
        for (const int n : chords[c])
            note (ev, start + chordLength * c, chordLength - 0.1, n, 80);
}

std::vector<Scene> m5Scenes()
{
    std::vector<Scene> scenes;

    // (a) Chorus I and II, default and full mix, after the dry pad for reference.
    scenes.push_back ({ "(a) Pad, dry (reference)", kPad, {}, 4.4 });
    padChords (scenes.back().events, 0.1, 1.8);
    for (const int mode : { 1, 2 })
        for (const float mix : { 0.5f, 1.0f })
        {
            Scene s { juce::String ("(a) Pad, chorus ") + (mode == 1 ? "I" : "II") + (mix < 0.75f ? ", default mix (0.5)" : ", full mix (1.0)"),
                      kPad, {}, 4.4 };
            s.setup.push_back ({ vs::ParamID::fxChorusMode, static_cast<float> (mode) });
            s.setup.push_back ({ vs::ParamID::fxChorusMix, mix });
            padChords (s.events, 0.1, 1.8);
            scenes.push_back (s);
        }

    { // (b) Lead with the 700 ms ping-pong delay at level 0.4 (the delay defaults otherwise).
        Scene s { "(b) Lead, ping-pong delay 700 ms, level 0.4, feedback 35 %", kLead, {}, 8.0 };
        s.setup.push_back ({ vs::ParamID::fxDelayLevel, 0.4f });
        const int phrase[] { 67, 70, 72, 74, 72 };
        const double at[] { 0.1, 0.5, 0.9, 1.6, 2.8 };
        const double dur[] { 0.3, 0.3, 0.6, 1.0, 0.8 };
        for (int i = 0; i < 5; ++i)
            note (s.events, at[i], dur[i], phrase[i], 100);
        scenes.push_back (s);
    }

    // (c) Hall reverb on the pad at short, medium and long decay.
    for (const float decay : { 1.0f, 2.5f, 8.0f })
    {
        Scene s { "(c) Pad, hall reverb level 0.35, decay " + juce::String (decay, 1) + " s", kPad, {}, 1.8 + 1.2 * static_cast<double> (decay) };
        s.setup.push_back ({ vs::ParamID::fxReverbLevel, 0.35f });
        s.setup.push_back ({ vs::ParamID::fxReverbDecay, vs::map::reverbDecayFromSec (decay) });
        for (const int n : { 48, 55, 60, 64 })
            note (s.events, 0.1, 1.4, n, 80);
        scenes.push_back (s);
    }

    { // (d) All three effects on a Dual layer.
        Scene s { "(d) Dual layer (pad left + lead right), chorus I + delay 0.3 + reverb 0.3", kPad, {}, 9.0 };
        for (const auto& [id, v] : std::vector<std::pair<juce::String, float>> {
                 { "key.mode", 1.0f }, { "lower.mix.pan", -0.4f }, { "upper.mix.pan", 0.4f }, { "upper.vco1.waveform", 3.0f },
                 { "upper.vco2.waveform", 4.0f }, { "upper.vco1.range", 3.0f }, { "upper.vco2.range", 2.0f },
                 { "upper.vcf.cutoff", 0.64f }, { "upper.vcomod.lfo_mod", 0.1f }, { "upper.lfo.delay", 0.35f },
                 { "upper.env2.release", 0.4f }, { "upper.vca.level", 0.55f }, { "fx.chorus.mode", 1.0f },
                 { "fx.delay.level", 0.3f }, { "fx.delay.time", vs::map::delayTimeFromSec (0.45f) }, { "fx.reverb.level", 0.3f } })
            s.setup.push_back ({ id, v });
        const int chords[3][3] { { 48, 55, 64 }, { 45, 52, 60 }, { 43, 50, 59 } };
        for (int c = 0; c < 3; ++c)
            for (const int n : chords[c])
                note (s.events, 0.1 + 2.2 * c, 2.0, n, 85);
        scenes.push_back (s);
    }

    { // (e) Arpeggiated chord on Upper over a Split bass, host transport at 118 bpm, delay + reverb.
        Scene s { "(e) Split: bass (Lower) + arpeggio on Upper (Up, 2 octaves, 1/16, swing 57 %), host at 118 bpm, delay 3/16 + reverb",
                  {}, {}, 10.0, 118.0 };
        for (const auto& [id, v] : std::vector<std::pair<juce::String, float>> {
                 { "key.mode", 2.0f }, { "key.split_point", 60.0f }, { "lower.assign.mode", 2.0f }, { "lower.vco1.range", 1.0f },
                 { "lower.vcf.cutoff", 0.32f }, { "lower.vcf.resonance", 0.4f }, { "lower.env2.sustain", 0.7f },
                 { "lower.env2.release", 0.3f }, { "lower.mix.pan", -0.2f }, { "upper.vco1.waveform", 3.0f },
                 { "upper.vcf.cutoff", 0.6f }, { "upper.vcf.resonance", 0.35f }, { "upper.env2.decay", 0.4f },
                 { "upper.env2.sustain", 0.4f }, { "upper.env2.release", 0.3f }, { "upper.mix.pan", 0.25f },
                 { "upper.vca.level", 0.65f }, { "arp.mode", 1.0f }, { "arp.target", 1.0f }, { "arp.rate", 3.0f },
                 { "arp.octaves", 2.0f }, { "arp.swing", 57.0f }, { "arp.gate", 55.0f }, { "fx.delay.level", 0.3f },
                 { "fx.delay.time", vs::map::delayTimeFromSec (static_cast<float> (60.0 / 118.0 * 0.75)) },
                 { "fx.delay.feedback", 0.4f }, { "fx.reverb.level", 0.25f }, { "global.vintage", 0.3f } })
            s.setup.push_back ({ id, v });
        const double bar = 4.0 * 60.0 / 118.0;
        const int bass[4] { 36, 33, 29, 31 };
        const int chords[4][3] { { 64, 67, 72 }, { 64, 69, 72 }, { 65, 69, 72 }, { 62, 67, 71 } };
        for (int b = 0; b < 4; ++b)
        {
            note (s.events, 0.02 + bar * b, bar - 0.1, bass[b], 100);
            for (const int n : chords[b])
                note (s.events, 0.01 + bar * b, bar - 0.05, n, 90);
        }
        scenes.push_back (s);
    }

    { // (f) Step sequencer through a Solo patch with portamento: a tie and a rest; re-transposed.
        Scene s { "(f) Step sequencer, 8 steps with a tie (step 3) and a rest (step 4), Solo + portamento; key C3, then F3 at 4.1 s",
                  {}, {}, 9.5 };
        for (const auto& [id, v] : std::vector<std::pair<juce::String, float>> {
                 { "lower.assign.mode", 2.0f }, { "lower.porta.time", vs::map::portaFromSec (0.06f) },
                 { "lower.vco1.waveform", 2.0f }, { "lower.vco2.waveform", 3.0f }, { "lower.vco2.range", 0.0f },
                 { "lower.vcf.cutoff", 0.38f }, { "lower.vcf.resonance", 0.55f }, { "lower.env1.decay", 0.4f },
                 { "lower.env1.sustain", 0.15f }, { "lower.env2.sustain", 0.8f }, { "lower.env2.release", 0.25f },
                 { "arp.mode", 2.0f }, { "arp.target", 0.0f }, { "arp.rate", 3.0f }, { "arp.gate", 70.0f },
                 { "arp.tempo", 112.0f }, { "seq.length", 8.0f }, { "fx.reverb.level", 0.15f }, { "global.vintage", 0.4f } })
            s.setup.push_back ({ id, v });
        // Step: semitones, type (0 Note, 1 Rest, 2 Tie), velocity.
        const int st[8][2] { { 0, 0 }, { 3, 0 }, { 3, 2 }, { 0, 1 }, { 10, 0 }, { 7, 0 }, { 12, 0 }, { 5, 0 } };
        const float vel[8] { 1.0f, 0.7f, 0.7f, 0.5f, 0.9f, 0.6f, 1.0f, 0.7f };
        for (int i = 0; i < 8; ++i)
        {
            s.setup.push_back ({ vs::seqStepParamID (i + 1, vs::SeqStepField::Note), static_cast<float> (st[i][0]) });
            s.setup.push_back ({ vs::seqStepParamID (i + 1, vs::SeqStepField::Type), static_cast<float> (st[i][1]) });
            s.setup.push_back ({ vs::seqStepParamID (i + 1, vs::SeqStepField::Velocity), vel[i] });
        }
        note (s.events, 0.05, 4.1, 48, 100); // C3: the sequence is transposed down an octave from C4
        note (s.events, 4.1, 3.0, 53, 100);  // F3, pressed as C3 is released: re-transposed, restarted
        scenes.push_back (s);
    }
    return scenes;
}
} // namespace

TEST_CASE ("Render m5_demo: the M5 audition, effects and arpeggiator/sequencer in the plugin", "[.render]")
{
    juce::ScopedJuceInitialiser_GUI init;
    std::vector<float> left, right;
    juce::String cues;
    cues << "m5_demo.wav: 48 kHz, 24 bit, stereo. Scene start times:\n";
    for (const auto& s : m5Scenes())
    {
        const double t = static_cast<double> (left.size()) / kFs;
        cues << juce::String (static_cast<int> (t) / 60) << ":" << juce::String (std::fmod (t, 60.0), 1).paddedLeft ('0', 4) << "  " << s.name << "\n";
        const auto [lowerOns, upperOns] = renderScene (s, left, right);
        if (s.name.startsWith ("(e)"))
            CHECK (upperOns >= 60); // 4 bars of 1/16 at 118 bpm, minus the waits at the chord changes
        if (s.name.startsWith ("(f)"))
            CHECK (lowerOns >= 36); // 7 s of 1/16 at 112 bpm = 52 steps, 6 of every 8 start a note (tie, rest)
        WARN (s.name << ": note-ons Lower " << lowerOns << ", Upper " << upperOns);
    }

    float peak = 0.0f;
    for (size_t i = 0; i < left.size(); ++i)
        peak = std::max ({ peak, std::abs (left[i]), std::abs (right[i]) });
    WARN ("rendered " << static_cast<double> (left.size()) / kFs << " s, peak " << juce::Decibels::gainToDecibels (peak) << " dBFS\n" << cues);
    REQUIRE (peak < 1.0f);
    REQUIRE (peak > 0.05f);

    const auto envDir = juce::SystemStats::getEnvironmentVariable ("VS_RENDER_DIR", {});
    auto dir = envDir.isNotEmpty() ? juce::File (envDir)
                                   : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("vs_renders");
    dir.createDirectory();
    auto file = dir.getChildFile ("m5_demo.wav");
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (kFs).withNumChannels (2).withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, options);
    REQUIRE (writer != nullptr);
    const float* ch[] { left.data(), right.data() };
    writer->writeFromFloatArrays (ch, 2, static_cast<int> (left.size()));
    WARN ("wrote " << file.getFullPathName());
    dir.getChildFile ("m5_demo_scenes.txt").replaceWithText (cues);
}
