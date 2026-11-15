// Editor -> audio notes (docs/architecture.md 9.3.4, "Playing the keys"): the wait-free queue's
// order, the full-queue rules (the newest note on is dropped, a note off is never lost), the reader
// collecting pending offs on its own (the editor closed), and a two-thread run whose outcome is
// checked exactly: no note on is ever delivered twice without an off between, and at the end the
// audio side's notes equal the writer's notes, whatever the timing.

#include <catch2/catch_test_macros.hpp>

#include "common/engine/UiNotes.h"

#include <array>
#include <atomic>
#include <chrono>
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

TEST_CASE ("UiNotes: a full queue drops the newest note on, never a note off", "[ui][notes]")
{
    UiNotes q;
    for (int n = 0; n < static_cast<int> (UiNotes::kCapacity); ++n)
        REQUIRE (q.noteOn (n, 100));
    // Full: the next note on is dropped, and its off sends nothing later.
    CHECK_FALSE (q.noteOn (100, 100));
    CHECK_FALSE (q.isOn (100));
    CHECK (q.droppedNoteOns() == 1);
    q.noteOff (100);
    // An off finds the queue full: it waits in the pending-off mask.
    q.noteOff (5);
    CHECK (q.pendingHas (5));
    CHECK (q.pendedNoteOffs() == 1);
    // The writer's retry finds no room: nothing moves. A note on for a note whose off is pending
    // is refused (an off may never overtake a later on of its note).
    q.retryPendingOffs();
    CHECK (q.pendingHas (5));
    CHECK_FALSE (q.noteOn (5, 100));
    CHECK (q.droppedNoteOns() == 2);

    // The reader drains the ring, then the pending offs.
    auto d = drainAll (q);
    REQUIRE (d.size() == UiNotes::kCapacity + 1);
    for (int n = 0; n < static_cast<int> (UiNotes::kCapacity); ++n)
        CHECK (d[static_cast<size_t> (n)] == std::pair { n, 100 });
    CHECK (d.back() == std::pair { 5, 0 });
    CHECK_FALSE (q.pendingHas (5));
    CHECK (q.noteOn (5, 77));
    CHECK (drainAll (q) == Drained { { 5, 77 } });
}

TEST_CASE ("UiNotes: pending offs reach the reader without any further writer call (the editor closed)", "[ui][notes]")
{
    UiNotes q;
    for (int n = 0; n < static_cast<int> (UiNotes::kCapacity); ++n)
        REQUIRE (q.noteOn (n, 100));
    q.allNotesOff(); // every off finds the ring full: all 64 pend
    CHECK (q.pendedNoteOffs() == UiNotes::kCapacity);
    CHECK_FALSE (q.anyOn());
    // No writer call from here on. One drain delivers the ons, then every off.
    const auto d = drainAll (q);
    REQUIRE (d.size() == 2 * UiNotes::kCapacity);
    std::array<bool, 128> on {};
    for (const auto& [n, v] : d)
        on[static_cast<size_t> (n)] = v > 0;
    for (bool b : on)
        CHECK_FALSE (b);
    CHECK (drainAll (q).empty());
}

TEST_CASE ("UiNotes: discard empties the queue and the pending offs", "[ui][notes]")
{
    UiNotes q;
    for (int n = 0; n < static_cast<int> (UiNotes::kCapacity); ++n)
        REQUIRE (q.noteOn (n, 100));
    q.noteOff (3);
    REQUIRE (q.pendingHas (3));
    q.discard();
    CHECK_FALSE (q.pendingHas (3));
    CHECK (drainAll (q).empty());
    CHECK (q.noteOn (3, 50));
    CHECK (drainAll (q) == Drained { { 3, 50 } });
}

TEST_CASE ("UiNotes: writer and reader threads; no note on twice without an off, and the end states agree", "[ui][notes]")
{
    // The reader sometimes waits until the writer has made many calls, so the ring fills: both the
    // drop rule and the pending-off rule run under real concurrency. The run lasts until each was
    // seen often (bounded by a generous time limit); the checks do not depend on timing.
    UiNotes q;
    constexpr int kNotes = 12;
    std::atomic<bool> stop { false }, writerDone { false };
    std::atomic<std::uint64_t> writerCalls { 0 }, drops { 0 }, pends { 0 };
    std::array<bool, kNotes> writerOn {}; // read by the main thread only after join

    std::thread writer ([&] {
        std::mt19937 rng (12345);
        std::uniform_int_distribution<int> pick (0, kNotes - 1);
        while (! stop.load (std::memory_order_relaxed))
        {
            const int n = pick (rng);
            if (q.isOn (n))
                q.noteOff (n);
            else
                q.noteOn (n, 1 + static_cast<int> (rng() % 127u));
            if ((rng() & 15u) == 0)
                q.retryPendingOffs();
            drops.store (q.droppedNoteOns(), std::memory_order_relaxed);
            pends.store (q.pendedNoteOffs(), std::memory_order_relaxed);
            writerCalls.fetch_add (1, std::memory_order_release);
        }
        for (int n = 0; n < kNotes; ++n)
            writerOn[static_cast<size_t> (n)] = q.isOn (n);
        writerDone.store (true, std::memory_order_release);
    });

    std::array<bool, kNotes> sounding {};
    int doubleOns = 0, badNotes = 0;
    std::uint64_t drains = 0;
    auto apply = [&] (int n, int v) noexcept {
        if (n < 0 || n >= kNotes)
        {
            ++badNotes;
            return;
        }
        auto& s = sounding[static_cast<size_t> (n)];
        if (v > 0 && s)
            ++doubleOns; // an off was lost or overtaken
        s = v > 0;       // a doubled off is harmless (the writer's retry and the reader both sent it)
    };
    std::thread reader ([&] {
        while (! writerDone.load (std::memory_order_acquire))
        {
            if ((drains & 1u) == 0)
            {
                // Stall until the writer made 300 calls: more than the ring holds.
                const auto from = writerCalls.load (std::memory_order_acquire);
                while (writerCalls.load (std::memory_order_acquire) - from < 300 && ! writerDone.load (std::memory_order_acquire))
                    std::this_thread::yield();
            }
            q.drain (apply);
            ++drains;
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);
    while ((drops.load() < 2000 || pends.load() < 2000 || writerCalls.load() < 200000) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    stop.store (true);
    writer.join();
    reader.join();
    q.drain (apply); // the last block after both threads stopped

    INFO ("writer calls " << writerCalls.load() << ", drops " << drops.load() << ", pended offs " << pends.load());
    REQUIRE (drops.load() >= 2000); // the full-queue paths really ran
    REQUIRE (pends.load() >= 2000);
    CHECK (badNotes == 0);
    CHECK (doubleOns == 0);
    for (int n = 0; n < kNotes; ++n)
        CHECK (sounding[static_cast<size_t> (n)] == writerOn[static_cast<size_t> (n)]);

    // Releasing everything leaves nothing sounding (offs that pend are collected by the drain).
    q.allNotesOff();
    q.drain (apply);
    for (bool s : sounding)
        CHECK_FALSE (s);
}
