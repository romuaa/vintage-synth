// M3 bi-timbral tests (docs/architecture.md 4.6 item 2), through the real processor where the
// contract is about the processor: Dual = sum of both parts, Split boundaries and split moves,
// pan and balance laws, Whole mode (Upper silent and not rendered, and clean when it comes back),
// mixer latch after a key-mode change, stereo and mono output, key-mode changes, block-size
// independence with both parts at full Vintage, when parameter changes take effect, event
// overflow and random streams with both parts, and the new parameters.
//
// Host rates (gate 4.4): every engine behaviour test runs at 48 kHz (2x), 96 kHz (1x) and
// 192 kHz (1x). Positions and lengths in the tests are written in 48 kHz samples; ProcRig scales
// them to the host rate and returns the output on the 48 kHz grid (every 2nd / 4th sample: the
// test signals are band-limited far below 24 kHz). Host block sizes are in host samples.
// Only at 48 kHz: the pan/balance laws, mixer smoothing and parameter/state tests (pure gain
// math and parameter plumbing, no rate-dependent code) and the large random-stream sweep (more
// seeds at 48 kHz, fewer at the high rates to keep the suite fast).
// Oversampling by host rate (4.4) is in OversamplingTests.cpp.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "engine/EventParser.h"
#include "engine/PartMixer.h"
#include "params/Mappings.h"
#include "params/ParameterLayout.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <vector>

namespace
{
struct Midi
{
    int pos; // 48 kHz samples
    juce::MidiMessage msg;
};

Midi noteOn (int pos, int note, int vel = 100) { return { pos, juce::MidiMessage::noteOn (1, note, static_cast<juce::uint8> (vel)) }; }
Midi noteOff (int pos, int note) { return { pos, juce::MidiMessage::noteOff (1, note) }; }
Midi cc (int pos, int controller, int value) { return { pos, juce::MidiMessage::controllerEvent (1, controller, value) }; }
Midi bend (int pos, int value14) { return { pos, juce::MidiMessage::pitchWheel (1, value14) }; }

struct Stereo
{
    std::vector<float> l, r;
};

/** Host block boundaries: blockSize-long blocks, also split at every change position (host samples). */
std::vector<int> blockStarts (int total, int blockSize, std::vector<int> splits)
{
    std::sort (splits.begin(), splits.end());
    std::vector<int> starts;
    size_t next = 0;
    for (int pos = 0; pos < total;)
    {
        starts.push_back (pos);
        while (next < splits.size() && splits[next] <= pos)
            ++next;
        int end = std::min (pos + blockSize, total);
        if (next < splits.size())
            end = std::min (end, splits[next]);
        pos = end;
    }
    starts.push_back (total);
    return starts;
}

/**
    The real processor, driven like a host at a given rate. Every render goes through render(),
    which checks the output bounds (a render that overran its buffer corrupted the heap on Windows
    in M2a) and splits host blocks at parameter changes, so a change lands on the same sample for
    any block size (see the "take effect" test for what happens without the split).
*/
struct ProcRig
{
    juce::ScopedJuceInitialiser_GUI init;
    vs::VintageSynthProcessor proc;
    double fs;
    int scale; // host samples per 48 kHz sample
    int maxBlock;
    int channels;

    explicit ProcRig (double rate = 48000.0, int maxBlockSize = 512, int numChannels = 2)
        : fs (rate), scale (static_cast<int> (std::lround (rate / 48000.0))), maxBlock (maxBlockSize), channels (numChannels)
    {
        REQUIRE (std::abs (rate - 48000.0 * scale) < 1.0e-6);
    }

    void set (const juce::String& id, float plain)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
    }

    float get (const juce::String& id) { return proc.getState().getRawParameterValue (id)->load(); }

    void prepare()
    {
        juce::AudioProcessor::BusesLayout layout;
        layout.outputBuses.add (channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
        REQUIRE (proc.setBusesLayout (layout));
        proc.setNonRealtime (false);
        proc.setPlayConfigDetails (0, channels, fs, maxBlock);
        proc.prepareToPlay (fs, maxBlock);
    }

    struct Change
    {
        int at; // 48 kHz samples
        std::function<void (ProcRig&)> apply;
    };

    /**
        Renders numSamples (48 kHz units) and returns them on the 48 kHz grid. blockSize is in host
        samples. afterBlock (end, rig) runs after every host block (end in host samples).
    */
    Stereo render (int numSamples, std::vector<Midi> midi, int blockSize, std::vector<Change> changes = {},
                   const std::function<void (int, ProcRig&)>& afterBlock = {})
    {
        REQUIRE (blockSize >= 1);
        REQUIRE (blockSize <= maxBlock);
        std::stable_sort (midi.begin(), midi.end(), [] (const Midi& a, const Midi& b) { return a.pos < b.pos; });
        std::stable_sort (changes.begin(), changes.end(), [] (const Change& a, const Change& b) { return a.at < b.at; });
        const int total = numSamples * scale;
        std::vector<int> splits;
        for (const auto& c : changes)
            splits.push_back (c.at * scale);
        const auto starts = blockStarts (total, blockSize, splits);

        std::vector<float> l (static_cast<size_t> (total)), r (static_cast<size_t> (total));
        juce::AudioBuffer<float> buffer (channels, blockSize);
        size_t nextMidi = 0, nextChange = 0;
        for (size_t b = 0; b + 1 < starts.size(); ++b)
        {
            const int pos = starts[b], end = starts[b + 1], len = end - pos;
            while (nextChange < changes.size() && changes[nextChange].at * scale <= pos)
                changes[nextChange++].apply (*this);
            REQUIRE (len >= 1);
            REQUIRE (len <= blockSize);
            REQUIRE (static_cast<size_t> (end) <= l.size());
            juce::MidiBuffer mb;
            while (nextMidi < midi.size() && midi[nextMidi].pos * scale < end)
            {
                mb.addEvent (midi[nextMidi].msg, std::max (0, midi[nextMidi].pos * scale - pos));
                ++nextMidi;
            }
            buffer.setSize (channels, len, false, false, true);
            buffer.clear();
            proc.processBlock (buffer, mb);
            std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + len, l.begin() + pos);
            const int rch = channels > 1 ? 1 : 0;
            std::copy (buffer.getReadPointer (rch), buffer.getReadPointer (rch) + len, r.begin() + pos);
            if (afterBlock)
                afterBlock (end, *this);
        }
        Stereo out;
        out.l.resize (static_cast<size_t> (numSamples));
        out.r.resize (static_cast<size_t> (numSamples));
        for (int i = 0; i < numSamples; ++i)
        {
            out.l[static_cast<size_t> (i)] = l[static_cast<size_t> (i * scale)];
            out.r[static_cast<size_t> (i)] = r[static_cast<size_t> (i * scale)];
        }
        return out;
    }

    float masterGain() { return juce::Decibels::decibelsToGain (get (vs::ParamID::masterVolume)); }
};

/** Parsed events at host positions (48 kHz positions x scale). */
std::vector<vs::NoteEvent> toEvents (const std::vector<Midi>& midi, int scale)
{
    std::vector<vs::NoteEvent> v;
    for (const auto& m : midi)
    {
        vs::NoteEvent e {};
        if (vs::parseMidiMessage (m.msg.getRawData(), m.msg.getRawDataSize(), m.pos * scale, e))
            v.push_back (e);
    }
    std::stable_sort (v.begin(), v.end(), [] (const auto& a, const auto& b) { return a.offset < b.offset; });
    return v;
}

/**
    Stand-alone reference Part fed exactly what the processor's part gets: the same parameters
    (read through the same binding), seed, oversampling factor, Vintage, events and host blocks.
*/
struct RefPart
{
    vs::Part part;
    vs::PartParams params;
    float vintage = 0.0f;
    int scale;

    RefPart (ProcRig& rig, const char* prefix, std::uint32_t seed) : scale (rig.scale)
    {
        vs::PartParamBinding b;
        b.bind (rig.proc.getState(), prefix);
        b.read (params);
        vintage = rig.get (vs::ParamID::vintage);
        part.setParams (params);
        part.prepare (rig.fs, rig.maxBlock, rig.proc.getOversamplingFactor(), seed);
    }

    /** numSamples and split points in 48 kHz units, events at host positions; output at host rate. */
    std::vector<float> renderHost (int numSamples, const std::vector<vs::NoteEvent>& events, int blockSize,
                                   const std::vector<int>& splits48 = {})
    {
        const int total = numSamples * scale;
        std::vector<int> splits;
        for (const int s : splits48)
            splits.push_back (s * scale);
        const auto starts = blockStarts (total, blockSize, splits);
        std::vector<float> out (static_cast<size_t> (total));
        std::vector<vs::NoteEvent> chunk;
        for (size_t b = 0; b + 1 < starts.size(); ++b)
        {
            const int start = starts[b], len = starts[b + 1] - start;
            REQUIRE (static_cast<size_t> (start + len) <= out.size());
            chunk.clear();
            for (auto e : events)
                if (e.offset >= start && e.offset < start + len)
                {
                    e.offset -= start;
                    chunk.push_back (e);
                }
            part.setParams (params);
            part.setVintage (vintage);
            part.render (out.data() + start, len, chunk);
        }
        return out;
    }
};

std::vector<float> onGrid (const std::vector<float>& host, int scale)
{
    std::vector<float> v (host.size() / static_cast<size_t> (scale));
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = host[i * static_cast<size_t> (scale)];
    return v;
}

double rmsDb (const std::vector<float>& x, int from, int len)
{
    REQUIRE (from >= 0);
    REQUIRE (static_cast<size_t> (from + len) <= x.size());
    return vs::test::toDb (vs::test::rms (x.data() + from, static_cast<size_t> (len)));
}

double peak (const std::vector<float>& x, int from, int to)
{
    double m = 0.0;
    for (int i = std::max (0, from); i < std::min (to, static_cast<int> (x.size())); ++i)
        m = std::max (m, static_cast<double> (std::abs (x[static_cast<size_t> (i)])));
    return m;
}

double maxStep (const std::vector<float>& x, int from, int to)
{
    double m = 0.0;
    for (int i = std::max (1, from); i < std::min (to, static_cast<int> (x.size())); ++i)
        m = std::max (m, static_cast<double> (std::abs (x[static_cast<size_t> (i)] - x[static_cast<size_t> (i - 1)])));
    return m;
}

double maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    REQUIRE (a.size() == b.size());
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max (m, static_cast<double> (std::abs (a[i] - b[i])));
    return m;
}

/** Difference energy relative to the signal energy, dB (both channels). */
double differenceDb (const Stereo& ref, const Stereo& other)
{
    double signal = 0.0, diff = 0.0;
    for (const auto& [a, b] : { std::pair { &ref.l, &other.l }, std::pair { &ref.r, &other.r } })
        for (size_t i = 0; i < a->size(); ++i)
        {
            signal += static_cast<double> ((*a)[i]) * static_cast<double> ((*a)[i]);
            const double d = static_cast<double> ((*b)[i]) - static_cast<double> ((*a)[i]);
            diff += d * d;
        }
    REQUIRE (signal > 1.0); // audible
    return 10.0 * std::log10 (std::max (diff, 1.0e-30) / signal);
}

/** Two clearly different patches: Lower a resonant saw pad (Poly), Upper a square lead (Solo). */
void twoPatches (ProcRig& rig)
{
    rig.set ("lower.vcf.cutoff", 0.45f);
    rig.set ("lower.vcf.resonance", 0.55f);
    rig.set ("lower.env2.attack", 0.25f);
    rig.set ("lower.env2.release", 0.35f);
    rig.set ("upper.vco1.waveform", 3.0f); // square
    rig.set ("upper.vco2.waveform", 3.0f);
    rig.set ("upper.vco2.range", 2.0f);     // 4'
    rig.set ("upper.vcf.cutoff", 0.75f);
    rig.set ("upper.assign.mode", 2.0f);    // Solo
    rig.set ("upper.env2.release", 0.2f);
    rig.set ("upper.lfo.rate", 0.8f);
    rig.set ("upper.vcomod.lfo_mod", 0.15f);
}

constexpr float kWhole = 0.0f, kDual = 1.0f, kSplit = 2.0f; // key.mode choice index (plain value)

#define VS_EVERY_RATE const double rate = GENERATE (48000.0, 96000.0, 192000.0); INFO ("host rate " << rate)
} // namespace

// =================================================================================================
// Dual, Split, Whole
// =================================================================================================

TEST_CASE ("M3 Dual: the output is the sum of both parts rendered separately (full Vintage)", "[m3][dual]")
{
    VS_EVERY_RATE;
    for (const int block : { 512, 97 })
    {
        ProcRig rig (rate);
        twoPatches (rig);
        rig.set (vs::ParamID::keyMode, kDual);
        rig.set (vs::ParamID::vintage, 1.0f);
        rig.prepare();

        const std::vector<Midi> midi { noteOn (101, 48), noteOn (2203, 55, 70), noteOn (5007, 60, 120), bend (9001, 12000),
                                       cc (12011, 1, 90), noteOff (30011, 55), noteOff (36001, 60), noteOff (40009, 48) };
        constexpr int length = 60000;
        const auto out = rig.render (length, midi, block);

        RefPart lower (rig, vs::kLowerPrefix, vs::VintageSynthProcessor::kLowerSeed);
        RefPart upper (rig, vs::kUpperPrefix, vs::VintageSynthProcessor::kUpperSeed);
        const auto events = toEvents (midi, rig.scale);
        const auto lo = onGrid (lower.renderHost (length, events, block), rig.scale);
        const auto up = onGrid (upper.renderHost (length, events, block), rig.scale);
        const float g = rig.masterGain();

        std::vector<float> expected (static_cast<size_t> (length));
        for (size_t i = 0; i < expected.size(); ++i)
            expected[i] = (lo[i] + up[i]) * g;

        // Both parts really sound, and differently (a mixer that dropped or duplicated one would fail).
        REQUIRE (rmsDb (lo, 6000, 20000) > -40.0);
        REQUIRE (rmsDb (up, 6000, 20000) > -40.0);
        std::vector<float> loMinusUp (lo.size());
        for (size_t i = 0; i < lo.size(); ++i)
            loMinusUp[i] = lo[i] - up[i];
        REQUIRE (rmsDb (loMinusUp, 6000, 20000) > -40.0);

        const double errL = maxAbsDiff (out.l, expected), errR = maxAbsDiff (out.r, expected);
        INFO ("block " << block << ": max |L - (lower + upper) g| = " << errL << ", R: " << errR);
        WARN ("Dual = sum of parts, " << rate << " Hz, block " << block << ": max deviation L " << errL << ", R " << errR);
        CHECK (errL <= 1.0e-6);
        CHECK (errR <= 1.0e-6);
    }
}

TEST_CASE ("M3 Split: notes below the split point play Lower, at or above it Upper; moving it with held keys", "[m3][split]")
{
    VS_EVERY_RATE;
    // Lower hard left and Upper hard right: L carries only Lower, R only Upper.
    ProcRig rig (rate);
    rig.set (vs::ParamID::keyMode, kSplit);
    rig.set (vs::ParamID::keySplitPoint, 60.0f);
    rig.set ("lower.mix.pan", -1.0f);
    rig.set ("upper.mix.pan", 1.0f);
    rig.set ("lower.env2.release", 0.3f); // 16 ms
    rig.set ("upper.env2.release", 0.3f);
    rig.prepare();

    constexpr int seg = 24000;
    std::vector<Midi> midi {
        noteOn (0, 59), noteOff (seg - 4800, 59),             // segment 0: 59 -> Lower
        noteOn (seg, 60), noteOff (2 * seg - 4800, 60),       // segment 1: 60 -> Upper
        noteOn (2 * seg, 62),                                 // segment 2: 62 -> Upper, held ...
        noteOff (3 * seg + 14000, 62),                        // ... released after the split moved to 70
        noteOn (4 * seg, 62), noteOff (5 * seg - 4800, 62),   // segment 4: 62 -> Lower now
        noteOn (5 * seg, 70), noteOff (6 * seg - 4800, 70),   // segment 5: 70 -> Upper (at the new split)
    };
    const std::vector<ProcRig::Change> changes { { 3 * seg, [] (ProcRig& r) { r.set (vs::ParamID::keySplitPoint, 70.0f); } } };
    const auto out = rig.render (7 * seg, midi, 256, changes);

    auto loud = [&] (const std::vector<float>& x, int s) { return rmsDb (x, s * seg + 2400, 9600) > -40.0; };
    auto silent = [&] (const std::vector<float>& x, int from, int to) { return peak (x, from, to) < 1.0e-9; };

    CHECK (loud (out.l, 0));
    CHECK (silent (out.r, 0, seg));
    CHECK (loud (out.r, 1));
    CHECK (silent (out.l, seg, 2 * seg));
    CHECK (loud (out.r, 2));
    CHECK (loud (out.r, 3)); // still held after the split point moved: it still sounds in Upper ...
    CHECK (silent (out.l, 2 * seg, 4 * seg)); // ... and the move did not make Lower play it
    CHECK (peak (out.r, 3 * seg + 14000 + 4800, 4 * seg) < 1.0e-6); // released in Upper: no hanging note
    CHECK (loud (out.l, 4));
    CHECK (silent (out.r, 4 * seg, 5 * seg));
    CHECK (loud (out.r, 5));
    CHECK (silent (out.l, 5 * seg, 6 * seg));
    CHECK (peak (out.l, 6 * seg + 4800, 7 * seg) < 1.0e-6);
    CHECK (peak (out.r, 6 * seg + 4800, 7 * seg) < 1.0e-6);
    CHECK_FALSE (rig.proc.getLowerPart().isSounding());
    CHECK_FALSE (rig.proc.getUpperPart().isSounding());
}

TEST_CASE ("M3 Whole: Upper is silent and never rendered; the output is Lower alone", "[m3][whole]")
{
    VS_EVERY_RATE;
    ProcRig rig (rate);
    twoPatches (rig);
    rig.set ("upper.mix.pan", 1.0f);
    rig.set (vs::ParamID::mixBalance, 1.0f); // balance has no effect in Whole
    rig.set (vs::ParamID::vintage, 1.0f);
    rig.prepare();
    REQUIRE_FALSE (rig.proc.isUpperRendering());

    const std::vector<Midi> midi { noteOn (11, 48), noteOn (3001, 64), cc (5000, 64, 127), bend (7000, 3000),
                                   noteOff (20000, 48), noteOff (21000, 64), cc (30000, 64, 0) };
    // Upper's parameters move all the time: still nothing may be rendered for it.
    std::vector<ProcRig::Change> changes;
    std::vector<int> splits;
    for (int k = 1; k < 40; ++k)
    {
        changes.push_back ({ k * 1000, [k] (ProcRig& r) { r.set ("upper.vcf.cutoff", static_cast<float> (k % 7) / 7.0f); } });
        splits.push_back (k * 1000);
    }
    const auto out = rig.render (48000, midi, 256, changes);

    CHECK (rig.proc.getUpperPart().getRenderedSamples() == 0);
    CHECK_FALSE (rig.proc.isUpperRendering());

    RefPart lower (rig, vs::kLowerPrefix, vs::VintageSynthProcessor::kLowerSeed);
    auto lo = onGrid (lower.renderHost (48000, toEvents (midi, rig.scale), 256, splits), rig.scale);
    const float g = rig.masterGain();
    for (auto& s : lo)
        s *= g;
    REQUIRE (rmsDb (lo, 4000, 10000) > -40.0);
    CHECK (maxAbsDiff (out.l, lo) <= 1.0e-7); // Lower at unity, centred (balance ignored), Upper absent
    CHECK (maxAbsDiff (out.r, lo) <= 1.0e-7);
}

TEST_CASE ("M3 Whole -> Dual: Upper starts clean (parameters at target, fresh LFO), like a freshly prepared part", "[m3][whole]")
{
    VS_EVERY_RATE;
    ProcRig rig (rate);
    twoPatches (rig);
    rig.set ("lower.mix.pan", -1.0f); // R carries Upper only
    rig.set ("upper.mix.pan", 1.0f);
    rig.prepare();

    // One second of Whole with Upper's parameters far away from where they end up. The final
    // values differ from those at prepare time, so a part that was never rendered would still
    // glide from stale smoother values.
    std::vector<ProcRig::Change> changes;
    changes.push_back ({ 1000, [] (ProcRig& r) { r.set ("upper.vcf.cutoff", 0.1f); r.set ("upper.vca.level", 0.2f); } });
    changes.push_back ({ 47000, [] (ProcRig& r) { r.set ("upper.vcf.cutoff", 0.62f); r.set ("upper.vca.level", 0.7f); } });
    changes.push_back ({ 48000, [] (ProcRig& r) { r.set (vs::ParamID::keyMode, kDual); } });
    const std::vector<Midi> midi { noteOn (100, 40), noteOff (20000, 40), noteOn (48000, 67), noteOff (70000, 67) };
    const auto out = rig.render (96000, midi, 256, changes);

    // Reference: a fresh Upper part with the final parameters, the note at its first sample.
    RefPart upper (rig, vs::kUpperPrefix, vs::VintageSynthProcessor::kUpperSeed);
    const auto ref = onGrid (upper.renderHost (48000, { { 0, vs::EventType::NoteOn, 67, 100.0f / 127.0f },
                                                        { 22000 * rig.scale, vs::EventType::NoteOff, 67, 0.0f } }, 256), rig.scale);
    const float k = rig.masterGain() * vs::map::panGains (1.0f).right;
    std::vector<float> got (out.r.begin() + 48000, out.r.end()), expected (ref.size());
    for (size_t i = 0; i < ref.size(); ++i)
        expected[i] = ref[i] * k;
    REQUIRE (rmsDb (expected, 2000, 10000) > -40.0);
    const double err = maxAbsDiff (got, expected);
    INFO ("max deviation from a fresh part: " << err);
    CHECK (err <= 1.0e-6);
    CHECK (peak (out.r, 0, 48000) < 1.0e-9); // and nothing from Upper during Whole
}

TEST_CASE ("M3 Whole: controllers sent in Whole are in effect when Upper starts (pedal, bend)", "[m3][whole][pedal]")
{
    VS_EVERY_RATE;
    ProcRig rig (rate);
    rig.set ("lower.mix.pan", -1.0f);
    rig.set ("upper.mix.pan", 1.0f);
    rig.set ("upper.vco1.waveform", 0.0f); // sine
    rig.set ("upper.mixer.source_mix", 0.0f);
    rig.set ("upper.vcf.cutoff", 1.0f);
    rig.set ("upper.vcf.key_follow", 0.0f);
    rig.set ("upper.env2.sustain", 1.0f);
    rig.prepare();

    // In Whole: pedal down and a full bend up (default range 2 st). Then Dual, play and release
    // a note: it must sound at +2 semitones from the start and be held by the pedal.
    const std::vector<Midi> midi { cc (1000, 64, 127), bend (2000, 16383), noteOn (48000, 69), noteOff (52000, 69) };
    const std::vector<ProcRig::Change> changes { { 48000, [] (ProcRig& r) { r.set (vs::ParamID::keyMode, kDual); } } };
    const auto out = rig.render (48000 * 3, midi, 256, changes);

    const std::vector<float> early (out.r.begin() + 48000 + 480, out.r.begin() + 48000 + 480 + 4800); // 10..110 ms
    const double hz = vs::test::measureFrequency (early, 48000.0);
    INFO ("Upper pitch right after the switch: " << hz << " Hz (expected " << 440.0 * std::pow (2.0, 2.0 / 12.0) << ")");
    CHECK (std::abs (1200.0 * std::log2 (hz / (440.0 * std::pow (2.0, 2.0 / 12.0)))) < 2.0);
    CHECK (rmsDb (out.r, 48000 * 2, 24000) > -30.0); // released key, still held by the pedal
}

TEST_CASE ("M3 Dual -> Whole: Upper releases (no click), then stops being rendered", "[m3][whole][mode]")
{
    VS_EVERY_RATE;
    ProcRig rig (rate);
    twoPatches (rig);
    rig.set (vs::ParamID::keyMode, kDual);
    rig.set ("upper.env2.release", 0.5f); // 100 ms
    rig.prepare();
    const std::vector<Midi> midi { noteOn (0, 52), noteOn (0, 59), noteOn (0, 64) };
    const std::vector<ProcRig::Change> changes { { 24000, [] (ProcRig& r) { r.set (vs::ParamID::keyMode, kWhole); } } };
    const auto out = rig.render (72000, midi, 256, changes);

    const auto rendered = rig.proc.getUpperPart().getRenderedSamples();
    CHECK_FALSE (rig.proc.isUpperRendering());
    CHECK (rendered > 24000 * rig.scale);          // it kept rendering its release tail ...
    CHECK (rendered < (24000 + 24000) * rig.scale); // ... and stopped well before the end
    rig.render (4800, {}, 256);
    CHECK (rig.proc.getUpperPart().getRenderedSamples() == rendered); // nothing more in Whole

    // Click check: the largest step around the change stays within the signal's own steps.
    const double before = maxStep (out.l, 20000, 24000), around = maxStep (out.l, 24000, 26000);
    INFO ("max step before " << before << ", around the change " << around);
    CHECK (around <= 1.1 * before);
    CHECK (peak (out.l, 48000, 72000) < 1.0e-6); // all voices of both parts were released
}

// =================================================================================================
// Mixer latch after a key-mode change (owner decision, M3 review)
// =================================================================================================

TEST_CASE ("M3 latch: after Dual -> Whole a Lower tail the balance had silenced stays silent until Lower plays again", "[m3][mixer][latch]")
{
    VS_EVERY_RATE;
    // Dual with balance +1 (Lower at 0), Lower hard left: L carries only Lower. A long Lower
    // release is running when the mode changes to Whole (Lower's target becomes unity). The tail
    // must stay inaudible (L exactly 0) until Lower's next note-on, from where Lower moves to unity.
    auto run = [rate] (int block) {
        ProcRig rig (rate);
        rig.set (vs::ParamID::keyMode, kDual);
        rig.set (vs::ParamID::mixBalance, 1.0f);
        rig.set ("lower.mix.pan", -1.0f);
        rig.set ("upper.mix.pan", 1.0f);
        rig.set ("lower.env2.release", 0.8f); // 1.6 s
        rig.prepare();
        const std::vector<Midi> midi { noteOn (100, 48), noteOn (100, 55), noteOn (40001, 60), noteOff (60000, 60) };
        const std::vector<ProcRig::Change> changes { { 24000, [] (ProcRig& r) { r.set (vs::ParamID::keyMode, kWhole); } } };
        return rig.render (72000, midi, block, changes);
    };
    const auto ref = run (512);
    CHECK (peak (ref.l, 0, 40001) <= 0.0);                // silent in Dual and through the whole Lower tail in Whole
    CHECK (rmsDb (ref.r, 4800, 9600) > -40.0);            // (Upper was audible in Dual)
    CHECK (rmsDb (ref.l, 40001 + 2400, 9600) > -40.0);    // the new note plays Lower at unity
    // Without the latch the tail would come back here (measured -23 dB in the review).
    for (const int block : { 97, 1 })
    {
        const auto other = run (block);
        const double db = differenceDb (ref, other);
        INFO ("block " << block << " vs 512: " << db << " dB");
        CHECK (peak (other.l, 0, 40001) <= 0.0);
        CHECK (db < -100.0);
    }
}

TEST_CASE ("M3 latch: after Whole -> Dual a Lower tail keeps unity until Lower's next note, then moves to its balance gain", "[m3][mixer][latch]")
{
    VS_EVERY_RATE;
    // Whole, Lower hard left (L = Lower only), long release running at the change to Dual with
    // balance +1 (Lower's target 0). Until the next note-on the output equals the same render with
    // balance 0 (no change of Lower's gain); after it, Lower falls to 0.
    auto run = [rate] (int block, float balance) {
        ProcRig rig (rate);
        rig.set (vs::ParamID::mixBalance, balance);
        rig.set ("lower.mix.pan", -1.0f);
        rig.set ("upper.mix.pan", 1.0f);
        rig.set ("lower.env2.release", 0.8f);
        rig.prepare();
        const std::vector<Midi> midi { noteOn (100, 48), noteOn (100, 55), noteOn (40001, 60), noteOff (60000, 60) };
        const std::vector<ProcRig::Change> changes { { 24000, [] (ProcRig& r) { r.set (vs::ParamID::keyMode, kDual); } } };
        return rig.render (72000, midi, block, changes);
    };
    for (const int block : { 512, 97, 1 })
    {
        INFO ("block " << block);
        const auto latched = run (block, 1.0f), reference = run (block, 0.0f);
        const std::vector<float> a (latched.l.begin(), latched.l.begin() + 40001), b (reference.l.begin(), reference.l.begin() + 40001);
        CHECK (maxAbsDiff (a, b) <= 0.0);                    // the tail at the level it was heard at
        CHECK (rmsDb (latched.l, 30000, 4800) > -60.0);      // (and there is a tail to hear)
        CHECK (peak (latched.l, 40001 + 4800, 72000) < 1.0e-4); // after the note-on: Lower at its balance gain 0
        CHECK (rmsDb (latched.r, 40001 + 2400, 9600) > -40.0);  // Upper plays the new note
    }
}

// =================================================================================================
// Mixer: pan and balance laws, smoothing, stereo and mono output
// =================================================================================================

namespace
{
/** The decided law: unity at the centre, the near side exactly 1, the far side sqrt2 cos. */
double lawFar (double x) // x in 0..1: how far toward the near side
{
    return std::sqrt (2.0) * std::cos ((x + 1.0) * juce::MathConstants<double>::pi / 4.0);
}
} // namespace

TEST_CASE ("M3 pan and balance law (map): never a boost, near side unity, far side falls to exactly 0, monotonic, continuous", "[m3][mixer]")
{
    float prevL = 2.0f, prevR = -1.0f;
    for (int k = -1000; k <= 1000; ++k)
    {
        const float p = static_cast<float> (k) / 1000.0f;
        const auto g = vs::map::panGains (p);
        const auto b = vs::map::balanceGains (p);
        INFO ("pan " << p);
        CHECK (g.left <= 1.0f);
        CHECK (g.right <= 1.0f);
        CHECK (g.left >= 0.0f);
        CHECK (g.right >= 0.0f);
        CHECK (std::abs (b.lower - g.left) <= 0.0f); // the same law for the part balance
        CHECK (std::abs (b.upper - g.right) <= 0.0f);
        if (p <= 0.0f)
            CHECK (std::abs (g.left - 1.0f) <= 0.0f); // near side: exactly unity
        if (p >= 0.0f)
            CHECK (std::abs (g.right - 1.0f) <= 0.0f);
        const double far = p < 0.0f ? g.right : g.left;
        CHECK (std::abs (far - lawFar (std::abs (static_cast<double> (p)))) < 2.0e-6);
        CHECK (g.left <= prevL);  // monotonic
        CHECK (g.right >= prevR);
        CHECK (std::abs (g.left - prevL) < 0.003f + (k == -1000 ? 2.0f : 0.0f)); // continuous: small steps
        prevL = g.left;
        prevR = g.right;
    }
    CHECK (std::abs (vs::map::panGains (-1.0f).right) <= 0.0f); // exactly 0 at the far extreme
    CHECK (std::abs (vs::map::panGains (1.0f).left) <= 0.0f);
    CHECK (std::abs (vs::map::panGains (0.0f).left - 1.0f) <= 0.0f); // exactly unity at the centre
    CHECK (std::abs (vs::map::panGains (0.0f).right - 1.0f) <= 0.0f);
}

TEST_CASE ("M3 pan law through the processor, both parts: levels follow the law (+-0.05 dB), centre is unity", "[m3][mixer]")
{
    for (const bool upperPart : { false, true })
    {
        // Lower: Whole mode. Upper: Split at 0, so every note goes to Upper.
        auto levels = [upperPart] (float pan) {
            ProcRig rig;
            if (upperPart)
            {
                rig.set (vs::ParamID::keyMode, kSplit);
                rig.set (vs::ParamID::keySplitPoint, 0.0f);
            }
            rig.set (upperPart ? "upper.mix.pan" : "lower.mix.pan", pan);
            rig.set (upperPart ? "lower.mix.pan" : "upper.mix.pan", -0.7f); // the other part's pan must not matter
            rig.set ("lower.env2.sustain", 1.0f);
            rig.set ("upper.env2.sustain", 1.0f);
            rig.prepare();
            const auto out = rig.render (24000, { noteOn (0, 57) }, 512);
            return std::pair { rmsDb (out.l, 9600, 9600), rmsDb (out.r, 9600, 9600) };
        };
        const auto [cl, cr] = levels (0.0f);
        INFO ((upperPart ? "Upper" : "Lower"));
        CHECK (std::abs (cl - cr) < 1.0e-4);
        for (const float pan : { -1.0f, -0.6f, -0.25f, 0.3f, 0.8f, 1.0f })
        {
            const auto [l, r] = levels (pan);
            const double x = std::abs (static_cast<double> (pan));
            const double expFar = 20.0 * std::log10 (std::max (1.0e-12, lawFar (x)));
            const double nearDb = pan < 0.0f ? l - cl : r - cl, farDb = pan < 0.0f ? r - cl : l - cl;
            INFO ("pan " << pan << ": near " << nearDb << " dB (law 0), far " << farDb << " dB (law " << expFar << ")");
            CHECK (std::abs (nearDb) <= 0.05);
            if (x < 1.0)
                CHECK (std::abs (farDb - expFar) <= 0.05);
            else
                CHECK (farDb < -200.0); // exactly silent
        }
    }
}

TEST_CASE ("M3 balance law through the processor: Lower and Upper follow the law, unity at 0, no effect in Whole", "[m3][mixer]")
{
    // Dual, Lower hard left, Upper hard right: L measures Lower's gain, R Upper's.
    auto levels = [] (float balance, float mode) {
        ProcRig rig;
        rig.set (vs::ParamID::keyMode, mode);
        rig.set (vs::ParamID::mixBalance, balance);
        rig.set ("lower.mix.pan", -1.0f);
        rig.set ("upper.mix.pan", 1.0f);
        rig.set ("lower.env2.sustain", 1.0f);
        rig.set ("upper.env2.sustain", 1.0f);
        rig.set (vs::ParamID::vintage, 0.5f);
        rig.prepare();
        return rig.render (24000, { noteOn (0, 57), noteOn (0, 64) }, 512);
    };
    const auto centre = levels (0.0f, kDual);
    const double l0 = rmsDb (centre.l, 9600, 9600), r0 = rmsDb (centre.r, 9600, 9600);
    REQUIRE (l0 > -40.0);
    REQUIRE (r0 > -40.0);

    for (const float b : { -1.0f, -0.5f, -0.1f, 0.4f, 1.0f })
    {
        const auto out = levels (b, kDual);
        const double dl = rmsDb (out.l, 9600, 9600) - l0, dr = rmsDb (out.r, 9600, 9600) - r0;
        const double x = std::abs (static_cast<double> (b));
        const double expFar = 20.0 * std::log10 (std::max (1.0e-12, lawFar (x)));
        const double nearDb = b < 0.0f ? dl : dr, farDb = b < 0.0f ? dr : dl;
        INFO ("balance " << b << ": near part " << nearDb << " dB (law 0), far part " << farDb << " dB (law " << expFar << ")");
        CHECK (std::abs (nearDb) <= 0.05);
        if (x < 1.0)
            CHECK (std::abs (farDb - expFar) <= 0.05);
        else
            CHECK (farDb < -200.0);
    }

    // Whole: the balance does nothing (Lower at unity), bit for bit.
    const auto w0 = levels (0.0f, kWhole);
    for (const float b : { -1.0f, 1.0f, 0.37f })
    {
        const auto w = levels (b, kWhole);
        CHECK (maxAbsDiff (w.l, w0.l) <= 0.0);
        CHECK (maxAbsDiff (w.r, w0.r) <= 0.0);
    }
}

TEST_CASE ("M3 mixer: gain changes are smoothed (bounded step, no zipper), exact at the target, block-size independent", "[m3][mixer]")
{
    vs::PartMixer m;
    constexpr double fs = 48000.0;
    m.prepare (fs);
    m.setTargets (0.0f, -1.0f, 0.0f, false); // Lower hard left
    m.reset();
    std::vector<float> one (4800, 1.0f), l (4800), r (4800);

    m.setTargets (0.0f, 1.0f, 0.0f, false); // jump to hard right
    m.process (one.data(), nullptr, l.data(), r.data(), 4800);
    // A one-pole with tau = 10 ms: the largest per-sample step is |delta| (1 - e^(-1/(tau fs))).
    const double bound = 1.0 * (1.0 - std::exp (-1.0 / (vs::PartMixer::kSmoothingSeconds * fs))) * 1.0001;
    CHECK (std::abs (l[0] - 1.0f) < 0.01f); // starts where it was
    CHECK (maxStep (l, 0, 4800) <= bound);
    CHECK (maxStep (r, 0, 4800) <= bound);
    for (size_t i = 1; i < l.size(); ++i)
    {
        REQUIRE (l[i] <= l[i - 1]); // monotonic: no overshoot or steps
        REQUIRE (r[i] >= r[i - 1]);
    }
    // 10 tau: settled within 1e-4; after 40 tau exactly at the target (no denormal tail).
    CHECK (std::abs (m.gain (0, 0)) < 1.0e-4f);
    for (int k = 0; k < 3; ++k)
        m.process (one.data(), nullptr, l.data(), r.data(), 4800);
    CHECK (std::fpclassify (m.gain (0, 0)) == FP_ZERO);
    CHECK (std::abs (m.gain (0, 1) - 1.0f) <= 0.0f);

    // The same jump processed in blocks of 1, 7 and 4800 gives bit-identical gains (the settled
    // deviation is snapped per sample, not at the end of a call).
    auto curve = [] (int block) {
        vs::PartMixer mx;
        mx.prepare (fs);
        mx.setTargets (0.0f, -1.0f, 0.3f, false);
        mx.reset();
        mx.setTargets (0.7f, 1.0f, -0.4f, false);
        std::vector<float> in (24000, 1.0f), up (24000, 0.5f), ll (24000), rr (24000);
        for (int pos = 0; pos < 24000; pos += block)
        {
            const int n = std::min (block, 24000 - pos);
            mx.process (in.data() + pos, up.data() + pos, ll.data() + pos, rr.data() + pos, n);
        }
        return std::pair { ll, rr };
    };
    const auto a = curve (4800);
    for (const int block : { 1, 7 })
    {
        const auto b = curve (block);
        CHECK (maxAbsDiff (a.first, b.first) <= 0.0);
        CHECK (maxAbsDiff (a.second, b.second) <= 0.0);
    }
}

TEST_CASE ("M3 output: true stereo, and a mono bus gets (L + R) / 2", "[m3][mixer][mono]")
{
    VS_EVERY_RATE;
    auto run = [rate] (int channels) {
        ProcRig rig (rate, 512, channels);
        twoPatches (rig);
        rig.set (vs::ParamID::keyMode, kSplit);
        rig.set ("lower.mix.pan", -0.8f);
        rig.set ("upper.mix.pan", 0.5f);
        rig.set (vs::ParamID::mixBalance, 0.3f);
        rig.prepare();
        return rig.render (36000, { noteOn (10, 40), noteOn (20, 72), noteOff (20000, 40), noteOff (24000, 72) }, 480);
    };
    const auto stereo = run (2);
    const auto mono = run (1);
    std::vector<float> fold (stereo.l.size());
    for (size_t i = 0; i < fold.size(); ++i)
        fold[i] = 0.5f * (stereo.l[i] + stereo.r[i]);
    CHECK (rmsDb (stereo.l, 4800, 9600) > -40.0);
    CHECK (std::abs (rmsDb (stereo.l, 4800, 9600) - rmsDb (stereo.r, 4800, 9600)) > 1.0); // really stereo
    CHECK (maxAbsDiff (mono.l, fold) <= 1.0e-7);
}

// =================================================================================================
// Key-mode changes, block-size independence, when changes take effect, overflow, random streams
// =================================================================================================

TEST_CASE ("M3 key-mode change releases every voice of both parts, keeps the controllers, no click", "[m3][mode][pedal]")
{
    VS_EVERY_RATE;
    for (const float to : { kSplit, kWhole })
    {
        ProcRig rig (rate);
        twoPatches (rig);
        rig.set (vs::ParamID::keyMode, kDual);
        rig.set ("lower.env2.release", 0.4f); // 40 ms
        rig.set ("upper.env2.release", 0.4f);
        rig.set ("lower.env2.sustain", 1.0f);
        rig.set ("upper.env2.sustain", 1.0f);
        rig.prepare();
        // Pedal down, keys held; mode change at 1 s; keys released later (their note-offs are
        // forgotten); a new low note at 2 s is released while the pedal is still down.
        const std::vector<Midi> midi { cc (0, 64, 127), noteOn (100, 48), noteOn (100, 64), noteOff (60000, 48), noteOff (60000, 64),
                                       noteOn (96000, 40), noteOff (100000, 40) };
        const std::vector<ProcRig::Change> changes { { 48000, [to] (ProcRig& r) { r.set (vs::ParamID::keyMode, to); } } };
        const auto out = rig.render (48000 * 3, midi, 256, changes);

        INFO ("to mode " << to);
        CHECK (peak (out.l, 48000 + 24000, 96000) < 1.0e-6); // released at the change despite the pedal
        CHECK (peak (out.r, 48000 + 24000, 96000) < 1.0e-6);
        CHECK (rmsDb (out.l, 120000, 12000) > -40.0); // the pedal is still down: the new note is held
        const double before = maxStep (out.l, 40000, 48000), around = maxStep (out.l, 48000, 50000);
        INFO ("max step before " << before << ", around the change " << around);
        CHECK (around <= 1.1 * before);
    }
}

TEST_CASE ("M3 render does not depend on the host block size when every parameter change lands on the same sample (both parts, Vintage 1, voice reuse, mode changes)", "[m3][control]")
{
    // What this proves: given the same parameter change samples, everything else (routing, both
    // parts, mixer, latch, Upper stopping and resuming) is independent of the host block size.
    // ProcRig ends host blocks at every change, which a real host does not do; when a change takes
    // effect with real host blocks is pinned by the next test.
    //
    // Lower Poly (voices reused after idle), Upper Solo (one voice reused for every note), pans
    // and balance off-centre, full Vintage, notes at odd offsets, and key-mode / split changes:
    // Dual -> Split -> Whole -> Dual. Upper's release tail ends shortly before Dual comes back,
    // at a point where one block size has already stopped rendering Upper and the other has not
    // (checked), so Upper must come back the same either way.
    VS_EVERY_RATE;
    auto run = [rate] (int block, int backToDual, bool* upperRenderingAtReturn, int* upperSilentFrom) {
        ProcRig rig (rate);
        twoPatches (rig);
        rig.set (vs::ParamID::vintage, 1.0f);
        rig.set (vs::ParamID::keyMode, kDual);
        rig.set ("lower.mix.pan", -0.4f);
        rig.set ("upper.mix.pan", 0.6f);
        rig.set (vs::ParamID::mixBalance, -0.2f);
        rig.set ("lower.env1.decay", 0.3f);
        rig.set ("upper.env2.release", 0.3f); // 16 ms
        rig.set ("upper.porta.time", 0.2f);
        rig.prepare();
        std::vector<Midi> midi;
        for (int i = 0; i < 24; ++i)
        {
            const int at = 101 + i * 1999;
            const int note = 40 + (i * 7) % 36;
            midi.push_back (noteOn (at, note, 50 + i * 3));
            midi.push_back (noteOff (at + 1500 + (i % 5) * 701, note));
        }
        midi.push_back (bend (7001, 11000));
        midi.push_back (cc (13003, 1, 70));
        midi.push_back (noteOn (50003, 80)); // Upper (split 66), released by the change to Whole
        midi.push_back (noteOn (backToDual + 2001, 72));
        midi.push_back (noteOff (backToDual + 9001, 72));
        const std::vector<ProcRig::Change> changes {
            { 20000, [] (ProcRig& r) { r.set (vs::ParamID::keyMode, kSplit); r.set (vs::ParamID::keySplitPoint, 57.0f); } },
            { 30000, [] (ProcRig& r) { r.set (vs::ParamID::keySplitPoint, 66.0f); } },
            { 52000, [] (ProcRig& r) { r.set (vs::ParamID::keyMode, kWhole); } },
            { backToDual, [upperRenderingAtReturn] (ProcRig& r) {
                 if (upperRenderingAtReturn != nullptr)
                     *upperRenderingAtReturn = r.proc.isUpperRendering();
                 r.set (vs::ParamID::keyMode, kDual);
             } },
        };
        return rig.render (backToDual + 20000, midi, block, changes, [upperSilentFrom] (int end, ProcRig& r) {
            if (upperSilentFrom != nullptr && end > 52000 * r.scale && *upperSilentFrom < 0 && ! r.proc.getUpperPart().isSounding())
                *upperSilentFrom = end / r.scale;
        });
    };

    // Where does Upper's tail end? (Block size 1: exact.)
    int silentFrom = -1;
    run (1, 60000, nullptr, &silentFrom);
    REQUIRE (silentFrom > 52000);
    REQUIRE (silentFrom < 58000);

    // A return point at which 512 and 37 disagree about whether Upper is still rendered.
    int backToDual = -1;
    for (int r = silentFrom + 4; r < silentFrom + 1200 && backToDual < 0; r += 4)
    {
        bool a = false, b = false;
        run (512, r, &a, nullptr);
        run (37, r, &b, nullptr);
        if (a != b)
            backToDual = r;
    }
    INFO ("Upper silent from sample " << silentFrom << ", back to Dual at " << backToDual);
    REQUIRE (backToDual > 0);

    const auto ref = run (512, backToDual, nullptr, nullptr);
    for (const int block : { 500, 37, 1 })
    {
        const double db = differenceDb (ref, run (block, backToDual, nullptr, nullptr));
        INFO ("block " << block << " vs 512: " << db << " dB");
        WARN ("M3 block-size independence (both parts, Vintage 1), " << rate << " Hz, " << block << " vs 512: " << db << " dB");
        CHECK (db < -100.0);
    }
}

TEST_CASE ("M3 key.mode and key.split_point take effect at the first sample of the host block that delivers them (accepted, documented)", "[m3][control][mode]")
{
    // Hosts deliver parameter changes per block, so a key-mode change (which releases notes)
    // lands at a block-size-dependent time. Decision (M3 review): accept and document, no
    // quantisation. Pinned here: with 512-sample host blocks, a change the host delivers with the
    // block starting at 3 x 512 acts exactly like a change at that sample with 1-sample blocks,
    // and not like one at the sample inside the previous block where a user might have set it.
    VS_EVERY_RATE;
    auto run = [rate] (int block, int changeAt48, bool splitToo) {
        ProcRig rig (rate);
        twoPatches (rig);
        rig.set (vs::ParamID::keyMode, kDual);
        rig.prepare();
        const std::vector<Midi> midi { noteOn (100, 48), noteOn (100, 67), noteOn (3000, 62), noteOff (9000, 62) };
        const std::vector<ProcRig::Change> changes { { changeAt48, [splitToo] (ProcRig& r) {
            r.set (vs::ParamID::keyMode, kSplit);
            if (splitToo)
                r.set (vs::ParamID::keySplitPoint, 64.0f);
        } } };
        return rig.render (12000, midi, block, changes);
    };
    const int blockHost = 512;
    // Host block boundary 3 x 512 host samples, in 48 kHz units.
    const int boundary48 = 3 * blockHost / static_cast<int> (std::lround (rate / 48000.0));
    for (const bool splitToo : { false, true })
    {
        INFO ("split point changed too: " << splitToo);
        const auto delivered = run (blockHost, boundary48, splitToo);
        const auto exact = run (1, boundary48, splitToo);
        const auto intended = run (1, boundary48 - 100, splitToo); // what a sample-accurate change would give
        CHECK (differenceDb (exact, delivered) < -100.0);
        CHECK (differenceDb (exact, intended) > -60.0);
    }
}

TEST_CASE ("M3 event-buffer overflow reaches both parts (pedal down too): no hanging note", "[m3][events][pedal]")
{
    VS_EVERY_RATE;
    for (const bool pedal : { false, true })
    {
        ProcRig rig (rate);
        rig.set (vs::ParamID::keyMode, kDual);
        rig.set ("lower.env2.sustain", 1.0f);
        rig.set ("upper.env2.sustain", 1.0f);
        rig.set ("lower.mix.pan", -1.0f);
        rig.set ("upper.mix.pan", 1.0f);
        rig.prepare();
        std::vector<Midi> midi { noteOn (0, 48), noteOn (0, 67) };
        if (pedal)
            midi.push_back (cc (1, 64, 127));
        // One host block: 3000 bends (more than the 2048-event buffer), then the note-offs and
        // pedal up, which are dropped with the overflow. 24064 x scale is a 512-sample host block
        // start at every rate, and the events end 111 x scale <= 444 host samples later.
        constexpr int base = 24064;
        for (int i = 0; i < 3000; ++i)
            midi.push_back (bend (base + 1 + i % 100, 8192));
        midi.push_back (noteOff (base + 110, 48));
        midi.push_back (noteOff (base + 110, 67));
        if (pedal)
            midi.push_back (cc (base + 111, 64, 0));
        const auto out = rig.render (72000, midi, 512);
        INFO ("pedal " << pedal);
        CHECK (rmsDb (out.l, 12000, 4800) > -40.0);
        CHECK (rmsDb (out.r, 12000, 4800) > -40.0);
        CHECK (peak (out.l, 48000, 72000) < 1.0e-6);
        CHECK (peak (out.r, 48000, 72000) < 1.0e-6);
    }
}

TEST_CASE ("M3 random event streams (panic, overflow, mode/split/assign changes) never hang a note", "[m3][robust]")
{
    VS_EVERY_RATE;
    const std::uint32_t seeds = rate < 50000.0 ? 40u : 8u;
    for (std::uint32_t seed = 1; seed <= seeds; ++seed)
    {
        std::mt19937 rng (seed * 7919u + static_cast<std::uint32_t> (rate));
        auto uni = [&rng] (int a, int b) { return std::uniform_int_distribution<int> (a, b) (rng); };

        ProcRig rig (rate);
        rig.set ("lower.env2.sustain", 1.0f);
        rig.set ("upper.env2.sustain", 1.0f);
        rig.set ("lower.assign.mode", static_cast<float> (uni (0, 2)));
        rig.set ("upper.assign.mode", static_cast<float> (uni (0, 2)));
        rig.set ("lower.assign.unison_voices", static_cast<float> (uni (0, 3)));
        rig.set (vs::ParamID::keyMode, static_cast<float> (uni (0, 2)));
        rig.prepare();

        std::vector<Midi> midi;
        std::vector<int> down;
        std::vector<ProcRig::Change> changes;
        constexpr int length = 96000;
        for (int pos = 0; pos < length - 4000; pos += uni (10, 400))
        {
            const int k = uni (0, 26);
            if (k < 9)
            {
                const int note = uni (30, 90);
                midi.push_back (noteOn (pos, note));
                down.push_back (note);
            }
            else if (k < 15 && ! down.empty())
            {
                const auto idx = static_cast<size_t> (uni (0, static_cast<int> (down.size()) - 1));
                midi.push_back (noteOff (pos, down[idx]));
                down.erase (down.begin() + static_cast<std::ptrdiff_t> (idx));
            }
            else if (k == 15)
                midi.push_back (cc (pos, 64, uni (0, 1) * 127));
            else if (k == 16)
                changes.push_back ({ pos, [m = uni (0, 2)] (ProcRig& r) { r.set (vs::ParamID::keyMode, static_cast<float> (m)); } });
            else if (k == 17)
                changes.push_back ({ pos, [s = uni (40, 80)] (ProcRig& r) { r.set (vs::ParamID::keySplitPoint, static_cast<float> (s)); } });
            else if (k == 18)
                changes.push_back ({ pos, [m = uni (0, 2), p = uni (0, 1)] (ProcRig& r) {
                    r.set (p == 0 ? "lower.assign.mode" : "upper.assign.mode", static_cast<float> (m));
                } });
            else if (k == 19)
                midi.push_back (bend (pos, uni (0, 16383)));
            else if (k == 20 && uni (0, 3) == 0)
                midi.push_back (cc (pos, 120, 0)); // All Sound Off (keys stay physically down)
            else if (k == 21 && uni (0, 3) == 0)
                midi.push_back (cc (pos, 121, 0)); // Reset Controllers
            else if (k == 22 && uni (0, 3) == 0)
                midi.push_back (cc (pos, 123, 0)); // All Notes Off
            else if (k == 23 && uni (0, 8) == 0)
                for (int i = 0; i < 2100; ++i) // overflow: the rest of this block's events are dropped
                    midi.push_back (bend (pos, 8192));
        }
        // Let go of everything: every key still down, and the pedal.
        for (const int note : down)
            midi.push_back (noteOff (length - 3000, note));
        midi.push_back (cc (length - 3000, 64, 0));
        const int block = uni (0, 1) == 0 ? 512 : 61;
        const auto out = rig.render (length + 48000, midi, block, changes);

        INFO ("seed " << seed << ", block " << block);
        CHECK (peak (out.l, length + 24000, length + 48000) < 1.0e-6);
        CHECK (peak (out.r, length + 24000, length + 48000) < 1.0e-6);
        CHECK_FALSE (rig.proc.getLowerPart().isSounding());
        CHECK_FALSE (rig.proc.getUpperPart().isSounding());
    }
}

// =================================================================================================
// Parameters and state
// =================================================================================================

TEST_CASE ("M3 parameters: IDs, order, ranges, defaults and display text", "[m3][params]")
{
    juce::ScopedJuceInitialiser_GUI init;
    vs::VintageSynthProcessor proc;
    auto& state = proc.getState();
    const auto& params = proc.getParameters();
    REQUIRE (params.size() == 101);

    auto idAt = [&] (int i) { return dynamic_cast<juce::AudioProcessorParameterWithID*> (params[i])->paramID; };
    auto indexOf = [&] (const juce::String& id) {
        for (int i = 0; i < params.size(); ++i)
            if (idAt (i) == id)
                return i;
        return -1;
    };
    // Appended in the contract's order: per-part pan at the end of the float list, then the globals.
    CHECK (indexOf ("lower.mix.pan") == indexOf ("lower.mod.wheel_vco") + 1);
    CHECK (indexOf ("upper.mix.pan") == indexOf ("upper.mod.wheel_vco") + 1);
    CHECK (indexOf (vs::ParamID::keyMode) == indexOf (vs::ParamID::vintage) + 1);
    CHECK (indexOf (vs::ParamID::keySplitPoint) == indexOf (vs::ParamID::vintage) + 2);
    CHECK (indexOf (vs::ParamID::mixBalance) == indexOf (vs::ParamID::vintage) + 3);
    CHECK (indexOf (vs::ParamID::mixBalance) == params.size() - 1);

    for (const char* id : { "lower.mix.pan", "upper.mix.pan", vs::ParamID::mixBalance })
    {
        auto* p = dynamic_cast<juce::AudioParameterFloat*> (state.getParameter (id));
        REQUIRE (p != nullptr);
        CHECK (std::abs (p->range.start + 1.0f) < 1.0e-9f);
        CHECK (std::abs (p->range.end - 1.0f) < 1.0e-9f);
        CHECK (std::abs (p->get()) < 1.0e-9f);
    }
    CHECK (state.getParameter ("lower.mix.pan")->getName (64) == "Lower Pan");
    CHECK (state.getParameter ("upper.mix.pan")->getName (64) == "Upper Pan");

    auto* mode = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (vs::ParamID::keyMode));
    REQUIRE (mode != nullptr);
    CHECK (mode->choices == juce::StringArray { "Whole", "Dual", "Split" });
    CHECK (mode->getIndex() == 0);
    CHECK (mode->getName (64) == "Key Mode");

    auto* split = dynamic_cast<juce::AudioParameterInt*> (state.getParameter (vs::ParamID::keySplitPoint));
    REQUIRE (split != nullptr);
    CHECK (split->get() == 60);
    CHECK (split->getRange().getStart() == 0);
    CHECK (split->getRange().getEnd() == 127);
    CHECK (split->getCurrentValueAsText() == "C4");
    juce::RangedAudioParameter* sp = split; // getText / getValueForText are public in the base
    auto textOf = [sp] (int note) { return sp->getText (sp->convertTo0to1 (static_cast<float> (note)), 16); };
    CHECK (textOf (0) == "C-1");
    CHECK (textOf (1) == "C#-1");
    CHECK (textOf (61) == "C#4");
    CHECK (textOf (69) == "A4");
    CHECK (textOf (127) == "G9");
    auto noteFor = [sp] (const char* t) { return juce::roundToInt (sp->convertFrom0to1 (sp->getValueForText (t))); };
    CHECK (noteFor ("C4") == 60);
    CHECK (noteFor ("c#4") == 61);
    CHECK (noteFor ("Db4") == 61);
    CHECK (noteFor ("A-1") == 9);
    CHECK (noteFor ("Db-1") == 1); // flat with a negative octave
    CHECK (noteFor ("C#-1") == 1);
    CHECK (noteFor ("64") == 64);
    for (int n = 0; n < 128; ++n)
        CHECK (noteFor (textOf (n).toRawUTF8()) == n); // every name parses back

    // Unreadable or out-of-range text leaves the value unchanged (here 47, not the default 60).
    split->setValueNotifyingHost (split->convertTo0to1 (47.0f));
    for (const char* bad : { "C-", "C", "H4", "C#x", "", "Cb-1", "G#9", "200", "-3", "4-" })
    {
        INFO ("text '" << bad << "'");
        CHECK (noteFor (bad) == 47);
        CHECK (vs::noteFromName (bad) == -1);
    }

    auto* pan = state.getParameter ("lower.mix.pan");
    auto panText = [pan] (float v) { return pan->getText (pan->convertTo0to1 (v), 16); };
    CHECK (panText (0.0f) == "C");
    CHECK (panText (-0.5f) == "L 50");
    CHECK (panText (1.0f) == "R 100");
    auto panFor = [pan] (const char* t) { return pan->convertFrom0to1 (pan->getValueForText (t)); };
    CHECK (std::abs (panFor ("L 50") + 0.5f) < 1.0e-5f);
    CHECK (std::abs (panFor ("R 25") - 0.25f) < 1.0e-5f);
    CHECK (std::abs (panFor ("C")) < 1.0e-5f);
    auto* bal = state.getParameter (vs::ParamID::mixBalance);
    CHECK (bal->getText (bal->convertTo0to1 (0.0f), 16) == "Centre");
    CHECK (bal->getText (bal->convertTo0to1 (-0.3f), 16) == "Lower 30");
    CHECK (bal->getText (bal->convertTo0to1 (0.7f), 16) == "Upper 70");
    CHECK (std::abs (bal->convertFrom0to1 (bal->getValueForText ("Upper 70")) - 0.7f) < 1.0e-5f);
    CHECK (std::abs (bal->convertFrom0to1 (bal->getValueForText ("Lower 30")) + 0.3f) < 1.0e-5f);
}

TEST_CASE ("M3 parameters: state save and load round-trips the new parameters", "[m3][params][state]")
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::MemoryBlock saved;
    const std::pair<const char*, float> values[] { { "lower.mix.pan", -0.35f }, { "upper.mix.pan", 0.8f }, { vs::ParamID::keyMode, 2.0f },
                                                   { vs::ParamID::keySplitPoint, 47.0f }, { vs::ParamID::mixBalance, 0.45f } };
    {
        vs::VintageSynthProcessor proc;
        for (const auto& [id, v] : values)
        {
            auto* p = proc.getState().getParameter (id);
            p->setValueNotifyingHost (p->convertTo0to1 (v));
        }
        proc.getStateInformation (saved);
    }
    vs::VintageSynthProcessor restored;
    restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    for (const auto& [id, v] : values)
    {
        INFO (id);
        CHECK (std::abs (restored.getState().getRawParameterValue (id)->load() - v) < 1.0e-4f);
    }
}
