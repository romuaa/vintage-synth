#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace bs
{
/** Exitium B-Side's host parameters (docs/architecture.md 8.3, 8.7). Today only the shared globals,
    in this order (75): master.volume, key.mode, key.split_point, mix.balance, the 11 effect
    parameters, the 58 arpeggiator/sequencer parameters, master.limiter, master.limiter.ceiling.
    S2 appends the per-part parameters after them (lower.*, upper.*, chip.clock; 8.7), so these IDs,
    their order and their ranges stay what they are here. */
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** Number of host parameters of the plugin shell (S1b). */
inline constexpr int kShellParameterCount = 75;
} // namespace bs
