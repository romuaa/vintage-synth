#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "params/PartParamList.h"

namespace vs
{
namespace ParamID
{
// Global (non-part) parameters. Dotted, stable IDs. Frozen after v1.0 — append only.
inline constexpr const char* masterVolume = "master.volume";
inline constexpr const char* vintage = "global.vintage";
} // namespace ParamID

// Version hint for every parameter. JUCE/VST3 uses it to keep parameter order stable.
inline constexpr int kParamVersionHint = 1;

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** Cached raw-value pointers for one part. Bound once, read once per block (lock-free). */
class PartParamBinding
{
public:
    void bind (juce::AudioProcessorValueTreeState& state, const juce::String& prefix);

    /** Audio-thread safe: atomic loads only. */
    void read (PartParams& out) const noexcept;

private:
#define VS_DECLARE_PTR(member, ...) std::atomic<float>* member = nullptr;
    VS_PART_FLOAT_PARAMS (VS_DECLARE_PTR)
    VS_PART_CHOICE_PARAMS (VS_DECLARE_PTR)
#undef VS_DECLARE_PTR
};
} // namespace vs
