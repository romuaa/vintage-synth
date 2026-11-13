// Wave 3 follow-up (independent review): host bypass, stepped integer parameters, non-finite
// parameter values, the arp/seq selector in Whole mode, the player's pedal across a reset, the
// processor-level fuzz (ported from the review), no allocation on every path, and a gain report.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "IntegrationRig.h"
#include "vintage/params/PartMappings.h"
#include "vintage/params/ParameterLayout.h"

#if defined(__APPLE__)
 #include "../seq_unit/AllocationCounter.h"
#endif

#include <chrono>
#include <cstdio>
#include <limits>
#include <random>
#include <set>

using namespace vs::itest;

namespace
{
bool allFinite (const Stereo& s)
{
    for (size_t i = 0; i < s.l.size(); ++i)
        if (! std::isfinite (s.l[i]) || ! std::isfinite (s.r[i]))
            return false;
    return true;
}

double peakDb (const Stereo& s, size_t from, size_t to)
{
    return toDb (std::max (peakAbs (s.l, from, to), peakAbs (s.r, from, to)));
}

bool nothingSounds (Rig& rig)
{
    return ! rig.proc.getLowerPart().isSounding() && ! rig.proc.getUpperPart().isSounding()
           && ! rig.proc.getArpSeq (0).isSounding() && ! rig.proc.getArpSeq (1).isSounding();
}

std::vector<Rig::Change> bypassSpan (int from, int to)
{
    return { { from, [] (Rig& r) { r.bypass = true; } }, { to, [] (Rig& r) { r.bypass = false; } } };
}
} // namespace

// =================================================================================================
// Host bypass: a clean stop
// =================================================================================================

TEST_CASE ("Bypass: a key released while the host bypasses leaves nothing hanging (arp on and off)", "[wave3][bypass]")
{
    const double fs = GENERATE (48000.0, 192000.0);
    for (int arpOn = 0; arpOn < 2; ++arpOn)
    {
        INFO ("rate " << fs << ", arp " << arpOn);
        Rig rig (fs, 512);
        rig.set ("arp.mode", static_cast<float> (arpOn));
        rig.set ("lower.env2.release", 0.1f);
        rig.prepare();
        rig.render (rig.samples (0.3), { noteOn (100, 60) }, 512);
        REQUIRE (rig.proc.getLowerPart().isSounding());
        // The note-off arrives inside a bypassed block (the review's repro).
        const int at = static_cast<int> (rig.now);
        const auto during = rig.render (rig.samples (0.2), { noteOff (at + 10, 60) }, 512, bypassSpan (at, at + rig.samples (0.2)));
        CHECK (rig.proc.isBypassed());
        const auto after = rig.render (rig.samples (2.0), {}, 512);
        CHECK (nothingSounds (rig));
        CHECK (rig.noteOnPositions (0, at).empty());
        CHECK (peakDb (after, 0, after.l.size()) < -200.0);
        // The bypassed output fades to silence within kBypassFadeSeconds and stays there.
        CHECK (peakDb (during, static_cast<size_t> (rig.samples (vs::VintageSynthProcessor::kBypassFadeSeconds)), during.l.size()) < -200.0);
        CHECK (rig.allReleased (0));
    }
}

TEST_CASE ("Bypass: the fade-out is the normal output times a ramp, then silence", "[wave3][bypass]")
{
    // Entering bypass releases every note (as a note-off would) and fades the output out over
    // kBypassFadeSeconds. Reference: the key released at that sample, without bypass.
    auto render = [] (bool withBypass) {
        Rig rig (48000.0, 512);
        rig.set ("fx.reverb.level", 0.4f);
        rig.prepare();
        std::vector<Rig::Change> ch;
        std::vector<Midi> midi { noteOn (0, 57, 120) }; // with bypass: the key stays down
        if (withBypass)
            ch = bypassSpan (24000, 40000);
        else
            midi.push_back (noteOff (24000, 57));
        return rig.render (48000, midi, 100, ch);
    };
    const auto normal = render (false), bypassed = render (true);
    const int fade = static_cast<int> (std::lround (vs::VintageSynthProcessor::kBypassFadeSeconds * 48000.0));
    REQUIRE (peakAbs (normal.l, 23900, 24000) > 0.05);
    // Before the bypass: identical.
    for (size_t i = 0; i < 24000; ++i)
        REQUIRE (std::memcmp (&normal.l[i], &bypassed.l[i], sizeof (float)) == 0);
    // The fade: exactly the normal output times (1 - i / fade), split across 100-sample blocks.
    double worst = 0.0, maxStep = 0.0;
    for (int i = 0; i < fade; ++i)
    {
        const auto k = static_cast<size_t> (24000 + i);
        const float want = normal.l[k] * (static_cast<float> (fade - i) * (1.0f / static_cast<float> (fade)));
        worst = std::max (worst, static_cast<double> (std::abs (bypassed.l[k] - want)));
        maxStep = std::max (maxStep, static_cast<double> (std::abs (bypassed.l[k] - bypassed.l[k - 1])));
    }
    CHECK (worst < 1.0e-7);
    // No click at the edge: the steps inside the fade are no larger than the signal's own.
    double ownStep = 0.0;
    for (size_t k = 23000; k < 24000; ++k)
        ownStep = std::max (ownStep, static_cast<double> (std::abs (normal.l[k] - normal.l[k - 1])));
    CHECK (maxStep <= ownStep * 1.05);
    CHECK (peakAbs (bypassed.l, static_cast<size_t> (24000 + fade), 40000) <= 0.0);
    // After the bypass: the key still held is silent until pressed again (clean start).
    CHECK (peakAbs (bypassed.l, 40000, 48000) <= 0.0);
}

TEST_CASE ("Bypass: a Hold-latched arpeggio, effect tails and MIDI during bypass", "[wave3][bypass]")
{
    SECTION ("Hold latched: after bypass the pattern is gone")
    {
        Rig rig (48000.0, 512);
        rig.set ("arp.mode", 1);
        rig.set ("arp.hold", 1);
        rig.prepare();
        rig.render (24000, { noteOn (100, 60), noteOn (100, 64), noteOff (3000, 60), noteOff (3000, 64) }, 512);
        REQUIRE (rig.noteOnPositions (0, 6000, 24000).size() >= 2); // latched and playing
        rig.render (48000, {}, 512, bypassSpan (30000, 36000));
        CHECK (rig.noteOnPositions (0, 30000).empty());
        CHECK (nothingSounds (rig));
        // A new chord starts a new pattern normally.
        rig.render (24000, { noteOn (72100, 67) }, 512);
        CHECK (rig.noteOnPositions (0, 72000).size() >= 3);
    }
    SECTION ("effect tail: no stale echoes after un-bypass")
    {
        Rig rig (48000.0, 512);
        rig.set ("fx.delay.level", 0.8f);
        rig.set ("fx.delay.feedback", 0.7f);
        rig.set ("fx.delay.time", vs::map::delayTimeFromSec (0.3f));
        rig.set ("fx.reverb.level", 0.5f);
        rig.prepare();
        const auto out = rig.render (4 * 48000, { noteOn (0, 52, 120), noteOff (4800, 52) }, 256, bypassSpan (30000, 31000));
        REQUIRE (peakDb (out, 20000, 30000) > -40.0); // echoes were sounding
        CHECK (peakDb (out, 31000, out.l.size()) < -200.0);
    }
    SECTION ("MIDI during bypass is ignored: nothing starts or accumulates")
    {
        Rig rig (48000.0, 512);
        rig.set ("arp.mode", 1);
        rig.prepare();
        rig.render (48000, { noteOn (1000, 60), pedal (2000, true), noteOn (3000, 64), cc (3500, 123, 0) }, 512, bypassSpan (0, 4096));
        CHECK (rig.noteOnPositions (0).empty());
        CHECK (nothingSounds (rig));
        CHECK_FALSE (rig.proc.getLowerPart().isSustainDown());
    }
}

TEST_CASE ("Bypass toggled every block, blocks above the prepared size, mono bus, latency", "[wave3][bypass]")
{
    const int prepared = GENERATE (64, 256);
    const int block = GENERATE (1, 100, 700);
    const int channels = GENERATE (1, 2);
    INFO ("prepared " << prepared << ", block " << block << ", channels " << channels);
    Rig rig (48000.0, prepared, channels);
    rig.set ("arp.mode", 1);
    rig.set ("fx.delay.level", 0.4f);
    rig.prepare();
    const int latency = rig.proc.getLatencySamples();
    std::vector<Midi> midi;
    std::set<int> down;
    std::mt19937 rng (static_cast<unsigned> (prepared * 7 + block));
    for (int i = 0; i < 80; ++i)
    {
        const int at = std::uniform_int_distribution<int> (0, 40000) (rng);
        const int n = 48 + std::uniform_int_distribution<int> (0, 12) (rng);
        midi.push_back (i % 2 == 0 ? noteOn (at, n) : noteOff (at, n));
    }
    for (int n = 0; n < 128; ++n)
        midi.push_back (noteOff (44000, n));
    int toggles = 0;
    const auto out = rig.render (60000, midi, block, {}, [&] (Rig& r) {
        if (r.now < 44000)
        {
            r.bypass = ! r.bypass; // every block
            ++toggles;
        }
        else
            r.bypass = false;
    });
    CHECK (toggles > 10);
    CHECK (rig.proc.getLatencySamples() == latency);
    CHECK (allFinite (out));
    CHECK (nothingSounds (rig));
    CHECK (peakDb (out, 50000, 60000) < -150.0);
}

// =================================================================================================
// Parameters: stepped integers, non-finite values
// =================================================================================================

TEST_CASE ("Every integer and choice parameter is discrete with the right number of steps", "[wave3][params]")
{
    juce::ScopedJuceInitialiser_GUI init;
    vs::VintageSynthProcessor proc;
    int checked = 0;
    for (auto* p : proc.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        REQUIRE (ranged != nullptr);
        INFO (ranged->paramID);
        if (auto* i = dynamic_cast<juce::AudioParameterInt*> (p))
        {
            CHECK (p->isDiscrete());
            CHECK (p->getNumSteps() == i->getRange().getLength() + 1);
            ++checked;
        }
        else if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (p))
        {
            CHECK (p->isDiscrete());
            CHECK (p->getNumSteps() == c->choices.size());
            ++checked;
        }
    }
    // 2 x 19 part choices, key mode, split point, 2 effect choices, 5 arp choices, octaves and
    // length, 16 step notes and 16 step types.
    CHECK (checked == 38 + 1 + 1 + 2 + 5 + 2 + 32);
    auto* split = dynamic_cast<juce::AudioParameterInt*> (proc.getState().getParameter (vs::ParamID::keySplitPoint));
    REQUIRE (split != nullptr);
    CHECK (static_cast<juce::AudioProcessorParameter*> (split)->getNumSteps() == 128);
}

TEST_CASE ("Non-finite parameter values: the engine gets the parameter's default, the guard never fires", "[wave3][params][nan]")
{
    const float bad = GENERATE (std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity());
    // One parameter of every binding, and the processor's own globals.
    const char* ids[] { "lower.vcf.cutoff", "upper.vco1.waveform", "upper.env2.release", "fx.chorus.mode", "fx.delay.time",
                        "fx.reverb.decay", "fx.delay.feedback", "arp.swing", "arp.rate", "arp.tempo", "seq.step02.note",
                        "seq.length", "master.volume", "global.vintage", "key.mode", "key.split_point", "mix.balance" };
    for (const char* id : ids)
    {
        INFO (id << " = " << bad);
        auto render = [&] (bool poison) {
            Rig rig (48000.0, 256);
            rig.set ("key.mode", 1);
            rig.set ("arp.mode", 1);
            rig.set ("arp.target", 0); // the arpeggio on Lower; Upper plays the chord straight
            rig.set ("fx.chorus.mode", 1);
            rig.set ("fx.delay.level", 0.3f);
            rig.set ("fx.reverb.level", 0.3f);
            rig.prepare();
            auto* raw = rig.proc.getState().getRawParameterValue (id);
            auto* param = dynamic_cast<juce::RangedAudioParameter*> (rig.proc.getState().getParameter (id));
            const float def = param->convertFrom0to1 (param->getDefaultValue());
            // Half-way through, the parameter becomes `bad` (poisoned) or its default (reference).
            std::vector<Rig::Change> ch { { 12000, [raw, def, poison, this_bad = bad] (Rig&) { raw->store (poison ? this_bad : def); } } };
            auto out = rig.render (36000, { noteOn (10, 60), noteOn (10, 64), noteOn (10, 67) }, 256, ch);
            return std::make_pair (std::move (out), rig.proc.getGuardTrips());
        };
        const auto [ref, refTrips] = render (false);
        const auto [got, trips] = render (true);
        CHECK (trips == 0);
        CHECK (refTrips == 0);
        CHECK (allFinite (got));
        CHECK (bitIdentical (got, ref));
        CHECK (peakDb (got, 30000, 36000) > -60.0); // still playing
    }
}

TEST_CASE ("Non-finite value written before prepare: the part starts with the default", "[wave3][params][nan]")
{
    auto render = [] (bool poison) {
        Rig rig (48000.0, 512);
        if (poison)
        {
            rig.proc.getState().getRawParameterValue ("lower.vcf.cutoff")->store (std::numeric_limits<float>::quiet_NaN());
            rig.proc.getState().getRawParameterValue ("key.split_point")->store (std::numeric_limits<float>::infinity());
        }
        rig.prepare();
        auto out = rig.render (12000, { noteOn (0, 48) }, 512);
        return std::make_pair (std::move (out), rig.proc.getGuardTrips());
    };
    const auto [ref, a] = render (false);
    const auto [got, b] = render (true);
    CHECK (a == 0);
    CHECK (b == 0);
    CHECK (bitIdentical (got, ref));
}

// =================================================================================================
// Arp/seq selector in Whole mode; the player's pedal across a reset
// =================================================================================================

TEST_CASE ("Whole mode: the arp/seq plays Lower whatever arp.target says; target and mode hand-over", "[wave3][arp][target]")
{
    for (int target = 0; target < 3; ++target)
    {
        INFO ("target " << target);
        Rig rig (48000.0, 512);
        rig.set ("arp.mode", 1);
        rig.set ("arp.target", static_cast<float> (target));
        rig.prepare();
        rig.render (36000, { noteOn (500, 60), noteOn (500, 64), noteOff (30000, 60), noteOff (30000, 64) }, 512);
        const auto on = rig.noteOnPositions (0);
        CHECK (on.size() == 5); // internal clock from 500, every 6000 until the keys go up
        CHECK (rig.noteOnPositions (1).empty());
        CHECK (nothingSounds (rig));
    }
    SECTION ("Dual, target Upper -> Lower with keys held")
    {
        Rig rig (48000.0, 512);
        rig.set ("arp.mode", 1);
        rig.set ("arp.target", 1);
        rig.set ("key.mode", 1);
        rig.prepare();
        rig.render (48000, { noteOn (100, 60), noteOn (100, 64), noteOff (40000, 60), noteOff (40000, 64) }, 512,
                    { { 24000, [] (Rig& r) { r.set ("arp.target", 0); } } });
        // Before: Upper arpeggiated, Lower plays the chord; after: the reverse hand-over.
        CHECK (rig.noteOnPositions (1, 0, 24000).size() >= 3);
        CHECK (rig.noteOnPositions (0, 0, 24000).size() == 2);
        CHECK (rig.noteOnPositions (1, 24000).empty());
        CHECK (rig.noteOnPositions (0, 24000, 40000).size() >= 2);
        CHECK (rig.allReleased (0));
        CHECK (rig.allReleased (1));
        rig.render (24000, {}, 512);
        CHECK (nothingSounds (rig));
    }
    SECTION ("Dual target Upper -> Whole: the arpeggio moves to Lower with the next key")
    {
        Rig rig (48000.0, 512);
        rig.set ("arp.mode", 1);
        rig.set ("arp.target", 1);
        rig.set ("key.mode", 1);
        rig.prepare();
        rig.render (48000, { noteOn (100, 60), noteOn (30000, 67), noteOff (44000, 67) }, 512,
                    { { 24000, [] (Rig& r) { r.set ("key.mode", 0); } } });
        CHECK (rig.noteOnPositions (1, 0, 24000).size() >= 3);
        CHECK (rig.noteOnPositions (1, 24000).empty());
        CHECK (rig.noteOnPositions (0, 30000, 44000).size() >= 2); // arpeggiated on Lower now
        rig.render (24000, {}, 512);
        CHECK (nothingSounds (rig));
    }
}

TEST_CASE ("The player's pedal is forgotten by reset and the guard like everything else", "[wave3][arp][pedal]")
{
    // Pedal down with the arp Off (passed to the part); then a host reset or the guard; a key-mode
    // change (which resyncs the modules' pedal from the player's pedal); a key pressed and
    // released; arp on. The processor forgot the pedal at the reset, so the released key is not
    // retained and the arpeggio has nothing to play. If the reset left the player's pedal
    // remembered, the key would stay in the pattern and play on with no key or pedal held.
    for (int how = 0; how < 2; ++how)
    {
        INFO ((how == 0 ? "host reset" : "guard"));
        Rig rig (48000.0, 512);
        rig.prepare();
        rig.render (4800, { pedal (100, true), noteOn (200, 50), noteOff (1000, 50) }, 512);
        REQUIRE (rig.proc.getLowerPart().isSustainDown());
        if (how == 0)
            rig.hostReset();
        else
        {
            rig.proc.forceNonFiniteForTesting();
            rig.render (512, {}, 512);
            REQUIRE (rig.proc.getGuardTrips() == 1);
        }
        const int t = static_cast<int> (rig.now);
        rig.render (48000, { noteOn (t + 3000, 60), noteOff (t + 4000, 60) }, 512,
                    { { t + 1000, [] (Rig& r) { r.set ("key.mode", 1); } }, { t + 8000, [] (Rig& r) { r.set ("arp.mode", 1); } } });
        CHECK (rig.noteOnPositions (0, t + 8000).empty());
        CHECK (rig.noteOnPositions (1, t + 8000).empty());
        CHECK (nothingSounds (rig));
    }
}

// =================================================================================================
// Processor fuzz (ported from the independent review)
// =================================================================================================

namespace
{
struct FuzzScript
{
    std::vector<Midi> midi;
    std::vector<Rig::Change> changes;
    int total = 0, endAt = 0;
};

void fuzzSetup (Rig& r)
{
    r.set ("lower.env2.release", 0.25f);
    r.set ("upper.env2.release", 0.25f);
    r.set ("arp.mode", 1);
    r.set ("fx.chorus.mode", 1);
    r.set ("fx.delay.level", 0.3f);
    r.set ("fx.reverb.level", 0.3f);
    r.set ("fx.reverb.decay", vs::map::reverbDecayFromSec (0.6f));
    r.set ("fx.delay.feedback", 0.3f);
    r.set ("fx.delay.time", vs::map::delayTimeFromSec (0.15f));
}

/** Random MIDI (keys only pressed when up), panics, overflow bursts (optional), controllers; random changes
    to every arp/seq, effect (short tails) and key-mode parameter; transport play (also at negative
    positions), stop, locate, tempo and missing fields; host reset(); non-finite parameter writes;
    bypass spans. Then everything goes up and the arp/seq loses Hold. */
FuzzScript makeFuzz (const Rig& rig, unsigned seed, bool withOverflow)
{
    std::mt19937 rng (seed);
    auto pick = [&] (int n) { return std::uniform_int_distribution<int> (0, n - 1) (rng); };
    FuzzScript sc;
    const int busy = rig.samples (2.0);
    sc.endAt = busy + rig.samples (0.1);
    sc.total = sc.endAt + rig.samples (2.5); // the effect tails (reverb 0.6 s) are over by then

    // MIDI in time order, so "only press keys that are up" holds.
    std::vector<int> times;
    for (int i = 0; i < 220; ++i)
        times.push_back (pick (busy));
    std::sort (times.begin(), times.end());
    bool down[128] {};
    for (const int at : times)
    {
        const int r = pick (42);
        if (r < 16)
        {
            const int n = 30 + pick (60);
            if (! down[n])
            {
                down[n] = true;
                sc.midi.push_back (noteOn (at, n, 1 + pick (127)));
            }
        }
        else if (r < 29)
        {
            const int n = 30 + pick (60);
            down[n] = false;
            sc.midi.push_back (noteOff (at, n));
        }
        else if (r < 33)
            sc.midi.push_back (pedal (at, pick (2) == 1));
        else if (r == 33)
            sc.midi.push_back (cc (at, 123, 0));
        else if (r == 34)
            sc.midi.push_back (cc (at, 120, 0));
        else if (r == 35)
            sc.midi.push_back (cc (at, 121, 0));
        else if (r == 36)
            sc.midi.push_back ({ at, juce::MidiMessage::pitchWheel (1, pick (16384)) });
        else if (r == 37)
            sc.midi.push_back (cc (at, 1, pick (128)));
        else if (r == 38 && withOverflow)
        {
            // Overflow burst: more events at one position than the parser holds. It drops the
            // rest and appends All Notes Off, so afterwards every key counts as up.
            for (int k = 0; k < 2100; ++k)
                sc.midi.push_back (k % 2 == 0 ? noteOn (at, 100 + (k / 2) % 20, 90) : noteOff (at, 100 + (k / 2) % 20));
            std::fill (std::begin (down), std::end (down), false);
        }
    }
    const char* nanTargets[] { "lower.vcf.cutoff", "upper.vcf.resonance", "arp.swing", "arp.rate", "fx.chorus.mix",
                               "key.split_point", "global.vintage", "seq.step05.note" };
    for (int i = 0; i < 70; ++i)
    {
        const int at = pick (busy);
        const int what = pick (30);
        const int v = pick (64);
        if (what == 29)
        {
            sc.changes.push_back ({ at, [] (Rig& r) { r.bypass = true; } });
            sc.changes.push_back ({ std::min (busy, at + 1 + pick (2000)), [] (Rig& r) { r.bypass = false; } });
            continue;
        }
        const char* nanId = nanTargets[v % 8];
        sc.changes.push_back ({ at, [what, v, nanId] (Rig& r) {
                                   const float f = static_cast<float> (v);
                                   switch (what)
                                   {
                                       case 0: case 1: r.set ("arp.mode", std::fmod (f, 3.0f)); break;
                                       case 2: r.set ("arp.target", std::fmod (f, 3.0f)); break;
                                       case 3: r.set ("arp.hold", std::fmod (f, 2.0f)); break;
                                       case 4: r.set ("arp.rate", std::fmod (f, 6.0f)); break;
                                       case 5: r.set ("arp.swing", 50.0f + std::fmod (f, 26.0f)); break;
                                       case 6: r.set ("arp.gate", 5.0f + std::fmod (f, 20.0f) * 5.0f); break;
                                       case 7: r.set ("arp.pattern", std::fmod (f, 5.0f)); break;
                                       case 8: r.set ("arp.octaves", 1.0f + std::fmod (f, 4.0f)); break;
                                       case 9: r.set ("seq.length", 1.0f + std::fmod (f, 16.0f)); break;
                                       case 10: r.set (vs::seqStepParamID (1 + v % 16, vs::SeqStepField::Type), std::fmod (f, 3.0f)); break;
                                       case 11: r.set ("key.mode", std::fmod (f, 3.0f)); break;
                                       case 12: r.set ("key.split_point", 30.0f + f); break;
                                       case 13: r.set ("lower.assign.mode", std::fmod (f, 3.0f)); break;
                                       case 14: r.set ("upper.assign.mode", std::fmod (f, 3.0f)); break;
                                       case 15: r.set ("arp.tempo", 20.0f + f * 4.0f); break;
                                       case 16: r.hostReset(); break;
                                       case 17: r.proc.getState().getRawParameterValue (nanId)->store (v % 2 == 0 ? std::numeric_limits<float>::quiet_NaN()
                                                                                                                   : std::numeric_limits<float>::infinity());
                                           break;
                                       case 18:
                                           if (r.transport.playing)
                                               r.stop();
                                           else
                                               r.play (60.0 + 3.0 * static_cast<double> (v), -2.0 + 0.37 * static_cast<double> (v));
                                           break;
                                       case 19: r.locate (0.29 * static_cast<double> (v) - 3.0); break;
                                       case 20: r.tempo (70.0 + 2.5 * static_cast<double> (v)); break;
                                       case 21: r.transport.reportBpm = (v % 4) != 0; break;
                                       case 22: r.transport.reportPpq = (v % 5) != 0; break;
                                       case 23: r.transport.attached = (v % 6) != 0; break;
                                       case 24: r.set ("fx.chorus.mode", std::fmod (f, 3.0f)); break;
                                       case 25: r.set ("fx.delay.level", std::fmod (f, 8.0f) / 10.0f); break;
                                       case 26: r.set ("fx.delay.ping_pong", std::fmod (f, 2.0f)); break;
                                       case 27: r.set ("fx.reverb.level", std::fmod (f, 8.0f) / 10.0f); break;
                                       default: r.set ("fx.reverb.damping", f / 64.0f); break;
                                   }
                               } });
    }
    // The end: bypass off, every key and the pedal up, Hold off.
    sc.changes.push_back ({ sc.endAt, [] (Rig& r) {
                               r.bypass = false;
                               r.set ("arp.hold", 0);
                           } });
    for (int n = 0; n < 128; ++n)
        if (down[n])
            sc.midi.push_back (noteOff (sc.endAt, n));
    sc.midi.push_back (pedal (sc.endAt, false));
    return sc;
}

struct FuzzResult
{
    bool ok = true;
    double blockDiffDb = -400.0;
};

FuzzResult runFuzzSeed (unsigned seed, bool verbose)
{
    std::mt19937 meta (seed * 31u);
    const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
    const int preparedSizes[] { 64, 256, 512 };
    const double fs = rates[meta() % 4];
    const int prep = preparedSizes[meta() % 3];
    int block = 1 + static_cast<int> (meta() % static_cast<unsigned> (prep * 3));
    if (block < 16 && fs > 50000.0)
        block = 16; // run time
    // Overflow bursts (more events at one sample than the parser holds) drop the rest of a chunk by
    // design (section 2), so what survives depends on where chunks start: those seeds check the
    // properties at both block sizes but are not compared with each other.
    const bool withOverflow = seed % 3 == 0;
    FuzzResult res;
    Stereo outs[2];
    for (int run = 0; run < 2; ++run)
    {
        Rig rig (fs, prep);
        fuzzSetup (rig);
        rig.prepare();
        auto sc = makeFuzz (rig, seed, withOverflow);
        const int b = run == 0 ? prep : block;
        outs[run] = rig.render (sc.total, sc.midi, b, sc.changes);
        const bool fin = allFinite (outs[run]);
        const bool sounding = ! nothingSounds (rig);
        const double tail = peakDb (outs[run], static_cast<size_t> (sc.total - rig.samples (0.2)), outs[run].l.size());
        const bool released = rig.eventsRecorded ? (rig.allReleased (0) && rig.allReleased (1)) : true;
        if (! fin || sounding || tail > -100.0 || ! released)
        {
            res.ok = false;
            if (verbose)
                std::printf ("fuzz FAIL seed %u fs %.0f prepared %d block %d: finite %d sounding %d tail %.1f dB released %d\n", seed, fs,
                             prep, b, fin, sounding, tail, released);
        }
    }
    res.blockDiffDb = withOverflow ? -400.0 : diffDb (outs[0], outs[1]);
    if (res.blockDiffDb > -100.0)
    {
        res.ok = false;
        if (verbose)
            std::printf ("fuzz FAIL seed %u fs %.0f prepared %d: block %d vs %d differs by %.1f dB\n", seed, fs, prep, block, prep, res.blockDiffDb);
    }
    return res;
}

void runFuzz (unsigned firstSeed, unsigned seeds, double maxSeconds)
{
    const auto t0 = std::chrono::steady_clock::now();
    unsigned ran = 0, failed = 0;
    double worst = -400.0;
    for (unsigned s = firstSeed; s < firstSeed + seeds; ++s)
    {
        const auto r = runFuzzSeed (s, true);
        ++ran;
        failed += r.ok ? 0u : 1u;
        worst = std::max (worst, r.blockDiffDb);
        if (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() > maxSeconds)
            break; // time-bounded: the seeds that ran are reported
    }
    std::printf ("processor fuzz: %u seeds from %u, %u failed, worst block-size difference %.1f dB\n", ran, firstSeed, failed, worst);
    CHECK (ran >= std::min (seeds, 4u));
    CHECK (failed == 0);
}
} // namespace

TEST_CASE ("Processor fuzz: nothing hangs, finite, silent at the end, block-size independent", "[wave3][fuzz]")
{
    runFuzz (1, 12, 6.0);
}

TEST_CASE ("Processor fuzz, long run", "[.fuzzlong]")
{
    runFuzz (1000, 300, 1200.0);
}

// =================================================================================================
// No allocation on any path
// =================================================================================================

#if defined(__APPLE__) && VS_SEQ_COUNT_ALLOCATIONS
TEST_CASE ("No allocation: oversized blocks, target changes, reset, guard and the bypassed path", "[wave3][realtime]")
{
    Rig rig (48000.0, 256);
    rig.set ("arp.mode", 1);
    rig.set ("key.mode", 1);
    rig.set ("fx.chorus.mode", 2);
    rig.set ("fx.delay.level", 0.5f);
    rig.set ("fx.reverb.level", 0.5f);
    rig.prepare();
    rig.play (120.0, 0.0);
    rig.updatePlayHead();
    // One buffer per block length, allocated here (AudioBuffer::setSize growing a buffer allocates).
    juce::AudioBuffer<float> buffers[3] { juce::AudioBuffer<float> (2, 1500), juce::AudioBuffer<float> (2, 256), juce::AudioBuffer<float> (2, 17) };
    std::vector<juce::MidiBuffer> blocks (240);
    for (int b = 0; b < 240; ++b)
    {
        auto& mb = blocks[static_cast<size_t> (b)];
        if (b % 5 == 0)
            mb.addEvent (juce::MidiMessage::noteOn (1, 48 + b % 24, static_cast<juce::uint8> (100)), (b * 37) % 1500);
        if (b % 9 == 0)
            mb.addEvent (juce::MidiMessage::noteOff (1, 48 + (b + 5) % 24), 3);
        if (b % 13 == 0)
            mb.addEvent (juce::MidiMessage::controllerEvent (1, 64, (b / 13) % 2 == 0 ? 127 : 0), 100);
    }
    auto* target = rig.proc.getState().getRawParameterValue ("arp.target");
    auto* hold = rig.proc.getState().getRawParameterValue ("arp.hold");
    auto* cutoff = rig.proc.getState().getRawParameterValue ("lower.vcf.cutoff");
    vs::seq::test::AllocationCounter counter;
    {
        auto* probe = new int (3);
        vs::seq::test::gEscape = probe;
        delete probe;
    }
    const int base = counter.count();
    REQUIRE (base > 0);
    for (int b = 0; b < 240; ++b)
    {
        auto& buffer = buffers[b % 3]; // 1500, 256, 17: above, at and below the prepared size (256)
        const int len = buffer.getNumSamples();
        buffer.clear();
        if (b % 20 == 0)
            target->store (static_cast<float> ((b / 20) % 3));
        if (b == 50)
            hold->store (1.0f);
        if (b == 70)
            cutoff->store (std::numeric_limits<float>::quiet_NaN());
        if (b == 90)
            rig.proc.reset();
        if (b == 110)
            rig.proc.forceNonFiniteForTesting();
        if (b >= 130 && b < 160)
            rig.proc.processBlockBypassed (buffer, blocks[static_cast<size_t> (b)]); // fade, then silence
        else
            rig.proc.processBlock (buffer, blocks[static_cast<size_t> (b)]);
        if (rig.playHead.ppq.has_value())
            *rig.playHead.ppq += static_cast<double> (len) * 120.0 / (60.0 * 48000.0);
    }
    CHECK (counter.count() == base);
    CHECK (rig.proc.getGuardTrips() == 1);
}
#endif

// =================================================================================================
// Gain report for the owner (hidden)
// =================================================================================================

TEST_CASE ("Report: peak levels of a 32-voice Dual chord at velocity 127, dry and through the effects", "[.report]")
{
    struct Cfg
    {
        const char* name;
        std::vector<std::pair<juce::String, float>> params;
    };
    const std::vector<Cfg> cfgs {
        { "dry", {} },
        { "moderate effects (chorus I, delay 0.3, reverb 0.3)", { { "fx.chorus.mode", 1.0f }, { "fx.delay.level", 0.3f }, { "fx.reverb.level", 0.3f } } },
        { "extreme effects (chorus II full, delay 1.0 at 95 % / 250 ms, reverb 1.0 at 20 s)",
          { { "fx.chorus.mode", 2.0f }, { "fx.chorus.mix", 1.0f }, { "fx.delay.level", 1.0f }, { "fx.delay.feedback", 1.0f },
            { "fx.delay.time", vs::map::delayTimeFromSec (0.25f) }, { "fx.reverb.level", 1.0f }, { "fx.reverb.decay", 1.0f } } },
    };
    for (const auto& c : cfgs)
    {
        Rig rig (48000.0, 512);
        rig.set ("key.mode", 1);
        for (const auto& [id, v] : c.params)
            rig.set (id, v);
        rig.prepare();
        std::vector<Midi> midi;
        for (int n = 0; n < 16; ++n)
            midi.push_back (noteOn (0, 36 + 3 * n, 127));
        const auto out = rig.render (rig.samples (6.0), midi, 512);
        int clamped = 0;
        for (size_t i = 0; i < out.l.size(); ++i)
            clamped += (std::abs (out.l[i]) >= 3.999f || std::abs (out.r[i]) >= 3.999f) ? 1 : 0;
        std::printf ("peak, Dual 32 voices vel 127, master -6 dB, %s: %.2f dBFS (L %.3f, R %.3f), samples at the +12 dBFS clamp %d\n", c.name,
                     peakDb (out, 0, out.l.size()), peakAbs (out.l, 0, out.l.size()), peakAbs (out.r, 0, out.r.size()), clamped);
    }
}
