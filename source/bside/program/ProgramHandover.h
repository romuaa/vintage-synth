#pragma once

// The hand-over of a compiled patch program (docs/architecture.md 8.6.7) from the thread that
// applies a preset (message thread, or a host's state thread) to the audio thread, wait-free on the
// audio side, with no allocation and no lock anywhere. JUCE-free; one instance per part.
//
// Four buffers hold a Program each (8 KB; 32 KB in all, built in the object, never allocated again).
// The writer fills a buffer that is neither the published one nor one the audio thread protects, then
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
// TWO PARTS IN ONE SEQLOCK WINDOW (S2c implements this; each part has its own ProgramHandover). The
// window is shared, so the outcome is shared: claim both, then decide once.
//     token = gate.beginRead();   ... copy the parameters ...
//     const Program* lower = lowerHandover.beginAcquire();
//     const Program* upper = upperHandover.beginAcquire();
//     const bool ok = lower != nullptr && upper != nullptr && gate.endRead (token);
//     if (ok) { lowerHandover.commit();  upperHandover.commit(); }       // both ...
//     else    { if (lower) lowerHandover.abort();  if (upper) upperHandover.abort(); }  // ... or neither
// A part must never keep a new table while the other keeps its old one, or the block would mix two
// presets. (If beginAcquire returns nullptr nothing was claimed and nothing needs undoing; abort() on
// the other part is the only cleanup.) Tested in tests/bside/integration/TablePresetTests.cpp.
//
// Why the claim is safe (all the atomics below are sequentially consistent). The audio thread stores
// candidate = i and then checks published == i. If the check holds, then in the total order the check
// came before any later publish, and the writer reads `candidate` only after its own previous publish
// store and before choosing a buffer, so a writer that could pick i must read the claim first: it
// sees i and picks another buffer. commit() stores inUse before it clears the claim, and the writer
// reads the claim before inUse, so the buffer is protected without a gap. When the published buffer
// is the committed one already (the common case, no table change) the audio thread claims nothing
// and reads one atomic.
//
// Memory order of the contents: the writer's copy into the buffer happens before its publish
// store; the audio thread's load of `published` (seq_cst, so acquire) makes the copy visible. The
// audio thread's reads of a buffer happen before the commit that stops protecting it, and the writer
// reuses a buffer only after it loaded a protection state without it, so those reads cannot overlap
// the writer's next copy.
//
// Verification: every atomic step and the buffer copy call a Hook (empty and optimised away in the
// shipped type). tests/bside/unit/HandoverModelTests.cpp instantiates the same code with a hook that
// parks the calling thread at each step, and explores every interleaving of a writer and a reader
// (with state-hash pruning), checking that the writer never writes the committed or a claimed buffer,
// that a successful block's table matches its parameters, that the committed buffer keeps its content,
// and that the writer always finds a free buffer. Hook::kMutation selects deliberately broken variants
// (test only; 0 in the shipped type) that the exploration must catch.

#include "bside/program/Program.h"

#include <atomic>
#include <cstdint>

namespace vs::bside::program
{
/** The scheduling points of the protocol (arguments of Hook::before). Test code uses values from kTestStepBase up. */
namespace step
{
constexpr int WriterLoadClaim = 1, WriterLoadInUse = 2, WriterWriteBuffer = 3, WriterPublish = 4;
constexpr int ReaderLoadPublished = 11, ReaderLoadInUse = 12, ReaderStoreClaim = 13, ReaderVerify = 14, ReaderStoreInUse = 15,
              ReaderClearClaim = 16;
constexpr int kTestStepBase = 100;
} // namespace step

/** The hook of the shipped type: nothing. `kMutation` 0 is the real protocol. */
struct NoHook
{
    static constexpr int kMutation = 0;
    void before (int /*step*/, int /*argument*/ = 0) const noexcept {}
    void saw (int /*value*/) const noexcept {}
};

template <class H>
struct HandoverInspector; // test access to the private state

template <class Hook>
class BasicProgramHandover
{
public:
    static constexpr int kBuffers = 4;

    /** Every buffer holds the program of an empty table; that one is published and in use. */
    BasicProgramHandover() noexcept
    {
        const Program empty = emptyProgram();
        for (auto& b : buffers)
            b = empty;
    }

    BasicProgramHandover (const BasicProgramHandover&) = delete;
    BasicProgramHandover& operator= (const BasicProgramHandover&) = delete;

    // ---- Writer (not the audio thread; one at a time) ----

    /** Makes `program` the published table. Copies it into a free buffer first (8 KB), so it can be
        built anywhere. Call inside the ApplyGate hold of the parameter writes it belongs to. Allocates
        nothing and cannot fail. */
    void publish (const Program& program) noexcept
    {
        const int p = published.load (std::memory_order_seq_cst);
        int c = -1, u = -1;
        if constexpr (Hook::kMutation == 3)
        {
            // Broken on purpose: the committed index before the claim (a stale index, then a cleared claim).
            hook.before (step::WriterLoadInUse);
            u = inUseIndex.load (std::memory_order_seq_cst);
            hook.saw (u);
            hook.before (step::WriterLoadClaim);
            c = candidate.load (std::memory_order_seq_cst);
            hook.saw (c);
        }
        else
        {
            // The claim is read BEFORE the committed index: commit() stores the index and then ends the claim,
            // so a writer that sees the claim gone sees the committed index too (the other order could read a
            // stale index, then a cleared claim, and find a buffer that is in use "free").
            hook.before (step::WriterLoadClaim);
            c = candidate.load (std::memory_order_seq_cst);
            hook.saw (c);
            hook.before (step::WriterLoadInUse);
            u = inUseIndex.load (std::memory_order_seq_cst);
            hook.saw (u);
        }
        if constexpr (Hook::kMutation == 4)
            c = -1; // broken on purpose: the claim is ignored
        int b = 0;
        while (b == p || b == u || b == c)
            ++b; // at most three buffers are excluded, so b < kBuffers
        if (b >= kBuffers)
        {
            ++dropped; // cannot happen (checked by the exhaustive test); never write outside the buffers
            return;
        }
        hook.before (step::WriterWriteBuffer, b);
        buffers[b] = program;
        generations[b] = ++lastGeneration;
        hook.before (step::WriterPublish, b);
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
        hook.before (step::ReaderLoadPublished);
        const int i = published.load (std::memory_order_seq_cst);
        hook.saw (i);
        hook.before (step::ReaderLoadInUse);
        const int committed = inUseIndex.load (std::memory_order_relaxed);
        hook.saw (committed);
        if (i == committed)
        {
            pending = -1; // already protected as the committed buffer
            return &buffers[i];
        }
        hook.before (step::ReaderStoreClaim, i);
        candidate.store (i, std::memory_order_seq_cst);
        if constexpr (Hook::kMutation != 2) // broken on purpose when 2: no re-check
        {
            hook.before (step::ReaderVerify);
            const int again = published.load (std::memory_order_seq_cst);
            hook.saw (again);
            if (again != i)
            {
                hook.before (step::ReaderClearClaim);
                candidate.store (-1, std::memory_order_seq_cst);
                return nullptr;
            }
        }
        pending = i;
        return &buffers[i];
    }

    /** The block's parameter read succeeded: the table of beginAcquire() is now the one in use. */
    void commit() noexcept
    {
        if (pending >= 0)
        {
            if constexpr (Hook::kMutation == 1)
            {
                // Broken on purpose: the claim ends before the committed index is stored.
                hook.before (step::ReaderClearClaim);
                candidate.store (-1, std::memory_order_seq_cst);
                hook.before (step::ReaderStoreInUse, pending);
                inUseIndex.store (pending, std::memory_order_seq_cst);
            }
            else
            {
                hook.before (step::ReaderStoreInUse, pending);
                inUseIndex.store (pending, std::memory_order_seq_cst); // protect it before the claim ends
                hook.before (step::ReaderClearClaim);
                candidate.store (-1, std::memory_order_seq_cst);
            }
            pending = -1;
        }
    }

    /** The parameter read failed (a hold began): keep the previous table. */
    void abort() noexcept
    {
        if (pending >= 0)
        {
            hook.before (step::ReaderClearClaim);
            candidate.store (-1, std::memory_order_seq_cst);
            pending = -1;
        }
    }

    /** The committed table. AUDIO THREAD ONLY (it reads the index the audio thread writes without a
        barrier): valid, and never written, until the next commit(). Tests may call it from the thread that
        plays the audio thread's role, or from any thread while no block runs. */
    const Program& inUse() const noexcept { return buffers[inUseIndex.load (std::memory_order_relaxed)]; }
    /** Which publish the committed table is (0 = the initial empty table). Same thread rule as inUse(). */
    std::uint32_t inUseGeneration() const noexcept { return generations[inUseIndex.load (std::memory_order_relaxed)]; }

    Hook& stepHook() noexcept { return hook; }

private:
    friend struct HandoverInspector<Hook>;

    Program buffers[kBuffers];
    std::uint32_t generations[kBuffers] = {};
    std::uint32_t lastGeneration = 0;                  // writer only
    int dropped = 0;                                   // writer only: publishes that found no free buffer (never)
    std::atomic<int> published { 0 };                  // written by the writer
    std::atomic<int> inUseIndex { 0 };                 // written by the audio thread
    std::atomic<int> candidate { -1 };                 // written by the audio thread
    int pending = -1;                                  // audio thread only: the claimed index, -1 = none to commit
    [[no_unique_address]] Hook hook;
};

using ProgramHandover = BasicProgramHandover<NoHook>;
} // namespace vs::bside::program
