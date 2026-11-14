// Patch tables in the preset manager (docs/architecture.md 8.8), generic layer: a second manager
// with tables switched on is built over the Vintage processor's parameters, with a stand-in
// validator and a sink that records when and with what it is called. The real B-Side tables
// (compiled and handed to the audio thread) are in tests/bside/integration/TablePresetTests.cpp.
#include <catch2/catch_test_macros.hpp>

#include "PresetTestHelpers.h"

#include <chrono>

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

struct RecordingSink final : TableSink
{
    ApplyGate* gate = nullptr;
    std::atomic<float>* marker = nullptr;
    std::vector<Call> calls;

    void publishTable (int tone, const std::vector<std::string>& lines) noexcept override
    {
        calls.push_back ({ tone, lines, gate->isHolding(), gate->holdDepth(), marker->load() });
    }
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
    // Canonical form (trailing spaces trimmed by the stand-in canonicaliser), published and held.
    CHECK (t.sink->calls[0].lines == std::vector<std::string> { "ok one", "ok two" });
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
    CHECK (t.pm->currentTable (1) == std::vector<std::string> { "ok new" });
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

    auto inst = tableTestInstrument();
    inst.canonicaliseTable = [] (const std::vector<std::string>&) { return std::vector<std::string> { "not ok" }; }; // fails its own validator
    auto schema2 = Schema::fromProcessor (t.rig.proc, { "lower.", "upper." }, { "master.volume", "master.limiter", "master.limiter.ceiling" }, inst);
    PresetManager pm2 (t.rig.proc.getState(), schema2, makeInitBank (schema2), t.rig.presets().getGate(), Locations { t.rig.temp.dir });
    auto q = makeInitProgram (schema2);
    q.data = { { "ok x" }, {} };
    pm2.applyProgram (q, 0, false);
    CHECK (pm2.currentTable (0).empty());
    CHECK (pm2.tableError (0).contains ("normal form"));
    inst.canonicaliseTable = [] (const std::vector<std::string>&) -> std::vector<std::string> { throw 1; };
    auto schema3 = Schema::fromProcessor (t.rig.proc, { "lower.", "upper." }, { "master.volume", "master.limiter", "master.limiter.ceiling" }, inst);
    PresetManager pm3 (t.rig.proc.getState(), schema3, makeInitBank (schema3), t.rig.presets().getGate(), Locations { t.rig.temp.dir });
    q.data = { { "ok x" }, {} };
    pm3.applyProgram (q, 0, false);
    CHECK (pm3.currentTable (0).empty());
    CHECK (pm3.tableError (0).isNotEmpty());
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
    CHECK (info.getProperty ("lower").toString() == "ok a\nok b\n");
    CHECK (info.getProperty ("upper").toString() == "ok c\n");

    t.pm->applyProgram (t.programWithTables ({}, {}), 0, false);
    t.sink->calls.clear();
    auto prepared = t.pm->prepareTablesInfo (info);
    CHECK (t.sink->calls.empty()); // preparing publishes nothing
    t.pm->withParametersHeld ([&] { t.pm->restoreTables (std::move (prepared)); });
    REQUIRE (t.sink->calls.size() == 2);
    CHECK (t.sink->calls[0].holding);
    CHECK (t.sink->calls[1].holding);
    CHECK (t.pm->currentTable (0) == std::vector<std::string> { "ok a", "ok b" });
    CHECK (t.pm->currentTable (1) == std::vector<std::string> { "ok c" });
    CHECK (t.pm->tableError (0).isEmpty());
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
