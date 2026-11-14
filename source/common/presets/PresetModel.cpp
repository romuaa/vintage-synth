#include "common/presets/PresetModel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace vs::presets
{
bool Instrument::isValid() const noexcept
{
    const bool tablesOk = ! hasTables() || (maxTableLines > 0 && maxTableBytes > 0);
    return id.isNotEmpty() && groups >= 1 && groups <= 26 && slotsPerGroup >= 1 && slotsPerGroup <= 9 && programExtension.startsWithChar ('.')
           && toneExtension.startsWithChar ('.') && bankExtension.startsWithChar ('.') && tablesOk;
}

juce::String Instrument::positionName (int i) const
{
    const int per = std::max (1, slotsPerGroup);
    const int c = std::clamp (i, 0, std::max (0, bankSize() - 1));
    return juce::String::charToString (static_cast<juce::juce_wchar> ('A' + c / per)) + juce::String (c % per + 1);
}

int Instrument::slotFromFileName (const juce::String& fileName) const
{
    if (fileName.length() < 2)
        return -1;
    const auto group = static_cast<int> (juce::CharacterFunctions::toUpperCase (fileName[0])) - 'A';
    const auto slot = static_cast<int> (fileName[1]) - '1';
    if (group < 0 || group >= groups || slot < 0 || slot >= slotsPerGroup)
        return -1;
    return group * slotsPerGroup + slot;
}

Schema::Schema (std::vector<ParamSpec> allParams, std::vector<juce::String> prefixes, std::vector<juce::String> excludedIds, Instrument instrument)
    : specs (std::move (allParams)), tonePrefixes (std::move (prefixes)), inst (std::move (instrument))
{
    excluded.assign (specs.size(), false);
    for (const auto& id : excludedIds)
    {
        const int i = indexOf (id);
        if (i >= 0)
            excluded[static_cast<size_t> (i)] = true;
    }

    for (const auto& p : tonePrefixes)
        toneKeys.push_back (p.endsWithChar ('.') ? p.dropLastCharacters (1) : p);

    // The tone's parameters, in the order of the first part's; every other part must match.
    toneIndex.assign (tonePrefixes.size(), {});
    valid = ! tonePrefixes.empty() && inst.isValid();
    std::vector<bool> inTone (specs.size(), false);
    if (valid)
    {
        for (int i = 0; i < numParams(); ++i)
            if (specs[static_cast<size_t> (i)].id.startsWith (tonePrefixes[0]))
                toneSuffixes.push_back (specs[static_cast<size_t> (i)].id.substring (tonePrefixes[0].length()));
        for (size_t t = 0; t < tonePrefixes.size(); ++t)
            for (const auto& suffix : toneSuffixes)
            {
                const int i = indexOf (tonePrefixes[t] + suffix);
                if (i < 0)
                    valid = false;
                else
                {
                    toneIndex[t].push_back (i);
                    inTone[static_cast<size_t> (i)] = true;
                }
            }
        // A tone file has no part: it is read and clamped with the first part's specs, so every
        // part must have the same range, default and step for the same suffix.
        for (size_t t = 1; t < tonePrefixes.size() && valid; ++t)
            for (size_t k = 0; k < toneSuffixes.size(); ++k)
            {
                const auto& a = specs[static_cast<size_t> (toneIndex[0][k])];
                const auto& b = specs[static_cast<size_t> (toneIndex[t][k])];
                const auto same = [] (float x, float y) { return std::memcmp (&x, &y, sizeof (float)) == 0; };
                if (! same (a.lo, b.lo) || ! same (a.hi, b.hi) || ! same (a.def, b.def) || a.discrete != b.discrete)
                    valid = false;
            }
        for (int i = 0; i < numParams(); ++i) // a prefixed parameter outside the common set
            for (const auto& p : tonePrefixes)
                if (specs[static_cast<size_t> (i)].id.startsWith (p) && ! inTone[static_cast<size_t> (i)])
                    valid = false;
    }
    for (int i = 0; i < numParams(); ++i)
        if (! inTone[static_cast<size_t> (i)] && ! excluded[static_cast<size_t> (i)])
            globals.push_back (i);
}

Schema Schema::fromProcessor (const juce::AudioProcessor& processor, std::vector<juce::String> tonePrefixes,
                              std::vector<juce::String> excludedIds, Instrument instrument)
{
    // The defaults are the parameters' values as they are now: build the schema before anything
    // changes them (a processor that has just been constructed).
    std::vector<ParamSpec> all;
    for (auto* p : processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (ranged == nullptr)
            continue;
        ParamSpec s;
        s.id = ranged->paramID;
        const auto& range = ranged->getNormalisableRange();
        s.lo = range.start;
        s.hi = range.end;
        s.discrete = ranged->isDiscrete();
        if (auto* f = dynamic_cast<juce::AudioParameterFloat*> (ranged))
            s.def = f->get();
        else if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (ranged))
            s.def = static_cast<float> (c->getIndex());
        else if (auto* n = dynamic_cast<juce::AudioParameterInt*> (ranged))
            s.def = static_cast<float> (n->get());
        else
            s.def = range.convertFrom0to1 (ranged->getDefaultValue());
        all.push_back (std::move (s));
    }
    return Schema (std::move (all), std::move (tonePrefixes), std::move (excludedIds), std::move (instrument));
}

int Schema::indexOf (const juce::String& id) const
{
    for (size_t i = 0; i < specs.size(); ++i)
        if (specs[i].id == id)
            return static_cast<int> (i);
    return -1;
}

float Schema::sanitise (int i, double raw) const noexcept
{
    const auto& s = specs[static_cast<size_t> (i)];
    if (! std::isfinite (raw))
        return s.def;
    const double lo = static_cast<double> (s.lo), hi = static_cast<double> (s.hi);
    double v = std::min (std::max (raw, lo), hi);
    if (s.discrete)
        v = std::min (std::max (lo + std::round (v - lo), lo), hi);
    return static_cast<float> (v);
}

std::vector<float> Schema::defaults() const
{
    std::vector<float> v;
    v.reserve (specs.size());
    for (const auto& s : specs)
        v.push_back (s.def);
    return v;
}

bool isControlCharacter (juce::juce_wchar c) noexcept
{
    const auto u = static_cast<std::uint32_t> (c);
    return u < 0x20u || (u >= 0x7Fu && u <= 0x9Fu) || u == 0x2028u || u == 0x2029u;
}

bool isFormatCharacter (juce::juce_wchar c) noexcept
{
    const auto u = static_cast<std::uint32_t> (c);
    return u == 0x00ADu                       // soft hyphen
           || u == 0x034Fu                    // combining grapheme joiner
           || u == 0x061Cu                    // Arabic letter mark
           || u == 0x115Fu || u == 0x1160u    // Hangul fillers
           || u == 0x17B4u || u == 0x17B5u    // Khmer inherent vowels (invisible)
           || u == 0x180Eu                    // Mongolian vowel separator
           || (u >= 0x200Bu && u <= 0x200Fu)  // zero-width space, joiners, LRM, RLM
           || (u >= 0x202Au && u <= 0x202Eu)  // LRE, RLE, PDF, LRO, RLO
           || (u >= 0x2060u && u <= 0x206Fu)  // word joiner, invisible operators, LRI, RLI, FSI, PDI, deprecated
           || u == 0x3164u                    // Hangul filler
           || u == 0xFEFFu                    // byte order mark / zero-width no-break space
           || u == 0xFFA0u                    // half-width Hangul filler
           || (u >= 0xFFF9u && u <= 0xFFFBu)  // interlinear annotation
           || (u >= 0x1D173u && u <= 0x1D17Au) // musical symbol format characters
           || (u >= 0xE0000u && u <= 0xE007Fu); // tag characters
}

juce::String sanitiseText (const juce::String& s, int maxChars)
{
    juce::String cleaned;
    for (auto c : s)
    {
        if (isFormatCharacter (c))
            continue;
        cleaned += isControlCharacter (c) ? juce::juce_wchar (' ') : c;
    }
    return cleaned.trim().substring (0, maxChars).trim();
}

juce::String sanitiseName (const juce::String& s)
{
    const auto n = sanitiseText (s, kMaxNameChars);
    return n.isEmpty() ? juce::String ("Untitled") : n;
}

// ---------------------------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------------------------
std::string joinTableLines (const std::vector<std::string>& lines)
{
    size_t n = 0;
    for (const auto& l : lines)
        n += l.size() + 1;
    std::string out;
    out.reserve (n);
    for (const auto& l : lines)
    {
        out += l;
        out += '\n';
    }
    return out;
}

juce::String checkTableShape (const Instrument& instrument, const std::vector<std::string>& lines)
{
    if (lines.size() > static_cast<size_t> (std::max (0, instrument.maxTableLines)))
        return "The table has more than " + juce::String (instrument.maxTableLines) + " lines.";
    size_t bytes = 0;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        bytes += lines[i].size() + 1u;
        if (bytes > instrument.maxTableBytes)
            return "The table is longer than " + juce::String (static_cast<juce::int64> (instrument.maxTableBytes)) + " bytes.";
        if (lines[i].find_first_of (std::string ("\n\r\0", 3)) != std::string::npos)
            return "Line " + juce::String (static_cast<int> (i) + 1) + " of the table contains a line break or a NUL character.";
    }
    return {};
}

juce::String validateTableLines (const Instrument& instrument, const std::vector<std::string>& lines)
{
    if (lines.empty())
        return {};
    if (! instrument.hasTables())
        return "This instrument has no tables.";
    if (auto e = checkTableShape (instrument, lines); e.isNotEmpty())
        return e;
    try
    {
        return instrument.validateTable (lines).substring (0, 1024); // a message is shown and stored: bounded
    }
    catch (...)
    {
        return "The table could not be checked.";
    }
}

juce::String splitTableText (const Instrument& instrument, const juce::String& text, std::vector<std::string>& lines)
{
    lines.clear();
    const char* utf8 = text.toRawUTF8();
    const size_t n = text.getNumBytesAsUTF8();
    if (n > instrument.maxTableBytes)
        return "The table is longer than " + juce::String (static_cast<juce::int64> (instrument.maxTableBytes)) + " bytes.";
    size_t start = 0;
    for (size_t i = 0; i < n; ++i)
        if (utf8[i] == '\n')
        {
            if (lines.size() >= static_cast<size_t> (std::max (0, instrument.maxTableLines)))
            {
                lines.clear();
                return "The table has more than " + juce::String (instrument.maxTableLines) + " lines.";
            }
            lines.emplace_back (utf8 + start, i - start);
            start = i + 1;
        }
    if (start < n)
    {
        if (lines.size() >= static_cast<size_t> (std::max (0, instrument.maxTableLines)))
        {
            lines.clear();
            return "The table has more than " + juce::String (instrument.maxTableLines) + " lines.";
        }
        lines.emplace_back (utf8 + start, n - start);
    }
    return {};
}

Program makeInitProgram (const Schema& schema)
{
    Program p;
    p.meta.name = "Init";
    p.values = schema.defaults();
    return p;
}

Bank makeInitBank (const Schema& schema)
{
    Bank b;
    b.name = "Untitled";
    b.programs.assign (static_cast<size_t> (schema.bankSize()), makeInitProgram (schema));
    return b;
}

Tone toneOf (const Schema& schema, const Program& program, int tone)
{
    Tone t;
    t.meta = program.meta;
    if (static_cast<size_t> (tone) < program.data.size())
        t.data = program.data[static_cast<size_t> (tone)];
    t.values.reserve (static_cast<size_t> (schema.toneSize()));
    for (int k = 0; k < schema.toneSize(); ++k)
        t.values.push_back (program.values[static_cast<size_t> (schema.toneParam (tone, k))]);
    return t;
}

void setTone (const Schema& schema, Program& program, int tone, const Tone& t)
{
    if (schema.instrument().hasTables())
    {
        if (program.data.size() < static_cast<size_t> (schema.numTones()))
            program.data.resize (static_cast<size_t> (schema.numTones()));
        program.data[static_cast<size_t> (tone)] = t.data;
    }
    for (int k = 0; k < schema.toneSize(); ++k)
        program.values[static_cast<size_t> (schema.toneParam (tone, k))] = t.values[static_cast<size_t> (k)];
}
} // namespace vs::presets
