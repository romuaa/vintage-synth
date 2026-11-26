// Patch tables in preset files (docs/architecture.md 8.8), the generic layer: the model, the writer
// and the bounded reader, with a stand-in validator (every line starts with "ok"). The B-Side tables
// with the real reader are tested in tests/bside/integration/TablePresetTests.cpp.
#include <catch2/catch_test_macros.hpp>

#include "PresetTestHelpers.h"

#include <chrono>
#include <random>

using namespace vs;
using namespace vs::presets;
using namespace vs::ptest;

namespace
{
const char* kHead = "{\"format\":1,\"instrument\":\"test-tables\",\"kind\":\"program\",\"name\":\"Tabby\",";

Schema tableSchema() { return smallSchema (tableTestInstrument()); }

std::string programText (const std::string& tables) { return std::string (kHead) + tables + "}"; }

std::string toneText (const std::string& table)
{
    return "{\"format\":1,\"instrument\":\"test-tables\",\"kind\":\"tone\",\"name\":\"T\",\"table\":" + table + "}";
}

std::string bankText (const std::string& programBodies)
{
    return "{\"format\":1,\"instrument\":\"test-tables\",\"kind\":\"bank\",\"programs\":[" + programBodies + "]}";
}

Program withTables (const Schema& schema, std::vector<std::string> lower, std::vector<std::string> upper)
{
    auto p = randomProgram (schema, 3, "Tabby");
    p.data = { std::move (lower), std::move (upper) };
    return p;
}

double secondsOf (const std::function<void()>& f)
{
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
}
} // namespace

TEST_CASE ("Instrument without tables: keys named tables are ignored, nothing is written", "[presets][tables]")
{
    const auto schema = smallSchema(); // the plain instrument (Vintage's situation)
    REQUIRE_FALSE (schema.instrument().hasTables());
    auto p = randomProgram (schema, 4);
    const auto plain = toJson (schema, p);
    p.data = { { "ok one" }, { "ok two" } }; // even if a caller fills the model, the writer does not write it
    CHECK (toJson (schema, p) == plain);
    CHECK (plain.find ("tables") == std::string::npos);

    const std::string withKey = "{\"format\":1,\"instrument\":\"test-synth\",\"kind\":\"program\",\"name\":\"T\","
                                "\"tables\":{\"a\":[\"not ok at all\"]},\"table\":5}";
    const auto r = parseProgram (schema, withKey);
    CHECK (r.ok);
    CHECK (r.value.data.empty());
    const auto tone = parseTone (schema, "{\"format\":1,\"instrument\":\"test-synth\",\"kind\":\"tone\",\"table\":[1,2,3]}");
    CHECK (tone.ok);
    CHECK (tone.value.data.empty());
    // setTone leaves the data of an instrument without tables alone.
    Program q = makeInitProgram (schema);
    setTone (schema, q, 0, tone.value);
    CHECK (q.data.empty());
}

TEST_CASE ("Tables: program, tone and bank round trips; a re-save is byte-identical", "[presets][tables]")
{
    const auto schema = tableSchema();
    REQUIRE (schema.isValid());
    const auto p = withTables (schema, { "ok a", "ok b \"quoted\" \\ back" }, {});
    const auto text = toJson (schema, p);
    const auto back = parseProgram (schema, text);
    REQUIRE (back.ok);
    REQUIRE (back.value.data.size() == 2);
    CHECK (back.value.data[0] == p.data[0]);
    CHECK (back.value.data[1].empty());
    CHECK (toJson (schema, back.value) == text);

    // Layout: one line per table line, empty tables as [].
    CHECK (text.find ("  \"tables\": {\n    \"a\": [\n      \"ok a\",\n      \"ok b \\\"quoted\\\" \\\\ back\"\n    ],\n    \"b\": []\n  }\n}\n") != std::string::npos);

    const auto tone = toneOf (schema, p, 0);
    CHECK (tone.data == p.data[0]);
    const auto toneJson = toJson (schema, tone);
    CHECK (toneJson.find ("  \"table\": [\n    \"ok a\",") != std::string::npos);
    const auto toneBack = parseTone (schema, toneJson);
    REQUIRE (toneBack.ok);
    CHECK (toneBack.value.data == tone.data);
    CHECK (toJson (schema, toneBack.value) == toneJson);

    // A tone goes into either part.
    auto q = makeInitProgram (schema);
    setTone (schema, q, 1, toneBack.value);
    REQUIRE (q.data.size() == 2);
    CHECK (q.data[1] == tone.data);
    CHECK (q.data[0].empty());

    Bank bank = makeInitBank (schema);
    bank.programs[5] = p;
    bank.programs[63] = withTables (schema, {}, { "ok z" });
    const auto bankJson = toJson (schema, bank);
    const auto bankBack = parseBank (schema, bankJson);
    REQUIRE (bankBack.ok);
    CHECK (bankBack.value.programs[5].data[0] == p.data[0]);
    CHECK (bankBack.value.programs[63].data[1] == std::vector<std::string> { "ok z" });
    CHECK (bankBack.value.programs[0].data[0].empty());
    CHECK (toJson (schema, bankBack.value) == bankJson);
}

TEST_CASE ("Tables: missing or empty means no table; unknown keys are ignored", "[presets][tables]")
{
    const auto schema = tableSchema();
    for (const auto& text : { programText ("\"x\":1"), programText ("\"tables\":{}"), programText ("\"tables\":{\"a\":[],\"b\":[]}"),
                              programText ("\"tables\":{\"c\":[\"anything\"],\"a\":[]}") })
    {
        INFO (text);
        const auto r = parseProgram (schema, text);
        REQUIRE (r.ok);
        REQUIRE (r.value.data.size() == 2);
        CHECK (r.value.data[0].empty());
        CHECK (r.value.data[1].empty());
    }
    const auto t = parseTone (schema, toneText ("[]"));
    CHECK (t.ok);
    CHECK (t.value.data.empty());
    CHECK (parseTone (schema, "{\"format\":1,\"instrument\":\"test-tables\",\"kind\":\"tone\"}").ok);
}

TEST_CASE ("Tables: a table the validator rejects rejects the whole file, named by part and program", "[presets][tables]")
{
    const auto schema = tableSchema();
    {
        const auto r = parseProgram (schema, programText ("\"tables\":{\"a\":[\"ok\"],\"b\":[\"ok\",\"bad\"]}"));
        REQUIRE_FALSE (r.ok);
        CHECK (r.error.startsWith ("B table of \"Tabby\": line 2, column 1: expected"));
        CHECK (r.value.data.empty());
    }
    {
        const auto r = parseTone (schema, toneText ("[\"nope\"]"));
        REQUIRE_FALSE (r.ok);
        CHECK (r.error.startsWith ("Table of \"T\": line 1"));
    }
    {
        const auto good = "{\"name\":\"One\"}";
        const auto bad = "{\"name\":\"Two\",\"tables\":{\"a\":[\"ok\",\"ok\",\"wrong\"]}}";
        const auto r = parseBank (schema, bankText (std::string (good) + "," + bad));
        REQUIRE_FALSE (r.ok);
        CHECK (r.error.startsWith ("Program 2: A table of \"Two\": line 3"));
    }
    // A validator that throws turns into a rejection, not a crash.
    auto inst = tableTestInstrument();
    inst.validateTable = [] (const std::vector<std::string>&) -> juce::String { throw std::runtime_error ("boom"); };
    const auto throwing = smallSchema (inst);
    const auto r = parseProgram (throwing, programText ("\"tables\":{\"a\":[\"ok\"]}"));
    CHECK_FALSE (r.ok);
}

TEST_CASE ("Tables: wrong types and shapes are rejected", "[presets][tables]")
{
    const auto schema = tableSchema();
    const std::vector<std::string> bad {
        "\"tables\":5",
        "\"tables\":\"ok\"",
        "\"tables\":[\"ok\"]",
        "\"tables\":{\"a\":\"ok\"}",
        "\"tables\":{\"a\":5}",
        "\"tables\":{\"a\":{\"0\":\"ok\"}}",
        "\"tables\":{\"a\":[5]}",
        "\"tables\":{\"a\":[null]}",
        "\"tables\":{\"a\":[[\"ok\"]]}",
        "\"tables\":{\"b\":[\"ok\",true]}",
        "\"tables\":{\"a\":[\"ok\"],\"a\":[\"ok\"]}", // duplicate key
        "\"tables\":{},\"tables\":{}",                 // duplicate key
        "\"tables\":{\"a\":[\"ok\\n\"]}",              // a line break inside a line
        "\"tables\":{\"a\":[\"ok\\r\"]}",
        "\"tables\":{\"a\":[\"ok\\nok\"]}",
        "\"tables\":{\"a\":[\"ok\\u0000\"]}",          // NUL: the strict reader refuses \u0000
        "\"tables\":{\"a\":[\"ok\\ud800\"]}",          // lone surrogate
    };
    for (const auto& t : bad)
    {
        INFO (t);
        const auto r = parseProgram (schema, programText (t));
        CHECK_FALSE (r.ok);
        CHECK (r.error.isNotEmpty());
    }
    for (const char* t : { "5", "\"ok\"", "{}", "[1]", "[\"ok\",[]]" })
    {
        INFO (t);
        CHECK_FALSE (parseTone (schema, toneText (t)).ok);
    }
    // A raw NUL, raw control characters and invalid UTF-8 inside a table line.
    for (const std::string& line : { std::string ("ok\0x", 4), std::string ("ok\x01"), std::string ("ok\xff\xfe"), std::string ("ok\xc0\xaf"),
                                    std::string ("ok\xed\xa0\x80") })
    {
        const auto text = programText ("\"tables\":{\"a\":[\"" + line + "\"]}");
        CHECK_FALSE (parseProgram (schema, text).ok);
    }
    // A non-UTF-8 byte in a key of the tables object.
    CHECK_FALSE (parseProgram (schema, programText ("\"tables\":{\"\xff\":[]}")).ok);
}

TEST_CASE ("Tables: line, byte and JSON limits are enforced before any validator work", "[presets][tables][security][serial]")
{
    const auto schema = tableSchema(); // 64 lines, 2000 bytes
    int validatorCalls = 0;
    auto inst = tableTestInstrument();
    const auto inner = inst.validateTable;
    inst.validateTable = [&] (const std::vector<std::string>& l) { ++validatorCalls; return inner (l); };
    const auto counting = smallSchema (inst);

    auto listOf = [] (int lines, const std::string& line)
    {
        std::string s = "[";
        for (int i = 0; i < lines; ++i)
            s += (i ? ",\"" : "\"") + line + "\"";
        return s + "]";
    };
    // Exactly at the limits: accepted. 64 lines; and bytes: 2000 including a line end per line.
    CHECK (parseProgram (counting, programText ("\"tables\":{\"a\":" + listOf (64, "ok") + "}")).ok);
    CHECK (parseProgram (counting, programText ("\"tables\":{\"a\":[\"ok" + std::string (2000 - 3, 'x') + "\"]}")).ok);
    validatorCalls = 0;
    // One over: rejected without calling the validator.
    const auto tooManyLines = parseProgram (counting, programText ("\"tables\":{\"a\":" + listOf (65, "ok") + "}"));
    CHECK_FALSE (tooManyLines.ok);
    CHECK (tooManyLines.error.contains ("more than 64 lines"));
    const auto tooLong = parseProgram (counting, programText ("\"tables\":{\"b\":[\"ok" + std::string (2000 - 2, 'x') + "\"]}"));
    CHECK_FALSE (tooLong.ok);
    CHECK (tooLong.error.contains ("longer than 2000 bytes"));
    const auto manyShort = parseProgram (counting, programText ("\"tables\":{\"a\":" + listOf (64, std::string (40, 'o')) + "}"));
    CHECK_FALSE (manyShort.ok); // 64 x 41 bytes
    CHECK (validatorCalls == 0);

    // Hostile sizes: a million empty lines, a megabyte-long line, thousands of members: bounded work.
    std::string million = "[";
    for (int i = 0; i < 1000000; ++i)
        million += i ? ",\"\"" : "\"\"";
    million += "]";
    bool ok = true;
    CHECK (secondsOf ([&] { ok = parseProgram (counting, programText ("\"tables\":{\"a\":" + million + "}")).ok; }) < 1.0);
    CHECK_FALSE (ok);
    CHECK (secondsOf ([&] { ok = parseProgram (counting, programText ("\"tables\":{\"a\":[\"" + std::string (900000, 'o') + "\"]}")).ok; }) < 1.0);
    CHECK_FALSE (ok);
    std::string members = "{";
    for (int i = 0; i < 20000; ++i)
        members += (i ? ",\"k" : "\"k") + std::to_string (i) + "\":[]";
    members += "}";
    CHECK (secondsOf ([&] { ok = parseProgram (counting, programText ("\"tables\":" + members)).ok; }) < 1.0);
    CHECK_FALSE (ok);
    // Deep nesting inside a table list, and a lot of nested lists.
    CHECK_FALSE (parseProgram (counting, programText ("\"tables\":{\"a\":" + std::string (500, '[') + std::string (500, ']') + "}")).ok);
    CHECK (validatorCalls == 0);

    // The caps of an instrument with tables replace the plain defaults; without tables they stay.
    auto big = tableTestInstrument();
    big.maxFileBytes = 4u * 1024u * 1024u;
    CHECK (big.fileCap() == 4u * 1024u * 1024u);
    CHECK (testInstrument().fileCap() == kMaxFileBytes);
    const auto bigSchema = smallSchema (big);
    std::string blank = std::string (kHead) + "\"x\":1" + std::string (kMaxFileBytes + 10, ' ') + "}";
    CHECK (parseProgram (bigSchema, blank).ok); // under the larger cap, white space is fine
    CHECK_FALSE (parseProgram (schema, blank).ok);
    CHECK_FALSE (parseProgram (bigSchema, std::string (kHead) + "\"x\":1" + std::string (big.fileCap(), ' ') + "}").ok);
}

TEST_CASE ("Tables: an instrument with tables needs limits to be valid", "[presets][tables]")
{
    auto inst = tableTestInstrument();
    CHECK (inst.isValid());
    auto noLines = inst;
    noLines.maxTableLines = 0;
    CHECK_FALSE (noLines.isValid());
    auto noBytes = inst;
    noBytes.maxTableBytes = 0;
    CHECK_FALSE (noBytes.isValid());
    CHECK (testInstrument().isValid());
}

TEST_CASE ("Tables: checkTableShape, split and join", "[presets][tables]")
{
    const auto inst = tableTestInstrument();
    CHECK (checkTableShape (inst, {}).isEmpty());
    CHECK (checkTableShape (inst, { "ok", "ok" }).isEmpty());
    CHECK (checkTableShape (inst, { "a\nb" }).isNotEmpty());
    CHECK (checkTableShape (inst, { "a\rb" }).isNotEmpty());
    CHECK (checkTableShape (inst, { std::string ("a\0b", 3) }).isNotEmpty());
    CHECK (joinTableLines ({ "x", "", "y" }) == "x\n\ny\n");
    CHECK (joinTableLines ({}).empty());

    std::vector<std::string> lines;
    CHECK (splitTableText (inst, "a\nb\n", lines).isEmpty());
    CHECK (lines == std::vector<std::string> { "a", "b" });
    CHECK (splitTableText (inst, "a\n\nb", lines).isEmpty()); // no final LF: the last line still counts
    CHECK (lines == std::vector<std::string> { "a", "", "b" });
    CHECK (splitTableText (inst, "", lines).isEmpty());
    CHECK (lines.empty());
    CHECK (splitTableText (inst, "\n", lines).isEmpty());
    CHECK (lines == std::vector<std::string> { "" });
    CHECK (splitTableText (inst, juce::String::repeatedString ("x\n", 65), lines).isNotEmpty()); // 65 lines
    CHECK (lines.empty());
    CHECK (splitTableText (inst, juce::String::repeatedString ("x", 2001), lines).isNotEmpty()); // 2001 bytes
    CHECK (lines.empty());
    // join then split gives the lines back.
    const std::vector<std::string> some { "ok 1", "", "ok  3 " };
    CHECK (splitTableText (inst, juce::String::fromUTF8 (joinTableLines (some).c_str()), lines).isEmpty());
    CHECK (lines == some);
}

TEST_CASE ("Tables: fuzzed files never crash, and whatever is accepted saves and loads again unchanged", "[presets][tables][fuzz]")
{
    const auto schema = tableSchema();
    std::vector<std::string> seeds;
    seeds.push_back (toJson (schema, withTables (schema, { "ok a", "ok b" }, { "ok c" })));
    seeds.push_back (toJson (schema, [&] { auto b = makeInitBank (schema); b.programs[2] = withTables (schema, { "ok" }, { "ok", "ok" }); return b; }()));
    seeds.push_back (toJson (schema, toneOf (schema, withTables (schema, { "ok q" }, {}), 0)));
    std::mt19937 rng (12345);
    int accepted = 0;
    const char* pieces[] = { "\"", "\\", "[", "]", "{", "}", ",", ":", "\\n", "\\u0000", "ok", "tables", "table", "\xff", "\0" };
    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < 4000 && std::chrono::steady_clock::now() - start < std::chrono::seconds (10); ++iteration)
    {
        auto text = seeds[rng() % seeds.size()];
        for (int m = 0, n = 1 + static_cast<int> (rng() % 4); m < n; ++m)
        {
            const auto pos = rng() % (text.size() + 1);
            switch (rng() % 4)
            {
                case 0: if (pos < text.size()) text.erase (pos, 1 + rng() % 8); break;
                case 1: text.insert (pos, pieces[rng() % std::size (pieces)]); break;
                case 2: if (pos < text.size()) text[pos] = static_cast<char> (rng()); break;
                default: text.resize (pos); break;
            }
        }
        if (const auto p = parseProgram (schema, text); p.ok)
        {
            ++accepted;
            const auto again = toJson (schema, p.value);
            const auto q = parseProgram (schema, again);
            REQUIRE (q.ok);
            CHECK (toJson (schema, q.value) == again);
            for (const auto& table : p.value.data)
                CHECK (validateTableLines (schema.instrument(), table).isEmpty());
        }
        if (const auto b = parseBank (schema, text); b.ok)
            CHECK (parseBank (schema, toJson (schema, b.value)).ok);
        if (const auto t = parseTone (schema, text); t.ok)
            CHECK (validateTableLines (schema.instrument(), t.value.data).isEmpty());
    }
    CHECK (accepted > 0);
}
