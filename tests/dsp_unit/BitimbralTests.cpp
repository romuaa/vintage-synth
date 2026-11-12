// M3 bi-timbral tests (docs/architecture.md 4.6 item 2), through the real processor where the
// contract is about the processor: Dual = sum of both parts, Split boundaries and split moves,
// pan and balance laws, Whole mode (Upper silent and not rendered, and clean when it comes back),
// stereo and mono output, key-mode changes, block-size independence with both parts at full
// Vintage, event overflow and random streams with both parts, and state of the new parameters.
// Oversampling by host rate (4.4) is in OversamplingTests.cpp.
#include <catch2/catch_test_macros.hpp>

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
    int pos;
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

/**
    The real processor, driven like a host. Every render goes through render(), which checks the
    output bounds (a render that overran its buffer corrupted the heap on Windows in M2a) and
    splits host blocks at parameter changes, so a change lands on the same sample for any block size.
*/
struct ProcRig
{
    juce::ScopedJuceInitialiser_GUI init;
    vs::VintageSynthProcessor proc;
    double fs;
    int maxBlock;
    int channels;
    bool offline;

    explicit ProcRig (double rate = 48000.0, int maxBlockSize = 512, int numChannels = 2, bool isOffline = false)
        : fs (rate), maxBlock (maxBlockSize), channels (numChannels), offline (isOffline)
    {
    }

    void set (const juce::String& id, float plain)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
    }

    float get (const juce::String& id) { return proc.getState().getRawParameterValue (id)->load(); }

    /** Applies the bus layout and prepares (call after setting the starting parameters). */
    void prepare()
    {
        juce::AudioProcessor::BusesLayout layout;
        layout.outputBuses.add (channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
        REQUIRE (proc.setBusesLayout (layout));
        proc.setNonRealtime (offline);
        proc.setPlayConfigDetails (0, channels, fs, maxBlock);
        proc.prepareToPlay (fs, maxBlock);
    }

    struct Change
    {
        int at;
        std::function<void (ProcRig&)> apply;
    };

    /** afterBlock (end, rig) is called after every host block (end = first sample not yet rendered). */
    Stereo render (int numSamples, std::vector<Midi> midi, int blockSize, std::vector<Change> changes = {},
                   const std::function<void (int, ProcRig&)>& afterBlock = {})
    {
        REQUIRE (blockSize >= 1);
        REQUIRE (blockSize <= maxBlock);
        std::stable_sort (midi.begin(), midi.end(), [] (const Midi& a, const Midi& b) { return a.pos < b.pos; });
        std::stable_sort (changes.begin(), changes.end(), [] (const Change& a, const Change& b) { return a.at < b.at; });
        Stereo out;
        out.l.assign (static_cast<size_t> (numSamples), 0.0f);
        out.r.assign (static_cast<size_t> (numSamples), 0.0f);
        juce::AudioBuffer<float> buffer (channels, blockSize);
        size_t nextMidi = 0, nextChange = 0;
        for (int pos = 0; pos < numSamples;)
        {
            while (nextChange < changes.size() && changes[nextChange].at <= pos)
                changes[nextChange++].apply (*this);
            int end = std::min (pos + blockSize, numSamples);
            if (nextChange < changes.size())
                end = std::min (end, changes[nextChange].at);
            const int len = end - pos;
            REQUIRE (len >= 1);
            REQUIRE (static_cast<size_t> (pos + len) <= out.l.size());

            juce::MidiBuffer mb;
            while (nextMidi < midi.size() && midi[nextMidi].pos < end)
            {
                mb.addEvent (midi[nextMidi].msg, std::max (0, midi[nextMidi].pos - pos));
                ++nextMidi;
            }
            buffer.setSize (channels, len, false, false, true);
            buffer.clear();
            proc.processBlock (buffer, mb);
            std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + len, out.l.begin() + pos);
            const int rch = channels > 1 ? 1 : 0;
            std::copy (buffer.getReadPointer (rch), buffer.getReadPointer (rch) + len, out.r.begin() + pos);
            pos = end;
            if (afterBlock)
                afterBlock (pos, *this);
        }
        return out;
    }

    float masterGain() { return juce::Decibels::decibelsToGain (get (vs::ParamID::masterVolume)); }
};

std::vector<vs::NoteEvent> toEvents (const std::vector<Midi>& midi)
{
    std::vector<vs::NoteEvent> v;
    for (const auto& m : midi)
    {
        vs::NoteEvent e {};
        if (vs::parseMidiMessage (m.msg.getRawData(), m.msg.getRawDataSize(), m.pos, e))
            v.push_back (e);
    }
    std::stable_sort (v.begin(), v.end(), [] (const auto& a, const auto& b) { return a.offset < b.offset; });
    return v;
}

/**
    Stand-alone reference Part fed exactly what the processor's part gets: the same parameters
    (read through the same binding), seed, oversampling factor, Vintage and events.
*/
struct RefPart
{
    vs::Part part;
    vs::PartParams params;
    float vintage = 0.0f;

    RefPart (ProcRig& rig, const char* prefix, std::uint32_t seed)
    {
        vs::PartParamBinding b;
        b.bind (rig.proc.getState(), prefix);
        b.read (params);
        vintage = rig.get (vs::ParamID::vintage);
        part.setParams (params);
        part.prepare (rig.fs, rig.maxBlock, rig.proc.getOversamplingFactor(), seed);
    }

    std::vector<float> render (int numSamples, const std::vector<vs::NoteEvent>& events, int blockSize)
    {
        std::vector<float> out (static_cast<size_t> (numSamples));
        std::vector<vs::NoteEvent> chunk;
        for (int start = 0; start < numSamples; start += blockSize)
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
            part.setParams (params);
            part.setVintage (vintage);
            part.render (out.data() + start, len, chunk);
        }
        return out;
    }
};

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
} // namespace

// =================================================================================================
// Dual, Split, Whole
// =================================================================================================

TEST_CASE ("M3 Dual: the output is the sum of both parts rendered separately (full Vintage)", "[m3][dual]")
{
    for (const int block : { 512, 97 })
    {
        ProcRig rig;
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
        const auto events = toEvents (midi);
        const auto lo = lower.render (length, events, block);
        const auto up = upper.render (length, events, block);
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
        WARN ("Dual = sum of parts, block " << block << ": max deviation L " << errL << ", R " << errR
                                             << " (peak " << peak (expected, 0, length) << ")");
        CHECK (errL <= 1.0e-6);
        CHECK (errR <= 1.0e-6);
    }
}

TEST_CASE ("M3 Split: notes below the split point play Lower, at or above it Upper; moving it with held keys", "[m3][split]")
{
    // Lower hard left and Upper hard right: L carries only Lower, R only Upper.
    ProcRig rig;
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
    ProcRig rig;
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
    for (int k = 1; k < 40; ++k)
        changes.push_back ({ k * 1000, [k] (ProcRig& r) { r.set ("upper.vcf.cutoff", static_cast<float> (k % 7) / 7.0f); } });
    const auto out = rig.render (48000, midi, 256, changes);

    CHECK (rig.proc.getUpperPart().getRenderedSamples() == 0);
    CHECK_FALSE (rig.proc.isUpperRendering());

    RefPart lower (rig, vs::kLowerPrefix, vs::VintageSynthProcessor::kLowerSeed);
    // The reference uses the same block structure as the processor saw (256, split at changes).
    std::vector<float> lo;
    {
        const auto ev = toEvents (midi);
        std::vector<float> all (48000);
        std::vector<vs::NoteEvent> chunk;
        int pos = 0;
        size_t c = 0;
        while (pos < 48000)
        {
            int end = std::min (pos + 256, 48000);
            while (c < changes.size() && changes[c].at <= pos)
                ++c;
            if (c < changes.size())
                end = std::min (end, changes[c].at);
            chunk.clear();
            for (auto e : ev)
                if (e.offset >= pos && e.offset < end)
                {
                    e.offset -= pos;
                    chunk.push_back (e);
                }
            lower.part.setParams (lower.params);
            lower.part.setVintage (lower.vintage);
            lower.part.render (all.data() + pos, end - pos, chunk);
            pos = end;
        }
        lo = std::move (all);
    }
    const float g = rig.masterGain();
    for (auto& s : lo)
        s *= g;
    REQUIRE (rmsDb (lo, 4000, 10000) > -40.0);
    CHECK (maxAbsDiff (out.l, lo) <= 1.0e-7); // Lower at unity, centred (balance ignored), Upper absent
    CHECK (maxAbsDiff (out.r, lo) <= 1.0e-7);
}

TEST_CASE ("M3 Whole -> Dual: Upper starts clean (parameters at target, fresh LFO), like a freshly prepared part", "[m3][whole]")
{
    ProcRig rig;
    twoPatches (rig);
    rig.set ("lower.mix.pan", -1.0f); // R carries Upper only
    rig.set ("upper.mix.pan", 1.0f);
    rig.prepare();

    // One second of Whole with Upper's parameters far away from where they end up.
    std::vector<ProcRig::Change> changes;
    changes.push_back ({ 1000, [] (ProcRig& r) { r.set ("upper.vcf.cutoff", 0.1f); r.set ("upper.vca.level", 0.2f); } });
    // Final values differ from those at prepare time, so a part that was never rendered would
    // still glide from stale smoother values.
    changes.push_back ({ 47000, [] (ProcRig& r) { r.set ("upper.vcf.cutoff", 0.62f); r.set ("upper.vca.level", 0.7f); } });
    changes.push_back ({ 48000, [] (ProcRig& r) { r.set (vs::ParamID::keyMode, kDual); } });
    const std::vector<Midi> midi { noteOn (100, 40), noteOff (20000, 40), noteOn (48000, 67), noteOff (70000, 67) };
    const auto out = rig.render (96000, midi, 256, changes);

    // Reference: a fresh Upper part with the final parameters, the note at its first sample.
    RefPart upper (rig, vs::kUpperPrefix, vs::VintageSynthProcessor::kUpperSeed);
    const auto ref = upper.render (48000, { { 0, vs::EventType::NoteOn, 67, 100.0f / 127.0f }, { 22000, vs::EventType::NoteOff, 67, 0.0f } }, 256);
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
    ProcRig rig;
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
    ProcRig rig;
    twoPatches (rig);
    rig.set (vs::ParamID::keyMode, kDual);
    rig.set ("upper.env2.release", 0.5f); // 100 ms
    rig.prepare();
    const std::vector<Midi> midi { noteOn (0, 52), noteOn (0, 59), noteOn (0, 64) };
    const std::vector<ProcRig::Change> changes { { 24000, [] (ProcRig& r) { r.set (vs::ParamID::keyMode, kWhole); } } };
    const auto out = rig.render (72000, midi, 256, changes);

    const auto rendered = rig.proc.getUpperPart().getRenderedSamples();
    CHECK_FALSE (rig.proc.isUpperRendering());
    CHECK (rendered > 24000);          // it kept rendering its release tail ...
    CHECK (rendered < 24000 + 24000);  // ... and stopped well before the end
    rig.render (4800, {}, 256);
    CHECK (rig.proc.getUpperPart().getRenderedSamples() == rendered); // nothing more in Whole

    // Click check: the largest step around the change stays within the signal's own steps.
    const double before = maxStep (out.l, 20000, 24000), around = maxStep (out.l, 24000, 26000);
    INFO ("max step before " << before << ", around the change " << around);
    CHECK (around <= 1.1 * before);
    CHECK (peak (out.l, 48000, 72000) < 1.0e-6); // all voices of both parts were released
}

// =================================================================================================
// Mixer: pan and balance laws, smoothing, stereo and mono output
// =================================================================================================

TEST_CASE ("M3 pan law: constant power (+-0.1 dB), unity in both channels at the centre, for both parts", "[m3][mixer]")
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

        // Unity at the centre: Whole/Split with one part centred equals that part alone; checked
        // against the Dual-sum test, which compares with stand-alone parts at the centre.
        for (const float pan : { -1.0f, -0.6f, -0.25f, 0.3f, 0.8f, 1.0f })
        {
            const auto [l, r] = levels (pan);
            const double theta = (static_cast<double> (pan) + 1.0) * juce::MathConstants<double>::pi / 4.0;
            const double expL = 20.0 * std::log10 (std::max (1.0e-12, std::sqrt (2.0) * std::cos (theta)));
            const double expR = 20.0 * std::log10 (std::max (1.0e-12, std::sqrt (2.0) * std::sin (theta)));
            const double power = 10.0 * std::log10 (std::pow (10.0, (l - cl) / 10.0) + std::pow (10.0, (r - cl) / 10.0));
            INFO ("pan " << pan << ": L " << l - cl << " dB (law " << expL << "), R " << r - cl << " dB (law " << expR
                         << "), total power " << power << " dB re centre channel (law 3.01)");
            CHECK (std::abs (power - 10.0 * std::log10 (2.0)) <= 0.1);
            if (pan > -1.0f)
                CHECK (std::abs ((r - cl) - expR) <= 0.1);
            else
                CHECK (r < -200.0); // exactly silent on the far side
            if (pan < 1.0f)
                CHECK (std::abs ((l - cl) - expL) <= 0.1);
            else
                CHECK (l < -200.0);
        }
    }
}

TEST_CASE ("M3 balance law: constant power (+-0.1 dB), both parts at unity at 0, no effect in Whole", "[m3][mixer]")
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
        const auto law = vs::map::balanceGains (b);
        const double theta = (static_cast<double> (b) + 1.0) * juce::MathConstants<double>::pi / 4.0;
        const double expLo = 20.0 * std::log10 (std::max (1.0e-12, std::sqrt (2.0) * std::cos (theta)));
        const double expUp = 20.0 * std::log10 (std::max (1.0e-12, std::sqrt (2.0) * std::sin (theta)));
        const double power = 10.0 * std::log10 (std::pow (10.0, dl / 10.0) + std::pow (10.0, dr / 10.0));
        INFO ("balance " << b << ": Lower " << dl << " dB (law " << expLo << "), Upper " << dr << " dB (law " << expUp
                         << "), power " << power << " dB (law 3.01); map gains " << law.lower << " / " << law.upper);
        CHECK (std::abs (power - 10.0 * std::log10 (2.0)) <= 0.1);
        if (b > -1.0f)
            CHECK (std::abs (dr - expUp) <= 0.1);
        else
            CHECK (dr < -200.0);
        if (b < 1.0f)
            CHECK (std::abs (dl - expLo) <= 0.1);
        else
            CHECK (dl < -200.0);
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

TEST_CASE ("M3 mixer: gain changes are smoothed (bounded step, no zipper), exact at the target", "[m3][mixer]")
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
    const double bound = std::sqrt (2.0) * (1.0 - std::exp (-1.0 / (vs::PartMixer::kSmoothingSeconds * fs))) * 1.0001;
    CHECK (std::abs (l[0] - std::sqrt (2.0f)) < 0.01f); // starts where it was
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
    CHECK (std::abs (m.gain (0, 1) - std::sqrt (2.0f)) < 1.0e-6f);
}

TEST_CASE ("M3 output: true stereo, and a mono bus gets (L + R) / 2", "[m3][mixer][mono]")
{
    auto run = [] (int channels) {
        ProcRig rig (48000.0, 512, channels);
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
// Key-mode changes, block-size independence, overflow, random streams
// =================================================================================================

TEST_CASE ("M3 key-mode change releases every voice of both parts, keeps the controllers, no click", "[m3][mode][pedal]")
{
    for (const float to : { kSplit, kWhole })
    {
        ProcRig rig;
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

TEST_CASE ("M3 render does not depend on the host block size with both parts sounding (Vintage 1, voice reuse, mode changes)", "[m3][control]")
{
    // Lower Poly (voices reused after idle), Upper Solo (one voice reused for every note), pans
    // and balance off-centre, full Vintage, notes at odd offsets, and key-mode / split changes
    // at fixed samples: Dual -> Split -> Whole -> Dual. Upper's release tail ends shortly before
    // Dual comes back, at a point where one block size has already stopped rendering Upper and
    // the other has not (checked), so Upper must come back the same either way.
    auto run = [] (int block, int backToDual, bool* upperRenderingAtReturn, int* upperSilentFrom) {
        ProcRig rig;
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
            if (upperSilentFrom != nullptr && end > 52000 && *upperSilentFrom < 0 && ! r.proc.getUpperPart().isSounding())
                *upperSilentFrom = end;
        });
    };

    // Where does Upper's tail end? (Block size 1: exact.)
    int silentFrom = -1;
    run (1, 60000, nullptr, &silentFrom);
    REQUIRE (silentFrom > 52000);
    REQUIRE (silentFrom < 58000);

    // A return point at which 512 and 37 disagree about whether Upper is still rendered.
    int backToDual = -1;
    for (int r = silentFrom + 8; r < silentFrom + 1200 && backToDual < 0; r += 8)
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
    for (const int block : { 500, 37 })
    {
        const double db = differenceDb (ref, run (block, backToDual, nullptr, nullptr));
        INFO ("block " << block << " vs 512: " << db << " dB");
        WARN ("M3 block-size independence (both parts, Vintage 1), " << block << " vs 512: " << db << " dB");
        CHECK (db < -100.0);
    }
}

TEST_CASE ("M3 event-buffer overflow reaches both parts (pedal down too): no hanging note", "[m3][events][pedal]")
{
    for (const bool pedal : { false, true })
    {
        ProcRig rig;
        rig.set (vs::ParamID::keyMode, kDual);
        rig.set ("lower.env2.sustain", 1.0f);
        rig.set ("upper.env2.sustain", 1.0f);
        rig.set ("lower.mix.pan", -1.0f);
        rig.set ("upper.mix.pan", 1.0f);
        rig.prepare();
        std::vector<Midi> midi { noteOn (0, 48), noteOn (0, 67) };
        if (pedal)
            midi.push_back (cc (1, 64, 127));
        // One block: 3000 bends (more than the 2048-event buffer), then the note-offs and pedal
        // up, which are dropped with the overflow.
        for (int i = 0; i < 3000; ++i)
            midi.push_back (bend (24064 + 1 + i % 400, 8192)); // all inside the block 24064 .. 24575
        midi.push_back (noteOff (24500, 48));
        midi.push_back (noteOff (24500, 67));
        if (pedal)
            midi.push_back (cc (24501, 64, 0));
        const auto out = rig.render (72000, midi, 512);
        INFO ("pedal " << pedal);
        CHECK (rmsDb (out.l, 12000, 4800) > -40.0);
        CHECK (rmsDb (out.r, 12000, 4800) > -40.0);
        CHECK (peak (out.l, 48000, 72000) < 1.0e-6);
        CHECK (peak (out.r, 48000, 72000) < 1.0e-6);
    }
}

TEST_CASE ("M3 random event streams with key-mode and split changes never hang a note", "[m3][robust]")
{
    for (std::uint32_t seed = 1; seed <= 12; ++seed)
    {
        std::mt19937 rng (seed);
        auto uni = [&rng] (int a, int b) { return std::uniform_int_distribution<int> (a, b) (rng); };

        ProcRig rig;
        rig.set ("lower.env2.sustain", 1.0f);
        rig.set ("upper.env2.sustain", 1.0f);
        rig.set ("upper.assign.mode", static_cast<float> (uni (0, 2)));
        rig.set (vs::ParamID::keyMode, static_cast<float> (uni (0, 2)));
        rig.prepare();

        std::vector<Midi> midi;
        std::vector<int> down;
        std::vector<ProcRig::Change> changes;
        constexpr int length = 96000;
        for (int pos = 0; pos < length - 4000; pos += uni (10, 400))
        {
            const int k = uni (0, 20);
            if (k < 9)
            {
                const int note = uni (30, 90);
                midi.push_back (noteOn (pos, note));
                down.push_back (note);
            }
            else if (k < 16 && ! down.empty())
            {
                const auto idx = static_cast<size_t> (uni (0, static_cast<int> (down.size()) - 1));
                midi.push_back (noteOff (pos, down[idx]));
                down.erase (down.begin() + static_cast<std::ptrdiff_t> (idx));
            }
            else if (k == 16)
                midi.push_back (cc (pos, 64, uni (0, 1) * 127));
            else if (k == 17)
                changes.push_back ({ pos, [m = uni (0, 2)] (ProcRig& r) { r.set (vs::ParamID::keyMode, static_cast<float> (m)); } });
            else if (k == 18)
                changes.push_back ({ pos, [s = uni (40, 80)] (ProcRig& r) { r.set (vs::ParamID::keySplitPoint, static_cast<float> (s)); } });
            else if (k == 19)
                midi.push_back (bend (pos, uni (0, 16383)));
        }
        // Let go of everything: every key still down, and the pedal.
        for (const int note : down)
            midi.push_back (noteOff (length - 3000, note));
        midi.push_back (cc (length - 3000, 64, 0));
        const auto out = rig.render (length + 48000, midi, uni (0, 1) == 0 ? 512 : 61, changes);

        INFO ("seed " << seed);
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
    CHECK (textOf (61) == "C#4");
    CHECK (textOf (69) == "A4");
    CHECK (textOf (127) == "G9");
    auto noteFor = [sp] (const char* t) { return juce::roundToInt (sp->convertFrom0to1 (sp->getValueForText (t))); };
    CHECK (noteFor ("C4") == 60);
    CHECK (noteFor ("c#4") == 61);
    CHECK (noteFor ("Db4") == 61);
    CHECK (noteFor ("A-1") == 9);
    CHECK (noteFor ("64") == 64);
    for (int n = 0; n < 128; ++n)
        CHECK (noteFor (textOf (n).toRawUTF8()) == n); // every name parses back

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
