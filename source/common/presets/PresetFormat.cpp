#include "common/presets/PresetFormat.h"

#include "common/presets/StrictJson.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace vs::presets
{
// ---------------------------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------------------------
namespace
{
/** |v| as text with the significant digits of n written at decimal exponent x (the exponent of n's
    first digit), in printf's %g layout for `precision` digits: fixed notation unless x < -4 or
    x >= precision, then d.ddde+XX; trailing zeros dropped. No locale is involved. */
std::string layoutDigits (bool negative, std::uint64_t n, int x, int precision)
{
    std::string digits = std::to_string (n); // integer formatting: locale-independent
    while (digits.size() > 1 && digits.back() == '0')
        digits.pop_back();
    std::string out = negative ? "-" : "";
    if (x < -4 || x >= precision)
    {
        out += digits[0];
        if (digits.size() > 1)
            out += "." + digits.substr (1);
        out += x < 0 ? "e-" : "e+";
        const auto e = std::to_string (x < 0 ? -x : x);
        out += (e.size() < 2 ? "0" : "") + e;
        return out;
    }
    if (x < 0)
        return out + "0." + std::string (static_cast<size_t> (-x - 1), '0') + digits;
    const auto intDigits = static_cast<size_t> (x) + 1u;
    if (digits.size() <= intDigits)
        return out + digits + std::string (intDigits - digits.size(), '0');
    return out + digits.substr (0, intDigits) + "." + digits.substr (intDigits);
}

double timesPow10 (double v, int k)
{
    // Exact powers of ten up to 1e22, applied in steps (k stays within about +-60 here).
    static constexpr double p10[] = { 1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    while (k > 22)
    {
        v *= p10[22];
        k -= 22;
    }
    while (k < -22)
    {
        v /= p10[22];
        k += 22;
    }
    return k >= 0 ? v * p10[k] : v / p10[-k];
}

bool readsBackAs (const std::string& text, float v)
{
    double d = 0.0;
    return json::parseNumber (text.data(), text.size(), d) && std::bit_cast<std::uint32_t> (static_cast<float> (d)) == std::bit_cast<std::uint32_t> (v);
}
} // namespace

std::string formatNumber (float v)
{
    if (! std::isfinite (v))
        v = 0.0f; // never written: callers sanitise first
    if (std::fpclassify (v) == FP_ZERO)
        return "0"; // also turns -0 into 0
    const bool negative = std::signbit (v);
    const double magnitude = std::fabs (static_cast<double> (v));
    // Decimal exponent of the first significant digit (corrected below if log10 is off by one).
    int x = static_cast<int> (std::floor (std::log10 (magnitude)));
    // A value of 10 or more needs at least its integer digits: 1 digit would write 60 as "6e+01".
    const int first = magnitude >= 1.0 ? std::min (9, x + 1) : 1;
    std::string text;
    for (int precision = first; precision <= 9; ++precision)
    {
        // The nearest `precision`-digit decimal: n * 10^(x - precision + 1), n in [10^(p-1), 10^p).
        int ex = x;
        double scaled = timesPow10 (magnitude, precision - 1 - ex);
        auto n = static_cast<std::uint64_t> (std::llround (scaled));
        const auto lo = static_cast<std::uint64_t> (std::llround (timesPow10 (1.0, precision - 1)));
        if (n < lo) // log10 rounded up across a power of ten
        {
            --ex;
            scaled = timesPow10 (magnitude, precision - 1 - ex);
            n = static_cast<std::uint64_t> (std::llround (scaled));
        }
        if (n >= lo * 10u) // rounding carried into a new digit (9.99 -> 10.0)
        {
            ++ex;
            n = (n + 5u) / 10u;
        }
        text = layoutDigits (negative, n, ex, precision);
        if (readsBackAs (text, v))
            break;
    }
    return text;
}

namespace
{
std::string quoteBytes (std::string_view text)
{
    std::string out = "\"";
    for (const char ch : text)
    {
        const auto c = static_cast<unsigned char> (ch);
        if (c == '"')
            out += "\\\"";
        else if (c == '\\')
            out += "\\\\";
        else if (c == '\t')
            out += "\\t"; // the short escapes: a table may hold tabs, and "\u0009" would be three times the size
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\b')
            out += "\\b";
        else if (c == '\f')
            out += "\\f";
        else if (c < 0x20)
        {
            static constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 0xFu];
        }
        else
            out += static_cast<char> (c);
    }
    return out + "\"";
}

std::string quote (const juce::String& text) { return quoteBytes (text.toRawUTF8()); }

std::string pad (int level);

/** "[ ... ]" with one string per line; the elements sit one level inside `level`. */
std::string stringList (const std::vector<std::string>& lines, int level)
{
    if (lines.empty())
        return "[]";
    std::string out = "[\n";
    for (size_t i = 0; i < lines.size(); ++i)
        out += pad (level + 1) + quoteBytes (lines[i]) + (i + 1 < lines.size() ? ",\n" : "\n");
    return out + pad (level) + "]";
}

std::string pad (int level) { return std::string (static_cast<size_t> (level) * 2u, ' '); }

/** "{ ... }" with one member per line; the members sit one level inside `level`. */
std::string object (const std::vector<std::pair<std::string, std::string>>& members, int level)
{
    if (members.empty())
        return "{}";
    std::string out = "{\n";
    for (size_t i = 0; i < members.size(); ++i)
        out += pad (level + 1) + quote (members[i].first) + ": " + members[i].second + (i + 1 < members.size() ? ",\n" : "\n");
    return out + pad (level) + "}";
}

std::string quoted (const juce::String& s) { return quote (s); }

std::vector<std::pair<std::string, std::string>> metaMembers (const Meta& m)
{
    return { { "name", quoted (sanitiseName (m.name)) },
             { "category", quoted (sanitiseText (m.category, kMaxCategoryChars)) },
             { "author", quoted (sanitiseText (m.author, kMaxAuthorChars)) } };
}

float clean (const Schema& schema, int i, float v) { return schema.sanitise (i, static_cast<double> (v)); }

/** The program's members (without the file header), for a program file or a bank entry. */
std::vector<std::pair<std::string, std::string>> programMembers (const Schema& schema, const Program& p, int level)
{
    auto members = metaMembers (p.meta);
    std::vector<std::pair<std::string, std::string>> globals;
    for (int i : schema.globalParams())
        globals.emplace_back (schema.spec (i).id.toStdString(), formatNumber (clean (schema, i, p.values[static_cast<size_t> (i)])));
    members.emplace_back ("global", object (globals, level));
    for (int t = 0; t < schema.numTones(); ++t)
    {
        std::vector<std::pair<std::string, std::string>> tone;
        for (int k = 0; k < schema.toneSize(); ++k)
        {
            const int i = schema.toneParam (t, k);
            tone.emplace_back (schema.toneSuffix (k).toStdString(), formatNumber (clean (schema, i, p.values[static_cast<size_t> (i)])));
        }
        members.emplace_back (schema.toneKey (t).toStdString(), object (tone, level));
    }
    if (schema.instrument().hasTables())
    {
        // One list per tone, in the schema's order; an empty list is "no table".
        std::vector<std::pair<std::string, std::string>> tables;
        for (int t = 0; t < schema.numTones(); ++t)
        {
            static const std::vector<std::string> none;
            const auto& lines = static_cast<size_t> (t) < p.data.size() ? p.data[static_cast<size_t> (t)] : none;
            tables.emplace_back (schema.toneKey (t).toStdString(), stringList (lines, level + 1));
        }
        members.emplace_back ("tables", object (tables, level));
    }
    return members;
}

std::vector<std::pair<std::string, std::string>> header (const Schema& schema, const char* kind)
{
    return { { "format", std::to_string (kFormat) }, { "instrument", quote (schema.instrument().id) }, { "kind", std::string ("\"") + kind + "\"" } };
}
} // namespace

std::string toJson (const Schema& schema, const Program& program)
{
    auto members = header (schema, "program");
    for (auto& m : programMembers (schema, program, 1))
        members.push_back (std::move (m));
    return object (members, 0) + "\n";
}

std::string toJson (const Schema& schema, const Tone& tone)
{
    auto members = header (schema, "tone");
    for (auto& m : metaMembers (tone.meta))
        members.push_back (std::move (m));
    std::vector<std::pair<std::string, std::string>> params;
    for (int k = 0; k < schema.toneSize(); ++k)
    {
        const int i = schema.toneParam (0, k);
        params.emplace_back (schema.toneSuffix (k).toStdString(), formatNumber (clean (schema, i, tone.values[static_cast<size_t> (k)])));
    }
    members.emplace_back ("params", object (params, 1));
    if (schema.instrument().hasTables())
        members.emplace_back ("table", stringList (tone.data, 1));
    return object (members, 0) + "\n";
}

std::string toJson (const Schema& schema, const Bank& bank)
{
    auto members = header (schema, "bank");
    members.emplace_back ("name", quoted (sanitiseName (bank.name)));
    members.emplace_back ("author", quoted (sanitiseText (bank.author, kMaxAuthorChars)));

    std::string list = "[\n";
    for (size_t i = 0; i < bank.programs.size(); ++i)
        list += pad (2) + object (programMembers (schema, bank.programs[i], 2), 2) + (i + 1 < bank.programs.size() ? ",\n" : "\n");
    list += pad (1) + "]";
    if (bank.programs.empty())
        list = "[]";
    members.emplace_back ("programs", list);
    return object (members, 0) + "\n";
}

// ---------------------------------------------------------------------------------------------
// Reading (StrictJson.h does the parsing; this maps the tree onto the model)
// ---------------------------------------------------------------------------------------------
namespace
{
using json::Document;
using Index = Document::Index;

juce::String fromUtf8 (std::string_view s) { return juce::String::fromUTF8 (s.data(), static_cast<int> (s.size())); }

/** Checks the text and returns the root object after format, instrument and kind are verified. */
bool openRoot (const Schema& schema, const char* utf8, size_t n, const char* kind, Document& doc, juce::String& error)
{
    if (utf8 == nullptr || n == 0)
    {
        error = "The file is empty.";
        return false;
    }
    const auto& inst = schema.instrument();
    if (n > inst.fileCap())
    {
        error = "The file is too large to be a preset.";
        return false;
    }
    if (n >= 3 && static_cast<unsigned char> (utf8[0]) == 0xEF && static_cast<unsigned char> (utf8[1]) == 0xBB
        && static_cast<unsigned char> (utf8[2]) == 0xBF)
    {
        utf8 += 3;
        n -= 3;
    }
    json::Limits limits;
    limits.maxInputBytes = inst.fileCap();
    limits.maxDepth = kMaxJsonDepth;
    if (inst.maxJsonValues > 0)
        limits.maxValues = inst.maxJsonValues;
    limits.maxElements = std::max (limits.maxElements, inst.maxTableLines);
    std::string why;
    if (! doc.parse (utf8, n, limits, why))
    {
        error = "Not a valid preset file (or truncated): " + juce::String (why);
        return false;
    }
    const Index root = doc.root();
    if (doc.type (root) != json::Type::Object)
    {
        error = "The file does not contain a preset object.";
        return false;
    }
    const Index format = doc.member (root, "format");
    if (! doc.isNumber (format))
    {
        error = "The file has no format number.";
        return false;
    }
    if (! (std::fabs (doc.number (format) - static_cast<double> (kFormat)) < 1.0e-9))
    {
        const double f = doc.number (format);
        const bool whole = std::isfinite (f) && std::fabs (f) < 1.0e6 && std::fabs (f - std::round (f)) < 1.0e-9;
        error = "Unsupported preset format" + (whole ? " " + juce::String (std::to_string (static_cast<long long> (std::llround (f)))) : juce::String())
                + " (this version reads format " + juce::String (std::to_string (kFormat)) + ").";
        return false;
    }
    const Index instrument = doc.member (root, "instrument");
    if (doc.type (instrument) != json::Type::String || fromUtf8 (doc.string (instrument)) != schema.instrument().id)
    {
        error = "This file is not for this instrument.";
        return false;
    }
    const Index k = doc.member (root, "kind");
    if (doc.type (k) != json::Type::String || doc.string (k) != std::string_view (kind))
    {
        error = juce::String ("This is not a ") + kind + " file.";
        return false;
    }
    return true;
}

bool readText (const Document& doc, Index obj, const char* key, int maxChars, juce::String& out, juce::String& error, bool isName)
{
    const Index v = doc.member (obj, key);
    if (v == Document::npos)
    {
        if (isName)
            out = "Untitled";
        return true;
    }
    if (doc.type (v) != json::Type::String)
    {
        error = juce::String ("\"") + key + "\" must be text.";
        return false;
    }
    out = isName ? sanitiseName (fromUtf8 (doc.string (v))) : sanitiseText (fromUtf8 (doc.string (v)), maxChars);
    return true;
}

bool readMeta (const Document& doc, Index obj, Meta& meta, juce::String& error)
{
    return readText (doc, obj, "name", kMaxNameChars, meta.name, error, true)
           && readText (doc, obj, "category", kMaxCategoryChars, meta.category, error, false)
           && readText (doc, obj, "author", kMaxAuthorChars, meta.author, error, false);
}

/** Reads the members of `section` into values[index(k)]; missing or unusable values keep what is
    in `values` (the default, set by the caller). */
template <typename KeyFn>
bool readSection (const Schema& schema, const Document& doc, Index section, const juce::String& sectionName,
                  const std::vector<int>& indices, KeyFn keyOf, std::vector<float>& values, juce::String& error)
{
    if (section == Document::npos)
        return true; // missing section: all defaults
    if (doc.type (section) != json::Type::Object)
    {
        error = "\"" + sectionName + "\" must be an object.";
        return false;
    }
    for (size_t k = 0; k < indices.size(); ++k)
    {
        const int i = indices[k];
        const Index v = doc.member (section, keyOf (k, i));
        if (doc.isNumber (v))
            values[static_cast<size_t> (i)] = schema.sanitise (i, doc.number (v)); // non-finite -> default there
        // any other kind of value (text, null, boolean, nested): the default stays
    }
    return true;
}

/** Reads one table: a list of strings. Missing = no table. The message names `what`. */
bool readTableLines (const Schema& schema, const Document& doc, Index list, const juce::String& what, std::vector<std::string>& lines,
                     juce::String& error)
{
    lines.clear();
    if (list == Document::npos)
        return true;
    if (doc.type (list) != json::Type::Array)
    {
        error = what + " must be a list of text lines.";
        return false;
    }
    const auto& inst = schema.instrument();
    const size_t n = doc.size (list);
    if (n > static_cast<size_t> (inst.maxTableLines))
    {
        error = what + " has more than " + juce::String (inst.maxTableLines) + " lines.";
        return false;
    }
    size_t bytes = 0;
    lines.reserve (n);
    for (size_t i = 0; i < n; ++i)
    {
        const Index e = doc.element (list, i);
        if (doc.type (e) != json::Type::String)
        {
            error = what + ", line " + juce::String (static_cast<int> (i) + 1) + ": a table line must be text.";
            lines.clear();
            return false;
        }
        const auto text = doc.string (e);
        bytes += text.size() + 1u;
        if (bytes > inst.maxTableBytes)
        {
            error = what + " is longer than " + juce::String (static_cast<juce::int64> (inst.maxTableBytes)) + " bytes.";
            lines.clear();
            return false;
        }
        lines.emplace_back (text);
    }
    if (auto e = validateTableLines (inst, lines); e.isNotEmpty())
    {
        error = what + ": " + e;
        lines.clear();
        return false;
    }
    return true;
}

/** "Lower table of "Name"" for messages. */
juce::String tableName (const juce::String& part, const juce::String& programName)
{
    auto p = part;
    if (p.isNotEmpty())
        p = p.substring (0, 1).toUpperCase() + p.substring (1);
    return p + " table" + (programName.isNotEmpty() ? " of \"" + programName + "\"" : juce::String());
}

bool readProgramObject (const Schema& schema, const Document& doc, Index obj, Program& out, juce::String& error)
{
    if (doc.type (obj) != json::Type::Object)
    {
        error = "A program must be an object.";
        return false;
    }
    Program p;
    p.values = schema.defaults();
    if (! readMeta (doc, obj, p.meta, error))
        return false;
    const auto globalKey = [&] (size_t, int i) { return schema.spec (i).id.toStdString(); };
    if (! readSection (schema, doc, doc.member (obj, "global"), "global", schema.globalParams(), globalKey, p.values, error))
        return false;
    for (int t = 0; t < schema.numTones(); ++t)
    {
        std::vector<int> indices;
        for (int k = 0; k < schema.toneSize(); ++k)
            indices.push_back (schema.toneParam (t, k));
        const auto toneKey = [&] (size_t k, int) { return schema.toneSuffix (static_cast<int> (k)).toStdString(); };
        if (! readSection (schema, doc, doc.member (obj, schema.toneKey (t).toStdString()), schema.toneKey (t), indices, toneKey, p.values,
                           error))
            return false;
    }
    if (schema.instrument().hasTables())
    {
        // "tables": { "<tone key>": [ "<line>", ... ], ... }. Missing or empty = no table; keys that
        // are not a tone of this schema are ignored like unknown parameter IDs.
        p.data.assign (static_cast<size_t> (schema.numTones()), {});
        const Index tables = doc.member (obj, "tables");
        if (tables != Document::npos)
        {
            if (doc.type (tables) != json::Type::Object)
            {
                error = "\"tables\" must be an object.";
                return false;
            }
            for (int t = 0; t < schema.numTones(); ++t)
                if (! readTableLines (schema, doc, doc.member (tables, schema.toneKey (t).toStdString()),
                                      tableName (schema.toneKey (t), p.meta.name), p.data[static_cast<size_t> (t)], error))
                    return false;
        }
    }
    out = std::move (p);
    return true;
}
} // namespace

Parsed<Program> parseProgram (const Schema& schema, const char* utf8, size_t numBytes)
{
    Parsed<Program> result;
    Document doc;
    if (openRoot (schema, utf8, numBytes, "program", doc, result.error) && readProgramObject (schema, doc, doc.root(), result.value, result.error))
        result.ok = true;
    else
        result.value = {};
    return result;
}

Parsed<Tone> parseTone (const Schema& schema, const char* utf8, size_t numBytes)
{
    Parsed<Tone> result;
    Document doc;
    if (! openRoot (schema, utf8, numBytes, "tone", doc, result.error))
        return result;
    const Index root = doc.root();
    Tone t;
    if (! readMeta (doc, root, t.meta, result.error))
        return result;
    // A tone has no part: it is read against part 0's parameters (ranges, defaults, discreteness).
    // Every part has the same specs for the same suffix (Schema::isValid checks it), so the result
    // fits any part.
    std::vector<float> values = schema.defaults();
    std::vector<int> indices;
    for (int k = 0; k < schema.toneSize(); ++k)
        indices.push_back (schema.toneParam (0, k));
    const auto key = [&] (size_t k, int) { return schema.toneSuffix (static_cast<int> (k)).toStdString(); };
    if (! readSection (schema, doc, doc.member (root, "params"), "params", indices, key, values, result.error))
        return result;
    for (int i : indices)
        t.values.push_back (values[static_cast<size_t> (i)]);
    if (schema.instrument().hasTables()
        && ! readTableLines (schema, doc, doc.member (root, "table"), "Table of \"" + t.meta.name + "\"", t.data,
                             result.error))
        return result;
    result.value = std::move (t);
    result.ok = true;
    return result;
}

Parsed<Bank> parseBank (const Schema& schema, const char* utf8, size_t numBytes)
{
    Parsed<Bank> result;
    Document doc;
    if (! openRoot (schema, utf8, numBytes, "bank", doc, result.error))
        return result;
    const Index root = doc.root();
    Bank b = makeInitBank (schema);
    b.name = "Untitled";
    if (! readText (doc, root, "name", kMaxNameChars, b.name, result.error, true)
        || ! readText (doc, root, "author", kMaxAuthorChars, b.author, result.error, false))
        return result;
    const Index programs = doc.member (root, "programs");
    if (programs != Document::npos)
    {
        if (doc.type (programs) != json::Type::Array)
        {
            result.error = "\"programs\" must be a list.";
            return result;
        }
        if (doc.size (programs) > static_cast<size_t> (schema.bankSize()))
        {
            result.error = "A bank holds at most " + juce::String (schema.bankSize()) + " programs.";
            return result;
        }
        for (size_t i = 0; i < doc.size (programs); ++i)
            if (! readProgramObject (schema, doc, doc.element (programs, i), b.programs[i], result.error))
            {
                result.error = "Program " + juce::String (static_cast<int> (i) + 1) + ": " + result.error;
                return result;
            }
    }
    result.value = std::move (b);
    result.ok = true;
    return result;
}
} // namespace vs::presets
