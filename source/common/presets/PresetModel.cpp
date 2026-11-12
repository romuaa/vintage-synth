#include "common/presets/PresetModel.h"

#include <algorithm>
#include <cmath>

namespace vs::presets
{
Schema::Schema (std::vector<ParamSpec> allParams, std::vector<juce::String> prefixes, std::vector<juce::String> excludedIds)
    : specs (std::move (allParams)), tonePrefixes (std::move (prefixes))
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
    valid = ! tonePrefixes.empty();
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
                              std::vector<juce::String> excludedIds)
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
    return Schema (std::move (all), std::move (tonePrefixes), std::move (excludedIds));
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

juce::String sanitiseText (const juce::String& s, int maxChars)
{
    juce::String cleaned;
    for (auto c : s)
        cleaned += (c < 32 || c == 127) ? juce::juce_wchar (' ') : c;
    return cleaned.trim().substring (0, maxChars).trim();
}

juce::String sanitiseName (const juce::String& s)
{
    const auto n = sanitiseText (s, kMaxNameChars);
    return n.isEmpty() ? juce::String ("Untitled") : n;
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
    b.programs.assign (static_cast<size_t> (kBankSize), makeInitProgram (schema));
    return b;
}

Tone toneOf (const Schema& schema, const Program& program, int tone)
{
    Tone t;
    t.meta = program.meta;
    t.values.reserve (static_cast<size_t> (schema.toneSize()));
    for (int k = 0; k < schema.toneSize(); ++k)
        t.values.push_back (program.values[static_cast<size_t> (schema.toneParam (tone, k))]);
    return t;
}

void setTone (const Schema& schema, Program& program, int tone, const Tone& t)
{
    for (int k = 0; k < schema.toneSize(); ++k)
        program.values[static_cast<size_t> (schema.toneParam (tone, k))] = t.values[static_cast<size_t> (k)];
}
} // namespace vs::presets
