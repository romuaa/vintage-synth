// Editor -> audio notes (docs/architecture.md 9.3.4, "Playing the keys"): the wait-free queue's
// order, the full-queue rule (the newest note on is dropped, a note off is never lost: every note
// on reserves the slot of its off), writer calls made while the reader is in the middle of a drain
// (single-threaded and exhaustive at emit granularity), and a two-thread run. The outcome is
// checked exactly: no note on is ever delivered for a note that is already on, and at the end the
// audio side's notes equal the writer's notes.

#include <catch2/catch_test_macros.hpp>

#include "common/engine/UiNotes.h"

#include <array>
#include <atomic>
#include <functional>
#include <random>
#include <thread>
#include <utility>
#include <vector>

using namespace vs;

namespace
{
using Drained = std::vector<std::pair<int, int>>; // note, velocity (0 = off)

Drained drainAll (UiNotes& q)
{
    Drained d;
    q.drain ([&d] (int n, int v) noexcept { d.emplace_back (n, v); });
    return d;
}

/** The audio side: which notes sound, and whether an on ever came for a note already on. */
struct Engine
{
    std::array<bool, 128> on {};
    int doubleOns = 0;
    void apply (int n, int v) noexcept
    {
        auto& s = on[static_cast<size_t> (n & 127)];
        if (v > 0 && s)
            ++doubleOns; // an off was lost or came out of order
        s = v > 0;       // an off for a silent note is harmless
    }
};

constexpr int kCap = static_cast<int> (UiNotes::kCapacity);
} // namespace

TEST_CASE ("UiNotes: notes come out in order with their velocity; an empty drain emits nothing", "[ui][notes]")
{
    UiNotes q;
    CHECK (drainAll (q).empty());
    CHECK (q.noteOn (60, 100));
    q.noteOff (60);
    CHECK (q.noteOn (62, 40));
    CHECK (q.noteOn (64, 127));
    q.noteOff (62);
    CHECK (drainAll (q) == Drained { { 60, 100 }, { 60, 0 }, { 62, 40 }, { 64, 127 }, { 62, 0 } });
    CHECK (drainAll (q).empty());
    CHECK (q.isOn (64));
    CHECK_FALSE (q.isOn (62));

    // Out-of-range input is ignored; velocity is clamped to 1..127 (0 would read as an off).
    CHECK_FALSE (q.noteOn (-1, 100));
    CHECK_FALSE (q.noteOn (128, 100));
    CHECK (q.noteOn (10, 0));
    CHECK (q.noteOn (11, 300));
    CHECK (drainAll (q) == Drained { { 10, 1 }, { 11, 127 } });
    // An off for a note this writer does not have on sends nothing; a repeated on releases first.
    q.noteOff (70);
    CHECK (q.noteOn (64, 90));
    CHECK (drainAll (q) == Drained { { 64, 0 }, { 64, 90 } });
}

TEST_CASE ("UiNotes: a full queue drops the newest note on; every note off still fits", "[ui][notes]")
{
    UiNotes q;
    // Each held note owns its slot and the slot of its future off: 32 held notes fill the queue.
    for (int n = 0; n < kCap / 2; ++n)
        REQUIRE (q.noteOn (n, 100));
    CHECK_FALSE (q.noteOn (100, 100));
    CHECK_FALSE (q.isOn (100));
    CHECK (q.droppedNoteOns() == 1);
    q.noteOff (100); // its on was dropped: nothing is sent
    // Every off is queued, in order, behind its on.
    q.allNotesOff();
    CHECK_FALSE (q.anyOn());
    const auto d = drainAll (q);
    REQUIRE (d.size() == static_cast<size_t> (kCap));
    for (int n = 0; n < kCap / 2; ++n)
    {
        CHECK (d[static_cast<size_t> (n)] == std::pair { n, 100 });
        CHECK (d[static_cast<size_t> (kCap / 2 + n)] == std::pair { n, 0 });
    }

    // Press and release pairs fill all 64 slots; then the next on is dropped.
    for (int i = 0; i < kCap / 2; ++i)
    {
        REQUIRE (q.noteOn (40 + i % 12, 90));
        q.noteOff (40 + i % 12);
    }
    CHECK_FALSE (q.noteOn (72, 90));
    CHECK (drainAll (q).size() == static_cast<size_t> (kCap));
    CHECK (q.noteOn (72, 90)); // room again
}

TEST_CASE ("UiNotes: a held note's off arrives even when the queue filled up after its on (the editor closes)", "[ui][notes]")
{
    UiNotes q;
    REQUIRE (q.noteOn (60, 100));
    for (int i = 0; i < 40; ++i) // presses fill the rest; some are dropped
    {
        q.noteOn (40 + i % 12, 90);
        q.noteOff (40 + i % 12);
    }
    q.noteOff (60); // the last writer call ever: the slot was reserved by its on
    Engine e;
    q.drain ([&e] (int n, int v) noexcept { e.apply (n, v); });
    CHECK (e.doubleOns == 0);
    for (bool s : e.on)
        CHECK_FALSE (s);
    CHECK (drainAll (q).empty());
}

TEST_CASE ("UiNotes: discard empties the queue", "[ui][notes]")
{
    UiNotes q;
    for (int n = 0; n < kCap / 2; ++n)
        REQUIRE (q.noteOn (n, 100));
    q.discard();
    CHECK (drainAll (q).empty());
    q.allNotesOff();
    CHECK (q.noteOn (3, 50));
    const auto d = drainAll (q);
    CHECK (d.size() == static_cast<size_t> (kCap / 2 + 1));
    CHECK (d.back() == std::pair { 3, 50 });
}

TEST_CASE ("UiNotes: writer calls in the middle of a drain (a nearly full queue, then press and release)", "[ui][notes]")
{
    // The reader has fixed which slots it drains; the writer (modelled by calls from inside the
    // emit callback) presses 72 into the last free slot and releases it at once. With a side path
    // for offs that the reader collects later in the same drain, the off overtook its own on and
    // the note hung (review of 5a81815).
    UiNotes q;
    for (int i = 0; i < 31; ++i)
    {
        REQUIRE (q.noteOn (40 + i % 12, 100));
        q.noteOff (40 + i % 12);
    }
    REQUIRE (q.noteOn (20, 100)); // 63 entries queued
    Engine e;
    bool injected = false;
    auto apply = [&] (int n, int v) noexcept {
        e.apply (n, v);
        if (! injected)
        {
            injected = true;
            q.noteOn (72, 100); // whether or not it is queued, the end states must agree
            q.noteOff (72);
        }
    };
    q.drain (apply);
    q.drain (apply);
    q.noteOff (20);
    q.drain (apply);
    CHECK (e.doubleOns == 0);
    for (int n = 0; n < 128; ++n)
        CHECK (e.on[static_cast<size_t> (n)] == q.isOn (n));
}

TEST_CASE ("UiNotes: writer calls at every point of a drain, random scripts: the end states always agree", "[ui][notes]")
{
    // Single-threaded and deterministic: the writer's calls are made from the reader's emit
    // callback at random points, and between drains, across queues from empty to full.
    std::mt19937 rng (2026);
    int mismatches = 0, doubleOns = 0;
    std::uint64_t drops = 0;
    for (int trial = 0; trial < 3000; ++trial)
    {
        UiNotes q;
        Engine e;
        auto writerStep = [&] {
            const int n = 60 + static_cast<int> (rng() % 6u);
            if (q.isOn (n))
                q.noteOff (n);
            else
                q.noteOn (n, 1 + static_cast<int> (rng() % 127u));
        };
        const int prefill = static_cast<int> (rng() % 70u);
        for (int i = 0; i < prefill; ++i)
            writerStep();
        for (int block = 0; block < 6; ++block)
        {
            const int injectAt = static_cast<int> (rng() % 70u);
            const int calls = 1 + static_cast<int> (rng() % 8u);
            int emitted = 0;
            q.drain ([&] (int n, int v) noexcept {
                e.apply (n, v);
                if (emitted++ == injectAt)
                    for (int c = 0; c < calls; ++c)
                        writerStep();
            });
            const int between = static_cast<int> (rng() % 40u);
            for (int i = 0; i < between; ++i)
                writerStep();
        }
        q.drain ([&] (int n, int v) noexcept { e.apply (n, v); });
        for (int n = 0; n < 128; ++n)
            mismatches += e.on[static_cast<size_t> (n)] != q.isOn (n) ? 1 : 0;
        doubleOns += e.doubleOns;
        drops += q.droppedNoteOns();
        q.allNotesOff();
        q.drain ([&] (int n, int v) noexcept { e.apply (n, v); });
        for (bool s : e.on)
            mismatches += s ? 1 : 0;
    }
    CHECK (drops > 0); // the full-queue rule ran
    CHECK (doubleOns == 0);
    CHECK (mismatches == 0);
}

TEST_CASE ("UiNotes: writer and reader threads; no note on twice without an off, and the end states agree", "[ui][notes]")
{
    // A fixed number of reader cycles; every second one stalls until the writer has made 300 more
    // calls (more than the queue holds), so the full-queue rule runs under real concurrency. The
    // checks do not depend on timing.
    UiNotes q;
    constexpr int kNotes = 12, kCycles = 600;
    std::atomic<bool> readerDone { false };
    std::atomic<std::uint64_t> writerCalls { 0 };
    std::array<bool, kNotes> writerOn {}; // read by the main thread only after join
    std::uint64_t drops = 0;

    std::thread writer ([&] {
        std::mt19937 rng (12345);
        std::uniform_int_distribution<int> pick (0, kNotes - 1);
        while (! readerDone.load (std::memory_order_acquire))
        {
            const int n = pick (rng);
            if (q.isOn (n))
                q.noteOff (n);
            else
                q.noteOn (n, 1 + static_cast<int> (rng() % 127u));
            writerCalls.fetch_add (1, std::memory_order_release);
        }
        for (int n = 0; n < kNotes; ++n)
            writerOn[static_cast<size_t> (n)] = q.isOn (n);
        drops = q.droppedNoteOns();
    });

    Engine e;
    int badNotes = 0;
    auto apply = [&] (int n, int v) noexcept {
        if (n < 0 || n >= kNotes)
            ++badNotes;
        e.apply (n, v);
    };
    for (int cycle = 0; cycle < kCycles; ++cycle)
    {
        if ((cycle & 1) == 0)
        {
            const auto from = writerCalls.load (std::memory_order_acquire);
            while (writerCalls.load (std::memory_order_acquire) - from < 300)
                std::this_thread::yield();
        }
        q.drain (apply);
    }
    readerDone.store (true, std::memory_order_release);
    writer.join();
    q.drain (apply); // the last block after the writer stopped

    INFO ("writer calls " << writerCalls.load() << ", drops " << drops);
    CHECK (drops > 0); // the full-queue rule ran (each stall makes 300 calls into 64 slots)
    CHECK (badNotes == 0);
    CHECK (e.doubleOns == 0);
    for (int n = 0; n < kNotes; ++n)
        CHECK (e.on[static_cast<size_t> (n)] == writerOn[static_cast<size_t> (n)]);

    q.allNotesOff();
    q.drain (apply);
    for (bool s : e.on)
        CHECK_FALSE (s);
}
