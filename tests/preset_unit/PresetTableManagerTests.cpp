// Patch tables in the preset manager (docs/architecture.md 8.8), generic layer: a second manager
// with tables switched on is built over the Vintage processor's parameters, with a stand-in
// validator and a sink that records when and with what it is called. The real B-Side tables
// (compiled and handed to the audio thread) are in tests/bside/integration/TablePresetTests.cpp.
#include <catch2/catch_test_macros.hpp>

#include "PresetTestHelpers.h"

#include <chrono>
#include <thread>

using namespace vs;
using namespace vs::presets;
using namespace vs::ptest;

namespace
{
struct Call
{
    int tone;
    std::vector<std::string> lines;
    bool holding;
    int holdDepth;
    float markerValue; // the value of a tone parameter at the time of the call
};

struct RecordedTable final : PreparedTable
{
    std::vector<std::string> lines;
};

struct RecordingSink final : TableSink
{
    ApplyGate* gate = nullptr;
    std::atomic<float>* marker = nullptr;
    std::vector<Call> calls;
    int prepares = 0;
    bool preparedInsideHold = false; // must stay false: nothing is prepared inside a hold
    bool failNext = false, throwNext = false;
    RecordedTable none;

    std::unique_ptr<PreparedTable> prepareTable (int, const std::vector<std::string>& lines) override
    {
        ++prepares;
        if (gate->isHolding())
            preparedInsideHold = true;
        if (throwNext)
        {
            throwNext = false;
            throw std::bad_alloc();
        }
        if (failNext)
        {
            failNext = false;
            return nullptr;
        }
        auto t = std::make_unique<RecordedTable>();
        t->lines = lines;
        return t;
    }
    void publishTable (int tone, PreparedTable& prepared) noexcept override
    {
        calls.push_back ({ tone, static_cast<RecordedTable&> (prepared).lines, gate->isHolding(), gate->holdDepth(), marker->load() });
    }
    PreparedTable& emptyTable (int) noexcept override { return none; }
};

/** The Vintage processor plus a manager with tables, over the same parameters and gate. */
struct TableRig
{
    PresetRig rig;
    RecordingSink* sink = nullptr;
    std::unique_ptr<PresetManager> pm;
    Schema schema;
    juce::String markerId;

    TableRig()
    {
        // The plain manager of the rig stays idle; the table manager below is the one under test.
        schema = Schema::fromProcessor (rig.proc, { "lower.", "upper." },
                                        { "master.volume", "master.limiter", "master.limiter.ceiling" }, tableTestInstrument());
        REQUIRE (schema.isValid());
        auto s = std::make_unique<RecordingSink>();
        sink = s.get();
        sink->gate = &rig.presets().getGate();
        markerId = schema.spec (schema.toneParam (0, 0)).id;
        sink->marker = rig.proc.getState().getRawParameterValue (markerId);
        pm = std::make_unique<PresetManager> (rig.proc.getState(), schema, makeInitBank (schema), rig.presets().getGate(),
                                              Locations { rig.temp.dir }, std::move (s));
    }

    Program programWithTables (std::vector<std::string> lower, std::vector<std::string> upper, float marker = 0.5f)
    {
        auto p = makeInitProgram (schema);
        p.meta.name = "Tabby";
        const int i = schema.toneParam (0, 0);
        p.values[static_cast<size_t> (i)] = schema.sanitise (i, static_cast<double> (schema.spec (i).lo)
                                                              + static_cast<double> (marker) * static_cast<double> (schema.spec (i).hi - schema.spec (i).lo));
        p.data = { std::move (lower), std::move (upper) };
        return p;
    }
};

} // namespace

TEST_CASE ("Table manager: applying a program publishes its tables inside the hold, with the parameters", "[presets][tables][manager]")
{
    TableRig t;
    auto p = t.programWithTables ({ "ok one", "ok two   " }, {});
    t.pm->applyProgram (p, 7, true);

    REQUIRE (t.sink->calls.size() == 2); // one per tone, in order
    CHECK (t.sink->calls[0].tone == 0);
    CHECK (t.sink->calls[1].tone == 1);
    for (const auto& c : t.sink->calls)
    {
        CHECK (c.holding);
        CHECK (c.holdDepth == 1);
    }
    // Verbatim (trailing spaces kept), published and held.
    CHECK (t.sink->calls[0].lines == std::vector<std::string> { "ok one", "ok two   " });
    CHECK_FALSE (t.sink->preparedInsideHold);
    CHECK (t.sink->calls[1].lines.empty());
    CHECK (t.pm->currentTable (0) == t.sink->calls[0].lines);
    CHECK (t.pm->currentTable (1).empty());
    CHECK (t.pm->tableError (0).isEmpty());
    // The parameters of the same apply were written by the time the tables were handed over.
    const int i = t.schema.toneParam (0, 0);
    CHECK (t.rig.matches (i, t.sink->calls[0].markerValue, p.values[static_cast<size_t> (i)]));

    // captureProgram carries them; applying Init clears them (and publishes the clearing).
    CHECK (t.pm->captureProgram().data[0] == t.pm->currentTable (0));
    t.sink->calls.clear();
    t.pm->initialise();
    REQUIRE (t.sink->calls.size() == 2);
    CHECK (t.sink->calls[0].lines.empty());
    CHECK (t.pm->currentTable (0).empty());
    CHECK (t.rig.presets().getGate().holdDepth() == 0);
}

TEST_CASE ("Table manager: applying a tone replaces only that part's table", "[presets][tables][manager]")
{
    TableRig t;
    t.pm->applyProgram (t.programWithTables ({ "ok lower" }, { "ok upper" }), 0, false);
    t.sink->calls.clear();

    Tone tone;
    tone.meta.name = "T";
    tone.values = toneOf (t.schema, makeInitProgram (t.schema), 0).values;
    tone.data = { "ok from tone" };
    t.pm->applyTone (tone, 1);
    REQUIRE (t.sink->calls.size() == 1);
    CHECK (t.sink->calls[0].tone == 1);
    CHECK (t.sink->calls[0].holding);
    CHECK (t.pm->currentTable (0) == std::vector<std::string> { "ok lower" });
    CHECK (t.pm->currentTable (1) == std::vector<std::string> { "ok from tone" });
    CHECK (t.pm->isModified());
}

TEST_CASE ("Table manager: setTable validates, publishes in a hold and marks the program modified", "[presets][tables][manager]")
{
    TableRig t;
    t.pm->applyProgram (t.programWithTables ({ "ok keep" }, {}), 3, false);
    REQUIRE_FALSE (t.pm->isModified());
    t.sink->calls.clear();

    const auto bad = t.pm->setTable (0, { "ok fine", "nope" });
    CHECK (bad.startsWith ("Lower table: line 2"));
    CHECK (t.sink->calls.empty());
    CHECK (t.pm->currentTable (0) == std::vector<std::string> { "ok keep" });
    CHECK_FALSE (t.pm->isModified());
    CHECK (t.pm->setTable (5, {}).isNotEmpty());
    CHECK (t.pm->setTable (-1, {}).isNotEmpty());
    CHECK (t.pm->setTable (0, { "ok\nx" }).isNotEmpty()); // a line break inside a line
    CHECK (t.pm->setTable (0, std::vector<std::string> (65, "ok")).isNotEmpty());

    CHECK (t.pm->setTable (1, { "ok new  " }).isEmpty());
    REQUIRE (t.sink->calls.size() == 1);
    CHECK (t.sink->calls[0].tone == 1);
    CHECK (t.sink->calls[0].holding);
    CHECK (t.pm->currentTable (1) == std::vector<std::string> { "ok new  " }); // verbatim
    CHECK_FALSE (t.sink->preparedInsideHold);
    CHECK (t.pm->isModified());
    CHECK (t.pm->setTable (1, {}).isEmpty()); // empty = no table
    CHECK (t.pm->currentTable (1).empty());
}

TEST_CASE ("Table manager: an unusable table in memory becomes no table plus an error; the rest still applies", "[presets][tables][manager]")
{
    TableRig t;
    auto p = t.programWithTables ({ "ok fine" }, { "bad" }, 0.25f);
    t.pm->applyProgram (p, 0, false);
    CHECK (t.pm->currentTable (0) == std::vector<std::string> { "ok fine" });
    CHECK (t.pm->currentTable (1).empty());
    CHECK (t.pm->tableError (1).startsWith ("Upper table of \"Tabby\": line 1"));
    CHECK (t.pm->tableError (0).isEmpty());
    const int i = t.schema.toneParam (0, 0);
    CHECK (t.rig.matches (i, t.rig.proc.getState().getRawParameterValue (t.markerId)->load(), p.values[static_cast<size_t> (i)]));
    // The next apply clears the error.
    t.pm->applyProgram (t.programWithTables ({}, { "ok" }), 0, false);
    CHECK (t.pm->tableError (1).isEmpty());

    // A table too big for the instrument, and a canonicaliser that misbehaves.
    p = t.programWithTables ({}, {});
    p.data[0] = std::vector<std::string> (65, "ok");
    t.pm->applyProgram (p, 0, false);
    CHECK (t.pm->currentTable (0).empty());
    CHECK (t.pm->tableError (0).contains ("more than 64 lines"));

}

TEST_CASE ("Table manager: a table the sink cannot prepare is rejected before the hold, so audio and state agree", "[presets][tables][manager]")
{
    TableRig t;
    t.pm->applyProgram (t.programWithTables ({ "ok keep" }, { "ok keep too" }), 0, false);
    t.sink->calls.clear();
    for (const bool throws : { false, true })
    {
        (throws ? t.sink->throwNext : t.sink->failNext) = true;
        t.pm->applyProgram (t.programWithTables ({ "ok new" }, {}, 0.75f), 0, false);
        // The rejected table is empty everywhere: held, saved and published as "no table"; the rest applied.
        CHECK (t.pm->currentTable (0).empty());
        CHECK (t.pm->tableError (0).contains ("could not be prepared"));
        REQUIRE (t.sink->calls.size() == 2);
        CHECK (t.sink->calls[0].lines.empty());
        CHECK (t.sink->calls[0].holding);
        CHECK (t.pm->createTablesInfo().getProperty ("lower").toString().isEmpty());
        CHECK_FALSE (t.sink->preparedInsideHold);
        t.sink->calls.clear();
    }
    // setTable: nothing changes, nothing is published.
    t.pm->applyProgram (t.programWithTables ({ "ok keep" }, {}), 0, false);
    t.sink->calls.clear();
    t.sink->failNext = true;
    CHECK (t.pm->setTable (0, { "ok other" }).contains ("could not be prepared"));
    CHECK (t.pm->currentTable (0) == std::vector<std::string> { "ok keep" });
    CHECK (t.sink->calls.empty());
}

namespace
{
struct HostListener final : juce::AudioProcessorListener
{
    int nonParameterChanges = 0, programChanges = 0;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override
    {
        nonParameterChanges += d.nonParameterStateChanged ? 1 : 0;
        programChanges += d.programChanged ? 1 : 0;
    }
};
} // namespace

TEST_CASE ("Table manager: the host is told when a table changes; the modified marker follows the canonical form", "[presets][tables][manager]")
{
    TableRig t;
    HostListener listener;
    t.rig.proc.addListener (&listener);
    t.pm->applyProgram (t.programWithTables ({ "ok one" }, {}), 0, false, false); // tellHost = false: no program change message
    CHECK (listener.nonParameterChanges == 0);
    REQUIRE_FALSE (t.pm->isModified());

    // A different text with the same canonical form (the stand-in trims trailing spaces): host told, not "modified".
    CHECK (t.pm->setTable (0, { "ok one   " }).isEmpty());
    CHECK (listener.nonParameterChanges == 1);
    CHECK (t.pm->currentTable (0) == std::vector<std::string> { "ok one   " });
    CHECK_FALSE (t.pm->isModified());
    // The very same text again: nothing to tell.
    CHECK (t.pm->setTable (0, { "ok one   " }).isEmpty());
    CHECK (listener.nonParameterChanges == 1);
    // A different table: modified, host told.
    CHECK (t.pm->setTable (0, { "ok two" }).isEmpty());
    CHECK (listener.nonParameterChanges == 2);
    CHECK (t.pm->isModified());
    // A rejected table tells nothing.
    CHECK (t.pm->setTable (0, { "bad" }).isNotEmpty());
    CHECK (listener.nonParameterChanges == 2);

    // A tone that differs from the part only in its table (the parameters are what they are already).
    auto tone = t.pm->captureTone (0);
    tone.data = { "ok three" };
    t.pm->applyTone (tone, 0);
    CHECK (listener.nonParameterChanges == 3);
    tone.data = { "ok three" }; // same again: no news
    t.pm->applyTone (tone, 0);
    CHECK (listener.nonParameterChanges == 3);

    // Only on the message thread: from another thread the table still changes, the host is not called.
    std::thread other ([&] { juce::String e = t.pm->setTable (1, { "ok from elsewhere" }); (void) e; });
    other.join();
    CHECK (listener.nonParameterChanges == 3);
    CHECK (t.pm->currentTable (1) == std::vector<std::string> { "ok from elsewhere" });
    t.rig.proc.removeListener (&listener);
}

TEST_CASE ("Table manager: error messages are cut between characters", "[presets][tables][manager]")
{
    // A validator whose message is full of multi-byte characters, at every offset around the limit.
    for (int lead = 0; lead < 4; ++lead)
        for (const char* unit : { "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x8e\xb9" })
        {
            auto inst = tableTestInstrument();
            const juce::String unitText = juce::String::fromUTF8 (unit);
            inst.validateTable = [=] (const std::vector<std::string>&) { return juce::String (std::string (static_cast<size_t> (lead), 'x')) + juce::String::repeatedString (unitText, 700); };
            const auto reply = validateTableLines (inst, { "ok" });
            INFO ("lead " << lead << " unit bytes " << std::strlen (unit));
            CHECK (reply.getNumBytesAsUTF8() <= 1024);
            CHECK (juce::CharPointer_UTF8::isValidString (reply.toRawUTF8(), 4096));
            CHECK (reply.getNumBytesAsUTF8() > 1024 - 4); // cut as late as a character boundary allows
            CHECK (reply.endsWith (unitText)); // never ends in a broken character
        }
    CHECK (truncateUtf8 ("short", 100) == "short");
    CHECK (truncateUtf8 ("", 0).isEmpty());
    CHECK (truncateUtf8 (juce::String::fromUTF8 ("\xc3\xa9"), 1).isEmpty()); // the cut would fall inside the only character
    CHECK (truncateUtf8 (juce::String::fromUTF8 ("a\xc3\xa9"), 2) == "a");
}

TEST_CASE ("Table manager: nothing is saved that this instrument's own reader would refuse", "[presets][tables][manager][files]")
{
    // An instrument whose file cap is smaller than a bank file (a bank of 64 programs is tens of kilobytes).
    auto inst = tableTestInstrument();
    inst.maxFileBytes = 20000;
    PresetRig rig;
    auto schema = Schema::fromProcessor (rig.proc, { "lower.", "upper." }, { "master.volume", "master.limiter", "master.limiter.ceiling" }, inst);
    PresetManager pm (rig.proc.getState(), schema, makeInitBank (schema), rig.presets().getGate(), Locations { rig.temp.dir });
    const auto bankFile = (rig.temp / "b").withFileExtension (".tbank");
    const auto error = pm.saveBankFile (bankFile, "Too big");
    CHECK (error.contains ("Not saved"));
    CHECK (error.contains ("20000"));
    CHECK_FALSE (bankFile.exists());
    CHECK_FALSE (rig.temp.dir.exists()); // not even the folder or a temporary file
    CHECK (pm.bankStatus() == BankStatus::Factory); // nothing changed
    // A program file fits.
    const auto programFile = (rig.temp / "p").withFileExtension (".tprog");
    CHECK (pm.saveProgramFile (programFile).isEmpty());
    CHECK (programFile.getSize() <= 20000);
    // Every save goes through the same check: with a cap smaller than a program the program file is refused.
    auto tiny = tableTestInstrument();
    tiny.maxFileBytes = 300;
    auto schemaTiny = Schema::fromProcessor (rig.proc, { "lower.", "upper." }, { "master.volume", "master.limiter", "master.limiter.ceiling" }, tiny);
    PresetManager pmTiny (rig.proc.getState(), schemaTiny, makeInitBank (schemaTiny), rig.presets().getGate(), Locations { rig.temp.dir });
    const auto tinyFile = (rig.temp / "t").withFileExtension (".tprog");
    CHECK (pmTiny.saveProgramFile (tinyFile).contains ("Not saved"));
    CHECK_FALSE (tinyFile.exists());
}

TEST_CASE ("Table manager: files carry the tables; a rejected file changes nothing", "[presets][tables][manager][files]")
{
    TableRig t;
    t.pm->applyProgram (t.programWithTables ({ "ok lower" }, { "ok upper", "ok more" }), 4, false);
    const auto programFile = t.rig.temp / "p.vsprog";
    REQUIRE (t.pm->saveProgramFile (programFile.withFileExtension (".tprog")).isEmpty());
    const auto saved = readText (programFile.withFileExtension (".tprog"));
    CHECK (saved.find ("\"tables\"") != std::string::npos);
    CHECK (saved.find ("\"ok more\"") != std::string::npos);

    // Change everything, then load: the tables come back.
    t.pm->applyProgram (t.programWithTables ({}, {}, 0.9f), 4, true);
    CHECK (t.pm->currentTable (1).empty());
    REQUIRE (t.pm->loadProgramFile (programFile.withFileExtension (".tprog")).isEmpty());
    CHECK (t.pm->currentTable (0) == std::vector<std::string> { "ok lower" });
    CHECK (t.pm->currentTable (1) == std::vector<std::string> { "ok upper", "ok more" });

    // Tone files: saved from a part, loaded into the other.
    const auto toneFile = (t.rig.temp / "t").withFileExtension (".ttone");
    REQUIRE (t.pm->saveToneFile (toneFile, 1).isEmpty());
    REQUIRE (t.pm->loadToneFile (toneFile, 0).isEmpty());
    CHECK (t.pm->currentTable (0) == t.pm->currentTable (1));

    // A bad table in a file: rejected, nothing changes (tables, parameters, calls).
    auto bad = saved;
    const auto at = bad.find ("\"ok lower\"");
    REQUIRE (at != std::string::npos);
    bad.replace (at, 10, "\"wrong\"");
    const auto badFile = (t.rig.temp / "bad").withFileExtension (".tprog");
    REQUIRE (badFile.replaceWithText (juce::String (bad)));
    const auto tablesBefore = std::pair { t.pm->currentTable (0), t.pm->currentTable (1) };
    const auto valuesBefore = t.rig.raw();
    const auto callsBefore = t.sink->calls.size();
    const auto error = t.pm->loadProgramFile (badFile);
    CHECK (error.contains ("Lower table of \"Tabby\": line 1, column 1"));
    CHECK (t.pm->currentTable (0) == tablesBefore.first);
    CHECK (t.pm->currentTable (1) == tablesBefore.second);
    CHECK (sameValues (t.rig.raw(), valuesBefore));
    CHECK (t.sink->calls.size() == callsBefore);

    // A user bank: save, edit a slot, reload; selecting a slot applies its tables.
    REQUIRE (t.pm->saveBankFile ((t.rig.temp / "b").withFileExtension (".tbank"), "Mine").isEmpty());
    CHECK (t.pm->setTable (0, { "ok slot five" }).isEmpty());
    REQUIRE (t.pm->storeInBank (5).isEmpty());
    t.pm->applyProgram (t.programWithTables ({}, {}), 0, true);
    REQUIRE (t.pm->selectProgram (5));
    CHECK (t.pm->currentTable (0) == std::vector<std::string> { "ok slot five" });
    REQUIRE (t.pm->loadBankFile ((t.rig.temp / "b").withFileExtension (".tbank")).isEmpty()); // selects slot 0
    REQUIRE (t.pm->selectProgram (5));
    CHECK (t.pm->currentTable (0) == std::vector<std::string> { "ok slot five" });
}

TEST_CASE ("Table manager: session tables round trip, in canonical text", "[presets][tables][manager][state]")
{
    TableRig t;
    t.pm->applyProgram (t.programWithTables ({ "ok a  ", "ok b" }, { "ok c" }), 0, false);
    const auto info = t.pm->createTablesInfo();
    REQUIRE (info.isValid());
    CHECK (info.hasType (kTablesInfoType));
    CHECK (info.getProperty ("lower").toString() == "ok a  \nok b\n"); // verbatim
    CHECK (info.getProperty ("upper").toString() == "ok c\n");

    t.pm->applyProgram (t.programWithTables ({}, {}), 0, false);
    t.sink->calls.clear();
    auto prepared = t.pm->prepareTablesInfo (info);
    CHECK (t.sink->calls.empty()); // preparing publishes nothing
    t.pm->withParametersHeld ([&] { t.pm->restoreTables (std::move (prepared)); });
    REQUIRE (t.sink->calls.size() == 2);
    CHECK (t.sink->calls[0].holding);
    CHECK (t.sink->calls[1].holding);
    CHECK (t.pm->currentTable (0) == std::vector<std::string> { "ok a  ", "ok b" });
    CHECK (t.pm->currentTable (1) == std::vector<std::string> { "ok c" });
    CHECK (t.pm->tableError (0).isEmpty());
    CHECK_FALSE (t.sink->preparedInsideHold);
    CHECK (t.pm->createTablesInfo().isEquivalentTo (info));

    // No tables child at all (a session saved before tables, or by a host that dropped it): no tables, no error.
    t.sink->calls.clear();
    t.pm->withParametersHeld ([&] { t.pm->restoreTables (t.pm->prepareTablesInfo ({})); });
    REQUIRE (t.sink->calls.size() == 2);
    CHECK (t.pm->currentTable (0).empty());
    CHECK (t.pm->tableError (0).isEmpty());
}

TEST_CASE ("Table manager: hostile session tables give that part no table and an error", "[presets][tables][manager][state][security]")
{
    TableRig t;
    const auto restore = [&] (const juce::ValueTree& info)
    {
        auto prepared = t.pm->prepareTablesInfo (info);
        t.pm->withParametersHeld ([&] { t.pm->restoreTables (std::move (prepared)); });
    };
    const auto make = [] (const juce::String& lower, const juce::String& upper)
    {
        juce::ValueTree v (kTablesInfoType);
        v.setProperty ("lower", lower, nullptr);
        v.setProperty ("upper", upper, nullptr);
        return v;
    };

    // One part bad, the other fine: only the bad one is dropped.
    restore (make ("ok fine\n", "ok first\nbad second\n"));
    CHECK (t.pm->currentTable (0) == std::vector<std::string> { "ok fine" });
    CHECK (t.pm->currentTable (1).empty());
    CHECK (t.pm->tableError (1).startsWith ("The session's upper table: line 2"));
    CHECK (t.pm->tableError (0).isEmpty());

    // Every kind of nonsense: an empty table and an error, never a crash, never a call with bad lines.
    const std::vector<std::pair<std::string, juce::var>> nonsense {
        { "not text", juce::var (42) },
        { "a list", juce::var (juce::Array<juce::var> { juce::var ("ok") }) },
        { "too many lines", juce::var (juce::String::repeatedString ("ok\n", 65)) },
        { "too many bytes", juce::var (juce::String::repeatedString ("o", 2001)) },
        { "a million empty lines", juce::var (juce::String::repeatedString ("\n", 1000000)) },
        { "a carriage return", juce::var (juce::String ("ok\r\n")) },
        { "a zero-width character", juce::var (juce::String::fromUTF8 ("ok\xe2\x80\x8b")) }, // the stand-in checks only "ok"
    };
    for (const auto& [what, value] : nonsense)
    {
        INFO (what);
        juce::ValueTree v (kTablesInfoType);
        v.setProperty ("lower", value, nullptr);
        t.sink->calls.clear();
        const auto seconds = [&] {
            const auto t0 = std::chrono::steady_clock::now();
            restore (v);
            return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        }();
        CHECK (seconds < 0.5);
        for (const auto& c : t.sink->calls)
            CHECK (validateTableLines (t.schema.instrument(), c.lines).isEmpty());
    }
    juce::ValueTree v (kTablesInfoType);
    v.setProperty ("lower", juce::String::repeatedString ("ok\n", 65), nullptr);
    restore (v);
    CHECK (t.pm->currentTable (0).empty());
    CHECK (t.pm->tableError (0).contains ("more than 64 lines"));
    v.setProperty ("lower", 42, nullptr);
    restore (v);
    CHECK (t.pm->tableError (0).contains ("not text"));
    // A wrong type of child is the same as none.
    restore (juce::ValueTree ("Other"));
    CHECK (t.pm->tableError (0).isEmpty());
    CHECK (t.pm->currentTable (0).empty());
}

TEST_CASE ("Instrument without tables: the manager keeps no tables and the session has no child", "[presets][tables][manager][state]")
{
    PresetRig rig; // the Vintage processor's own manager
    auto& pm = rig.presets();
    CHECK_FALSE (pm.hasTables());
    CHECK_FALSE (pm.createTablesInfo().isValid());
    CHECK (pm.setTable (0, { "x" }).isNotEmpty());
    CHECK (pm.currentTable (0).empty());
    CHECK (pm.tableError (0).isEmpty());
    CHECK (pm.captureProgram().data.empty());
    CHECK (pm.captureTone (0).data.empty());
    pm.restoreTables ({}); // inert
    juce::MemoryBlock state;
    rig.proc.getStateInformation (state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), static_cast<int> (state.getSize()));
    REQUIRE (xml != nullptr);
    CHECK (xml->getChildByName ("Tables") == nullptr);
    CHECK (xml->getChildByName ("Preset") != nullptr);
}
