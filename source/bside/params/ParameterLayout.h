#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <optional>

#include "bside/params/PartMappings.h"
#include "bside/params/PartParamList.h"

namespace bs
{
/** Exitium B-Side's host parameters (docs/architecture.md 8.3, 8.7), 202 in this order:
    the 75 shared globals (master.volume, key.mode, key.split_point, mix.balance, the 11 effect
    parameters, the 58 arpeggiator/sequencer parameters, master.limiter, master.limiter.ceiling),
    then the 63 per-part parameters of the lower part (lower.*), the same 63 of the upper part
    (upper.*; both generated from PartParamList.h), then chip.clock. The IDs, their order and their
    ranges are part of the host-facing contract: only append. */
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** Number of shared host parameters (S1b); they are the first ones of the layout. */
inline constexpr int kShellParameterCount = 75;
/** Index of the first lower.* parameter, of the first upper.* parameter and of chip.clock. */
inline constexpr int kLowerFirstIndex = kShellParameterCount;
inline constexpr int kUpperFirstIndex = kShellParameterCount + kPartParamCount;
inline constexpr int kChipClockIndex = kShellParameterCount + 2 * kPartParamCount;
/** Number of host parameters of the plugin after S2: 75 + 2 x 63 + 1 = 202. */
inline constexpr int kParameterCount = kChipClockIndex + 1;

/** The first well-formed number in typed text: an optional sign, digits, an optional fraction, an optional exponent;
    a unit after it is ignored ("12 ct" gives 12, "1e3" 1000, "--5" -5, " +7 " 7). None if the text holds no number.
    An exponent past the double range gives infinity: callers treat a non-finite value as "no usable value". */
std::optional<double> parseFirstNumber (const juce::String& text);

/** Cached raw-value pointers for one part. Bound once off the audio thread, read once per block
    (lock-free). Nothing reads it yet: the chip is wired in later. */
class PartParamBinding
{
public:
    /** @param prefix  kLowerPrefix or kUpperPrefix. Throws vs::ConfigError if a parameter is missing. */
    void bind (juce::AudioProcessorValueTreeState& state, const juce::String& prefix);

    /** Audio-thread safe: atomic loads only. Every value is sanitised: a non-finite host value becomes
        the default, anything else is clamped to the range; in-range values pass bit-exactly. */
    void read (PartParams& out) const noexcept;

private:
#define BS_DECLARE_PTR(member, ...) std::atomic<float>* member = nullptr;
    BS_PART_PARAMS (BS_DECLARE_PTR, BS_DECLARE_PTR, BS_DECLARE_PTR)
#undef BS_DECLARE_PTR
};

/** chip.clock (0 = 50 Hz system, 1 = 60 Hz system). */
class ChipClockBinding
{
public:
    void bind (juce::AudioProcessorValueTreeState& state);
    int read() const noexcept;

private:
    std::atomic<float>* value = nullptr;
};
} // namespace bs
