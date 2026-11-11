#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace vs
{
namespace ParamID
{
// Dotted, stable IDs. Frozen after v1.0 — append only.
inline constexpr const char* masterVolume = "master.volume";
} // namespace ParamID

// Version hint for every parameter. JUCE/VST3 uses it to keep parameter order stable.
inline constexpr int kParamVersionHint = 1;

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
} // namespace vs
