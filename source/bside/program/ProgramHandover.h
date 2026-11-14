#pragma once

// The hand-over of a compiled patch program (docs/architecture.md 8.6.7) from the thread that
// applies a preset (message thread, or a host's state thread) to the audio thread, wait-free on the
// audio side, with no allocation and no lock anywhere. JUCE-free; one instance per part.
//
// Four buffers hold a Program each (8 KB; 32 KB in all, built in the object, never allocated again).
// The writer fills a buffer that is neither the published one nor one the audio thread claims, then
// publishes its index. Nothing is ever freed: the "old" table is simply the buffer the next publish
// may reuse, so no thread frees anything while another reads it. (The contract's three buffers are
// not enough once the audio thread has a candidate claim next to its committed one: published,
// committed and candidate can be three different buffers for an instant, and the writer needs a
// fourth. See docs/decisions.md.)
//
// Writer side (one writer at a time: the caller serialises, PresetManager's writer lock does):
//     publish (program)            call it INSIDE the ApplyGate hold that writes the parameters.
//
// Audio side, once per block, inside the seqlock read of ApplyGate.h (the same window in which the
// block copies its parameters):
//     token = gate.beginRead();
//     ... copy the parameters ...
//     const Program* p = handover.beginAcquire();   // the published table, claimed; nullptr = give up
//     bool ok = p != nullptr && gate.endRead (token);
//     if (ok) handover.commit();  else if (p != nullptr) handover.abort();
//     // ok: parameters and table are one consistent set; the table stays valid (never written)
//     //     until the next commit, i.e. for this whole block, and `inUse()` returns it afterwards.
//     // not ok: the block keeps the previous parameters, and inUse() is still the previous table.
// A voice copies the program at note-on (memcpy of sizeof (Program)), from inUse().
//
// Why the claim is safe (all the atomics below are sequentially consistent). The audio thread stores
// candidate = i and then checks published == i. If the check holds, then in the total order the check
// came before any later publish, and the writer reads `candidate` only after its own previous publish
// store and before choosing a buffer, so a writer that could pick i must read the claim first: it
// sees i and picks another buffer. commit() stores inUse before it clears the claim, and the writer
// reads the claim before inUse, so the buffer is protected without a gap. When the published buffer is the committed one already (the common case,
// no table change) the audio thread claims nothing and reads one atomic.
//
// Memory order of the contents: the writer's copy into the buffer happens before its publish
// store; the audio thread's load of `published` (seq_cst, so acquire) makes the copy visible. The
// audio thread's reads of a buffer happen before the commit that stops protecting it, and the writer
// reuses a buffer only after it loaded a protection state without it, so those reads cannot overlap
// the writer's next copy.

#include "bside/program/Program.h"

#include <atomic>
#include <cstdint>

namespace vs::bside::program
{
class ProgramHandover
{
public:
    static constexpr int kBuffers = 4;

    /** Every buffer holds the program of an empty table; that one is published and in use. */
    ProgramHandover() noexcept
    {
        const Program empty = emptyProgram();
        for (auto& b : buffers)
            b = empty;
    }

    ProgramHandover (const ProgramHandover&) = delete;
    ProgramHandover& operator= (const ProgramHandover&) = delete;

    // ---- Writer (not the audio thread; one at a time) ----

    /** Makes `program` the published table. Copies it into a free buffer first (8 KB), so it can be
        built anywhere. Call inside the ApplyGate hold of the parameter writes it belongs to. */
    void publish (const Program& program) noexcept
    {
        // The claim is read BEFORE the committed index: commit() stores the index and then ends the claim,
        // so a writer that sees the claim gone sees the committed index too (the other order could read a
        // stale index, then a cleared claim, and find a buffer that is in use "free").
        const int p = published.load (std::memory_order_seq_cst);
        const int c = candidate.load (std::memory_order_seq_cst);
        const int u = inUseIndex.load (std::memory_order_seq_cst);
        int b = 0;
        while (b == p || b == u || b == c)
            ++b; // at most three buffers are excluded, so b < kBuffers
        buffers[b] = program;
        generations[b] = ++lastGeneration;
        published.store (b, std::memory_order_seq_cst);
    }

    /** Number of publishes so far (writer's view; tests). */
    std::uint32_t publishes() const noexcept { return lastGeneration; }

    // ---- Audio thread (wait-free) ----

    /** The published table, claimed so that the writer leaves it alone; nullptr when a publish raced
        with the claim (treat the block's read as failed: the next block gets the newer table). A
        non-null result must be followed by commit() or abort(). */
    const Program* beginAcquire() noexcept
    {
        const int i = published.load (std::memory_order_seq_cst);
        if (i == inUseIndex.load (std::memory_order_relaxed))
        {
            pending = -1; // already protected as the committed buffer
            return &buffers[i];
        }
        candidate.store (i, std::memory_order_seq_cst);
        if (published.load (std::memory_order_seq_cst) != i)
        {
            candidate.store (-1, std::memory_order_seq_cst);
            return nullptr;
        }
        pending = i;
        return &buffers[i];
    }

    /** The block's parameter read succeeded: the table of beginAcquire() is now the one in use. */
    void commit() noexcept
    {
        if (pending >= 0)
        {
            inUseIndex.store (pending, std::memory_order_seq_cst); // protect it before the claim ends
            candidate.store (-1, std::memory_order_seq_cst);
            pending = -1;
        }
    }

    /** The parameter read failed (a hold began): keep the previous table. */
    void abort() noexcept
    {
        if (pending >= 0)
        {
            candidate.store (-1, std::memory_order_seq_cst);
            pending = -1;
        }
    }

    /** The committed table. Valid (and never written) until the next commit(). */
    const Program& inUse() const noexcept { return buffers[inUseIndex.load (std::memory_order_relaxed)]; }
    /** Which publish the committed table is (0 = the initial empty table); for tests and displays. */
    std::uint32_t inUseGeneration() const noexcept { return generations[inUseIndex.load (std::memory_order_relaxed)]; }

private:
    Program buffers[kBuffers];
    std::uint32_t generations[kBuffers] = {};
    std::uint32_t lastGeneration = 0;                  // writer only
    std::atomic<int> published { 0 };                  // written by the writer
    std::atomic<int> inUseIndex { 0 };                 // written by the audio thread
    std::atomic<int> candidate { -1 };                 // written by the audio thread
    int pending = -1;                                  // audio thread only: the claimed index, -1 = none to commit
};
} // namespace vs::bside::program
