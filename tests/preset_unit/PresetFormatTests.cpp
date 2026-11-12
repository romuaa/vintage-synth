// Preset files (docs/architecture.md 6b.2): the data model, the JSON writer and the tolerant reader.
// Pure functions: no processor, no files.

#include <catch2/catch_test_macros.hpp>

#include "PresetTestHelpers.h"

#include <limits>

using namespace vs;
using namespace vs::presets;
using namespace vs::ptest;

namespace
{
/** A valid program file for the small schema, with `a.cut` set to the given JSON text. */
std::string programWith (const std::string& cutJson, const std::string& extraTop = {})
{
    return "{\"format\":1,\"kind\":\"program\",\"name\":\"T\"," + extraTop + "\"a\":{\"cut\":" + cutJson + "}}";
}

Parsed<Program> parse (const std::string& s) { return parseProgram (smallSchema(), s); }
} // namespace

TEST_CASE ("Schema: tones, globals and exclusions are derived from the IDs", "[presets][format]")
{
    const auto schema = smallSchema();
    REQUIRE (schema.isValid());
    CHECK (schema.numTones() == 2);
    CHECK (schema.toneKey (0) == "a");
    CHECK (schema.toneKey (1) == "b");
    CHECK (schema.toneSize() == 3);
    CHECK (schema.toneSuffix (0) == "cut");
    CHECK (schema.spec (schema.toneParam (1, 2)).id == "b.pan");
    CHECK (schema.globalParams().size() == 3); // master.vol is excluded
    CHECK (schema.isExcluded (schema.indexOf ("master.vol")));
    CHECK_FALSE (schema.isExcluded (schema.indexOf ("g.mode")));

    // Parts that do not match are reported, not silently half-handled.
    std::vector<ParamSpec> odd { { "a.x", 0, 1, 0, false }, { "b.y", 0, 1, 0, false } };
    CHECK_FALSE (Schema (odd, { "a.", "b." }, {}).isValid());
}

TEST_CASE ("Schema::sanitise: clamp, round discrete values, non-finite becomes the default", "[presets][format]")
{
    const auto s = smallSchema();
    const int cut = s.indexOf ("a.cut"), wave = s.indexOf ("a.wave");
    CHECK (feq (s.sanitise (cut, 0.25), 0.25f));
    CHECK (feq (s.sanitise (cut, 7.0), 1.0f));
    CHECK (feq (s.sanitise (cut, -7.0), 0.0f));
    CHECK (feq (s.sanitise (cut, std::numeric_limits<double>::infinity()), 0.6f));
    CHECK (feq (s.sanitise (cut, -std::numeric_limits<double>::infinity()), 0.6f));
    CHECK (feq (s.sanitise (cut, std::numeric_limits<double>::quiet_NaN()), 0.6f));
    CHECK (feq (s.sanitise (wave, 2.4), 2.0f));
    CHECK (feq (s.sanitise (wave, 2.6), 3.0f));
    CHECK (feq (s.sanitise (wave, 99.0), 5.0f));
    CHECK (feq (s.sanitise (wave, -4.0), 0.0f));
}

TEST_CASE ("formatNumber: shortest text that reads back as the same float", "[presets][format]")
{
    CHECK (formatNumber (0.65f) == "0.65");
    CHECK (formatNumber (3.0f) == "3");
    CHECK (formatNumber (-0.4f) == "-0.4");
    CHECK (formatNumber (0.0f) == "0");
    CHECK (formatNumber (-0.0f) == "0");
    CHECK (formatNumber (127.0f) == "127");

    std::mt19937 rng (7);
    std::uniform_real_distribution<float> u (-100.0f, 100.0f);
    for (int i = 0; i < 20000; ++i)
    {
        const float v = i % 3 == 0 ? u (rng) : (i % 3 == 1 ? u (rng) * 1.0e-6f : std::floor (u (rng)));
        const auto text = formatNumber (v);
        REQUIRE (sameBitsF (std::strtof (text.c_str(), nullptr), v + 0.0f)); // +0.0f: -0 is written as 0
    }
    for (const float v : { std::numeric_limits<float>::min(), std::numeric_limits<float>::max(), 1.0e-10f, 16777216.0f })
        CHECK (sameBitsF (std::strtof (formatNumber (v).c_str(), nullptr), v));
}

TEST_CASE ("Program round trip: every value exact, and a save, load, save cycle is byte-identical", "[presets][format]")
{
    const auto schema = smallSchema();
    const auto p = randomProgram (schema, 1, "Round Trip");
    const auto text = toJson (schema, p);

    const auto back = parseProgram (schema, text);
    REQUIRE (back.ok);
    CHECK (back.value.meta.name == "Round Trip");
    CHECK (back.value.meta.category == "Test");
    CHECK (back.value.meta.author == "Tester");
    // Excluded parameters are not in the file: they come back as their default.
    for (int i = 0; i < schema.numParams(); ++i)
    {
        const auto expected = schema.isExcluded (i) ? schema.spec (i).def : p.values[static_cast<size_t> (i)];
        INFO (schema.spec (i).id);
        CHECK (sameBitsF (back.value.values[static_cast<size_t> (i)], expected));
    }
    CHECK (toJson (schema, back.value) == text);
}

TEST_CASE ("File layout: fixed key order, tones without the part prefix, one parameter per line", "[presets][format]")
{
    const auto schema = smallSchema();
    auto p = makeInitProgram (schema);
    p.meta.name = "Layout";
    const auto text = toJson (schema, p);
    const std::string expected =
        "{\n"
        "  \"format\": 1,\n"
        "  \"kind\": \"program\",\n"
        "  \"name\": \"Layout\",\n"
        "  \"category\": \"\",\n"
        "  \"author\": \"\",\n"
        "  \"global\": {\n"
        "    \"g.mode\": 0,\n"
        "    \"g.split\": 60,\n"
        "    \"g.mix\": 0.25\n"
        "  },\n"
        "  \"a\": {\n"
        "    \"cut\": 0.6,\n"
        "    \"wave\": 2,\n"
        "    \"pan\": 0\n"
        "  },\n"
        "  \"b\": {\n"
        "    \"cut\": 0.6,\n"
        "    \"wave\": 2,\n"
        "    \"pan\": 0\n"
        "  }\n"
        "}\n";
    CHECK (text == expected);
    CHECK (text.find ("a.cut") == std::string::npos);
    CHECK (text.find ("master.vol") == std::string::npos); // not part of a program
}

TEST_CASE ("Tone round trip; a tone has no part, so it fits either part", "[presets][format]")
{
    const auto schema = smallSchema();
    const auto p = randomProgram (schema, 2);
    const auto toneA = toneOf (schema, p, 0);
    const auto text = toJson (schema, toneA);
    CHECK (text.find ("\"kind\": \"tone\"") != std::string::npos);
    CHECK (text.find ("\"cut\"") != std::string::npos);
    CHECK (text.find ("a.cut") == std::string::npos);

    const auto back = parseTone (schema, text);
    REQUIRE (back.ok);
    CHECK (sameValues (back.value.values, toneA.values));
    CHECK (toJson (schema, back.value) == text);

    // Putting the tone into the other part changes only that part.
    auto q = randomProgram (schema, 3);
    const auto before = q.values;
    setTone (schema, q, 1, back.value);
    for (int k = 0; k < schema.toneSize(); ++k)
    {
        CHECK (sameBitsF (q.values[static_cast<size_t> (schema.toneParam (1, k))], toneA.values[static_cast<size_t> (k)]));
        CHECK (sameBitsF (q.values[static_cast<size_t> (schema.toneParam (0, k))], before[static_cast<size_t> (schema.toneParam (0, k))]));
    }
}

TEST_CASE ("Bank round trip: 64 programs, byte-identical re-save", "[presets][format]")
{
    const auto schema = smallSchema();
    Bank bank = makeInitBank (schema);
    bank.name = "My Bank";
    bank.author = "Me";
    for (int i = 0; i < kBankSize; ++i)
        bank.programs[static_cast<size_t> (i)] = randomProgram (schema, 100u + static_cast<unsigned> (i), "Program " + juce::String (i));
    const auto text = toJson (schema, bank);
    const auto back = parseBank (schema, text);
    REQUIRE (back.ok);
    CHECK (back.value.name == "My Bank");
    CHECK (back.value.author == "Me");
    REQUIRE (back.value.programs.size() == static_cast<size_t> (kBankSize));
    for (int i = 0; i < kBankSize; ++i)
    {
        CHECK (back.value.programs[static_cast<size_t> (i)].meta.name == "Program " + juce::String (i));
        for (int k = 0; k < schema.numParams(); ++k)
            if (! schema.isExcluded (k))
                REQUIRE (sameBitsF (back.value.programs[static_cast<size_t> (i)].values[static_cast<size_t> (k)],
                                    bank.programs[static_cast<size_t> (i)].values[static_cast<size_t> (k)]));
    }
    CHECK (toJson (schema, back.value) == text);

    // A bank file with fewer programs: the rest are Init.
    const auto few = parseBank (schema, "{\"format\":1,\"kind\":\"bank\",\"programs\":[{\"name\":\"One\"}]}");
    REQUIRE (few.ok);
    CHECK (few.value.programs[0].meta.name == "One");
    CHECK (few.value.programs[63].meta.name == "Init");
    CHECK (sameValues (few.value.programs[5].values, schema.defaults()));
}

TEST_CASE ("Tolerant reading: unknown IDs ignored, missing take the default, values clamped", "[presets][format]")
{
    const auto schema = smallSchema();
    const auto cut = static_cast<size_t> (schema.indexOf ("a.cut"));
    const auto wave = static_cast<size_t> (schema.indexOf ("a.wave"));
    const auto split = static_cast<size_t> (schema.indexOf ("g.split"));

    SECTION ("unknown IDs, sections and keys are ignored")
    {
        const auto r = parse ("{\"format\":1,\"kind\":\"program\",\"future\":{\"x\":1},\"a\":{\"cut\":0.3,\"nonsense\":9},"
                              "\"global\":{\"g.unknown\":4,\"g.split\":70},\"c\":{\"cut\":1}}");
        REQUIRE (r.ok);
        CHECK (feq (r.value.values[cut], 0.3f));
        CHECK (feq (r.value.values[split], 70.0f));
        CHECK (r.value.values.size() == static_cast<size_t> (schema.numParams()));
    }
    SECTION ("missing parameters, missing sections and an empty program take the defaults")
    {
        const auto r = parse ("{\"format\":1,\"kind\":\"program\",\"a\":{\"cut\":0.3}}");
        REQUIRE (r.ok);
        CHECK (feq (r.value.values[cut], 0.3f));
        CHECK (feq (r.value.values[wave], 2.0f));
        CHECK (feq (r.value.values[split], 60.0f));
        CHECK (r.value.meta.name == "Untitled");
        const auto empty = parse ("{\"format\":1,\"kind\":\"program\"}");
        REQUIRE (empty.ok);
        CHECK (sameValues (empty.value.values, schema.defaults()));
    }
    SECTION ("out-of-range values are clamped; discrete values are whole steps")
    {
        CHECK (feq (parse (programWith ("5.5")).value.values[cut], 1.0f));
        CHECK (feq (parse (programWith ("-5")).value.values[cut], 0.0f));
        auto r = parse ("{\"format\":1,\"kind\":\"program\",\"a\":{\"wave\":2.4},\"global\":{\"g.split\":300,\"g.mode\":-1}}");
        REQUIRE (r.ok);
        CHECK (feq (r.value.values[wave], 2.0f));
        CHECK (feq (r.value.values[split], 127.0f));
        CHECK (feq (r.value.values[static_cast<size_t> (schema.indexOf ("g.mode"))], 0.0f));
    }
    SECTION ("non-finite and non-numeric values become the default")
    {
        for (const char* bad : { "1e999", "-1e999", "\"NaN\"", "\"0.1\"", "null", "true", "[0.2]", "{\"v\":1}", "\"\"" })
        {
            INFO (bad);
            const auto r = parse (programWith (bad));
            REQUIRE (r.ok);
            CHECK (feq (r.value.values[cut], 0.6f));
        }
    }
    SECTION ("a hand-written file may use integers, exponents and a BOM")
    {
        const auto r = parse ("\xEF\xBB\xBF" + programWith ("5e-1"));
        REQUIRE (r.ok);
        CHECK (feq (r.value.values[cut], 0.5f));
    }
}

TEST_CASE ("Names, categories and authors are cleaned and limited", "[presets][format]")
{
    const auto schema = smallSchema();
    const auto longName = std::string (200, 'x');
    auto r = parse (programWith ("0.3", "\"category\":\"" + std::string (60, 'c') + "\",\"author\":\"" + std::string (100, 'a') + "\","));
    REQUIRE (r.ok);
    CHECK (r.value.meta.category.length() == kMaxCategoryChars);
    CHECK (r.value.meta.author.length() == kMaxAuthorChars);

    auto named = parse ("{\"format\":1,\"kind\":\"program\",\"name\":\"" + longName + "\"}");
    REQUIRE (named.ok);
    CHECK (named.value.meta.name.length() == kMaxNameChars);

    auto control = parse ("{\"format\":1,\"kind\":\"program\",\"name\":\"  Bad\\n\\tName\\u0001 \"}");
    REQUIRE (control.ok);
    CHECK (control.value.meta.name == "Bad  Name");

    CHECK (parse ("{\"format\":1,\"kind\":\"program\",\"name\":\"   \"}").value.meta.name == "Untitled");
    CHECK (parse ("{\"format\":1,\"kind\":\"program\",\"name\":\"\"}").value.meta.name == "Untitled");

    // Non-ASCII names survive a round trip (characters, not bytes, are counted).
    auto p = makeInitProgram (schema);
    p.meta.name = juce::String::fromUTF8 ("Pad \xc3\xa4\xc3\xb6 \xe2\x99\xaa");
    const auto back = parseProgram (schema, toJson (schema, p));
    REQUIRE (back.ok);
    CHECK (back.value.meta.name == p.meta.name);

    // The writer cleans what a caller hands it, so a written file always reads back unchanged.
    p.meta.name = "Quote \" and \\ backslash\nnewline";
    const auto text = toJson (schema, p);
    const auto again = parseProgram (schema, text);
    REQUIRE (again.ok);
    CHECK (toJson (schema, again.value) == text);
    CHECK (again.value.meta.name == "Quote \" and \\ backslash newline");
}

TEST_CASE ("Malformed files are rejected whole, with a message", "[presets][format]")
{
    const std::string good = programWith ("0.3");
    REQUIRE (parse (good).ok);

    struct Case
    {
        const char* what;
        std::string text;
    };
    const std::vector<Case> bad {
        { "empty", "" },
        { "whitespace", "  \n " },
        { "not JSON", "hello" },
        { "open brace", "{" },
        { "root is an array", "[1,2,3]" },
        { "root is a number", "42" },
        { "root is a string", "\"program\"" },
        { "trailing garbage", good + " xyz" },
        { "two documents", good + good },
        { "no format", "{\"kind\":\"program\"}" },
        { "format 0", "{\"format\":0,\"kind\":\"program\"}" },
        { "format 2 (newer)", "{\"format\":2,\"kind\":\"program\"}" },
        { "format as text", "{\"format\":\"1\",\"kind\":\"program\"}" },
        { "format 1.5", "{\"format\":1.5,\"kind\":\"program\"}" },
        { "no kind", "{\"format\":1}" },
        { "wrong kind (a tone)", "{\"format\":1,\"kind\":\"tone\"}" },
        { "wrong kind (a bank)", "{\"format\":1,\"kind\":\"bank\"}" },
        { "tone section is a list", "{\"format\":1,\"kind\":\"program\",\"a\":[1]}" },
        { "tone section is a number", "{\"format\":1,\"kind\":\"program\",\"b\":3}" },
        { "global section is text", "{\"format\":1,\"kind\":\"program\",\"global\":\"x\"}" },
        { "name is a number", "{\"format\":1,\"kind\":\"program\",\"name\":5}" },
        { "category is a list", "{\"format\":1,\"kind\":\"program\",\"category\":[]}" },
        { "NUL byte", std::string ("{\"format\":1,\"kind\":\"program\",\"name\":\"a\0b\"}", 48) },
        { "invalid UTF-8", "{\"format\":1,\"kind\":\"program\",\"name\":\"\xff\xfe\"}" },
        { "overlong UTF-8", "{\"format\":1,\"kind\":\"program\",\"name\":\"\xc0\xaf\"}" },
        { "nested far too deep", std::string (5000, '[') + std::string (5000, ']') },
        { "nested objects too deep", "{\"format\":1,\"kind\":\"program\",\"a\":" + std::string (100, '{') + std::string (100, '}') + "}" },
        { "far too large", "{\"format\":1,\"kind\":\"program\",\"name\":\"" + std::string (kMaxFileBytes, 'x') + "\"}" },
    };
    for (const auto& c : bad)
    {
        INFO (c.what);
        const auto r = parse (c.text);
        CHECK_FALSE (r.ok);
        CHECK (r.error.isNotEmpty());
    }

    const auto schema = smallSchema();
    CHECK_FALSE (parseTone (schema, good).ok);                                           // a program is not a tone
    CHECK_FALSE (parseBank (schema, good).ok);                                           // nor a bank
    CHECK_FALSE (parseTone (schema, "{\"format\":1,\"kind\":\"tone\",\"params\":5}").ok);
    CHECK_FALSE (parseBank (schema, "{\"format\":1,\"kind\":\"bank\",\"programs\":\"x\"}").ok);
    CHECK_FALSE (parseBank (schema, "{\"format\":1,\"kind\":\"bank\",\"programs\":[5]}").ok);
    CHECK_FALSE (parseBank (schema, "{\"format\":1,\"kind\":\"bank\",\"programs\":[{\"a\":3}]}").ok); // a broken program breaks the bank

    std::string tooMany = "{\"format\":1,\"kind\":\"bank\",\"programs\":[";
    for (int i = 0; i < kBankSize + 1; ++i)
        tooMany += std::string (i == 0 ? "" : ",") + "{}";
    const auto many = parseBank (schema, tooMany + "]}");
    CHECK_FALSE (many.ok);
    // 64 are fine.
    tooMany.erase (tooMany.size() - 3); // drops ",{}"
    CHECK (parseBank (schema, tooMany + "]}").ok);
}

TEST_CASE ("Truncated files are rejected at every cut point", "[presets][format]")
{
    const auto schema = smallSchema();
    const auto program = toJson (schema, randomProgram (schema, 5));
    for (size_t n = 0; n + 1 < program.size(); ++n) // the last cut leaves the closing brace: that one is valid
    {
        const auto r = parseProgram (schema, program.data(), n);
        REQUIRE_FALSE (r.ok);
    }
    const auto tone = toJson (schema, toneOf (schema, randomProgram (schema, 6), 1));
    for (size_t n = 0; n + 1 < tone.size(); ++n)
        REQUIRE_FALSE (parseTone (schema, tone.data(), n).ok);

    Bank bank = makeInitBank (schema);
    for (int i = 0; i < 6; ++i)
        bank.programs[static_cast<size_t> (i)] = randomProgram (schema, 30u + static_cast<unsigned> (i));
    const auto text = toJson (schema, bank);
    for (size_t n = 0; n + 1 < text.size(); n += 7)
        REQUIRE_FALSE (parseBank (schema, text.data(), n).ok);
    for (size_t n = text.size() - 6; n + 1 < text.size(); ++n)
        REQUIRE_FALSE (parseBank (schema, text.data(), n).ok);
    CHECK (parseBank (schema, text).ok);
}

TEST_CASE ("Fuzzed files: never a crash; whatever is accepted is valid and stable", "[presets][format]")
{
    const auto schema = smallSchema();
    const auto seed = toJson (schema, randomProgram (schema, 9, "Fuzz"));
    Bank bank = makeInitBank (schema);
    for (int i = 0; i < 4; ++i)
        bank.programs[static_cast<size_t> (i)] = randomProgram (schema, 40u + static_cast<unsigned> (i));
    const auto bankSeed = toJson (schema, bank);

    std::mt19937 rng (12345);
    const std::string tokens[] = { "{", "}", "[", "]", ",", ":", "\"", "1e999", "null", "-", "\\", "\\u", "NaN", "0x10", "\n" };
    int accepted = 0;
    for (int iter = 0; iter < 4000; ++iter)
    {
        std::string s = (iter % 4 == 3) ? bankSeed : seed;
        const int edits = 1 + static_cast<int> (rng() % 4);
        for (int e = 0; e < edits; ++e)
        {
            const auto at = static_cast<size_t> (rng() % (s.size() + 1));
            switch (rng() % 4)
            {
                case 0: if (at < s.size()) s[at] = static_cast<char> (rng() % 256); break;
                case 1: if (at < s.size()) s.erase (at, 1 + rng() % 5); break;
                case 2: s.insert (at, tokens[rng() % (sizeof (tokens) / sizeof (tokens[0]))]); break;
                default: s.resize (at); break;
            }
        }
        if (iter % 4 == 3)
        {
            const auto r = parseBank (schema, s);
            if (r.ok)
            {
                ++accepted;
                REQUIRE (r.value.programs.size() == static_cast<size_t> (kBankSize));
                const auto again = parseBank (schema, toJson (schema, r.value));
                REQUIRE (again.ok);
                REQUIRE (toJson (schema, again.value) == toJson (schema, r.value));
            }
            else
                REQUIRE (r.error.isNotEmpty());
            continue;
        }
        const auto r = parseProgram (schema, s);
        if (! r.ok)
        {
            REQUIRE (r.error.isNotEmpty());
            continue;
        }
        ++accepted;
        REQUIRE (r.value.values.size() == static_cast<size_t> (schema.numParams()));
        for (int i = 0; i < schema.numParams(); ++i)
        {
            const float v = r.value.values[static_cast<size_t> (i)];
            const auto& sp = schema.spec (i);
            REQUIRE (std::isfinite (v));
            REQUIRE (v >= sp.lo);
            REQUIRE (v <= sp.hi);
            if (sp.discrete)
                REQUIRE (feq (v, std::round (v)));
        }
        REQUIRE (r.value.meta.name.length() <= kMaxNameChars);
        const auto text = toJson (schema, r.value);
        const auto again = parseProgram (schema, text);
        REQUIRE (again.ok);
        REQUIRE (toJson (schema, again.value) == text);
    }
    CHECK (accepted > 100); // the fuzzer does reach valid files, so the checks above ran on real results
    CHECK (accepted < 4000);
}

TEST_CASE ("Factory file names map to slots", "[presets][format]")
{
    CHECK (slotFromFileName ("A1-init.json") == 0);
    CHECK (slotFromFileName ("a8-x") == 7);
    CHECK (slotFromFileName ("B1") == 8);
    CHECK (slotFromFileName ("H8-last.json") == 63);
    CHECK (slotFromFileName ("I1") == -1);
    CHECK (slotFromFileName ("A9") == -1);
    CHECK (slotFromFileName ("A0") == -1);
    CHECK (slotFromFileName ("A") == -1);
    CHECK (slotFromFileName ("init.json") == -1);

    const auto schema = smallSchema();
    const std::string ok = "{\"format\":1,\"kind\":\"program\",\"name\":\"X\"}";
    const std::string broken = "{\"format\":1,";
    juce::StringArray errors;
    const auto bank = makeBankFromFiles (schema, "F",
                                         { { "B2-x.json", ok.data(), ok.size() },
                                           { "B2-dup.json", ok.data(), ok.size() },
                                           { "C1-broken.json", broken.data(), broken.size() },
                                           { "nope.json", ok.data(), ok.size() } },
                                         &errors);
    CHECK (bank.programs[9].meta.name == "X");
    CHECK (bank.programs[16].meta.name == "Init"); // C1 failed: stays Init
    CHECK (errors.size() == 3);
}
