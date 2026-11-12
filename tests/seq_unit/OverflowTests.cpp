// Output buffer overflow: owed note-offs are never lost (they are retried in the next call), a note
// the part still holds is not started a second time, one slot is always kept for the All Notes Off
// marker, and Off mode keeps the documented output contract.
#include <catch2/catch_test_macros.hpp>

#include "SeqTestUtil.h"

using namespace vs::seq;
using namespace vs::seq::test;
using vs::EventType;

namespace
{
Transport internalTransport() { return Transport { 48000.0, 120.0, 0.0, false }; }
Transport hostTransport (double ppq) { return Transport { 48000.0, 120.0, ppq, true }; }

Settings arpOn (double gate = 0.5)
{
    Settings s;
    s.mode = Mode::Arp;
    s.rate = Rate::Sixteenth;
    s.gate = gate;
    s.internalBpm = 120.0;
    return s;
}

std::vector<NoteEvent> proc (ArpSeq& m, const std::vector<NoteEvent>& in, int n, const Transport& tr, int capacity)
{
    std::vector<NoteEvent> out (static_cast<std::size_t> (std::max (capacity, 0)));
    const int count = m.process (in, tr, n, out);
    REQUIRE (count >= 0);
    REQUIRE (count <= capacity);
    out.resize (static_cast<std::size_t> (count));
    return out;
}
} // namespace

TEST_CASE ("Overflow repro 1: a note owed when the buffer has no room is released in the next call, and never doubled", "[seq][overflow]")
{
    for (const bool modeToOff : { false, true })
    {
        ArpSeq m;
        m.setSettings (arpOn (1.0));
        StreamChecker chk;
        for (const auto& e : proc (m, { noteOn (0, 60) }, 100, hostTransport (0.0), 64))
            chk.feed (e, Mode::Arp, true);
        REQUIRE (chk.gen.size() == 1);

        // The trigger: a transport start (jump) or a switch to Off flushes the note, but there is no room (capacity 0).
        if (modeToOff)
            m.setSettings (Settings {});
        const Transport tr = modeToOff ? hostTransport (100.0 / 24000.0) : hostTransport (8.0);
        const auto none = proc (m, {}, 100, tr, 0);
        CHECK (none.empty());
        CHECK (m.isSounding()); // still owed

        // Next call (room again): the owed note-off comes first, before anything else, at offset 0.
        const auto out = proc (m, {}, 100, modeToOff ? hostTransport (200.0 / 24000.0) : hostTransport (8.0 + 100.0 / 24000.0), 64);
        REQUIRE (! out.empty());
        CHECK (out[0].type == EventType::NoteOff);
        CHECK (out[0].note == 60);
        CHECK (out[0].offset == 0);
        for (const auto& e : out)
            chk.feed (e, modeToOff ? Mode::Off : Mode::Arp, true);
        INFO (chk.firstError);
        CHECK (chk.firstError.empty()); // in particular no "note-on for a pitch the part already holds"
        if (modeToOff)
            CHECK (chk.gen.empty());
    }
}

TEST_CASE ("Overflow: a note-on of a pitch the part still holds is held back until the release fits", "[seq][overflow]")
{
    ArpSeq m;
    m.setSettings (arpOn (1.0));
    proc (m, { noteOn (0, 60) }, 100, internalTransport(), 64); // 60 sounds
    // A jump flushes it with no room; the same call also hits the next step of the same pitch (1 key): the
    // note-on would double the pitch, so it must not be written while the off is still owed.
    const auto out = proc (m, {}, 6100, hostTransport (8.0), 0);
    CHECK (out.empty());
    CHECK (m.isSounding());
    StreamChecker chk;
    chk.gen.insert (60);
    chk.part.feed (noteOn (0, 60));
    const auto next = proc (m, {}, 6100, hostTransport (8.0 + 6100.0 / 24000.0), 64);
    for (const auto& e : next)
        chk.feed (e, Mode::Arp, true);
    INFO (chk.firstError);
    CHECK (chk.firstError.empty());
}

TEST_CASE ("Overflow repro 2: Off, capacity 4, pedal and notes: nothing hangs and the pedal does not stay down", "[seq][overflow]")
{
    ArpSeq m;
    m.setSettings (Settings {});
    const std::vector<NoteEvent> in { pedal (0, true), noteOn (1, 60), noteOn (2, 62), noteOn (3, 64), pedal (4, false), noteOff (5, 60), noteOff (6, 62) };
    const auto out = proc (m, in, 64, internalTransport(), 4);
    REQUIRE (out.size() <= 4);
    CHECK (out.back().type == EventType::AllNotesOff); // a slot is kept for the marker even against owed events
    CHECK (m.droppedCount() > 0);
    PartModel part;
    for (const auto& e : out)
        part.feed (e);
    // 64 was never started (dropped), 60 and 62 are released by the marker, the pedal is cleared by it.
    CHECK (part.sounding.empty());
    CHECK (! part.pedalDown);
}

TEST_CASE ("Overflow sweep: capacity 0..14 with the buffer exactly full when a note-off is owed", "[seq][overflow]")
{
    // A note sounds (gate 50 %, off due at 3000). The next call carries `bends` pitch-bend events and lasts 4000
    // samples, so the owed note-off competes with them for the buffer.
    for (int capacity = 0; capacity <= 14; ++capacity)
        for (int bends = 0; bends <= 14; ++bends)
        {
            ArpSeq m;
            m.setSettings (arpOn());
            StreamChecker chk;
            for (const auto& e : proc (m, { noteOn (0, 60) }, 100, internalTransport(), 64))
                chk.feed (e, Mode::Arp, true);
            std::vector<NoteEvent> in;
            for (int i = 0; i < bends; ++i)
                in.push_back (ctrl (10 + i, EventType::PitchBend, 0.1f));
            const auto out = proc (m, in, 4000, internalTransport(), capacity);
            int markers = 0, passedBends = 0;
            for (const auto& e : out)
            {
                chk.feed (e, Mode::Arp, false, true);
                markers += e.type == EventType::AllNotesOff ? 1 : 0;
                passedBends += e.type == EventType::PitchBend ? 1 : 0;
            }
            INFO ("capacity " << capacity << " bends " << bends << ": " << chk.firstError);
            CHECK (chk.firstError.empty());
            // A lost controller is always announced by the marker, in its reserved slot.
            if (passedBends < bends && capacity >= 1)
                CHECK (markers == 1);
            if (markers > 0)
                CHECK (out.back().type == EventType::AllNotesOff);
            // The next call with room settles everything: the module's note is released.
            const auto rest = proc (m, {}, 100, internalTransport(), 64);
            for (const auto& e : rest)
                chk.feed (e, Mode::Arp, true, true);
            CHECK (chk.firstError.empty());
            CHECK (chk.gen.empty());
            CHECK (chk.part.sounding.empty());
            CHECK_FALSE (m.isSounding());
        }
}

TEST_CASE ("Overflow: the marker slot is reserved even against owed events", "[seq][overflow]")
{
    // Off mode: with capacity 3 the first two events fit; the third (an owed note-off) must leave the marker slot.
    ArpSeq m;
    m.setSettings (Settings {});
    const auto out = proc (m, { noteOn (1, 60), noteOn (2, 62), noteOff (3, 60), noteOff (4, 62) }, 16, internalTransport(), 3);
    REQUIRE (out.size() == 3);
    CHECK (out[0].type == EventType::NoteOn);
    CHECK (out[1].type == EventType::NoteOn);
    CHECK (out[2].type == EventType::AllNotesOff);
}

TEST_CASE ("Capacity at the documented minimum never drops anything", "[seq][overflow]")
{
    // Dense input, fast steps, mode switches: with minimumCapacity() nothing is dropped or deferred.
    ArpSeq m;
    Settings s = arpOn();
    s.rate = Rate::ThirtySecond;
    s.gate = 1.0;
    s.arpOctaves = 4;
    m.setSettings (s);
    std::uint32_t seed = 3;
    const auto rnd = [&seed] (int n) {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<int> ((seed >> 8) % static_cast<std::uint32_t> (n));
    };
    for (int block = 0; block < 300; ++block)
    {
        if (block % 25 == 0)
        {
            Settings t = s;
            t.mode = static_cast<Mode> (rnd (3));
            m.setSettings (t);
        }
        const int n = 1 + rnd (8192);
        std::vector<NoteEvent> in;
        const int k = rnd (50);
        for (int i = 0; i < k; ++i)
        {
            const int t = rnd (n);
            switch (rnd (4))
            {
                case 0: in.push_back (noteOn (t, 36 + rnd (40))); break;
                case 1: in.push_back (noteOff (t, 36 + rnd (40))); break;
                case 2: in.push_back (ctrl (t, EventType::ModWheel, 0.5f)); break;
                default: in.push_back (pedal (t, rnd (2) == 0)); break;
            }
        }
        std::sort (in.begin(), in.end(), [] (const NoteEvent& a, const NoteEvent& b) { return a.offset < b.offset; });
        const int cap = ArpSeq::minimumCapacity (static_cast<int> (in.size()), ArpSeq::maxStepsIn (n, 48000.0));
        const int before = m.droppedCount();
        proc (m, in, n, hostTransport (static_cast<double> (block) * 3.7), cap);
        INFO ("block " << block << " n " << n << " cap " << cap);
        CHECK (m.droppedCount() == before);
    }
}

TEST_CASE ("Off after on: the flush note-off and the player's events stay ascending and inside the block", "[seq][overflow]")
{
    ArpSeq m;
    m.setSettings (arpOn (1.0));
    proc (m, { noteOn (0, 60) }, 100, hostTransport (0.0), 64);
    m.setSettings (Settings {});
    const auto out = proc (m, { ctrl (-3, EventType::PitchBend, 0.5f), ctrl (200, EventType::ModWheel, 0.5f) }, 100, hostTransport (100.0 / 24000.0), 64);
    REQUIRE (out.size() == 3);
    CHECK (out[0].type == EventType::NoteOff);
    int prev = 0;
    for (const auto& e : out)
    {
        CHECK (e.offset >= prev);
        CHECK (e.offset >= 0);
        CHECK (e.offset <= 99);
        prev = e.offset;
    }
}

TEST_CASE ("The overflow marker's offset is inside the block", "[seq][overflow]")
{
    ArpSeq m;
    m.setSettings (Settings {});
    const auto out = proc (m, { ctrl (10, EventType::ModWheel, 0.1f), ctrl (20, EventType::ModWheel, 0.2f), ctrl (500, EventType::ModWheel, 0.3f) },
                           32, internalTransport(), 2);
    REQUIRE (out.size() == 2);
    CHECK (out.back().type == EventType::AllNotesOff);
    CHECK (out.back().offset >= 0);
    CHECK (out.back().offset <= 31);
}
