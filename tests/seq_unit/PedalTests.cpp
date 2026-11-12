// Sustain pedal: while Arp/Seq is on the module consumes it (keeps released keys in the pattern) and
// does not pass it on, so a part that defers note-offs under the pedal cannot pile notes up.
// The part is modelled by PartModel (SeqTestUtil.h): note-offs under the pedal are deferred.
#include <catch2/catch_test_macros.hpp>

#include "SeqTestUtil.h"

using namespace vs::seq;
using namespace vs::seq::test;
using vs::EventType;

namespace
{
Transport internalTransport() { return Transport { 48000.0, 120.0, 0.0, false }; }

Settings arpOn (double gate = 0.5)
{
    Settings s;
    s.mode = Mode::Arp;
    s.rate = Rate::Sixteenth; // 6000 samples at 120 bpm / 48 kHz
    s.gate = gate;
    s.internalBpm = 120.0;
    return s;
}

std::vector<NoteEvent> proc (ArpSeq& m, const std::vector<NoteEvent>& in, int n, int capacity = 512)
{
    std::vector<NoteEvent> out (static_cast<std::size_t> (capacity));
    const int count = m.process (in, internalTransport(), n, out);
    out.resize (static_cast<std::size_t> (count));
    return out;
}
} // namespace

TEST_CASE ("Pedal pile-up: pedal down, 4-key chord, Arp Up over 4 octaves never leaves a note un-released", "[seq][pedal]")
{
    // Before the change the part deferred every arpeggio note-off under the pedal: all 16 notes sounded at once.
    ArpSeq m;
    Settings s = arpOn();
    s.arpOctaves = 4;
    m.setSettings (s);
    Run r;
    r.sr = 48000;
    r.total = 48000 * 6;
    r.block = 256;
    r.segs = { HostSeg { 0, false, 12000, 0 } };
    r.changes = { Change { 0, s } };
    r.input = { pedal (0, true), noteOn (0, 48), noteOn (0, 52), noteOn (0, 55), noteOn (0, 59),
                noteOff (20000, 48), noteOff (20000, 52), noteOff (20000, 55), noteOff (20000, 59) };
    const RunResult res = run (m, r);
    PartModel part;
    std::size_t maxSounding = 0;
    int pedalEvents = 0;
    std::vector<std::int64_t> ons, offs;
    for (const auto& e : res.events)
    {
        part.feed (e);
        maxSounding = std::max (maxSounding, part.sounding.size());
        pedalEvents += e.type == EventType::SustainPedal ? 1 : 0;
        (e.type == EventType::NoteOn ? ons : offs).push_back (e.offset);
        // Gate 50 % of 6000 samples: one step after a note's gate end it must be gone from the part.
        if (e.type == EventType::NoteOn)
            CHECK (part.sounding.size() <= 1 + 0);
    }
    CHECK (pedalEvents == 0);          // the pedal is not passed on
    CHECK (maxSounding == 1);          // never more than the one arpeggio note
    CHECK (ons.size() > 40);           // the arpeggio keeps going on the pedal after the keys were released at 20000
    CHECK (part.sounding.empty());     // nothing left, not even held by a pedal
    CHECK (! part.pedalDown);
    // After the last on, the matching off has come.
    CHECK (ons.size() == offs.size());
}

TEST_CASE ("Pedal: released keys stay in the pattern until the pedal goes up (Arp), and the part never sees the pedal", "[seq][pedal]")
{
    ArpSeq m;
    m.setSettings (arpOn());
    Run r;
    r.sr = 48000;
    r.total = 48000 * 2;
    r.block = 100;
    r.segs = { HostSeg { 0, false, 12000, 0 } };
    r.changes = { Change { 0, arpOn() } };
    r.input = { pedal (0, true), noteOn (10, 60), noteOff (3000, 60), pedal (30000, false) };
    const RunResult res = run (m, r);
    std::vector<std::int64_t> ons;
    for (const auto& e : res.events)
    {
        CHECK (e.type != EventType::SustainPedal);
        if (e.type == EventType::NoteOn)
            ons.push_back (e.offset);
    }
    // 10, 6010, ..., 24010 while the pedal holds the key; the pedal goes up at 30000: last step at 30010 would be after it.
    REQUIRE (ons.size() >= 4);
    CHECK (ons.back() == 24010);
}

TEST_CASE ("Pedal: switching Off -> Arp with the pedal passed on sends a pedal-up first", "[seq][pedal]")
{
    SECTION ("the player released the key but holds the pedal")
    {
        ArpSeq m;
        m.setSettings (Settings {});
        PartModel part;
        for (const auto& e : proc (m, { pedal (5, true), noteOn (10, 60), noteOff (20, 60) }, 100))
            part.feed (e);
        REQUIRE (part.pedalDown);
        REQUIRE (part.sounding.count (60) == 1); // still sounding: held by the part's pedal

        m.setSettings (arpOn());
        const auto out = proc (m, { noteOn (30, 64) }, 100);
        REQUIRE (! out.empty());
        CHECK (out[0].type == EventType::SustainPedal);
        CHECK (out[0].offset == 0);
        CHECK (out[0].value < 0.5f);
        for (const auto& e : out)
            part.feed (e);
        CHECK (! part.pedalDown);
        CHECK (part.heldByPedal.empty());
        // The pedal-held note was released by the pedal-up; what sounds now is the arpeggio's own note (the
        // player's pedal keeps 60 in the pattern), at most one at a time.
        CHECK (part.sounding.size() <= 1);
    }
    SECTION ("the key is still down: the part's note is released, then the arpeggio starts")
    {
        ArpSeq m;
        m.setSettings (Settings {});
        PartModel part;
        for (const auto& e : proc (m, { pedal (5, true), noteOn (10, 60) }, 100))
            part.feed (e);
        m.setSettings (arpOn());
        const auto out = proc (m, {}, 100);
        REQUIRE (out.size() == 3);
        CHECK (out[0].type == EventType::SustainPedal);
        CHECK (out[1].type == EventType::NoteOff);
        CHECK (out[2].type == EventType::NoteOn);
        for (const auto& e : out)
            part.feed (e);
        CHECK (! part.pedalDown);
        CHECK (part.sounding == std::set<int> { 60 });
        // And the player's pedal still counts for the arpeggio: releasing the key keeps it in the pattern.
        const auto next = proc (m, { noteOff (10, 60) }, 8000);
        int ons = 0;
        for (const auto& e : next)
        {
            part.feed (e);
            ons += e.type == EventType::NoteOn ? 1 : 0;
            CHECK (e.type != EventType::SustainPedal);
        }
        CHECK (ons >= 1);
        CHECK (part.sounding.size() <= 1);
    }
}

TEST_CASE ("Pedal: switching Arp -> Off sends the player's pedal state after releasing the module's note", "[seq][pedal]")
{
    ArpSeq m;
    m.setSettings (arpOn (1.0));
    PartModel part;
    for (const auto& e : proc (m, { pedal (0, true), noteOn (0, 60) }, 100))
    {
        CHECK (e.type != EventType::SustainPedal);
        part.feed (e);
    }
    REQUIRE (part.sounding == std::set<int> { 60 });
    REQUIRE (! part.pedalDown);

    m.setSettings (Settings {});
    const auto out = proc (m, {}, 100);
    REQUIRE (out.size() == 2);
    // The note-off first: after the pedal-down it would be deferred and the note would hang.
    CHECK (out[0].type == EventType::NoteOff);
    CHECK (out[1].type == EventType::SustainPedal);
    CHECK (out[1].value > 0.5f);
    for (const auto& e : out)
        part.feed (e);
    CHECK (part.sounding.empty());
    CHECK (part.pedalDown);

    // From here on the pedal is the player's: passed on.
    const auto up = proc (m, { pedal (5, false) }, 100);
    REQUIRE (up.size() == 1);
    CHECK (up[0].type == EventType::SustainPedal);
}

TEST_CASE ("Pedal: Reset Controllers is passed on and drops the pedal-kept keys; All Notes Off clears pedal and latch", "[seq][pedal]")
{
    SECTION ("Reset Controllers")
    {
        ArpSeq m;
        m.setSettings (arpOn());
        const auto out = proc (m, { pedal (0, true), noteOn (10, 60), noteOff (100, 60), ctrl (13000, EventType::ResetControllers) }, 40000);
        int resets = 0, ons = 0;
        std::int64_t lastOn = 0;
        for (const auto& e : out)
        {
            resets += e.type == EventType::ResetControllers ? 1 : 0;
            if (e.type == EventType::NoteOn)
            {
                ++ons;
                lastOn = e.offset;
            }
            CHECK (e.type != EventType::SustainPedal);
        }
        CHECK (resets == 1);
        CHECK (ons == 3); // 10, 6010, 12010; nothing after the reset
        CHECK (lastOn == 12010);
    }
    SECTION ("All Notes Off")
    {
        ArpSeq m;
        Settings s = arpOn();
        s.hold = true;
        m.setSettings (s);
        const auto out = proc (m, { pedal (0, true), noteOn (10, 60), noteOff (100, 60), ctrl (13000, EventType::AllNotesOff), noteOn (20000, 72) }, 40000);
        std::vector<int> after;
        for (const auto& e : out)
            if (e.type == EventType::NoteOn && e.offset >= 13000)
                after.push_back (e.note);
        REQUIRE (! after.empty());
        for (int n : after)
            CHECK (n == 72); // the old pattern, its latch and its pedal are gone
    }
}
