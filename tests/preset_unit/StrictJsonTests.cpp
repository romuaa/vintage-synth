// The strict JSON reader for preset files (common/presets/StrictJson.h), the locale-independent
// number text, the file identity ("instrument") and the text rules. Pure functions: no processor.
//
// The fuzz tests are seeded (reproducible) and time-bounded; "[.fuzzlong]" is a hidden long run
// (minutes) for the sanitizer builds: VintageSynthTests "[.fuzzlong]".

#include <catch2/catch_test_macros.hpp>

#include "PresetTestHelpers.h"
#include "common/presets/StrictJson.h"

#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace vs;
using namespace vs::presets;
using namespace vs::ptest;

namespace
{
json::Limits defaultLimits() { return {}; }

bool accepts (const std::string& text, const json::Limits& limits = defaultLimits())
{
    json::Document doc;
    std::string error;
    const bool ok = doc.parse (text.data(), text.size(), limits, error);
    if (! ok && error.empty())
        FAIL ("rejected without a message");
    return ok;
}

std::string header (const char* kind = "program") { return std::string ("{\"format\":1,\"instrument\":\"test-synth\",\"kind\":\"") + kind + "\""; }

double secondsOf (const std::function<void()>& f)
{
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
}
} // namespace

TEST_CASE ("Strict reader: the JSON it accepts, as a tree", "[presets][json]")
{
    json::Document doc;
    std::string error;
    const std::string text = " {\"b\" : [1, -2.5e1, true, false, null, \"x\\u00e4\\ud83c\\udfb9\\n\\\"\\\\\\/\"], \"a\":{}, \"c\":[]}\r\n\t";
    REQUIRE (doc.parse (text.data(), text.size(), {}, error));
    const auto root = doc.root();
    CHECK (doc.type (root) == json::Type::Object);
    CHECK (doc.size (root) == 3);
    CHECK (doc.memberKey (root, 0) == "a"); // members are kept sorted by key
    const auto b = doc.member (root, "b");
    REQUIRE (doc.type (b) == json::Type::Array);
    REQUIRE (doc.size (b) == 6);
    CHECK (doc.number (doc.element (b, 0)) > 0.99);
    CHECK (doc.number (doc.element (b, 1)) < -24.99);
    CHECK (doc.boolean (doc.element (b, 2)));
    CHECK (doc.type (doc.element (b, 3)) == json::Type::Bool);
    CHECK_FALSE (doc.boolean (doc.element (b, 3)));
    CHECK (doc.type (doc.element (b, 4)) == json::Type::Null);
    CHECK (doc.string (doc.element (b, 5)) == std::string_view ("x\xc3\xa4\xf0\x9f\x8e\xb9\n\"\\/"));
    CHECK (doc.member (root, "zz") == json::Document::npos);
    CHECK (doc.type (doc.member (root, "a")) == json::Type::Object);
    CHECK (doc.size (doc.member (root, "c")) == 0);
}

TEST_CASE ("Strict reader: everything that is not plain JSON is rejected", "[presets][json]")
{
    const std::vector<std::string> bad {
        "", " ", "{", "}", "[", "{]", "[}", "{\"a\":1,}", "[1,]", "[,1]", "{,}", "{\"a\" 1}", "{\"a\":}", "{a:1}", "{'a':1}",
        "['x']", "\"a\" \"b\"", "{} {}", "{}x", "[1 2]", "[01]", "[1.]", "[.5]", "[+1]", "[1e]", "[0x10]", "[NaN]", "[Infinity]",
        "[-Infinity]", "[nul]", "[nullx]", "[true1]", "[tru]", "[TRUE]", "[None]", "[\"\\x41\"]", "[\"\\u12\"]", "[\"\\u12G4\"]",
        "[\"\\ud800\"]", "[\"\\udc00\"]", "[\"\\ud800\\u0041\"]", "[\"\\u0000\"]", "[\"a\nb\"]", "[\"a\tb\"]", "[\"\\a\"]",
        "[\"\xff\"]", "[\"\xc0\xaf\"]", "[\"\xed\xa0\x80\"]", "[\"\xf4\x90\x80\x80\"]", "[\"\xe2\x82\"]", "/* c */ {}", "{} // c",
        "[1]\x0b", "\xef\xbb\xbf{}", "[\"unterminated]", "{\"a\":1,\"a\":2}", "{\"a\":{\"x\":1,\"y\":2,\"x\":3}}",
        "\xc2\xa0{}", // no-break space is not JSON white space
    };
    for (const auto& t : bad)
    {
        INFO (t);
        CHECK_FALSE (accepts (t));
    }
    // A NUL byte anywhere.
    CHECK_FALSE (accepts (std::string ("[\"a\0b\"]", 7)));
    CHECK_FALSE (accepts (std::string ("[1]\0", 4)));
}

TEST_CASE ("Strict reader: the review's desynchronisation inputs (single quotes) are rejected", "[presets][json]")
{
    const auto schema = smallSchema();
    // Single-quoted text used to hide a number from the old pre-check, which JUCE's parser then
    // overflowed on; the same trick hid nesting depth and trailing text.
    const std::string overflow = header() + ",\"x\":'\"',\"a\":9999999999999999999999999,\"b\":\"z\"}";
    const std::string deep = header() + ",\"x\":'\"',\"a\":" + std::string (200, '[') + std::string (200, ']') + ",\"b\":\"z\"}";
    const std::string trailing = header() + ",\"x\":'\"'}  garbage \"";
    for (const auto* t : { &overflow, &deep, &trailing })
    {
        const auto r = parseProgram (schema, *t);
        CHECK_FALSE (r.ok);
        CHECK (r.error.isNotEmpty());
    }
}

TEST_CASE ("Strict reader: caps on depth, members, elements, values and string length", "[presets][json]")
{
    json::Limits l;
    l.maxDepth = 12;
    l.maxMembers = 16;
    l.maxElements = 16;
    l.maxValues = 64;
    l.maxStringBytes = 8;
    CHECK (accepts (std::string (12, '[') + std::string (12, ']'), l));
    CHECK_FALSE (accepts (std::string (13, '[') + std::string (13, ']'), l));
    CHECK_FALSE (accepts (std::string (100000, '[') + std::string (100000, ']'), l)); // no stack growth: rejected at depth 13

    const auto list = [] (int n) {
        std::string s = "[";
        for (int i = 0; i < n; ++i)
            s += (i ? ",0" : "0");
        return s + "]";
    };
    const auto object = [] (int n) {
        std::string s = "{";
        for (int i = 0; i < n; ++i)
            s += (i ? ",\"" : "\"") + std::to_string (i) + "\":0";
        return s + "}";
    };
    CHECK (accepts (list (16), l));
    CHECK_FALSE (accepts (list (17), l));
    CHECK (accepts (object (16), l));
    CHECK_FALSE (accepts (object (17), l));
    CHECK (accepts ("[" + list (16) + "," + list (16) + "," + list (16) + "]", l)); // 1 + 3 + 48 = 52 values
    CHECK_FALSE (accepts ("[" + list (16) + "," + list (16) + "," + list (16) + "," + list (16) + "]", l)); // 69 values
    CHECK (accepts ("[\"12345678\"]", l));
    CHECK_FALSE (accepts ("[\"123456789\"]", l));
    CHECK_FALSE (accepts ("{\"123456789\":1}", l)); // keys too
    CHECK (accepts ("[\"\\u00e4\\u00e4\\u00e4\\u00e4\"]", l)); // 8 bytes decoded
    CHECK_FALSE (accepts ("[\"\\u00e4\\u00e4\\u00e4\\u00e4\\u00e4\"]", l));

    json::Limits small;
    small.maxInputBytes = 10;
    CHECK (accepts ("[1,2,3,4]", small));
    CHECK_FALSE (accepts ("[1,2,3,4,5]", small));
}

TEST_CASE ("Strict reader: time is linear in the input and memory is bounded by the caps", "[presets][json][serial]")
{
    // The review's quadratic case (80k keys in one object took 2.56 s under ASan) and its memory case
    // (8 MB of "{}" raised peak memory by ~360 MB) now end at a cap after linear work.
    std::string keys = header() + ",\"global\":{";
    for (int i = 0; i < 80000; ++i)
        keys += (i ? ",\"" : "\"") + std::to_string (i) + "\":0";
    keys += "}}";
    const auto schema = smallSchema();
    bool ok = true;
    const double tKeys = secondsOf ([&] { ok = parseProgram (schema, keys).ok; });
    CHECK_FALSE (ok); // over 1 MB and over 4096 members
    CHECK (tKeys < 0.5);

    std::string empties = header() + ",\"global\":[";
    while (empties.size() < kMaxFileBytes - 16)
        empties += "{},";
    empties += "{}]}";
    const double tEmpties = secondsOf ([&] { ok = parseProgram (schema, empties).ok; });
    CHECK_FALSE (ok); // over the value cap
    CHECK (tEmpties < 0.5);

    // The largest accepted documents: 4096 members in an object, and close to 1 MB of numbers
    // under the value cap (16 lists of 4096 long numbers).
    std::string members = header() + ",\"global\":{";
    for (int i = 0; i < 4096; ++i)
        members += (i ? ",\"" : "\"") + std::string (200, 'k') + std::to_string (i) + "\":-1.17549435e-38";
    members += "}}";
    double tMembers = 0.0;
    tMembers = secondsOf ([&] { ok = parseProgram (schema, members).ok; });
    CHECK (ok);
    std::string numbers = header() + ",\"global\":{";
    for (int k = 0; k < 15; ++k)
    {
        numbers += (k ? ",\"l" : "\"l") + std::to_string (k) + "\":[";
        for (int i = 0; i < 4096; ++i)
            numbers += (i ? ",-1.17549435e-38" : "-1.17549435e-38");
        numbers += "]";
    }
    numbers += "}}";
    REQUIRE (numbers.size() < kMaxFileBytes);
    double tNumbers = 0.0;
    tNumbers = secondsOf ([&] { ok = parseProgram (schema, numbers).ok; });
    CHECK (ok);
    std::printf ("strict reader: 80k keys rejected in %.4f s, 1 MB of {} rejected in %.4f s, 4096 long keys (%zu bytes) accepted in "
                 "%.4f s, %zu bytes of numbers accepted in %.4f s\n",
                 tKeys, tEmpties, members.size(), tMembers, numbers.size(), tNumbers);
    CHECK (tMembers < 1.0);
    CHECK (tNumbers < 1.0);
}

TEST_CASE ("Every file the writer produces is read back by the strict reader", "[presets][json]")
{
    const auto schema = smallSchema();
    Bank bank = makeInitBank (schema);
    for (int i = 0; i < kTestBankSize; ++i)
        bank.programs[static_cast<size_t> (i)] = randomProgram (schema, 500u + static_cast<unsigned> (i),
                                                                juce::String::fromUTF8 ("N\xc3\xa4 \"q\" \\ ") + juce::String (i));
    for (const auto& text : { toJson (schema, bank), toJson (schema, bank.programs[3]), toJson (schema, toneOf (schema, bank.programs[4], 1)) })
    {
        json::Document doc;
        std::string error;
        const bool parsed = doc.parse (text.data(), text.size(), {}, error);
        INFO (error);
        CHECK (parsed);
        bool noControls = true; // no control characters except LF (escaped inside strings)
        for (const char ch : text)
            noControls = noControls && (static_cast<unsigned char> (ch) >= 0x20 || ch == '\n');
        CHECK (noControls);
    }
}

TEST_CASE ("Bounded integers in session state", "[presets][json]")
{
    CHECK (json::parseBoundedInt ("0", -1, 63, 7) == 0);
    CHECK (json::parseBoundedInt ("63", -1, 63, 7) == 63);
    CHECK (json::parseBoundedInt ("-1", -1, 63, 7) == -1);
    for (const char* bad : { "64", "-2", "", "-", " 1", "1 ", "+1", "01", "1.0", "1e1", "0x1", "99999999999999999999", "-99999999999",
                             "2147483648", "12a" })
    {
        INFO (bad);
        CHECK (json::parseBoundedInt (bad, -1, 63, 7) == 7);
    }
}

TEST_CASE ("Numbers: text written and read without the process locale", "[presets][json][locale]")
{
    const auto schema = smallSchema();
    const auto program = randomProgram (schema, 77);
    const auto reference = toJson (schema, program);
    const std::string expectedNumbers[] = { "0.65", "1234.5", "-0.4", "1e-05", "1e+10", "60", "0.0001" };
    const float values[] = { 0.65f, 1234.5f, -0.4f, 1.0e-5f, 1.0e10f, 60.0f, 1.0e-4f };

    const char* tried[] = { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8", "ru_RU.UTF-8", "C.UTF-8" };
    int localesTried = 0;
    for (const char* name : tried)
    {
        if (std::setlocale (LC_ALL, name) == nullptr)
            continue;
        ++localesTried;
        INFO (name);
        for (size_t i = 0; i < std::size (values); ++i)
            CHECK (formatNumber (values[i]) == expectedNumbers[i]);
        CHECK (toJson (schema, program) == reference);
        const auto back = parseProgram (schema, reference);
        REQUIRE (back.ok);
        CHECK (toJson (schema, back.value) == reference);
        double d = 0.0;
        CHECK (json::parseNumber ("0.25", 4, d));
        CHECK (d > 0.2499);
        CHECK (d < 0.2501);
    }
    std::setlocale (LC_ALL, "C");
    INFO ("locales available: " << localesTried);
    CHECK (formatNumber (0.65f) == "0.65");
    SUCCEED();
}

TEST_CASE ("formatNumber: every float written reads back exactly through our reader", "[presets][json]")
{
    std::mt19937 rng (11);
    int checked = 0;
    for (int i = 0; i < 200000; ++i)
    {
        // Random bit patterns cover every exponent; non-finite ones are skipped.
        const auto bits = static_cast<std::uint32_t> (rng());
        float v = 0.0f;
        std::memcpy (&v, &bits, sizeof v);
        if (! std::isfinite (v))
            continue;
        const auto text = formatNumber (v);
        double d = 0.0;
        REQUIRE (json::parseNumber (text.data(), text.size(), d));
        INFO (text);
        REQUIRE (sameBitsF (static_cast<float> (d), v + 0.0f));
        ++checked;
    }
    CHECK (checked > 190000);
}

TEST_CASE ("A file of another instrument is rejected as a whole", "[presets][format]")
{
    const auto schema = smallSchema();
    const auto program = toJson (schema, randomProgram (schema, 8));
    REQUIRE (parseProgram (schema, program).ok);
    const auto swap = [&] (const std::string& text, const std::string& from, const std::string& to) {
        auto t = text;
        const auto at = t.find (from);
        REQUIRE (at != std::string::npos);
        t.replace (at, from.size(), to);
        return t;
    };
    CHECK_FALSE (parseProgram (schema, swap (program, "\"test-synth\"", "\"other-synth\"")).ok);
    CHECK_FALSE (parseProgram (schema, swap (program, "\"instrument\": \"test-synth\",\n", "")).ok);
    CHECK_FALSE (parseProgram (schema, swap (program, "\"test-synth\"", "5")).ok);
    CHECK_FALSE (parseProgram (schema, swap (program, "\"test-synth\"", "\"TEST-SYNTH\"")).ok);
    const auto tone = toJson (schema, toneOf (schema, randomProgram (schema, 9), 0));
    CHECK_FALSE (parseTone (schema, swap (tone, "\"test-synth\"", "\"other-synth\"")).ok);
    const auto bank = toJson (schema, makeInitBank (schema));
    CHECK_FALSE (parseBank (schema, swap (bank, "\"test-synth\"", "\"other-synth\"")).ok);

    // Bank size, slot naming and extensions come from the instrument too.
    auto other = testInstrument();
    other.id = "other-synth";
    other.groups = 2;
    other.slotsPerGroup = 4;
    std::vector<ParamSpec> specs { { "a.x", 0, 1, 0, false }, { "b.x", 0, 1, 0, false } };
    const Schema small (specs, { "a.", "b." }, {}, other);
    REQUIRE (small.isValid());
    CHECK (small.bankSize() == 8);
    CHECK (makeInitBank (small).programs.size() == 8);
    CHECK (other.positionName (7) == "B4");
    CHECK (other.slotFromFileName ("B4-x") == 7);
    CHECK (other.slotFromFileName ("B5-x") == -1);
    CHECK (other.slotFromFileName ("C1-x") == -1);
    CHECK_FALSE (parseProgram (small, program).ok); // a test-synth program is not an other-synth one
}

TEST_CASE ("Parts with different specs for the same suffix make an invalid schema", "[presets][format]")
{
    std::vector<ParamSpec> specs { { "a.x", 0, 1, 0.5f, false }, { "b.x", 0, 2, 0.5f, false } };
    CHECK_FALSE (Schema (specs, { "a.", "b." }, {}, testInstrument()).isValid());
    specs[1] = { "b.x", 0, 1, 0.5f, true };
    CHECK_FALSE (Schema (specs, { "a.", "b." }, {}, testInstrument()).isValid());
    specs[1] = { "b.x", 0, 1, 0.5f, false };
    CHECK (Schema (specs, { "a.", "b." }, {}, testInstrument()).isValid());
}

TEST_CASE ("Names lose control, bidirectional and invisible format characters", "[presets][format]")
{
    const auto u = [] (const char* s) { return juce::String::fromUTF8 (s); };
    CHECK (sanitiseText (u ("a\xc2\x85" "b"), 40) == "a b");                // C1 control (NEL) -> space
    CHECK (sanitiseText (u ("a\xc2\x9b" "b"), 40) == "a b");                // C1 CSI
    CHECK (sanitiseText (u ("evil\xe2\x80\xae" "gnp.exe"), 40) == "evilgnp.exe"); // right-to-left override
    CHECK (sanitiseText (u ("a\xe2\x81\xa6" "b\xe2\x81\xa9" "c"), 40) == "abc");  // isolates
    CHECK (sanitiseText (u ("a\xe2\x80\x8b" "b\xe2\x80\x8d" "c\xef\xbb\xbf"), 40) == "abc"); // zero-width, joiner, BOM
    CHECK (sanitiseText (u ("a\xe2\x80\xa8" "b"), 40) == "a b");            // line separator
    CHECK (sanitiseText (u ("a\xf3\xa0\x80\x81" "b"), 40) == "ab");         // tag character
    CHECK (sanitiseText (u ("Pad \xc3\xa4\xc3\xb6 \xe2\x99\xaa"), 40) == u ("Pad \xc3\xa4\xc3\xb6 \xe2\x99\xaa")); // ordinary text stays
    CHECK (sanitiseName (u ("\xe2\x80\xae\xe2\x80\x8b")) == "Untitled");
}

namespace
{
/** Mutates `seedText` with token insertions, byte flips, deletions and truncations; checks that the
    reader never misbehaves and that whatever is accepted is valid and stable. Runs until `seconds`. */
struct FuzzResult
{
    long iterations = 0, accepted = 0;
    double worstSeconds = 0.0;
    size_t worstBytes = 0;
};

FuzzResult fuzz (unsigned seed, double seconds, long maxIterations)
{
    const auto schema = smallSchema();
    std::vector<std::string> seeds;
    seeds.push_back (toJson (schema, randomProgram (schema, seed, "Fuzz")));
    Bank bank = makeInitBank (schema);
    for (int i = 0; i < 6; ++i)
        bank.programs[static_cast<size_t> (i)] = randomProgram (schema, seed + 40u + static_cast<unsigned> (i));
    seeds.push_back (toJson (schema, bank));
    seeds.push_back (toJson (schema, toneOf (schema, randomProgram (schema, seed + 7u), 1)));

    static const std::string tokens[] = {
        "{", "}", "[", "]", ",", ":", "\"", "'", "\\", "\\u", "\\ud83c", "\\udfb9", "\\u0000", "\\uDFFF", "\\\"", "\\n", "\\x",
        "1e999", "-1e-999", "999999999", "1234567890", "0.00000000000000000001", "000", "-", "+", ".", "e", "E", "null", "true",
        "false", "nul", "NaN", "Infinity", "0x10", "\n", "\t", "\r", " ", "\xc2\xa0", "\xef\xbb\xbf", "\xff", "\xc0\xaf",
        "\xed\xa0\x80", "\xf0\x9f\x8e\xb9", "\xe2\x80\xae", std::string (1, '\0'), "//", "/*", "\"format\":1", "\"format\":2",
        "\"instrument\":\"test-synth\"", "\"kind\":\"bank\"", "\"programs\":[", "\"a\":{", "\"global\":{", "\"cut\":0.5",
        "\"cut\":0.5,\"cut\":0.6", std::string (40, '['), std::string (40, '{'), std::string (13, '['), std::string (300, '9'),
    };
    std::mt19937 rng (seed);
    FuzzResult r;
    const auto start = std::chrono::steady_clock::now();
    for (; r.iterations < maxIterations; ++r.iterations)
    {
        if ((r.iterations & 63) == 0 && std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count() > seconds)
            break;
        const size_t which = rng() % seeds.size();
        std::string s = seeds[which];
        const int edits = 1 + static_cast<int> (rng() % 6);
        for (int e = 0; e < edits; ++e)
        {
            const auto at = static_cast<size_t> (rng() % (s.size() + 1));
            switch (rng() % 6)
            {
                case 0: if (at < s.size()) s[at] = static_cast<char> (rng() % 256); break;
                case 1: if (at < s.size()) s[at] = static_cast<char> (s[at] ^ (1 << (rng() % 8))); break;
                case 2: if (at < s.size()) s.erase (at, 1 + rng() % 8); break;
                case 3: s.insert (at, tokens[rng() % std::size (tokens)]); break;
                case 4: if (at < s.size()) s.insert (at, s.substr (at, rng() % 64)); break; // duplicate a span (duplicate keys, nesting)
                default: s.resize (at); break;
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = false;
        if (which == 1)
        {
            const auto p = parseBank (schema, s);
            ok = p.ok;
            if (ok)
            {
                REQUIRE (p.value.programs.size() == static_cast<size_t> (kTestBankSize));
                const auto again = parseBank (schema, toJson (schema, p.value));
                REQUIRE (again.ok);
                REQUIRE (toJson (schema, again.value) == toJson (schema, p.value));
            }
            else
                REQUIRE (p.error.isNotEmpty());
        }
        else if (which == 2)
        {
            const auto p = parseTone (schema, s);
            ok = p.ok;
            if (ok)
                REQUIRE (parseTone (schema, toJson (schema, p.value)).ok);
            else
                REQUIRE (p.error.isNotEmpty());
        }
        else
        {
            const auto p = parseProgram (schema, s);
            ok = p.ok;
            if (ok)
            {
                for (int i = 0; i < schema.numParams(); ++i)
                {
                    const float v = p.value.values[static_cast<size_t> (i)];
                    REQUIRE (std::isfinite (v));
                    REQUIRE (v >= schema.spec (i).lo);
                    REQUIRE (v <= schema.spec (i).hi);
                }
                const auto text = toJson (schema, p.value);
                const auto again = parseProgram (schema, text);
                REQUIRE (again.ok);
                REQUIRE (toJson (schema, again.value) == text);
            }
            else
                REQUIRE (p.error.isNotEmpty());
        }
        const double dt = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        if (dt > r.worstSeconds)
        {
            r.worstSeconds = dt;
            r.worstBytes = s.size();
        }
        r.accepted += ok ? 1 : 0;

        // The raw reader on its own, with the generic limits.
        json::Document doc;
        std::string error;
        if (! doc.parse (s.data(), s.size(), {}, error))
            REQUIRE_FALSE (error.empty());
    }
    return r;
}
} // namespace

TEST_CASE ("Fuzzing the strict reader: seeded, time-bounded; never a crash, accepted files are valid and stable", "[presets][json][fuzz][slow][serial]")
{
    const auto r = fuzz (20261113u, 4.0, 60000);
    std::printf ("strict reader fuzz: %ld inputs, %ld accepted, slowest %.5f s (%zu bytes)\n", r.iterations, r.accepted, r.worstSeconds,
                 r.worstBytes);
    CHECK (r.iterations > 2000);
    CHECK (r.accepted > 50);
    CHECK (r.accepted < r.iterations);
    CHECK (r.worstSeconds < 0.5);
}

TEST_CASE ("Fuzzing the strict reader for a long time", "[.fuzzlong]")
{
    long total = 0, accepted = 0;
    double worst = 0.0;
    for (unsigned seed = 1; seed <= 20; ++seed)
    {
        const auto r = fuzz (seed, 30.0, 100000000);
        total += r.iterations;
        accepted += r.accepted;
        worst = std::max (worst, r.worstSeconds);
    }
    std::printf ("long fuzz: %ld inputs, %ld accepted, slowest %.5f s\n", total, accepted, worst);
    CHECK (worst < 1.0);
}
