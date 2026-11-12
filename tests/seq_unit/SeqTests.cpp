// Step sequencer: transposition, step types (Note / Rest / Tie), length, restart rule, Hold.
// Same internal-clock setup as the arpeggiator tests (6000 samples per step), plus randomised
// sequences compared with the exact tick model of SeqTestUtil.h (host synced and internal).
#include <catch2/catch_test_macros.hpp>

#include "SeqTestUtil.h"

#include <numeric>
#include <string>

using namespace vs::seq;
using namespace vs::seq::test;
using vs::EventType;

namespace
{
constexpr int kStep = 6000;

SeqStep step (int semitones, StepType type = StepType::Note, float vel = 0.8f)
{
    return SeqStep { semitones, type, vel };
}

Settings seqSettings (std::initializer_list<SeqStep> steps)
{
    Settings s;
    s.mode = Mode::Seq;
    s.rate = Rate::Sixteenth;
    s.gate = 0.5f;
    s.internalBpm = 120.0;
    s.seqLength = static_cast<int> (steps.size());
    std::size_t i = 0;
    for (const auto& st : steps)
        s.seqSteps[i++] = st;
    return s;
}

struct Played
{
    std::vector<int> notes;
    std::vector<float> velocities;
    std::vector<std::int64_t> times;
    std::vector<NoteEvent> all;
};

Played runInternal (const Settings& s, std::vector<NoteEvent> input, std::int64_t total, std::vector<Change> more = {}, int block = 128)
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

std::vector<std::int64_t> offTimes (const Played& p)
{
    std::vector<std::int64_t> t;
    for (const auto& e : p.all)
        if (e.type == EventType::NoteOff)
            t.push_back (e.offset);
    return t;
}

struct Rng
{
    std::uint64_t s;
    explicit Rng (std::uint64_t seed) : s (seed) {}
    std::uint32_t next()
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t> (s >> 33);
    }
    int below (int n) { return static_cast<int> (next() % static_cast<std::uint32_t> (n)); }
};
} // namespace

TEST_CASE ("Seq: plays the step offsets from the lowest held key (relative to C4)", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (0), step (4), step (7), step (12) });
    SECTION ("key C4 plays the offsets as written")
        CHECK (runInternal (s, { noteOn (0, 60) }, 8 * kStep - 1).notes == std::vector<int> { 60, 64, 67, 72, 60, 64, 67, 72 });
    SECTION ("another root transposes by the difference to C4")
        CHECK (runInternal (s, { noteOn (0, 62) }, 5 * kStep - 1).notes == std::vector<int> { 62, 66, 69, 74, 62 });
    SECTION ("a root below C4 transposes down")
        CHECK (runInternal (s, { noteOn (0, 48) }, 4 * kStep - 1).notes == std::vector<int> { 48, 52, 55, 60 });
    SECTION ("only the lowest key counts")
        CHECK (runInternal (s, { noteOn (0, 70), noteOn (0, 62), noteOn (0, 90) }, 4 * kStep - 1).notes == std::vector<int> { 62, 66, 69, 74 });
}

TEST_CASE ("Seq: the transposition follows the lowest held key from the next step", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (0), step (2), step (4), step (6) });
    SECTION ("a lower key pressed between steps")
    {
        // Steps at 0, 6000, 12000, ...: key 55 goes down at 7000.
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (7000, 55) }, 6 * kStep - 1);
        CHECK (p.notes == std::vector<int> { 60, 62, 59, 61, 55, 57 });
    }
    SECTION ("the lowest key released: the next lowest takes over")
    {
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (0, 67), noteOff (7000, 60) }, 5 * kStep - 1);
        CHECK (p.notes == std::vector<int> { 60, 62, 71, 73, 67 });
    }
    SECTION ("a higher key does not change anything")
    {
        const auto p = runInternal (s, { noteOn (0, 60), noteOn (7000, 80) }, 4 * kStep - 1);
        CHECK (p.notes == std::vector<int> { 60, 62, 64, 66 });
    }
}

TEST_CASE ("Seq: note numbers are clamped to 0..127", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (24), step (-24) });
    CHECK (runInternal (s, { noteOn (0, 120) }, 2 * kStep - 1).notes == std::vector<int> { 127, 96 });
    CHECK (runInternal (s, { noteOn (0, 10) }, 2 * kStep - 1).notes == std::vector<int> { 34, 0 });
}

TEST_CASE ("Seq: the output velocity is the step's, not the key's", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (0, StepType::Note, 0.9f), step (0, StepType::Note, 0.25f), step (0, StepType::Note, 0.0f), step (0, StepType::Note, 1.0f) });
    const auto p = runInternal (s, { noteOn (0, 60, 0.3f) }, 4 * kStep - 1);
    REQUIRE (p.velocities.size() == 4);
    CHECK (std::fabs (p.velocities[0] - 0.9f) < 1.0e-7f);
    CHECK (std::fabs (p.velocities[1] - 0.25f) < 1.0e-7f);
    CHECK (p.velocities[2] > 0.0f);                       // never a zero-velocity note-on (that would read as a note-off)
    CHECK (p.velocities[2] <= 1.0f / 127.0f + 1.0e-7f);
    CHECK (std::fabs (p.velocities[3] - 1.0f) < 1.0e-7f);
}

TEST_CASE ("Seq: every length from 1 to 16 cycles through exactly that many steps", "[seq][sequencer]")
{
    for (int length = 1; length <= 16; ++length)
    {
        Settings s;
        s.mode = Mode::Seq;
        s.gate = 0.5f;
        s.internalBpm = 120.0;
        s.seqLength = length;
        for (int i = 0; i < 16; ++i)
            s.seqSteps[static_cast<std::size_t> (i)] = step (i); // offset i marks the step
        const auto p = runInternal (s, { noteOn (0, 60) }, 40 * kStep - 1);
        REQUIRE (p.notes.size() == 40);
        for (int i = 0; i < 40; ++i)
        {
            INFO ("length " << length << " step " << i);
            CHECK (p.notes[static_cast<std::size_t> (i)] == 60 + i % length);
        }
    }
}

TEST_CASE ("Seq: a length change takes effect from the next step", "[seq][sequencer]")
{
    Settings s = seqSettings ({ step (0), step (1), step (2), step (3), step (4), step (5) });
    Settings shorter = s;
    shorter.seqLength = 3;
    // Steps at 0, 6000, ...; the change lands at 15000: steps 0..2 use length 6, then length 3 with the counter at 3 -> 3 % 3 = 0.
    const auto p = runInternal (s, { noteOn (0, 60) }, 8 * kStep - 1, { Change { 15000, shorter } });
    CHECK (p.notes == std::vector<int> { 60, 61, 62, 60, 61, 62, 60, 61 });
}

TEST_CASE ("Seq: Rest emits nothing, the clock keeps its place", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (0), step (0, StepType::Rest), step (7), step (0, StepType::Rest) });
    const auto p = runInternal (s, { noteOn (0, 60) }, 8 * kStep - 1);
    CHECK (p.notes == std::vector<int> { 60, 67, 60, 67 });
    CHECK (p.times == std::vector<std::int64_t> { 0, 2 * kStep, 4 * kStep, 6 * kStep });
}

TEST_CASE ("Seq: Tie extends the previous note without retriggering", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (0), step (0, StepType::Tie), step (0, StepType::Tie), step (0, StepType::Rest) });
    const auto p = runInternal (s, { noteOn (0, 60) }, 4 * kStep - 1);
    CHECK (p.notes == std::vector<int> { 60 });
    // On at 0; off at the gate end (50 %) of the last tied step: 2 * 6000 + 3000.
    CHECK (offTimes (p) == std::vector<std::int64_t> { 2 * kStep + kStep / 2 });
}

TEST_CASE ("Seq: Tie at the end of a tie chain, then a new note, retriggers", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (0), step (0, StepType::Tie), step (7) });
    const auto p = runInternal (s, { noteOn (0, 60) }, 6 * kStep - 1);
    CHECK (p.notes == std::vector<int> { 60, 67, 60, 67 });
    // Note 60 held through two steps (off at 6000 + 3000), 67 one step (off at 12000 + 3000), and again.
    CHECK (p.times == std::vector<std::int64_t> { 0, 2 * kStep, 3 * kStep, 5 * kStep });
    CHECK (offTimes (p) == std::vector<std::int64_t> { kStep + kStep / 2, 2 * kStep + kStep / 2, 4 * kStep + kStep / 2, 5 * kStep + kStep / 2 });
}

TEST_CASE ("Seq: a Tie across the end of the loop extends the last note into step 1", "[seq][sequencer]")
{
    // [Tie, Rest, Note]: the Note at step 3 is tied into step 1 of the next cycle.
    const auto s = seqSettings ({ step (0, StepType::Tie), step (0, StepType::Rest), step (5) });
    const auto p = runInternal (s, { noteOn (0, 60) }, 7 * kStep - 1);
    // Step 0 (Tie, nothing sounds), 1 Rest, 2 Note 65 @12000 (extended), 3 = Tie @18000 extends, 4 Rest, 5 Note @30000, 6 Tie @36000.
    CHECK (p.notes == std::vector<int> { 65, 65 });
    CHECK (p.times == std::vector<std::int64_t> { 2 * kStep, 5 * kStep });
    const auto offs = offTimes (p);
    CHECK (offs == std::vector<std::int64_t> { 3 * kStep + kStep / 2, 6 * kStep + kStep / 2 }); // the gate of the tied step
}

TEST_CASE ("Seq: a Tie with nothing sounding is silent (after a Rest, or as the very first step)", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (0, StepType::Tie), step (0, StepType::Rest), step (0, StepType::Tie) });
    const auto p = runInternal (s, { noteOn (0, 60) }, 12 * kStep);
    CHECK (p.all.empty());
}

TEST_CASE ("Seq: only Rests and Ties neither hang nor spin", "[seq][sequencer]")
{
    for (int variant = 0; variant < 3; ++variant)
    {
        Settings s;
        s.mode = Mode::Seq;
        s.internalBpm = 300.0;
        s.rate = Rate::ThirtySecond;
        s.seqLength = variant == 0 ? 1 : 16;
        for (int i = 0; i < 16; ++i)
            s.seqSteps[static_cast<std::size_t> (i)] = step (i, (i + variant) % 2 == 0 ? StepType::Rest : StepType::Tie);
        if (variant == 2)
            for (auto& st : s.seqSteps)
                st.type = StepType::Tie;
        const auto p = runInternal (s, { noteOn (0, 60) }, 48000 * 20, {}, 4096);
        CHECK (p.all.empty());
    }
}

TEST_CASE ("Seq: a step change at a block boundary acts from the next step", "[seq][sequencer]")
{
    const auto a = seqSettings ({ step (0), step (0), step (0), step (0) });
    const auto b = seqSettings ({ step (12), step (7), step (5), step (3) });
    // Change at 9000: steps 0 and 1 (0, 6000) are from `a`; step 2 (12000) onwards from `b`, position 2 -> offset 5.
    const auto p = runInternal (a, { noteOn (0, 60) }, 6 * kStep - 1, { Change { 9000, b } });
    CHECK (p.notes == std::vector<int> { 60, 60, 65, 63, 72, 67 });
}

TEST_CASE ("Seq: the sequence restarts from step 1 when the first key goes down", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (0), step (2), step (4), step (6) });
    const auto p = runInternal (s, { noteOn (0, 60), noteOff (15000, 60), noteOn (20000, 60) }, 30000 + 5 * kStep);
    // 0, 6000, 12000 (steps 1-3 of the first run); the key goes up at 15000 and down again at 20000: step 1 again at 20000.
    CHECK (p.notes == std::vector<int> { 60, 62, 64, 60, 62, 64, 66, 60, 62, 64 });
    CHECK (p.times[3] == 20000);
}

TEST_CASE ("Seq: a second key while one is held does not restart the sequence", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (0), step (2), step (4), step (6) });
    const auto p = runInternal (s, { noteOn (0, 60), noteOn (7000, 64) }, 6 * kStep - 1);
    CHECK (p.notes == std::vector<int> { 60, 62, 64, 66, 60, 62 });
}

TEST_CASE ("Seq Hold: keeps playing after release; a new key re-transposes without restarting", "[seq][sequencer]")
{
    Settings s = seqSettings ({ step (0), step (2), step (4), step (6) });
    s.hold = true;
    const auto p = runInternal (s, { noteOn (0, 60), noteOff (1000, 60), noteOn (20000, 50) }, 10 * kStep - 1);
    // Steps at 0, 6000, 12000, 18000 (root 60), the new key goes down at 20000: steps from 24000 use root 50 and the
    // sequence is NOT restarted (position 4 -> step 1 of the cycle ... offsets 0, 2, 4, 6 repeat).
    CHECK (p.notes == std::vector<int> { 60, 62, 64, 66, 50, 52, 54, 56, 50, 52 });
}

TEST_CASE ("Seq without Hold: releasing the last key stops the sequence", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (0) });
    const auto p = runInternal (s, { noteOn (0, 60), noteOff (13000, 60) }, 10 * kStep);
    CHECK (p.notes.size() == 3); // 0, 6000, 12000
    SoundLedger led;
    for (const auto& e : p.all)
        led.feed (e);
    CHECK (led.sounding.empty());
}

TEST_CASE ("Seq: releasing the last key ends a note held open for a Tie immediately", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (0), step (0, StepType::Tie), step (0, StepType::Tie), step (0, StepType::Tie) });
    // Note at 0 stays open through the ties; the key goes up at 9000: the note ends then.
    const auto p = runInternal (s, { noteOn (0, 60), noteOff (9000, 60) }, 6 * kStep);
    CHECK (p.notes.size() == 1);
    CHECK (offTimes (p) == std::vector<std::int64_t> { 9000 });
}

TEST_CASE ("Seq: the sustain pedal keeps the released root", "[seq][sequencer]")
{
    const auto s = seqSettings ({ step (0), step (2) });
    const auto p = runInternal (s, { noteOn (0, 60), pedal (10, true), noteOff (100, 60), pedal (20000, false) }, 8 * kStep);
    // 0, 6000, 12000, 18000 play; the pedal comes up at 20000 and ends the sequence.
    CHECK (p.notes == std::vector<int> { 60, 62, 60, 62 });
}

TEST_CASE ("Seq: random sequences match the exact timing model (host synced and internal clock)", "[seq][sequencer][timing]")
{
    constexpr int kD[] = { 1, 2, 3, 4, 6, 8 };
    constexpr Rate kRates[] = { Rate::Quarter, Rate::Eighth, Rate::EighthTriplet, Rate::Sixteenth, Rate::SixteenthTriplet, Rate::ThirtySecond };
    constexpr int kSr[] = { 44100, 48000, 96000, 192000 };
    constexpr std::int64_t kBpm[] = { 5000, 9750, 12000, 13337, 24001 };
    constexpr int kBlocks[] = { 1, 5, 64, 480, 4096 };
    Rng rng (2024);
    std::int64_t totalNotes = 0, totalTies = 0;
    for (int i = 0; i < 200; ++i)
    {
        MCfg c;
        c.sr = kSr[rng.below (4)];
        const int di = rng.below (6);
        c.d = kD[di];
        c.swing80 = 40 + rng.below (21);
        c.gate80 = (i % 4 == 0) ? 80 : 4 + rng.below (77); // every 4th run is legato (gate 100 %)
        const std::int64_t bpm100 = kBpm[rng.below (5)];
        const int block = kBlocks[i % 5];
        const bool internal = i % 2 == 1;

        Settings s;
        s.mode = Mode::Seq;
        s.rate = kRates[di];
        s.swing = static_cast<double> (c.swing80) / 80.0;
        s.gate = static_cast<double> (c.gate80) / 80.0;
        s.internalBpm = static_cast<double> (bpm100) / 100.0;
        s.seqLength = 1 + rng.below (16);
        const int root = 30 + rng.below (60);
        std::vector<MStep> pattern;
        for (int k = 0; k < s.seqLength; ++k)
        {
            const int r = rng.below (6);
            const StepType type = r < 3 ? StepType::Note : (r < 4 ? StepType::Rest : StepType::Tie);
            const int semis = rng.below (49) - 24;
            s.seqSteps[static_cast<std::size_t> (k)] = SeqStep { semis, type, 0.7f };
            pattern.push_back (MStep { type, std::clamp (root + semis, 0, 127) });
        }

        const std::int64_t stepSamples = 60 * static_cast<std::int64_t> (c.sr) * 100 / (bpm100 * c.d);
        const std::int64_t keyDown = rng.below (2000);
        const std::int64_t total = std::min<std::int64_t> (keyDown + 3 * stepSamples * s.seqLength + 5, static_cast<std::int64_t> (c.sr) * (block == 1 ? 1 : 6) / (block == 1 ? 2 : 1));
        const std::int64_t anchor = kTick * 33 + rng.below (static_cast<int> (kTick * 9));

        ArpSeq m;
        Run r;
        r.sr = c.sr;
        r.total = total;
        r.block = block;
        r.segs = { HostSeg { 0, ! internal, bpm100, anchor } };
        r.changes = { Change { 0, s } };
        r.input = { noteOn (static_cast<int> (keyDown), root) };
        const RunResult res = run (m, r);
        std::int64_t steps = 0;
        const MSeg seg = internal ? MSeg { keyDown, total - keyDown, bpm100, 0, true } : MSeg { 0, total, bpm100, anchor, false };
        const auto expected = model (c, { seg }, keyDown, pattern, steps);
        std::string why;
        const bool ok = res.wellFormed && matchesModel (res.events, expected, total, &why);
        INFO ("sr " << c.sr << " bpm100 " << bpm100 << " d " << c.d << " swing80 " << c.swing80 << " gate80 " << c.gate80 << " block " << block
                    << (internal ? " internal" : " host") << " length " << s.seqLength << ": " << why);
        CHECK (ok);
        for (const auto& e : expected)
            totalNotes += e.on ? 1 : 0;
        for (const auto& st : pattern)
            totalTies += st.kind == StepType::Tie ? 1 : 0;
    }
    CHECK (totalNotes > 1500);
    CHECK (totalTies > 100);
}
