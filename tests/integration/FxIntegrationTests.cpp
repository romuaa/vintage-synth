// Wave 3 integration, parameters and effects (docs/architecture.md 6a.3 item 2):
// - the 69 new parameters: IDs, order, names, ranges, defaults, display text <-> value;
// - every parameter reaches its module with the documented mapping (checked against formulas
//   written here, not the Mappings.h functions);
// - effects (and arp/seq) off: bit-identical to the M3 pipeline, rebuilt from the M3 components
//   (parser, router, two Parts, mixer, master gain, clamp) without the processor;
// - an effect switched on and off again returns to the dry signal bit-exactly;
// - each effect audible and stereo; mono bus = (L + R) / 2 after the effects;
// - tail length from the parameters, and the audio really is silent after it;
// - the non-finite guard resets the effects and both arp/seq instances.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "IntegrationRig.h"
#include "common/engine/EventParser.h"
#include "common/engine/KeyRouter.h"
#include "common/engine/PartMixer.h"
#include "vintage/params/PartMappings.h"
#include "vintage/params/ParameterLayout.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <random>

using namespace vs::itest;

namespace
{
juce::String idAt (const juce::Array<juce::AudioProcessorParameter*>& params, int i)
{
    return dynamic_cast<juce::AudioProcessorParameterWithID*> (params[i])->paramID;
}

int indexOf (const juce::Array<juce::AudioProcessorParameter*>& params, const juce::String& id)
{
    for (int i = 0; i < params.size(); ++i)
        if (idAt (params, i) == id)
            return i;
    return -1;
}

float plainDefault (juce::AudioProcessorValueTreeState& state, const juce::String& id)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter (id));
    REQUIRE (p != nullptr);
    return p->convertFrom0to1 (p->getDefaultValue());
}
} // namespace

// =================================================================================================
// Parameters
// =================================================================================================

TEST_CASE ("Wave 3 parameters: 69 appended in the contract's order with IDs, names, ranges and defaults", "[wave3][params]")
{
    juce::ScopedJuceInitialiser_GUI init;
    vs::VintageSynthProcessor proc;
    auto& state = proc.getState();
    const auto& params = proc.getParameters();
    REQUIRE (params.size() == 172);

    struct Expected
    {
        const char* id;
        const char* name;
    };
    std::vector<Expected> expected {
        { "fx.chorus.mode", "Chorus" },          { "fx.chorus.mix", "Chorus Mix" },
        { "fx.delay.time", "Delay Time" },       { "fx.delay.feedback", "Delay Feedback" },
        { "fx.delay.damping", "Delay Damping" }, { "fx.delay.ping_pong", "Delay Ping-Pong" },
        { "fx.delay.level", "Delay Level" },     { "fx.reverb.decay", "Reverb Decay" },
        { "fx.reverb.damping", "Reverb Damping" }, { "fx.reverb.predelay", "Reverb Pre-Delay" },
        { "fx.reverb.level", "Reverb Level" },
        { "arp.mode", "Arp/Seq Mode" },          { "arp.target", "Arp/Seq Part" },
        { "arp.rate", "Arp/Seq Rate" },          { "arp.swing", "Arp/Seq Swing" },
        { "arp.gate", "Arp/Seq Gate" },          { "arp.hold", "Arp/Seq Hold" },
        { "arp.tempo", "Internal Tempo" },       { "arp.pattern", "Arp Pattern" },
        { "arp.octaves", "Arp Octaves" },        { "seq.length", "Seq Length" },
    };
    std::vector<std::string> stepIds, stepNames; // written out independently of seqStepParamID
    for (int s = 1; s <= 16; ++s)
    {
        char nn[3];
        std::snprintf (nn, sizeof (nn), "%02d", s);
        for (const auto& [suffix, label] : { std::pair { "note", "Note" }, std::pair { "type", "Type" }, std::pair { "velocity", "Velocity" } })
        {
            stepIds.push_back (std::string ("seq.step") + nn + "." + suffix);
            stepNames.push_back (std::string ("Seq Step ") + nn + " " + label);
        }
    }
    for (size_t i = 0; i < stepIds.size(); ++i)
        expected.push_back ({ stepIds[i].c_str(), stepNames[i].c_str() });
    REQUIRE (expected.size() == 69);

    const int first = indexOf (params, vs::ParamID::mixBalance) + 1;
    CHECK (first == 101);
    for (size_t k = 0; k < expected.size(); ++k)
    {
        const int i = first + static_cast<int> (k);
        INFO ("index " << i);
        CHECK (idAt (params, i) == expected[k].id);
        CHECK (params[i]->getName (64) == expected[k].name);
    }

    auto choice = [&] (const char* id) { return dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (id)); };
    auto checkChoices = [&] (const char* id, std::vector<const char*> labels, int def) {
        INFO (id);
        auto* c = choice (id);
        REQUIRE (c != nullptr);
        REQUIRE (c->choices.size() == static_cast<int> (labels.size()));
        for (size_t i = 0; i < labels.size(); ++i)
            CHECK (c->choices[static_cast<int> (i)] == labels[i]);
        CHECK (c->getIndex() == def);
    };
    checkChoices ("fx.chorus.mode", { "Off", "I", "II" }, 0);
    checkChoices ("fx.delay.ping_pong", { "Off", "On" }, 1);
    checkChoices ("arp.mode", { "Off", "Arpeggiator", "Sequencer" }, 0);
    checkChoices ("arp.target", { "Lower", "Upper", "Both" }, 2);
    checkChoices ("arp.rate", { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" }, 3);
    checkChoices ("arp.hold", { "Off", "On" }, 0);
    checkChoices ("arp.pattern", { "Up", "Down", "Up & Down", "Random", "As Played" }, 0);
    for (int s = 1; s <= 16; ++s)
        checkChoices (stepIds[static_cast<size_t> ((s - 1) * 3 + 1)].c_str(), { "Note", "Rest", "Tie" }, 0);

    // Float ranges and defaults (plain values).
    struct F
    {
        const char* id;
        float lo, hi, def;
    };
    const F floats[] { { "fx.chorus.mix", 0, 1, 0.5f },     { "fx.delay.feedback", 0, 1, 0.35f / 0.95f },
                       { "fx.delay.damping", 0, 1, 0.3f },  { "fx.delay.level", 0, 1, 0.0f },
                       { "fx.reverb.damping", 0, 1, 0.5f }, { "fx.reverb.predelay", 0, 1, 0.1f },
                       { "fx.reverb.level", 0, 1, 0.0f },   { "arp.swing", 50, 75, 50 },
                       { "arp.gate", 5, 100, 50 },          { "arp.tempo", 20, 300, 120 } };
    for (const auto& f : floats)
    {
        INFO (f.id);
        auto* p = dynamic_cast<juce::AudioParameterFloat*> (state.getParameter (f.id));
        REQUIRE (p != nullptr);
        CHECK (std::abs (p->range.start - f.lo) < 1.0e-6f);
        CHECK (std::abs (p->range.end - f.hi) < 1.0e-6f);
        CHECK (std::abs (p->get() - f.def) < 1.0e-5f);
    }
    // Exponential defaults, checked in physical units with the formulas of the contract.
    const float delayV = plainDefault (state, "fx.delay.time");
    CHECK (std::abs (0.001 * std::pow (2000.0, static_cast<double> (delayV)) - 0.7) < 1.0e-5);
    const float decayV = plainDefault (state, "fx.reverb.decay");
    CHECK (std::abs (0.3 * std::pow (20.0 / 0.3, static_cast<double> (decayV)) - 2.5) < 1.0e-5);
    for (int s = 0; s < 16; ++s)
    {
        CHECK (std::abs (plainDefault (state, stepIds[static_cast<size_t> (s * 3)])) < 1.0e-6f);
        CHECK (std::abs (plainDefault (state, stepIds[static_cast<size_t> (s * 3 + 2)]) - 0.8f) < 1.0e-6f);
    }
    // Integer parameters: discrete (hosts step them) with the documented ranges.
    struct I
    {
        const char* id;
        int lo, hi, def;
    };
    std::vector<I> ints { { "arp.octaves", 1, 4, 1 }, { "seq.length", 1, 16, 16 } };
    for (int s = 0; s < 16; ++s)
        ints.push_back ({ stepIds[static_cast<size_t> (s * 3)].c_str(), -24, 24, 0 });
    for (const auto& i : ints)
    {
        INFO (i.id);
        auto* p = dynamic_cast<juce::AudioParameterInt*> (state.getParameter (i.id));
        REQUIRE (p != nullptr);
        CHECK (p->getRange().getStart() == i.lo);
        CHECK (p->getRange().getEnd() == i.hi);
        CHECK (p->get() == i.def);
        CHECK (p->isDiscrete());
        CHECK (static_cast<juce::AudioProcessorParameter*> (p)->getNumSteps() == i.hi - i.lo + 1);
    }
}

TEST_CASE ("Wave 3 parameters: display text and value round-trip through the shared mappings", "[wave3][params]")
{
    juce::ScopedJuceInitialiser_GUI init;
    vs::VintageSynthProcessor proc;
    auto& state = proc.getState();
    auto text = [&] (const char* id, float plain) {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter (id));
        return p->getText (p->convertTo0to1 (plain), 64);
    };
    auto parse = [&] (const char* id, const juce::String& t) {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter (id));
        return p->convertFrom0to1 (p->getValueForText (t));
    };

    // Known texts (formulas: 1 ms x 2000^v; 0.3 s x (20 / 0.3)^v; 95 % x v; 200 ms x v).
    CHECK (text ("fx.delay.time", plainDefault (state, "fx.delay.time")) == "700 ms");
    CHECK (text ("fx.delay.time", 0.0f) == "1.0 ms");
    CHECK (text ("fx.delay.time", 1.0f) == "2.00 s");
    CHECK (text ("fx.delay.feedback", 1.0f) == "95.0 %");
    CHECK (text ("fx.delay.feedback", 0.35f / 0.95f) == "35.0 %");
    CHECK (text ("fx.reverb.decay", plainDefault (state, "fx.reverb.decay")) == "2.50 s");
    CHECK (text ("fx.reverb.decay", 0.0f) == "300 ms");
    CHECK (text ("fx.reverb.decay", 1.0f) == "20.00 s");
    CHECK (text ("fx.reverb.predelay", 0.1f) == "20.0 ms");
    CHECK (text ("fx.reverb.predelay", 1.0f) == "200.0 ms");
    CHECK (text ("fx.chorus.mix", 0.5f) == "50 %");
    CHECK (text ("arp.swing", 62.5f) == "62.5 %");
    CHECK (text ("arp.gate", 100.0f) == "100.0 %");
    CHECK (text ("arp.tempo", 133.0f) == "133.0 bpm");
    CHECK (text ("seq.step01.note", 7.0f) == "+7 st");
    CHECK (text ("seq.step01.note", 0.0f) == "0 st");
    CHECK (text ("seq.step16.note", -24.0f) == "-24 st");
    CHECK (text ("arp.rate", 4.0f) == "1/16T");

    // Parsing: physical text -> slider value with the contract's formulas.
    CHECK (std::abs (parse ("fx.delay.time", "250 ms") - static_cast<float> (std::log (250.0) / std::log (2000.0))) < 1.0e-5f);
    CHECK (std::abs (parse ("fx.delay.time", "1.5 s") - static_cast<float> (std::log (1500.0) / std::log (2000.0))) < 1.0e-5f);
    CHECK (std::abs (parse ("fx.reverb.decay", "5 s") - static_cast<float> (std::log (5.0 / 0.3) / std::log (20.0 / 0.3))) < 1.0e-5f);
    CHECK (std::abs (parse ("fx.reverb.predelay", "50 ms") - 0.25f) < 1.0e-6f);
    CHECK (std::abs (parse ("fx.delay.feedback", "47.5 %") - 0.5f) < 1.0e-6f);
    CHECK (std::abs (parse ("arp.swing", "60 %") - 60.0f) < 1.0e-6f);
    CHECK (std::abs (parse ("arp.tempo", "90 bpm") - 90.0f) < 1.0e-6f);
    CHECK (std::abs (parse ("seq.step03.note", "+5 st") - 5.0f) < 1.0e-6f);
    CHECK (std::abs (parse ("seq.step03.note", "-12 st") + 12.0f) < 1.0e-6f);
    CHECK (std::abs (parse ("arp.swing", "90 %") - 75.0f) < 1.0e-6f); // clamped to the range

    // Every new parameter: text(value) parses back to (about) the value, over its range.
    const auto& params = proc.getParameters();
    for (int i = 101; i < params.size(); ++i)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (params[i]);
        REQUIRE (p != nullptr);
        INFO (p->paramID);
        const int steps = p->isDiscrete() ? p->getNumSteps() : 21;
        for (int k = 0; k < steps; ++k)
        {
            const float v = static_cast<float> (k) / static_cast<float> (steps - 1);
            const auto t = p->getText (v, 64);
            const float back = p->getValueForText (t);
            INFO (t);
            // Parsing the shown text gives a value that shows the same text (the display rounds),
            // within the display's resolution of the value; discrete values come back exactly.
            CHECK (p->getText (back, 64) == t);
            if (p->isDiscrete())
                CHECK (std::abs (back - v) <= 1.0e-6f);
            else
                CHECK (std::abs (back - v) <= 0.01f);
        }
    }
}

TEST_CASE ("Wave 3 parameters: every parameter reaches its module with the documented mapping", "[wave3][params]")
{
    Rig rig;
    rig.prepare();
    rig.set ("fx.chorus.mode", 2);
    rig.set ("fx.chorus.mix", 0.8f);
    rig.set ("fx.delay.time", 0.4f);
    rig.set ("fx.delay.feedback", 0.6f);
    rig.set ("fx.delay.damping", 0.7f);
    rig.set ("fx.delay.ping_pong", 0);
    rig.set ("fx.delay.level", 0.25f);
    rig.set ("fx.reverb.decay", 0.8f);
    rig.set ("fx.reverb.damping", 0.2f);
    rig.set ("fx.reverb.predelay", 0.65f);
    rig.set ("fx.reverb.level", 0.45f);
    rig.set ("arp.mode", 2);
    rig.set ("arp.target", 0); // Lower only
    rig.set ("arp.rate", 4);
    rig.set ("arp.swing", 61.0f);
    rig.set ("arp.gate", 33.0f);
    rig.set ("arp.hold", 1);
    rig.set ("arp.tempo", 97.5f);
    rig.set ("arp.pattern", 3);
    rig.set ("arp.octaves", 3);
    rig.set ("seq.length", 11);
    for (int s = 1; s <= 16; ++s)
    {
        rig.set (vs::seqStepParamID (s, vs::SeqStepField::Note), static_cast<float> (3 * s - 24));
        rig.set (vs::seqStepParamID (s, vs::SeqStepField::Type), static_cast<float> (s % 3));
        rig.set (vs::seqStepParamID (s, vs::SeqStepField::Velocity), static_cast<float> (s) / 20.0f);
    }
    rig.render (64, {}, 64);

    const auto& f = rig.proc.getAppliedFxSettings();
    CHECK (f.chorus.mode == vs::fx::ChorusMode::II);
    CHECK (std::abs (f.chorus.mix - 0.8f) < 1.0e-6f);
    CHECK (std::abs (static_cast<double> (f.delay.timeSeconds) - 0.001 * std::pow (2000.0, 0.4)) < 1.0e-6);
    CHECK (std::abs (f.delay.feedback - 0.95f * 0.6f) < 1.0e-6f);
    CHECK (std::abs (f.delay.damping - 0.7f) < 1.0e-6f);
    CHECK_FALSE (f.delay.pingPong);
    CHECK (std::abs (f.delay.level - 0.25f) < 1.0e-6f);
    CHECK (std::abs (static_cast<double> (f.reverb.decaySeconds) - 0.3 * std::pow (20.0 / 0.3, 0.8)) < 1.0e-5);
    CHECK (std::abs (f.reverb.damping - 0.2f) < 1.0e-6f);
    CHECK (std::abs (f.reverb.preDelaySeconds - 0.2f * 0.65f) < 1.0e-6f);
    CHECK (std::abs (f.reverb.level - 0.45f) < 1.0e-6f);

    const auto& lower = rig.proc.getArpSeq (0).settings();
    CHECK (lower.mode == vs::seq::Mode::Seq);
    CHECK (lower.rate == vs::seq::Rate::SixteenthTriplet);
    CHECK (std::abs (lower.swing - 0.61) < 1.0e-6);
    CHECK (std::abs (lower.gate - 0.33) < 1.0e-6);
    CHECK (lower.hold);
    CHECK (std::abs (lower.internalBpm - 97.5) < 1.0e-9);
    CHECK (lower.arpPattern == vs::seq::ArpPattern::Random);
    CHECK (lower.arpOctaves == 3);
    CHECK (lower.seqLength == 11);
    CHECK (lower.seed == vs::kArpSeed);
    for (int s = 0; s < 16; ++s)
    {
        INFO ("step " << s + 1);
        const auto& st = lower.seqSteps[static_cast<size_t> (s)];
        CHECK (st.semitones == 3 * (s + 1) - 24);
        CHECK (static_cast<int> (st.type) == (s + 1) % 3);
        CHECK (std::abs (st.velocity - static_cast<float> (s + 1) / 20.0f) < 1.0e-6f);
    }
    // arp.target = Lower: the Upper instance gets the same settings, but mode Off.
    const auto& upper = rig.proc.getArpSeq (1).settings();
    CHECK (upper.mode == vs::seq::Mode::Off);
    CHECK (upper.rate == lower.rate);
    CHECK (upper.seqLength == lower.seqLength);

    // The selector matters outside Whole mode (in Whole the arp/seq always sits in front of Lower).
    rig.set ("arp.target", 1);
    rig.render (64, {}, 64);
    CHECK (rig.proc.getArpSeq (0).settings().mode == vs::seq::Mode::Seq);
    CHECK (rig.proc.getArpSeq (1).settings().mode == vs::seq::Mode::Off);
    rig.set ("key.mode", 1);
    rig.render (64, {}, 64);
    CHECK (rig.proc.getArpSeq (0).settings().mode == vs::seq::Mode::Off);
    CHECK (rig.proc.getArpSeq (1).settings().mode == vs::seq::Mode::Seq);
    rig.set ("arp.target", 2);
    rig.set ("arp.mode", 1);
    rig.render (64, {}, 64);
    CHECK (rig.proc.getArpSeq (0).settings().mode == vs::seq::Mode::Arp);
    CHECK (rig.proc.getArpSeq (1).settings().mode == vs::seq::Mode::Arp);
}

TEST_CASE ("Wave 3 parameters: state round-trip of all 172 parameters", "[wave3][params][state]")
{
    juce::ScopedJuceInitialiser_GUI init;
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    juce::MemoryBlock saved;
    std::vector<float> values;
    {
        vs::VintageSynthProcessor proc;
        for (auto* p : proc.getParameters())
        {
            p->setValueNotifyingHost (u (rng));
            values.push_back (p->getValue());
        }
        proc.getStateInformation (saved);
    }
    vs::VintageSynthProcessor restored;
    restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    const auto& params = restored.getParameters();
    REQUIRE (params.size() == 172);
    for (int i = 0; i < params.size(); ++i)
    {
        INFO (idAt (params, i));
        CHECK (std::abs (params[i]->getValue() - values[static_cast<size_t> (i)]) < 1.0e-6f);
    }
}

// =================================================================================================
// Effects off: bit-identical to the M3 pipeline
// =================================================================================================

namespace
{
/**
    The M3 processor's signal path rebuilt from its components, without the processor: event
    parser, key router, the two Parts, the part mixer, the master gain and the output clamp, in the
    processor's call order (Whole and Dual without key-mode changes, so no latch or resume).
*/
struct M3Reference
{
    vs::Part lower, upper;
    vs::PartParams lp, up;
    vs::PartParamBinding lb, ub;
    vs::EventParser parser;
    vs::KeyRouter router;
    vs::PartMixer mixer;
    std::vector<vs::NoteEvent> le, ue;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gain;
    juce::AudioProcessorValueTreeState& state;
    double fs;
    bool whole;

    M3Reference (vs::VintageSynthProcessor& proc, double rate, int maxBlock)
        : state (proc.getState()), fs (rate)
    {
        lb.bind (state, vs::kLowerPrefix);
        ub.bind (state, vs::kUpperPrefix);
        lb.read (lp);
        ub.read (up);
        lower.setParams (lp);
        upper.setParams (up);
        lower.prepare (rate, maxBlock, proc.getOversamplingFactor(), vs::VintageSynthProcessor::kLowerSeed);
        upper.prepare (rate, maxBlock, proc.getOversamplingFactor(), vs::VintageSynthProcessor::kUpperSeed);
        whole = static_cast<int> (raw (vs::ParamID::keyMode)) == 0;
        router.setMode (whole ? vs::KeyMode::Whole : vs::KeyMode::Dual);
        parser.prepare();
        le.assign (static_cast<size_t> (parser.capacity()), vs::NoteEvent {});
        ue.assign (static_cast<size_t> (parser.capacity()), vs::NoteEvent {});
        mixer.prepare (rate);
        mixer.setTargets (raw (vs::ParamID::mixBalance), lp.pan, up.pan, whole);
        mixer.reset();
        gain.reset (rate, 0.02);
        gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (raw (vs::ParamID::masterVolume)));
    }

    float raw (const char* id) { return state.getRawParameterValue (id)->load(); }

    Stereo render (int total, const std::vector<Midi>& midi, int blockSize)
    {
        juce::ScopedNoDenormals noDenormals; // as processBlock: the release tails reach the subnormal range
        Stereo out;
        out.l.assign (static_cast<size_t> (total), 0.0f);
        out.r.assign (static_cast<size_t> (total), 0.0f);
        std::vector<float> lo (static_cast<size_t> (blockSize)), hi (static_cast<size_t> (blockSize));
        std::vector<float> l (static_cast<size_t> (blockSize)), r (static_cast<size_t> (blockSize));
        for (int pos = 0; pos < total; pos += blockSize)
        {
            const int len = std::min (blockSize, total - pos);
            lb.read (lp);
            ub.read (up);
            lower.setParams (lp);
            upper.setParams (up);
            lower.setVintage (raw (vs::ParamID::vintage));
            upper.setVintage (raw (vs::ParamID::vintage));
            gain.setTargetValue (juce::Decibels::decibelsToGain (raw (vs::ParamID::masterVolume)));
            mixer.setTargets (raw (vs::ParamID::mixBalance), lp.pan, up.pan, whole);
            parser.clear();
            for (const auto& m : midi)
                if (m.pos >= pos && m.pos < pos + len)
                    parser.add (m.msg.getRawData(), m.msg.getRawDataSize(), m.pos - pos);
            parser.finish();
            const auto c = router.route (parser.events(), le, ue);
            lower.render (lo.data(), len, { le.data(), static_cast<size_t> (c.lower) });
            if (whole)
                upper.render (hi.data(), 0, { ue.data(), static_cast<size_t> (c.upper) });
            else
                upper.render (hi.data(), len, { ue.data(), static_cast<size_t> (c.upper) });
            mixer.process (lo.data(), whole ? nullptr : hi.data(), l.data(), r.data(), len);
            for (int i = 0; i < len; ++i)
            {
                const float g = gain.getNextValue() * vs::VintageSynthProcessor::kOutputTrim;
                float a = l[static_cast<size_t> (i)] * g, b = r[static_cast<size_t> (i)] * g;
                if (! std::isfinite (a) || ! std::isfinite (b))
                    a = b = 0.0f;
                out.l[static_cast<size_t> (pos + i)] = juce::jlimit (-4.0f, 4.0f, a);
                out.r[static_cast<size_t> (pos + i)] = juce::jlimit (-4.0f, 4.0f, b);
            }
        }
        return out;
    }
};

std::vector<Midi> m3Phrase (double fs)
{
    auto at = [fs] (double s) { return static_cast<int> (std::lround (s * fs)); };
    return { noteOn (at (0.0), 48, 90), noteOn (at (0.05), 55, 70), pedal (at (0.1), true), noteOff (at (0.2), 48),
             { at (0.25), juce::MidiMessage::pitchWheel (1, 11000) }, noteOn (at (0.3), 60, 110), cc (at (0.35), 1, 80),
             noteOff (at (0.4), 55), pedal (at (0.5), false), noteOff (at (0.6), 60), noteOn (at (0.7), 64, 100),
             noteOn (at (0.7), 67, 100), noteOff (at (0.9), 64), noteOff (at (1.0), 67), noteOn (at (1.1), 50, 60),
             noteOff (at (1.3), 50) };
}
} // namespace

TEST_CASE ("Effects and arp/seq off: output bit-identical to the M3 pipeline", "[wave3][fx][bitexact]")
{
    const double fs = GENERATE (48000.0, 96000.0);
    const int keyMode = GENERATE (0, 1); // Whole, Dual
    const int block = GENERATE (512, 37);
    // Output stage (Wave 4): 0 = the limiter on at its defaults, the phrase stays below its knee
    // (transparent); 1 = the limiter off, its ceiling at -12 dB and the master at +6 dB: the phrase
    // goes far above that ceiling (off = the path without a limiter, plus the trim the reference
    // also applies).
    const int loud = GENERATE (0, 1);
    INFO ("rate " << fs << ", key mode " << keyMode << ", block " << block << ", loud " << loud);

    Rig rig (fs, 512);
    if (loud == 1)
    {
        rig.set ("master.limiter", 0);
        rig.set ("master.limiter.ceiling", -12.0f);
        rig.set ("master.volume", 6.0f);
    }
    rig.set ("global.vintage", 1.0f);
    rig.set ("key.mode", static_cast<float> (keyMode));
    rig.set ("lower.mix.pan", -0.3f);
    rig.set ("upper.mix.pan", 0.6f);
    rig.set ("mix.balance", 0.2f);
    rig.set ("upper.vco1.waveform", 3);
    rig.set ("lower.assign.mode", 2);
    rig.set ("lower.porta.time", 0.2f);
    // Every effect and arp/seq setting far from its default, except what switches them off.
    rig.set ("fx.chorus.mode", 0);
    rig.set ("fx.chorus.mix", 1.0f);
    rig.set ("fx.delay.time", 1.0f);
    rig.set ("fx.delay.feedback", 1.0f);
    rig.set ("fx.delay.damping", 0.0f);
    rig.set ("fx.delay.ping_pong", 0);
    rig.set ("fx.delay.level", 0.0f);
    rig.set ("fx.reverb.decay", 1.0f);
    rig.set ("fx.reverb.damping", 0.0f);
    rig.set ("fx.reverb.predelay", 1.0f);
    rig.set ("fx.reverb.level", 0.0f);
    rig.set ("arp.mode", 0);
    rig.set ("arp.hold", 1);
    rig.set ("arp.rate", 5);
    rig.set ("arp.gate", 100.0f);
    rig.set ("arp.target", 0);
    rig.prepare();
    rig.transport.playing = true; // a playing host must not matter while the arp is off

    M3Reference ref (rig.proc, fs, 512);
    const int total = static_cast<int> (1.6 * fs);
    const auto midi = m3Phrase (fs);
    const auto got = rig.render (total, midi, block);
    const auto want = ref.render (total, midi, block);
    CHECK (peakAbs (want.l, 0, want.l.size()) > 0.05);
    const double kneeStart = juce::Decibels::decibelsToGain (vs::kDefaultLimiterCeilingDb - vs::fx::SoftLimiter::kKneeDb);
    const double peak = std::max (peakAbs (want.l, 0, want.l.size()), peakAbs (want.r, 0, want.r.size()));
    if (loud == 0)
        CHECK (peak < kneeStart);
    else
        CHECK (peak > juce::Decibels::decibelsToGain (-12.0));
    CHECK (bitIdentical (got, want));
    CHECK (std::abs (rig.proc.getTailLengthSeconds() - rig.proc.getSynthTailSeconds()) < 1.0e-12); // no effect tail
}

TEST_CASE ("Effects off: the router's events reach the part unchanged, also one past the block end", "[wave3][fx][bitexact]")
{
    // A MIDI event at or after the end of a host block is applied after the last sample (section
    // 2). The arp/seq module moves such an event onto the last sample; while it is off the part
    // must get the router's event itself.
    Rig rig;
    rig.prepare();
    juce::AudioBuffer<float> buffer (2, 64);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 10);
    midi.addEvent (juce::MidiMessage::noteOn (1, 64, static_cast<juce::uint8> (90)), 64);  // = block length
    midi.addEvent (juce::MidiMessage::noteOn (1, 67, static_cast<juce::uint8> (80)), 300); // far past it
    rig.proc.processBlock (buffer, midi);
    const auto ev = rig.proc.getLastPartEvents (0);
    REQUIRE (ev.size() == 3);
    CHECK (ev[0].offset == 10);
    CHECK (ev[1].offset == 64);
    CHECK (ev[2].offset == 64);
    CHECK (ev[1].note == 64);
    CHECK (std::abs (ev[1].value - 90.0f / 127.0f) < 1.0e-7f);
}

TEST_CASE ("Effects switched on and off again return to the dry signal bit-exactly", "[wave3][fx][bitexact]")
{
    const double fs = 48000.0;
    auto renderWith = [&] (bool fxBurst) {
        Rig rig (fs, 256);
        rig.prepare();
        std::vector<Rig::Change> changes;
        if (fxBurst)
        {
            changes.push_back ({ rig.samples (0.1), [] (Rig& r) {
                                    r.set ("fx.chorus.mode", 1);
                                    r.set ("fx.delay.level", 0.5f);
                                    r.set ("fx.reverb.level", 0.5f);
                                } });
            changes.push_back ({ rig.samples (0.4), [] (Rig& r) {
                                    r.set ("fx.chorus.mode", 0);
                                    r.set ("fx.delay.level", 0.0f);
                                    r.set ("fx.reverb.level", 0.0f);
                                } });
        }
        std::vector<Midi> midi { noteOn (0, 57), noteOff (rig.samples (0.3), 57), noteOn (rig.samples (0.6), 62),
                                 noteOff (rig.samples (0.9), 62) };
        return rig.render (rig.samples (1.2), midi, 100, changes);
    };
    const auto dry = renderWith (false);
    const auto burst = renderWith (true);
    // During the burst the effects are audible...
    const size_t a = 4800, b = 19200;
    Stereo d1 { { dry.l.begin() + a, dry.l.begin() + b }, { dry.r.begin() + a, dry.r.begin() + b } };
    Stereo b1 { { burst.l.begin() + a, burst.l.begin() + b }, { burst.r.begin() + a, burst.r.begin() + b } };
    CHECK (diffDb (d1, b1) > -20.0);
    // ...and 20 ms (the off fade) after switching off the output is exactly the dry signal again.
    const size_t from = static_cast<size_t> (0.4 * fs + 0.02 * fs) + 1;
    Stereo d2 { { dry.l.begin() + static_cast<std::ptrdiff_t> (from), dry.l.end() }, { dry.r.begin() + static_cast<std::ptrdiff_t> (from), dry.r.end() } };
    Stereo b2 { { burst.l.begin() + static_cast<std::ptrdiff_t> (from), burst.l.end() }, { burst.r.begin() + static_cast<std::ptrdiff_t> (from), burst.r.end() } };
    CHECK (bitIdentical (d2, b2));
    CHECK (peakAbs (dry.l, static_cast<size_t> (0.6 * fs), static_cast<size_t> (0.9 * fs)) > 0.05);
}

// =================================================================================================
// Effects on
// =================================================================================================

namespace
{
double correlation (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        ab += static_cast<double> (a[i]) * static_cast<double> (b[i]);
        aa += static_cast<double> (a[i]) * static_cast<double> (a[i]);
        bb += static_cast<double> (b[i]) * static_cast<double> (b[i]);
    }
    return ab / std::sqrt (std::max (aa * bb, 1.0e-30));
}

double sideToMidDb (const Stereo& s, size_t from, size_t to)
{
    double side = 0.0, mid = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        const double m = 0.5 * (static_cast<double> (s.l[i]) + static_cast<double> (s.r[i]));
        const double d = 0.5 * (static_cast<double> (s.l[i]) - static_cast<double> (s.r[i]));
        side += d * d;
        mid += m * m;
    }
    return 10.0 * std::log10 (std::max (side, 1.0e-30) / std::max (mid, 1.0e-30));
}

Stereo renderNote (const std::function<void (Rig&)>& setup, double seconds = 2.0, double rate = 48000.0)
{
    Rig rig (rate, 512);
    setup (rig);
    rig.prepare();
    return rig.render (rig.samples (seconds), { noteOn (0, 57, 110), noteOff (rig.samples (0.5), 57) }, 512);
}
} // namespace

TEST_CASE ("Each effect is audible and stereo when on; the dry synth is mono at centre", "[wave3][fx]")
{
    const double fs = GENERATE (48000.0, 96000.0, 192000.0);
    INFO ("rate " << fs);
    const auto s = [fs] (double t) { return static_cast<size_t> (t * fs); };
    const auto dry = renderNote ([] (Rig&) {}, 2.0, fs);
    CHECK (sameBits (dry.l, dry.r)); // centre pan, no effect: L == R exactly
    const double dryTail = rms (dry.l, s (0.8), s (2.0));
    CHECK (dryTail < 1.0e-6); // the note has died away by 0.8 s

    SECTION ("chorus I and II")
    {
        for (const int mode : { 1, 2 })
        {
            INFO ("chorus mode " << mode);
            const auto c = renderNote ([mode] (Rig& r) { r.set ("fx.chorus.mode", static_cast<float> (mode)); }, 2.0, fs);
            CHECK (sideToMidDb (c, s (0.1), s (0.5)) > -25.0); // stereo
            Stereo d1 { { dry.l.begin(), dry.l.begin() + static_cast<std::ptrdiff_t> (s (0.5)) }, { dry.r.begin(), dry.r.begin() + static_cast<std::ptrdiff_t> (s (0.5)) } };
            Stereo c1 { { c.l.begin(), c.l.begin() + static_cast<std::ptrdiff_t> (s (0.5)) }, { c.r.begin(), c.r.begin() + static_cast<std::ptrdiff_t> (s (0.5)) } };
            CHECK (diffDb (d1, c1) > -15.0); // audible
        }
    }
    SECTION ("delay, ping-pong")
    {
        const auto d = renderNote ([] (Rig& r) {
            r.set ("fx.delay.level", 0.4f);
            r.set ("fx.delay.time", vs::map::delayTimeFromSec (0.3f));
        }, 2.0, fs);
        // Echoes after the dry note has died away, and they alternate between the sides.
        CHECK (toDb (rms (d.l, s (0.8), s (2.0)) + rms (d.r, s (0.8), s (2.0))) > -50.0);
        CHECK (sideToMidDb (d, s (0.8), s (2.0)) > -6.0);
    }
    SECTION ("reverb")
    {
        const auto r = renderNote ([] (Rig& rr) { rr.set ("fx.reverb.level", 0.5f); }, 2.0, fs);
        CHECK (toDb (rms (r.l, s (0.8), s (2.0))) > -50.0);
        CHECK (std::abs (correlation (r.l, r.r, s (0.8), s (2.0))) < 0.5);
    }
}

TEST_CASE ("Mono output bus is (L + R) / 2 after the effects", "[wave3][fx]")
{
    auto setup = [] (Rig& r) {
        r.set ("fx.chorus.mode", 1);
        r.set ("fx.delay.level", 0.4f);
        r.set ("fx.reverb.level", 0.3f);
        r.set ("lower.mix.pan", 0.7f);
    };
    Rig st (48000.0, 512, 2), mono (48000.0, 512, 1);
    setup (st);
    setup (mono);
    st.prepare();
    mono.prepare();
    const std::vector<Midi> midi { noteOn (0, 60), noteOff (12000, 60) };
    const auto a = st.render (48000, midi, 512);
    const auto b = mono.render (48000, midi, 512);
    size_t mismatches = 0;
    for (size_t i = 0; i < a.l.size(); ++i)
    {
        const float want = 0.5f * (a.l[i] + a.r[i]);
        mismatches += std::memcmp (&want, &b.l[i], sizeof (float)) != 0 ? 1u : 0u;
    }
    CHECK (mismatches == 0);
    CHECK (sideToMidDb (a, 4800, 48000) > -20.0); // the stereo render really is stereo
}

namespace
{
/** The synth's release tail by the contract's formula, written out here: ENV-2 ends at -100 dB,
    2.5 mapped release times (1 ms x 10000^v), x 2^(60/24) with ENV-2 key follow (note 0), x (1 + 8 %
    x Vintage); the longest of the parts that can sound (Upper not in Whole mode). */
double expectedSynthTail (float lowerRelease, float upperRelease, bool lowerKeyFollow, bool upperKeyFollow, float vintageAmount, int keyMode)
{
    auto one = [&] (float v, bool kf) {
        return 2.5 * 0.001 * std::pow (10000.0, static_cast<double> (v)) * (kf ? std::pow (2.0, 2.5) : 1.0)
               * (1.0 + 0.08 * static_cast<double> (vintageAmount));
    };
    const double l = one (lowerRelease, lowerKeyFollow);
    return keyMode == 0 ? l : std::max (l, one (upperRelease, upperKeyFollow));
}
} // namespace

TEST_CASE ("Tail length: the synth's release plus the longest tail of the effects that are on", "[wave3][fx]")
{
    juce::ScopedJuceInitialiser_GUI init;
    vs::VintageSynthProcessor proc; // not prepared: the tail is a function of the parameters
    auto set = [&] (const char* id, float plain) {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
    };
    // Defaults: effects off, ENV-2 release 0.45 (63 ms): only the synth.
    const double synth0 = expectedSynthTail (0.45f, 0.45f, false, false, 0.0f, 0);
    CHECK (std::abs (proc.getTailLengthSeconds() - synth0) < 1.0e-6);
    CHECK (std::abs (synth0 - 2.5 * 0.0631) < 1.0e-3);

    set ("fx.chorus.mode", 1);
    CHECK (std::abs (proc.getTailLengthSeconds() - (synth0 + vs::fx::Chorus::tailSeconds ({ vs::fx::ChorusMode::I, 0.5f }))) < 1.0e-6);
    set ("fx.chorus.mix", 0.0f); // mix 0 = bypass
    CHECK (std::abs (proc.getTailLengthSeconds() - synth0) < 1.0e-6);

    set ("fx.delay.level", 0.3f);
    set ("fx.delay.time", vs::map::delayTimeFromSec (0.25f));
    set ("fx.delay.feedback", 0.5f / 0.95f);
    // Delay: time x (1 + (140 dB + build-up) / per-repeat loss) + 50 ms (Delay.h), here 0.25 s at 50 %.
    const double fb = 0.5, perRepeat = -20.0 * std::log10 (fb), buildUp = 20.0 * std::log10 (1.0 / (1.0 - fb));
    const double delayTail = static_cast<double> (vs::map::delayTimeSec (vs::map::delayTimeFromSec (0.25f))) * (1.0 + (140.0 + buildUp) / perRepeat) + 0.05;
    CHECK (std::abs (proc.getTailLengthSeconds() - (synth0 + delayTail)) < 1.0e-4);

    set ("fx.reverb.level", 0.2f);
    set ("fx.reverb.decay", vs::map::reverbDecayFromSec (6.0f));
    vs::fx::ReverbParams rp { vs::map::reverbDecaySec (vs::map::reverbDecayFromSec (6.0f)), 0.5f, 0.02f, 0.2f };
    rp.preDelaySeconds = vs::map::reverbPreDelaySec (vs::map::reverbPreDelayFromSec (0.02f));
    const double reverbTail = vs::fx::Reverb::tailSeconds (rp);
    CHECK (reverbTail > delayTail);
    CHECK (std::abs (proc.getTailLengthSeconds() - (synth0 + reverbTail)) < 1.0e-6);

    // A long release feeding the reverb adds to it; Upper only counts outside Whole mode; key
    // follow and Vintage lengthen it.
    set ("lower.env2.release", 0.8f);
    set ("upper.env2.release", 0.9f);
    CHECK (std::abs (proc.getTailLengthSeconds() - (expectedSynthTail (0.8f, 0.9f, false, false, 0.0f, 0) + reverbTail)) < 1.0e-5);
    set ("key.mode", 2);
    CHECK (std::abs (proc.getTailLengthSeconds() - (expectedSynthTail (0.8f, 0.9f, false, false, 0.0f, 2) + reverbTail)) < 1.0e-5);
    set ("upper.env2.key_follow", 1);
    set ("global.vintage", 1.0f);
    CHECK (std::abs (proc.getTailLengthSeconds() - std::min (30.0, expectedSynthTail (0.8f, 0.9f, false, true, 1.0f, 2) + reverbTail)) < 1.0e-5);

    set ("fx.delay.feedback", 1.0f); // 95 %, 0.25 s: hundreds of seconds, capped at 30 in total
    CHECK (std::abs (proc.getTailLengthSeconds() - 30.0) < 1.0e-12);
    set ("fx.delay.level", 0.0f);
    set ("fx.reverb.level", 0.0f);
    set ("fx.chorus.mode", 0);
    set ("upper.env2.key_follow", 0);
    set ("lower.env2.release", 1.0f); // 10 s x 2.5 x 1.08 = 27 s
    CHECK (std::abs (proc.getTailLengthSeconds() - expectedSynthTail (1.0f, 0.9f, false, false, 1.0f, 2)) < 1.0e-5);
    set ("lower.env2.key_follow", 1); // x 5.66: capped
    CHECK (std::abs (proc.getTailLengthSeconds() - 30.0) < 1.0e-12);
}

TEST_CASE ("Tail length: the output really is silent after the reported tail", "[wave3][fx]")
{
    const double fs = 48000.0;
    struct Case
    {
        const char* name;
        std::vector<std::pair<juce::String, float>> params;
        int note;
    };
    const std::vector<Case> cases {
        { "release 1.6 s, effects off", { { "lower.env2.release", 0.8f } }, 45 },
        { "release 0.4 s + delay 0.1 s at 60 %", { { "lower.env2.release", 0.65f }, { "fx.delay.level", 1.0f },
                                                   { "fx.delay.time", vs::map::delayTimeFromSec (0.1f) }, { "fx.delay.feedback", 0.6f / 0.95f } }, 45 },
        { "release 1 s into reverb 0.6 s", { { "lower.env2.release", 0.75f }, { "fx.reverb.level", 1.0f },
                                             { "fx.reverb.decay", vs::map::reverbDecayFromSec (0.6f) } }, 45 },
        { "key follow at a low note, Vintage 1", { { "lower.env2.release", 0.55f }, { "lower.env2.key_follow", 1.0f },
                                                   { "global.vintage", 1.0f } }, 24 },
        { "Dual, Upper rings longer, chorus", { { "key.mode", 1.0f }, { "upper.env2.release", 0.75f }, { "fx.chorus.mode", 2.0f } }, 57 },
    };
    for (const auto& c : cases)
    {
        INFO (c.name);
        Rig rig (fs, 512);
        for (const auto& [id, v] : c.params)
            rig.set (id, v);
        rig.prepare();
        const double tail = rig.proc.getTailLengthSeconds();
        REQUIRE (tail > 0.3);
        REQUIRE (tail < 12.0);
        // A loud note held until its envelope has settled; the tail counts from the note-off.
        const int off = rig.samples (0.3);
        const auto out = rig.render (off + rig.samples (tail) + rig.samples (0.3), { noteOn (0, c.note, 127), noteOff (off, c.note) }, 512);
        const auto after = static_cast<size_t> (off + rig.samples (tail));
        const double peakAfter = toDb (std::max (peakAbs (out.l, after, out.l.size()), peakAbs (out.r, after, out.r.size())));
        // ...and it was not silent early in the tail (the reported tail is not trivially long).
        const auto early = static_cast<size_t> (off + rig.samples (0.2 * tail));
        const double peakEarly = toDb (peakAbs (out.l, early, early + 4800));
        std::printf ("tail %-40s reported %6.2f s: peak after %7.1f dBFS, at 20 %% of it %6.1f dBFS\n", c.name, tail, peakAfter, peakEarly);
        CHECK (peakAfter < -100.0);
        CHECK (peakEarly > -100.0);
    }
}

TEST_CASE ("Non-finite guard: resets the effects and both arp/seq instances with the parts", "[wave3][fx][arp]")
{
    Rig rig (48000.0, 512);
    rig.set ("key.mode", 1); // Dual: both parts, both arp instances
    rig.set ("arp.mode", 1);
    rig.set ("arp.rate", 3);
    rig.set ("fx.delay.level", 0.8f);
    rig.set ("fx.delay.feedback", 0.9f);
    rig.set ("fx.delay.time", vs::map::delayTimeFromSec (0.05f));
    rig.set ("fx.reverb.level", 0.5f);
    rig.prepare();
    // Keys held through the whole render: without the reset the arpeggio would go on.
    std::vector<Midi> midi { noteOn (0, 60), noteOn (0, 64) };
    const int bad = rig.samples (0.5);
    // Host parameter values are sanitised, so a NaN cannot come from a parameter any more: the
    // test hook puts one into the mix of the next chunk.
    const auto out = rig.render (rig.samples (1.5), midi, 512, { { bad, [] (Rig& r) { r.proc.forceNonFiniteForTesting(); } } });
    CHECK (rig.proc.getGuardTrips() == 1);
    for (const auto& ch : { out.l, out.r })
        for (const float x : ch)
            REQUIRE (std::isfinite (x));
    // Before: arpeggio and echoes. After the guard fired: both arp instances forgot the held keys
    // (no further steps), owe nothing, and the effect tails were dropped.
    CHECK (rig.noteOnPositions (0, 0, bad).size() >= 3); // 1/16 at 120 bpm: 4 steps
    CHECK (rig.noteOnPositions (1, 0, bad).size() >= 3);
    CHECK (rig.noteOnPositions (0, bad + 1024).empty());
    CHECK (rig.noteOnPositions (1, bad + 1024).empty());
    CHECK_FALSE (rig.proc.getArpSeq (0).isSounding());
    CHECK_FALSE (rig.proc.getArpSeq (1).isSounding());
    CHECK (toDb (peakAbs (out.l, static_cast<size_t> (bad + 1024), out.l.size())) < -120.0);
    CHECK (toDb (peakAbs (out.r, static_cast<size_t> (bad + 1024), out.r.size())) < -120.0);
    // Playing again works: a new key arpeggiates.
    rig.render (rig.samples (0.5), { noteOn (static_cast<int> (rig.now) + 100, 67) }, 512);
    CHECK (rig.noteOnPositions (0, rig.now - rig.samples (0.5)).size() >= 3);
}
