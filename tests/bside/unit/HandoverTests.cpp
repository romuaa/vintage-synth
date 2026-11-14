// The hand-over of a compiled patch program to the audio thread (docs/architecture.md 8.6.7,
// program/ProgramHandover.h): single-thread behaviour and a two-thread stress test. The version that
// also checks "parameters and table are one consistent set" through the real ApplyGate is in
// tests/bside/integration/TablePresetTests.cpp.
#include <catch2/catch_test_macros.hpp>

#include "bside/program/ProgramHandover.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

using namespace vs::bside::program;

namespace
{
/** A program whose every command carries `tag`: a copy that mixes two programs shows two tags. */
Program stamped (std::uint32_t tag)
{
    Program p = emptyProgram();
    p.numCmds = kMaxCommands;
    for (auto& c : p.cmds)
    {
        c.op = static_cast<std::uint8_t> (tag);
        c.u0 = static_cast<std::uint8_t> (tag >> 8);
        c.a = static_cast<std::int16_t> (tag);
        c.b = static_cast<std::int16_t> (tag >> 16);
        c.c = static_cast<std::int16_t> (tag * 7u);
    }
    return p;
}

/** The tag of a stamped program (0 for the initial empty program), or -1 if its commands do not all
    agree (a torn or mixed copy). */
long long tagOf (const Program& p)
{
    if (p == emptyProgram())
        return 0;
    const auto& c0 = p.cmds[0];
    const std::uint32_t tag = static_cast<std::uint32_t> (c0.op) | static_cast<std::uint32_t> (c0.u0) << 8
                              | static_cast<std::uint32_t> (static_cast<std::uint16_t> (c0.b)) << 16;
    const Program expected = stamped (tag);
    return std::memcmp (&expected, &p, sizeof (Program)) == 0 ? static_cast<long long> (tag) : -1;
}
} // namespace

TEST_CASE ("handover: starts with the empty program, published and in use", "[bside][handover]")
{
    auto h = std::make_unique<ProgramHandover>();
    CHECK (h->inUse() == emptyProgram());
    CHECK (h->inUseGeneration() == 0);
    CHECK (h->publishes() == 0);
    const Program* p = h->beginAcquire();
    REQUIRE (p != nullptr);
    CHECK (*p == emptyProgram());
    h->commit();
    CHECK (h->inUse() == emptyProgram());
}

TEST_CASE ("handover: a published program becomes the one in use at the next commit, not before", "[bside][handover]")
{
    auto h = std::make_unique<ProgramHandover>();
    h->publish (stamped (11));
    CHECK (h->publishes() == 1);
    CHECK (tagOf (h->inUse()) == 0); // still the empty program
    CHECK (h->inUse() == emptyProgram());

    const Program* p = h->beginAcquire();
    REQUIRE (p != nullptr);
    CHECK (tagOf (*p) == 11);
    CHECK (h->inUse() == emptyProgram()); // claimed, not committed
    h->commit();
    CHECK (tagOf (h->inUse()) == 11);
    CHECK (h->inUseGeneration() == 1);

    // Nothing new: the same table again, commit changes nothing.
    p = h->beginAcquire();
    REQUIRE (p != nullptr);
    CHECK (p == &h->inUse());
    h->commit();
    CHECK (tagOf (h->inUse()) == 11);
}

TEST_CASE ("handover: an aborted read keeps the previous table", "[bside][handover]")
{
    auto h = std::make_unique<ProgramHandover>();
    h->publish (stamped (1));
    REQUIRE (h->beginAcquire() != nullptr);
    h->commit();
    h->publish (stamped (2));
    REQUIRE (h->beginAcquire() != nullptr);
    h->abort(); // a hold began: the block keeps the old parameters and the old table
    CHECK (tagOf (h->inUse()) == 1);
    REQUIRE (h->beginAcquire() != nullptr); // the next block gets the newer one
    h->commit();
    CHECK (tagOf (h->inUse()) == 2);
    h->abort(); // abort without a claim is harmless
    h->commit();
    CHECK (tagOf (h->inUse()) == 2);
}

TEST_CASE ("handover: the buffer in use and a claimed buffer are never overwritten, however often the writer publishes", "[bside][handover]")
{
    auto h = std::make_unique<ProgramHandover>();
    h->publish (stamped (100));
    REQUIRE (h->beginAcquire() != nullptr);
    h->commit();
    const Program* inUse = &h->inUse();

    // Publish many times with no audio activity: the in-use buffer keeps its bytes.
    for (std::uint32_t i = 0; i < 50; ++i)
    {
        h->publish (stamped (200 + i));
        REQUIRE (tagOf (*inUse) == 100);
        REQUIRE (&h->inUse() == inUse);
    }
    // Claim the newest, then publish more: the claimed buffer keeps its bytes too, and so does the one in use.
    const Program* claimed = h->beginAcquire();
    REQUIRE (claimed != nullptr);
    const auto claimedTag = tagOf (*claimed);
    REQUIRE (claimedTag == 249);
    for (std::uint32_t i = 0; i < 50; ++i)
    {
        h->publish (stamped (300 + i));
        REQUIRE (tagOf (*claimed) == claimedTag);
        REQUIRE (tagOf (*inUse) == 100);
    }
    h->commit();
    CHECK (&h->inUse() == claimed);
    for (std::uint32_t i = 0; i < 50; ++i)
    {
        h->publish (stamped (400 + i));
        REQUIRE (tagOf (h->inUse()) == claimedTag);
    }
}

TEST_CASE ("handover: with the writer publishing flat out, the reader only ever sees whole programs, in order", "[bside][handover][threads]")
{
    auto h = std::make_unique<ProgramHandover>();
    std::atomic<bool> stop { false };
    std::atomic<std::uint32_t> written { 0 };
    std::thread writer ([&] {
        std::uint32_t tag = 1;
        while (! stop.load (std::memory_order_relaxed))
        {
            h->publish (stamped (tag));
            written.store (tag, std::memory_order_release);
            ++tag;
            if ((tag & 63u) == 0u)
                std::this_thread::yield();
        }
    });

    long long torn = 0, reads = 0, regressions = 0, failedClaims = 0;
    long long lastTag = 0;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds (1500))
    {
        const Program* p = h->beginAcquire();
        if (p == nullptr)
        {
            ++failedClaims;
            continue;
        }
        // Read the claimed program while the writer goes on, as the block does at note-on.
        const long long first = tagOf (*p);
        std::this_thread::yield();
        const long long second = tagOf (*p);
        if (first < 0 || second != first)
            ++torn;
        if (((reads++) & 3) != 0)
        {
            h->commit();
            const long long t = tagOf (h->inUse());
            if (t < 0)
                ++torn;
            if (t != 0 && t < lastTag)
                ++regressions;
            if (t > 0)
                lastTag = t;
        }
        else
            h->abort();
    }
    stop = true;
    writer.join();
    INFO ("reads " << reads << ", failed claims " << failedClaims << ", writer published " << written.load());
    CHECK (torn == 0);
    CHECK (regressions == 0);
    CHECK (reads > 1000);
    CHECK (written.load() > 1000);
}
