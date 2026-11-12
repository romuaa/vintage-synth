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

    /** Makes the current params the starting state (no smoothing from the defaults). */
    void apply()
    {
        part.setParams (params);
        part.reset();
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
    // M2: Poly is now the default; this M1 behaviour is the Solo mode.
    Rig rig;
    rig.params.assignMode = 2; // Solo
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
    // One voice per combination (M1 semantics): in Poly (the M2 default) the 36 un-released
    // notes would stack up, so the test runs in Solo.
    Rig rig;
    rig.params.assignMode = 2; // Solo
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
    // Vintage 1, -105 dB with Vintage 0 in M2a; -95.8 dB with Vintage 1 since M2b computes the
    // drift walk from the time since its last retarget, which rounds the walk to float once per
    // sub-block instead of along a chain, and made real host block sizes agree much better).
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
    // so they would legitimately differ). Measured -110 dB on arm64 (M2a), -116.8 dB (M2b).
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


// =============================================================================================
// M2b: polyphony (docs/architecture.md 3.2 / 3.6 item 2)
// =============================================================================================
namespace
{
using vs::EventType;

vs::NoteEvent ev (int pos, EventType t, int note = 0, float value = 0.0f)
{
    return { pos, t, static_cast<std::uint8_t> (note), value };
}
vs::NoteEvent on (int pos, int note, float velocity = 0.8f) { return ev (pos, EventType::NoteOn, note, velocity); }
vs::NoteEvent off (int pos, int note) { return ev (pos, EventType::NoteOff, note); }

/** One pure sine per voice (VCO-1 sine, open filter, full sustain, fast attack, no velocity). */
void sinePatch (vs::PartParams& p)
{
    p.vco1Wave = 0;
    p.sourceMix = 0.0f;
    p.vcfCutoff = 1.0f;
    p.vcfEnvMod = 0.0f;
    p.vcfResonance = 0.0f;
    p.vcfKeyFollow = 0.0f;
    p.env2Attack = 0.0f;
    p.env2Sustain = 1.0f;
    p.velToVca = 0.0f;
    p.vcaLevel = 0.5f;
}

double maxStep (const std::vector<float>& x, int from, int to)
{
    double m = 0.0;
    for (int i = std::max (1, from); i < to; ++i)
        m = std::max (m, static_cast<double> (std::abs (x[static_cast<size_t> (i)] - x[static_cast<size_t> (i - 1)])));
    return m;
}

struct PitchPoint
{
    double t;     // samples
    double semis; // MIDI note units
};

/** Per-period pitch from rising zero crossings (linear interpolation) in [from, to). */
std::vector<PitchPoint> pitchTrack (const std::vector<float>& x, int from, int to)
{
    std::vector<PitchPoint> track;
    double prev = -1.0;
    for (int i = std::max (1, from); i < to; ++i)
    {
        const float a = x[static_cast<size_t> (i - 1)], b = x[static_cast<size_t> (i)];
        if (a < 0.0f && b >= 0.0f)
        {
            const double t = static_cast<double> (i - 1) + static_cast<double> (-a / (b - a));
            if (prev >= 0.0)
                track.push_back ({ 0.5 * (prev + t), 69.0 + 12.0 * std::log2 (kFs / (t - prev) / 440.0) });
            prev = t;
        }
    }
    return track;
}

double pitchAt (const std::vector<PitchPoint>& track, double t)
{
    REQUIRE_FALSE (track.empty());
    const auto it = std::min_element (track.begin(), track.end(), [t] (const PitchPoint& a, const PitchPoint& b) {
        return std::abs (a.t - t) < std::abs (b.t - t);
    });
    return it->semis;
}

/** Hann-windowed magnitude spectrum of 2^order samples starting at `start`. */
std::vector<float> hannSpectrum (const std::vector<float>& x, size_t start, int order)
{
    const size_t n = static_cast<size_t> (1) << order;
    REQUIRE (start + n <= x.size());
    std::vector<float> seg (x.begin() + static_cast<std::ptrdiff_t> (start), x.begin() + static_cast<std::ptrdiff_t> (start + n));
    for (size_t i = 0; i < n; ++i)
        seg[i] *= static_cast<float> (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * static_cast<double> (i) / static_cast<double> (n)));
    return vs::test::magnitudeSpectrum (seg, order);
}

struct Peak
{
    double hz;
    double db;
};

/** Largest bin within +-maxDevHz of expectedHz, refined by parabolic interpolation (log magnitude). */
Peak findPeak (const std::vector<float>& spec, int order, double expectedHz, double maxDevHz)
{
    const double binHz = kFs / static_cast<double> (1 << order);
    const int lo = std::max (1, static_cast<int> ((expectedHz - maxDevHz) / binHz));
    const int hi = std::min (static_cast<int> (spec.size()) - 2, static_cast<int> ((expectedHz + maxDevHz) / binHz) + 1);
    int best = lo;
    for (int b = lo; b <= hi; ++b)
        if (spec[static_cast<size_t> (b)] > spec[static_cast<size_t> (best)])
            best = b;
    const double l = vs::test::toDb (spec[static_cast<size_t> (best - 1)]), c = vs::test::toDb (spec[static_cast<size_t> (best)]),
                 r = vs::test::toDb (spec[static_cast<size_t> (best + 1)]);
    const double denom = l - 2.0 * c + r;
    const double offset = std::abs (denom) > 1.0e-12 ? 0.5 * (l - r) / denom : 0.0;
    return { (static_cast<double> (best) + offset) * binHz, c - 0.25 * (l - r) * offset };
}

double noteHz (double note) { return 440.0 * std::pow (2.0, (note - 69.0) / 12.0); }
} // namespace

TEST_CASE ("Poly: a 16-note chord renders 16 distinct pitches at equal level", "[part][poly]")
{
    Rig rig;
    sinePatch (rig.params);
    rig.apply();

    std::vector<vs::NoteEvent> events;
    for (int i = 0; i < vs::kMaxVoices; ++i)
        events.push_back (on (0, 45 + 3 * i)); // 110 Hz .. 1480 Hz, 3 semitones apart
    constexpr int order = 16;
    const auto out = rig.render (24000 + (1 << order), events);
    CHECK (rig.part.numSoundingVoices() == vs::kMaxVoices);

    const auto spec = hannSpectrum (out, 24000, order);
    double lo = 1.0e9, hi = -1.0e9;
    for (int i = 0; i < vs::kMaxVoices; ++i)
    {
        const double expected = noteHz (45 + 3 * i);
        const auto p = findPeak (spec, order, expected, 3.0);
        INFO ("note " << 45 + 3 * i << ": " << p.hz << " Hz (expected " << expected << "), " << p.db << " dB");
        CHECK (std::abs (p.hz - expected) < 0.1);
        lo = std::min (lo, p.db);
        hi = std::max (hi, p.db);
    }
    INFO ("level spread across the 16 notes: " << hi - lo << " dB");
    CHECK (hi - lo < 0.5); // a stolen, faded or doubled voice would stand out

    // Nothing else: every other local maximum is a played note or one of its harmonics (the
    // ladder's tanh adds odd harmonics), or at least 40 dB below the notes.
    const double binHz = kFs / static_cast<double> (1 << order);
    for (size_t b = 2; b < static_cast<size_t> (2000.0 / binHz); ++b)
    {
        const double db = vs::test::toDb (spec[b]);
        if (spec[b] > spec[b - 1] && spec[b] >= spec[b + 1] && db > lo - 40.0)
        {
            bool isNote = false;
            for (int i = 0; i < vs::kMaxVoices; ++i)
                for (int h = 1; h <= 5; ++h)
                    isNote = isNote || std::abs (static_cast<double> (b) * binHz - h * noteHz (45 + 3 * i)) < 3.0;
            INFO ("unexpected peak at " << static_cast<double> (b) * binHz << " Hz, " << db << " dB");
            CHECK (isNote);
        }
    }
}

TEST_CASE ("Poly: the 17th note steals the oldest voice with a 3 ms fade (no click) and starts 3 ms late", "[part][poly][steal]")
{
    // 16 low sines (32.7 .. 77.8 Hz) make a smooth signal whose largest sample-to-sample step is
    // small; cutting one voice off would step by up to its whole amplitude (several times more).
    static constexpr int stealAt = 24000;
    auto run = [] (bool steal, std::vector<float>* out) {
        Rig rig;
        sinePatch (rig.params);
        rig.apply();
        std::vector<vs::NoteEvent> events;
        for (int i = 0; i < vs::kMaxVoices; ++i)
            events.push_back (on (0, 24 + i)); // note 24 (voice 0) is the oldest
        if (steal)
            events.push_back (on (stealAt, 84)); // 1047 Hz: easy to see when it starts
        *out = rig.render (stealAt + 12000, events);
        return rig.part.getLatencySamples();
    };
    std::vector<float> a, b;
    const int latency = run (true, &a);
    run (false, &b);
    const int fadeHost = 144; // 3 ms at 48 kHz

    bool identicalBefore = true;
    for (size_t i = 0; i < static_cast<size_t> (stealAt); ++i)
        identicalBefore = identicalBefore && std::abs (a[i] - b[i]) <= 0.0f;
    CHECK (identicalBefore);

    // Click bound: during the fade the step stays within the chord's own steady-state steps.
    const double steady = maxStep (a, 12000, stealAt);
    const double fade = maxStep (a, stealAt, stealAt + fadeHost + latency - 2);
    // What a hard cut would do: the stolen voice's amplitude (note 24 alone).
    std::vector<float> lone;
    {
        Rig rig;
        sinePatch (rig.params);
        rig.apply();
        lone = rig.render (stealAt, { on (0, 24) });
    }
    float voicePeak = 0.0f;
    for (size_t i = 12000; i < static_cast<size_t> (stealAt); ++i)
        voicePeak = std::max (voicePeak, std::abs (lone[i]));
    INFO ("steady max step " << steady << ", max step during the fade " << fade << ", stolen voice peak " << voicePeak);
    WARN ("steal fade: steady max step " << steady << ", during the fade " << fade << " (ratio " << fade / steady
                                         << "), a hard cut could step by up to " << voicePeak);
    CHECK (fade <= 1.1 * steady);
    CHECK (voicePeak > 4.0 * steady); // the bound would catch a hard cut

    // The new note starts at the end of the fade: the difference a - b gains a 1 kHz component there.
    std::vector<float> d (a.size());
    for (size_t i = 0; i < a.size(); ++i)
        d[i] = a[i] - b[i];
    const double threshold = 0.25 * steady;
    int onset = -1;
    for (int i = stealAt + 1; i < static_cast<int> (d.size()) && onset < 0; ++i)
        if (std::abs (d[static_cast<size_t> (i)] - d[static_cast<size_t> (i - 1)]) > threshold)
            onset = i;
    INFO ("new note audible at +" << onset - stealAt << " samples (fade " << fadeHost << ", latency " << latency << ")");
    CHECK (onset >= stealAt + fadeHost);
    CHECK (onset <= stealAt + fadeHost + latency + 12);
}

TEST_CASE ("Poly: the stolen voice is reset and replays the new note; no voice is added", "[part][poly][steal]")
{
    Rig rig;
    sinePatch (rig.params);
    rig.apply();
    std::vector<vs::NoteEvent> events;
    for (int i = 0; i < vs::kMaxVoices; ++i)
        events.push_back (on (0, 40 + i));
    events.push_back (on (4800, 90));
    rig.render (4800 + 100, events); // inside the fade
    CHECK (rig.part.getVoice (0).getNote() == 40);
    CHECK (rig.part.getVoice (0).getFadeRemaining() > 0);
    rig.render (100); // fade (144 host samples) over
    CHECK (rig.part.getVoice (0).getNote() == 90);
    CHECK (rig.part.getVoice (0).getFadeRemaining() == 0);
    CHECK (rig.part.numSoundingVoices() == vs::kMaxVoices);
}

TEST_CASE ("Sustain pedal holds released notes in Poly, Solo and Unison", "[part][pedal]")
{
    SECTION ("Poly: released notes sound until pedal up")
    {
        Rig rig;
        sinePatch (rig.params);
        rig.params.env2Release = vs::map::envTimeFromSec (0.05f);
        rig.apply();
        const auto out = rig.render (36000, { on (0, 69), on (0, 76), ev (100, EventType::SustainPedal, 0, 1.0f),
                                              off (4800, 69), off (4800, 76), ev (24000, EventType::SustainPedal, 0, 0.0f) });
        CHECK (vs::test::rms (out.data() + 19000, 4800) > 0.1);    // 0.3 s after the key-ups
        CHECK (vs::test::rms (out.data() + 30000, 6000) < 1.0e-5); // released by the pedal
        CHECK_FALSE (rig.part.isSounding());
    }

    for (const int mode : { 1, 2 })
    {
        DYNAMIC_SECTION ((mode == 1 ? "Unison" : "Solo") << ": the pedal holds the sounding note; pedal up returns to the held key")
        {
            Rig rig;
            sinePatch (rig.params);
            rig.params.assignMode = mode;
            rig.params.unisonVoices = 0; // 2 voices
            rig.params.unisonDetune = 0.0f;
            rig.apply();
            // Hold A4, play A5, pedal down, release A5: A5 stays. Pedal up: back to A4 (legato).
            const auto out = rig.render (48000 * 2, { on (0, 69), on (12000, 81), ev (13000, EventType::SustainPedal, 0, 1.0f),
                                                      off (24000, 81), ev (48000, EventType::SustainPedal, 0, 0.0f) });
            const std::vector<float> held (out.begin() + 30000, out.begin() + 46000);
            const std::vector<float> back (out.begin() + 60000, out.end());
            CHECK (std::abs (vs::test::measureFrequency (held, kFs) - 880.0) < 0.5);
            CHECK (std::abs (vs::test::measureFrequency (back, kFs) - 440.0) < 0.5);
            CHECK (vs::test::rms (out.data() + 48000, 4800) > 0.1); // no gap at pedal up
        }
    }
}

TEST_CASE ("Portamento: RC glide covers 99 % of the interval in the portamento time", "[part][porta]")
{
    static constexpr double glideSec = 0.4;
    static constexpr int noteAt = 24000;

    auto glideCurve = [] (int mode, int portaMode, bool releaseFirst) {
        Rig rig;
        sinePatch (rig.params);
        rig.params.assignMode = mode;
        rig.params.portaMode = portaMode;
        rig.params.portaTime = vs::map::portaFromSec (static_cast<float> (glideSec));
        rig.apply();
        std::vector<vs::NoteEvent> events { on (0, 57) };
        if (releaseFirst)
            events.push_back (off (12000, 57));
        events.push_back (on (noteAt, 69));
        const auto out = rig.render (noteAt + static_cast<int> (1.5 * glideSec * kFs), events);
        return std::make_pair (pitchTrack (out, noteAt + 1000, static_cast<int> (out.size())), rig.part.getLatencySamples());
    };

    // Expected: 69 - 12 * 100^(-t / T) after the note-on (plus the output latency).
    auto check = [] (const std::vector<PitchPoint>& track, int latency) {
        double worst = 0.0;
        for (const double frac : { 0.25, 0.5, 0.75, 1.0 })
        {
            const double t = noteAt + latency + frac * glideSec * kFs;
            const double expected = 69.0 - 12.0 * std::pow (100.0, -frac);
            const double got = pitchAt (track, t);
            INFO ("t = " << frac << " T: pitch " << got << ", expected " << expected);
            CHECK (std::abs (got - expected) < 0.06);
            worst = std::max (worst, std::abs (got - expected));
        }
        // When does the pitch get within 1 % of the interval (0.12 semitone)?
        double reach = -1.0;
        for (const auto& p : track)
            if (reach < 0.0 && p.semis > 69.0 - 0.12)
                reach = (p.t - noteAt - latency) / kFs;
        INFO ("99 % reached after " << reach << " s (setting " << glideSec << " s), worst pitch error " << worst << " semitones");
        WARN ("portamento: 99 % reached after " << reach << " s for a " << glideSec << " s setting ("
                                                << 100.0 * (reach - glideSec) / glideSec << " %), worst pitch error " << worst << " st");
        CHECK (std::abs (reach - glideSec) < 0.02 * glideSec);
    };

    SECTION ("Solo, legato note")
    {
        const auto [track, latency] = glideCurve (2, 0, false);
        check (track, latency);
    }
    SECTION ("Poly glides a new voice from the previously played (already released) note")
    {
        const auto [track, latency] = glideCurve (0, 0, true);
        check (track, latency);
    }
    SECTION ("Legato mode: no glide when no key is held")
    {
        const auto [track, latency] = glideCurve (0, 1, true);
        CHECK (std::abs (pitchAt (track, noteAt + latency + 0.1 * glideSec * kFs) - 69.0) < 0.01);
    }
    SECTION ("Legato mode glides when a key is held")
    {
        const auto [track, latency] = glideCurve (2, 1, false);
        check (track, latency);
    }
    SECTION ("Portamento off: no glide")
    {
        Rig rig;
        sinePatch (rig.params);
        rig.params.assignMode = 2;
        rig.apply();
        const auto out = rig.render (noteAt + 12000, { on (0, 57), on (noteAt, 69) });
        const auto track = pitchTrack (out, noteAt + 200, static_cast<int> (out.size()));
        CHECK (std::abs (pitchAt (track, noteAt + 600) - 69.0) < 0.01);
    }
}

TEST_CASE ("Velocity scales the VCA gain: 1 - amount x (1 - velocity)", "[part][velocity]")
{
    auto level = [] (float amount, float velocity) {
        Rig rig;
        sinePatch (rig.params);
        rig.params.velToVca = amount;
        rig.apply();
        const auto out = rig.render (24000, { on (0, 69, velocity) });
        return vs::test::rms (out.data() + 12000, 12000);
    };
    const double full = level (0.0f, 1.0f);
    REQUIRE (full > 0.1);
    for (const auto& [amount, vel] : std::vector<std::pair<float, float>> { { 0.5f, 0.5f }, { 1.0f, 0.25f }, { 0.0f, 0.1f }, { 0.5f, 1.0f } })
    {
        const double expected = 1.0 - static_cast<double> (amount) * (1.0 - static_cast<double> (vel));
        const double ratio = level (amount, vel) / full;
        INFO ("amount " << amount << " velocity " << vel << ": ratio " << ratio << " expected " << expected);
        CHECK (std::abs (ratio - expected) < 0.002 * expected);
    }
}

TEST_CASE ("Velocity scales the VCF envelope depth (and nothing else)", "[part][velocity]")
{
    auto run = [] (float velocity, float velToVcf, float envMod) {
        Rig rig;
        auto& p = rig.params;
        p.vco1Wave = 2; // saw: the filter matters
        p.vcfCutoff = 0.3f;
        p.vcfResonance = 0.3f;
        p.env1Decay = 0.4f;
        p.env1Sustain = 0.2f;
        p.velToVca = 0.0f;
        p.velToVcf = velToVcf;
        p.vcfEnvMod = envMod;
        rig.apply();
        return rig.render (24000, { on (0, 45, velocity) });
    };
    // Half velocity at amount 1 halves the depth: same as full velocity with half the ENV mod.
    const auto a = run (0.5f, 1.0f, 0.6f);
    const auto b = run (1.0f, 0.0f, 0.3f);
    const auto c = run (1.0f, 0.0f, 0.6f);
    const double same = differenceDb (b, a);
    const double different = differenceDb (c, a);
    INFO ("vel 0.5 / amount 1 vs half depth: " << same << " dB; vs full depth: " << different << " dB");
    CHECK (same < -100.0);
    CHECK (different > -20.0);
}

TEST_CASE ("Pitch bend range is set per part (0..12 semitones)", "[part][bend]")
{
    auto pitch = [] (int range, float bend, int newRangeAtBlock = -1, int newRange = 0) {
        Rig rig;
        sinePatch (rig.params);
        rig.params.bendRange = range;
        rig.apply();
        const auto out = rig.render (48000, { on (0, 69), ev (0, EventType::PitchBend, 0, bend) }, kBlock,
                                     [=] (int b, vs::PartParams& p) {
                                         if (newRangeAtBlock >= 0 && b >= newRangeAtBlock)
                                             p.bendRange = newRange;
                                     });
        return vs::test::measureFrequency (std::vector<float> (out.begin() + 30000, out.end()), kFs);
    };
    CHECK (std::abs (pitch (2, 1.0f) - noteHz (71.0)) < 0.2);
    CHECK (std::abs (pitch (7, 1.0f) - noteHz (76.0)) < 0.3);
    CHECK (std::abs (pitch (12, -1.0f) - noteHz (57.0)) < 0.1);
    CHECK (std::abs (pitch (0, 1.0f) - 440.0) < 0.1);
    // Changing the range while bent moves the pitch (applies to the held bend).
    CHECK (std::abs (pitch (2, 0.5f, 40, 12) - noteHz (75.0)) < 0.3);
}

TEST_CASE ("Mod wheel adds wheel x depth to the VCO LFO mod amount (same taper and target)", "[part][wheel]")
{
    // The wheel moves 100 ms before the note, so its smoothing has settled.
    auto run = [] (float slider, float depth, float wheel) {
        Rig rig;
        auto& p = rig.params;
        p.vco1Wave = 2;
        p.vco2Wave = 2;
        p.vcoModLfo = slider;
        p.modWheelDepth = depth;
        p.lfoRate = 0.7f;
        rig.apply();
        std::vector<vs::NoteEvent> events;
        if (wheel > 0.0f)
            events.push_back (ev (0, EventType::ModWheel, 0, wheel));
        events.push_back (on (4800, 57));
        return rig.render (48000, events);
    };
    const auto wheelOnly = run (0.0f, 0.3f, 1.0f);
    const auto sliderOnly = run (0.3f, 0.3f, 0.0f);
    const auto none = run (0.0f, 0.3f, 0.0f);
    const auto half = run (0.0f, 0.6f, 0.5f);
    CHECK (differenceDb (sliderOnly, wheelOnly) < -100.0);
    CHECK (differenceDb (sliderOnly, half) < -100.0);
    // Added to the slider before the squared taper (2 x 0.3^2), not after it (2 x 0.2^2 + 2 x 0.1^2).
    CHECK (differenceDb (sliderOnly, run (0.2f, 0.1f, 1.0f)) < -100.0);
    CHECK (differenceDb (none, wheelOnly) > -20.0); // it does add vibrato
    // Capped at the slider's maximum.
    CHECK (differenceDb (run (1.0f, 0.3f, 0.0f), run (0.9f, 0.3f, 1.0f)) < -100.0);
    // Target switch: VCO-2 only while only VCO-1 is heard -> no vibrato from the wheel either.
    auto vco1Only = [&] (float wheel) {
        Rig rig;
        auto& p = rig.params;
        sinePatch (p);
        p.vcoModTarget = 2;
        p.modWheelDepth = 1.0f;
        rig.apply();
        std::vector<vs::NoteEvent> events;
        if (wheel > 0.0f)
            events.push_back (ev (0, EventType::ModWheel, 0, wheel));
        events.push_back (on (4800, 57));
        return rig.render (24000, events);
    };
    CHECK (differenceDb (vco1Only (0.0f), vco1Only (1.0f)) < -120.0);
}

TEST_CASE ("Unison: voices detuned symmetrically across the spread, loudness about constant (1/sqrt n)", "[part][unison]")
{
    SECTION ("detune spread")
    {
        Rig rig;
        sinePatch (rig.params);
        rig.params.assignMode = 1;
        rig.params.unisonVoices = 1;    // 4
        rig.params.unisonDetune = 1.0f; // 50 cents total
        rig.apply();
        constexpr int order = 16;
        const auto out = rig.render (12000 + (1 << order), { on (0, 69) });
        CHECK (rig.part.numSoundingVoices() == 4);
        const auto spec = hannSpectrum (out, 12000, order);
        double lo = 1.0e9, hi = -1.0e9;
        for (const double cents : { -25.0, -25.0 / 3.0, 25.0 / 3.0, 25.0 })
        {
            const double expected = 440.0 * std::pow (2.0, cents / 1200.0);
            const auto p = findPeak (spec, order, expected, 1.5);
            INFO (cents << " ct: " << p.hz << " Hz (expected " << expected << "), " << p.db << " dB");
            CHECK (std::abs (p.hz - expected) < 0.15);
            lo = std::min (lo, p.db);
            hi = std::max (hi, p.db);
        }
        CHECK (hi - lo < 1.0);
        // Nothing at the undetuned pitch (4 voices: no centre voice).
        CHECK (vs::test::toDb (spec[static_cast<size_t> (std::lround (440.0 / (kFs / (1 << order))))]) < lo - 15.0);
    }

    SECTION ("loudness")
    {
        auto level = [] (int mode, int voicesIndex) {
            Rig rig;
            sinePatch (rig.params);
            rig.params.assignMode = mode;
            rig.params.unisonVoices = voicesIndex;
            rig.params.unisonDetune = 1.0f;
            rig.apply();
            const auto out = rig.render (6 * 48000, { on (0, 69) });
            return vs::test::toDb (vs::test::rms (out.data() + 48000, 5 * 48000));
        };
        const double solo = level (2, 0);
        for (int idx = 0; idx < 4; ++idx)
        {
            const double db = level (1, idx);
            INFO ("unison " << (2 << idx) << ": " << db - solo << " dB relative to Solo");
            WARN ("unison " << (2 << idx) << " voices: " << db - solo << " dB relative to Solo");
            CHECK (std::abs (db - solo) < 1.0);
        }
    }
}

TEST_CASE ("Changing the assign mode or unison count releases the voices without a click", "[part][mode]")
{
    static constexpr int changeBlock = 100; // 25600 samples
    static constexpr int changeAt = changeBlock * kBlock;
    auto run = [] (int fromMode, int toMode, int fromCount, int toCount, std::vector<int> chord) {
        Rig rig;
        sinePatch (rig.params);
        rig.params.assignMode = fromMode;
        rig.params.unisonVoices = fromCount;
        rig.params.unisonDetune = 0.2f;
        rig.params.env2Release = vs::map::envTimeFromSec (0.3f);
        rig.apply();
        std::vector<vs::NoteEvent> events;
        for (const int n : chord)
            events.push_back (on (0, n));
        const auto out = rig.render (changeAt + 48000, events, kBlock, [=] (int b, vs::PartParams& p) {
            if (b >= changeBlock)
            {
                p.assignMode = toMode;
                p.unisonVoices = toCount;
            }
        });
        const double steady = maxStep (out, 12000, changeAt);
        const double after = maxStep (out, changeAt, changeAt + 4800);
        INFO ("steady max step " << steady << ", after the change " << after);
        CHECK (after <= 1.1 * steady);
        CHECK (vs::test::rms (out.data() + changeAt + 4800, 2400) > 0.01);    // a release, not a cut
        CHECK (vs::test::rms (out.data() + changeAt + 40000, 8000) < 1.0e-4); // and it ends
        CHECK_FALSE (rig.part.isSounding());
    };
    SECTION ("Poly -> Solo") { run (0, 2, 1, 1, { 33, 36, 40, 43 }); }
    SECTION ("Unison 4 -> 8") { run (1, 1, 1, 2, { 36 }); }
    SECTION ("Solo -> Unison") { run (2, 1, 1, 1, { 36 }); }
}

TEST_CASE ("Render does not depend on the host block size with 16 voices, steals, glide and pedal (Vintage 1)", "[part][control][poly]")
{
    static constexpr int length = 3 * 48000;
    auto run = [] (int blockSize) {
        Rig rig;
        rig.part.setVintage (1.0f);
        auto& p = rig.params;
        p.vcfResonance = 0.6f;
        p.vcoSync = 1;
        p.vco1CrossMod = 0.3f;
        p.vcoModLfo = 0.1f;
        p.lfoRate = 0.8f;
        p.portaTime = 0.2f;
        p.velToVcf = 0.5f;
        p.env2Release = 0.5f;
        rig.apply();
        std::vector<vs::NoteEvent> events;
        // 20 notes at odd offsets (four steals), pedal, bends and wheel.
        for (int i = 0; i < 20; ++i)
            events.push_back (on (101 + i * 997, 40 + (i * 7) % 36, 0.3f + 0.035f * static_cast<float> (i)));
        events.push_back (ev (5003, EventType::SustainPedal, 0, 1.0f));
        events.push_back (ev (30011, EventType::ModWheel, 0, 0.7f));
        events.push_back (ev (40009, EventType::PitchBend, 0, -0.4f));
        for (int i = 0; i < 20; i += 2)
            events.push_back (off (60013 + i * 311, 40 + (i * 7) % 36));
        events.push_back (ev (90001, EventType::SustainPedal, 0, 0.0f));
        std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b) { return a.offset < b.offset; });
        auto out = rig.render (length, events, blockSize);
        return out;
    };
    const auto ref = run (512);
    for (const int bs : { 500, 37 })
    {
        const double ratioDb = differenceDb (ref, run (bs));
        INFO ("block size " << bs << " vs 512: " << ratioDb << " dB");
        WARN ("16-voice block-size independence, " << bs << " vs 512: " << ratioDb << " dB");
        CHECK (ratioDb < -100.0);
    }
}

TEST_CASE ("Render does not depend on the host block size when a voice is reused after it went idle", "[part][control]")
{
    // A voice whose envelope finishes inside a sub-block used to keep its oscillators running to
    // the end of that sub-block (and then freeze), so the next note on that voice depended on the
    // host block size (Solo and Unison reuse the same voices on every note): -9 dB before the fix.
    auto run = [] (int blockSize, int mode, float porta) {
        Rig rig;
        rig.part.setVintage (1.0f);
        rig.params.assignMode = mode;
        rig.params.portaTime = porta;
        rig.params.unisonDetune = 0.5f;
        rig.params.vco1Wave = 3;
        rig.params.vco2Wave = 5; // noise: its generator state must not depend on the split either
        rig.params.sourceMix = 0.3f;
        rig.apply();
        std::vector<vs::NoteEvent> e { on (0, 88), off (2000, 88), on (24001, 57), off (30011, 57), on (48003, 64) };
        return rig.render (72000, e, blockSize);
    };
    for (const int mode : { 0, 1, 2 })
        for (const float porta : { 0.0f, 0.25f })
        {
            const double db = differenceDb (run (512, mode, porta), run (97, mode, porta));
            INFO ("mode " << mode << ", portamento " << porta << ": " << db << " dB");
            CHECK (db < -100.0);
        }
}
