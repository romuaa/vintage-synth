#include "bside/params/ParameterLayout.h"

#include <cmath>

#include "common/params/FxArpParams.h"
#include "common/params/GlobalParams.h"
#include "common/params/ParamFactory.h"

namespace bs
{
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

float numberIn (const juce::String& t) { return t.retainCharacters ("0123456789.-").getFloatValue(); }

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
    }
    return {};
}

float floatFromText (Disp d, const juce::String& t)
{
    switch (d)
    {
        case Disp::Cents:
        case Disp::Percent:
        case Disp::PercentSigned:
        case Disp::Register:
        case Disp::Hz:
        case Disp::TickHz:        return numberIn (t);
        case Disp::WheelCents:    return map::wheelVibratoFromCents (numberIn (t));
        case Disp::UnisonCents:   return map::unisonDetuneFromCents (numberIn (t));
        case Disp::Unit:          return vs::fromText (vs::Display::Percent, t);
        case Disp::Porta:         return vs::fromText (vs::Display::PortaTime, t);
        case Disp::Pan:           return vs::fromText (vs::Display::Pan, t);
    }
    return 0.0f;
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

int intFromText (IntDisp d, const juce::String& t)
{
    switch (d)
    {
        case IntDisp::Plain:
        case IntDisp::Semitones:  return juce::roundToInt (numberIn (t));
        case IntDisp::Multiplier: return juce::roundToInt (numberIn (t));
        case IntDisp::Attack:
        case IntDisp::Decay:
        case IntDisp::Release:
        {
            // "250 ms", "1.5 s"; a bare number counts as milliseconds.
            const double value = static_cast<double> (t.retainCharacters ("0123456789.").getFloatValue());
            const bool ms = t.containsIgnoreCase ("ms");
            const bool sec = ! ms && t.containsIgnoreCase ("s");
            return map::envSettingFromMs (stageOf (d), sec ? value * 1000.0 : value);
        }
    }
    return 0;
}

std::unique_ptr<juce::AudioParameterFloat> makeFloat (const juce::String& id, const juce::String& name, float mn, float mx,
                                                      float def, Disp d)
{
    auto attrs = juce::AudioParameterFloatAttributes()
                     .withStringFromValueFunction ([d] (float v, int) { return floatText (d, v); })
                     .withValueFromStringFunction ([d, mn, mx] (const juce::String& t) { return juce::jlimit (mn, mx, floatFromText (d, t)); });

    juce::NormalisableRange<float> range { mn, mx };
    if (d == Disp::TickHz) // exponential: equal steps of the slider are equal musical ratios
        range = juce::NormalisableRange<float> (
            mn, mx, [] (float s, float e, float n) { return s * std::pow (e / s, juce::jlimit (0.0f, 1.0f, n)); },
            [] (float s, float e, float v) { return std::log (juce::jlimit (s, e, v) / s) / std::log (e / s); });

    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, vs::kParamVersionHint }, name, range, def, std::move (attrs));
}

std::unique_ptr<vs::SteppedIntParameter> makeInt (const juce::String& id, const juce::String& name, int mn, int mx, int def, IntDisp d)
{
    auto attrs = juce::AudioParameterIntAttributes()
                     .withStringFromValueFunction ([d] (int v, int) { return intText (d, v); })
                     .withValueFromStringFunction ([d, mn, mx] (const juce::String& t) { return juce::jlimit (mn, mx, intFromText (d, t)); });
    return std::make_unique<vs::SteppedIntParameter> (juce::ParameterID { id, vs::kParamVersionHint }, name, mn, mx, def, attrs);
}

void addPart (juce::AudioProcessorValueTreeState::ParameterLayout& layout, const juce::String& prefix, const juce::String& namePrefix)
{
    // The order is part of the host-facing contract: never reorder, only append.
#define BS_ADD_FLOAT(member, id, name, mn, mx, def, kind) \
    layout.add (makeFloat (prefix + id, namePrefix + juce::String::fromUTF8 (name), mn, mx, def, Disp::kind));
#define BS_ADD_INT(member, id, name, mn, mx, def, kind) \
    layout.add (makeInt (prefix + id, namePrefix + juce::String::fromUTF8 (name), mn, mx, def, IntDisp::kind));
#define BS_ADD_CHOICE(member, id, name, labels, def) \
    layout.add (vs::makeChoice (prefix + id, namePrefix + juce::String::fromUTF8 (name), labels, def));
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
    layout.add (vs::makeChoice (kChipClockId, "Chip Clock", kChipClockLabels, 0));
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
