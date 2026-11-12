// Arpeggiator: patterns, octaves, Hold, sustain pedal, velocity, "from the next step" semantics,
// random reproducibility. Run on the internal clock: 120 bpm, 48 kHz, 1/16 steps = 6000 samples,
// first step at the first key.
#include <catch2/catch_test_macros.hpp>

#include "SeqTestUtil.h"

#include <map>

using namespace vs::seq;
using namespace vs::seq::test;
using vs::EventType;

namespace
{
constexpr int kStep = 6000; // samples per 1/16 at 120 bpm, 48 kHz

Settings baseArp()
{
    Settings s;
    s.mode = Mode::Arp;
    s.rate = Rate::Sixteenth;
    s.gate = 0.5f;
    s.internalBpm = 120.0;
    return s;
}

struct Played
{
    std::vector<int> notes;
    std::vector<float> velocities;
    std::vector<std::int64_t> times;
    std::vector<NoteEvent> all;
};

Played runInternal (const Settings& s, std::vector<NoteEvent> input, std::int64_t total, int block = 128, std::vector<Change> more = {})
{
    ArpSeq m;
    Run r;
    r.sr = 48000;
    r.total = total;
    r.block = block;
    r.segs = { HostSeg { 0, false, 12000, 0 } };
    r.changes = { Change { 0, s } };
    for (auto& c : more)
        r.changes.push_back (c);
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
            p.times.push_back (e.offset);
        }
    return p;
}

std::vector<NoteEvent> chordAtZero (std::initializer_list<int> notes)
{
    std::vector<NoteEvent> v;
    for (int n : notes)
        v.push_back (noteOn (0, n));
    return v;
}

std::vector<int> first (const std::vector<int>& v, std::size_t n)
{
    return std::vector<int> (v.begin(), v.begin() + static_cast<std::ptrdiff_t> (std::min (n, v.size())));
}

Played runPattern (ArpPattern p, int octaves, std::initializer_list<int> keys, int steps = 24)
{
    Settings s = baseArp();
    s.arpPattern = p;
    s.arpOctaves = octaves;
    return runInternal (s, chordAtZero (keys), static_cast<std::int64_t> (steps) * kStep - 1);
}
} // namespace

TEST_CASE ("Arp Up: ascending notes, repeating", "[seq][arp]")
{
    const auto p = runPattern (ArpPattern::Up, 1, { 67, 60, 64 }, 8);
    CHECK (p.notes == std::vector<int> { 60, 64, 67, 60, 64, 67, 60, 64 });
}

TEST_CASE ("Arp Down: descending notes, repeating", "[seq][arp]")
{
    const auto p = runPattern (ArpPattern::Down, 1, { 67, 60, 64 }, 8);
    CHECK (p.notes == std::vector<int> { 67, 64, 60, 67, 64, 60, 67, 64 });
}

TEST_CASE ("Arp Up & Down: end notes are not repeated", "[seq][arp]")
{
    const auto p = runPattern (ArpPattern::UpDown, 1, { 60, 64, 67 }, 10);
    CHECK (p.notes == std::vector<int> { 60, 64, 67, 64, 60, 64, 67, 64, 60, 64 });
    // Two notes alternate; a single note repeats.
    CHECK (runPattern (ArpPattern::UpDown, 1, { 60, 64 }, 6).notes == std::vector<int> { 60, 64, 60, 64, 60, 64 });
    CHECK (runPattern (ArpPattern::UpDown, 1, { 62 }, 4).notes == std::vector<int> { 62, 62, 62, 62 });
}

TEST_CASE ("Arp As Played: keeps the order the keys were pressed", "[seq][arp]")
{
    const auto p = runPattern (ArpPattern::AsPlayed, 1, { 67, 60, 64 }, 7);
    CHECK (p.notes == std::vector<int> { 67, 60, 64, 67, 60, 64, 67 });
}

TEST_CASE ("Arp octave range repeats the pattern transposed", "[seq][arp]")
{
    SECTION ("Up, 2 octaves")
        CHECK (runPattern (ArpPattern::Up, 2, { 60, 64, 67 }, 8).notes == std::vector<int> { 60, 64, 67, 72, 76, 79, 60, 64 });
    SECTION ("Up, 4 octaves visits 4 x the notes")
    {
        const auto n = runPattern (ArpPattern::Up, 4, { 60, 67 }, 9).notes;
        CHECK (n == std::vector<int> { 60, 67, 72, 79, 84, 91, 96, 103, 60 });
    }
    SECTION ("Down, 2 octaves is the exact reverse")
        CHECK (runPattern (ArpPattern::Down, 2, { 60, 64, 67 }, 7).notes == std::vector<int> { 79, 76, 72, 67, 64, 60, 79 });
    SECTION ("Up & Down, 2 octaves: no repeated end notes")
        CHECK (runPattern (ArpPattern::UpDown, 2, { 60, 64 }, 14).notes
               == std::vector<int> { 60, 64, 72, 76, 72, 64, 60, 64, 72, 76, 72, 64, 60, 64 });
    SECTION ("As Played, 2 octaves: the pressed order, then the same order up an octave")
        CHECK (runPattern (ArpPattern::AsPlayed, 2, { 64, 60, 67 }, 8).notes == std::vector<int> { 64, 60, 67, 76, 72, 79, 64, 60 });
    SECTION ("notes that would pass 127 are left out")
        CHECK (runPattern (ArpPattern::Up, 4, { 120, 125 }, 6).notes == std::vector<int> { 120, 125, 120, 125, 120, 125 });
    SECTION ("a chord wider than an octave: each repeat is the whole chord transposed")
        CHECK (runPattern (ArpPattern::Up, 2, { 60, 76 }, 5).notes == std::vector<int> { 60, 76, 72, 88, 60 });
}

TEST_CASE ("Arp Random: seeded, reproducible, drawn from the pattern, not an ordered pattern", "[seq][arp]")
{
    Settings s = baseArp();
    s.arpPattern = ArpPattern::Random;
    s.arpOctaves = 2;
    s.seed = 1234;
    const auto keys = chordAtZero ({ 60, 62, 64, 65, 67 });
    const auto a = runInternal (s, keys, 200 * kStep);
    const auto b = runInternal (s, keys, 200 * kStep, 7);     // another block size
    const auto c = runInternal (s, keys, 200 * kStep, 4096);
    REQUIRE (a.notes.size() == 200);
    CHECK (a.notes == b.notes);
    CHECK (a.notes == c.notes);

    s.seed = 1235;
    const auto d = runInternal (s, keys, 200 * kStep);
    CHECK (d.notes != a.notes);

    std::map<int, int> hist;
    int ups = 0, downs = 0, repeats = 0;
    for (std::size_t i = 0; i < a.notes.size(); ++i)
    {
        ++hist[a.notes[i]];
        if (i > 0)
        {
            ups += a.notes[i] > a.notes[i - 1] ? 1 : 0;
            downs += a.notes[i] < a.notes[i - 1] ? 1 : 0;
            repeats += a.notes[i] == a.notes[i - 1] ? 1 : 0;
        }
    }
    const std::set<int> pattern { 60, 62, 64, 65, 67, 72, 74, 76, 77, 79 };
    for (const auto& kv : hist)
        CHECK (pattern.count (kv.first) == 1);
    CHECK (hist.size() == 10);                       // every note of both octaves turns up in 200 draws
    CHECK (ups > 40);
    CHECK (downs > 40);                              // not Up, not Down
    CHECK (repeats > 5);                             // a true random pick sometimes repeats
    for (const auto& kv : hist)
        CHECK (kv.second < 40);                      // roughly uniform: expected 20 each
}

TEST_CASE ("Arp Random: a new pattern after Hold restarts the same random sequence", "[seq][arp]")
{
    Settings s = baseArp();
    s.arpPattern = ArpPattern::Random;
    s.seed = 99;
    s.hold = true;
    // First pattern: keys at 0, released at 1000 (latched). Second: after all keys were up, a
    // chord at 36000 (a step boundary); the draws start from the seed again.
    const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64), noteOn (0, 67), noteOff (1000, 60), noteOff (1000, 64), noteOff (1000, 67),
                                      noteOn (36000, 60), noteOn (36000, 64), noteOn (36000, 67) },
                                72 * kStep);
    REQUIRE (p.notes.size() >= 12);
    // Steps at 0..30000 (6 steps) use the first pattern; steps from 36000 the second.
    const std::vector<int> firstSix (p.notes.begin(), p.notes.begin() + 6);
    const std::vector<int> nextSix (p.notes.begin() + 6, p.notes.begin() + 12);
    CHECK (firstSix == nextSix);
}

TEST_CASE ("Arp: velocity is that of the pressed key", "[seq][arp]")
{
    Settings s = baseArp();
    const auto p = runInternal (s, { noteOn (0, 60, 0.25f), noteOn (0, 64, 0.5f), noteOn (0, 67, 1.0f) }, 7 * kStep);
    REQUIRE (p.notes.size() == 7);
    CHECK (p.notes == std::vector<int> { 60, 64, 67, 60, 64, 67, 60 });
    const float expected[] = { 0.25f, 0.5f, 1.0f, 0.25f, 0.5f, 1.0f, 0.25f };
    for (std::size_t i = 0; i < 7; ++i)
        CHECK (std::fabs (p.velocities[i] - expected[i]) < 1.0e-7f);

    // Transposed octaves keep the velocity of the key they come from.
    s.arpOctaves = 2;
    const auto q = runInternal (s, { noteOn (0, 60, 0.25f), noteOn (0, 64, 0.5f) }, 4 * kStep);
    CHECK (q.notes == std::vector<int> { 60, 64, 72, 76 });
    const float expected2[] = { 0.25f, 0.5f, 0.25f, 0.5f };
    for (std::size_t i = 0; i < 4; ++i)
        CHECK (std::fabs (q.velocities[i] - expected2[i]) < 1.0e-7f);
}

TEST_CASE ("Arp: a key added or released between steps changes the pattern from the next step", "[seq][arp]")
{
    Settings s = baseArp();
    SECTION ("added")
    {
        // Steps at 0, 6000, 12000 ... The key 67 goes down at 7000.
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64), noteOn (7000, 67) }, 8 * kStep);
        CHECK (p.notes == std::vector<int> { 60, 64, 67, 60, 64, 67, 60, 64 }); // step 2 (position 2) is already the new key
        CHECK (p.times[2] == 12000);
    }
    SECTION ("a key pressed exactly at a step is part of that step")
    {
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (6000, 64) }, 4 * kStep);
        CHECK (p.notes == std::vector<int> { 60, 64, 60, 64 }); // the step at 6000 already sees both keys
    }
    SECTION ("released")
    {
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64), noteOn (0, 67), noteOff (7000, 64) }, 6 * kStep);
        CHECK (p.notes == std::vector<int> { 60, 64, 60, 67, 60, 67 }); // 64 still played at 6000, gone at 12000
    }
}

TEST_CASE ("Arp: a pattern or octave change at a block boundary acts from the next step", "[seq][arp]")
{
    Settings s = baseArp();
    s.arpPattern = ArpPattern::Up;
    Settings down = s;
    down.arpPattern = ArpPattern::Down;
    // Steps at 0, 6000, 12000, ...; the change lands at 9000: steps 0 and 1 are Up, the rest Down.
    const auto p = runInternal (s, chordAtZero ({ 60, 64, 67 }), 6 * kStep, 128, { Change { 9000, down } });
    // The step position keeps counting across the change: step 2 is position 2 of Down over 3 notes (the lowest),
    // then 67, 64, 60.
    CHECK (p.notes == std::vector<int> { 60, 64, 60, 67, 64, 60 });
}

TEST_CASE ("Arp Hold: notes stay after release; the next key after all were up starts a new pattern", "[seq][arp]")
{
    Settings s = baseArp();
    s.hold = true;
    const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64), noteOff (2000, 60), noteOff (2000, 64),
                                      noteOn (50000, 72) },
                                100000);
    // Latched 60/64 keep playing after the release at 2000; at 50000 the pattern is replaced by 72
    // (the clock keeps running: the next step after 50000 is at 54000).
    std::vector<int> before, after;
    for (std::size_t i = 0; i < p.notes.size(); ++i)
        (p.times[i] < 50000 ? before : after).push_back (p.notes[i]);
    CHECK (first (before, 4) == std::vector<int> { 60, 64, 60, 64 });
    REQUIRE (before.size() == 9); // steps at 0, 6000, ..., 48000
    REQUIRE (! after.empty());
    for (int n : after)
        CHECK (n == 72);
    CHECK (p.times[before.size()] == 54000);
}

TEST_CASE ("Arp Hold: keys added while others are still down join the pattern", "[seq][arp]")
{
    Settings s = baseArp();
    s.hold = true;
    // 60 is still down when 64 arrives: a chord. Then both are released.
    const auto p = runInternal (s, { noteOn (0, 60), noteOn (7000, 64), noteOff (9000, 60), noteOff (9000, 64) }, 8 * kStep);
    CHECK (p.notes == std::vector<int> { 60, 60, 60, 64, 60, 64, 60, 64 });
}

TEST_CASE ("Arp Hold off again: the latched notes are dropped, the sounding gate finishes", "[seq][arp]")
{
    Settings s = baseArp();
    s.hold = true;
    Settings off = s;
    off.hold = false;
    const auto p = runInternal (s, { noteOn (0, 60), noteOff (1000, 60) }, 12 * kStep, 128, { Change { 31000, off } });
    REQUIRE (! p.times.empty());
    CHECK (p.times.back() <= 30000);       // the step at 30000 was the last one
    SoundLedger led;
    for (const auto& e : p.all)
        led.feed (e);
    CHECK (led.sounding.empty());
}

TEST_CASE ("Arp: with the sustain pedal down, released keys stay in the pattern", "[seq][arp]")
{
    Settings s = baseArp();
    const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 64), pedal (100, true), noteOff (200, 60), noteOff (200, 64), pedal (40000, false) },
                                80000);
    // Notes keep going until the pedal goes up at 40000 (last step at 36000, one more would be 42000).
    REQUIRE (! p.times.empty());
    CHECK (p.times.back() == 36000);
    CHECK (p.notes.size() == 7);
    CHECK (first (p.notes, 4) == std::vector<int> { 60, 64, 60, 64 });
}

TEST_CASE ("Arp: pedal pressed before the keys, released keys stay until the pedal goes up", "[seq][arp]")
{
    Settings s = baseArp();
    const auto p = runInternal (s, { pedal (0, true), noteOn (10, 60), noteOff (3000, 60), pedal (20000, false), noteOn (30000, 62) }, 60000);
    // 60 starts at 10 (first key), keeps going on the pedal; pedal up at 20000 drops it (last step 18010).
    std::vector<std::int64_t> t60;
    for (std::size_t i = 0; i < p.notes.size(); ++i)
        if (p.notes[i] == 60)
            t60.push_back (p.times[i]);
    REQUIRE (! t60.empty());
    CHECK (t60.front() == 10);
    CHECK (t60.back() == 18010);
    CHECK (p.notes.back() == 62);
}

TEST_CASE ("Arp: notes of a pattern never overlap and have one note-off each", "[seq][arp]")
{
    Settings s = baseArp();
    s.arpPattern = ArpPattern::UpDown;
    s.arpOctaves = 3;
    s.gate = 1.0f;
    const auto p = runInternal (s, chordAtZero ({ 60, 63, 67 }), 60 * kStep + 100);
    SoundLedger led;
    std::size_t maxSounding = 0;
    for (const auto& e : p.all)
    {
        led.feed (e);
        maxSounding = std::max (maxSounding, led.sounding.size());
    }
    CHECK (led.doubleOns == 0);
    CHECK (led.orphanOffs == 0);
    CHECK (maxSounding == 1);
    CHECK (led.ons == 61);
    CHECK (led.offs >= 60);
}
