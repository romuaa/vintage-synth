// Long seeded random streams: key presses, pedal, panic events, setting changes and transport
// changes (tempo steps, loop jumps, start/stop). Invariants that must hold whatever happens:
//  - every note-on the module emits has exactly one note-off (and none is on twice),
//  - nothing is left sounding after "all keys up + All Notes Off",
//  - offsets are ascending and inside the block,
//  - controllers pass through unchanged, in order, at their offsets,
//  - the output is identical (same events at the same absolute samples) for any block size,
//  - process() does not allocate.
#include <catch2/catch_test_macros.hpp>

#include "AllocationCounter.h"
#include "SeqTestUtil.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <map>

using namespace vs::seq;
using namespace vs::seq::test;
using vs::EventType;

namespace
{
struct Gen
{
    std::uint64_t s;
    explicit Gen (std::uint64_t seed) : s (seed * 2654435761ull + 12345ull) { next(); }
    std::uint32_t next()
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t> (s >> 33);
    }
    int below (int n) { return static_cast<int> (next() % static_cast<std::uint32_t> (n)); }
    bool chance (int percent) { return below (100) < percent; }
};

constexpr Rate kRates[] = { Rate::Quarter, Rate::Eighth, Rate::EighthTriplet, Rate::Sixteenth, Rate::SixteenthTriplet, Rate::ThirtySecond };

Settings randomSettings (Gen& g)
{
    Settings s;
    const int m = g.below (100);
    s.mode = m < 12 ? Mode::Off : (m < 55 ? Mode::Arp : Mode::Seq);
    s.rate = kRates[g.below (6)];
    s.swing = 0.5f + static_cast<float> (g.below (26)) / 100.0f;
    s.gate = 0.05f + static_cast<float> (g.below (96)) / 100.0f;
    s.hold = g.chance (30);
    s.internalBpm = 20.0 + static_cast<double> (g.below (28001)) / 100.0;
    s.arpPattern = static_cast<ArpPattern> (g.below (5));
    s.arpOctaves = 1 + g.below (4);
    s.seed = g.next();
    s.seqLength = 1 + g.below (16);
    for (auto& st : s.seqSteps)
    {
        const int r = g.below (100);
        st.type = r < 50 ? StepType::Note : (r < 75 ? StepType::Rest : StepType::Tie);
        st.semitones = g.below (49) - 24;
        st.velocity = static_cast<float> (g.below (101)) / 100.0f;
    }
    if (g.chance (4)) // out-of-range values the module must clamp
    {
        s.swing = g.chance (50) ? std::nanf ("") : 9.0f;
        s.gate = g.chance (50) ? -3.0f : 40.0f;
        s.arpOctaves = g.below (20) - 5;
        s.seqLength = g.below (40) - 10;
    }
    return s;
}

/** Generates well-formed key traffic (plus the odd stray note-off) and controllers. */
struct EventSource
{
    std::array<bool, 128> held {};
    bool pedalDown = false;
    int heldCount = 0;

    NoteEvent next (Gen& g, int offset)
    {
        for (;;)
        {
            const int r = g.below (100);
            if (r < 34 && heldCount < 6)
            {
                const int n = 36 + g.below(49);
                if (! held[static_cast<std::size_t> (n)])
                {
                    held[static_cast<std::size_t> (n)] = true;
                    ++heldCount;
                    return noteOn (offset, n, 0.1f + static_cast<float> (g.below (91)) / 100.0f);
                }
            }
            else if (r < 62 && heldCount > 0)
            {
                int n = 36 + g.below (49);
                while (! held[static_cast<std::size_t> (n)])
                    n = 36 + (n - 36 + 1) % 49;
                held[static_cast<std::size_t> (n)] = false;
                --heldCount;
                return noteOff (offset, n);
            }
            else if (r < 64)
                return noteOff (offset, 36 + g.below (49)); // stray: may or may not be down
            else if (r < 72)
            {
                pedalDown = ! pedalDown;
                return pedal (offset, pedalDown);
            }
            else if (r < 80)
                return ctrl (offset, EventType::PitchBend, static_cast<float> (g.below (201) - 100) / 100.0f);
            else if (r < 88)
                return ctrl (offset, EventType::ModWheel, static_cast<float> (g.below (101)) / 100.0f);
            else if (r < 91)
            {
                held.fill (false);
                heldCount = 0;
                pedalDown = false;
                return ctrl (offset, EventType::AllNotesOff);
            }
            else if (r < 93)
            {
                held.fill (false);
                heldCount = 0;
                pedalDown = false;
                return ctrl (offset, EventType::AllSoundOff);
            }
            else if (r < 95)
            {
                pedalDown = false;
                return ctrl (offset, EventType::ResetControllers);
            }
            // otherwise draw again
        }
    }
};

struct Scenario
{
    Run run;
    std::vector<std::pair<std::int64_t, Mode>> modeChanges;
    std::vector<NoteEvent> expectedPass; // controllers in input order, absolute offsets
    std::int64_t settleStart = 0;

    Mode modeAt (std::int64_t t) const
    {
        Mode m = Mode::Off;
        for (const auto& c : modeChanges)
            if (c.first <= t)
                m = c.second;
        return m;
    }
};

Scenario makeScenario (std::uint64_t seed, int sr, std::int64_t duration, std::int64_t settle)
{
    Gen g (seed);
    Scenario sc;
    sc.run.sr = sr;
    sc.run.total = duration + settle;
    sc.settleStart = duration;
    sc.run.segs.clear();

    // Settings, transport: changed only at segment boundaries (the same absolute samples for any block size).
    bool playing = g.chance (50);
    std::int64_t bpm100 = 4000 + g.below (20001);
    std::int64_t anchor = static_cast<std::int64_t> (g.below (6144 * 64));
    std::int64_t start = 0;
    while (start < duration)
    {
        const std::int64_t len = 200 + g.below (3000);
        sc.run.segs.push_back (HostSeg { start, playing, bpm100, anchor });
        if (start == 0 || g.chance (14))
        {
            const Settings s = randomSettings (g);
            sc.run.changes.push_back (Change { start, s });
            sc.modeChanges.push_back ({ start, s.mode });
        }
        // The next stretch: continue, change tempo, loop-jump, or toggle the transport.
        const std::int64_t next = start + len;
        const std::int64_t elapsedTicks = len * 6144 * bpm100 / (6000 * static_cast<std::int64_t> (sr));
        anchor += elapsedTicks;
        if (g.chance (6))
        {
            playing = ! playing;
            anchor = static_cast<std::int64_t> (g.below (6144 * 64));
        }
        if (g.chance (15))
            bpm100 = 4000 + g.below (20001);
        if (g.chance (6))
            anchor = static_cast<std::int64_t> (g.below (6144 * 64)); // loop or locate
        start = next;
    }

    // Input events.
    EventSource src;
    std::int64_t t = 0;
    for (;;)
    {
        t += g.chance (25) ? 0 : 1 + g.below (1400);
        if (t >= duration)
            break;
        sc.run.input.push_back (src.next (g, static_cast<int> (t)));
    }
    // All keys up, pedal up, All Notes Off, then a settle time with the settings left alone.
    const int s0 = static_cast<int> (duration);
    for (std::size_t n = 0; n < 128; ++n)
        if (src.held[n])
            sc.run.input.push_back (noteOff (s0, static_cast<int> (n)));
    if (src.pedalDown)
        sc.run.input.push_back (pedal (s0, false));
    sc.run.input.push_back (ctrl (s0, EventType::AllNotesOff));

    for (const auto& e : sc.run.input)
        if (e.type != EventType::NoteOn && e.type != EventType::NoteOff)
            sc.expectedPass.push_back (e);
    return sc;
}

struct Verdict
{
    bool ok = true;
    std::string why;
    std::int64_t noteOns = 0;
};

Verdict verify (const Scenario& sc, const RunResult& res)
{
    Verdict v;
    const auto fail = [&v] (const std::string& s) {
        if (v.ok)
        {
            v.ok = false;
            v.why = s;
        }
    };
    if (! res.wellFormed)
        fail ("offsets not ascending / outside the block");

    std::set<int> sounding;
    std::vector<NoteEvent> passed;
    for (const auto& e : res.events)
    {
        switch (e.type)
        {
            case EventType::NoteOn:
                ++v.noteOns;
                if (e.note > 127)
                    fail ("note out of range");
                if (! (e.value > 0.0f && e.value <= 1.0f))
                    fail ("velocity out of (0, 1]");
                if (! sounding.insert (e.note).second)
                    fail ("note-on for a note that already sounds: " + std::to_string (e.note) + " @" + std::to_string (e.offset));
                if (e.offset >= sc.settleStart)
                    fail ("note-on after the final All Notes Off @" + std::to_string (e.offset));
                break;
            case EventType::NoteOff:
                if (sounding.erase (e.note) == 0 && sc.modeAt (e.offset) != Mode::Off)
                    fail ("note-off without note-on outside Off mode: " + std::to_string (e.note) + " @" + std::to_string (e.offset));
                break;
            case EventType::AllNotesOff:
            case EventType::AllSoundOff:
                // Off passes the user's panic on (it releases what the user's keys started downstream); in every
                // other mode the module's own note-offs must already have been emitted before the panic.
                if (sc.modeAt (e.offset) == Mode::Off)
                    sounding.clear();
                else if (! sounding.empty())
                    fail ("panic passed on while the module still owed note-offs @" + std::to_string (e.offset));
                passed.push_back (e);
                break;
            case EventType::PitchBend:
            case EventType::ModWheel:
            case EventType::SustainPedal:
            case EventType::ResetControllers:
            default:
                passed.push_back (e);
                break;
        }
    }
    if (! sounding.empty())
        fail ("notes left sounding at the end: " + std::to_string (sounding.size()));
    if (! sameEvents (passed, sc.expectedPass))
        fail ("controller events were changed, dropped, reordered or moved");
    return v;
}
} // namespace

TEST_CASE ("Invariants on long random streams: matched note-offs, nothing hangs, controllers untouched, offsets sane", "[seq][invariants]")
{
    constexpr int kSr[] = { 44100, 48000, 96000, 192000 };
    std::int64_t totalOns = 0;
    int withHost = 0, withInternal = 0;
    for (int i = 0; i < 60; ++i)
    {
        const int sr = kSr[i % 4];
        const Scenario sc = makeScenario (1000 + static_cast<std::uint64_t> (i), sr, static_cast<std::int64_t> (sr) * 3, sr);
        const int block = (i % 5 == 0) ? 1 : (i % 5 == 1 ? 33 : (i % 5 == 2 ? 480 : (i % 5 == 3 ? 1024 : 8192)));
        Scenario run1 = sc;
        run1.run.block = sr > 48000 && block == 1 ? 2 : block;
        ArpSeq m;
        const RunResult res = run (m, run1.run);
        const Verdict v = verify (sc, res);
        INFO ("scenario " << i << " sr " << sr << " block " << run1.run.block << ": " << v.why);
        CHECK (v.ok);
        totalOns += v.noteOns;
        for (const auto& sg : sc.run.segs)
            (sg.playing ? withHost : withInternal) += 1;
    }
    CHECK (totalOns > 3000); // the streams do produce plenty of notes
    CHECK (withHost > 100);
    CHECK (withInternal > 100);
}

TEST_CASE ("Invariants: identical output (same events at the same absolute samples) for every block size", "[seq][invariants]")
{
    constexpr int kSr[] = { 44100, 48000, 96000 };
    constexpr int kBlocks[] = { 1, 2, 3, 7, 16, 64, 100, 333, 480, 1000, 4096, 50000 };
    std::int64_t comparedEvents = 0;
    for (int i = 0; i < 24; ++i)
    {
        const int sr = kSr[i % 3];
        Scenario sc = makeScenario (5000 + static_cast<std::uint64_t> (i), sr, static_cast<std::int64_t> (sr) * 2, sr / 2);
        sc.run.block = 480;
        ArpSeq ref;
        const RunResult reference = run (ref, sc.run);
        REQUIRE (verify (sc, reference).ok);
        comparedEvents += static_cast<std::int64_t> (reference.events.size());
        for (const int block : kBlocks)
        {
            if (block == 1 && sr > 48000)
                continue; // keeps the runtime down; 2 is the next smallest
            sc.run.block = block;
            ArpSeq m;
            const RunResult res = run (m, sc.run);
            INFO ("scenario " << i << " sr " << sr << " block " << block);
            CHECK (res.wellFormed);
            CHECK (sameEvents (res.events, reference.events));
        }
    }
    CHECK (comparedEvents > 2000);
}

TEST_CASE ("Invariants: settings and transport changed at arbitrary block boundaries never hang a note", "[seq][invariants]")
{
    // Unlike the scenarios above, nothing is aligned here: blocks of random length, settings and the transport changed
    // between any two blocks (so a tempo ramp, a stop in the middle of a gate, a mode change in the middle of a tie ...).
    for (int trial = 0; trial < 40; ++trial)
    {
        Gen g (9000 + static_cast<std::uint64_t> (trial));
        ArpSeq m;
        EventSource src;
        SoundLedger ledger;
        std::vector<NoteEvent> out (ArpSeq::kRecommendedCapacity);
        std::vector<NoteEvent> in;
        double ppq = 0.0;
        double bpm = 120.0;
        bool playing = g.chance (50);
        std::int64_t doubles = 0, orphansOutsideOff = 0, ons = 0;
        Mode mode = Mode::Off;
        const double sr = 48000.0;
        for (int block = 0; block < 1500; ++block)
        {
            const int n = 1 + g.below (700);
            if (g.chance (10))
            {
                const Settings s = randomSettings (g);
                m.setSettings (s);
                mode = m.settings().mode;
            }
            if (g.chance (5))
                playing = ! playing;
            if (g.chance (10))
                bpm = 40.0 + static_cast<double> (g.below (18000)) / 100.0;
            if (g.chance (4))
                ppq = static_cast<double> (g.below (100000)) / 100.0;
            in.clear();
            for (int t = 0; t < n;)
            {
                t += g.chance (50) ? g.below (150) : 0;
                if (t >= n)
                    break;
                in.push_back (src.next (g, t));
                t += g.below (200);
            }
            const Transport tr { sr, bpm, ppq, playing };
            const int count = m.process (in, tr, n, out);
            ppq += static_cast<double> (n) * bpm / (60.0 * sr);
            int prev = 0;
            for (int i = 0; i < count; ++i)
            {
                const NoteEvent& e = out[static_cast<std::size_t> (i)];
                REQUIRE (e.offset >= prev);
                REQUIRE (e.offset < n);
                prev = e.offset;
                if (e.type == EventType::NoteOn)
                    ++ons;
                if (e.type == EventType::NoteOff && ledger.sounding.count (e.note) == 0 && mode != Mode::Off)
                    ++orphansOutsideOff;
                if (e.type == EventType::AllNotesOff || e.type == EventType::AllSoundOff)
                {
                    if (mode == Mode::Off)
                        ledger.sounding.clear();
                    else
                        REQUIRE (ledger.sounding.empty()); // the module's note-offs precede the panic it passes on
                }
                ledger.feed (e);
            }
            doubles = ledger.doubleOns;
        }
        // All keys up, pedal up, All Notes Off: the module must release everything it owns, by itself.
        in.clear();
        for (std::size_t n = 0; n < 128; ++n)
            if (src.held[n])
                in.push_back (noteOff (0, static_cast<int>(n)));
        in.push_back (ctrl (0, EventType::AllNotesOff));
        for (int block = 0; block < 3; ++block)
        {
            const Transport tr { sr, bpm, ppq, playing };
            const int count = m.process (in, tr, 480, out);
            ppq += 480.0 * bpm / (60.0 * sr);
            for (int i = 0; i < count; ++i)
            {
                REQUIRE (out[static_cast<std::size_t> (i)].type != EventType::NoteOn);
                ledger.feed (out[static_cast<std::size_t> (i)]);
            }
            in.clear();
        }
        INFO ("trial " << trial);
        CHECK (doubles == 0);
        CHECK (orphansOutsideOff == 0);
        CHECK (ledger.sounding.empty());
        CHECK (ons > 0);
        CHECK_FALSE (m.isSounding());
    }
}

TEST_CASE ("Invariants: after all keys up and All Notes Off nothing sounds, in every mode, at every phase", "[seq][invariants]")
{
    // Panic at many different moments (inside a gate, on a step, on a tie, with Hold and pedal): the module's own
    // note-off must come with it and nothing may start afterwards.
    for (const Mode mode : { Mode::Arp, Mode::Seq })
        for (const bool hold : { false, true })
            for (int phase = 0; phase < 60; ++phase)
            {
                Settings s;
                s.mode = mode;
                s.hold = hold;
                s.rate = Rate::Sixteenth;
                s.gate = 1.0f;
                s.internalBpm = 120.0;
                for (std::size_t k = 0; k < 16; ++k)
                    s.seqSteps[k].type = k % 4 == 3 ? StepType::Tie : StepType::Note;
                ArpSeq m;
                m.setSettings (s);
                const int panicAt = 100 + phase * 997;
                Run r;
                r.sr = 48000;
                r.total = panicAt + 48000;
                r.block = 211;
                r.segs = { HostSeg { 0, false, 12000, 0 } };
                r.changes = { Change { 0, s } };
                r.input = { noteOn (0, 60), noteOn (10, 64), pedal (20, phase % 2 == 0), noteOff (3000, 60), noteOff (3001, 64),
                            noteOff (panicAt, 77), ctrl (panicAt, EventType::AllNotesOff) };
                std::sort (r.input.begin(), r.input.end(), [] (const NoteEvent& a, const NoteEvent& b) { return a.offset < b.offset; });
                const RunResult res = run (m, r);
                SoundLedger led;
                int after = 0;
                for (const auto& e : res.events)
                {
                    led.feed (e);
                    if (e.type == EventType::NoteOn && e.offset >= panicAt)
                        ++after;
                }
                INFO ("mode " << static_cast<int> (mode) << " hold " << hold << " phase " << phase);
                CHECK (res.wellFormed);
                CHECK (led.sounding.empty());
                CHECK (led.doubleOns == 0);
                CHECK (after == 0);
            }
}

TEST_CASE ("Real time: process() and setSettings() do not allocate", "[seq][realtime]")
{
#if ! VS_SEQ_COUNT_ALLOCATIONS
    SKIP ("allocation counting uses the macOS malloc_logger hook (not available here or under ASan)");
#else
    constexpr int sr = 48000;
    constexpr int block = 64;
    Scenario sc = makeScenario (424242, sr, static_cast<std::int64_t> (sr) * 20, sr / 4);

    // Everything the loop needs is prepared before counting starts.
    struct Job
    {
        std::size_t begin, end;
        Transport tr;
        int settingsIndex; // -1: none
    };
    std::vector<NoteEvent> rel; // input with block-relative offsets
    std::vector<Job> jobs;
    std::size_t inIdx = 0;
    std::size_t changeIdx = 0;
    for (std::int64_t s = 0; s < sc.run.total; s += block)
    {
        Job j {};
        j.begin = rel.size();
        while (inIdx < sc.run.input.size() && sc.run.input[inIdx].offset < s + block)
        {
            NoteEvent e = sc.run.input[inIdx++];
            e.offset = static_cast<int> (e.offset - s);
            rel.push_back (e);
        }
        j.end = rel.size();
        j.settingsIndex = -1;
        while (changeIdx < sc.run.changes.size() && sc.run.changes[changeIdx].at <= s)
            j.settingsIndex = static_cast<int> (changeIdx++);
        const HostSeg* seg = &sc.run.segs[0];
        for (const auto& sg : sc.run.segs)
            if (sg.start <= s)
                seg = &sg;
        const double bpm = static_cast<double> (seg->bpm100) / 100.0;
        j.tr = Transport { static_cast<double> (sr), bpm,
                           static_cast<double> (seg->anchorTicks) / 6144.0 + static_cast<double> (s - seg->start) * bpm / (60.0 * sr), seg->playing };
        jobs.push_back (j);
    }
    std::vector<NoteEvent> out (ArpSeq::kRecommendedCapacity);
    ArpSeq m;
    {
        // The hook sees both entry points (guards against a silently disabled counter).
        AllocationCounter counter;
        void* raw = std::malloc (64);
        gEscape = raw;
        std::free (raw);
        std::vector<float> probe (4096);
        gEscape = probe.data();
        CHECK (counter.count() >= 2);
    }

    int allocations = 0;
    std::int64_t events = 0;
    {
        AllocationCounter counter;
        for (const auto& j : jobs)
        {
            if (j.settingsIndex >= 0)
                m.setSettings (sc.run.changes[static_cast<std::size_t> (j.settingsIndex)].s);
            events += m.process (std::span<const NoteEvent> (rel.data() + j.begin, j.end - j.begin), j.tr, block, out);
        }
        m.reset();
        allocations = counter.count();
    }
    CHECK (allocations == 0);
    CHECK (events > 600); // the run did real work (notes, controllers, mode changes)
#endif
}
