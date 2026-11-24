// Exitium B-Side's plugin shell as a host sees it (docs/architecture.md 8.3): identity, the 75
// parameters, 64 programs, state, bypass, the non-finite guard, the arpeggiator in front of the chip
// part, preset files of B-Side's own kind, and real-time safety of processBlock.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "bside/PluginProcessor.h"
#include "bside/presets/BSidePresets.h"
#include "bside/program/TableReader.h"
#include "common/params/FxArpParams.h"
#include "common/params/GlobalParams.h"

#if defined(__APPLE__)
 #include "../../seq_unit/AllocationCounter.h"
#endif

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

struct Rig
{
    juce::ScopedJuceInitialiser_GUI init;
    bs::BSideProcessor proc;

    explicit Rig (bool prepared = true)
    {
        if (prepared)
            prepare();
    }

    void prepare (double rate = kRate, int maxBlock = kBlock)
    {
        proc.setNonRealtime (false);
        proc.setPlayConfigDetails (0, 2, rate, maxBlock);
        proc.prepareToPlay (rate, maxBlock);
    }

    void set (const juce::String& id, float plain)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
    }
    float get (const juce::String& id) { return proc.getState().getRawParameterValue (id)->load(); }

    /** One block; returns its peak and clears `finite` on a non-finite sample. */
    float block (juce::MidiBuffer midi, bool* finite = nullptr, bool bypassed = false)
    {
        juce::AudioBuffer<float> buffer (2, kBlock);
        buffer.clear();
        if (bypassed)
            proc.processBlockBypassed (buffer, midi);
        else
            proc.processBlock (buffer, midi);
        float peak = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < kBlock; ++i)
            {
                const float v = buffer.getSample (ch, i);
                if (finite != nullptr && ! std::isfinite (v))
                    *finite = false;
                peak = std::max (peak, std::abs (v));
            }
        return peak;
    }

    float seconds (double s, bool* finite = nullptr)
    {
        float peak = 0.0f;
        for (int b = 0; b < static_cast<int> (s * kRate / kBlock); ++b)
            peak = std::max (peak, block ({}, finite));
        return peak;
    }
};

juce::MidiBuffer on (int note, int sampleOffset = 0, int velocity = 100)
{
    juce::MidiBuffer m;
    m.addEvent (juce::MidiMessage::noteOn (1, note, static_cast<juce::uint8> (velocity)), sampleOffset);
    return m;
}
double peakOf (const std::vector<float>& v)
{
    double p = 0.0;
    for (const float x : v)
        p = std::max (p, std::abs (static_cast<double> (x)));
    return p;
}

juce::MidiBuffer off (int note)
{
    juce::MidiBuffer m;
    m.addEvent (juce::MidiMessage::noteOff (1, note), 0);
    return m;
}
} // namespace

TEST_CASE ("B-Side: identity, bus layout, and no editor-less surprises", "[bside][shell]")
{
    Rig rig (false);
    CHECK (rig.proc.getName() == "Exitium B-Side");
    CHECK (rig.proc.acceptsMidi());
    CHECK_FALSE (rig.proc.producesMidi());
    CHECK_FALSE (rig.proc.isMidiEffect());
    CHECK (rig.proc.hasEditor());
    CHECK (rig.proc.getTotalNumInputChannels() == 0);
    CHECK (rig.proc.getTotalNumOutputChannels() == 2);
    // The build's plugin identity (docs/architecture.md 8.12, Q1): frozen at the first release.
    CHECK (JucePlugin_ManufacturerCode == 0x45786974); // 'Exit'
    CHECK (JucePlugin_PluginCode == 0x42737931);       // 'Bsy1'
    CHECK (juce::String (JucePlugin_Manufacturer) == "Exitium");
    CHECK (JucePlugin_IsSynth);
    CHECK (JucePlugin_WantsMidiInput);
    CHECK (juce::String (JucePlugin_Vst3Category) == "Instrument|Synth");
}

TEST_CASE ("B-Side: the 75 shared parameters come first, in the contract's order, with Vintage's ranges", "[bside][shell]")
{
    Rig rig (false);
    const auto& params = rig.proc.getParameters();
    REQUIRE (params.size() == bs::kParameterCount); // 75 shared + 2 x 64 per part + chip.clock (8.7, 8.21.2; ParamTests.cpp)
    REQUIRE (bs::kParameterCount == 204);
    REQUIRE (bs::kShellParameterCount == 75);

    juce::StringArray ids;
    for (auto* p : params)
        ids.add (dynamic_cast<juce::AudioProcessorParameterWithID*> (p)->paramID);
    auto unique = ids;
    unique.removeDuplicates (false);
    CHECK (unique.size() == ids.size());

    CHECK (ids[0] == "master.volume");
    CHECK (ids[1] == "key.mode");
    CHECK (ids[2] == "key.split_point");
    CHECK (ids[3] == "mix.balance");
    CHECK (ids[4] == vs::ParamID::fxChorusMode); // the 11 effect parameters follow
    CHECK (ids[15] == vs::ParamID::arpMode);     // then the 58 arp/seq parameters
    CHECK (ids[72] == "seq.step16.velocity");
    CHECK (ids[73] == "master.limiter");
    CHECK (ids[74] == "master.limiter.ceiling");

    // Defaults the shared appenders define (same as Vintage's).
    CHECK (rig.get ("master.volume") == Catch::Approx (-6.0f));
    CHECK (rig.get ("key.mode") == Catch::Approx (0.0f));
    CHECK (rig.get ("key.split_point") == Catch::Approx (60.0f));
    CHECK (rig.get ("master.limiter") == Catch::Approx (1.0f));
    CHECK (rig.get ("master.limiter.ceiling") == Catch::Approx (-1.0f));
}

TEST_CASE ("B-Side: 64 programs (the 16 of the factory bank, then Init), in an 8 x 8 bank with B-Side's own preset identity", "[bside][shell][presets]")
{
    Rig rig (false);
    CHECK (rig.proc.getNumPrograms() == 64);
    CHECK (rig.proc.getCurrentProgram() == 0);
    // Before S4b (8.21.2) all 64 were Init; the bank's 16 programs A1 to B8 are tested in BankTests.cpp, the rest are still Init.
    CHECK (rig.proc.getProgramName (0) == "Init");
    CHECK (rig.proc.getProgramName (1) == "Moving Pulse");
    CHECK (rig.proc.getProgramName (15) == "Bass And Lead");
    for (int i = 16; i < 64; ++i)
        REQUIRE (rig.proc.getProgramName (i) == "Init");

    auto& presets = rig.proc.getPresets();
    const auto& schema = presets.getSchema();
    CHECK (schema.isValid());
    CHECK (schema.instrument().id == "b-side");
    CHECK (schema.instrument().groups == 8);
    CHECK (schema.instrument().slotsPerGroup == 8);
    CHECK (schema.instrument().programExtension == ".bsprog");
    CHECK (schema.instrument().toneExtension == ".bstone");
    CHECK (schema.instrument().bankExtension == ".bsbank");
    CHECK (presets.positionName (63) == "H8");
    CHECK (schema.numParams() == 204);
    // Master volume and the limiter are session settings: not part of a program.
    CHECK (schema.isExcluded (schema.indexOf ("master.volume")));
    CHECK (schema.isExcluded (schema.indexOf ("master.limiter")));
    CHECK (schema.isExcluded (schema.indexOf ("master.limiter.ceiling")));
    CHECK_FALSE (schema.isExcluded (schema.indexOf ("fx.reverb.level")));
    CHECK (presets.getLocations().root.getFullPathName().endsWith ("Exitium" + juce::File::getSeparatorString() + "B-Side"));

    juce::StringArray errors;
    const auto bank = bs::makeBSideFactoryBank (schema, &errors);
    CHECK (errors.isEmpty());
    CHECK (static_cast<int> (bank.programs.size()) == 64);
    CHECK (bank.programs[0].meta.name == "Init");
}

TEST_CASE ("B-Side: a note renders finite, audible sound and is silent after release", "[bside][shell]")
{
    Rig rig;
    bool finite = true;
    CHECK (rig.block (on (60, 3), &finite) > 0.0f);
    const float held = rig.seconds (0.5, &finite);
    CHECK (held > 0.01f);
    CHECK (held < 1.0f);
    rig.block (off (60), &finite);
    rig.seconds (1.0, &finite); // the chip's release, then the idle rule (silent for a control interval, or 500 ms and a fade)
    CHECK (rig.seconds (0.5, &finite) < 1.0e-4f);
    CHECK (finite);
    // the part's one latency (docs/architecture.md 8.21.1): Model A's chain at 96 kHz, the board filters and the decimator
    CHECK (rig.proc.getLatencySamples() == rig.proc.getLowerPart().getLatencySamples());
    CHECK (rig.proc.getLatencySamples() == 22);
}

TEST_CASE ("B-Side: the state round-trips, carries its tag and version, and refuses another instrument's", "[bside][shell][state]")
{
    Rig a (false), b (false);
    for (const char* id : { "fx.reverb.level", "fx.delay.time", "mix.balance", "arp.rate", "seq.step03.note", "master.limiter.ceiling" })
    {
        auto* p = a.proc.getState().getParameter (id);
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (0.37f);
    }
    a.set ("key.mode", 2.0f);
    a.set ("key.split_point", 48.0f);

    juce::MemoryBlock state;
    a.proc.getStateInformation (state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), static_cast<int> (state.getSize()));
    REQUIRE (xml != nullptr);
    CHECK (xml->hasTagName ("BSideState"));
    CHECK (xml->getIntAttribute ("stateVersion") == 1);
    CHECK (xml->getChildByName ("Preset") != nullptr);

    b.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    for (auto* p : a.proc.getParameters())
    {
        const auto id = dynamic_cast<juce::AudioProcessorParameterWithID*> (p)->paramID;
        INFO (id);
        CHECK (b.proc.getState().getParameter (id)->getValue() == Catch::Approx (p->getValue()).margin (1.0e-6));
    }

    // Another instrument's state (a different tag) is ignored: nothing changes.
    juce::XmlElement foreign ("VintageSynthState");
    juce::MemoryBlock foreignBlock;
    juce::AudioProcessor::copyXmlToBinary (foreign, foreignBlock);
    Rig c (false);
    c.set ("fx.reverb.level", 0.9f);
    c.proc.setStateInformation (foreignBlock.getData(), static_cast<int> (foreignBlock.getSize()));
    CHECK (c.get ("fx.reverb.level") == Catch::Approx (0.9f));

    // The default state saved and restored changes nothing either.
    Rig d (false), e (false);
    juce::MemoryBlock defaults;
    d.proc.getStateInformation (defaults);
    e.set ("fx.reverb.level", 0.9f);
    e.proc.setStateInformation (defaults.getData(), static_cast<int> (defaults.getSize()));
    CHECK (e.get ("fx.reverb.level") == Catch::Approx (d.get ("fx.reverb.level")));
}

TEST_CASE ("B-Side: host bypass fades out, then is silent; the first block after it starts clean", "[bside][shell][bypass]")
{
    Rig rig;
    bool finite = true;
    rig.block (on (64), &finite);
    CHECK (rig.seconds (0.1, &finite) > 0.01f);
    CHECK (rig.block ({}, &finite, true) < 0.1f); // fading from the level it had: never louder
    for (int i = 0; i < 4; ++i)
        rig.block (on (72), &finite, true); // MIDI is ignored while bypassed
    CHECK (rig.block ({}, &finite, true) < 1.0e-9f);
    CHECK (rig.block ({}, &finite) < 1.0e-9f); // back: nothing sounds until a key is pressed
    CHECK (rig.block (on (67), &finite) >= 0.0f);
    CHECK (rig.seconds (0.1, &finite) > 0.01f);
    CHECK (finite);
}

TEST_CASE ("B-Side: the non-finite guard fires once and the output stays finite", "[bside][shell][guard]")
{
    Rig rig;
    bool finite = true;
    rig.block (on (60), &finite);
    CHECK (rig.proc.getGuardTrips() == 0);
    rig.proc.forceNonFiniteForTesting();
    rig.block ({}, &finite);
    CHECK (rig.proc.getGuardTrips() == 1);
    CHECK (finite);
    rig.seconds (0.1, &finite);
    CHECK (rig.proc.getGuardTrips() == 1);
    CHECK (finite);
    // The engine was reset by the guard: a new note sounds again.
    rig.block (on (62), &finite);
    CHECK (rig.seconds (0.1, &finite) > 0.01f);
}

TEST_CASE ("B-Side: the arpeggiator plays the chip part", "[bside][shell][arp]")
{
    // Same chord, arp off and on (no host transport: the module runs on its own tempo, 120 bpm).
    const auto noteOns = [] (bool arpOn) {
        Rig rig;
        if (arpOn)
            rig.set ("arp.mode", 1.0f);
        juce::MidiBuffer chord;
        for (const int n : { 60, 64, 67 })
            chord.addEvent (juce::MidiMessage::noteOn (1, n, static_cast<juce::uint8> (100)), 0);
        int count = 0;
        float peak = 0.0f;
        for (int b = 0; b < static_cast<int> (2.0 * kRate / kBlock); ++b)
        {
            peak = std::max (peak, rig.block (b == 0 ? chord : juce::MidiBuffer {}));
            for (const auto& e : rig.proc.getLastPartEvents (0))
                if (e.type == vs::EventType::NoteOn)
                    ++count;
        }
        CHECK (peak > 0.01f);
        return count;
    };
    CHECK (noteOns (false) == 3);
    CHECK (noteOns (true) > 8); // 1/16 notes at 120 bpm: 8 steps a second, so well over 8 in 2 s
}

TEST_CASE ("B-Side: a program change applies Init to a changed sound; master volume stays", "[bside][shell][presets]")
{
    Rig rig (false);
    rig.set ("fx.reverb.level", 0.8f);
    rig.set ("master.volume", -20.0f);
    rig.proc.setCurrentProgram (5); // applied at once on the message thread (this one)
    CHECK (rig.proc.getCurrentProgram() == 5);
    CHECK (rig.get ("fx.reverb.level") == Catch::Approx (0.0f).margin (1.0e-6));
    CHECK (rig.get ("master.volume") == Catch::Approx (-20.0f)); // a session setting, not part of a program
}

TEST_CASE ("B-Side: program, tone and bank files carry B-Side's identity and refuse another instrument's", "[bside][shell][presets]")
{
    Rig rig (false);
    auto& presets = rig.proc.getPresets();
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("bside-shell-test-" + juce::String (juce::Time::currentTimeMillis()));
    REQUIRE (dir.createDirectory());

    rig.set ("fx.delay.level", 0.6f);
    const auto program = dir.getChildFile ("test.bsprog");
    CHECK (presets.saveProgramFile (program, "Shell Test").isEmpty());
    CHECK (program.loadFileAsString().contains ("\"instrument\": \"b-side\""));
    rig.set ("fx.delay.level", 0.0f);
    CHECK (presets.loadProgramFile (program).isEmpty());
    CHECK (rig.get ("fx.delay.level") == Catch::Approx (0.6f).margin (1.0e-4));

    const auto bank = dir.getChildFile ("test.bsbank");
    CHECK (presets.saveBankFile (bank, "Shell Bank").isEmpty());
    CHECK (bank.loadFileAsString().contains ("\"instrument\": \"b-side\""));
    CHECK (presets.loadBankFile (bank).isEmpty());

    // A Vintage program file is another instrument's: rejected with a message, nothing changes.
    const auto foreign = dir.getChildFile ("foreign.bsprog");
    REQUIRE (foreign.replaceWithText (program.loadFileAsString().replace ("\"b-side\"", "\"vintage-synth\"")));
    rig.set ("fx.delay.level", 0.25f);
    CHECK (presets.loadProgramFile (foreign).isNotEmpty());
    CHECK (rig.get ("fx.delay.level") == Catch::Approx (0.25f).margin (1.0e-4));

    dir.deleteRecursively();
}

// ---- the chip part in the shell (docs/architecture.md 8.21.1, behaviour gates 5 and 6) ------------------------

namespace
{
/** Renders `blocks` blocks of the processor (stereo), `midi` in the first; returns left and right. */
std::pair<std::vector<float>, std::vector<float>> renderStereo (Rig& rig, int blocks, juce::MidiBuffer midi = {}, int switchAt = -1,
                                                                 std::function<void()> atSwitch = {})
{
    std::vector<float> l, r;
    for (int b = 0; b < blocks; ++b)
    {
        if (b == switchAt && atSwitch)
            atSwitch();
        juce::AudioBuffer<float> buffer (2, kBlock);
        buffer.clear();
        juce::MidiBuffer m;
        if (b == 0)
            m = midi;
        rig.proc.processBlock (buffer, m);
        l.insert (l.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + kBlock);
        r.insert (r.end(), buffer.getReadPointer (1), buffer.getReadPointer (1) + kBlock);
    }
    return { l, r };
}

juce::MidiBuffer chord (std::initializer_list<int> notes)
{
    juce::MidiBuffer m;
    for (const int n : notes)
        m.addEvent (juce::MidiMessage::noteOn (1, n, static_cast<juce::uint8> (127)), 7);
    return m;
}
} // namespace

TEST_CASE ("B-Side: Whole mode: Upper costs nothing and is silent", "[bside][shell][part]")
{
    Rig rig;
    rig.set ("master.limiter", 0.0f);
    renderStereo (rig, 100, chord ({ 48, 60, 67 }));
    CHECK (rig.proc.getLowerPart().numActiveVoices() == 3);
    CHECK_FALSE (rig.proc.isUpperRendering());
    CHECK (rig.proc.getUpperPart().getRenderedSamples() == 0); // never rendered a sample
    CHECK (rig.proc.getUpperPart().numActiveVoices() == 0);
}

TEST_CASE ("B-Side: Dual equals the sum of both parts rendered apart", "[bside][shell][part]")
{
    Rig rig;
    rig.set ("master.limiter", 0.0f);
    rig.set ("key.mode", 1.0f); // Dual
    rig.set ("upper.chip.model", 0.0f);
    rig.set ("upper.osc1.wave", 2.0f);
    const auto out = renderStereo (rig, 60, chord ({ 48, 60, 67 }));

    // the two parts on their own, with the parameters the processor gave them
    bs::PartParams lower, upper;
    upper.chipModel = 0;
    upper.osc1Wave = 2;
    std::vector<float> sum (out.first.size(), 0.0f);
    for (int p = 0; p < 2; ++p)
    {
        bs::Part part;
        part.setParams (p == 0 ? lower : upper);
        part.prepare (kRate, kBlock, 2, p == 0 ? lower : upper, 0);
        std::vector<vs::NoteEvent> ev;
        for (const int n : { 48, 60, 67 })
            ev.push_back ({ 7, vs::EventType::NoteOn, static_cast<std::uint8_t> (n), 1.0f });
        std::vector<float> y (kBlock);
        for (int b = 0; b < 60; ++b)
        {
            part.render (y.data(), kBlock, b == 0 ? std::span<const vs::NoteEvent> (ev) : std::span<const vs::NoteEvent> {});
            for (int i = 0; i < kBlock; ++i)
                sum[static_cast<std::size_t> (b * kBlock + i)] += y[static_cast<std::size_t> (i)];
        }
    }
    // master volume -6 dB and the fixed trim (x 0.5); centre pan and balance are unity (the no-boost law)
    const double g = std::pow (10.0, -6.0 / 20.0) * 0.5;
    double worst = 0.0, peak = 0.0;
    for (std::size_t i = 0; i < sum.size(); ++i)
    {
        worst = std::max (worst, std::abs (static_cast<double> (out.first[i]) - g * static_cast<double> (sum[i])));
        worst = std::max (worst, std::abs (static_cast<double> (out.second[i]) - g * static_cast<double> (sum[i])));
        peak = std::max (peak, std::abs (static_cast<double> (out.first[i])));
    }
    INFO ("peak " << peak << ", largest difference " << worst);
    REQUIRE (peak > 0.01);
    CHECK (worst <= 1.0e-6 * std::max (1.0, peak) * 8.0); // float tolerance: a few steps of the output
}

TEST_CASE ("B-Side: a structural switch on Lower leaves Upper's output bit-identical", "[bside][shell][part][switch]")
{
    // Lower panned hard left, Upper hard right (the no-boost law: the far side is exactly 0): the right channel is
    // Upper alone. Every structural parameter of Lower switched while both play; chip.clock is global (both parts).
    for (const char* id : { "lower.chip.model", "lower.filter.topology", "lower.chip.resolution", "lower.chip.volume_dc" })
    {
        auto run = [&] (bool doSwitch) {
            Rig rig;
            rig.set ("master.limiter", 0.0f);
            rig.set ("key.mode", 1.0f);
            rig.set ("lower.mix.pan", -1.0f);
            rig.set ("upper.mix.pan", 1.0f);
            auto* p = rig.proc.getState().getParameter (id);
            const float flipped = p->getValue() > 0.5f ? 0.0f : 1.0f;
            return renderStereo (rig, 60, chord ({ 52, 64 }), 20, [&] {
                if (doSwitch)
                    p->setValueNotifyingHost (flipped);
            });
        };
        const auto a = run (false), b = run (true);
        INFO (id);
        CHECK (b.second == a.second); // Upper: bit-identical
        CHECK (b.first != a.first);   // and the switch did act on Lower
        CHECK (peakOf (b.second) > 0.01);
    }
}

TEST_CASE ("B-Side: mix.pan pans each part with the no-boost law; the tail is 3 x the longest release of the parts that can sound", "[bside][shell][part]")
{
    Rig rig;
    rig.set ("master.limiter", 0.0f);
    rig.set ("lower.mix.pan", 1.0f);
    const auto out = renderStereo (rig, 40, chord ({ 60 }));
    const std::vector<float> left (out.first.begin() + 30 * kBlock, out.first.end()); // after the mixer's (exponential) smoothing
    CHECK (peakOf (left) <= 0.0); // hard right: the left side is exactly 0
    CHECK (peakOf (out.second) > 0.01);

    // the tail: Lower's osc 1 release 6 (204 ms nominal) x 3, effects off; Upper counts in Dual only
    Rig t (false);
    CHECK (t.proc.getTailLengthSeconds() == Catch::Approx (3.0 * 0.204).margin (1e-9));
    t.set ("upper.osc1.release", 15.0f); // 24 s nominal
    CHECK (t.proc.getTailLengthSeconds() == Catch::Approx (3.0 * 0.204).margin (1e-9));
    t.set ("key.mode", 1.0f);
    CHECK (t.proc.getTailLengthSeconds() == Catch::Approx (30.0).margin (1e-9)); // 72 s, capped by the shell
    t.set ("key.mode", 0.0f);
    t.set ("lower.osc2.wave", 2.0f);
    t.set ("lower.osc2.release", 9.0f); // 750 ms nominal
    CHECK (t.proc.getTailLengthSeconds() == Catch::Approx (3.0 * 0.750).margin (1e-9));
}

TEST_CASE ("B-Side: the Tempo tick rate follows arp.tempo while the host gives no tempo", "[bside][shell][part]")
{
    Rig rig;
    rig.set ("arp.tempo", 90.0f);
    rig.set ("lower.tick.rate", 2.0f); // Tempo
    rig.block ({});
    CHECK (rig.proc.getTickTempo() == Catch::Approx (90.0));
    renderStereo (rig, 2, chord ({ 60 }));
    // 24 ticks per beat at 90 bpm, the 50 Hz system clock: 985248 x 60 / (90 x 24)
    CHECK (rig.proc.getLowerPart().getVoice (0).engine.tickPeriod() == 27368u);
}

#if defined(VS_SEQ_COUNT_ALLOCATIONS) && VS_SEQ_COUNT_ALLOCATIONS // macOS without AddressSanitizer
TEST_CASE ("B-Side: processBlock does not allocate (arp, effects, key modes, bypass, program change request)", "[bside][shell][realtime]")
{
    Rig rig;
    rig.set ("fx.chorus.mode", 2.0f);
    rig.set ("fx.delay.level", 0.5f);
    rig.set ("fx.reverb.level", 0.5f);
    juce::AudioBuffer<float> buffer (2, kBlock);
    std::vector<juce::MidiBuffer> blocks (200);
    for (int b = 0; b < 200; ++b)
    {
        if (b % 7 == 0)
            blocks[static_cast<size_t> (b)].addEvent (juce::MidiMessage::noteOn (1, 48 + b % 24, static_cast<juce::uint8> (100)), b % kBlock);
        if (b % 11 == 0)
            blocks[static_cast<size_t> (b)].addEvent (juce::MidiMessage::noteOff (1, 48 + (b + 5) % 24), 3);
        if (b % 13 == 0)
            blocks[static_cast<size_t> (b)].addEvent (juce::MidiMessage::controllerEvent (1, 64, (b / 13) % 2 == 0 ? 127 : 0), 100);
        if (b == 170)
            blocks[static_cast<size_t> (b)].addEvent (juce::MidiMessage::programChange (1, 9), 5);
    }
    auto* keyMode = rig.proc.getState().getRawParameterValue ("key.mode");
    auto* arpMode = rig.proc.getState().getRawParameterValue ("arp.mode");
    vs::seq::test::AllocationCounter counter;
    {
        auto* probe = new int (3); // the counter must see an allocation on this thread
        vs::seq::test::gEscape = probe;
        delete probe;
    }
    const int base = counter.count();
    REQUIRE (base > 0);
    for (int b = 0; b < 200; ++b)
    {
        if (b == 60)
            keyMode->store (2.0f);
        if (b == 90)
            arpMode->store (2.0f);
        if (b == 120)
            arpMode->store (0.0f);
        buffer.clear();
        if (b >= 140 && b < 160)
            rig.proc.processBlockBypassed (buffer, blocks[static_cast<size_t> (b)]);
        else
            rig.proc.processBlock (buffer, blocks[static_cast<size_t> (b)]);
    }
    CHECK (counter.count() == base);
}
TEST_CASE ("B-Side: processBlock does not allocate with chip notes, steals, unison, a table applied while playing and every structural switch", "[bside][shell][realtime][part]")
{
    Rig rig;
    rig.set ("key.mode", 1.0f); // Dual: both parts
    auto& state = rig.proc.getState();
    const auto table = vs::bside::program::readTable ("bside-table 1\n[restart]\nmode classic\n[wave1]\n    wave pulse+gate +0\nloop:\n    pitch +12\n    pitch +0\n    jump loop\n"
                                                    "[pulse1]\n    set 512\n    pingpong 512 3584 48\n[filter]\n    cutoff 1500\n    sweep -30 x20\n");
    REQUIRE (table.ok);
    juce::AudioBuffer<float> buffer (2, kBlock);
    std::vector<juce::MidiBuffer> blocks (260);
    for (int b = 0; b < 260; ++b)
    {
        auto& m = blocks[static_cast<size_t> (b)];
        if (b % 3 == 0)
            for (int k = 0; k < 7; ++k) // more notes than voices: steals
                m.addEvent (juce::MidiMessage::noteOn (1, 36 + (b * 5 + k * 7) % 60, static_cast<juce::uint8> (40 + k * 10)), (b * 13 + k * 31) % kBlock);
        if (b % 5 == 0)
            m.addEvent (juce::MidiMessage::noteOff (1, 36 + (b * 3) % 60), 11);
        if (b % 17 == 0)
            m.addEvent (juce::MidiMessage::pitchWheel (1, (b * 997) % 16384), 30);
        if (b % 19 == 0)
            m.addEvent (juce::MidiMessage::controllerEvent (1, 1, (b * 7) % 128), 40);
        if (b % 23 == 0)
            m.addEvent (juce::MidiMessage::controllerEvent (1, 64, (b / 23) % 2 == 0 ? 127 : 0), 50);
    }
    // what changes while playing, at fixed blocks (raw parameter values; written before the block)
    struct Change
    {
        int block;
        const char* id;
        float value;
    };
    const std::vector<Change> changes {
        { 20, "lower.assign.mode", 1.0f },    { 30, "lower.mix.level", -18.0f },          { 40, "lower.assign.unison_voices", 3.0f },
        { 60, "lower.chip.model", 0.0f },     { 70, "upper.mix.level", 6.0f },            { 71, "upper.mix.level", -24.0f }, // a ramp cut by the next
        { 80, "lower.filter.topology", 1.0f }, { 100, "chip.clock", 1.0f },               { 120, "lower.chip.resolution", 1.0f },
        { 130, "lower.mix.level", 0.0f },     { 140, "lower.chip.volume_dc", 1.0f },      { 150, "upper.chip.model", 0.0f },
        { 160, "lower.assign.mode", 2.0f },   { 170, "lower.porta.time", 0.5f },          { 180, "lower.chip.variation", 90.0f },
        { 190, "lower.filter.topology", 0.0f }, { 200, "lower.tick.rate", 2.0f },         { 210, "lower.assign.mode", 0.0f },
        { 220, "lower.mix.level", -6.0f },    { 240, "upper.mix.level", 0.0f },
    };
    // the counter must see an allocation on this thread
    vs::seq::test::AllocationCounter counter;
    {
        auto* probe = new int (3);
        vs::seq::test::gEscape = probe;
        delete probe;
    }
    const int base = counter.count();
    REQUIRE (base > 0);
    std::size_t next = 0;
    for (int b = 0; b < 260; ++b)
    {
        while (next < changes.size() && changes[next].block == b)
        {
            state.getRawParameterValue (changes[next].id)->store (changes[next].value);
            ++next;
        }
        if (b == 110)
            rig.proc.tableHandover (0).publish (table.program); // a table applied while playing (inside no hold: the
                                                                 // audio side's claim and commit run in processBlock)
        buffer.clear();
        rig.proc.processBlock (buffer, blocks[static_cast<size_t> (b)]);
    }
    CHECK (counter.count() == base);
    CHECK (rig.proc.getLowerPart().switchesDone() >= 5);
}
#endif
