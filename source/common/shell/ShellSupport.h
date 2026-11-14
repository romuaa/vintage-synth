#pragma once

// Non-template helpers of the shared bi-timbral skeleton (BiTimbralProcessor.h): the small pure
// functions its block algorithm uses (audio thread: no allocation, no locks), and the state-format
// constants and helpers (message or host thread, never the audio thread).

#include <juce_audio_processors/juce_audio_processors.h>

#include "common/engine/KeyRouter.h"
#include "common/engine/NoteEvent.h"
#include "common/params/FxArpParams.h"
#include "common/params/ParamFactory.h"

#include <cstring>
#include <span>

namespace vs::presets
{
class PresetManager;
}

namespace vs::shell
{
// ---- State format (shared by every instrument; the tag and the version come from the instrument) ----
inline constexpr const char* kStateVersionAttr = "stateVersion";
inline constexpr const char* kPresetInfoType = "Preset"; // child of the state tree: the bank / program the sound came from

/** A session saved before a parameter existed has no entry for it. The loaded sound must not
    depend on the instance's history, so every parameter of ours missing from the incoming state
    gets an entry without a value, which AudioProcessorValueTreeState restores to the parameter's
    default. (JUCE 9.0.5 happens to do the same for a missing entry, through the child it appends
    while reconnecting its parameters; this makes the rule explicit and independent of that
    internal. Tested in OutputStageTests, "State: a parameter missing ...".) */
void addMissingParameters (juce::ValueTree& state, const juce::Array<juce::AudioProcessorParameter*>& params);

/** The shared placeholder editor (InstrumentEditor.h; declared here to keep the GUI headers out of
    the processor's header). Message thread. */
juce::AudioProcessorEditor* createInstrumentEditor (juce::AudioProcessor& owner, presets::PresetManager& presets);

namespace detail
{
/** Offset of the first note-on in a routed chunk, or -1. */
inline int firstNoteOn (std::span<const NoteEvent> events) noexcept
{
    for (const auto& e : events)
        if (e.type == EventType::NoteOn)
            return e.offset;
    return -1;
}

inline KeyMode keyModeOf (const BoundParam& p) noexcept
{
    return static_cast<KeyMode> (p.index()); // NaN-safe, clamped to 0..2
}

inline bool sameBits (float a, float b) noexcept { return std::memcmp (&a, &b, sizeof (float)) == 0; }

/** True if `out` is exactly what the arp/seq module writes when it only passes `in` on in mode
    Off: the same events in the same order, offsets moved inside [0, len - 1] at most. */
inline bool isPassThrough (std::span<const NoteEvent> in, std::span<const NoteEvent> out, int len) noexcept
{
    if (in.size() != out.size())
        return false;
    const int last = juce::jmax (len - 1, 0);
    for (size_t i = 0; i < in.size(); ++i)
        if (in[i].type != out[i].type || in[i].note != out[i].note || ! sameBits (in[i].value, out[i].value)
            || juce::jlimit (0, last, in[i].offset) != out[i].offset)
            return false;
    return true;
}

/** Whether the arp/seq sits in front of `part` (0 Lower, 1 Upper). In Whole mode only Lower
    plays, so the arp/seq applies to Lower whatever arp.target says; the selector matters in Dual
    and Split. */
inline bool partSelected (ArpTarget target, KeyMode mode, int part) noexcept
{
    if (mode == KeyMode::Whole)
        return part == 0;
    return target == ArpTarget::Both || static_cast<int> (target) == part;
}
} // namespace detail
} // namespace vs::shell
