#pragma once

// Helpers for the preset tests (docs/architecture.md 6b.5). Nothing here touches the user's real
// preset folders: every Rig gets a temporary Locations root.

#include "../integration/IntegrationRig.h"
#include "common/presets/FactoryBank.h"
#include "common/presets/PresetFormat.h"
#include "common/presets/PresetManager.h"

#include <random>

namespace vs::ptest
{
using namespace vs::itest;

struct TempDir
{
    juce::File dir;
    // A random name: ctest runs test processes in parallel, and "the next name that does not exist
    // yet" is not unique across processes (the folder is only created by a test's first save).
    TempDir() : dir (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("vs_preset_test_" + juce::Uuid().toString())) {}
    ~TempDir() { dir.deleteRecursively(); }
    juce::File operator/ (const juce::String& name) const { return dir.getChildFile (name); }
};

/** The processor in the Wave 3 rig, with presets pointed at a temp folder and the audio hand-over
    in its offline mode (no waiting: nothing runs on another thread unless a test starts it). */
struct PresetRig : Rig
{
    TempDir temp;

    explicit PresetRig (double rate = 48000.0, int maxBlockSize = 512, int numChannels = 2) : Rig (rate, maxBlockSize, numChannels)
    {
        presets().setLocations ({ temp.dir });
        presets().getGate().setTimeouts (0, 0);
    }

    presets::PresetManager& presets() { return proc.getPresets(); }
    const presets::Schema& schema() { return proc.getPresets().getSchema(); }

    /** The plain value of every parameter, by schema index. */
    std::vector<float> raw()
    {
        std::vector<float> v;
        for (int i = 0; i < schema().numParams(); ++i)
            v.push_back (proc.getState().getRawParameterValue (schema().spec (i).id)->load());
        return v;
    }

    /** Sets parameters through the host API (as a host or the UI would). */
    void setAll (const std::vector<float>& values, bool includeExcluded = false)
    {
        for (int i = 0; i < schema().numParams(); ++i)
            if (includeExcluded || ! schema().isExcluded (i))
                set (schema().spec (i).id, values[static_cast<size_t> (i)]);
    }

    /** True if parameter i holds `expected` (to float precision of a normalised round trip). */
    bool matches (int i, float actual, float expected)
    {
        const auto& s = schema().spec (i);
        return std::abs (actual - expected) <= 2.0e-6f * (s.hi - s.lo);
    }

    /** Parameters outside the excluded set that differ from `expected`: their IDs. */
    juce::StringArray differences (const std::vector<float>& expected)
    {
        juce::StringArray bad;
        const auto now = raw();
        for (int i = 0; i < schema().numParams(); ++i)
            if (! schema().isExcluded (i) && ! matches (i, now[static_cast<size_t> (i)], expected[static_cast<size_t> (i)]))
                bad.add (schema().spec (i).id);
        return bad;
    }
};

/** A program with every parameter at a pseudo-random valid value (never all defaults). */
inline presets::Program randomProgram (const presets::Schema& schema, unsigned seed, const juce::String& name = "Random")
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> u (0.0, 1.0);
    presets::Program p;
    p.meta.name = name;
    p.meta.category = "Test";
    p.meta.author = "Tester";
    p.values.resize (static_cast<size_t> (schema.numParams()));
    for (int i = 0; i < schema.numParams(); ++i)
    {
        const auto& s = schema.spec (i);
        p.values[static_cast<size_t> (i)] = schema.sanitise (i, static_cast<double> (s.lo) + u (rng) * static_cast<double> (s.hi - s.lo));
    }
    return p;
}

inline bool sameBitsF (float a, float b) { return std::memcmp (&a, &b, sizeof (float)) == 0; }

/** Float equality for tests (bitwise, with -0 == 0): GCC rejects float == under -Wfloat-equal. */
inline bool feq (float a, float b) { return sameBitsF (a + 0.0f, b + 0.0f); }

inline bool sameValues (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (! sameBitsF (a[i], b[i]))
            return false;
    return true;
}

inline std::string readText (const juce::File& f)
{
    juce::MemoryBlock mb;
    f.loadFileAsData (mb);
    return std::string (static_cast<const char*> (mb.getData()), mb.getSize());
}

// Factory slots the tests use for a role (docs/factory-sounds.md).
inline constexpr int kSlotSoloBass = 25; // D2 Rubber Floor: Solo bass
inline constexpr int kSlotDual = 48;     // G1 Twin Horizon: Dual
inline constexpr int kSlotSplit = 52;    // G5 Basement Duo: Split at G3 (55), Solo bass below
inline constexpr int kSlotArp = 56;      // H1 Glass Steps: arpeggiator
inline constexpr int kSlotSeq = 60;      // H5 Motor Line: step sequencer
inline constexpr int kSlotEffects = 62;  // H7 Deep Space Echo: chorus, delay and reverb

inline constexpr int kTestBankSize = 64;
inline constexpr const char* kTestInstrumentId = "test-synth";

/** The small schema's instrument: 8 x 8 slots, its own id and extensions. */
inline presets::Instrument testInstrument()
{
    presets::Instrument i;
    i.id = kTestInstrumentId;
    i.groups = 8;
    i.slotsPerGroup = 8;
    i.programExtension = ".tprog";
    i.toneExtension = ".ttone";
    i.bankExtension = ".tbank";
    return i;
}

/** A processor-free schema for the format tests: a few parameters of each kind, two tone prefixes. */
inline presets::Schema smallSchema()
{
    std::vector<presets::ParamSpec> all;
    auto add = [&] (const char* id, float lo, float hi, float def, bool discrete) { all.push_back ({ id, lo, hi, def, discrete }); };
    add ("a.cut", 0.0f, 1.0f, 0.6f, false);
    add ("a.wave", 0.0f, 5.0f, 2.0f, true);
    add ("a.pan", -1.0f, 1.0f, 0.0f, false);
    add ("master.vol", -60.0f, 6.0f, -6.0f, false);
    add ("b.cut", 0.0f, 1.0f, 0.6f, false);
    add ("b.wave", 0.0f, 5.0f, 2.0f, true);
    add ("b.pan", -1.0f, 1.0f, 0.0f, false);
    add ("g.mode", 0.0f, 2.0f, 0.0f, true);
    add ("g.split", 0.0f, 127.0f, 60.0f, true);
    add ("g.mix", -1.0f, 1.0f, 0.25f, false);
    return presets::Schema (std::move (all), { "a.", "b." }, { "master.vol" }, testInstrument());
}
} // namespace vs::ptest
