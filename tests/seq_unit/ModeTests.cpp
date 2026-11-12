// Front-end behaviour that does not depend on the pattern: Off pass-through, controller and
// panic events, the order of events at one sample offset, switching modes and time sources while
// notes sound, output capacity, malformed input, settings clamping.
#include <catch2/catch_test_macros.hpp>

#include "SeqTestUtil.h"

#include <cmath>

using namespace vs::seq;
using namespace vs::seq::test;
using vs::EventType;

namespace
{
Transport internalTransport() { return Transport { 48000.0, 120.0, 0.0, false }; }
Transport hostTransport (double ppq, double bpm = 120.0) { return Transport { 48000.0, bpm, ppq, true }; }

std::vector<NoteEvent> proc (ArpSeq& m, const std::vector<NoteEvent>& in, int n, const Transport& tr = internalTransport(), int capacity = 512)
{
    std::vector<NoteEvent> out (static_cast<std::size_t> (capacity));
    const int count = m.process (in, tr, n, out);
    REQUIRE (count >= 0);
    REQUIRE (count <= capacity);
    out.resize (static_cast<std::size_t> (count));
    return out;
}

Settings arpOn (float gate = 0.5f)
{
    Settings s;
    s.mode = Mode::Arp;
    s.rate = Rate::Sixteenth; // 6000 samples at 120 bpm / 48 kHz
    s.gate = gate;
    s.internalBpm = 120.0;
    return s;
}

Settings seqOn()
{
    Settings s = arpOn();
    s.mode = Mode::Seq;
    return s;
}

Settings offSettings()
{
    Settings s;
    s.mode = Mode::Off;
    return s;
}

bool is (const NoteEvent& e, EventType t, int offset)
{
    return e.type == t && e.offset == offset;
}
bool isNote (const NoteEvent& e, EventType t, int note, int offset)
{
    return e.type == t && e.note == note && e.offset == offset;
}
} // namespace

TEST_CASE ("Off: well-formed events pass through bit-exactly (every type, odd payloads)", "[seq][mode]")
{
    const std::vector<NoteEvent> in {
        ctrl (0, EventType::ResetControllers), pedal (0, true), noteOn (3, 60, 0.5f), ctrl (5, EventType::PitchBend, -0.25f),
        ctrl (7, EventType::AllNotesOff),       ctrl (7, EventType::ModWheel, 0.3f),
        NoteEvent { 9, EventType::NoteOn, 64, std::nanf ("") },  // odd float payloads survive too
        NoteEvent { 12, EventType::NoteOn, 200, 7.0f },          // out-of-range note and velocity
        noteOff (30, 61), noteOff (63, 60),
    };
    for (const Transport& tr : { internalTransport(), hostTransport (3.2) })
    {
        ArpSeq m;
        m.setSettings (offSettings());
        const auto out = proc (m, in, 64, tr);
        INFO ("playing " << tr.playing);
        CHECK (sameEvents (out, in));
    }
}

TEST_CASE ("Off: malformed offsets (before the block, after it, out of order) are moved to keep the output ascending and inside the block", "[seq][mode]")
{
    const std::vector<NoteEvent> in { pedal (-4, true), noteOn (3, 60), ctrl (5, EventType::PitchBend, -0.25f), noteOn (2, 62), noteOff (1000, 60) };
    ArpSeq m;
    m.setSettings (offSettings());
    const auto out = proc (m, in, 64);
    REQUIRE (out.size() == in.size());
    const int expectedOffsets[] = { 0, 3, 5, 5, 63 };
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        CHECK (out[i].type == in[i].type);
        CHECK (out[i].note == in[i].note);
        CHECK (out[i].offset == expectedOffsets[i]);
    }
}

TEST_CASE ("Off is the default and stays bit-exact across many blocks", "[seq][mode]")
{
    ArpSeq m;
    std::vector<NoteEvent> in { noteOn (0, 60), noteOff (30, 60) };
    for (int i = 0; i < 20; ++i)
        CHECK (sameEvents (proc (m, in, 64), in));
}

TEST_CASE ("Arp: pitch bend, mod wheel, reset and panic events pass through unchanged, in order; the sustain pedal is consumed", "[seq][mode]")
{
    ArpSeq m;
    m.setSettings (arpOn());
    const std::vector<NoteEvent> in { ctrl (5, EventType::PitchBend, 0.5f), noteOn (10, 60), ctrl (10, EventType::ModWheel, 0.7f), pedal (20, true),
                                      ctrl (21, EventType::PitchBend, -1.0f), noteOff (30, 60), ctrl (40, EventType::ModWheel, 0.2f), pedal (50, false),
                                      ctrl (60, EventType::ResetControllers), ctrl (70, EventType::AllSoundOff), ctrl (80, EventType::AllNotesOff) };
    const auto out = proc (m, in, 100);
    std::vector<NoteEvent> passed, expected;
    for (const auto& e : out)
        if (e.type != EventType::NoteOn && e.type != EventType::NoteOff)
            passed.push_back (e);
    for (const auto& e : in) // the sustain pedal is consumed while Arp/Seq is on
        if (e.type != EventType::NoteOn && e.type != EventType::NoteOff && e.type != EventType::SustainPedal)
            expected.push_back (e);
    CHECK (sameEvents (passed, expected));
}

TEST_CASE ("Order at one sample offset: note-off, then input events, then the clock step", "[seq][mode]")
{
    // 1/16 steps at 6000 samples, gate 50 %: key 60 on at 0, its off falls due at 3000.
    SECTION ("a controller at the offset of a gate end comes after the note-off")
    {
        ArpSeq m;
        m.setSettings (arpOn());
        const auto out = proc (m, { noteOn (0, 60), ctrl (3000, EventType::PitchBend, 0.5f) }, 4000);
        REQUIRE (out.size() == 3);
        CHECK (isNote (out[0], EventType::NoteOn, 60, 0));
        CHECK (isNote (out[1], EventType::NoteOff, 60, 3000));
        CHECK (is (out[2], EventType::PitchBend, 3000));
    }
    SECTION ("controllers and keys at the step's offset come before the note-on, and the new key is in that step")
    {
        for (const bool bendFirst : { true, false })
        {
            ArpSeq m;
            m.setSettings (arpOn());
            std::vector<NoteEvent> in { noteOn (0, 60) };
            if (bendFirst)
            {
                in.push_back (ctrl (6000, EventType::PitchBend, 0.25f));
                in.push_back (noteOn (6000, 64));
            }
            else
            {
                in.push_back (noteOn (6000, 64));
                in.push_back (ctrl (6000, EventType::PitchBend, 0.25f));
            }
            const auto out = proc (m, in, 7000);
            REQUIRE (out.size() == 4);
            CHECK (isNote (out[0], EventType::NoteOn, 60, 0));
            CHECK (isNote (out[1], EventType::NoteOff, 60, 3000));
            CHECK (is (out[2], EventType::PitchBend, 6000));
            CHECK (isNote (out[3], EventType::NoteOn, 64, 6000)); // position 1 of {60, 64}
        }
    }
}

TEST_CASE ("Panic: All Notes Off releases the module's note first, then passes on; nothing more plays", "[seq][mode]")
{
    for (const EventType panic : { EventType::AllNotesOff, EventType::AllSoundOff })
    {
        ArpSeq m;
        m.setSettings (arpOn (1.0f)); // gate 100 %: the note is still on at 3500
        const auto out = proc (m, { noteOn (0, 60), ctrl (3500, panic), noteOff (4000, 60), noteOn (9000, 62) }, 16000);
        // 60 at 0; panic at 3500 (the module's own off first); the held key was cleared by the panic, so the
        // step at 6000 does not play; 62 starts a new pattern at 9000.
        REQUIRE (out.size() >= 4);
        CHECK (isNote (out[0], EventType::NoteOn, 60, 0));
        CHECK (isNote (out[1], EventType::NoteOff, 60, 3500));
        CHECK (is (out[2], panic, 3500));
        CHECK (isNote (out[3], EventType::NoteOn, 62, 9000));
        for (std::size_t i = 4; i < out.size(); ++i)
            CHECK (out[i].offset >= 9000);
        CHECK (m.isSounding()); // 62 sounds again at the end of the block (step at 15000)
    }
}

TEST_CASE ("Panic clears the Hold latch and the pedal", "[seq][mode]")
{
    Settings s = arpOn();
    s.hold = true;
    ArpSeq m;
    m.setSettings (s);
    const auto out = proc (m, { noteOn (0, 60), noteOff (100, 60), ctrl (13000, EventType::AllNotesOff) }, 40000);
    std::vector<NoteEvent> ons;
    for (const auto& e : out)
        if (e.type == EventType::NoteOn)
            ons.push_back (e);
    REQUIRE (ons.size() == 3); // 0, 6000, 12000, then nothing after the panic
    CHECK (ons.back().offset == 12000);
    SoundLedger led;
    for (const auto& e : out)
        led.feed (e);
    CHECK (led.sounding.empty());
}

TEST_CASE ("Reset Controllers releases the keys that only the pedal holds", "[seq][mode]")
{
    ArpSeq m;
    m.setSettings (arpOn());
    const auto out = proc (m, { noteOn (0, 60), pedal (10, true), noteOff (100, 60), ctrl (13000, EventType::ResetControllers) }, 40000);
    std::vector<std::int64_t> ons;
    for (const auto& e : out)
        if (e.type == EventType::NoteOn)
            ons.push_back (e.offset);
    CHECK (ons == std::vector<std::int64_t> { 0, 6000, 12000 });
    int resets = 0;
    for (const auto& e : out)
        resets += e.type == EventType::ResetControllers ? 1 : 0;
    CHECK (resets == 1);
}

TEST_CASE ("Switching Arp -> Off while a note sounds releases it at the start of the next block", "[seq][mode]")
{
    ArpSeq m;
    m.setSettings (arpOn (1.0f));
    auto out = proc (m, { noteOn (0, 60) }, 100);
    REQUIRE (out.size() == 1);
    CHECK (m.isSounding());

    m.setSettings (offSettings());
    out = proc (m, { ctrl (10, EventType::PitchBend, 0.1f), noteOn (20, 62) }, 100);
    REQUIRE (out.size() == 3);
    CHECK (isNote (out[0], EventType::NoteOff, 60, 0)); // ours, before anything is passed on
    CHECK (is (out[1], EventType::PitchBend, 10));
    CHECK (isNote (out[2], EventType::NoteOn, 62, 20));  // from here on events are the user's own
    CHECK_FALSE (m.isSounding());

    out = proc (m, { noteOff (5, 62) }, 100);
    REQUIRE (out.size() == 1);
    CHECK (isNote (out[0], EventType::NoteOff, 62, 5));
}

TEST_CASE ("Switching Off -> Arp while keys that were passed through are held releases them, then arpeggiates them", "[seq][mode]")
{
    ArpSeq m;
    m.setSettings (offSettings());
    auto out = proc (m, { noteOn (10, 64) }, 100);
    REQUIRE (out.size() == 1);

    m.setSettings (arpOn());
    out = proc (m, {}, 100);
    REQUIRE (out.size() == 2);
    CHECK (isNote (out[0], EventType::NoteOff, 64, 0)); // the part still holds the note we let through
    CHECK (isNote (out[1], EventType::NoteOn, 64, 0));  // the held key is now part of the arpeggio

    // Releasing the key now is consumed by the arpeggiator; the module ends its own note.
    out = proc (m, { noteOff (50, 64) }, 100);
    CHECK (out.empty());
    out = proc (m, {}, 4000);
    REQUIRE (out.size() == 1);
    CHECK (isNote (out[0], EventType::NoteOff, 64, 2800)); // gate 50 % of 6000 from the note-on, 200 samples ago
}

TEST_CASE ("Switching Off -> Arp after the key was released leaves nothing to release", "[seq][mode]")
{
    ArpSeq m;
    m.setSettings (offSettings());
    proc (m, { noteOn (10, 64), noteOff (20, 64) }, 100);
    m.setSettings (arpOn());
    CHECK (proc (m, {}, 100).empty());
}

TEST_CASE ("Switching Arp <-> Seq while a note sounds releases it and restarts the new generator", "[seq][mode]")
{
    ArpSeq m;
    m.setSettings (arpOn (1.0f));
    proc (m, { noteOn (0, 60) }, 100);
    m.setSettings (seqOn()); // step 1: Note, offset 0, velocity 0.8
    auto out = proc (m, {}, 100);
    REQUIRE (out.size() == 2);
    CHECK (isNote (out[0], EventType::NoteOff, 60, 0));
    CHECK (isNote (out[1], EventType::NoteOn, 60, 0));
}

TEST_CASE ("Transport stop and start with a key held: the sounding note is released at offset 0 of the block, the clock follows", "[seq][mode]")
{
    ArpSeq m;
    m.setSettings (arpOn (1.0f));
    auto out = proc (m, { noteOn (0, 60) }, 100, hostTransport (0.0));
    REQUIRE (out.size() == 1);
    CHECK (isNote (out[0], EventType::NoteOn, 60, 0));

    out = proc (m, {}, 100, Transport { 48000.0, 120.0, 0.0, false }); // stopped: the internal clock starts at once
    REQUIRE (out.size() == 2);
    CHECK (isNote (out[0], EventType::NoteOff, 60, 0));
    CHECK (isNote (out[1], EventType::NoteOn, 60, 0));

    out = proc (m, {}, 100, hostTransport (8.0)); // playing again, on a grid point
    REQUIRE (out.size() == 2);
    CHECK (isNote (out[0], EventType::NoteOff, 60, 0));
    CHECK (isNote (out[1], EventType::NoteOn, 60, 0));

    out = proc (m, {}, 100, hostTransport (8.0 + 100.0 / 24000.0)); // continuous: nothing happens inside this block
    CHECK (out.empty());
}

TEST_CASE ("Host sync: the first key waits for the next grid point; steps are on the host grid", "[seq][mode]")
{
    ArpSeq m;
    m.setSettings (arpOn());
    // 120 bpm, 48 kHz: grid points of 1/16 every 6000 samples. ppq 1.0 is a grid point; the block starts 1000 samples later.
    const double ppq = 1.0 + 1000.0 / 24000.0;
    const auto out = proc (m, { noteOn (100, 60) }, 7000, hostTransport (ppq));
    REQUIRE (! out.empty());
    CHECK (isNote (out[0], EventType::NoteOn, 60, 5000)); // ppq 1.25 is 6000 - 1000 samples into the block
}

TEST_CASE ("Capacity: with no room nothing is written and nothing breaks", "[seq][mode]")
{
    for (const int capacity : { 0, 1, 2 })
    {
        ArpSeq m;
        m.setSettings (arpOn());
        std::vector<NoteEvent> out (static_cast<std::size_t> (capacity) + 1);
        const std::vector<NoteEvent> in { noteOn (0, 60), ctrl (5, EventType::PitchBend, 0.1f), noteOn (7, 64) };
        const int n = m.process (in, internalTransport(), 7000, std::span<NoteEvent> (out.data(), static_cast<std::size_t> (capacity)));
        CHECK (n <= capacity);
    }
}

TEST_CASE ("Capacity: on overflow new notes and pass-through events are dropped, owed note-offs are not", "[seq][mode]")
{
    ArpSeq m;
    m.setSettings (arpOn (1.0f));
    REQUIRE (proc (m, { noteOn (0, 60) }, 100).size() == 1); // 60 sounds, gate 100 %: its off falls due at 6000
    REQUIRE (m.isSounding());

    // Next block: room for two events. The bends do not fit (they must leave room for the owed
    // note-off and an overflow marker); the note-off at 6000 is written; the step's note-on at 6000
    // does not fit and is dropped; the marker closes the block.
    std::vector<NoteEvent> in;
    for (int i = 0; i < 5; ++i)
        in.push_back (ctrl (10 + i, EventType::PitchBend, 0.1f));
    const auto out = proc (m, in, 7000 - 100, internalTransport(), 2);
    REQUIRE (out.size() == 2);
    CHECK (isNote (out[0], EventType::NoteOff, 60, 5900));
    CHECK (is (out[1], EventType::AllNotesOff, 5900));
    CHECK (m.droppedCount() > 0);
    CHECK_FALSE (m.isSounding()); // the dropped note-on was never started
}

TEST_CASE ("Capacity: a long random burst never leaves a note on and never exceeds the buffer", "[seq][mode]")
{
    for (const int capacity : { 3, 4, 6, 16 })
    {
        ArpSeq m;
        Settings s = arpOn();
        s.arpPattern = ArpPattern::UpDown;
        s.rate = Rate::ThirtySecond;
        s.gate = 0.5f;
        m.setSettings (s);
        std::uint32_t seed = 7;
        const auto rnd = [&seed] (int n) {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<int> ((seed >> 8) % static_cast<std::uint32_t> (n));
        };
        SoundLedger ledger;
        ledger.panicClears = true; // the overflow marker is the contract's way to release everything downstream
        for (int block = 0; block < 400; ++block)
        {
            std::vector<NoteEvent> in;
            for (int i = 0, k = rnd (6); i < k; ++i)
            {
                const int t = rnd (480);
                switch (rnd (4))
                {
                    case 0: in.push_back (noteOn (t, 40 + rnd (24))); break;
                    case 1: in.push_back (noteOff (t, 40 + rnd (24))); break;
                    case 2: in.push_back (ctrl (t, EventType::PitchBend, 0.1f)); break;
                    default: in.push_back (pedal (t, rnd (2) == 0)); break;
                }
            }
            std::sort (in.begin(), in.end(), [] (const NoteEvent& a, const NoteEvent& b) { return a.offset < b.offset; });
            const auto out = proc (m, in, 480, internalTransport(), capacity);
            int prev = 0;
            for (const auto& e : out)
            {
                CHECK (e.offset >= prev);
                prev = e.offset;
                ledger.feed (e);
            }
        }
        // Finish: panic, then one more block.
        for (const auto& e : proc (m, { ctrl (0, EventType::AllNotesOff) }, 480, internalTransport(), capacity))
            ledger.feed (e);
        INFO ("capacity " << capacity);
        CHECK (ledger.sounding.empty());
        CHECK (ledger.doubleOns == 0);
        CHECK (ledger.orphanOffs == 0);
    }
}

TEST_CASE ("Malformed input: stray note-offs, repeated note-ons and invalid notes do no harm", "[seq][mode]")
{
    ArpSeq m;
    m.setSettings (arpOn());
    // A note-off for a key that is not down, a note number that does not exist, a repeated note-on.
    const auto out = proc (m, { noteOff (0, 50), noteOn (0, 200), noteOn (0, 60), noteOn (100, 60), noteOff (7000, 99) }, 7000);
    int ons = 0;
    for (const auto& e : out)
        if (e.type == EventType::NoteOn)
        {
            ++ons;
            CHECK (e.note == 60);
        }
    CHECK (ons == 2); // steps at 0 and 6000, one note each, not two for the repeated press
}

TEST_CASE ("Empty and odd blocks: zero length, events outside the block, unsorted input", "[seq][mode]")
{
    SECTION ("a zero-length block still applies its events and plays the step at the start of the next one")
    {
        ArpSeq m;
        m.setSettings (arpOn());
        auto out = proc (m, { ctrl (0, EventType::PitchBend, 0.5f), noteOn (0, 60) }, 0);
        REQUIRE (out.size() == 1);
        CHECK (is (out[0], EventType::PitchBend, 0));
        out = proc (m, {}, 100);
        REQUIRE (out.size() == 1);
        CHECK (isNote (out[0], EventType::NoteOn, 60, 0));
    }
    SECTION ("offsets before the block and after its end are clamped into it; output stays ascending")
    {
        ArpSeq m;
        m.setSettings (arpOn());
        const auto out = proc (m, { ctrl (-5, EventType::PitchBend, 0.1f), ctrl (50, EventType::ModWheel, 0.2f), ctrl (20, EventType::PitchBend, 0.3f),
                                    ctrl (500, EventType::ModWheel, 0.4f) },
                               100);
        REQUIRE (out.size() == 4);
        CHECK (out[0].offset == 0);
        CHECK (out[1].offset == 50);
        CHECK (out[2].offset == 50); // not ascending in the input: the later event cannot come earlier
        CHECK (out[3].offset == 99);
        CHECK (std::fabs (out[2].value - 0.3f) < 1.0e-7f);
    }
    SECTION ("an empty input produces nothing")
    {
        ArpSeq m;
        m.setSettings (arpOn());
        CHECK (proc (m, {}, 480).empty());
    }
}

TEST_CASE ("Invalid transports fall back to the internal clock", "[seq][mode]")
{
    for (const Transport& tr : { Transport { 48000.0, 0.0, 0.0, true }, Transport { 48000.0, -10.0, 0.0, true },
                                 Transport { 48000.0, 120.0, std::nan (""), true }, Transport { 0.0, 120.0, 0.0, false } })
    {
        ArpSeq m;
        m.setSettings (arpOn());
        const auto out = proc (m, { noteOn (0, 60) }, 100, tr);
        REQUIRE (! out.empty());
        CHECK (isNote (out[0], EventType::NoteOn, 60, 0));
    }
}

TEST_CASE ("reset(): keys, clock and owed notes are forgotten, settings are kept", "[seq][mode]")
{
    ArpSeq m;
    m.setSettings (arpOn());
    proc (m, { noteOn (0, 60) }, 100);
    REQUIRE (m.isSounding());
    m.reset();
    CHECK_FALSE (m.isSounding());
    CHECK (m.settings().mode == Mode::Arp);
    CHECK (proc (m, {}, 20000).empty()); // the held key is gone
    const auto out = proc (m, { noteOn (0, 62) }, 100);
    REQUIRE (out.size() == 1);
    CHECK (isNote (out[0], EventType::NoteOn, 62, 0));
}

TEST_CASE ("Settings: out-of-range and non-finite values are clamped", "[seq][mode]")
{
    Settings s;
    s.swing = std::nanf ("");
    s.gate = 7.0f;
    s.internalBpm = 1.0e9;
    s.arpOctaves = 99;
    s.seqLength = 0;
    s.seqSteps[3].semitones = -99;
    s.seqSteps[4].semitones = 99;
    s.seqSteps[5].velocity = -1.0f;
    s.seqSteps[6].velocity = std::nanf ("");
    const Settings c = s.sanitised();
    CHECK (std::fabs (c.swing - 0.5f) < 1.0e-7f);
    CHECK (std::fabs (c.gate - 1.0f) < 1.0e-7f);
    CHECK (std::fabs (c.internalBpm - 300.0) < 1.0e-9);
    CHECK (c.arpOctaves == 4);
    CHECK (c.seqLength == 1);
    CHECK (c.seqSteps[3].semitones == -24);
    CHECK (c.seqSteps[4].semitones == 24);
    CHECK (std::fabs (c.seqSteps[5].velocity) < 1.0e-7f);
    CHECK (std::isfinite (c.seqSteps[6].velocity));

    Settings lo;
    lo.swing = 0.1f;
    lo.gate = 0.0f;
    lo.internalBpm = 0.5;
    lo.arpOctaves = -3;
    lo.seqLength = 99;
    const Settings d = lo.sanitised();
    CHECK (std::fabs (d.swing - 0.5f) < 1.0e-7f);
    CHECK (std::fabs (d.gate - 0.05f) < 1.0e-7f);
    CHECK (std::fabs (d.internalBpm - 20.0) < 1.0e-9);
    CHECK (d.arpOctaves == 1);
    CHECK (d.seqLength == 16);

    // setSettings applies the same clamping.
    ArpSeq m;
    m.setSettings (s);
    CHECK (m.settings().seqLength == 1);
}
