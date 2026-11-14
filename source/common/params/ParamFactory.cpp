#include "common/params/ParamFactory.h"

#include "common/params/Mappings.h"

namespace vs
{
namespace
{
/** "350 ms" / "1.2 s" -> seconds; a bare number counts as seconds (as the envelope times). */
float parseSeconds (const juce::String& t)
{
    return t.containsIgnoreCase ("ms") ? t.getFloatValue() / 1000.0f : t.getFloatValue();
}

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

} // namespace

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
        case Display::DelayTime: return formatSeconds (map::delayTimeSec (v));
        case Display::Feedback: return juce::String (map::delayFeedback (v) * 100.0f, 1) + " %";
        case Display::ReverbDecay: return formatSeconds (map::reverbDecaySec (v));
        case Display::PreDelay: return juce::String (map::reverbPreDelaySec (v) * 1000.0f, 1) + " ms";
        case Display::PercentValue: return juce::String (v, 1) + " %";
        case Display::Bpm:      return juce::String (v, 1) + " bpm";
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
        case Display::DelayTime: return map::delayTimeFromSec (parseSeconds (t));
        case Display::Feedback: return map::delayFeedbackFromGain (t.getFloatValue() / 100.0f);
        case Display::ReverbDecay: return map::reverbDecayFromSec (parseSeconds (t));
        case Display::PreDelay:
            // "20 ms" or a bare number: milliseconds; "0.02 s": seconds.
            return map::reverbPreDelayFromSec (t.containsIgnoreCase ("ms") || ! t.containsIgnoreCase ("s")
                                                   ? t.getFloatValue() / 1000.0f
                                                   : t.getFloatValue());
        case Display::PercentValue: return t.getFloatValue();
        case Display::Bpm:      return t.getFloatValue();
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

void BoundParam::bind (juce::AudioProcessorValueTreeState& state, const juce::String& id)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter (id));
    value = state.getRawParameterValue (id);
    // Every build type, not only debug: a missing parameter would otherwise be a null read much later.
    if (p == nullptr || value == nullptr)
        throw ConfigError ("the parameter layout has no parameter '" + id.toStdString() + "'");
    lo = p->getNormalisableRange().start;
    hi = p->getNormalisableRange().end;
    def = p->convertFrom0to1 (p->getDefaultValue());
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
    const auto isInteger = [] (const juce::String& x) {
        // An optional minus sign, then at least one digit, nothing else.
        const int start = x.startsWithChar ('-') ? 1 : 0;
        return x.length() > start && x.substring (start).containsOnly ("0123456789");
    };
    int note = -1;
    if (isInteger (t))
    {
        note = t.getIntValue(); // a plain MIDI note number
    }
    else
    {
        static constexpr int semis[] { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
        const auto letter = static_cast<int> (juce::CharacterFunctions::toUpperCase (t[0]));
        if (letter < 'A' || letter > 'G')
            return -1;
        int pc = semis[static_cast<size_t> (letter - 'A')];
        int i = 1;
        for (; i < t.length() && (static_cast<int> (t[i]) == '#' || static_cast<int> (t[i]) == 'b'); ++i)
            pc += static_cast<int> (t[i]) == '#' ? 1 : -1;
        const auto octaveText = t.substring (i).trim();
        if (! isInteger (octaveText)) // "C-" or "C#x": not a note
            return -1;
        note = (octaveText.getIntValue() + 1) * 12 + pc;
    }
    return note >= 0 && note <= 127 ? note : -1; // out of range ("Cb-1", "G#9", "200"): rejected
}
} // namespace vs
