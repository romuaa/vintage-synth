#pragma once

// Editor -> audio notes (docs/architecture.md 9.3.4, "Playing the keys"). JUCE-free.
//
// The keys of the Play page's keyboard strip play notes. The message thread is the one writer,
// the audio thread the one reader; owned by the processor like UiFeed and lives as long as it.
// Wait-free on both sides: no lock, no allocation, no system call; every atomic is lock-free
// (static_asserted below).
//
// Layout: a ring of kCapacity {note, velocity} slots (velocity 0 = note off), one FIFO for ons and
// offs alike. The writer publishes its write count with a release store, the reader hands slots
// back with its read count (release store). With nothing written the reader's drain is one
// acquire load.
//
// Never lose a note off, by reservation: every note the writer has on owns a slot for its future
// off. A note on is queued only if the ring has room for it and for its off on top of the offs
// already owed (used + notes on + 2 <= capacity); otherwise the newest note on is dropped and the
// writer never sends its off. So a note off always finds a slot: it is queued at once, in order,
// behind its own note on. There is no second path for offs (an earlier design kept a pending-off
// mask beside the ring; any such side channel can overtake or be overtaken by the ring, see
// docs/engineering-notes.md, "M7 Play page keys").

#include <atomic>
#include <bit>
#include <cstdint>

namespace vs
{
class UiNotes
{
public:
    static constexpr std::uint32_t kCapacity = 64;
    static_assert ((kCapacity & (kCapacity - 1)) == 0, "a power of two");

    struct Note
    {
        std::uint8_t note = 0;
        std::uint8_t velocity = 0; // 0 = note off
    };

    // ---- Writer side (message thread; one writer) ----

    /** Plays `note` (0..127) at `velocity` (1..127). False if it was dropped (the queue has no room
        for it and its off); a later noteOff of it then sends nothing. A note that is already on is
        released first. */
    bool noteOn (int note, int velocity) noexcept
    {
        if (note < 0 || note > 127)
            return false;
        if (isOn (note))
            noteOff (note);
        const int vel = velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity);
        if (used() + static_cast<std::uint32_t> (notesOn) + 2u > kCapacity)
        {
            ++dropped;
            return false;
        }
        push (note, vel);
        setBit (note, true);
        return true;
    }

    /** Releases `note` if this writer has it on. Always queued: its slot was reserved by its on. */
    void noteOff (int note) noexcept
    {
        if (note < 0 || note > 127 || ! isOn (note))
            return;
        setBit (note, false);
        push (note, 0);
    }

    /** Releases every note this writer has on. */
    void allNotesOff() noexcept
    {
        for (int n = 0; n < 128; ++n)
            if (isOn (n))
                noteOff (n);
    }

    /** True while this writer has `note` on (its on was queued and no off was asked for). */
    bool isOn (int note) const noexcept { return note >= 0 && note < 128 && ((onMask[note >> 6] >> (note & 63)) & 1u) != 0; }
    bool anyOn() const noexcept { return notesOn > 0; }
    /** Diagnostics (writer side, tests): note ons dropped because the queue had no room. */
    std::uint64_t droppedNoteOns() const noexcept { return dropped; }

    // ---- Reader side (audio thread; one reader) ----

    /** Calls `emit (note, velocity)` for every queued note in order; returns how many it emitted.
        One acquire load when nothing was written. */
    template <typename Emit>
    int drain (Emit&& emit) noexcept
    {
        const std::uint32_t end = written.load (std::memory_order_acquire);
        if (end == readCount)
            return 0;
        int count = 0;
        for (; readCount != end; ++readCount, ++count)
        {
            const Note n = slots[readCount & (kCapacity - 1)];
            emit (static_cast<int> (n.note), static_cast<int> (n.velocity));
        }
        consumed.store (readCount, std::memory_order_release);
        return count;
    }

    /** Empties the queue without playing anything (prepareToPlay, reset, bypassed blocks): the
        engine releases everything there, so nothing the writer had on stays sounding. */
    void discard() noexcept
    {
        drain ([] (int, int) noexcept {});
    }

private:
    static std::uint64_t bit (int note) noexcept { return std::uint64_t { 1 } << (note & 63); }
    void setBit (int note, bool on) noexcept
    {
        if (on)
            onMask[note >> 6] |= bit (note);
        else
            onMask[note >> 6] &= ~bit (note);
        notesOn = std::popcount (onMask[0]) + std::popcount (onMask[1]);
    }

    /** Slots written and not yet handed back (an upper bound while the reader drains). */
    std::uint32_t used() const noexcept { return writeCount - consumed.load (std::memory_order_acquire); }

    void push (int note, int velocity) noexcept
    {
        // Room is guaranteed by the reservation rule (noteOn checks it for the on and its off).
        slots[writeCount & (kCapacity - 1)] = Note { static_cast<std::uint8_t> (note), static_cast<std::uint8_t> (velocity) };
        ++writeCount;
        written.store (writeCount, std::memory_order_release);
    }

    Note slots[kCapacity] {};
    std::atomic<std::uint32_t> written { 0 };  // stored by the writer only
    std::atomic<std::uint32_t> consumed { 0 }; // stored by the reader only

    // Writer only.
    std::uint32_t writeCount = 0;
    std::uint64_t onMask[2] { 0, 0 };
    int notesOn = 0;
    std::uint64_t dropped = 0;
    // Reader only.
    std::uint32_t readCount = 0;
};

static_assert (std::atomic<std::uint32_t>::is_always_lock_free, "UiNotes: lock-free 32-bit atomics");
} // namespace vs
