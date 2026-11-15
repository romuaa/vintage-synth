// The preset API the Browser and the program menu use (docs/architecture.md 9.3.7, 9.3.8): slotMeta,
// bankGeneration, slotValue, applyToneFromSlot, and the PresetActions flows (load, save, store, tone
// files) with a test FileSource instead of dialogs. The existing preset tests are unchanged.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "PresetTestHelpers.h"
#include "common/presets/PresetActions.h"

#include <thread>

using namespace vs;
using namespace vs::presets;
using namespace vs::ptest;
using Catch::Approx;

namespace
{
/** A user bank file with a recognisable program in slot 9, made through the manager. */
struct UserBank
{
    juce::File file;
    Program stored;
};

UserBank makeUserBank (PresetRig& rig, unsigned seed = 77)
{
    auto& pm = rig.presets();
    UserBank b;
    b.file = (rig.temp / "Banks").getChildFile ("mine.vsbank");
    REQUIRE (pm.saveBankFile (b.file, "My Bank").isEmpty());
    b.stored = randomProgram (rig.schema(), seed, "Stored One");
    rig.setAll (b.stored.values);
    REQUIRE (pm.renameProgram (pm.currentIndex(), "Stored One").isEmpty());
    REQUIRE (pm.storeInBank (9).isEmpty());
    REQUIRE (pm.selectProgram (3));
    return b;
}

/** Answers every dialog at once with `file` (nothing is shown); remembers what was asked. */
struct FakeDialogs
{
    juce::File answer;
    std::vector<PresetActions::FileRequest> requests;
    juce::StringArray errors;
    void install (PresetActions& a)
    {
        a.setFileSource ([this] (const PresetActions::FileRequest& r, std::function<void (const juce::File&)> done) {
            requests.push_back (r);
            if (answer != juce::File())
                done (answer);
        });
        a.setErrorHandler ([this] (const juce::String& e) { errors.add (e); });
    }
};
} // namespace

TEST_CASE ("slotMeta: the bank's metadata by slot, the current slot shows the current program; out of range is empty", "[presets][browser]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    for (int s = 0; s < pm.bankSize(); ++s)
    {
        const auto m = pm.slotMeta (s);
        CHECK (m.name == pm.programName (s));
        CHECK (m.name.isNotEmpty());
    }
    REQUIRE (pm.selectProgram (kSlotDual));
    CHECK (pm.slotMeta (kSlotDual).name == "Twin Horizon");
    CHECK (pm.slotMeta (kSlotDual).category == pm.currentCategory());
    CHECK (pm.slotMeta (kSlotDual).author == pm.currentAuthor());
    // The current slot shows what is loaded, also after a rename of the current program.
    REQUIRE (pm.renameProgram (kSlotDual, "Renamed").isEmpty());
    CHECK (pm.slotMeta (kSlotDual).name == "Renamed");
    CHECK (pm.slotMeta (3).name == pm.programName (3));
    CHECK (pm.slotMeta (-1).name.isEmpty());
    CHECK (pm.slotMeta (pm.bankSize()).name.isEmpty());
    CHECK (pm.slotMeta (pm.bankSize()).category.isEmpty());
}

TEST_CASE ("slotMeta can be read from another thread while programs change", "[presets][browser][threads]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    std::atomic<bool> stop { false };
    std::atomic<int> bad { 0 };
    std::thread reader ([&] {
        while (! stop.load())
            for (int s = 0; s < pm.bankSize(); ++s)
                if (pm.slotMeta (s).name.isEmpty())
                    ++bad;
    });
    for (int i = 0; i < 200; ++i)
        pm.selectProgram (i % pm.bankSize());
    stop = true;
    reader.join();
    CHECK (bad.load() == 0);
}

TEST_CASE ("bankGeneration changes with the bank shown and with nothing else", "[presets][browser]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    const auto g0 = pm.bankGeneration();
    // The sound changes, the bank does not.
    pm.selectProgram (5);
    pm.applyTone (pm.captureTone (0), 1);
    pm.initialise();
    rig.set ("lower.vcf.cutoff", 0.3f);
    CHECK (pm.bankGeneration() == g0);

    // Another bank: a file saved, then loaded.
    const auto file = (rig.temp / "Banks").getChildFile ("b.vsbank");
    REQUIRE (pm.saveBankFile (file, "B").isEmpty());
    const auto g1 = pm.bankGeneration();
    CHECK (g1 != g0);
    // Storing and renaming change what the grid shows.
    REQUIRE (pm.storeInBank (7).isEmpty());
    const auto g2 = pm.bankGeneration();
    CHECK (g2 != g1);
    REQUIRE (pm.renameProgram (4, "Four").isEmpty());
    const auto g3 = pm.bankGeneration();
    CHECK (g3 != g2);
    CHECK (pm.slotMeta (4).name == "Four");
    // Back to the factory bank.
    pm.setBank ({}, juce::File());
    CHECK (pm.bankGeneration() != g3);

    // A session whose bank is resolved later: the bank shown changes twice (pending, then loaded).
    PresetRig a;
    REQUIRE (a.presets().saveBankFile ((a.temp / "Banks").getChildFile ("s.vsbank"), "S").isEmpty());
    juce::MemoryBlock state;
    a.proc.getStateInformation (state);
    PresetRig b;
    b.presets().setLocations ({ a.temp.dir });
    const auto h0 = b.presets().bankGeneration();
    b.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    const auto h1 = b.presets().bankGeneration();
    CHECK (h1 != h0);
    CHECK (b.presets().bankStatus() == BankStatus::Pending);
    b.presets().pollRequests();
    CHECK (b.presets().bankStatus() == BankStatus::User);
    CHECK (b.presets().bankGeneration() != h1);
}

TEST_CASE ("slotValue: the stored value of a parameter in a slot; NaN out of range", "[presets][browser]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    const int keyMode = rig.schema().indexOf ("key.mode");
    REQUIRE (keyMode >= 0);
    CHECK (pm.slotValue (0, keyMode) < 0.5f);             // Init is Whole
    CHECK (pm.slotValue (kSlotDual, keyMode) >= 0.5f);    // Twin Horizon is Dual
    CHECK (std::isnan (pm.slotValue (-1, keyMode)));
    CHECK (std::isnan (pm.slotValue (pm.bankSize(), keyMode)));
    CHECK (std::isnan (pm.slotValue (0, -1)));
    CHECK (std::isnan (pm.slotValue (0, rig.schema().numParams())));
}

TEST_CASE ("applyToneFromSlot: one tone of a stored program into a part; nothing else changes", "[presets][browser][tone]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    const auto bank = makeUserBank (rig);
    const auto& schema = rig.schema();
    const auto before = rig.raw();
    const int slotBefore = pm.currentIndex();

    SECTION ("Lower tone into Upper")
    {
        REQUIRE (pm.applyToneFromSlot (9, 0, 1).isEmpty());
        const auto after = rig.raw();
        for (int k = 0; k < schema.toneSize(); ++k)
        {
            CHECK (rig.matches (schema.toneParam (1, k), after[static_cast<size_t> (schema.toneParam (1, k))],
                                bank.stored.values[static_cast<size_t> (schema.toneParam (0, k))]));
            // The Lower part is unchanged.
            CHECK (sameBitsF (after[static_cast<size_t> (schema.toneParam (0, k))], before[static_cast<size_t> (schema.toneParam (0, k))]));
        }
        // Globals (key mode, effects, master volume) are untouched.
        for (int g : schema.globalParams())
            CHECK (sameBitsF (after[static_cast<size_t> (g)], before[static_cast<size_t> (g)]));
        CHECK (pm.isModified());
        CHECK (pm.currentIndex() == slotBefore);
    }
    SECTION ("Upper tone into Lower")
    {
        REQUIRE (pm.applyToneFromSlot (9, 1, 0).isEmpty());
        const auto after = rig.raw();
        for (int k = 0; k < schema.toneSize(); ++k)
        {
            CHECK (rig.matches (schema.toneParam (0, k), after[static_cast<size_t> (schema.toneParam (0, k))],
                                bank.stored.values[static_cast<size_t> (schema.toneParam (1, k))]));
            CHECK (sameBitsF (after[static_cast<size_t> (schema.toneParam (1, k))], before[static_cast<size_t> (schema.toneParam (1, k))]));
        }
    }
    SECTION ("it equals applyTone of the tone of the stored program")
    {
        PresetRig other;
        REQUIRE (other.presets().loadBankFile (bank.file).isEmpty());
        REQUIRE (other.presets().selectProgram (9));
        const auto tone = toneOf (other.schema(), other.presets().captureProgram(), 0);
        REQUIRE (other.presets().selectProgram (3));
        other.presets().applyTone (tone, 1);
        REQUIRE (pm.applyToneFromSlot (9, 0, 1).isEmpty());
        CHECK (sameValues (rig.raw(), other.raw()));
    }
    SECTION ("bad arguments change nothing and say why")
    {
        for (const auto& args : { std::array<int, 3> { -1, 0, 0 }, { 64, 0, 0 }, { 9, -1, 0 }, { 9, 2, 0 }, { 9, 0, -1 }, { 9, 0, 2 } })
            CHECK (pm.applyToneFromSlot (args[0], args[1], args[2]).isNotEmpty());
        CHECK (sameValues (rig.raw(), before));
        CHECK_FALSE (pm.isModified());
    }
}

TEST_CASE ("PresetActions: program, bank and tone files through a test dialog; a cancelled dialog does nothing", "[presets][browser][actions]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    PresetActions actions (pm);
    FakeDialogs dialogs;
    dialogs.install (actions);
    const auto original = randomProgram (rig.schema(), 5, "Round Trip");
    rig.setAll (original.values);

    SECTION ("cancelled: no file, no change, no error")
    {
        const auto before = rig.raw();
        actions.loadProgramFile();
        actions.saveProgramAs();
        actions.loadBankFile();
        actions.saveBankAs();
        actions.loadToneFile (0);
        actions.saveToneAs (1);
        CHECK (dialogs.requests.size() == 6);
        CHECK (dialogs.errors.isEmpty());
        CHECK (sameValues (rig.raw(), before));
        CHECK_FALSE ((rig.temp / "Programs").exists());
    }
    SECTION ("program: saved with the extension added, loaded back")
    {
        dialogs.answer = rig.temp / "mine"; // no extension chosen
        actions.saveProgramAs();
        REQUIRE (dialogs.errors.isEmpty());
        CHECK (dialogs.requests.back().save);
        CHECK (dialogs.requests.back().extension == ".vsprog");
        CHECK (dialogs.requests.back().folder == pm.getLocations().programs());
        const auto file = (rig.temp / "mine").withFileExtension (".vsprog");
        REQUIRE (file.existsAsFile());
        rig.setAll (randomProgram (rig.schema(), 6).values);
        dialogs.answer = file;
        actions.loadProgramFile();
        CHECK (dialogs.errors.isEmpty());
        CHECK (dialogs.requests.back().pattern == "*.vsprog");
        CHECK (rig.differences (original.values).isEmpty());
    }
    SECTION ("tone: saved from and loaded into a chosen part")
    {
        dialogs.answer = rig.temp / "t";
        actions.saveToneAs (0);
        REQUIRE (dialogs.errors.isEmpty());
        const auto file = (rig.temp / "t").withFileExtension (".vstone");
        REQUIRE (file.existsAsFile());
        dialogs.answer = file;
        actions.loadToneFile (1);
        CHECK (dialogs.errors.isEmpty());
        const auto& schema = rig.schema();
        const auto now = rig.raw();
        for (int k = 0; k < schema.toneSize(); ++k)
            CHECK (rig.matches (schema.toneParam (1, k), now[static_cast<size_t> (schema.toneParam (1, k))],
                                original.values[static_cast<size_t> (schema.toneParam (0, k))]));
    }
    SECTION ("bank: saved (it becomes the user bank), loaded by another instance")
    {
        dialogs.answer = (rig.temp / "Banks").getChildFile ("b");
        actions.saveBankAs();
        REQUIRE (dialogs.errors.isEmpty());
        CHECK (pm.bankStatus() == BankStatus::User);
        CHECK (pm.bankName() == "b");
        PresetRig other;
        PresetActions otherActions (other.presets());
        FakeDialogs otherDialogs;
        otherDialogs.install (otherActions);
        otherDialogs.answer = (rig.temp / "Banks").getChildFile ("b").withFileExtension (".vsbank");
        otherActions.loadBankFile();
        CHECK (otherDialogs.errors.isEmpty());
        CHECK (other.presets().bankStatus() == BankStatus::User);
    }
    SECTION ("files that are not ours: the strict reader refuses, the message is reported, the sound is unchanged")
    {
        const auto before = rig.raw();
        const auto bad = rig.temp / "bad.vsprog";
        REQUIRE (rig.temp.dir.createDirectory());
        REQUIRE (bad.replaceWithText ("{ not json at all"));
        dialogs.answer = bad;
        actions.loadProgramFile();
        actions.loadToneFile (0);
        dialogs.answer = rig.temp / "bad.vsprog";
        actions.loadBankFile();
        CHECK (dialogs.errors.size() == 3);
        CHECK (sameValues (rig.raw(), before));
        CHECK (pm.isFactoryBank());
    }
}

TEST_CASE ("PresetActions: save stores in place in a user bank and asks elsewhere; store into a chosen slot", "[presets][browser][actions]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    PresetActions actions (pm);
    FakeDialogs dialogs;
    dialogs.install (actions);
    juce::Component anchor;

    // Factory bank: Save has nowhere to store; the "Save as" choices go to a dialog.
    CHECK_FALSE (actions.savesInPlace());
    actions.handleSaveAsChoice (0);
    CHECK (dialogs.requests.empty());
    actions.handleSaveAsChoice (1);
    actions.handleSaveAsChoice (2);
    REQUIRE (dialogs.requests.size() == 2);
    CHECK (dialogs.requests[0].extension == ".vsprog");
    CHECK (dialogs.requests[1].extension == ".vsbank");
    actions.handleSaveAsChoice (100); // a slot of a bank that is read-only: refused with a message
    REQUIRE (dialogs.errors.size() == 1);
    CHECK (pm.isFactoryBank());

    const auto bank = makeUserBank (rig);
    CHECK (actions.savesInPlace());
    // Save: the current sound goes into the current slot and the file is written at once.
    REQUIRE (pm.selectProgram (3));
    rig.set ("lower.vcf.cutoff", 0.123f);
    REQUIRE (pm.renameProgram (3, "Edited Three").isEmpty());
    CHECK (pm.isModified());
    actions.save (anchor);
    CHECK (dialogs.errors.size() == 1); // no new error
    CHECK_FALSE (pm.isModified());
    PresetRig other;
    REQUIRE (other.presets().loadBankFile (bank.file).isEmpty());
    CHECK (other.presets().programName (3) == "Edited Three");

    // "Store in slot": any slot of the user bank, by menu id 100 + slot.
    rig.set ("lower.vcf.cutoff", 0.9f);
    actions.handleSaveAsChoice (100 + 20);
    CHECK (dialogs.errors.size() == 1);
    CHECK (pm.currentIndex() == 20);
    PresetRig third;
    REQUIRE (third.presets().loadBankFile (bank.file).isEmpty());
    REQUIRE (third.presets().selectProgram (20));
    CHECK (third.get ("lower.vcf.cutoff") == Approx (0.9f).margin (0.01));

    // Detached (the session's bank was not found): Save does not store in place.
    PresetRig a;
    REQUIRE (a.presets().saveBankFile ((a.temp / "Banks").getChildFile ("gone.vsbank"), "Gone").isEmpty());
    juce::MemoryBlock state;
    a.proc.getStateInformation (state);
    PresetRig lost;
    lost.presets().setLocations ({ lost.temp.dir }); // an empty user folder
    lost.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    lost.presets().pollRequests();
    CHECK (lost.presets().bankStatus() == BankStatus::Missing);
    PresetActions lostActions (lost.presets());
    CHECK_FALSE (lostActions.savesInPlace());
}

TEST_CASE ("PresetActions: menu results of 0 or after the owner is gone do nothing; no dialog outlives its owner", "[presets][browser][actions]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    const auto before = rig.raw();
    std::function<void (const juce::File&)> pending;
    {
        PresetActions actions (pm);
        FakeDialogs dialogs;
        dialogs.install (actions);
        actions.handleLoadChoice (0);
        actions.handleSaveChoice (0);
        actions.handleSaveAsChoice (0);
        CHECK (dialogs.requests.empty());
        // A dialog that is still open when the owner goes away.
        actions.setFileSource ([&pending] (const PresetActions::FileRequest&, std::function<void (const juce::File&)> done) { pending = std::move (done); });
        actions.loadProgramFile();
        REQUIRE (pending != nullptr);
    }
    // The callback the dialog would make now: its action holds only a weak reference to the actions object.
    const auto file = rig.temp / "x.vsprog";
    REQUIRE (rig.temp.dir.createDirectory());
    REQUIRE (pm.saveProgramFile (file, "X").isEmpty());
    pending (file);
    CHECK (sameValues (rig.raw(), before));
}
