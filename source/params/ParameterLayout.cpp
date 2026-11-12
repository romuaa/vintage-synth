#include "params/ParameterLayout.h"

#include "params/Mappings.h"

namespace vs
{
namespace
{
enum class Display
{
    Panel10,
    CutoffHz,
    HpfHz,
    LfoRate,
    LfoDelay,
    EnvTime,
    Cents,
    PortaTime,
    DetuneCents,
    Percent,
    Pan,     // -1..1: "L 50", "C", "R 50"
    Balance  // -1..1: "Lower 50", "Centre", "Upper 50"
};

juce::String formatHz (float hz)
{
    return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " kHz" : juce::String (hz, hz < 10.0f ? 2 : 1) + " Hz";
}

juce::String formatSeconds (float s)
{
    return s >= 1.0f ? juce::String (s, 2) + " s" : juce::String (s * 1000.0f, s < 0.01f ? 1 : 0) + " ms";
}

/** -1..1 -> "<neg> 50" / centre / "<pos> 50" (percent of the way to the side). */
juce::String formatSide (float v, const char* negative, const char* centre, const char* positive)
{
    const int pct = juce::roundToInt (std::abs (v) * 100.0f);
    if (pct == 0)
        return centre;
    return juce::String (v < 0.0f ? negative : positive) + " " + juce::String (pct);
}

/** Inverse of formatSide: a leading negative/positive word (or its first letter) gives the side. */
float parseSide (const juce::String& text, const char* negative, const char* positive)
{
    const auto t = text.trim();
    const float amount = juce::jlimit (0.0f, 100.0f, std::abs (t.retainCharacters ("0123456789.").getFloatValue())) / 100.0f;
    const auto startsWithWord = [&t] (const char* word) {
        return t.startsWithIgnoreCase (word) || (t.isNotEmpty() && juce::CharacterFunctions::toLowerCase (t[0]) == juce::CharacterFunctions::toLowerCase (static_cast<juce::juce_wchar> (word[0])));
    };
    if (startsWithWord (negative))
        return -amount;
    if (startsWithWord (positive))
        return amount;
    return juce::jlimit (-1.0f, 1.0f, t.getFloatValue() / 100.0f); // plain signed percent; "C"/"Centre" = 0
}

/** Parse "1.2 kHz" / "350 Hz" -> Hz, and "350 ms" / "1.2 s" -> seconds. */
float parseScaled (const juce::String& text, const char* kiloUnit, float kiloFactor)
{
    const auto value = text.retainCharacters ("0123456789.-").getFloatValue();
    return text.containsIgnoreCase (kiloUnit) ? value * kiloFactor : value;
}

juce::String toText (Display d, float v)
{
    switch (d)
    {
        case Display::Panel10:  return juce::String (v * 10.0f, 1);
        case Display::CutoffHz: return formatHz (map::cutoffHz (v));
        case Display::HpfHz:    return formatHz (map::hpfHz (v));
        case Display::LfoRate:  return formatHz (map::lfoRateHz (v));
        case Display::LfoDelay: return formatSeconds (map::lfoDelaySec (v));
        case Display::EnvTime:  return formatSeconds (map::envTimeSec (v));
        case Display::Cents:    return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (map::fineTuneSemis (v) * 100.0f)) + " ct";
        case Display::PortaTime: return v <= 0.0f ? juce::String ("Off") : formatSeconds (map::portaTimeSec (v));
        case Display::DetuneCents: return juce::String (map::unisonDetuneCents (v), 1) + " ct";
        case Display::Percent:  return juce::String (juce::roundToInt (v * 100.0f)) + " %";
        case Display::Pan:      return formatSide (v, "L", "C", "R");
        case Display::Balance:  return formatSide (v, "Lower", "Centre", "Upper");
    }
    return {};
}

float fromText (Display d, const juce::String& t)
{
    switch (d)
    {
        case Display::Panel10:  return t.getFloatValue() / 10.0f;
        case Display::CutoffHz: return map::cutoffFromHz (juce::jmax (1.0f, parseScaled (t, "k", 1000.0f)));
        case Display::HpfHz:    return map::hpfFromHz (juce::jmax (1.0f, parseScaled (t, "k", 1000.0f)));
        case Display::LfoRate:  return map::lfoRateFromHz (juce::jmax (0.001f, parseScaled (t, "k", 1000.0f)));
        case Display::LfoDelay: return map::lfoDelayFromSec (juce::jmax (0.0f, t.containsIgnoreCase ("ms") ? t.getFloatValue() / 1000.0f : t.getFloatValue()));
        case Display::EnvTime:  return map::envTimeFromSec (juce::jmax (0.0001f, t.containsIgnoreCase ("ms") ? t.getFloatValue() / 1000.0f : t.getFloatValue()));
        case Display::Cents:    return t.getFloatValue() / 100.0f;
        case Display::PortaTime:
            if (t.trim().equalsIgnoreCase ("off"))
                return 0.0f;
            return map::portaFromSec (t.containsIgnoreCase ("ms") ? t.getFloatValue() / 1000.0f : t.getFloatValue());
        case Display::DetuneCents: return t.getFloatValue() / map::unisonDetuneCents (1.0f);
        case Display::Percent:  return t.getFloatValue() / 100.0f;
        case Display::Pan:      return parseSide (t, "L", "R");
        case Display::Balance:  return parseSide (t, "Lower", "Upper");
    }
    return 0.0f;
}

std::unique_ptr<juce::AudioParameterFloat> makeFloat (const juce::String& id, const juce::String& name,
                                                      float min, float max, float def, Display d)
{
    auto attrs = juce::AudioParameterFloatAttributes()
                     .withStringFromValueFunction ([d] (float v, int) { return toText (d, v); })
                     .withValueFromStringFunction ([d, min, max] (const juce::String& t) {
                         return juce::jlimit (min, max, fromText (d, t));
                     });

    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, kParamVersionHint }, name,
                                                        juce::NormalisableRange<float> { min, max }, def,
                                                        std::move (attrs));
}

template <size_t N>
std::unique_ptr<juce::AudioParameterChoice> makeChoice (const juce::String& id, const juce::String& name,
                                                        const std::array<const char*, N>& labels, int def)
{
    juce::StringArray choices;
    for (const auto* l : labels)
        choices.add (l);

    return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, kParamVersionHint }, name,
                                                         choices, def);
}

void addPart (juce::AudioProcessorValueTreeState::ParameterLayout& layout, const juce::String& prefix,
              const juce::String& namePrefix)
{
    // Order is part of the host-facing contract: never reorder, only append.
#define VS_ADD_FLOAT(member, id, name, mn, mx, def, kind) \
    layout.add (makeFloat (prefix + id, namePrefix + juce::String::fromUTF8 (name), mn, mx, def, Display::kind));
#define VS_ADD_CHOICE(member, id, name, labels, def) \
    layout.add (makeChoice (prefix + id, namePrefix + juce::String::fromUTF8 (name), labels, def));
    VS_PART_FLOAT_PARAMS (VS_ADD_FLOAT)
    VS_PART_CHOICE_PARAMS (VS_ADD_CHOICE)
#undef VS_ADD_FLOAT
#undef VS_ADD_CHOICE
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamID::masterVolume, kParamVersionHint },
        "Master Volume",
        juce::NormalisableRange<float> { -60.0f, 6.0f, 0.1f },
        -6.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    addPart (layout, kLowerPrefix, "Lower ");
    addPart (layout, kUpperPrefix, "Upper ");

    layout.add (makeFloat (ParamID::vintage, "Vintage", 0.0f, 1.0f, 0.0f, Display::Panel10));

    // M3: key modes and the part mixer (appended, docs/architecture.md 4.5).
    layout.add (makeChoice (ParamID::keyMode, "Key Mode", kKeyModeLabels, 0));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { ParamID::keySplitPoint, kParamVersionHint }, "Split Point", 0, 127, kDefaultSplitPoint,
        juce::AudioParameterIntAttributes()
            .withStringFromValueFunction ([] (int v, int) { return noteName (v); })
            .withValueFromStringFunction ([] (const juce::String& t) {
                const int n = noteFromName (t);
                return n < 0 ? kDefaultSplitPoint : n;
            })));
    layout.add (makeFloat (ParamID::mixBalance, "Balance", -1.0f, 1.0f, 0.0f, Display::Balance));

    return layout;
}

juce::String noteName (int note)
{
    static constexpr const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int n = juce::jlimit (0, 127, note);
    return juce::String (names[n % 12]) + juce::String (n / 12 - 1);
}

int noteFromName (const juce::String& text)
{
    const auto t = text.trim();
    if (t.isEmpty())
        return -1;
    if (juce::CharacterFunctions::isDigit (t[0]))
        return juce::jlimit (0, 127, t.getIntValue()); // a plain MIDI note number

    static constexpr int semis[] { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    const auto letter = juce::CharacterFunctions::toUpperCase (t[0]);
    if (letter < 'A' || letter > 'G')
        return -1;
    int pc = semis[static_cast<size_t> (letter - 'A')];
    int i = 1;
    for (; i < t.length() && (t[i] == '#' || t[i] == 'b'); ++i)
        pc += t[i] == '#' ? 1 : -1;
    const auto octaveText = t.substring (i).trim();
    if (octaveText.isEmpty() || ! octaveText.containsOnly ("-0123456789"))
        return -1;
    return juce::jlimit (0, 127, (octaveText.getIntValue() + 1) * 12 + pc);
}

void PartParamBinding::bind (juce::AudioProcessorValueTreeState& state, const juce::String& prefix)
{
#define VS_BIND(member, id, ...)                                  \
    member = state.getRawParameterValue (prefix + id);            \
    jassert (member != nullptr);
    VS_PART_FLOAT_PARAMS (VS_BIND)
    VS_PART_CHOICE_PARAMS (VS_BIND)
#undef VS_BIND
}

void PartParamBinding::read (PartParams& out) const noexcept
{
#define VS_READ_FLOAT(member, ...) out.member = member->load (std::memory_order_relaxed);
#define VS_READ_CHOICE(member, ...) out.member = static_cast<int> (member->load (std::memory_order_relaxed));
    VS_PART_FLOAT_PARAMS (VS_READ_FLOAT)
    VS_PART_CHOICE_PARAMS (VS_READ_CHOICE)
#undef VS_READ_FLOAT
#undef VS_READ_CHOICE
}
} // namespace vs
