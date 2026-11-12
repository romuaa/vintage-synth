#pragma once

#include "common/engine/NoteEvent.h"

#include <cstdint>
#include <span>
#include <vector>

namespace vs
{
/**
    Parses one raw MIDI message (as delivered by the host, running status already resolved).
    Channel is ignored (omni). Returns false for anything a Part does not use: system
    messages (SysEx, clock, ...), other controllers, program change, pressure, and malformed
    data (too short, or a data byte with the high bit set), which is rejected rather than
    reinterpreted. Never allocates.
*/
inline bool parseMidiMessage (const std::uint8_t* data, int numBytes, int offset, NoteEvent& out) noexcept
{
    // Only 3-byte channel messages are used (note off/on, controller, pitch bend); program
    // change and channel pressure (2 bytes) and all system messages are dropped.
    if (data == nullptr || numBytes < 3 || data[0] < 0x80 || data[0] >= 0xF0)
        return false;
    if ((data[1] & 0x80) != 0 || (data[2] & 0x80) != 0)
        return false; // a status byte where data is expected: malformed

    const int status = data[0] & 0xF0;
    const auto d1 = data[1];
    const auto d2 = data[2];

    out = NoteEvent { offset, EventType::NoteOff, d1, 0.0f };
    switch (status)
    {
        case 0x90:
            if (d2 > 0)
            {
                out.type = EventType::NoteOn;
                out.value = static_cast<float> (d2) / 127.0f;
            }
            return true; // velocity 0 = NoteOff

        case 0x80:
            return true;

        case 0xE0:
            out.type = EventType::PitchBend;
            out.note = 0;
            out.value = static_cast<float> ((d2 << 7 | d1) - 8192) / 8192.0f;
            return true;

        case 0xB0:
            out.note = 0;
            switch (d1)
            {
                case 1:   out.type = EventType::ModWheel; out.value = static_cast<float> (d2) / 127.0f; return true;
                case 64:  out.type = EventType::SustainPedal; out.value = d2 >= 64 ? 1.0f : 0.0f; return true;
                case 120: out.type = EventType::AllSoundOff; return true;
                case 121: out.type = EventType::ResetControllers; return true;
                case 123: out.type = EventType::AllNotesOff; return true;
                default:  return false;
            }

        default:
            return false;
    }
}

/**
    Fixed-capacity NoteEvent buffer, filled once per render chunk on the audio thread.

    Overflow rule (docs/architecture.md section 2): when the buffer is full, the remaining
    events of the chunk are dropped and an AllNotesOff is appended at the offset of the last
    (dropped) event, so a note can never hang. One slot is reserved for it.
*/
class EventParser
{
public:
    static constexpr int kDefaultCapacity = 2048;

    /** Allocates. Call from prepareToPlay only. */
    void prepare (int capacity = kDefaultCapacity)
    {
        buffer.assign (static_cast<size_t> (capacity < 2 ? 2 : capacity), NoteEvent {});
        clear();
    }

    void clear() noexcept
    {
        count = 0;
        overflowed = false;
        lastOffset = 0;
    }

    /** Parses and appends one raw message; offsets must be ascending within a chunk. */
    void add (const std::uint8_t* data, int numBytes, int offset) noexcept
    {
        NoteEvent e;
        if (! parseMidiMessage (data, numBytes, offset, e))
            return;

        if (overflowed || count >= capacity() - 1)
        {
            overflowed = true;
            lastOffset = offset > lastOffset ? offset : lastOffset;
            return;
        }
        buffer[static_cast<size_t> (count++)] = e;
    }

    /** Closes the chunk: appends the overflow AllNotesOff if events were dropped. */
    void finish() noexcept
    {
        if (overflowed && count < capacity())
            buffer[static_cast<size_t> (count++)] = NoteEvent { lastOffset, EventType::AllNotesOff, 0, 0.0f };
    }

    std::span<const NoteEvent> events() const noexcept { return { buffer.data(), static_cast<size_t> (count) }; }
    int capacity() const noexcept { return static_cast<int> (buffer.size()); }
    bool didOverflow() const noexcept { return overflowed; }

private:
    std::vector<NoteEvent> buffer;
    int count = 0;
    int lastOffset = 0;
    bool overflowed = false;
};
} // namespace vs
