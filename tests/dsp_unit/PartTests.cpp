#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.h"
#include "engine/Part.h"
#include "params/Mappings.h"

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 256;

struct Rig
{
    vs::Part part;
    vs::PartParams params;

    explicit Rig (int osFactor = 2)
    {
        part.setParams (params);
        part.prepare (kFs, kBlock, osFactor, 1234u);
    }

    /** Renders numSamples, applying the given MIDI (positions relative to the first sample). */
    std::vector<float> render (int numSamples, const juce::MidiBuffer& midi = {})
    {
        std::vector<float> out (static_cast<size_t> (numSamples));
        for (int start = 0; start < numSamples; start += kBlock)
        {
            const int len = std::min (kBlock, numSamples - start);
            part.setParams (params);
            part.render (out.data() + start, len, midi, start);
        }
        return out;
    }
};

juce::MidiBuffer notes (std::initializer_list<std::tuple<int, int, bool>> events)
{
    juce::MidiBuffer m;
    for (const auto& [pos, note, on] : events)
        m.addEvent (on ? juce::MidiMessage::noteOn (1, note, 0.8f) : juce::MidiMessage::noteOff (1, note), pos);
    return m;
}

int firstNonSilent (const std::vector<float>& x, float threshold = 1.0e-4f)
{
    for (size_t i = 0; i < x.size(); ++i)
        if (std::abs (x[i]) > threshold)
            return static_cast<int> (i);
    return -1;
}
} // namespace

TEST_CASE ("Part is silent without notes", "[part]")
{
    Rig rig;
    const auto out = rig.render (48000);
    CHECK (vs::test::rms (out.data(), out.size()) < 1.0e-7);
}

TEST_CASE ("Note-on is sample-accurate", "[part]")
{
    for (const int pos : { 0, 100, 257, 1000 })
    {
        Rig rig;
        rig.params.env2Attack = 0.0f; // 1 ms
        const auto out = rig.render (4096, notes ({ { pos, 60, true } }));
        const int first = firstNonSilent (out);
        INFO ("note at " << pos << " first sound at " << first << " latency " << rig.part.getLatencySamples());
        CHECK (first >= pos);
        CHECK (first <= pos + rig.part.getLatencySamples() + 4);
    }
}

TEST_CASE ("Note plays at the right pitch", "[part]")
{
    Rig rig;
    rig.params.vco1Wave = 0; // sine
    rig.params.sourceMix = 0.0f;
    rig.params.vcfCutoff = 1.0f;
    rig.params.vcfEnvMod = 0.0f;
    rig.params.vcfResonance = 0.0f;
    rig.params.env2Sustain = 1.0f;
    const auto out = rig.render (48000 * 2, notes ({ { 0, 69, true } }));
    const std::vector<float> tail (out.begin() + 24000, out.end());
    CHECK (std::abs (vs::test::measureFrequency (tail, kFs) - 440.0) < 0.2);
}

TEST_CASE ("Release ends in silence and frees the voice", "[part]")
{
    Rig rig;
    rig.params.env2Release = vs::map::envTimeFromSec (0.2f);
    const auto out = rig.render (48000, notes ({ { 0, 60, true }, { 4800, 60, false } }));
    CHECK (vs::test::rms (out.data() + 1000, 2000) > 0.01);
    CHECK (vs::test::rms (out.data() + 40000, 8000) < 1.0e-5);
    CHECK_FALSE (rig.part.isSounding());
}

TEST_CASE ("Last-note priority returns to held notes (legato)", "[part]")
{
    Rig rig;
    rig.params.vco1Wave = 0;
    rig.params.sourceMix = 0.0f;
    rig.params.vcfCutoff = 1.0f;
    rig.params.vcfEnvMod = 0.0f;
    rig.params.env2Sustain = 1.0f;

    // Hold A4, play A5 on top, release A5 -> back to A4 without silence.
    const auto out = rig.render (48000 * 3, notes ({ { 0, 69, true }, { 48000, 81, true }, { 96000, 81, false } }));
    const std::vector<float> a5 (out.begin() + 60000, out.begin() + 90000);
    const std::vector<float> a4 (out.begin() + 110000, out.end());
    CHECK (std::abs (vs::test::measureFrequency (a5, kFs) - 880.0) < 0.5);
    CHECK (std::abs (vs::test::measureFrequency (a4, kFs) - 440.0) < 0.5);
    CHECK (vs::test::rms (out.data() + 96000, 4800) > 0.05);
}

TEST_CASE ("Every waveform/switch combination renders finite, bounded audio", "[part]")
{
    Rig rig;
    rig.params.vcfResonance = 1.0f;
    rig.params.vco1CrossMod = 1.0f;
    rig.params.vcoModLfo = 1.0f;
    rig.params.vcoModEnv = 1.0f;
    rig.params.pwmDepth = 1.0f;
    rig.params.vcfEnvMod = 1.0f;
    rig.params.vcfLfoMod = 1.0f;
    rig.params.vcaLevel = 1.0f;
    rig.params.lfoRate = 1.0f;

    for (int w1 = 0; w1 < 6; ++w1)
        for (int w2 = 0; w2 < 6; ++w2)
        {
            rig.params.vco1Wave = w1;
            rig.params.vco2Wave = w2;
            rig.params.vcoSync = (w1 + w2) % 2;
            rig.params.vcfSlope = w1 % 2;
            rig.params.vco2Mode = w2 % 2;
            rig.params.pwmSource = w1 % 3;
            rig.params.env1Polarity = w2 % 2;
            rig.params.vcaLfoMod = w1 % 4;
            const auto out = rig.render (2400, notes ({ { 0, 36 + w1 * 8 + w2, true } }));
            for (const float s : out)
            {
                REQUIRE (std::isfinite (s));
                REQUIRE (std::abs (s) < 4.0f);
            }
        }
}

TEST_CASE ("Rendering is deterministic", "[part]")
{
    auto run = [] {
        Rig rig;
        rig.params.vcoModLfo = 0.3f;
        rig.params.lfoWave = 4; // random
        return rig.render (9600, notes ({ { 10, 48, true }, { 5000, 48, false } }));
    };
    CHECK (run() == run());
}

TEST_CASE ("Vintage zero is exactly clean; vintage detunes", "[part][vintage]")
{
    auto pitchAt = [] (float vintage) {
        Rig rig;
        rig.params.vco1Wave = 0;
        rig.params.sourceMix = 0.0f;
        rig.params.vcfCutoff = 1.0f;
        rig.params.vcfEnvMod = 0.0f;
        rig.params.env2Sustain = 1.0f;
        rig.part.setVintage (vintage);
        const auto out = rig.render (48000, notes ({ { 0, 69, true } }));
        return vs::test::measureFrequency (std::vector<float> (out.begin() + 12000, out.end()), kFs);
    };
    CHECK (std::abs (pitchAt (0.0f) - 440.0) < 0.2);
    const double cents = 1200.0 * std::log2 (pitchAt (1.0f) / 440.0);
    CHECK (std::abs (cents) > 0.2);
    CHECK (std::abs (cents) < 10.0);
}

TEST_CASE ("MIDI parsing: pitch bend, note-on velocity 0, all-notes-off, SysEx ignored", "[part][midi]")
{
    Rig rig;
    rig.params.vco1Wave = 0;
    rig.params.sourceMix = 0.0f;
    rig.params.vcfCutoff = 1.0f;
    rig.params.vcfEnvMod = 0.0f;
    rig.params.env2Sustain = 1.0f;
    rig.params.env2Release = 0.0f;

    juce::MidiBuffer m;
    const juce::uint8 sysex[] { 0x7D, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
    m.addEvent (juce::MidiMessage::createSysExMessage (sysex, sizeof (sysex)), 0);
    m.addEvent (juce::MidiMessage::noteOn (1, 69, 0.8f), 0);
    m.addEvent (juce::MidiMessage::pitchWheel (1, 16383), 1); // +2 semitones
    const auto bent = rig.render (48000, m);
    const double hz = vs::test::measureFrequency (std::vector<float> (bent.begin() + 12000, bent.end()), kFs);
    CHECK (std::abs (hz - 440.0 * std::pow (2.0, (2.0 * 8191.0 / 8192.0) / 12.0)) < 0.5);

    juce::MidiBuffer off;
    off.addEvent (juce::MidiMessage::noteOn (1, 69, static_cast<juce::uint8> (0)), 0); // velocity 0 = note off
    rig.render (4800, off);
    CHECK_FALSE (rig.part.isSounding());

    juce::MidiBuffer panic;
    panic.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
    panic.addEvent (juce::MidiMessage::allNotesOff (1), 100);
    rig.render (9600, panic);
    CHECK_FALSE (rig.part.isSounding());
}
