// Arpeggiator follow-ups: velocity sanitising, pattern changes that continue after the pitch played
// last, legato overlap at 100 % gate, swing/gate values that floats cannot store exactly.
// Internal clock: 120 bpm, 48 kHz, 1/16 steps = 6000 samples, first step at the first key.
#include <catch2/catch_test_macros.hpp>

#include "SeqTestUtil.h"

#include <cmath>
#include <limits>

using namespace vs::seq;
using namespace vs::seq::test;
using vs::EventType;

namespace
{
constexpr int kStep = 6000;

Settings baseArp()
{
    Settings s;
    s.mode = Mode::Arp;
    s.rate = Rate::Sixteenth;
    s.gate = 0.5;
    s.internalBpm = 120.0;
    return s;
}

struct Played
{
    std::vector<int> notes;
    std::vector<float> velocities;
    std::vector<NoteEvent> all;
};

Played runInternal (const Settings& s, std::vector<NoteEvent> input, std::int64_t total)
{
    ArpSeq m;
    Run r;
    r.sr = 48000;
    r.total = total;
    r.block = 128;
    r.segs = { HostSeg { 0, false, 12000, 0 } };
    r.changes = { Change { 0, s } };
    r.input = std::move (input);
    const RunResult res = run (m, r);
    REQUIRE (res.wellFormed);
    Played p;
    p.all = res.events;
    for (const auto& e : res.events)
        if (e.type == EventType::NoteOn)
        {
            p.notes.push_back (e.note);
            p.velocities.push_back (e.value);
        }
    return p;
}
} // namespace

TEST_CASE ("Arp: velocities from the input are sanitised (NaN, above 1, below 0, 0)", "[seq][arp]")
{
    const float nan = std::nanf ("");
    const auto p = runInternal (baseArp(), { noteOn (0, 60, nan), noteOn (0, 62, 7.0f), noteOn (0, 64, -1.0f), noteOn (0, 65, 0.0f), noteOn (0, 67, 0.5f) }, 5 * kStep);
    REQUIRE (p.notes == std::vector<int> { 60, 62, 64, 65, 67 });
    CHECK (std::fabs (p.velocities[0] - 0.8f) < 1.0e-7f);          // NaN -> default
    CHECK (std::fabs (p.velocities[1] - 1.0f) < 1.0e-7f);          // clamped to 1
    CHECK (std::fabs (p.velocities[2] - 1.0f / 127.0f) < 1.0e-7f); // never negative
    CHECK (std::fabs (p.velocities[3] - 1.0f / 127.0f) < 1.0e-7f); // never 0 (a note-on with velocity 0 reads as a note-off)
    CHECK (std::fabs (p.velocities[4] - 0.5f) < 1.0e-7f);          // ordinary values untouched
    for (float v : p.velocities)
        CHECK ((v > 0.0f && v <= 1.0f));
    const float inf = std::numeric_limits<float>::infinity();
    const auto q = runInternal (baseArp(), { noteOn (0, 60, inf), noteOn (0, 62, -inf) }, 2 * kStep);
    REQUIRE (q.velocities.size() == 2);
    CHECK (std::fabs (q.velocities[0] - 0.8f) < 1.0e-7f);
    CHECK (std::fabs (q.velocities[1] - 0.8f) < 1.0e-7f);
}

TEST_CASE ("Arp: adding or releasing a key goes on after the pitch played last (no repeat, no skip)", "[seq][arp]")
{
    Settings s = baseArp();
    SECTION ("Up: C E G, E just played at step 4, B added: G follows (not E again)")
    {
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64), noteOn (0, 67), noteOn (27000, 71) }, 9 * kStep - 1);
        CHECK (p.notes == std::vector<int> { 60, 64, 67, 60, 64, 67, 71, 60, 64 });
    }
    SECTION ("Up: a key released after it played: the next pitch above")
    {
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64), noteOn (0, 67), noteOn (0, 72), noteOff (13000, 67) }, 8 * kStep - 1);
        CHECK (p.notes == std::vector<int> { 60, 64, 67, 72, 60, 64, 72, 60 });
    }
    SECTION ("Up: the key just played is released: its successor, not the one after")
    {
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64), noteOn (0, 67), noteOff (7000, 64) }, 6 * kStep - 1);
        CHECK (p.notes == std::vector<int> { 60, 64, 67, 60, 67, 60 });
    }
    SECTION ("Down: G E played, B added: C then B")
    {
        s.arpPattern = ArpPattern::Down;
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64), noteOn (0, 67), noteOn (7000, 71) }, 8 * kStep - 1);
        CHECK (p.notes == std::vector<int> { 67, 64, 60, 71, 67, 64, 60, 71 });
    }
    SECTION ("Up & Down: the direction of travel is kept when a key is added on the way down")
    {
        s.arpPattern = ArpPattern::UpDown;
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64), noteOn (0, 67), noteOn (20000, 62) }, 10 * kStep - 1);
        CHECK (p.notes == std::vector<int> { 60, 64, 67, 64, 62, 60, 62, 64, 67, 64 });
    }
    SECTION ("Up & Down: added on the way up")
    {
        s.arpPattern = ArpPattern::UpDown;
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64), noteOn (0, 67), noteOn (7000, 66) }, 8 * kStep - 1);
        CHECK (p.notes == std::vector<int> { 60, 64, 66, 67, 66, 64, 60, 64 });
    }
    SECTION ("As Played: continues after the last played key")
    {
        s.arpPattern = ArpPattern::AsPlayed;
        const auto p = runInternal (s, { noteOn (0, 67), noteOn (0, 60), noteOn (0, 64), noteOn (7000, 72) }, 8 * kStep - 1);
        CHECK (p.notes == std::vector<int> { 67, 60, 64, 72, 67, 60, 64, 72 });
    }
}

TEST_CASE ("Arp legato: with gate 100 % and a different pitch next, the note-on comes before the old note-off", "[seq][arp]")
{
    Settings s = baseArp();
    s.gate = 1.0;
    SECTION ("different pitch: on, then off, at the same offset")
    {
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64) }, 3 * kStep + 100);
        REQUIRE (p.all.size() >= 5);
        CHECK (p.all[0].type == EventType::NoteOn);
        CHECK (p.all[1].type == EventType::NoteOn);
        CHECK (p.all[1].offset == kStep);
        CHECK (p.all[1].note == 64);
        CHECK (p.all[2].type == EventType::NoteOff);
        CHECK (p.all[2].offset == kStep);
        CHECK (p.all[2].note == 60);
        CHECK (p.all[3].type == EventType::NoteOn);
        CHECK (p.all[4].type == EventType::NoteOff);
        CHECK (p.all[4].note == 64);
    }
    SECTION ("same pitch: off, then on (the part must retrigger)")
    {
        const auto p = runInternal (s, { noteOn (0, 60) }, 3 * kStep + 100);
        REQUIRE (p.all.size() >= 5);
        CHECK (p.all[1].type == EventType::NoteOff);
        CHECK (p.all[1].offset == kStep);
        CHECK (p.all[2].type == EventType::NoteOn);
        CHECK (p.all[2].offset == kStep);
    }
    SECTION ("gate just below 100 %: unchanged, the off comes first and earlier")
    {
        s.gate = 0.99;
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64) }, 3 * kStep);
        REQUIRE (p.all.size() >= 4);
        CHECK (p.all[1].type == EventType::NoteOff);
        CHECK (p.all[1].offset < kStep);
        CHECK (p.all[2].type == EventType::NoteOn);
        CHECK (p.all[2].offset == kStep);
    }
    SECTION ("a part sees an overlap of two notes at most, and a pitch is never started twice")
    {
        s.arpPattern = ArpPattern::UpDown;
        s.arpOctaves = 2;
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 62), noteOn (0, 74) }, 40 * kStep);
        StreamChecker chk;
        std::size_t most = 0;
        for (const auto& e : p.all)
        {
            chk.feed (e, Mode::Arp, false);
            most = std::max (most, chk.part.sounding.size());
        }
        CHECK (chk.firstError.empty());
        CHECK (most == 2);
    }
    SECTION ("the pattern empties while a legato note sounds: it ends at its gate end")
    {
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64), noteOff (7000, 60), noteOff (7000, 64) }, 4 * kStep);
        // 60 @0, 64 @6000 (legato off of 60), keys released at 7000: 64's gate (100 %) ends at 12000 with no next step.
        REQUIRE (p.all.size() == 4);
        CHECK (p.all[3].type == EventType::NoteOff);
        CHECK (p.all[3].note == 64);
        CHECK (p.all[3].offset == 2 * kStep);
    }
}

TEST_CASE ("Arp: swing and gate that floats cannot store exactly land on the exact sample", "[seq][arp]")
{
    // 1/16 at 120 bpm / 48 kHz = 6000 samples. Swing 0.6: step 1 starts at 6000 + 0.2 * 6000 = 7200 exactly.
    // Gate 0.3: step 0 ends at 0.3 * 7200 = 2160, step 1 at 7200 + 0.3 * 4800 = 8640. (0.6f is 0.60000002384: one sample late.)
    Settings s = baseArp();
    s.swing = 0.6;
    s.gate = 0.3;
    const auto p = runInternal (s, { noteOn (0, 60) }, 3 * kStep);
    REQUIRE (p.all.size() >= 5);
    CHECK (p.all[0].offset == 0);
    CHECK (p.all[1].offset == 2160);
    CHECK (p.all[2].offset == 7200);
    CHECK (p.all[3].offset == 8640);
    CHECK (p.all[4].offset == 12000);
}
