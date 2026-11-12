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
// M3 (appended after vintage, in this order).
inline constexpr const char* keyMode = "key.mode";
inline constexpr const char* keySplitPoint = "key.split_point";
inline constexpr const char* mixBalance = "mix.balance";
} // namespace ParamID

/** "Key Mode" choices; order == vs::KeyMode. */
inline constexpr std::array<const char*, 3> kKeyModeLabels { "Whole", "Dual", "Split" };
inline constexpr int kDefaultSplitPoint = 60; // C4

/** MIDI note -> name, middle C (60) = "C4". */
juce::String noteName (int note);
/** "C4", "c#4", "Db-1", "60" -> MIDI note (clamped to 0..127); -1 if unreadable. */
int noteFromName (const juce::String& text);

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
