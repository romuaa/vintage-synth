#include "vintage/params/ParameterLayout.h"

#include "vintage/params/PartMappings.h"

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

    addMasterVolume (layout);

    addPart (layout, kLowerPrefix, "Lower ");
    addPart (layout, kUpperPrefix, "Upper ");

    layout.add (makeFloat (ParamID::vintage, "Vintage", 0.0f, 1.0f, 0.0f, Display::Panel10));

    // M3: key modes and the part mixer (appended, docs/architecture.md 4.5).
    addKeyMixParameters (layout);

    addFxParameters (layout);
    addArpSeqParameters (layout);

    // Wave 4: the output soft limiter (session settings, not part of a program; docs/decisions.md).
    addLimiterParameters (layout);

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
