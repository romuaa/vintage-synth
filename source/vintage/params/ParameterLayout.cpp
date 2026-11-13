#include "vintage/params/ParameterLayout.h"

#include "vintage/params/PartMappings.h"
#include "common/fx/SoftLimiter.h"

namespace vs
{
namespace
{
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
    // The text parser needs the parameter's own value (unreadable text leaves it unchanged); the
    // parameter does not exist yet when the lambda is made, so it reads it through a shared slot.
    auto splitPointHolder = std::make_shared<juce::AudioParameterInt*> (nullptr);
    auto split = std::make_unique<SteppedIntParameter> ( // stepped for hosts (same ID and range as M3)
        juce::ParameterID { ParamID::keySplitPoint, kParamVersionHint }, "Split Point", 0, 127, kDefaultSplitPoint,
        juce::AudioParameterIntAttributes()
            .withStringFromValueFunction ([] (int v, int) { return noteName (v); })
            .withValueFromStringFunction ([current = splitPointHolder] (const juce::String& t) {
                const int n = noteFromName (t);
                return n >= 0 ? n : (*current != nullptr ? (*current)->get() : kDefaultSplitPoint);
            }));
    *splitPointHolder = split.get(); // owned by the processor's parameter tree for its whole life
    layout.add (std::move (split));
    layout.add (makeFloat (ParamID::mixBalance, "Balance", -1.0f, 1.0f, 0.0f, Display::Balance));

    addFxParameters (layout);
    addArpSeqParameters (layout);

    // Wave 4: the output soft limiter (session settings, not part of a program; docs/decisions.md).
    layout.add (makeChoice (ParamID::masterLimiter, "Limiter", kOffOnLabels, 1));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamID::masterLimiterCeiling, kParamVersionHint },
        "Limiter Ceiling",
        juce::NormalisableRange<float> { fx::SoftLimiter::kMinCeilingDb, fx::SoftLimiter::kMaxCeilingDb },
        kDefaultLimiterCeilingDb,
        juce::AudioParameterFloatAttributes()
            .withLabel ("dB")
            .withStringFromValueFunction ([] (float v, int) { // shown in 0.1 dB, stored exactly
                const float shown = std::round (v * 10.0f) / 10.0f;
                return juce::String (std::abs (shown) < 0.05f ? 0.0f : shown, 1); // never "-0.0"
            })
            .withValueFromStringFunction ([] (const juce::String& t) { return t.getFloatValue(); })));

    return layout;
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
    // Sanitised (Mappings.h): a non-finite host value becomes the default, anything else is clamped
    // to the range; in-range values pass bit-exactly.
#define VS_READ_FLOAT(member, id, name, mn, mx, def, kind) \
    out.member = map::sanitiseParam (member->load (std::memory_order_relaxed), mn, mx, def);
#define VS_READ_CHOICE(member, id, name, labels, def) \
    out.member = map::sanitiseIndex (member->load (std::memory_order_relaxed), 0, static_cast<int> (labels.size()) - 1, def);
    VS_PART_FLOAT_PARAMS (VS_READ_FLOAT)
    VS_PART_CHOICE_PARAMS (VS_READ_CHOICE)
#undef VS_READ_FLOAT
#undef VS_READ_CHOICE
}
} // namespace vs
