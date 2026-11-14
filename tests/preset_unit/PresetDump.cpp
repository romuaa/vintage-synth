// Hidden "[.presetdump]" test: writes everything a user or a host can see of Vintage's presets and
// saved state to $VS_DUMP_DIR, so two builds can be compared byte for byte (`diff -r`): every factory
// program as a program file and both of its tones as tone files, the factory bank saved as a bank file,
// the saved state after each program, after a tone load, after a state restore, and the files and
// states of seeded random programs. Proves that a change to the shared preset code leaves an
// instrument without tables unchanged. It uses only API that exists in both builds. Never writes
// inside the repository; does nothing without VS_DUMP_DIR.
//
//   VS_DUMP_DIR=$TMPDIR/preset-before VintageSynthTests "[.presetdump]"
#include <catch2/catch_test_macros.hpp>

#include "PresetTestHelpers.h"

using namespace vs::ptest;
using namespace vs::presets;

namespace
{
void writeState (PresetRig& rig, const juce::File& file)
{
    juce::MemoryBlock state;
    rig.proc.getStateInformation (state);
    REQUIRE (file.replaceWithData (state.getData(), state.getSize()));
}
} // namespace

TEST_CASE ("Preset dump: program, tone and bank files and saved states", "[.presetdump]")
{
    const auto dirName = juce::SystemStats::getEnvironmentVariable ("VS_DUMP_DIR", {});
    if (dirName.isEmpty())
        SKIP ("VS_DUMP_DIR not set");
    const juce::File out (dirName);
    REQUIRE (out.createDirectory());

    PresetRig rig;
    auto& pm = rig.presets();
    for (int i = 0; i < pm.bankSize(); ++i)
    {
        const auto n = juce::String (i).paddedLeft ('0', 2);
        REQUIRE (pm.selectProgram (i));
        REQUIRE (pm.saveProgramFile (out.getChildFile ("program_" + n + ".vsprog")).isEmpty());
        REQUIRE (pm.saveToneFile (out.getChildFile ("tone_" + n + "_lower.vstone"), 0).isEmpty());
        REQUIRE (pm.saveToneFile (out.getChildFile ("tone_" + n + "_upper.vstone"), 1).isEmpty());
        writeState (rig, out.getChildFile ("state_" + n + ".bin"));
    }

    REQUIRE (pm.saveBankFile (out.getChildFile ("factory.vsbank"), "Dump").isEmpty());
    writeState (rig, out.getChildFile ("state_after_bank_save.bin"));

    // Loading files back (program, tone into the other part) and saving the state again.
    REQUIRE (pm.loadProgramFile (out.getChildFile ("program_17.vsprog")).isEmpty());
    writeState (rig, out.getChildFile ("state_after_program_load.bin"));
    REQUIRE (pm.loadToneFile (out.getChildFile ("tone_33_lower.vstone"), 1).isEmpty());
    writeState (rig, out.getChildFile ("state_after_tone_load.bin"));
    REQUIRE (pm.loadBankFile (out.getChildFile ("factory.vsbank")).isEmpty());
    writeState (rig, out.getChildFile ("state_after_bank_load.bin"));

    // A state restored from a saved state, saved again.
    juce::MemoryBlock saved;
    rig.proc.getStateInformation (saved);
    rig.proc.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    writeState (rig, out.getChildFile ("state_after_restore.bin"));

    for (unsigned seed = 1; seed <= 6; ++seed)
    {
        const auto n = juce::String (seed);
        const auto p = randomProgram (rig.schema(), seed, "Random " + n);
        pm.applyProgram (p, 5, true);
        REQUIRE (pm.saveProgramFile (out.getChildFile ("random_" + n + ".vsprog")).isEmpty());
        writeState (rig, out.getChildFile ("random_" + n + ".bin"));
        const auto text = toJson (rig.schema(), p);
        REQUIRE (out.getChildFile ("random_direct_" + n + ".vsprog").replaceWithText (juce::String (text)));
    }
    pm.initialise();
    writeState (rig, out.getChildFile ("state_after_initialise.bin"));
}
