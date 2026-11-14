// The designed factory bank (M6b, docs/factory-sounds.md, docs/architecture.md 6b.4): every program
// parses, is its own sound, sounds, stays finite, releases to silence within the tail the
// processor reports, and meets the level rule.

#include <catch2/catch_test_macros.hpp>

#include "FactorySound.h"

#include <cstdio>
#include <map>
#include <set>

using namespace vs;
using namespace vs::ftest;

namespace
{
/** Level rule (docs/factory-sounds.md): standard phrase at velocity 100 within this many dB of its group's median. */
constexpr double kGroupToleranceDb = 2.0;
/** And no program's full-velocity eight-note chord more than this above Init's (RMS and peak). */
constexpr double kChordHeadroomDb = 3.0;
/** Where the output limiter's knee starts at its defaults (ceiling -1 dBFS, knee 3 dB below it). */
constexpr double kLimiterKneeDbfs = -4.0;

double medianOf (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    return v.size() % 2 == 1 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}
} // namespace

TEST_CASE ("Factory bank: 64 designed programs, all parse, distinct, known categories", "[presets][factory]")
{
    PresetRig rig;
    juce::StringArray errors;
    const auto bank = makeVintageFactoryBank (rig.schema(), &errors);
    INFO (errors.joinIntoString ("; "));
    CHECK (errors.isEmpty());
    REQUIRE (bank.programs.size() == static_cast<size_t> (kPresetBankSize));
    CHECK (bank.name == "Factory");

    const auto init = rig.schema().defaults();
    CHECK (bank.programs[0].meta.name == "Init");
    CHECK (sameValues (bank.programs[0].values, init));

    std::set<juce::String> names;
    std::map<LevelGroup, int> perGroup;
    const int splitIndex = rig.schema().indexOf ("key.split_point");
    const int modeIndex = rig.schema().indexOf ("key.mode");
    REQUIRE (splitIndex >= 0);
    REQUIRE (modeIndex >= 0);
    for (int i = 0; i < kPresetBankSize; ++i)
    {
        const auto& p = bank.programs[static_cast<size_t> (i)];
        INFO (rig.presets().positionName (i) << " " << p.meta.name << " [" << p.meta.category << "]");
        CHECK (names.insert (p.meta.name).second);
        CHECK (p.meta.author == "Exitium");
        const auto info = categoryInfo (p.meta.category);
        CHECK (info.known);
        CHECK ((info.group == LevelGroup::Reference) == (i == 0));
        ++perGroup[info.group];
        if (i == 0)
            continue;

        // Its own sound: clearly different from Init and from every other program.
        int different = 0;
        for (size_t k = 0; k < p.values.size(); ++k)
            different += sameBitsF (p.values[k], init[k]) ? 0 : 1;
        CHECK (different >= 5);
        for (int j = 1; j < i; ++j)
            CHECK_FALSE (sameValues (p.values, bank.programs[static_cast<size_t> (j)].values));

        // The category's phrase matches the key mode: Split programs split between the phrase's bass
        // line and its chord; Dual programs are Dual.
        const int mode = static_cast<int> (p.values[static_cast<size_t> (modeIndex)]);
        if (info.phrase == Phrase::SplitPhrase)
        {
            CHECK (mode == 2);
            const float split = p.values[static_cast<size_t> (splitIndex)];
            CHECK (split > 48.0f);
            CHECK (split <= 60.0f);
        }
        if (p.meta.category == "Dual")
            CHECK (mode == 1);
    }
    // The level groups are big enough for a median to mean something.
    CHECK (perGroup[LevelGroup::Sustained] >= 8);
    CHECK (perGroup[LevelGroup::Percussive] >= 8);
    CHECK (perGroup[LevelGroup::Bass] >= 8);
}

TEST_CASE ("Every factory program sounds, is finite, and releases to silence within its reported tail", "[presets][factory]")
{
    const auto& bank = factoryBank();
    for (int slot = 0; slot < kPresetBankSize; ++slot)
    {
        const auto& prog = bank.programs[static_cast<size_t> (slot)];
        FactoryRig rig (slot);
        INFO (rig.presets().positionName (slot) << " " << prog.meta.name);
        const auto perf = phraseFor (prog, rig.schema());
        const double tail = rig.proc.getTailLengthSeconds();
        CHECK (tail <= 30.0);
        const auto out = rig.play (perf, tail + 0.25);
        const auto m = measure (out, perf);
        REQUIRE (m.finite);
        INFO ("rms " << m.rmsDb << " dB, peak " << m.peakDb << " dBFS, last 100 ms " << m.endPeakDb << " dBFS");
        CHECK (m.rmsDb > -40.0);  // not silent
        CHECK (m.peakDb < 0.0);   // below full scale at the default master volume
        CHECK (m.endPeakDb < -80.0); // nothing stuck: silent once the reported tail has passed
    }
}

TEST_CASE ("Factory level rule: comparable loudness per group, and chord headroom against Init", "[presets][factory][level]")
{
    const auto& bank = factoryBank();
    std::vector<double> rms (static_cast<size_t> (kPresetBankSize));
    std::map<LevelGroup, std::vector<double>> byGroup;
    double initChordRms = 0.0, initChordPeak = 0.0;
    // Measured with the output limiter off: the rule compares programs, not the limiter.
    for (int slot = 0; slot < kPresetBankSize; ++slot)
    {
        const auto& prog = bank.programs[static_cast<size_t> (slot)];
        const auto info = categoryInfo (prog.meta.category);
        FactoryRig rig (slot, false);
        const auto perf = phraseFor (prog, rig.schema());
        const auto m = measure (rig.play (perf, 0.0), perf);
        rms[static_cast<size_t> (slot)] = m.rmsDb;
        if (info.group != LevelGroup::Reference)
            byGroup[info.group].push_back (m.rmsDb);

        FactoryRig chordRig (slot, false);
        auto chord = fullVelocityChord();
        const auto out = chordRig.play (chord, 0.0);
        chord.firstOn = at (0.55); // steady part
        const auto cm = measure (out, chord);
        INFO (rig.presets().positionName (slot) << " " << prog.meta.name << ": chord RMS " << cm.rmsDb << " dB, peak " << cm.peakDb << " dBFS");
        CHECK (cm.peakDb < 0.0); // below full scale without the limiter
        if (slot == 0)
        {
            initChordRms = cm.rmsDb;
            initChordPeak = cm.peakDb;
        }
        else
        {
            CHECK (cm.rmsDb <= initChordRms + kChordHeadroomDb);
            CHECK (cm.peakDb <= initChordPeak + kChordHeadroomDb);
        }
    }

    std::map<LevelGroup, double> median;
    for (const auto& [g, v] : byGroup)
        median[g] = medianOf (v);
    for (int slot = 1; slot < kPresetBankSize; ++slot)
    {
        const auto& prog = bank.programs[static_cast<size_t> (slot)];
        const auto g = categoryInfo (prog.meta.category).group;
        const double dev = rms[static_cast<size_t> (slot)] - median[g];
        INFO (prog.meta.name << " [" << groupName (g) << "]: " << rms[static_cast<size_t> (slot)] << " dB, median " << median[g]
                             << " dB, deviation " << dev << " dB");
        CHECK (std::abs (dev) <= kGroupToleranceDb);
    }
}

TEST_CASE ("Factory bank and the output limiter: standard phrases stay below the knee", "[presets][factory][level]")
{
    // With the limiter on at its defaults (as a new instance plays): a program's standard phrase at
    // velocity 100 must not reach the knee, so the limiter does not shape the factory sounds. The
    // full-velocity eight-note chords that do reach it are listed (expected for a few programs).
    const auto& bank = factoryBank();
    juce::StringArray chordsInKnee;
    for (int slot = 0; slot < kPresetBankSize; ++slot)
    {
        const auto& prog = bank.programs[static_cast<size_t> (slot)];
        FactoryRig rig (slot);
        const auto perf = phraseFor (prog, rig.schema());
        const auto m = measure (rig.play (perf, 0.0), perf);
        INFO (rig.presets().positionName (slot) << " " << prog.meta.name << ": phrase peak " << m.peakDb << " dBFS");
        CHECK (m.peakDb < kLimiterKneeDbfs);

        FactoryRig chordRig (slot);
        const auto chord = fullVelocityChord();
        const auto cm = measure (chordRig.play (chord, 0.0), chord);
        if (cm.peakDb >= kLimiterKneeDbfs)
            chordsInKnee.add (rig.presets().positionName (slot) + " " + prog.meta.name + " (" + juce::String (cm.peakDb, 1) + " dBFS)");
    }
    std::printf ("full-velocity chords reaching the limiter knee (%d): %s\n", chordsInKnee.size(),
                 chordsInKnee.joinIntoString (", ").toRawUTF8());
}
