#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.h"
#include "engine/EventParser.h"
#include "engine/Part.h"
#include "params/Mappings.h"

#include <functional>

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 256;    // default host block size of the tests
constexpr int kMaxBlock = 512; // prepared maximum

struct Rig
{
    vs::Part part;
    vs::PartParams params;

    explicit Rig (int osFactor = 2)
    {
        part.setParams (params);
        part.prepare (kFs, kMaxBlock, osFactor, 1234u);
    }

    /** Called before each block with the block index; may change the parameters. */
    using BlockHook = std::function<void (int, vs::PartParams&)>;

    /**
        Renders numSamples in host blocks of blockSize (the last one shorter), applying the given
        events (offsets relative to the first sample, ascending). Every render in these tests goes
        through here, so the output range is checked in one place: a block that ran past the end
        of the output corrupted the heap on Windows during M2a.
    */
    std::vector<float> render (int numSamples, const std::vector<vs::NoteEvent>& events = {},
                               int blockSize = kBlock, const BlockHook& onBlock = {})
    {
        REQUIRE (blockSize >= 1);
        REQUIRE (blockSize <= kMaxBlock);
        std::vector<float> out (static_cast<size_t> (numSamples));
        std::vector<vs::NoteEvent> chunk;
        for (int start = 0, b = 0; start < numSamples; start += blockSize, ++b)
        {
            const int len = std::min (blockSize, numSamples - start);
            REQUIRE (static_cast<size_t> (start + len) <= out.size());
            chunk.clear();
            for (auto e : events)
                if (e.offset >= start && e.offset < start + len)
                {
                    e.offset -= start;
                    chunk.push_back (e);
                }
            if (onBlock)
                onBlock (b, params);
            part.setParams (params);
            part.render (out.data() + start, len, chunk);
        }
        return out;
    }
};

std::vector<vs::NoteEvent> notes (std::initializer_list<std::tuple<int, int, bool>> events)
{
    std::vector<vs::NoteEvent> v;
    for (const auto& [pos, note, on] : events)
        v.push_back ({ pos, on ? vs::EventType::NoteOn : vs::EventType::NoteOff, static_cast<std::uint8_t> (note), on ? 0.8f : 0.0f });
    return v;
}

/** Parses a MidiBuffer the way the processor does (raw bytes, one chunk). */
std::vector<vs::NoteEvent> parse (const juce::MidiBuffer& midi)
{
    vs::EventParser parser;
    parser.prepare();
    for (const auto meta : midi)
        parser.add (meta.data, meta.numBytes, meta.samplePosition);
    parser.finish();
    const auto ev = parser.events();
    return { ev.begin(), ev.end() };
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
    const auto bent = rig.render (48000, parse (m));
    const double hz = vs::test::measureFrequency (std::vector<float> (bent.begin() + 12000, bent.end()), kFs);
    CHECK (std::abs (hz - 440.0 * std::pow (2.0, (2.0 * 8191.0 / 8192.0) / 12.0)) < 0.5);

    juce::MidiBuffer off;
    off.addEvent (juce::MidiMessage::noteOn (1, 69, static_cast<juce::uint8> (0)), 0); // velocity 0 = note off
    rig.render (4800, parse (off));
    CHECK_FALSE (rig.part.isSounding());

    juce::MidiBuffer panic;
    panic.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
    panic.addEvent (juce::MidiMessage::allNotesOff (1), 100);
    rig.render (9600, parse (panic));
    CHECK_FALSE (rig.part.isSounding());
}

TEST_CASE ("Static pitch error <= 0.1 cent across the keyboard", "[part][pitch]")
{
    for (const int osFactor : { 2, 4 })
        for (const int note : { 33, 57, 69, 93, 105 })
        {
            Rig rig (osFactor);
            rig.params.vco1Wave = 0; // sine
            rig.params.sourceMix = 0.0f;
            rig.params.vcfCutoff = 1.0f;
            rig.params.vcfEnvMod = 0.0f;
            rig.params.vcfResonance = 0.0f;
            rig.params.env2Sustain = 1.0f;
            const auto out = rig.render (48000 * 2, notes ({ { 0, note, true } }));
            const double hz = vs::test::measureFrequency (std::vector<float> (out.begin() + 24000, out.end()), kFs);
            const double expected = 440.0 * std::pow (2.0, (note - 69) / 12.0);
            const double cents = 1200.0 * std::log2 (hz / expected);
            INFO ("os " << osFactor << " note " << note << ": " << hz << " Hz, error " << cents << " cent");
            CHECK (std::abs (cents) <= 0.1);
        }
}

namespace
{
/**
    A patch with every modulation path moving fast: cutoff target jumping 0.25 <-> 0.85 every
    256 samples, pitch bend jumping between -1 and +1 every 384 samples, fast LFO on pitch, cutoff
    and PWM, 1 ms ENV-1 attack on pitch, resonance, cross-mod, sync and full Vintage (drift).
*/
std::vector<float> renderFastModulation (int maxSubBlock, int blockSize = kBlock, int numSamples = 24000)
{
    Rig rig;
    rig.part.setMaxSubBlock (maxSubBlock);
    rig.part.setVintage (1.0f);
    auto& p = rig.params;
    p.vcfResonance = 0.7f;
    p.vcoSync = 1;
    p.vco1CrossMod = 0.5f;
    p.vco1Wave = 4; // pulse
    p.pwmDepth = 0.8f;
    p.vcoModEnv = 0.3f;
    p.env1Attack = 0.0f;
    p.env1Decay = 0.3f;
    p.vcoModLfo = 0.15f;
    p.lfoRate = 0.9f;
    p.vcfLfoMod = 0.3f;
    p.hpfCutoff = 0.4f;
    p.env2Sustain = 1.0f;

    std::vector<vs::NoteEvent> events { { 0, vs::EventType::NoteOn, 45, 0.8f } };
    for (int pos = 384, sign = 1; pos < numSamples; pos += 384, sign = -sign)
        events.push_back ({ pos, vs::EventType::PitchBend, 0, static_cast<float> (sign) });

    // With 256-sample blocks the cutoff target jumps every block (the original test). Other
    // block sizes are compared against each other, so they just need the same rule.
    return rig.render (numSamples, events, blockSize, [] (int b, vs::PartParams& params) {
        params.vcfCutoff = b % 2 == 0 ? 0.25f : 0.85f;
    });
}

double differenceDb (const std::vector<float>& reference, const std::vector<float>& other)
{
    double signal = 0.0, diff = 0.0;
    for (size_t i = 0; i < reference.size(); ++i)
    {
        signal += static_cast<double> (reference[i]) * static_cast<double> (reference[i]);
        const double d = static_cast<double> (other[i]) - static_cast<double> (reference[i]);
        diff += d * d;
    }
    REQUIRE (signal > 1.0); // the patch is audible
    return 10.0 * std::log10 (std::max (diff, 1.0e-30) / signal);
}
} // namespace

TEST_CASE ("Sub-block rendering equals per-sample rendering within -90 dB (no control-rate steps)", "[part][control]")
{
    // Sub-block 1 runs every stage (smoothing, LFO, drift, coefficients, oscillators, filter)
    // one oversampled sample at a time, like the M1 engine. Any control-rate shortcut (stepped
    // or ramped parameters, zipper sidebands, drift retargets moved to a sub-block start) would
    // show up as a difference; only float rounding is allowed (measured -99.5 dB on arm64 with
    // Vintage 1, -105 dB with Vintage 0).
    const auto reference = renderFastModulation (1);
    const auto control = renderFastModulation (vs::kMaxSubBlock);
    const double ratioDb = differenceDb (reference, control);
    INFO ("difference relative to the signal: " << ratioDb << " dB");
    CHECK (ratioDb < -90.0);
}

TEST_CASE ("Render does not depend on the host block size (Vintage 1, odd event offsets)", "[part][control]")
{
    // The same 4 s in 512- vs 500-sample host blocks, with drift at full Vintage: bend events
    // every 384 samples (odd offsets) land at different positions inside the blocks, and all
    // sub-block boundaries differ. The patch is constant (parameter changes are per host block,
    // so they would legitimately differ). Measured -110 dB on arm64.
    constexpr int length = 4 * 48000;
    auto run = [] (int blockSize) {
        Rig rig;
        rig.part.setVintage (1.0f);
        auto& p = rig.params;
        p.vcfResonance = 0.7f;
        p.vcoSync = 1;
        p.vco1CrossMod = 0.5f;
        p.vcoModLfo = 0.15f;
        p.lfoRate = 0.9f;
        p.env2Sustain = 1.0f;
        std::vector<vs::NoteEvent> events { { 0, vs::EventType::NoteOn, 45, 0.8f } };
        for (int pos = 383, sign = 1; pos < length; pos += 384, sign = -sign)
            events.push_back ({ pos, vs::EventType::PitchBend, 0, 0.5f * static_cast<float> (sign) });
        events.push_back ({ length - 4801, vs::EventType::NoteOff, 45, 0.0f });
        return rig.render (length, events, blockSize);
    };
    const double ratioDb = differenceDb (run (512), run (500));
    INFO ("difference relative to the signal: " << ratioDb << " dB");
    CHECK (ratioDb < -100.0);
}

TEST_CASE ("AllNotesOff releases every key and clears the pedal; ResetControllers resets bend, wheel and pedal", "[part][midi]")
{
    using vs::EventType;
    Rig rig;
    rig.params.vco1Wave = 0;
    rig.params.sourceMix = 0.0f;
    rig.params.vcfCutoff = 1.0f;
    rig.params.vcfEnvMod = 0.0f;
    rig.params.env2Sustain = 1.0f;

    // Pedal down and wheel up are stored (used from M2b), AllNotesOff clears the pedal.
    rig.render (4800, { { 0, EventType::NoteOn, 69, 0.8f }, { 10, EventType::SustainPedal, 0, 1.0f },
                        { 20, EventType::ModWheel, 0, 0.7f } });
    CHECK (rig.part.isSustainDown());
    CHECK (std::abs (rig.part.getModWheel() - 0.7f) < 1.0e-7f);
    rig.render (9600, { { 0, EventType::AllNotesOff, 0, 0.0f } });
    CHECK_FALSE (rig.part.isSustainDown());
    CHECK_FALSE (rig.part.isSounding());
    CHECK (std::abs (rig.part.getModWheel() - 0.7f) < 1.0e-7f); // not a controller reset

    // ResetControllers: bend back to centre (pitch returns to 440 Hz), wheel 0, pedal up.
    rig.render (4800, { { 0, EventType::NoteOn, 69, 0.8f }, { 0, EventType::SustainPedal, 0, 1.0f },
                        { 0, EventType::PitchBend, 0, 1.0f } });
    const auto out = rig.render (48000, { { 0, EventType::ResetControllers, 0, 0.0f } });
    CHECK_FALSE (rig.part.isSustainDown());
    CHECK (std::abs (rig.part.getModWheel()) < 1.0e-7f);
    const double hz = vs::test::measureFrequency (std::vector<float> (out.begin() + 12000, out.end()), kFs);
    CHECK (std::abs (hz - 440.0) < 0.2);
}

TEST_CASE ("Voice::renderBlock renders any number of voices (the 33rd is not dropped)", "[part]")
{
    constexpr float fs = 96000.0f;
    vs::PartParams p;
    p.env2Sustain = 1.0f;
    vs::PartModulator modulator;
    modulator.prepare (fs, 1u);
    modulator.reset (p);

    std::vector<vs::Voice> voices (vs::Voice::kVoiceGroup + 1);
    std::vector<vs::Voice*> pointers;
    for (size_t v = 0; v < voices.size(); ++v)
    {
        voices[v].prepare (fs, static_cast<std::uint32_t> (v + 1));
        voices[v].setBlockParams (p, 0.0f);
        pointers.push_back (&voices[v]);
    }
    voices.back().noteOn (60, 0.8f); // only the voice beyond the first group plays

    vs::ModBlock block;
    float out[vs::kMaxSubBlock] {};
    double energy = 0.0;
    for (int b = 0; b < 200; ++b)
    {
        modulator.next (vs::kMaxSubBlock, block);
        vs::Voice::renderBlock (pointers.data(), static_cast<int> (pointers.size()), block, out);
        for (const float x : out)
            energy += static_cast<double> (x) * static_cast<double> (x);
    }
    CHECK (energy > 1.0);
}

