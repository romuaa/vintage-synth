#include "bside/params/ParameterLayout.h"

#include <cmath>

#include "common/params/FxArpParams.h"
#include "common/params/GlobalParams.h"
#include "common/params/ParamFactory.h"

namespace bs
{
std::optional<double> parseFirstNumber (const juce::String& text)
{
    const auto n = text.length();
    const auto isDigit = [&text, n] (int i) { return i < n && text[i] >= '0' && text[i] <= '9'; };
    for (int start = 0; start < n; ++start)
    {
        int i = start;
        if (text[i] == '+' || text[i] == '-')
            ++i;
        const int intStart = i;
        while (isDigit (i))
            ++i;
        const bool hasInt = i > intStart;
        bool hasFraction = false;
        if (i < n && text[i] == '.' && isDigit (i + 1))
        {
            ++i;
            while (isDigit (i))
                ++i;
            hasFraction = true;
        }
        if (! hasInt && ! hasFraction)
            continue; // not a number here: try the next position ("--5" gives -5, "abc" gives none)
        if (i < n && (text[i] == 'e' || text[i] == 'E'))
        {
            int j = i + 1;
            if (j < n && (text[j] == '+' || text[j] == '-'))
                ++j;
            if (isDigit (j))
            {
                while (isDigit (j))
                    ++j;
                i = j;
            }
        }
        // getDoubleValue reads exactly the well-formed prefix that was found (it never sees "nan" or "inf":
        // the scan above only accepts digits), and gives infinity for an exponent past the double range.
        return text.substring (start, i).getDoubleValue();
    }
    return std::nullopt;
}

namespace
{
juce::String signedText (float v, int decimals, const char* unit)
{
    // juce::String (float, 0) is "default formatting", not "no decimals": whole numbers are made from an int.
    const auto n = decimals == 0 ? juce::String (juce::roundToInt (v)) : juce::String (v, decimals);
    if (! n.containsAnyOf ("123456789")) // "-0.0" and "0.0" are both zero: no sign
        return (decimals == 0 ? juce::String ("0") : juce::String ("0.") + juce::String::repeatedString ("0", decimals)) + unit;
    return (n.startsWithChar ('-') ? n : "+" + n) + unit;
}

/** `v` limited to lo ... hi; a non-finite value (NaN, infinity, from text) gives `def`. */
float finiteOr (double v, float lo, float hi, float def)
{
    if (! std::isfinite (v))
        return def;
    const double c = v < static_cast<double> (lo) ? static_cast<double> (lo) : (v > static_cast<double> (hi) ? static_cast<double> (hi) : v);
    return static_cast<float> (c);
}

juce::String floatText (Disp d, float v)
{
    switch (d)
    {
        case Disp::Cents:         return signedText (v, 0, " ct");
        case Disp::Percent:       return juce::String (v, 1) + " %";
        case Disp::PercentSigned: return signedText (v, 1, " %");
        case Disp::Register:      return juce::String (juce::roundToInt (v));
        case Disp::Hz:            return juce::String (v, 2) + " Hz";
        case Disp::TickHz:        return juce::String (v, 1) + " Hz";
        case Disp::WheelCents:    return juce::String (juce::roundToInt (map::wheelVibratoCents (v))) + " ct";
        case Disp::UnisonCents:   return juce::String (map::unisonDetuneCents (v), 1) + " ct";
        case Disp::Unit:          return vs::toText (vs::Display::Percent, v);
        case Disp::Porta:         return vs::toText (vs::Display::PortaTime, v);
        case Disp::Pan:           return vs::toText (vs::Display::Pan, v);
        case Disp::Db:            return signedText (v, 1, " dB");
    }
    return {};
}

/** Typed text -> the plain value, not yet limited to the range. `def` is the parameter's default, the answer when the
    text holds no number. The shared parsers behind Unit, Porta and Pan accept "nan" and "inf": the caller limits (finiteOr). */
float floatFromText (Disp d, const juce::String& t, float def)
{
    const auto number = parseFirstNumber (t);
    // "1e999", "nan", "inf": no usable value, whatever the shared parsers below would make of the text (the pan parser
    // reads "inf" as full right, the others as infinity or NaN).
    if ((number && ! std::isfinite (*number)) || t.containsIgnoreCase ("nan") || t.containsIgnoreCase ("inf"))
        return def;
    switch (d)
    {
        case Disp::Cents:
        case Disp::Percent:
        case Disp::PercentSigned:
        case Disp::Register:
        case Disp::Hz:
        case Disp::TickHz:
        case Disp::Db:            return number ? static_cast<float> (*number) : def;
        case Disp::WheelCents:    return number ? map::wheelVibratoFromCents (static_cast<float> (*number)) : def;
        case Disp::UnisonCents:   return number ? map::unisonDetuneFromCents (static_cast<float> (*number)) : def;
        case Disp::Unit:          return number ? vs::fromText (vs::Display::Percent, t) : def;
        case Disp::Porta:         return number || t.trim().equalsIgnoreCase ("off") ? vs::fromText (vs::Display::PortaTime, t) : def;
        case Disp::Pan:           return number || t.trim().isNotEmpty() ? vs::fromText (vs::Display::Pan, t) : def;
    }
    return def;
}

/** "250 ms", "1 s", "1.5 s": whole milliseconds below one second, otherwise seconds without a trailing ".0". */
juce::String envTimeText (int ms)
{
    if (ms < 1000)
        return juce::String (ms) + " ms";
    auto s = juce::String (static_cast<double> (ms) / 1000.0, 1);
    if (s.endsWith (".0"))
        s = s.dropLastCharacters (2);
    return s + " s";
}

map::EnvStage stageOf (IntDisp d) { return d == IntDisp::Attack ? map::EnvStage::Attack : d == IntDisp::Decay ? map::EnvStage::Decay : map::EnvStage::Release; }

juce::String intText (IntDisp d, int v)
{
    switch (d)
    {
        case IntDisp::Plain:      return juce::String (v);
        case IntDisp::Semitones:  return (v > 0 ? "+" : "") + juce::String (v) + " st";
        case IntDisp::Attack:
        case IntDisp::Decay:
        case IntDisp::Release:    return envTimeText (map::envNominalMs (stageOf (d), v));
        case IntDisp::Multiplier: return "x" + juce::String (v);
    }
    return {};
}

/** Typed text -> an integer value in mn ... mx; text without a number gives `def`. */
int intFromText (IntDisp d, const juce::String& t, int mn, int mx, int def)
{
    const auto number = parseFirstNumber (t);
    if (! number || ! std::isfinite (*number))
        return def;
    switch (d)
    {
        case IntDisp::Plain:
        case IntDisp::Semitones:
        case IntDisp::Multiplier:
            return juce::roundToInt (finiteOr (*number, static_cast<float> (mn), static_cast<float> (mx), static_cast<float> (def)));
        case IntDisp::Attack:
        case IntDisp::Decay:
        case IntDisp::Release:
        {
            // "250 ms", "1.5 s"; a bare number counts as milliseconds.
            const bool ms = t.containsIgnoreCase ("ms");
            const bool sec = ! ms && t.containsIgnoreCase ("s");
            return map::envSettingFromMs (stageOf (d), sec ? *number * 1000.0 : *number);
        }
    }
    return def;
}

std::unique_ptr<juce::AudioParameterFloat> makeFloat (const juce::String& id, const juce::String& name, float mn, float mx,
                                                      float def, Disp d)
{
    auto attrs = juce::AudioParameterFloatAttributes()
                     .withStringFromValueFunction ([d] (float v, int) { return floatText (d, v); })
                     .withValueFromStringFunction ([d, mn, mx, def] (const juce::String& t) {
                         return finiteOr (static_cast<double> (floatFromText (d, t, def)), mn, mx, def);
                     });

    juce::NormalisableRange<float> range { mn, mx };
    if (d == Disp::TickHz) // exponential: equal steps of the slider are equal musical ratios; one formula, in PartMappings.h
        range = juce::NormalisableRange<float> (
            mn, mx, [] (float, float, float n) { return map::tickFreeHz (n); }, [] (float, float, float hz) { return map::tickFreeNorm (hz); });

    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, vs::kParamVersionHint }, name, range, def, std::move (attrs));
}

std::unique_ptr<vs::SteppedIntParameter> makeInt (const juce::String& id, const juce::String& name, int mn, int mx, int def, IntDisp d)
{
    auto attrs = juce::AudioParameterIntAttributes()
                     .withStringFromValueFunction ([d] (int v, int) { return intText (d, v); })
                     .withValueFromStringFunction ([d, mn, mx, def] (const juce::String& t) { return juce::jlimit (mn, mx, intFromText (d, t, mn, mx, def)); });
    return std::make_unique<vs::SteppedIntParameter> (juce::ParameterID { id, vs::kParamVersionHint }, name, mn, mx, def, attrs);
}

/** A choice that also understands typed text the way a person types it: a label in any case, or a bare number that is
    the leading number of exactly one label ("24" for "24 per beat", "5" for "5 st"). Anything else is JUCE's behaviour. */
class ChoiceParameter final : public juce::AudioParameterChoice
{
public:
    using juce::AudioParameterChoice::AudioParameterChoice;

    float getValueForText (const juce::String& text) const override
    {
        const auto t = text.trim();
        for (int i = 0; i < choices.size(); ++i)
            if (choices[i].equalsIgnoreCase (t))
                return convertTo0to1 (static_cast<float> (i));

        if (t.isNotEmpty() && t.containsOnly ("0123456789"))
        {
            const auto typed = digitsWithoutLeadingZeros (t);
            int found = -1, count = 0;
            for (int i = 0; i < choices.size(); ++i)
            {
                const auto lead = choices[i].initialSectionContainingOnly ("0123456789");
                if (lead.isNotEmpty() && digitsWithoutLeadingZeros (lead) == typed)
                {
                    found = i;
                    ++count;
                }
            }
            if (count == 1)
                return convertTo0to1 (static_cast<float> (found));
        }

        // JUCE's own rule (its override is private, so it is repeated here): an unknown text is index -1, which
        // convertTo0to1 limits to the first label, so the result is always finite and inside 0 ... 1.
        return convertTo0to1 (static_cast<float> (choices.indexOf (text)));
    }

private:
    static juce::String digitsWithoutLeadingZeros (const juce::String& s)
    {
        const auto r = s.trimCharactersAtStart ("0");
        return r.isEmpty() ? juce::String ("0") : r;
    }
};

template <size_t N>
std::unique_ptr<ChoiceParameter> makeChoice (const juce::String& id, const juce::String& name, const std::array<const char*, N>& labels, int def)
{
    juce::StringArray choices;
    for (const auto* l : labels)
        choices.add (l);
    return std::make_unique<ChoiceParameter> (juce::ParameterID { id, vs::kParamVersionHint }, name, choices, def);
}

void addPart (juce::AudioProcessorValueTreeState::ParameterLayout& layout, const juce::String& prefix, const juce::String& namePrefix)
{
    // The order is part of the host-facing contract: never reorder, only append.
#define BS_ADD_FLOAT(member, id, name, mn, mx, def, kind) \
    layout.add (makeFloat (prefix + id, namePrefix + juce::String::fromUTF8 (name), mn, mx, def, Disp::kind));
#define BS_ADD_INT(member, id, name, mn, mx, def, kind) \
    layout.add (makeInt (prefix + id, namePrefix + juce::String::fromUTF8 (name), mn, mx, def, IntDisp::kind));
#define BS_ADD_CHOICE(member, id, name, labels, def) \
    layout.add (makeChoice (prefix + id, namePrefix + juce::String::fromUTF8 (name), labels, def));
    BS_PART_PARAMS (BS_ADD_FLOAT, BS_ADD_CHOICE, BS_ADD_INT)
#undef BS_ADD_FLOAT
#undef BS_ADD_INT
#undef BS_ADD_CHOICE
}

std::atomic<float>* rawValue (juce::AudioProcessorValueTreeState& state, const juce::String& id)
{
    auto* p = state.getRawParameterValue (id);
    if (p == nullptr)
        throw vs::ConfigError ("the parameter layout has no parameter '" + id.toStdString() + "'");
    return p;
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    vs::addMasterVolume (layout);
    vs::addKeyMixParameters (layout);
    vs::addFxParameters (layout);
    vs::addArpSeqParameters (layout);
    vs::addLimiterParameters (layout);
    // S2 (8.7): the per-part parameters, lower then upper, then the one chip-wide parameter.
    addPart (layout, kLowerPrefix, "Lower ");
    addPart (layout, kUpperPrefix, "Upper ");
    layout.add (makeChoice (kChipClockId, "Chip Clock", kChipClockLabels, 0));
    return layout;
}

void PartParamBinding::bind (juce::AudioProcessorValueTreeState& state, const juce::String& prefix)
{
#define BS_BIND(member, id, ...) member = rawValue (state, prefix + id);
    BS_PART_PARAMS (BS_BIND, BS_BIND, BS_BIND)
#undef BS_BIND
}

void PartParamBinding::read (PartParams& out) const noexcept
{
#define BS_READ_FLOAT(member, id, name, mn, mx, def, kind) \
    out.member = vs::map::sanitiseParam (member->load (std::memory_order_relaxed), mn, mx, def);
#define BS_READ_INT(member, id, name, mn, mx, def, kind) \
    out.member = vs::map::sanitiseIndex (member->load (std::memory_order_relaxed), mn, mx, def);
#define BS_READ_CHOICE(member, id, name, labels, def) \
    out.member = vs::map::sanitiseIndex (member->load (std::memory_order_relaxed), 0, static_cast<int> (labels.size()) - 1, def);
    BS_PART_PARAMS (BS_READ_FLOAT, BS_READ_CHOICE, BS_READ_INT)
#undef BS_READ_FLOAT
#undef BS_READ_INT
#undef BS_READ_CHOICE
}

void ChipClockBinding::bind (juce::AudioProcessorValueTreeState& state) { value = rawValue (state, kChipClockId); }

int ChipClockBinding::read() const noexcept
{
    return vs::map::sanitiseIndex (value->load (std::memory_order_relaxed), 0, static_cast<int> (kChipClockLabels.size()) - 1, 0);
}
} // namespace bs
