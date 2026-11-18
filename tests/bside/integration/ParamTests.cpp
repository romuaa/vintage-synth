// B-Side's per-part host parameters (docs/architecture.md 8.7): the 202 IDs in the contract's order, every
// range, default and label, the display text, the text -> value round trip, state and preset round trips,
// sessions and files that predate the parameters, lower/upper symmetry and the binding.
//
// The expected table below is written out by hand from the contract (8.7), not generated from the X-macro
// list in source/bside/params/PartParamList.h, so a slip in either shows up here.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <typeinfo>
#include <string>
#include <vector>

#include "bside/PluginProcessor.h"
#include "bside/params/ParameterLayout.h"
#include "bside/params/PartMappings.h"
#include "common/params/GlobalParams.h"

namespace
{
enum class Kind { Float, Int, Choice };

struct Expected
{
    const char* suffix;
    const char* name;
    Kind kind;
    float lo, hi; // Float and Int: the range
    float def;    // Float and Int: the default; Choice: the default index
    std::vector<const char*> labels; // Choice only
};

const std::vector<const char*> kOffOn { "Off", "On" };
const std::vector<const char*> kWaves { "Off", "Triangle", "Saw", "Pulse", "Noise", "Tri+Saw", "Tri+Pulse", "Saw+Pulse", "Tri+Saw+Pulse" };

void addOsc (std::vector<Expected>& t, int n)
{
    const auto p = "osc" + std::to_string (n) + ".";
    const auto q = "Osc " + std::to_string (n) + " ";
    // The strings must outlive the table: kept in a static pool.
    static std::vector<std::string> pool;
    pool.reserve (4096);
    const auto keep = [&] (const std::string& s) { pool.push_back (s); return pool.back().c_str(); };
    t.push_back ({ keep (p + "wave"), keep (q + "Wave"), Kind::Choice, 0, 8, n == 1 ? 3.0f : 0.0f, kWaves });
    t.push_back ({ keep (p + "coarse"), keep (q + "Coarse"), Kind::Int, -24, 24, 0, {} });
    t.push_back ({ keep (p + "fine"), keep (q + "Fine"), Kind::Float, -100, 100, 0, {} });
    t.push_back ({ keep (p + "pw"), keep (q + "Pulse Width"), Kind::Float, 0, 100, 50, {} });
    t.push_back ({ keep (p + "attack"), keep (q + "Attack"), Kind::Int, 0, 15, 0, {} });
    t.push_back ({ keep (p + "decay"), keep (q + "Decay"), Kind::Int, 0, 15, 0, {} });
    t.push_back ({ keep (p + "sustain"), keep (q + "Sustain"), Kind::Int, 0, 15, 15, {} });
    t.push_back ({ keep (p + "release"), keep (q + "Release"), Kind::Int, 0, 15, 6, {} });
    t.push_back ({ keep (p + "sync"), keep (q + "Sync"), Kind::Choice, 0, 1, 0, kOffOn });
    t.push_back ({ keep (p + "ring"), keep (q + "Ring Mod"), Kind::Choice, 0, 1, 0, kOffOn });
    t.push_back ({ keep (p + "filter"), keep (q + "Filter"), Kind::Choice, 0, 1, 1, kOffOn });
}

/** The 63 per-part parameters of 8.7, in order. */
const std::vector<Expected>& contract()
{
    static const std::vector<Expected> table = [] {
        std::vector<Expected> t;
        for (int n = 1; n <= 3; ++n)
            addOsc (t, n);
        t.push_back ({ "osc3.mute", "Osc 3 Mute", Kind::Choice, 0, 1, 0, kOffOn });
        t.push_back ({ "filter.cutoff", "Filter Cutoff", Kind::Float, 0, 2047, 1024, {} });
        t.push_back ({ "filter.resonance", "Filter Resonance", Kind::Int, 0, 15, 0, {} });
        t.push_back ({ "filter.mode", "Filter Mode", Kind::Choice, 0, 7, 1,
                       { "Off", "Low", "Band", "High", "Low+Band", "Band+High", "Low+High", "All" } });
        t.push_back ({ "filter.key_follow", "Filter Key Follow", Kind::Float, 0, 100, 0, {} });
        t.push_back ({ "filter.topology", "Filter Topology", Kind::Choice, 0, 1, 0, { "Per Note", "Shared" } });
        t.push_back ({ "chip.model", "Chip Model", Kind::Choice, 0, 1, 1, { "Model A", "Model B" } });
        t.push_back ({ "chip.variation", "Chip Variation", Kind::Float, 0, 100, 50, {} });
        t.push_back ({ "chip.volume", "Chip Volume", Kind::Int, 0, 15, 15, {} });
        t.push_back ({ "chip.resolution", "Resolution", Kind::Choice, 0, 1, 0, { "Smooth", "Authentic" } });
        t.push_back ({ "chip.volume_dc", "Volume DC", Kind::Choice, 0, 1, 0, kOffOn });
        t.push_back ({ "restart.policy", "Restart Policy", Kind::Choice, 0, 2, 0, { "Clean", "Precondition", "Authentic" } });
        t.push_back ({ "tick.rate", "Tick Rate", Kind::Choice, 0, 2, 0, { "Frame", "Free", "Tempo" } });
        t.push_back ({ "tick.free_hz", "Tick Free Rate", Kind::Float, 10, 400, 50, {} });
        t.push_back ({ "tick.mult", "Tick Multiplier", Kind::Int, 1, 8, 1, {} });
        t.push_back ({ "tick.sync", "Tick Sync", Kind::Choice, 0, 3, 2, { "6 per beat", "12 per beat", "24 per beat", "48 per beat" } });
        t.push_back ({ "macro.pw_offset", "PW Offset", Kind::Float, -50, 50, 0, {} });
        t.push_back ({ "macro.cutoff_offset", "Cutoff Offset", Kind::Float, -100, 100, 0, {} });
        t.push_back ({ "macro.sweep_depth", "Sweep Depth", Kind::Float, 0, 200, 100, {} });
        t.push_back ({ "vel.volume", "Velocity \xe2\x86\x92 Volume", Kind::Float, 0, 1, 0.5f, {} });
        t.push_back ({ "vel.cutoff", "Velocity \xe2\x86\x92 Cutoff", Kind::Float, 0, 1, 0, {} });
        t.push_back ({ "vibrato.rate", "Vibrato Rate", Kind::Float, 0.5f, 12, 5.5f, {} });
        t.push_back ({ "mod.wheel_vibrato", "Mod Wheel Vibrato", Kind::Float, 0, 1, 0.3f, {} });
        t.push_back ({ "assign.mode", "Assign Mode", Kind::Choice, 0, 2, 0, { "Poly", "Unison", "Solo" } });
        t.push_back ({ "assign.unison_voices", "Unison Voices", Kind::Choice, 0, 3, 1, { "2", "4", "8", "16" } });
        t.push_back ({ "unison.detune", "Unison Detune", Kind::Float, 0, 1, 0.3f, {} });
        t.push_back ({ "porta.time", "Portamento Time", Kind::Float, 0, 1, 0, {} });
        t.push_back ({ "porta.mode", "Portamento Mode", Kind::Choice, 0, 1, 0, { "Always", "Legato" } });
        t.push_back ({ "bend.range", "Bend Range", Kind::Choice, 0, 12, 2,
                       { "0 st", "1 st", "2 st", "3 st", "4 st", "5 st", "6 st", "7 st", "8 st", "9 st", "10 st", "11 st", "12 st" } });
        t.push_back ({ "mix.pan", "Pan", Kind::Float, -1, 1, 0, {} });
        return t;
    }();
    return table;
}

struct Rig
{
    juce::ScopedJuceInitialiser_GUI init;
    bs::BSideProcessor proc;

    juce::RangedAudioParameter* param (const juce::String& id)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
        REQUIRE (p != nullptr);
        return p;
    }
    void set (const juce::String& id, float plain)
    {
        auto* p = param (id);
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
    }
    float get (const juce::String& id) { return proc.getState().getRawParameterValue (id)->load(); }
    /** The text a host shows for a plain value. */
    juce::String text (const juce::String& id, float plain)
    {
        auto* p = param (id);
        return p->getText (p->convertTo0to1 (plain), 100);
    }
    /** The plain value a host gets for typed text. */
    float fromText (const juce::String& id, const juce::String& t)
    {
        auto* p = param (id);
        return p->convertFrom0to1 (p->getValueForText (t));
    }
    juce::MemoryBlock state()
    {
        juce::MemoryBlock mb;
        proc.getStateInformation (mb);
        return mb;
    }
    void restore (const juce::XmlElement& xml)
    {
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (xml, mb);
        proc.setStateInformation (mb.getData(), static_cast<int> (mb.getSize()));
    }
    std::vector<juce::String> ids()
    {
        std::vector<juce::String> v;
        for (auto* p : proc.getParameters())
            v.push_back (dynamic_cast<juce::AudioProcessorParameterWithID*> (p)->paramID);
        return v;
    }
};

/** The full ID list the contract demands, in order, from the hand-written table. */
std::vector<juce::String> expectedPartIds()
{
    std::vector<juce::String> v;
    for (const char* prefix : { "lower.", "upper." })
        for (const auto& e : contract())
            v.push_back (juce::String (prefix) + e.suffix);
    v.push_back ("chip.clock");
    return v;
}

/** A value other than the default, inside the range, for every kind. */
float otherValue (const Expected& e)
{
    switch (e.kind)
    {
        case Kind::Choice: return e.def < e.hi ? e.def + 1.0f : e.def - 1.0f;
        case Kind::Int:    return e.def < e.hi ? e.hi : e.lo;
        case Kind::Float:  return e.lo + 0.37f * (e.hi - e.lo);
    }
    return e.def;
}

void removeParamElements (juce::XmlElement& xml, const std::function<bool (const juce::String&)>& drop)
{
    for (int i = xml.getNumChildElements(); --i >= 0;)
    {
        auto* child = xml.getChildElement (i);
        const auto id = child->getStringAttribute ("id");
        if (id.isNotEmpty() && drop (id))
            xml.removeChildElement (child, true);
        else
            removeParamElements (*child, drop);
    }
}

bool isNewParameterId (const juce::String& id)
{
    return id.startsWith ("lower.") || id.startsWith ("upper.") || id == "chip.clock";
}
} // namespace

TEST_CASE ("B-Side params: 202 parameters, the 127 new IDs in the contract's order after the first 75", "[bside][params]")
{
    Rig rig;
    const auto all = rig.ids();
    REQUIRE (all.size() == 202);
    REQUIRE (bs::kParameterCount == 202);
    REQUIRE (static_cast<int> (contract().size()) == 63);
    REQUIRE (bs::kPartParamCount == 63);

    const auto expected = expectedPartIds();
    REQUIRE (expected.size() == 127);
    for (size_t i = 0; i < expected.size(); ++i)
    {
        INFO ("index " << 75 + i);
        CHECK (all[75 + i] == expected[i]);
    }
    CHECK (all[75] == "lower.osc1.wave");
    CHECK (all[75 + 62] == "lower.mix.pan");
    CHECK (all[75 + 63] == "upper.osc1.wave");
    CHECK (all[75 + 125] == "upper.mix.pan");
    CHECK (all[201] == "chip.clock");
    CHECK (bs::kLowerFirstIndex == 75);
    CHECK (bs::kUpperFirstIndex == 138);
    CHECK (bs::kChipClockIndex == 201);

    auto unique = all;
    std::sort (unique.begin(), unique.end());
    CHECK (std::adjacent_find (unique.begin(), unique.end()) == unique.end());
    for (const auto& id : all)
    {
        INFO (id);
        CHECK (id == id.toLowerCase());
        CHECK (id.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789._"));
    }

    // The 75 before them are untouched.
    CHECK (all[0] == "master.volume");
    CHECK (all[73] == "master.limiter");
    CHECK (all[74] == "master.limiter.ceiling");
}

TEST_CASE ("B-Side params: names, ranges, defaults, steps and choice labels follow the table", "[bside][params]")
{
    Rig rig;
    for (const char* prefix : { "lower.", "upper." })
        for (const auto& e : contract())
        {
            const auto id = juce::String (prefix) + e.suffix;
            INFO (id);
            auto* p = rig.param (id);
            const juce::String part = juce::String (prefix) == "lower." ? "Lower " : "Upper ";
            CHECK (p->getName (100) == part + juce::String::fromUTF8 (e.name));

            const auto& range = p->getNormalisableRange();
            switch (e.kind)
            {
                case Kind::Choice:
                {
                    auto* c = dynamic_cast<juce::AudioParameterChoice*> (p);
                    REQUIRE (c != nullptr);
                    CHECK (c->getIndex() == static_cast<int> (e.def));
                    REQUIRE (c->choices.size() == static_cast<int> (e.labels.size()));
                    for (size_t i = 0; i < e.labels.size(); ++i)
                        CHECK (c->choices[static_cast<int> (i)] == e.labels[i]);
                    CHECK (p->isDiscrete());
                    break;
                }
                case Kind::Int:
                {
                    auto* n = dynamic_cast<juce::AudioParameterInt*> (p);
                    REQUIRE (n != nullptr);
                    CHECK (n->get() == static_cast<int> (e.def));
                    CHECK (range.start == Catch::Approx (e.lo));
                    CHECK (range.end == Catch::Approx (e.hi));
                    CHECK (p->isDiscrete()); // stepped for hosts (SteppedIntParameter)
                    break;
                }
                case Kind::Float:
                {
                    auto* f = dynamic_cast<juce::AudioParameterFloat*> (p);
                    REQUIRE (f != nullptr);
                    CHECK (f->get() == Catch::Approx (e.def));
                    CHECK (range.start == Catch::Approx (e.lo));
                    CHECK (range.end == Catch::Approx (e.hi));
                    CHECK_FALSE (p->isDiscrete());
                    break;
                }
            }
        }

    auto* clock = dynamic_cast<juce::AudioParameterChoice*> (rig.param ("chip.clock"));
    REQUIRE (clock != nullptr);
    CHECK (clock->getName (100) == "Chip Clock");
    CHECK (clock->getIndex() == 0);
    REQUIRE (clock->choices.size() == 2);
    CHECK (clock->choices[0] == "50 Hz system");
    CHECK (clock->choices[1] == "60 Hz system");
}

TEST_CASE ("B-Side params: lower and upper are symmetric", "[bside][params]")
{
    Rig rig;
    for (const auto& e : contract())
    {
        INFO (e.suffix);
        auto* lo = rig.param (juce::String ("lower.") + e.suffix);
        auto* up = rig.param (juce::String ("upper.") + e.suffix);
        CHECK (lo->getName (100).fromFirstOccurrenceOf ("Lower ", false, false) == up->getName (100).fromFirstOccurrenceOf ("Upper ", false, false));
        CHECK (lo->getNormalisableRange().start == Catch::Approx (up->getNormalisableRange().start));
        CHECK (lo->getNormalisableRange().end == Catch::Approx (up->getNormalisableRange().end));
        CHECK (lo->getDefaultValue() == Catch::Approx (up->getDefaultValue()));
        CHECK (lo->isDiscrete() == up->isDiscrete());
        CHECK (lo->getAllValueStrings() == up->getAllValueStrings());
        CHECK (typeid (*lo) == typeid (*up));
        CHECK (rig.text (juce::String ("lower.") + e.suffix, lo->convertFrom0to1 (0.3f)) == rig.text (juce::String ("upper.") + e.suffix, up->convertFrom0to1 (0.3f)));
    }
}

TEST_CASE ("B-Side params: envelope times are shown as their nominal times", "[bside][params][display]")
{
    Rig rig;
    const char* attack[16] { "2 ms", "8 ms", "16 ms", "24 ms", "38 ms", "56 ms", "68 ms", "80 ms", "100 ms", "250 ms", "500 ms", "800 ms", "1 s", "3 s", "5 s", "8 s" };
    // Three times the attack times.
    const char* slow[16] { "6 ms", "24 ms", "48 ms", "72 ms", "114 ms", "168 ms", "204 ms", "240 ms", "300 ms", "750 ms", "1.5 s", "2.4 s", "3 s", "9 s", "15 s", "24 s" };
    for (const char* part : { "lower.", "upper." })
        for (int n = 1; n <= 3; ++n)
        {
            const auto base = juce::String (part) + "osc" + juce::String (n) + ".";
            for (int i = 0; i < 16; ++i)
            {
                INFO (base << " setting " << i);
                CHECK (rig.text (base + "attack", static_cast<float> (i)) == attack[i]);
                CHECK (rig.text (base + "decay", static_cast<float> (i)) == slow[i]);
                CHECK (rig.text (base + "release", static_cast<float> (i)) == slow[i]);
                CHECK (rig.text (base + "sustain", static_cast<float> (i)) == juce::String (i));
                // Typed text gives the setting back.
                CHECK (rig.fromText (base + "attack", attack[i]) == Catch::Approx (static_cast<float> (i)));
                CHECK (rig.fromText (base + "decay", slow[i]) == Catch::Approx (static_cast<float> (i)));
                CHECK (rig.fromText (base + "release", slow[i]) == Catch::Approx (static_cast<float> (i)));
            }
        }
    // Defaults as shown.
    CHECK (rig.proc.getState().getParameter ("lower.osc1.attack")->getCurrentValueAsText() == "2 ms");
    CHECK (rig.proc.getState().getParameter ("lower.osc1.release")->getCurrentValueAsText() == "204 ms"); // setting 6: 3 x 68 ms
}

TEST_CASE ("B-Side params: display text of the other parameters", "[bside][params][display]")
{
    Rig rig;
    CHECK (rig.text ("lower.osc1.coarse", 5.0f) == "+5 st");
    CHECK (rig.text ("lower.osc1.coarse", -24.0f) == "-24 st");
    CHECK (rig.text ("lower.osc1.coarse", 0.0f) == "0 st");
    CHECK (rig.text ("lower.osc2.fine", 12.0f) == "+12 ct");
    CHECK (rig.text ("lower.osc2.fine", -100.0f) == "-100 ct");
    CHECK (rig.text ("lower.osc2.fine", 0.0f) == "0 ct");
    CHECK (rig.text ("upper.osc3.pw", 50.0f) == "50.0 %");
    CHECK (rig.text ("lower.filter.cutoff", 1024.0f) == "1024");
    CHECK (rig.text ("lower.filter.cutoff", 2047.0f) == "2047");
    CHECK (rig.text ("lower.filter.resonance", 15.0f) == "15");
    CHECK (rig.text ("lower.filter.key_follow", 100.0f) == "100.0 %");
    CHECK (rig.text ("lower.chip.variation", 50.0f) == "50.0 %");
    CHECK (rig.text ("lower.chip.volume", 15.0f) == "15");
    CHECK (rig.text ("lower.tick.free_hz", 50.0f) == "50.0 Hz");
    CHECK (rig.text ("lower.tick.free_hz", 400.0f) == "400.0 Hz");
    CHECK (rig.text ("lower.tick.mult", 8.0f) == "x8");
    CHECK (rig.text ("lower.macro.pw_offset", -10.0f) == "-10.0 %");
    CHECK (rig.text ("lower.macro.pw_offset", 25.0f) == "+25.0 %");
    CHECK (rig.text ("lower.macro.pw_offset", 0.0f) == "0.0 %");
    CHECK (rig.text ("lower.macro.cutoff_offset", 100.0f) == "+100.0 %");
    CHECK (rig.text ("lower.macro.sweep_depth", 200.0f) == "200.0 %");
    CHECK (rig.text ("lower.vel.volume", 0.5f) == "50 %");
    CHECK (rig.text ("lower.vibrato.rate", 5.5f) == "5.50 Hz");
    CHECK (rig.text ("lower.mod.wheel_vibrato", 0.3f) == "30 ct");
    CHECK (rig.text ("lower.mod.wheel_vibrato", 1.0f) == "100 ct");
    CHECK (rig.text ("lower.unison.detune", 0.3f) == "15.0 ct");
    CHECK (rig.text ("lower.unison.detune", 1.0f) == "50.0 ct");
    CHECK (rig.text ("lower.porta.time", 0.0f) == "Off");
    CHECK (rig.text ("lower.mix.pan", 0.0f) == "C");
    CHECK (rig.text ("lower.mix.pan", -0.5f) == "L 50");
    CHECK (rig.proc.getState().getParameter ("lower.osc1.wave")->getCurrentValueAsText() == "Pulse");
    CHECK (rig.proc.getState().getParameter ("upper.osc2.wave")->getCurrentValueAsText() == "Off");
    CHECK (rig.proc.getState().getParameter ("lower.bend.range")->getCurrentValueAsText() == "2 st");
    CHECK (rig.proc.getState().getParameter ("chip.clock")->getCurrentValueAsText() == "50 Hz system");
}

TEST_CASE ("B-Side params: typed text gives the value back, for every parameter", "[bside][params][display]")
{
    Rig rig;
    for (const char* prefix : { "lower.", "upper." })
        for (const auto& e : contract())
        {
            const auto id = juce::String (prefix) + e.suffix;
            INFO (id);
            auto* p = rig.param (id);
            if (e.kind == Kind::Float)
            {
                // What a host shows, typed back, shows the same again (the value is only as exact as its text).
                for (float n : { 0.0f, 0.1f, 0.25f, 0.5f, 0.77f, 1.0f })
                {
                    const auto shown = p->getText (n, 100);
                    const float plain = p->convertFrom0to1 (p->getValueForText (shown));
                    INFO (shown);
                    CHECK (rig.text (id, plain) == shown);
                    CHECK (plain >= e.lo);
                    CHECK (plain <= e.hi);
                }
            }
            else
            {
                // Integers and choices: every step, exactly.
                for (int v = static_cast<int> (e.lo); v <= static_cast<int> (e.hi); ++v)
                {
                    const auto shown = rig.text (id, static_cast<float> (v));
                    INFO (shown);
                    CHECK (rig.fromText (id, shown) == Catch::Approx (static_cast<float> (v)));
                }
            }
        }
    // Typed text beyond the range is limited to it.
    CHECK (rig.fromText ("lower.osc1.coarse", "+99 st") == Catch::Approx (24.0f));
    CHECK (rig.fromText ("lower.filter.cutoff", "5000") == Catch::Approx (2047.0f));
    CHECK (rig.fromText ("lower.tick.free_hz", "1 Hz") == Catch::Approx (10.0f));
    // Envelope text without a unit counts as milliseconds; seconds are read.
    CHECK (rig.fromText ("lower.osc1.attack", "250") == Catch::Approx (9.0f));
    CHECK (rig.fromText ("lower.osc1.attack", "0.8 s") == Catch::Approx (11.0f));
    CHECK (rig.fromText ("lower.osc1.decay", "1.5 s") == Catch::Approx (10.0f));
    CHECK (rig.fromText ("lower.osc1.attack", "99999 s") == Catch::Approx (15.0f));
    CHECK (rig.fromText ("lower.osc1.attack", "garbage") == Catch::Approx (0.0f));
}

TEST_CASE ("B-Side params: the tick rate is exponential", "[bside][params]")
{
    Rig rig;
    auto* p = rig.param ("lower.tick.free_hz");
    CHECK (p->convertFrom0to1 (0.0f) == Catch::Approx (10.0f));
    CHECK (p->convertFrom0to1 (1.0f) == Catch::Approx (400.0f));
    CHECK (p->convertFrom0to1 (0.5f) == Catch::Approx (63.2456f).epsilon (1.0e-4)); // the geometric mean
    CHECK (p->convertTo0to1 (50.0f) == Catch::Approx (std::log (5.0f) / std::log (40.0f)).epsilon (1.0e-5));
    CHECK (p->getDefaultValue() == Catch::Approx (std::log (5.0f) / std::log (40.0f)).epsilon (1.0e-5));
}

TEST_CASE ("B-Side params: the mappings shared with the DSP", "[bside][params][mappings]")
{
    namespace m = bs::map;
    // Smooth resolution (8.4.6): the register values are continuous, not rounded.
    CHECK (m::pulseWidthRegister (0.0f) == Catch::Approx (0.0f));
    CHECK (m::pulseWidthRegister (50.0f) == Catch::Approx (2047.5f));
    CHECK (m::pulseWidthRegister (33.3f) == Catch::Approx (1363.635f).epsilon (1.0e-6));
    CHECK (m::pulseWidthRegister (100.0f) == Catch::Approx (4095.0f));
    CHECK (m::pulseWidthRegister (-5.0f) == Catch::Approx (0.0f));
    CHECK (m::pulseWidthRegister (500.0f) == Catch::Approx (4095.0f));
    CHECK (m::pwOffsetRegister (-50.0f) == Catch::Approx (-2048.0f));
    CHECK (m::pwOffsetRegister (50.0f) == Catch::Approx (2048.0f));
    CHECK (m::pwOffsetRegister (0.0f) == Catch::Approx (0.0f));
    CHECK (m::pwOffsetRegister (0.01f) == Catch::Approx (0.4096f).epsilon (1.0e-5)); // not rounded to 0
    CHECK (m::cutoffOffsetRegister (-100.0f) == Catch::Approx (-2047.0f));
    CHECK (m::cutoffOffsetRegister (100.0f) == Catch::Approx (2047.0f));
    CHECK (m::cutoffOffsetRegister (50.0f) == Catch::Approx (1023.5f));
    // Authentic: the integer the chip stores, rounded to nearest and limited.
    CHECK (m::authenticRegister (m::pulseWidthRegister (50.0f), 0, 4095) == 2048);
    CHECK (m::authenticRegister (m::pulseWidthRegister (100.0f), 0, 4095) == 4095);
    CHECK (m::authenticRegister (m::pulseWidthRegister (0.0f), 0, 4095) == 0);
    CHECK (m::authenticRegister (m::pwOffsetRegister (-50.0f), -2048, 2048) == -2048);
    CHECK (m::authenticRegister (m::pwOffsetRegister (25.0f), -2048, 2048) == 1024);
    CHECK (m::authenticRegister (m::cutoffOffsetRegister (50.0f), -2047, 2047) == 1024);
    CHECK (m::authenticRegister (m::cutoffOffsetRegister (-50.0f), -2047, 2047) == -1024); // halves away from zero
    CHECK (m::authenticRegister (1.4f, 0, 4095) == 1);
    CHECK (m::authenticRegister (5000.0f, 0, 4095) == 4095);
    CHECK (m::authenticRegister (-5.0f, 0, 4095) == 0);
    CHECK (m::authenticRegister (std::numeric_limits<float>::quiet_NaN(), 3, 9) == 3);
    // The tick rate: the exponential map clamps, so an out-of-range value normalises inside 0 ... 1 (a mutation
    // without the clamp fails here; the range object clamps again in JUCE, so only this direct check sees it).
    CHECK (m::tickFreeNorm (1000.0f) == Catch::Approx (1.0f));
    CHECK (m::tickFreeNorm (1.0f) == Catch::Approx (0.0f));
    CHECK (m::tickFreeNorm (0.0f) == Catch::Approx (0.0f));
    CHECK (m::tickFreeNorm (-5.0f) == Catch::Approx (0.0f));
    CHECK (m::tickFreeHz (2.0f) == Catch::Approx (400.0f));
    CHECK (m::tickFreeHz (-1.0f) == Catch::Approx (10.0f));
    // The names used by the X-macro for the wave defaults.
    CHECK (bs::kOscWavePulse == 3);
    CHECK (bs::kOscWaveOff == 0);
    // The envelope lookup is nearest in the LOG domain (decisions.md): between 250 and 500 ms the midpoint is
    // 353.6 ms (linear: 375 ms), so 360 ms is nearer 500 ms in the log domain and nearer 250 ms in the linear one.
    CHECK (m::envSettingFromMs (m::EnvStage::Attack, 360.0) == 10);
    CHECK (m::envSettingFromMs (m::EnvStage::Attack, 350.0) == 9);
    CHECK (m::envSettingFromMs (m::EnvStage::Attack, 370.0) == 10);
    CHECK (m::envSettingFromMs (m::EnvStage::Decay, 1080.0) == 10); // 750 and 1500 ms: midpoint 1061 ms (linear 1125)
    CHECK (m::envSettingFromMs (m::EnvStage::Attack, 3.0) == 0);    // 2 and 8 ms: midpoint 4 ms (linear 5 ms)
    CHECK (m::envSettingFromMs (m::EnvStage::Attack, 5.1) == 1);
    CHECK (m::envSettingFromMs (m::EnvStage::Attack, 4.1) == 1);   // log: 4.1 > 4 so 8; linear: 4.1 < 5 so 2
    CHECK (m::envSettingFromMs (m::EnvStage::Attack, -3.0) == 0);
    CHECK (m::sweepDepthFactor (100.0f) == Catch::Approx (1.0f));
    CHECK (m::sweepDepthFactor (200.0f) == Catch::Approx (2.0f));
    CHECK (m::percentToFraction (25.0f) == Catch::Approx (0.25f));
    CHECK (m::wheelVibratoCents (1.0f) == Catch::Approx (100.0f));
    CHECK (m::unisonDetuneCents (1.0f) == Catch::Approx (50.0f));
    CHECK (m::fineSemitones (-100.0f) == Catch::Approx (-1.0f));
    CHECK (m::tickFreeHz (0.0f) == Catch::Approx (10.0f));
    CHECK (m::tickFreeHz (1.0f) == Catch::Approx (400.0f));
    CHECK (m::tickFreeNorm (m::tickFreeHz (0.3f)) == Catch::Approx (0.3f).epsilon (1.0e-5));
    const int attackMs[16] { 2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000 };
    for (int i = 0; i < 16; ++i)
    {
        CHECK (m::envNominalMs (m::EnvStage::Attack, i) == attackMs[i]);
        CHECK (m::envNominalMs (m::EnvStage::Decay, i) == 3 * attackMs[i]);
        CHECK (m::envNominalMs (m::EnvStage::Release, i) == 3 * attackMs[i]);
        CHECK (m::envSettingFromMs (m::EnvStage::Attack, attackMs[i]) == i);
        CHECK (m::envSettingFromMs (m::EnvStage::Decay, 3.0 * attackMs[i]) == i);
    }
    CHECK (m::kTickSyncPerBeat == std::array<int, 4> { 6, 12, 24, 48 });
    CHECK (m::kUnisonVoiceCounts == std::array<int, 4> { 2, 4, 8, 16 });
}

TEST_CASE ("B-Side params: the state round-trips with non-default values and keeps the first 75", "[bside][params][state]")
{
    Rig a, b;
    for (const char* prefix : { "lower.", "upper." })
        for (const auto& e : contract())
            a.set (juce::String (prefix) + e.suffix, otherValue (e));
    a.set ("chip.clock", 1.0f);
    a.set ("fx.reverb.level", 0.4f);

    const auto block = a.state();
    const auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), static_cast<int> (block.getSize()));
    REQUIRE (xml != nullptr);
    CHECK (xml->getIntAttribute ("stateVersion") == 1); // nothing is released: no migration

    b.proc.setStateInformation (block.getData(), static_cast<int> (block.getSize()));
    int differing = 0;
    for (auto* p : a.proc.getParameters())
    {
        const auto id = dynamic_cast<juce::AudioProcessorParameterWithID*> (p)->paramID;
        INFO (id);
        CHECK (b.proc.getState().getParameter (id)->getValue() == Catch::Approx (p->getValue()).margin (1.0e-6));
        if (a.get (id) > b.proc.getState().getParameter (id)->getDefaultValue() + 1.0e-3f || a.get (id) < b.proc.getState().getParameter (id)->getDefaultValue() - 1.0e-3f)
            ++differing;
    }
    CHECK (differing > 100); // the test did change many parameters
    CHECK (b.get ("chip.clock") == Catch::Approx (1.0f));
    CHECK (b.get ("upper.osc3.coarse") == Catch::Approx (24.0f));
}

TEST_CASE ("B-Side params: a session without the new parameters loads them at their defaults", "[bside][params][state]")
{
    Rig a, b;
    a.set ("fx.reverb.level", 0.4f);
    a.set ("lower.osc1.coarse", 7.0f);
    const auto block = a.state();
    auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), static_cast<int> (block.getSize()));
    REQUIRE (xml != nullptr);
    removeParamElements (*xml, isNewParameterId); // what a session saved before S2 looks like
    // The session really lacks them.
    bool found = false;
    removeParamElements (*xml, [&] (const juce::String& id) { found = found || isNewParameterId (id); return false; });
    CHECK_FALSE (found);

    // The target has other values in them: they go back to the defaults, the shared ones come from the session.
    for (const char* prefix : { "lower.", "upper." })
        for (const auto& e : contract())
            b.set (juce::String (prefix) + e.suffix, otherValue (e));
    b.set ("chip.clock", 1.0f);
    b.restore (*xml);
    CHECK (b.get ("fx.reverb.level") == Catch::Approx (0.4f));
    for (const char* prefix : { "lower.", "upper." })
        for (const auto& e : contract())
        {
            const auto id = juce::String (prefix) + e.suffix;
            INFO (id);
            CHECK (b.get (id) == Catch::Approx (e.def));
        }
    CHECK (b.get ("chip.clock") == Catch::Approx (0.0f));
}

TEST_CASE ("B-Side params: a program file without the new parameters loads at defaults, a saved one carries them", "[bside][params][presets]")
{
    Rig rig;
    auto& pm = rig.proc.getPresets();
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("bs_param_test_" + juce::Uuid().toString());
    REQUIRE (dir.createDirectory());
    pm.setLocations ({ dir });

    // The Init program file as it was before S2 ("lower": {}, "upper": {}, "global": {}).
    const auto old = dir.getChildFile ("old.bsprog");
    const juce::String oldText = R"({
  "format": 1,
  "instrument": "b-side",
  "kind": "program",
  "name": "Old",
  "category": "Init",
  "author": "Exitium",
  "global": { "fx.reverb.level": 0.25 },
  "lower": {},
  "upper": {}
})";
    REQUIRE (old.replaceWithText (oldText));

    for (const char* prefix : { "lower.", "upper." })
        for (const auto& e : contract())
            rig.set (juce::String (prefix) + e.suffix, otherValue (e));
    rig.set ("chip.clock", 1.0f);
    REQUIRE (pm.loadProgramFile (old).isEmpty());
    for (const char* prefix : { "lower.", "upper." })
        for (const auto& e : contract())
            CHECK (rig.get (juce::String (prefix) + e.suffix) == Catch::Approx (e.def));
    CHECK (rig.get ("chip.clock") == Catch::Approx (0.0f));
    CHECK (rig.get ("fx.reverb.level") == Catch::Approx (0.25f));

    // Save with non-default values: the file contains them, and loading it restores them.
    for (const char* prefix : { "lower.", "upper." })
        for (const auto& e : contract())
            rig.set (juce::String (prefix) + e.suffix, otherValue (e));
    rig.set ("chip.clock", 1.0f);
    const auto saved = dir.getChildFile ("new.bsprog");
    REQUIRE (pm.saveProgramFile (saved).isEmpty());
    const auto text = saved.loadFileAsString();
    CHECK (text.contains ("osc1.wave"));
    CHECK (text.contains ("mix.pan"));
    CHECK (text.contains ("chip.clock"));
    CHECK (text.contains ("tick.free_hz"));

    for (const char* prefix : { "lower.", "upper." })
        for (const auto& e : contract())
            rig.set (juce::String (prefix) + e.suffix, e.def);
    rig.set ("chip.clock", 0.0f);
    REQUIRE (pm.loadProgramFile (saved).isEmpty());
    for (const char* prefix : { "lower.", "upper." })
        for (const auto& e : contract())
        {
            INFO (juce::String (prefix) + e.suffix);
            CHECK (rig.get (juce::String (prefix) + e.suffix) == Catch::Approx (otherValue (e)).margin (1.0e-4));
        }
    CHECK (rig.get ("chip.clock") == Catch::Approx (1.0f));
    dir.deleteRecursively();
}

TEST_CASE ("B-Side params: the binding reads the host values, sanitised", "[bside][params][binding]")
{
    Rig rig;
    bs::PartParamBinding lower, upper;
    bs::ChipClockBinding clock;
    lower.bind (rig.proc.getState(), bs::kLowerPrefix);
    upper.bind (rig.proc.getState(), bs::kUpperPrefix);
    clock.bind (rig.proc.getState());

    bs::PartParams lo, up;
    lower.read (lo);
    upper.read (up);
    // Defaults equal the struct's defaults and the contract.
    const bs::PartParams defaults;
    CHECK (lo.osc1Wave == 3);
    CHECK (up.osc2Wave == 0);
    CHECK (lo.osc1Pw == Catch::Approx (50.0f));
    CHECK (lo.filterCutoff == Catch::Approx (1024.0f));
    CHECK (lo.osc3Release == 6);
    CHECK (lo.chipModel == 1);
    CHECK (lo.tickFreeHz == Catch::Approx (50.0f));
    CHECK (lo.bendRange == 2);
    CHECK (defaults.osc1Wave == lo.osc1Wave);
    CHECK (defaults.vibratoRate == Catch::Approx (lo.vibratoRate));
    CHECK (clock.read() == 0);

    // The two parts are separate.
    rig.set ("upper.osc2.coarse", -7.0f);
    rig.set ("upper.filter.mode", 5.0f);
    rig.set ("lower.vibrato.rate", 8.0f);
    rig.set ("chip.clock", 1.0f);
    lower.read (lo);
    upper.read (up);
    CHECK (up.osc2Coarse == -7);
    CHECK (lo.osc2Coarse == 0);
    CHECK (up.filterMode == 5);
    CHECK (lo.filterMode == 1);
    CHECK (lo.vibratoRate == Catch::Approx (8.0f));
    CHECK (clock.read() == 1);

    // A value a host should never write: non-finite becomes the default, out of range is limited.
    auto* raw = rig.proc.getState().getRawParameterValue ("lower.filter.cutoff");
    raw->store (std::numeric_limits<float>::quiet_NaN());
    rig.proc.getState().getRawParameterValue ("lower.osc1.coarse")->store (1000.0f);
    rig.proc.getState().getRawParameterValue ("lower.osc1.wave")->store (-3.0f);
    rig.proc.getState().getRawParameterValue ("lower.osc2.attack")->store (std::numeric_limits<float>::infinity());
    lower.read (lo);
    CHECK (lo.filterCutoff == Catch::Approx (1024.0f));
    CHECK (lo.osc1Coarse == 24);
    CHECK (lo.osc1Wave == 0);
    CHECK (lo.osc2Attack == 0);
}

TEST_CASE ("B-Side params: typed non-finite or huge text gives a finite value inside the range, for all 127", "[bside][params][display]")
{
    Rig rig;
    std::vector<juce::String> ids;
    for (const char* prefix : { "lower.", "upper." })
        for (const auto& e : contract())
            ids.push_back (juce::String (prefix) + e.suffix);
    ids.push_back ("chip.clock");
    REQUIRE (ids.size() == 127);
    for (const auto& id : ids)
        for (const char* text : { "nan", "NaN", "-nan", "inf", "-inf", "Infinity", "1e999", "-1e999", "1e39 ms", "9999999999999999999999" })
        {
            INFO (id << " <- " << text);
            const float v = rig.param (id)->getValueForText (text);
            CHECK (std::isfinite (v));
            CHECK (v >= 0.0f);
            CHECK (v <= 1.0f);
        }
    // Not-a-number text means "no value": the default (the shared parsers behind velocity, portamento and pan said NaN).
    for (const char* id : { "lower.vel.volume", "upper.vel.cutoff", "lower.porta.time", "upper.mix.pan", "lower.mix.pan", "lower.osc1.fine", "lower.filter.cutoff",
                            "lower.osc1.coarse", "lower.osc1.attack", "lower.tick.free_hz" })
        for (const char* text : { "nan", "NaN", "inf", "1e999", "abc", "" })
        {
            INFO (id << " <- " << text);
            auto* p = rig.param (id);
            CHECK (p->getValueForText (text) == Catch::Approx (p->getDefaultValue()).margin (1.0e-6));
        }
}

TEST_CASE ("B-Side params: the first well-formed number in typed text", "[bside][params][display]")
{
    const auto num = [] (const char* t) { return bs::parseFirstNumber (t); };
    CHECK (num ("1e3").value() == Catch::Approx (1000.0));
    CHECK (num ("--5").value() == Catch::Approx (-5.0)); // the first well-formed number is "-5"
    CHECK (num ("-1e3").value() == Catch::Approx (-1000.0));
    CHECK_FALSE (num ("abc").has_value());
    CHECK_FALSE (num ("").has_value());
    CHECK_FALSE (num ("nan").has_value());
    CHECK_FALSE (num ("inf").has_value());
    CHECK_FALSE (num ("-").has_value());
    CHECK_FALSE (num (".").has_value());
    CHECK (num (" 12 ct ").value() == Catch::Approx (12.0));
    CHECK (num ("+7").value() == Catch::Approx (7.0));
    CHECK (num ("1.5 s").value() == Catch::Approx (1.5));
    CHECK (num (".5").value() == Catch::Approx (0.5));
    CHECK (num ("-.5 %").value() == Catch::Approx (-0.5));
    CHECK (num ("2e").value() == Catch::Approx (2.0)); // an exponent needs digits
    CHECK (num ("2e+").value() == Catch::Approx (2.0));
    CHECK (num ("3E-2").value() == Catch::Approx (0.03));
    CHECK (num ("x 4 y 5").value() == Catch::Approx (4.0));
    CHECK (num ("-5 -6").value() == Catch::Approx (-5.0));
    CHECK_FALSE (std::isfinite (num ("1e999").value()));

    // Through a parameter (the sign and the exponent are read, nothing else is dropped).
    Rig rig;
    CHECK (rig.fromText ("lower.macro.cutoff_offset", "1e1") == Catch::Approx (10.0f));
    CHECK (rig.fromText ("lower.macro.cutoff_offset", "-1e1 %") == Catch::Approx (-10.0f));
    CHECK (rig.fromText ("lower.macro.cutoff_offset", "--5") == Catch::Approx (-5.0f));
    CHECK (rig.fromText ("lower.macro.cutoff_offset", "-1e3") == Catch::Approx (-100.0f)); // limited to the range
    CHECK (rig.fromText ("lower.macro.cutoff_offset", " +7 ") == Catch::Approx (7.0f));
    CHECK (rig.fromText ("lower.macro.cutoff_offset", "abc") == Catch::Approx (0.0f));
    CHECK (rig.fromText ("lower.macro.cutoff_offset", "") == Catch::Approx (0.0f));
    CHECK (rig.fromText ("lower.filter.cutoff", "1e3") == Catch::Approx (1000.0f));
    CHECK (rig.fromText ("lower.filter.cutoff", "abc") == Catch::Approx (1024.0f)); // the default
    CHECK (rig.fromText ("lower.osc2.fine", " 12 ct ") == Catch::Approx (12.0f));
    CHECK (rig.fromText ("lower.osc2.fine", "+7") == Catch::Approx (7.0f));
    CHECK (rig.fromText ("lower.osc1.coarse", "-5 st") == Catch::Approx (-5.0f));
    CHECK (rig.fromText ("lower.osc1.coarse", "1e1") == Catch::Approx (10.0f));
    CHECK (rig.fromText ("lower.osc1.coarse", "abc") == Catch::Approx (0.0f));
    CHECK (rig.fromText ("lower.filter.resonance", "") == Catch::Approx (0.0f));
    CHECK (rig.fromText ("lower.osc1.sustain", "abc") == Catch::Approx (15.0f)); // default 15
    // The envelope parser reads the sign (a negative time is the shortest) and the exponent.
    CHECK (rig.fromText ("lower.osc1.attack", "-250 ms") == Catch::Approx (0.0f));
    CHECK (rig.fromText ("lower.osc1.attack", "2.5e2") == Catch::Approx (9.0f));
    CHECK (rig.fromText ("lower.osc1.attack", "0.25e3 ms") == Catch::Approx (9.0f));
    CHECK (rig.fromText ("lower.osc1.attack", "abc") == Catch::Approx (0.0f));
    CHECK (rig.fromText ("lower.osc1.release", "garbage") == Catch::Approx (6.0f)); // the default setting
    // Envelope text is nearest in the log domain: 360 ms is nearer 500 ms (setting 10), 350 ms nearer 250 ms.
    CHECK (rig.fromText ("lower.osc1.attack", "360 ms") == Catch::Approx (10.0f));
    CHECK (rig.fromText ("lower.osc1.attack", "350 ms") == Catch::Approx (9.0f));
    CHECK (rig.fromText ("lower.osc1.decay", "1.08 s") == Catch::Approx (10.0f));
}

TEST_CASE ("B-Side params: choices understand any case and a bare number that names one label", "[bside][params][display]")
{
    Rig rig;
    // A label in any case.
    CHECK (rig.fromText ("lower.osc1.sync", "on") == Catch::Approx (1.0f));
    CHECK (rig.fromText ("lower.osc1.sync", "ON") == Catch::Approx (1.0f));
    CHECK (rig.fromText ("lower.osc1.sync", " off ") == Catch::Approx (0.0f));
    CHECK (rig.fromText ("lower.osc1.wave", "pulse") == Catch::Approx (3.0f));
    CHECK (rig.fromText ("upper.osc2.wave", "TRI+SAW") == Catch::Approx (5.0f));
    CHECK (rig.fromText ("lower.filter.mode", "band+high") == Catch::Approx (5.0f));
    CHECK (rig.fromText ("chip.clock", "60 hz SYSTEM") == Catch::Approx (1.0f));
    // A bare number that is the leading number of exactly one label.
    CHECK (rig.fromText ("lower.tick.sync", "24") == Catch::Approx (2.0f));
    CHECK (rig.fromText ("lower.tick.sync", "6") == Catch::Approx (0.0f));
    CHECK (rig.fromText ("lower.tick.sync", "48") == Catch::Approx (3.0f));
    CHECK (rig.fromText ("lower.tick.sync", "012") == Catch::Approx (1.0f)); // leading zeros do not matter
    CHECK (rig.fromText ("lower.bend.range", "5") == Catch::Approx (5.0f));
    CHECK (rig.fromText ("lower.bend.range", "12") == Catch::Approx (12.0f));
    CHECK (rig.fromText ("lower.bend.range", "0") == Catch::Approx (0.0f));
    CHECK (rig.fromText ("upper.bend.range", "1") == Catch::Approx (1.0f)); // not "10 st" or "11 st"
    CHECK (rig.fromText ("lower.assign.unison_voices", "4") == Catch::Approx (1.0f));
    CHECK (rig.fromText ("lower.assign.unison_voices", "16") == Catch::Approx (3.0f));
    CHECK (rig.fromText ("chip.clock", "60") == Catch::Approx (1.0f));
    // Anything else keeps JUCE's behaviour and never leaves the range: an unknown text, a number that names no
    // label, or the "Off"/"On" switches, which have no number.
    for (const char* id : { "lower.tick.sync", "lower.bend.range", "lower.osc1.sync", "lower.osc1.wave", "chip.clock" })
        for (const char* text : { "garbage", "99", "7", "-1", "2.5", "" })
        {
            INFO (id << " <- " << text);
            const float v = rig.param (id)->getValueForText (text);
            CHECK (std::isfinite (v));
            CHECK (v >= 0.0f);
            CHECK (v <= 1.0f);
        }
    // No label starts with 7 / has a number: JUCE's answer (the first label), unchanged.
    CHECK (rig.fromText ("lower.tick.sync", "7") == Catch::Approx (0.0f));
    CHECK (rig.fromText ("lower.osc1.sync", "1") == Catch::Approx (0.0f));
}

TEST_CASE ("B-Side params: the binding turns non-finite and out-of-range host values into defaults and limits", "[bside][params][binding]")
{
    Rig rig;
    bs::PartParamBinding lower;
    bs::ChipClockBinding clock;
    lower.bind (rig.proc.getState(), bs::kLowerPrefix);
    clock.bind (rig.proc.getState());
    const auto raw = [&] (const char* id) { return rig.proc.getState().getRawParameterValue (id); };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    bs::PartParams out;

    // NaN gives the DEFAULT, also where the default is not 0 (a choice and an integer), not index or value 0.
    raw ("lower.chip.model")->store (nan);    // default Model B = 1
    raw ("lower.osc1.wave")->store (nan);     // default Pulse = 3
    raw ("lower.bend.range")->store (inf);    // default 2 st
    raw ("lower.osc1.sustain")->store (nan);  // integer, default 15
    raw ("lower.tick.mult")->store (nan);     // integer 1 ... 8, default 1 (the lowest is not 0)
    raw ("lower.osc3.release")->store (-inf); // integer, default 6
    raw ("lower.tick.sync")->store (nan);     // default 24 per beat = 2
    raw ("lower.vibrato.rate")->store (nan);  // float, default 5.5
    lower.read (out);
    CHECK (out.chipModel == 1);
    CHECK (out.osc1Wave == 3);
    CHECK (out.bendRange == 2);
    CHECK (out.osc1Sustain == 15);
    CHECK (out.tickMult == 1);
    CHECK (out.osc3Release == 6);
    CHECK (out.tickSync == 2);
    CHECK (out.vibratoRate == Catch::Approx (5.5f));

    // The upper bound is the last label, not the label count: a value equal to the count, and far above.
    struct Choice
    {
        const char* id;
        int last;
        int bs::PartParams::*member;
    };
    const Choice choices[] { { "lower.assign.mode", 2, &bs::PartParams::assignMode },
                             { "lower.filter.mode", 7, &bs::PartParams::filterMode },
                             { "lower.osc1.wave", 8, &bs::PartParams::osc1Wave },
                             { "lower.osc1.sync", 1, &bs::PartParams::osc1Sync },
                             { "lower.bend.range", 12, &bs::PartParams::bendRange },
                             { "lower.tick.sync", 3, &bs::PartParams::tickSync },
                             { "lower.assign.unison_voices", 3, &bs::PartParams::unisonVoices },
                             { "lower.restart.policy", 2, &bs::PartParams::restartPolicy } };
    for (const auto& c : choices)
        for (const float value : { static_cast<float> (c.last + 1), static_cast<float> (c.last) + 0.4f, 1000.0f, 3.0e9f })
        {
            INFO (c.id << " raw " << value);
            raw (c.id)->store (value);
            lower.read (out);
            CHECK (out.*c.member == c.last);
            raw (c.id)->store (-5.0f);
            lower.read (out);
            CHECK (out.*c.member == 0);
        }
    for (const float value : { 2.0f, 3.0f, 1000.0f, 3.0e9f })
    {
        INFO ("chip.clock raw " << value);
        raw ("chip.clock")->store (value);
        CHECK (clock.read() == 1);
    }
    raw ("chip.clock")->store (-3.0f);
    CHECK (clock.read() == 0);
    raw ("chip.clock")->store (nan);
    CHECK (clock.read() == 0);
    raw ("chip.clock")->store (inf); // non-finite: the default, not the top
    CHECK (clock.read() == 0);
}

namespace
{
const Expected* contractEntry (const char* id)
{
    for (const auto& e : contract())
        if (juce::String (e.suffix) == id)
            return &e;
    return nullptr;
}
} // namespace

TEST_CASE ("B-Side params: every member of a default PartParams is the contract's default and what a fresh processor reads", "[bside][params][binding]")
{
    Rig rig;
    bs::PartParamBinding lower, upper;
    lower.bind (rig.proc.getState(), bs::kLowerPrefix);
    upper.bind (rig.proc.getState(), bs::kUpperPrefix);
    bs::PartParams lo, up;
    lower.read (lo);
    upper.read (up);
    const bs::PartParams defaults;
    int members = 0;
#define BS_T_FLOAT(member, id, name, mn, mx, dflt, kind)       \
    {                                                         \
        INFO (id);                                            \
        const auto* e = contractEntry (id);                   \
        REQUIRE (e != nullptr);                               \
        CHECK (defaults.member == Catch::Approx (e->def));    \
        CHECK (lo.member == Catch::Approx (defaults.member)); \
        CHECK (up.member == Catch::Approx (defaults.member)); \
        ++members;                                            \
    }
#define BS_T_INT(member, id, name, mn, mx, dflt, kind)         \
    {                                                         \
        INFO (id);                                            \
        const auto* e = contractEntry (id);                   \
        REQUIRE (e != nullptr);                               \
        CHECK (defaults.member == static_cast<int> (e->def)); \
        CHECK (lo.member == defaults.member);                 \
        CHECK (up.member == defaults.member);                 \
        ++members;                                            \
    }
#define BS_T_CHOICE(member, id, name, labels, dflt) BS_T_INT (member, id, name, 0, 0, dflt, 0)
    BS_PART_PARAMS (BS_T_FLOAT, BS_T_CHOICE, BS_T_INT)
#undef BS_T_FLOAT
#undef BS_T_INT
#undef BS_T_CHOICE
    CHECK (members == 63);
}
