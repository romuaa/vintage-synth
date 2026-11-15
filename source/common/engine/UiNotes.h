#pragma once

// Editor -> audio notes (docs/architecture.md 9.3.4, "Playing the keys"). JUCE-free.
//
// The keys of the Play page's keyboard strip play notes. The message thread is the one writer,
// the audio thread the one reader; owned by the processor like UiFeed and lives as long as it.
// Wait-free on both sides: no lock, no allocation, no system call; every atomic is lock-free
// (static_asserted below).
//
// Layout: a ring of kCapacity {note, velocity} slots (velocity 0 = note off). The writer publishes
// with one 64-bit word `published` (low 32 bits: slots written so far; high 32 bits: a counter of
// pending-off changes), the reader hands slots back with `consumed`.
//
// Never lose a note off: a full ring drops the newest note on (and the writer then never sends
// that note's off), but a note off that finds the ring full is kept in the pending-off mask
// (two 64-bit words, one bit per note). The writer retries it on every later call (and once per
// frame through retryPendingOffs); the reader also collects the mask after draining the ring in
// every block, so a pending off reaches the audio thread even if the editor closes meanwhile
// (no writer call ever comes). A note on is refused while its note's off is pending, so an off
// can never overtake a later on of the same note: the ring is drained before the mask, and the
// writer sees the bit cleared only after the reader drained the ring that block.
//
// With nothing written the reader's drain is one relaxed load.

#include <atomic>
#include <bit>
#include <cstdint>

namespace vs
{
class UiNotes
{
public:
    static constexpr std::uint32_t kCapacity = 64; // a power of two

    struct Note
    {
        std::uint8_t note = 0;
        std::uint8_t velocity = 0; // 0 = note off
    };

    // ---- Writer side (message thread; one writer) ----

    /** Plays `note` (0..127) at `velocity` (1..127). False if it was dropped: the ring is full or
        the note's off is still pending (a later noteOff of it then sends nothing). A note that
        is already on is released first. */
    bool noteOn (int note, int velocity) noexcept
    {
        if (note < 0 || note > 127)
            return false;
        if (isOn (note))
            noteOff (note);
        retryPendingOffs();
        const int vel = velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity);
        if (pendingHas (note) || ! push (note, vel))
        {
            ++dropped;
            return false;
        }
        setBit (onMask, note, true);
        return true;
    }

    /** Releases `note` if this writer has it on. Never lost: if the ring is full the off waits in
        the pending-off mask. */
    void noteOff (int note) noexcept
    {
        if (note < 0 || note > 127 || ! isOn (note))
            return;
        setBit (onMask, note, false);
        retryPendingOffs();
        if (push (note, 0))
            return;
        pending[note >> 6].fetch_or (bit (note), std::memory_order_relaxed);
        publish (published.load (std::memory_order_relaxed) + (std::uint64_t { 1 } << 32)); // the reader collects it
        ++pendedOffs;
    }

    /** Releases every note this writer has on. */
    void allNotesOff() noexcept
    {
        for (int n = 0; n < 128; ++n)
            if (isOn (n))
                noteOff (n);
    }

    /** Moves pending offs into the ring while it has room (call once per frame). */
    void retryPendingOffs() noexcept
    {
        for (int w = 0; w < 2; ++w)
        {
            std::uint64_t m = pending[w].load (std::memory_order_acquire);
            while (m != 0)
            {
                const int b = lowestBit (m);
                m &= m - 1;
                const int note = w * 64 + b;
                if (! push (note, 0))
                    return;
                // Taken by the reader meanwhile: it sends the off too (a doubled off is harmless).
                pending[w].fetch_and (~bit (note), std::memory_order_acq_rel);
            }
        }
    }

    /** True while this writer has `note` on (its on was queued and no off was asked for). */
    bool isOn (int note) const noexcept { return note >= 0 && note < 128 && ((onMask[note >> 6] >> (note & 63)) & 1u) != 0; }
    bool anyOn() const noexcept { return (onMask[0] | onMask[1]) != 0; }
    /** True while `note`'s off waits in the pending-off mask. */
    bool pendingHas (int note) const noexcept
    {
        return note >= 0 && note < 128 && (pending[note >> 6].load (std::memory_order_acquire) & bit (note)) != 0;
    }
    /** Diagnostics (writer side, tests): note ons dropped, note offs that had to wait. */
    std::uint64_t droppedNoteOns() const noexcept { return dropped; }
    std::uint64_t pendedNoteOffs() const noexcept { return pendedOffs; }

    // ---- Reader side (audio thread; one reader) ----

    /** Calls `emit (note, velocity)` for every queued note in order, then a note off (velocity 0)
        for every pending off; returns how many it emitted. One relaxed load when nothing changed. */
    template <typename Emit>
    int drain (Emit&& emit) noexcept
    {
        const std::uint64_t p = published.load (std::memory_order_relaxed);
        if (p == seen)
            return 0;
        std::atomic_thread_fence (std::memory_order_acquire);
        seen = p;
        int count = 0;
        const auto end = static_cast<std::uint32_t> (p);
        std::uint32_t r = consumed.load (std::memory_order_relaxed);
        for (; r != end; ++r, ++count)
        {
            const Note n = slots[r & (kCapacity - 1)];
            emit (static_cast<int> (n.note), static_cast<int> (n.velocity));
        }
        consumed.store (r, std::memory_order_release);
        // After the ring: an off that had to wait can only belong to a note on drained before it.
        for (int w = 0; w < 2; ++w)
        {
            std::uint64_t m = pending[w].exchange (0, std::memory_order_acq_rel);
            while (m != 0)
            {
                const int b = lowestBit (m);
                m &= m - 1;
                emit (w * 64 + b, 0);
                ++count;
            }
        }
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
    static void setBit (std::uint64_t (&mask)[2], int note, bool on) noexcept
    {
        if (on)
            mask[note >> 6] |= bit (note);
        else
            mask[note >> 6] &= ~bit (note);
    }
    static int lowestBit (std::uint64_t m) noexcept { return std::countr_zero (m); }

    bool push (int note, int velocity) noexcept
    {
        const std::uint64_t p = published.load (std::memory_order_relaxed);
        const auto w = static_cast<std::uint32_t> (p);
        if (w - consumed.load (std::memory_order_acquire) >= kCapacity)
            return false;
        slots[w & (kCapacity - 1)] = Note { static_cast<std::uint8_t> (note), static_cast<std::uint8_t> (velocity) };
        publish ((p & ~std::uint64_t { 0xFFFFFFFFu }) | static_cast<std::uint64_t> (w + 1u));
        return true;
    }
    void publish (std::uint64_t p) noexcept { published.store (p, std::memory_order_release); }

    Note slots[kCapacity] {};
    std::atomic<std::uint64_t> published { 0 };       // written by the writer only
    std::atomic<std::uint32_t> consumed { 0 };        // written by the reader only
    std::atomic<std::uint64_t> pending[2] { 0, 0 };   // set by the writer, taken by either side

    // Writer only.
    std::uint64_t onMask[2] { 0, 0 };
    std::uint64_t dropped = 0, pendedOffs = 0;
    // Reader only.
    std::uint64_t seen = 0;
};

static_assert (std::atomic<std::uint64_t>::is_always_lock_free, "UiNotes: lock-free 64-bit atomics (all five targets are 64-bit)");
static_assert (std::atomic<std::uint32_t>::is_always_lock_free, "UiNotes: lock-free 32-bit atomics");
} // namespace vs
