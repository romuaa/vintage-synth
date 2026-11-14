// Deterministic fuzzing of the four untrusted-input readers (table, trace, reference, spec):
// seeded random and mutated inputs; for every input
//   - the reader returns (no crash, no exception; sanitizers catch out-of-bounds and UB),
//   - it does not run long (a wall-clock cap per input catches a hang or quadratic behaviour),
//   - its memory stays inside the stated bound (a counting operator new, below),
//   - what it accepts is consistent (validate, canonical write, read back gives the same value),
//   - what it rejects has well-formed messages (at most 8, positions >= 1).
// The normal run is short. The long run is hidden: `bside_unit "[.fuzz-long]"`; the number of
// table cases is BS_FUZZ_CASES (default 1,000,000), the other formats get a tenth of it each.

#include "FuzzSupport.h"
#include "RowsPerTick.h"
#include "TableTestUtil.h"

#include "bside/trace/ReferenceFormat.h"
#include "bside/trace/TraceFormat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <new>

// ---- counting allocator for the whole test executable ----
// Replacing the global operator new would hide small under-runs and new/delete mismatches from
// AddressSanitizer, so under ASan it is NOT replaced: the allocation bound is measured only in
// builds without it (the Release and Debug builds of ctest and of the hidden long run); in the
// sanitizer builds the bound assertions pass trivially (the peak reads 0) and ASan does its own job.
#if defined(__SANITIZE_ADDRESS__)
 #define BS_FUZZ_ASAN 1
#elif defined(__has_feature)
 #if __has_feature(address_sanitizer)
  #define BS_FUZZ_ASAN 1
 #endif
#endif
#ifndef BS_FUZZ_ASAN
 #define BS_FUZZ_ASAN 0
#endif

namespace
{
std::atomic<std::size_t> gLive { 0 };
std::atomic<std::size_t> gPeak { 0 };

#if ! BS_FUZZ_ASAN
void* countedAlloc (std::size_t n)
{
    void* p = std::malloc (n + 16);
    if (p == nullptr)
        return nullptr;
    *static_cast<std::size_t*> (p) = n;
    const auto live = gLive.fetch_add (n, std::memory_order_relaxed) + n;
    auto peak = gPeak.load (std::memory_order_relaxed);
    while (live > peak && ! gPeak.compare_exchange_weak (peak, live, std::memory_order_relaxed))
    {
    }
    return static_cast<char*> (p) + 16;
}

void countedFree (void* p) noexcept
{
    if (p == nullptr)
        return;
    auto* base = static_cast<char*> (p) - 16;
    gLive.fetch_sub (*reinterpret_cast<std::size_t*> (base), std::memory_order_relaxed);
    std::free (base);
}
#endif


/** Bytes allocated at the same time, at most, since construction (above what was live before). */
struct AllocScope
{
    std::size_t base;
    AllocScope() : base (gLive.load()) { gPeak.store (base); }
    std::size_t peak() const { return gPeak.load() - base; }
};
} // namespace

#if ! BS_FUZZ_ASAN
void* operator new (std::size_t n)
{
    if (void* p = countedAlloc (n))
        return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n)
{
    if (void* p = countedAlloc (n))
        return p;
    throw std::bad_alloc();
}
void* operator new (std::size_t n, const std::nothrow_t&) noexcept { return countedAlloc (n); }
void* operator new[] (std::size_t n, const std::nothrow_t&) noexcept { return countedAlloc (n); }
void operator delete (void* p) noexcept { countedFree (p); }
void operator delete[] (void* p) noexcept { countedFree (p); }
// The sized forms only where the compiler has sized deallocation on: GCC then insists they are
// replaced as well (-Wsized-deallocation), and where it is off they have no prototype
// (-Wmissing-prototypes).
#if defined(__cpp_sized_deallocation) && __cpp_sized_deallocation >= 201309L
void operator delete (void* p, std::size_t) noexcept { countedFree (p); }
void operator delete[] (void* p, std::size_t) noexcept { countedFree (p); }
#endif
void operator delete (void* p, const std::nothrow_t&) noexcept { countedFree (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { countedFree (p); }
#endif

using namespace bst;
using namespace vs::bside::trace;
namespace prog = vs::bside::program;

namespace
{
constexpr double kMaxMillisecondsPerInput = 250.0;   // a hang or quadratic blow-up shows as seconds
constexpr std::size_t kTableAllocBound = 160 * 1024; // scratch (about 37 KB), 8 messages, the result's strings
std::size_t traceAllocBound (std::size_t inputBytes) { return 32 * 1024 + 8 * inputBytes; }

struct Stats
{
    std::size_t cases = 0, accepted = 0, rejected = 0, largest = 0;
    double maxMs = 0.0;
    std::size_t maxPeak = 0;
    void note (std::size_t inputBytes, double ms, std::size_t peak, bool ok)
    {
        ++cases;
        (ok ? accepted : rejected)++;
        largest = std::max (largest, inputBytes);
        maxMs = std::max (maxMs, ms);
        maxPeak = std::max (maxPeak, peak);
    }
};

std::string describeInput (const std::string& in)
{
    std::string s;
    for (std::size_t i = 0; i < std::min<std::size_t> (in.size(), 240); ++i)
    {
        const auto c = static_cast<unsigned char> (in[i]);
        if (c == '\n') s += "\\n";
        else if (c < 0x20 || c >= 0x7f) { char b[8]; std::snprintf (b, sizeof b, "\\x%02x", c); s += b; }
        else s += static_cast<char> (c);
    }
    return s + (in.size() > 240 ? "..." : "");
}

int countLines (const std::string& s)
{
    int n = 0;
    for (const char c : s)
        n += c == '\n';
    return n + 1;
}

void checkErrors (const vs::bside::text::ErrorList& e, const std::string& input)
{
    CHECK (e.total() >= 1);
    CHECK (static_cast<int> (e.stored().size()) <= 8);
    CHECK (static_cast<int> (e.stored().size()) == std::min (e.total(), 8));
    for (const auto& m : e.stored())
    {
        CHECK (m.line >= 1);
        CHECK (m.line <= countLines (input) + 1);
        CHECK (m.column >= 1);
        CHECK (! m.what.empty());
    }
    CHECK (! e.format().empty());
}

// ---- one case per format ----

void tableCase (const std::string& input, Stats& st)
{
    const auto t0 = std::chrono::steady_clock::now();
    AllocScope scope;
    const auto r = prog::readTable (input);
    const auto peak = scope.peak();
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    st.note (input.size(), ms, peak, r.ok);
    INFO ("input: " << describeInput (input));
    REQUIRE (ms < kMaxMillisecondsPerInput);
    REQUIRE (peak <= kTableAllocBound);
    if (r.ok)
    {
        std::string why;
        REQUIRE (prog::validate (r.program, &why));
        {
            Rng rowsRng (input.size() + 1);
            REQUIRE (maxRowsPerTick (r.program, rowsRng, 3) <= prog::kMaxRowsPerTick);
        }
        std::string text;
        REQUIRE (prog::writeTable (r.program, text));
        const auto again = prog::readTable (text);
        REQUIRE (again.ok);
        REQUIRE (again.program == r.program);
        std::string text2;
        REQUIRE (prog::writeTable (again.program, text2));
        REQUIRE (text2 == text);
        REQUIRE (r.errors.empty());
    }
    else
    {
        REQUIRE (r.program == prog::emptyProgram());
        checkErrors (r.errors, input);
    }
}

/** Hostile Program bytes: whatever validate() accepts must write to text that reads back to the same bytes. */
void programCase (const prog::Program& base, std::uint64_t seed, Stats& st)
{
    Rng rng (seed);
    prog::Program p = base;
    auto* bytes = reinterpret_cast<unsigned char*> (&p);
    const int usedRows = static_cast<int> (base.numRows), usedCmds = static_cast<int> (base.numCmds);
    const std::size_t rowsAt = offsetof (prog::Program, rows), cmdsAt = offsetof (prog::Program, cmds);
    for (int k = 0, n = 1 + static_cast<int> (rng.below (6)); k < n; ++k)
    {
        std::size_t at = 0;
        switch (rng.below (4))
        {
            case 0: at = rng.below (static_cast<std::uint32_t> (rowsAt)); break;
            case 1: at = rowsAt + rng.below (static_cast<std::uint32_t> (std::max (1, usedRows * 4 + 8))); break;
            case 2: at = cmdsAt + rng.below (static_cast<std::uint32_t> (std::max (1, usedCmds * 8 + 16))); break;
            default: at = rng.below (static_cast<std::uint32_t> (sizeof (prog::Program))); break;
        }
        const auto how = rng.below (4);
        bytes[at] = how == 0 ? static_cast<unsigned char> (rng.below (256)) : how == 1 ? static_cast<unsigned char> (bytes[at] + 1)
                    : how == 2 ? static_cast<unsigned char> (bytes[at] - 1) : static_cast<unsigned char> (bytes[at] ^ (1u << rng.below (8)));
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::string why;
    const bool valid = prog::validate (p, &why);
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    st.note (sizeof (p), ms, 0, valid);
    REQUIRE (ms < kMaxMillisecondsPerInput);
    if (! valid)
    {
        REQUIRE (! why.empty());
        std::string out = "x";
        REQUIRE_FALSE (prog::writeTable (p, out));
        REQUIRE (out.empty());
        return;
    }
    std::string text;
    REQUIRE (prog::writeTable (p, text));
    const auto again = prog::readTable (text);
    INFO ("seed " << seed << "\n" << text << again.message());
    REQUIRE (again.ok);
    REQUIRE (again.program == p);
    Rng rowsRng (seed);
    REQUIRE (maxRowsPerTick (p, rowsRng, 3) <= prog::kMaxRowsPerTick);
}

void traceCase (const std::string& input, Stats& st)
{
    const auto t0 = std::chrono::steady_clock::now();
    AllocScope scope;
    const auto r = readTrace (input);
    const auto peak = scope.peak();
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    st.note (input.size(), ms, peak, r.ok);
    INFO ("input: " << describeInput (input));
    REQUIRE (ms < kMaxMillisecondsPerInput);
    REQUIRE (peak <= traceAllocBound (input.size()));
    if (r.ok)
    {
        std::string why;
        REQUIRE (validateTrace (r.trace, &why));
        std::string text;
        REQUIRE (writeTrace (r.trace, text));
        const auto again = readTrace (text);
        REQUIRE (again.ok);
        std::string text2;
        REQUIRE (writeTrace (again.trace, text2));
        REQUIRE (text2 == text);
        std::vector<Checkpoint> s;
        REQUIRE (checkpointSchedule (r.trace, s));
        REQUIRE (s.size() >= 1);
    }
    else
    {
        REQUIRE (r.trace.writes.empty());
        checkErrors (r.errors, input);
    }
}

void referenceCase (const std::string& input, Stats& st)
{
    const auto t0 = std::chrono::steady_clock::now();
    AllocScope scope;
    const auto r = readReference (input);
    const auto peak = scope.peak();
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    st.note (input.size(), ms, peak, r.ok);
    INFO ("input: " << describeInput (input));
    REQUIRE (ms < kMaxMillisecondsPerInput);
    REQUIRE (peak <= traceAllocBound (input.size()));
    if (r.ok)
    {
        std::string text;
        REQUIRE (writeReference (r.reference, text));
        const auto again = readReference (text);
        REQUIRE (again.ok);
        REQUIRE (again.reference.points.size() == r.reference.points.size());
        std::string text2;
        REQUIRE (writeReference (again.reference, text2));
        REQUIRE (text2 == text);
    }
    else
        checkErrors (r.errors, input);
}

void specCase (const std::string& input, Stats& st)
{
    const auto t0 = std::chrono::steady_clock::now();
    AllocScope scope;
    const auto r = readSpec (input);
    const auto peak = scope.peak();
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    st.note (input.size(), ms, peak, r.ok);
    INFO ("input: " << describeInput (input));
    REQUIRE (ms < kMaxMillisecondsPerInput);
    REQUIRE (peak <= traceAllocBound (input.size()));
    if (r.ok)
    {
        std::string text;
        REQUIRE (writeSpec (r.spec, text));
        const auto again = readSpec (text);
        REQUIRE (again.ok);
        std::string text2;
        REQUIRE (writeSpec (again.spec, text2));
        REQUIRE (text2 == text);
    }
    else
        checkErrors (r.errors, input);
}

// ---- corpora ----

std::vector<std::string> tableCorpus()
{
    std::vector<std::string> c = { "", "bside-table 1\n", "bside-table 1\n[wave1]\n", kSnare, kArpeggio, kPulseLead, kFilterBass };
    Rng r (4242);
    for (int i = 0; i < 24; ++i)
        c.push_back (generateTable (r));
    return c;
}

std::string randomTrace (Rng& r)
{
    std::string t = "bside-trace 1\nclock " + std::to_string (r.range (1000, 2000000)) + "\nmodel " + (r.chance (50) ? "A" : "B") + "\n";
    if (r.chance (50))
        t += "check " + std::to_string (r.range (1, 9000)) + "\n";
    std::uint64_t c = 0;
    static const char* hex = "0123456789abcdef";
    for (int k = 0, n = r.range (0, 30); k < n; ++k)
    {
        c += static_cast<std::uint64_t> (r.chance (30) ? 0 : r.range (0, 50000));
        const int reg = r.range (0, 0x18), val = r.range (0, 255);
        t += std::to_string (c) + " " + hex[reg >> 4] + hex[reg & 15] + " " + hex[val >> 4] + hex[val & 15] + "\n";
    }
    return t + "end " + std::to_string (c + static_cast<std::uint64_t> (r.range (0, 99999))) + "\n";
}

std::string randomReference (Rng& r)
{
    Reference ref;
    ref.traceSha256 = std::string (64, 'a');
    ref.adapter = std::string (40, 'b');
    ref.emulator = std::string (40, 'c');
    std::uint64_t c = 0;
    for (int k = 0, n = r.range (0, 6); k < n; ++k)
    {
        c += static_cast<std::uint64_t> (r.range (0, 9000));
        Reference::Point p;
        p.cycle = c;
        for (int f = 0; f < kStateFields; ++f)
            p.state.v[static_cast<std::size_t> (f)] = static_cast<std::uint32_t> (r.next() % (static_cast<std::uint64_t> (kStateFieldMax[f]) + 1));
        ref.points.push_back (p);
    }
    std::string out;
    REQUIRE (writeReference (ref, out));
    return out;
}

std::string randomSpec (Rng& r)
{
    Spec s;
    s.traceSha256 = std::string (64, 'd');
    s.frames = r.range (1, 3);
    for (int f = 0; f < s.frames; ++f)
    {
        s.levelTenthDb.push_back (static_cast<std::int16_t> (r.range (-1200, 300)));
        for (int b = 0; b < kSpecBands; ++b)
            s.bandTenthDb.push_back (static_cast<std::int16_t> (r.range (-1200, 300)));
    }
    if (r.chance (60))
    {
        SpecSegment g;
        g.name = "seg";
        g.endFrame = static_cast<std::uint32_t> (s.frames);
        for (auto& v : g.spectrum)
            v = static_cast<std::int16_t> (r.range (-1200, 300));
        s.segments.push_back (g);
    }
    std::string out;
    REQUIRE (writeSpec (s, out));
    return out;
}

std::vector<std::string> corpusOf (std::string (*make) (Rng&), int n, std::uint64_t seed, const std::string& head)
{
    std::vector<std::string> c = { "", head };
    Rng r (seed);
    for (int i = 0; i < n; ++i)
        c.push_back (make (r));
    return c;
}

/** Runs `cases` inputs: every fourth is a fresh seed, the rest are mutations of the corpus. */
template <class Case>
Stats fuzz (const std::vector<std::string>& corpus, std::uint64_t cases, std::uint64_t seedBase, Case&& one)
{
    Stats st;
    for (std::uint64_t i = 0; i < cases; ++i)
    {
        Rng rng (seedBase + i);
        const auto& base = corpus[rng.below (static_cast<std::uint32_t> (corpus.size()))];
        one (mutate (base, rng, corpus), st);
    }
    return st;
}

void report (const char* what, const Stats& s, double seconds)
{
    std::printf ("fuzz %-9s cases %9zu  accepted %9zu  rejected %9zu  largest input %6zu B  slowest %7.3f ms  peak allocation %7zu B  (%.1f s)\n", what,
                 s.cases, s.accepted, s.rejected, s.largest, s.maxMs, s.maxPeak, seconds);
    std::fflush (stdout);
}

template <class F>
void timedFuzz (const char* what, F&& f)
{
    const auto t0 = std::chrono::steady_clock::now();
    const Stats s = f();
    report (what, s, std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
    CHECK (s.accepted > 0);                                   // the corpus is reached, not only garbage
    CHECK (s.rejected > 0);
}

std::uint64_t casesFromEnv (std::uint64_t fallback)
{
    if (const char* e = std::getenv ("BS_FUZZ_CASES"))
        if (const auto v = std::strtoull (e, nullptr, 10); v > 0)
            return v;
    return fallback;
}

void runAll (std::uint64_t tableCases, std::uint64_t otherCases)
{
    const auto tables = tableCorpus();
    timedFuzz ("table", [&] { return fuzz (tables, tableCases, 1, tableCase); });
    {
        std::vector<prog::Program> programs;
        for (const auto& t : tables)
            if (const auto r = prog::readTable (t); r.ok && r.program.numRows > 0)
                programs.push_back (r.program);
        REQUIRE (! programs.empty());
        timedFuzz ("program", [&] {
            Stats st;
            for (std::uint64_t i = 0; i < otherCases * 2; ++i)
                programCase (programs[i % programs.size()], 600000000 + i, st);
            return st;
        });
    }
    const auto traces = corpusOf (randomTrace, 24, 11, "bside-trace 1\nclock 985248\nmodel A\nend 0\n");
    timedFuzz ("trace", [&] { return fuzz (traces, otherCases, 7000000, traceCase); });
    const auto refs = corpusOf (randomReference, 16, 12, "bside-ref 1\n");
    timedFuzz ("reference", [&] { return fuzz (refs, otherCases, 8000000, referenceCase); });
    const auto specs = corpusOf (randomSpec, 12, 13, "bside-spec 1\n");
    timedFuzz ("spec", [&] { return fuzz (specs, otherCases, 9000000, specCase); });
}
} // namespace

TEST_CASE ("fuzz: seeded random and mutated inputs (short run)", "[bside][fuzz]")
{
    runAll (30000, 4000);
}

TEST_CASE ("fuzz: pathological inputs at the limits", "[bside][fuzz]")
{
    Stats st;
    const std::string head = "bside-table 1\n";
    tableCase (std::string (32768, 'x'), st);
    tableCase (head + std::string (32768 - head.size(), '['), st);
    tableCase (head + repeatLine ("[wave1]", 1023), st);
    tableCase (head + "[wave1]\n" + repeatLine (std::string (199, ','), 150), st);
    tableCase (head + "[wave1]\n" + repeatLine ("gate on, gate on, gate on, gate on, gate on, gate on, gate on, gate on, gate on, gate on, gate on, gate on", 200), st);
    tableCase (head + "[pulse1]\n" + repeatLine ("sweep +9999999999999999999999 x99999999999999999999", 100), st);
    tableCase (head + "[wave1]\n" + repeatLine ("x:", 1000), st);
    tableCase (head + "[wave1]\n" + repeatLine ("pitch =" + std::string (190, '9'), 100), st);
    tableCase (head + "[wave1]\n" + std::string (3, '\xff') + "\n" + repeatLine ("jump a", 400), st);
    tableCase (head + std::string (2000, '\t') + "\n", st);
    tableCase (std::string ("\xef\xbb\xbf") + head, st); // a byte order mark is rejected
    tableCase (head + "[wave1]\n" + repeatLine ("jump x", 64) + "x:\n" + repeatLine ("jump x", 1), st); // 65 rows
    {
        std::string t = head + "[wave1]\n";
        for (int i = 0; i < 64; ++i)
            t += "l" + std::to_string (i % 16) + ":\njump l" + std::to_string ((i + 1) % 16) + "\n";
        tableCase (t, st);
    }
    // A trace and a spec at their sizes.
    traceCase ("bside-trace 1\nclock 985248\nmodel A\ncheck 1\nend 50000\n", st);
    traceCase ("bside-trace 1\nclock 985248\nmodel A\ncheck 1\nend 1099511627776\n", st);
    traceCase ("bside-trace 1\nclock 985248\nmodel A\n" + repeatLine ("0 00 00", 20000) + "end 0\n", st);
    traceCase (std::string (200000, '9'), st);
    specCase ("bside-spec 1\ntrace-sha256 " + std::string (64, 'a') + "\nrate 48000\nbands 23\nframes 100000\n", st);
    specCase (repeatLine ("frame -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1", 3000), st);
    CHECK (st.cases == 19);
    CHECK (st.maxMs < kMaxMillisecondsPerInput);
}

TEST_CASE ("fuzz: the allocation counter itself works", "[bside][fuzz]")
{
    if (BS_FUZZ_ASAN)
    {
        SUCCEED ("not replaced under AddressSanitizer (see the top of this file)");
        return;
    }
    AllocScope scope;
    {
        std::vector<char> v (100000);
        v[0] = 1;
        CHECK (scope.peak() >= 100000);
    }
    CHECK (scope.peak() >= 100000);
    AllocScope again;
    CHECK (again.peak() == 0);
}

TEST_CASE ("fuzz: long run (10^6 table cases; BS_FUZZ_CASES overrides)", "[.fuzz-long]")
{
    const auto n = casesFromEnv (1000000);
    runAll (n, std::max<std::uint64_t> (n / 10, 1));
}
