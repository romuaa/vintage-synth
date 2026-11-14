#pragma once

// Global parameters every bi-timbral instrument has (docs/architecture.md 8.2.1): master volume,
// key mode, split point and part balance, and the output soft limiter. IDs, defaults and the
// appenders that define them for the host. An instrument calls the appenders in its own layout
// (common/shell); Vintage calls them in its historical order (master volume, its parts, its own
// globals, key/mix, effects, arp/seq, limiter), so its host-facing list is unchanged.

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

namespace vs
{
namespace ParamID
{
// Dotted, stable IDs. Frozen after v1.0: append only.
inline constexpr const char* masterVolume = "master.volume";
// M3: key modes and the part mixer (docs/architecture.md 4.5).
inline constexpr const char* keyMode = "key.mode";
inline constexpr const char* keySplitPoint = "key.split_point";
inline constexpr const char* mixBalance = "mix.balance";
// Wave 4: output soft limiter (docs/architecture.md 6c.2). Session settings like master.volume:
// not part of a program.
inline constexpr const char* masterLimiter = "master.limiter";
inline constexpr const char* masterLimiterCeiling = "master.limiter.ceiling";
} // namespace ParamID

/** "Key Mode" choices; order == vs::KeyMode. */
inline constexpr std::array<const char*, 3> kKeyModeLabels { "Whole", "Dual", "Split" };
inline constexpr int kDefaultSplitPoint = 60; // C4
inline constexpr float kDefaultLimiterCeilingDb = -1.0f;

/** Appends "Master Volume" (-60 ... +6 dB, default -6 dB). */
void addMasterVolume (juce::AudioProcessorValueTreeState::ParameterLayout& layout);
/** Appends key.mode, key.split_point and mix.balance, in this order. */
void addKeyMixParameters (juce::AudioProcessorValueTreeState::ParameterLayout& layout);
/** Appends master.limiter and master.limiter.ceiling, in this order. */
void addLimiterParameters (juce::AudioProcessorValueTreeState::ParameterLayout& layout);
} // namespace vs
