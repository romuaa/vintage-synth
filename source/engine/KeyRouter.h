#pragma once

#include "engine/NoteEvent.h"

#include <array>
#include <cstdint>
#include <span>

namespace vs
{
/** Order == the "Key Mode" parameter's choice order. */
enum class KeyMode : std::uint8_t
{
    Whole, // all notes to Lower; Upper is silent and not rendered
    Dual,  // all notes to both parts
    Split  // notes below the split point to Lower, at or above it to Upper
};

/**
    Routes the parsed NoteEvents of one render chunk to the Lower and Upper parts
    (docs/architecture.md 4.2). JUCE-free, no allocation, deterministic.

    - Note-ons go where the key mode (and split point) say at the moment they arrive.
    - A note-off goes to exactly the part(s) that received that key's note-on: the router
      remembers the owner of each of the 128 keys, so moving the split point (or anything else)
      while keys are held can never hang a note. A key pressed again without a release between
      (only possible with a broken stream) adds the new owner; the one note-off then reaches both.
      A note-off for a key nobody owns (already released, or forgotten by a mode change or a
      panic event) is dropped: no part holds that key.
    - Pitch bend, mod wheel, sustain pedal, All Notes Off, All Sound Off and Reset Controllers
      go to both parts in every mode (so the overflow marker, an All Notes Off, reaches both).
    - All Notes Off and All Sound Off also forget the owners (the parts release every key).

    The caller provides one output list per part, each at least as long as the input list:
    every input event produces at most one event per part, so nothing can be lost.

    Changing the key mode is the processor's job (setMode returns true): it releases every voice
    of both parts without clearing controllers, and the router forgets all held keys.
*/
class KeyRouter
{
public:
    static constexpr std::uint8_t kLower = 1, kUpper = 2;

    /** Forgets every key. Keeps mode and split point. */
    void reset() noexcept { owners.fill (0); }

    /**
        Sets the key mode. Returns true if it changed; then every held key is forgotten and the
        caller must release all voices of both parts (docs/architecture.md 4.2).
    */
    bool setMode (KeyMode m) noexcept
    {
        if (m == mode)
            return false;
        mode = m;
        reset();
        return true;
    }

    /** Split point: MIDI note 0..127; notes at or above it go to Upper. Affects new notes only. */
    void setSplitPoint (int note) noexcept { splitPoint = note < 0 ? 0 : (note > 127 ? 127 : note); }

    KeyMode getMode() const noexcept { return mode; }
    int getSplitPoint() const noexcept { return splitPoint; }

    /** Part mask (kLower | kUpper) that currently owns `note` (0 = released / unknown). */
    std::uint8_t ownerOf (int note) const noexcept { return note >= 0 && note < 128 ? owners[static_cast<size_t> (note)] : 0; }

    /** Number of keys some part holds (for tests and diagnostics). */
    int numOwnedKeys() const noexcept
    {
        int n = 0;
        for (const auto o : owners)
            n += o != 0 ? 1 : 0;
        return n;
    }

    /** Which part(s) a note-on of `note` goes to now. */
    std::uint8_t targetFor (int note) const noexcept
    {
        switch (mode)
        {
            case KeyMode::Whole: return kLower;
            case KeyMode::Dual:  return kLower | kUpper;
            case KeyMode::Split: return note < splitPoint ? kLower : kUpper;
        }
        return kLower;
    }

    struct Counts
    {
        int lower = 0, upper = 0;
    };

    /**
        Routes `in` (offsets ascending) into `lower` and `upper` (each at least in.size() long).
        Offsets are kept, so both output lists stay ascending. Returns how many events each got.
    */
    Counts route (std::span<const NoteEvent> in, std::span<NoteEvent> lower, std::span<NoteEvent> upper) noexcept
    {
        Counts c;
        const auto put = [&] (std::uint8_t mask, const NoteEvent& e) {
            if ((mask & kLower) != 0 && static_cast<size_t> (c.lower) < lower.size())
                lower[static_cast<size_t> (c.lower++)] = e;
            if ((mask & kUpper) != 0 && static_cast<size_t> (c.upper) < upper.size())
                upper[static_cast<size_t> (c.upper++)] = e;
        };

        for (const auto& e : in)
        {
            switch (e.type)
            {
                case EventType::NoteOn:
                {
                    if (e.note > 127)
                        break; // not a key
                    const auto mask = targetFor (e.note);
                    owners[e.note] = static_cast<std::uint8_t> (owners[e.note] | mask);
                    put (mask, e);
                    break;
                }

                case EventType::NoteOff:
                {
                    if (e.note > 127)
                        break;
                    const auto mask = owners[e.note];
                    owners[e.note] = 0;
                    put (mask, e);
                    break;
                }

                case EventType::AllNotesOff:
                case EventType::AllSoundOff:
                    reset();
                    put (kLower | kUpper, e);
                    break;

                case EventType::PitchBend:
                case EventType::ModWheel:
                case EventType::SustainPedal:
                case EventType::ResetControllers:
                    put (kLower | kUpper, e);
                    break;
            }
        }
        return c;
    }

private:
    std::array<std::uint8_t, 128> owners {};
    KeyMode mode = KeyMode::Whole;
    int splitPoint = 60;
};
} // namespace vs
