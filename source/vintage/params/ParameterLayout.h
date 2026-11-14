#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "common/params/FxArpParams.h"
#include "common/params/GlobalParams.h"
#include "common/params/ParamFactory.h"
#include "vintage/params/PartMappings.h"
#include "vintage/params/PartParamList.h"

namespace vs
{
namespace ParamID
{
// This instrument's own global parameter (the shared ones are in common/params/GlobalParams.h).
// Dotted, stable IDs. Frozen after v1.0 — append only.
inline constexpr const char* vintage = "global.vintage"; // appended after the upper part
} // namespace ParamID

/** Vintage's host-facing parameter list. Order is part of the contract: master volume, lower
    part, upper part, global.vintage, key/mix, effects, arp/seq, limiter (never reorder). */
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
