#pragma once

// The hand-over between the threads that apply presets or restore state (the message thread, or a
// host thread in setStateInformation) and the audio thread (which must not be called, blocked or
// made to wait). Atomics only on the audio side.
//
// Writers are serialised by PresetManager (one mutex, never taken on the audio thread), so at most
// one apply or state restore writes parameters at any time. Applying a program has three steps:
//   1. beginHold()      the sequence counter becomes odd: the audio thread keeps using the parameter
//                       values it already has (no key-mode change handling either), so the 170-odd
//                       parameter writes of step 3 can never be heard half done;
//   2. requestRelease   the audio thread releases every sounding note (the click-free release that
//                       a key-mode change uses) at the start of its next block and acknowledges;
//                       the writer waits for that, but only while audio is running (a heartbeat
//                       counter) and never longer than a timeout;
//   3. the parameters are set, then endHold(): the counter becomes even again and the next block
//      reads them all at once.
// Holds nest (a counter): the sequence changes only on the outermost begin and end.
//
// The audio thread reads the parameters as a seqlock reader, wait-free:
//     token = beginRead();                 // odd: a hold is on, keep the previous block's values
//     ... copy every parameter it uses ...
//     if (endRead (token)) use the copy;   // false: a hold began (or ended) meanwhile: discard it
// so a block never mixes values from before and after (or during) an apply, even when the hold
// begins while it is copying. Parameter values themselves are atomics (APVTS), so the copy is not
// a data race; the sequence only decides whether the copy is consistent.
// If the wait in step 2 times out (audio stopped, or a block longer than the timeout) the
// parameters are set anyway: the request stays pending and the audio thread honours it at its
// next block, which reads the new parameters in the same block, exactly as a key-mode change does.
// The audio thread acknowledges a release only after every parameter read of that block.
//
// MIDI Program Change: the audio thread posts the program number; the message thread takes the
// latest one (older requests are overwritten) and applies it shortly afterwards (its poll timer).
// So notes in the same block as the Program Change start on the old program and are then
// released by the apply; while rendering offline the moment it takes effect depends on when the
// message thread runs, so it is not deterministic.

#include <atomic>
#include <cstdint>

#include <juce_core/juce_core.h>

namespace vs::presets
{
class ApplyGate
{
public:
    // ---- audio thread: wait-free, no allocation ----
    /** Once per block, first thing (also for blocks that do no rendering). */
    void blockStarted() noexcept { heartbeat.fetch_add (1, std::memory_order_relaxed); }

    /** Starts a parameter read. An odd token means a hold is on: do not use what is read. */
    std::uint32_t beginRead() const noexcept { return sequence.load (std::memory_order_acquire); }
    /** True if the parameters read since beginRead(token) form one consistent set. */
    bool endRead (std::uint32_t token) const noexcept
    {
        std::atomic_thread_fence (std::memory_order_acquire); // the reads above happen before the re-check
        return (token & 1u) == 0u && sequence.load (std::memory_order_relaxed) == token;
    }
    /** True while a preset is being applied (a hold is on). */
    bool isHolding() const noexcept { return (sequence.load (std::memory_order_acquire) & 1u) != 0u; }

    /** True once per release request. The caller releases every note, then calls acknowledgeRelease(). */
    bool takeReleaseRequest() noexcept
    {
        const auto r = requested.load (std::memory_order_acquire);
        if (r == seen)
            return false;
        seen = r;
        return true;
    }
    void acknowledgeRelease() noexcept { acknowledged.store (seen, std::memory_order_release); }
    /** A MIDI Program Change arrived (0..127). The latest one wins. */
    void postMidiProgram (int program) noexcept { midiProgram.store (program, std::memory_order_release); }

    // ---- writers (serialised by the caller's mutex; never the audio thread) ----
    void beginHold() noexcept
    {
        if (holds.fetch_add (1, std::memory_order_relaxed) == 0)
        {
            sequence.fetch_add (1, std::memory_order_relaxed); // odd
            std::atomic_thread_fence (std::memory_order_release); // ... before any parameter write
        }
    }
    void endHold() noexcept
    {
        const int before = holds.fetch_sub (1, std::memory_order_relaxed);
        jassert (before > 0);
        if (before == 1)
            sequence.fetch_add (1, std::memory_order_release); // even: the writes above are visible first
    }
    int holdDepth() const noexcept { return holds.load (std::memory_order_relaxed); }

    /** The latest MIDI Program Change since the last call, or -1. */
    int takeMidiProgram() noexcept { return midiProgram.exchange (-1, std::memory_order_acq_rel); }

    /** Waits are given up when no block has started for idleMs, or after maxMs in all. 0 for both:
        never wait (offline rendering, tests). */
    void setTimeouts (int maxMs, int idleMs) noexcept
    {
        maxWaitMs.store (maxMs, std::memory_order_relaxed);
        idleWaitMs.store (idleMs, std::memory_order_relaxed);
    }

    /** Asks the audio thread to release every note and waits for its acknowledgement. Returns true
        if it came (false: audio is not running, or too slow: the request stays pending). */
    bool requestReleaseAndWait() noexcept
    {
        const auto id = requested.fetch_add (1, std::memory_order_acq_rel) + 1u;
        const int maxMs = maxWaitMs.load (std::memory_order_relaxed);
        const int idleMs = idleWaitMs.load (std::memory_order_relaxed);
        auto beat = heartbeat.load (std::memory_order_relaxed);
        if (beat == 0 || maxMs <= 0) // audio has never run, or waiting is off: nothing to wait for
            return isAcknowledged (id);

        const auto start = juce::Time::getMillisecondCounter();
        auto lastBeat = start;
        for (;;)
        {
            if (isAcknowledged (id))
                return true;
            const auto now = juce::Time::getMillisecondCounter();
            const auto b = heartbeat.load (std::memory_order_relaxed);
            if (b != beat)
            {
                beat = b;
                lastBeat = now;
            }
            if (now - lastBeat >= static_cast<std::uint32_t> (idleMs) || now - start >= static_cast<std::uint32_t> (maxMs))
                return false;
            juce::Thread::sleep (1);
        }
    }

    std::uint32_t blocksStarted() const noexcept { return heartbeat.load (std::memory_order_relaxed); }

private:
    bool isAcknowledged (std::uint32_t id) const noexcept
    {
        // wrap-safe "acknowledged >= id"
        return static_cast<std::int32_t> (acknowledged.load (std::memory_order_acquire) - id) >= 0;
    }

    std::atomic<std::uint32_t> requested { 0 }, acknowledged { 0 }, heartbeat { 0 };
    std::atomic<std::uint32_t> sequence { 0 }; // odd while a hold is on
    std::atomic<int> holds { 0 };
    std::atomic<int> midiProgram { -1 };
    std::atomic<int> maxWaitMs { 250 }, idleWaitMs { 40 };
    std::uint32_t seen = 0; // audio thread only
};
} // namespace vs::presets
