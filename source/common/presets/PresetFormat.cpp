#include "common/presets/PresetFormat.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vs::presets
{
// ---------------------------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------------------------
std::string formatNumber (float v)
{
    if (! std::isfinite (v))
        v = 0.0f; // never written: callers sanitise first
    if (std::fpclassify (v) == FP_ZERO)
        return "0"; // also turns -0 into 0
    // A value of 10 or more needs at least its integer digits: "%.1g" would write 60 as "6e+01".
    const double magnitude = std::fabs (static_cast<double> (v));
    const int first = magnitude >= 1.0 ? std::min (9, static_cast<int> (std::floor (std::log10 (magnitude))) + 1) : 1;
    char buf[48] = "0";
    for (int digits = first; digits <= 9; ++digits)
    {
        std::snprintf (buf, sizeof buf, "%.*g", digits, static_cast<double> (v));
        if (std::bit_cast<std::uint32_t> (std::strtof (buf, nullptr)) == std::bit_cast<std::uint32_t> (v) || digits == 9)
            break;
    }
    std::string s (buf);
    std::replace (s.begin(), s.end(), ',', '.'); // a host may have set a decimal-comma locale
    return s;
}

namespace
{
std::string quote (const juce::String& text)
{
    std::string out = "\"";
    for (const char* p = text.toRawUTF8(); *p != 0; ++p)
    {
        const auto c = static_cast<unsigned char> (*p);
        if (c == '"')
            out += "\\\"";
        else if (c == '\\')
            out += "\\\\";
        else if (c < 0x20)
        {
            char buf[8];
            std::snprintf (buf, sizeof buf, "\\u%04x", static_cast<unsigned> (c));
            out += buf;
        }
        else
            out += static_cast<char> (c);
    }
    return out + "\"";
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
    return members;
}

std::vector<std::pair<std::string, std::string>> header (const char* kind)
{
    return { { "format", std::to_string (kFormat) }, { "kind", std::string ("\"") + kind + "\"" } };
}
} // namespace

std::string toJson (const Schema& schema, const Program& program)
{
    auto members = header ("program");
    for (auto& m : programMembers (schema, program, 1))
        members.push_back (std::move (m));
    return object (members, 0) + "\n";
}

std::string toJson (const Schema& schema, const Tone& tone)
{
    auto members = header ("tone");
    for (auto& m : metaMembers (tone.meta))
        members.push_back (std::move (m));
    std::vector<std::pair<std::string, std::string>> params;
    for (int k = 0; k < schema.toneSize(); ++k)
    {
        const int i = schema.toneParam (0, k);
        params.emplace_back (schema.toneSuffix (k).toStdString(), formatNumber (clean (schema, i, tone.values[static_cast<size_t> (k)])));
    }
    members.emplace_back ("params", object (params, 1));
    return object (members, 0) + "\n";
}

std::string toJson (const Schema& schema, const Bank& bank)
{
    auto members = header ("bank");
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
// Reading
// ---------------------------------------------------------------------------------------------
namespace
{
/** Brackets nest at most kMaxJsonDepth deep (the JSON reader recurses: a hostile file must not
    be able to exhaust the stack), and nothing but white space follows the top-level value (the
    reader itself stops after the first value and would not notice). Strings are skipped. */
bool structureOk (const char* p, size_t n, juce::String& error)
{
    int depth = 0;
    bool inString = false, rootClosed = false;
    for (size_t i = 0; i < n; ++i)
    {
        const char c = p[i];
        if (rootClosed)
        {
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            {
                error = "Unexpected text after the end of the preset.";
                return false;
            }
            continue;
        }
        if (inString)
        {
            if (c == '\\')
                ++i;
            else if (c == '"')
                inString = false;
        }
        else if (c == '"')
            inString = true;
        else if (c == '{' || c == '[')
        {
            if (++depth > kMaxJsonDepth)
            {
                error = "The file is nested too deeply to be a preset.";
                return false;
            }
        }
        else if ((c == '}' || c == ']') && depth > 0 && --depth == 0)
            rootClosed = true;
    }
    return true;
}

bool isNumber (const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble(); }

/** Checks the text and returns the root object after format and kind are verified. */
bool openRoot (const char* utf8, size_t n, const char* kind, juce::var& root, juce::String& error)
{
    if (utf8 == nullptr || n == 0)
    {
        error = "The file is empty.";
        return false;
    }
    if (n > kMaxFileBytes)
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
    if (std::memchr (utf8, 0, n) != nullptr || ! juce::CharPointer_UTF8::isValidString (utf8, static_cast<int> (n)))
    {
        error = "The file is not valid UTF-8 text.";
        return false;
    }
    if (! structureOk (utf8, n, error))
        return false;
    const auto text = juce::String::fromUTF8 (utf8, static_cast<int> (n));
    const auto r = juce::JSON::parse (text, root);
    if (r.failed())
    {
        error = "Not valid JSON (or truncated): " + r.getErrorMessage();
        return false;
    }
    auto* obj = root.getDynamicObject();
    if (obj == nullptr)
    {
        error = "The file does not contain a preset object.";
        return false;
    }
    const auto format = obj->getProperty ("format");
    if (! isNumber (format))
    {
        error = "The file has no format number.";
        return false;
    }
    if (! (std::fabs (static_cast<double> (format) - static_cast<double> (kFormat)) < 1.0e-9))
    {
        error = "Unsupported preset format " + juce::String (static_cast<double> (format)) + " (this version reads format "
                + juce::String (kFormat) + ").";
        return false;
    }
    const auto k = obj->getProperty ("kind");
    if (! k.isString() || k.toString() != kind)
    {
        error = juce::String ("This is not a ") + kind + " file.";
        return false;
    }
    return true;
}

bool readText (const juce::DynamicObject& obj, const char* key, int maxChars, juce::String& out, juce::String& error, bool isName)
{
    if (! obj.hasProperty (key))
    {
        if (isName)
            out = "Untitled";
        return true;
    }
    const auto v = obj.getProperty (key);
    if (! v.isString())
    {
        error = juce::String ("\"") + key + "\" must be text.";
        return false;
    }
    out = isName ? sanitiseName (v.toString()) : sanitiseText (v.toString(), maxChars);
    return true;
}

bool readMeta (const juce::DynamicObject& obj, Meta& meta, juce::String& error)
{
    return readText (obj, "name", kMaxNameChars, meta.name, error, true)
           && readText (obj, "category", kMaxCategoryChars, meta.category, error, false)
           && readText (obj, "author", kMaxAuthorChars, meta.author, error, false);
}

/** Reads the members of `section` into values[index(k)]; missing or unusable values keep what is
    in `values` (the default, set by the caller). */
template <typename KeyFn>
bool readSection (const Schema& schema, const juce::var& section, const char* sectionName, const std::vector<int>& indices,
                  KeyFn keyOf, std::vector<float>& values, juce::String& error)
{
    if (section.isVoid())
        return true; // missing section: all defaults
    auto* obj = section.getDynamicObject();
    if (obj == nullptr)
    {
        error = juce::String ("\"") + sectionName + "\" must be an object.";
        return false;
    }
    for (size_t k = 0; k < indices.size(); ++k)
    {
        const int i = indices[k];
        const auto v = obj->getProperty (juce::Identifier (keyOf (k, i)));
        if (isNumber (v))
            values[static_cast<size_t> (i)] = schema.sanitise (i, static_cast<double> (v)); // non-finite -> default there
        // any other kind of value (text, null, boolean, nested): the default stays
    }
    return true;
}

bool readProgramObject (const Schema& schema, const juce::var& v, Program& out, juce::String& error)
{
    auto* obj = v.getDynamicObject();
    if (obj == nullptr)
    {
        error = "A program must be an object.";
        return false;
    }
    Program p;
    p.values = schema.defaults();
    if (! readMeta (*obj, p.meta, error))
        return false;
    auto globalKey = [&] (size_t, int i) { return schema.spec (i).id; };
    if (! readSection (schema, obj->getProperty ("global"), "global", schema.globalParams(), globalKey, p.values, error))
        return false;
    for (int t = 0; t < schema.numTones(); ++t)
    {
        std::vector<int> indices;
        for (int k = 0; k < schema.toneSize(); ++k)
            indices.push_back (schema.toneParam (t, k));
        auto toneKey = [&] (size_t k, int) { return schema.toneSuffix (static_cast<int> (k)); };
        if (! readSection (schema, obj->getProperty (juce::Identifier (schema.toneKey (t))), schema.toneKey (t).toRawUTF8(), indices,
                           toneKey, p.values, error))
            return false;
    }
    out = std::move (p);
    return true;
}
} // namespace

Parsed<Program> parseProgram (const Schema& schema, const char* utf8, size_t numBytes)
{
    Parsed<Program> result;
    juce::var root;
    if (openRoot (utf8, numBytes, "program", root, result.error) && readProgramObject (schema, root, result.value, result.error))
        result.ok = true;
    else
        result.value = {};
    return result;
}

Parsed<Tone> parseTone (const Schema& schema, const char* utf8, size_t numBytes)
{
    Parsed<Tone> result;
    juce::var root;
    if (! openRoot (utf8, numBytes, "tone", root, result.error))
        return result;
    auto* obj = root.getDynamicObject();
    Tone t;
    if (! readMeta (*obj, t.meta, result.error))
        return result;
    // Values go through a scratch program so the section reader and its rules are shared.
    std::vector<float> values = schema.defaults();
    std::vector<int> indices;
    for (int k = 0; k < schema.toneSize(); ++k)
        indices.push_back (schema.toneParam (0, k));
    auto key = [&] (size_t k, int) { return schema.toneSuffix (static_cast<int> (k)); };
    if (! readSection (schema, obj->getProperty ("params"), "params", indices, key, values, result.error))
        return result;
    for (int i : indices)
        t.values.push_back (values[static_cast<size_t> (i)]);
    result.value = std::move (t);
    result.ok = true;
    return result;
}

Parsed<Bank> parseBank (const Schema& schema, const char* utf8, size_t numBytes)
{
    Parsed<Bank> result;
    juce::var root;
    if (! openRoot (utf8, numBytes, "bank", root, result.error))
        return result;
    auto* obj = root.getDynamicObject();
    Bank b = makeInitBank (schema);
    b.name = "Untitled";
    if (! readText (*obj, "name", kMaxNameChars, b.name, result.error, true)
        || ! readText (*obj, "author", kMaxAuthorChars, b.author, result.error, false))
        return result;
    const auto programs = obj->getProperty ("programs");
    if (! programs.isVoid())
    {
        auto* list = programs.getArray();
        if (list == nullptr)
        {
            result.error = "\"programs\" must be a list.";
            return result;
        }
        if (list->size() > kBankSize)
        {
            result.error = "A bank holds at most " + juce::String (kBankSize) + " programs.";
            return result;
        }
        for (int i = 0; i < list->size(); ++i)
            if (! readProgramObject (schema, list->getReference (i), b.programs[static_cast<size_t> (i)], result.error))
            {
                result.error = "Program " + juce::String (i + 1) + ": " + result.error;
                return result;
            }
    }
    result.value = std::move (b);
    result.ok = true;
    return result;
}
} // namespace vs::presets
