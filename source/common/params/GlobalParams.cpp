#include "common/params/GlobalParams.h"

#include "common/fx/SoftLimiter.h"
#include "common/params/CommonLabels.h"
#include "common/params/ParamFactory.h"

namespace vs
{
void addMasterVolume (juce::AudioProcessorValueTreeState::ParameterLayout& layout)
{
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamID::masterVolume, kParamVersionHint },
        "Master Volume",
        juce::NormalisableRange<float> { -60.0f, 6.0f, 0.1f },
        -6.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));
}

void addKeyMixParameters (juce::AudioProcessorValueTreeState::ParameterLayout& layout)
{
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
}

void addLimiterParameters (juce::AudioProcessorValueTreeState::ParameterLayout& layout)
{
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
}
} // namespace vs
