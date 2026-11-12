#pragma once

// The hand-over between the message thread (which applies presets) and the audio thread (which
// must not be called, blocked or made to wait). Atomics only.
//
// Applying a program has three steps on the message thread (PresetManager):
//   1. beginHold()      the audio thread keeps using the parameter values it already has
//                       (no re-read of the host parameters, no key-mode change handling), so the
//                       170-odd parameter writes of step 3 can never be heard half done;
//   2. requestRelease   the audio thread releases every sounding note (the click-free release that
//                       a key-mode change uses) at the start of its next block and acknowledges;
//                       the message thread waits for that, but only while audio is running (a
//                       heartbeat counter) and never longer than a timeout;
//   3. the parameters are set, then endHold(): the next block reads them all at once.
// If the wait times out (audio stopped, or a block longer than the timeout) the parameters are
// set anyway: the request stays pending and the audio thread honours it at its next block, which
// reads the new parameters in the same block, exactly as a key-mode change does. Nothing waits on
// the audio thread to make progress, and the audio thread never waits for anything.
//
// MIDI Program Change: the audio thread posts the program number; the message thread takes the
// latest one (older requests are overwritten) and applies it.

#include <atomic>
#include <cstdint>

#include <juce_core/juce_core.h>

namespace vs::presets
{
class ApplyGate
{
public:
    // ---- audio thread: lock-free, no allocation ----
    /** Once per block, first thing (also for blocks that do no rendering). */
    void blockStarted() noexcept { heartbeat.fetch_add (1, std::memory_order_relaxed); }
    /** True while a preset is being applied: keep the parameter values of the last block. */
    bool isHolding() const noexcept { return hold.load (std::memory_order_acquire); }
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

    // ---- message thread ----
    void beginHold() noexcept { hold.store (true, std::memory_order_release); }
    void endHold() noexcept { hold.store (false, std::memory_order_release); }

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
    std::atomic<bool> hold { false };
    std::atomic<int> midiProgram { -1 };
    std::atomic<int> maxWaitMs { 250 }, idleWaitMs { 40 };
    std::uint32_t seen = 0; // audio thread only
};
} // namespace vs::presets
