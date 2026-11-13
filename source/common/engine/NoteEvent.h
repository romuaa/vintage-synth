#pragma once

#include <cstdint>
#include <type_traits>

namespace vs
{
/** Event kinds a Part (and later the arpeggiator/sequencer) understands. See docs/architecture.md section 2. */
enum class EventType : std::uint8_t
{
    NoteOn,
    NoteOff,
    PitchBend,
    ModWheel,
    SustainPedal,
    AllNotesOff,
    AllSoundOff,
    ResetControllers
};

/** One parsed MIDI event. Plain data: JUCE-free and trivially copyable. */
struct NoteEvent
{
    int offset;        // sample position inside the current render call, host rate, ascending
    EventType type;
    std::uint8_t note; // NoteOn / NoteOff
    float value;       // NoteOn: velocity 0..1; PitchBend: -1..1; ModWheel: 0..1; SustainPedal: 0 or 1
};

static_assert (std::is_trivially_copyable_v<NoteEvent>, "NoteEvent must stay plain data");
} // namespace vs
