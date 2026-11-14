// Exhaustive check of the hand-over protocol (program/ProgramHandover.h). The shipped code calls a
// Hook before every atomic step and before the buffer copy; here a hook parks the calling thread, and a
// controller decides, step by step, which of the two threads (a writer applying presets, a reader
// playing the audio thread's part of a block) runs next. Every interleaving is explored (replaying each
// schedule from the start, pruning states already seen: the state is the shared data, both threads'
// positions and everything each thread has read so far, so equal states have equal futures). All the
// atomics of the code are sequentially consistent, which is exactly this interleaving model.
//
// Checked at every state: the writer never writes the committed buffer or one the reader has claimed;
// the committed buffer keeps its content; a block whose seqlock read succeeded has a table that matches
// the parameters it copied; the writer always finds a free buffer. Four deliberately broken variants
// (Hook::kMutation) must each be caught.
#include <catch2/catch_test_macros.hpp>

#include "bside/program/ProgramHandover.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace vs::bside::program;

namespace
{
// ---- cooperative scheduling of two threads ----
struct Sched
{
    std::mutex m;
    std::condition_variable cv;
    bool parked[2] = { false, false }, finished[2] = { false, false };
    int stepId[2] = { 0, 0 }, stepArg[2] = { 0, 0 };
    int granted[2] = { 0, 0 };
    int grant = -1;

    void park (int role, int id, int arg)
    {
        std::unique_lock lk (m);
        stepId[role] = id;
        stepArg[role] = arg;
        parked[role] = true;
        cv.notify_all();
        cv.wait (lk, [&] { return grant == role; });
        grant = -1;
    }
    void finish (int role)
    {
        std::lock_guard lk (m);
        finished[role] = true;
        cv.notify_all();
    }
    void waitIdle()
    {
        std::unique_lock lk (m);
        cv.wait (lk, [&] { return (parked[0] || finished[0]) && (parked[1] || finished[1]); });
    }
    void give (int role)
    {
        std::lock_guard lk (m);
        parked[role] = false;
        ++granted[role];
        grant = role;
        cv.notify_all();
    }
    bool allFinished() const { return finished[0] && finished[1]; }
};

Sched* gSched = nullptr;
thread_local int tlRole = -1;
unsigned long long gHist[2] = { 0, 0 }; // unsigned: it wraps on purpose

template <int Mutation>
struct ModelHook
{
    static constexpr int kMutation = Mutation;
    void before (int id, int arg = 0) const noexcept
    {
        if (tlRole >= 0)
            gSched->park (tlRole, id, arg);
    }
    void saw (int v) const noexcept
    {
        if (tlRole >= 0)
            gHist[tlRole] = gHist[tlRole] * 1000003ULL + static_cast<unsigned long long> (static_cast<long long> (v) + 11);
    }
};

constexpr int kTWriterHold = step::kTestStepBase, kTWriterParam = step::kTestStepBase + 1, kTWriterEnd = step::kTestStepBase + 2;
constexpr int kTReaderToken = step::kTestStepBase + 3, kTReaderParam = step::kTestStepBase + 4, kTReaderEnd = step::kTestStepBase + 5,
              kTReaderContent = step::kTestStepBase + 6, kTReaderContent2 = step::kTestStepBase + 7;

/** A program whose every command carries `tag` (as in HandoverTests.cpp), for the first and last command only. */
Program stamped (std::uint32_t tag)
{
    Program p = emptyProgram();
    p.numCmds = kMaxCommands;
    for (auto& c : p.cmds)
    {
        c.a = static_cast<std::int16_t> (tag);
        c.b = static_cast<std::int16_t> (tag);
    }
    return p;
}
int tagOf (const Program& p) { return p.cmds[0].a == p.cmds[kMaxCommands - 1].a ? p.cmds[0].a : -1; }
} // namespace

namespace vs::bside::program
{
template <int M>
struct HandoverInspector<ModelHook<M>>
{
    using H = BasicProgramHandover<ModelHook<M>>;
    static int published (const H& h) { return h.published.load(); }
    static int inUse (const H& h) { return h.inUseIndex.load(); }
    static int candidate (const H& h) { return h.candidate.load(); }
    static int pending (const H& h) { return h.pending; }
    static int dropped (const H& h) { return h.dropped; }
    static std::uint32_t generation (const H& h, int i) { return h.generations[i]; }
    static std::uint32_t lastGeneration (const H& h) { return h.lastGeneration; }
    static int tag (const H& h, int i) { return tagOf (h.buffers[i]); }
};
} // namespace vs::bside::program

namespace
{
template <int M>
struct World
{
    using H = BasicProgramHandover<ModelHook<M>>;
    using I = HandoverInspector<ModelHook<M>>;
    std::unique_ptr<H> h = std::make_unique<H>();
    std::atomic<int> seq { 0 }, param { 0 };
    std::string violation; // set by thread code
    bool expectedValid = true;
    int expectedTag = 0; // the tag of the committed buffer, recorded as it is committed
    int publishesPerRun = 3, blocksPerRun = 2;
};

template <int M>
void writerBody (World<M>& w)
{
    ModelHook<M> hook;
    for (int k = 1; k <= w.publishesPerRun; ++k)
    {
        const Program program = stamped (static_cast<std::uint32_t> (k));
        hook.before (kTWriterHold);
        w.seq.fetch_add (1); // odd: a hold
        hook.before (kTWriterParam);
        w.param.store (k);
        w.h->publish (program);
        hook.before (kTWriterEnd);
        w.seq.fetch_add (1); // even again
    }
}

template <int M>
void readerBody (World<M>& w)
{
    ModelHook<M> hook;
    for (int block = 0; block < w.blocksPerRun; ++block)
    {
        hook.before (kTReaderToken);
        const int token = w.seq.load();
        hook.saw (token);
        bool ok = false;
        const Program* p = nullptr;
        int v = 0;
        if ((token & 1) == 0)
        {
            hook.before (kTReaderParam);
            v = w.param.load();
            hook.saw (v);
            p = w.h->beginAcquire();
            if (p != nullptr)
            {
                hook.before (kTReaderEnd);
                ok = w.seq.load() == token;
                hook.saw (ok ? 1 : 0);
                if (ok)
                    w.h->commit();
                else
                    w.h->abort();
            }
        }
        if (ok)
        {
            hook.before (kTReaderContent);
            const int t = tagOf (*p);
            hook.saw (t);
            if (t != v && w.violation.empty())
                w.violation = "a successful block's table (" + std::to_string (t) + ") does not match its parameters (" + std::to_string (v) + ")";
        }
        hook.before (kTReaderContent2);
        const int t2 = tagOf (w.h->inUse());
        hook.saw (t2);
        if (w.expectedValid && t2 != w.expectedTag && w.violation.empty())
            w.violation = "the committed buffer's content changed";
    }
}

struct Outcome
{
    std::string violation;
    unsigned long long key = 0;
    int runnable = 0; // bit per role
    bool terminal = false;
};

template <int M>
Outcome replay (const std::vector<int>& prefix, int publishes, int blocks)
{
    using I = typename World<M>::I;
    Sched sched;
    gSched = &sched;
    gHist[0] = gHist[1] = 0;
    World<M> w;
    w.publishesPerRun = publishes;
    w.blocksPerRun = blocks;
    std::thread writer ([&] { tlRole = 0; writerBody (w); sched.finish (0); });
    std::thread reader ([&] { tlRole = 1; readerBody (w); sched.finish (1); });

    Outcome out;
    const auto& h = *w.h;
    for (size_t n = 0;; ++n)
    {
        sched.waitIdle();
        // Invariants of the state (every thread is parked or finished: nothing moves).
        if (out.violation.empty() && ! w.violation.empty())
            out.violation = w.violation;
        if (out.violation.empty() && I::dropped (h) != 0)
            out.violation = "the writer found no free buffer";
        if (out.violation.empty() && w.expectedValid && I::tag (h, I::inUse (h)) != w.expectedTag)
            out.violation = "the committed buffer was overwritten";
        if (! out.violation.empty())
            break;
        if (sched.allFinished() || n == prefix.size())
        {
            out.terminal = sched.allFinished();
            for (int r = 0; r < 2; ++r)
                if (sched.parked[r])
                    out.runnable |= 1 << r;
            unsigned long long k = 1469598103934665603ULL;
            auto mix = [&] (auto v) { k = (k ^ (static_cast<unsigned long long> (v) + 1000ULL)) * 1099511628211ULL; };
            mix (sched.granted[0]);
            mix (sched.granted[1]);
            mix (gHist[0]);
            mix (gHist[1]);
            mix (I::published (h));
            mix (I::inUse (h));
            mix (I::candidate (h));
            mix (I::pending (h));
            mix (w.seq.load());
            mix (w.param.load());
            mix (I::lastGeneration (h));
            for (int b = 0; b < 4; ++b)
                mix (I::tag (h, b));
            mix (w.expectedTag);
            out.key = k;
            break;
        }
        const int role = prefix[n];
        if (! sched.parked[role])
        {
            out.violation = "internal: the schedule names a thread that cannot run";
            break;
        }
        // Checks that need the step about to run.
        const int id = sched.stepId[role], arg = sched.stepArg[role];
        if (id == step::WriterWriteBuffer && (arg == I::inUse (h) || arg == I::pending (h)))
        {
            out.violation = std::string ("the writer writes buffer ") + std::to_string (arg) + ", which is "
                            + (arg == I::inUse (h) ? "the committed one" : "claimed by the reader");
            break;
        }
        if (id == step::ReaderStoreInUse)
        {
            w.expectedTag = I::tag (h, arg);
            w.expectedValid = true;
        }
        sched.give (role);
    }
    // Let both threads finish, one step at a time (no checks): nothing ever runs concurrently.
    for (;;)
    {
        sched.waitIdle();
        if (sched.allFinished())
            break;
        sched.give (sched.parked[0] ? 0 : 1);
    }
    writer.join();
    reader.join();
    gSched = nullptr;
    return out;
}

struct Result
{
    std::string violation;
    std::vector<int> schedule;
    size_t states = 0, replays = 0, terminals = 0;
};

template <int M>
Result explore (int publishes, int blocks)
{
    Result r;
    std::unordered_set<unsigned long long> seen;
    std::vector<std::vector<int>> work { {} };
    while (! work.empty())
    {
        auto prefix = std::move (work.back());
        work.pop_back();
        const auto out = replay<M> (prefix, publishes, blocks);
        ++r.replays;
        if (! out.violation.empty())
        {
            r.violation = out.violation;
            r.schedule = prefix;
            return r;
        }
        if (! seen.insert (out.key).second)
            continue;
        ++r.states;
        if (out.terminal)
            ++r.terminals;
        for (int role = 0; role < 2; ++role)
            if ((out.runnable & (1 << role)) != 0)
            {
                auto next = prefix;
                next.push_back (role);
                work.push_back (std::move (next));
            }
    }
    return r;
}

template <int M>
Result run (const char* name, int publishes, int blocks)
{
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = explore<M> (publishes, blocks);
    std::printf ("handover model %-34s writer %d publishes, reader %d blocks: %zu states, %zu replays, %zu final states, %.1f s: %s%s\n", name,
                 publishes, blocks, r.states, r.replays, r.terminals, std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count (),
                 r.violation.empty() ? "no violation" : "VIOLATION: ", r.violation.c_str());
    return r;
}
} // namespace

TEST_CASE ("handover model: the real protocol holds in every interleaving", "[bside][handover][model]")
{
    const auto r = run<0> ("the protocol as shipped", 3, 2);
    CHECK (r.violation.empty());
    CHECK (r.terminals > 10); // runs to the end in many different ways
    CHECK (r.states > 500);
}

TEST_CASE ("handover model: commit() that clears the claim before storing the committed index is caught", "[bside][handover][model]")
{
    const auto r = run<1> ("mutant 1: claim cleared first", 3, 2);
    CHECK (r.violation.find ("writer writes buffer") != std::string::npos);
}

TEST_CASE ("handover model: beginAcquire without the re-check of published is caught", "[bside][handover][model]")
{
    const auto r = run<2> ("mutant 2: no re-check", 3, 2);
    CHECK (r.violation.find ("writer writes buffer") != std::string::npos);
}

TEST_CASE ("handover model: a writer that reads the committed index before the claim is caught", "[bside][handover][model]")
{
    const auto r = run<3> ("mutant 3: in-use read before claim", 3, 2);
    CHECK_FALSE (r.violation.empty());
}

TEST_CASE ("handover model: a writer that ignores the claim is caught", "[bside][handover][model]")
{
    const auto r = run<4> ("mutant 4: claim ignored", 3, 2);
    CHECK (r.violation.find ("writer writes buffer") != std::string::npos);
}
