// Sample-exact timing of the step clock, through the front-end with one held key.
//
// Every expectation comes from the integer tick model in SeqTestUtil.h: positions are integers in
// 1/6144 quarter notes, tempi are integers in 1/100 bpm, and a sample is
// ceil(ticks * 6000 * sr / (6144 * bpm100)), all in 64-bit integer arithmetic. The module uses
// doubles; the two must agree on every event, to the sample (tolerance 0).
#include <catch2/catch_test_macros.hpp>

#include "SeqTestUtil.h"

#include <numeric>
#include <string>

using namespace vs::seq;
using namespace vs::seq::test;

namespace
{
constexpr int kD[] = { 1, 2, 3, 4, 6, 8 };
constexpr Rate kRates[] = { Rate::Quarter, Rate::Eighth, Rate::EighthTriplet, Rate::Sixteenth, Rate::SixteenthTriplet, Rate::ThirtySecond };
constexpr int kSampleRates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
// Tempi in 1/100 bpm, including non-integer ones.
constexpr std::int64_t kTempi[] = { 2500, 5000, 9750, 11999, 12000, 13337, 17321, 29999 };
constexpr int kBlocks[] = { 1, 7, 64, 333, 480, 4096 };

/** Small deterministic generator (std distributions differ between standard libraries). */
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

int rateIndex (int d)
{
    for (int i = 0; i < 6; ++i)
        if (kD[i] == d)
            return i;
    return 3;
}

Settings arpSettings (int d, int swing16, int gate16)
{
    Settings s;
    s.mode = Mode::Arp;
    s.rate = kRates[rateIndex (d)];
    s.swing = static_cast<float> (swing16) / 16.0f;
    s.gate = static_cast<float> (gate16) / 16.0f;
    s.arpPattern = ArpPattern::Up;
    return s;
}

std::string describe (const MCfg& c, std::int64_t bpm100, int block)
{
    return "sr " + std::to_string (c.sr) + " bpm100 " + std::to_string (bpm100) + " d " + std::to_string (c.d) + " swing16 "
           + std::to_string (c.swing16) + " gate16 " + std::to_string (c.gate16) + " block " + std::to_string (block);
}

/** Samples after which a stretch at this tempo advances a whole number of ticks. */
std::int64_t lenUnit (std::int64_t bpm100, int sr)
{
    const std::int64_t num = 6144 * bpm100;
    const std::int64_t den = 6000 * static_cast<std::int64_t> (sr);
    return den / std::gcd (num, den);
}

std::int64_t ticksOver (std::int64_t len, std::int64_t bpm100, int sr)
{
    return len * 6144 * bpm100 / (6000 * static_cast<std::int64_t> (sr));
}

/** The note events a one-key arpeggio must produce for a single host segment. */
bool checkSingleSegment (const MCfg& c, std::int64_t bpm100, std::int64_t anchor, std::int64_t keyDown, std::int64_t total, int block,
                         std::string& why, bool keepEvents = false, std::vector<NoteEvent>* eventsOut = nullptr)
{
    ArpSeq m;
    Run r;
    r.sr = c.sr;
    r.total = total;
    r.block = block;
    r.segs = { HostSeg { 0, true, bpm100, anchor } };
    r.changes = { Change { 0, arpSettings (c.d, c.swing16, c.gate16) } };
    r.input = { noteOn (static_cast<int> (keyDown), 60) };
    const RunResult res = run (m, r);
    if (! res.wellFormed)
    {
        why = "output not ascending / out of block";
        return false;
    }
    if (keepEvents && eventsOut != nullptr)
        *eventsOut = res.events;
    std::int64_t steps = 0;
    const auto expected = model (c, { MSeg { 0, total, bpm100, anchor, false } }, keyDown, { MStep {} }, steps);
    if (steps < 1)
    {
        why = "the scenario is too short to contain a step";
        return false;
    }
    return matchesModel (res.events, expected, total, &why);
}
} // namespace

TEST_CASE ("Timing, host synced: every rate, swing and gate at 120 bpm / 48 kHz is exact to the sample", "[seq][timing]")
{
    for (int d : kD)
        for (int swing16 : { 8, 9, 10, 11, 12 })
            for (int gate16 : { 1, 4, 8, 12, 16 })
            {
                MCfg c;
                c.sr = 48000;
                c.d = d;
                c.swing16 = swing16;
                c.gate16 = gate16;
                std::string why;
                const bool ok = checkSingleSegment (c, 12000, 6144 * 7 + 1500, 0, 48000 * 2, 480, why);
                INFO (describe (c, 12000, 480) << ": " << why);
                CHECK (ok);
            }
}

TEST_CASE ("Timing, host synced: random tempi (non-integer too), sample rates, block sizes and start positions", "[seq][timing]")
{
    Rng rng (12345);
    int checked = 0;
    for (int i = 0; i < 240; ++i)
    {
        MCfg c;
        c.sr = kSampleRates[rng.below (6)];
        c.d = kD[rng.below (6)];
        c.swing16 = 8 + rng.below (5);
        c.gate16 = 1 + rng.below (16);
        const std::int64_t bpm100 = kTempi[rng.below (8)];
        const int block = kBlocks[i % 6];
        // Start anywhere in a bar (any tick, so also off the sample grid); key a little into the block.
        const std::int64_t anchor = static_cast<std::int64_t> (rng.below (6144 * 16)) + 6144 * 100;
        const std::int64_t keyDown = rng.below (3000);
        // At least a few steps, at most a couple of seconds; fewer samples for 1-sample blocks.
        const std::int64_t stepSamples = 60 * static_cast<std::int64_t> (c.sr) * 100 / (bpm100 * c.d);
        const std::int64_t total = std::max<std::int64_t> (block == 1 ? c.sr / 4 : c.sr * 2, keyDown + 4 * stepSamples);
        std::string why;
        const bool ok = checkSingleSegment (c, bpm100, anchor, keyDown, total, block, why);
        INFO (describe (c, bpm100, block) << " anchor " << anchor << " key " << keyDown << ": " << why);
        CHECK (ok);
        ++checked;
    }
    CHECK (checked == 240);
}

TEST_CASE ("Timing: two blocks split anywhere give the same events as one, at every block size", "[seq][timing]")
{
    MCfg c;
    c.sr = 44100;
    c.d = 6;
    c.swing16 = 11;
    c.gate16 = 10;
    const std::int64_t bpm100 = 13337;
    std::vector<NoteEvent> reference;
    std::string why;
    REQUIRE (checkSingleSegment (c, bpm100, 6144 * 3 + 77, 211, 44100 * 3, 44100 * 3, why, true, &reference));
    REQUIRE (reference.size() > 20);
    for (const int block : { 1, 2, 3, 5, 8, 64, 127, 128, 480, 1000, 4096, 10007 })
    {
        std::vector<NoteEvent> events;
        REQUIRE (checkSingleSegment (c, bpm100, 6144 * 3 + 77, 211, 44100 * 3, block, why, true, &events));
        INFO ("block " << block);
        CHECK (sameEvents (events, reference));
    }
}

TEST_CASE ("Timing: sub-sample step positions round up to the first sample at or after them", "[seq][timing]")
{
    // 97.5 bpm at 44.1 kHz, 1/16: a step is 6784.615... samples. Gate 50 %: 3392.307... later.
    ArpSeq m;
    Run r;
    r.sr = 44100;
    r.total = 20000;
    r.block = 100;
    r.segs = { HostSeg { 0, true, 9750, 0 } };
    r.changes = { Change { 0, arpSettings (4, 8, 8) } };
    r.input = { noteOn (0, 60) };
    const auto notes = notesOnly (run (m, r).events);
    REQUIRE (notes.size() == 6);
    const std::int64_t expected[] = { 0, 3393, 6785, 10177, 13570, 16962 }; // ceil(0), ceil(3392.3), ceil(6784.6), ceil(10176.9), ceil(13569.2), ceil(16961.5)
    for (std::size_t i = 0; i < 6; ++i)
        CHECK (notes[i].offset == expected[i]);
}

TEST_CASE ("Timing, tempo changes: stepwise tempo changes between blocks, non-integer tempi, any block size", "[seq][timing]")
{
    Rng rng (777);
    int checked = 0;
    std::int64_t totalSteps = 0;
    for (int i = 0; i < 90; ++i)
    {
        MCfg c;
        c.sr = kSampleRates[rng.below (6)];
        c.d = kD[rng.below (6)];
        c.swing16 = 8 + rng.below (5);
        c.gate16 = 1 + rng.below (16);
        const int block = kBlocks[i % 6];

        // Four stretches with different tempi; each one a whole number of ticks long so the host
        // position is continuous (exactly the previous end) at every change.
        std::vector<HostSeg> hs;
        std::vector<MSeg> ms;
        std::int64_t start = 0;
        std::int64_t anchor = 6144 * 50 + rng.below (6144);
        bool feasible = true;
        for (int k = 0; k < 4 && feasible; ++k)
        {
            // A tempo whose stretch can be a whole number of ticks within a few seconds.
            std::int64_t bpm100 = 0, len = 0;
            for (int attempt = 0; attempt < 16 && len == 0; ++attempt)
            {
                bpm100 = kTempi[rng.below (8)];
                const std::int64_t unit = lenUnit (bpm100, c.sr);
                const std::int64_t stepSamples = 60 * static_cast<std::int64_t> (c.sr) * 100 / (bpm100 * c.d);
                const std::int64_t target = std::max<std::int64_t> (block == 1 ? c.sr / 6 : c.sr / 2, stepSamples * 3 / 2);
                const std::int64_t candidate = std::max<std::int64_t> (1, target / unit) * unit;
                if (candidate <= 3 * static_cast<std::int64_t> (c.sr))
                    len = candidate;
            }
            if (len == 0)
            {
                feasible = false;
                break;
            }
            hs.push_back (HostSeg { start, true, bpm100, anchor });
            ms.push_back (MSeg { start, len, bpm100, anchor, false });
            start += len;
            anchor += ticksOver (len, bpm100, c.sr);
        }
        if (! feasible)
            continue;

        ArpSeq m;
        Run r;
        r.sr = c.sr;
        r.total = start;
        r.block = block;
        r.segs = hs;
        r.changes = { Change { 0, arpSettings (c.d, c.swing16, c.gate16) } };
        r.input = { noteOn (5, 60) };
        const RunResult res = run (m, r);
        std::int64_t steps = 0;
        const auto expected = model (c, ms, 5, { MStep {} }, steps);
        std::string why;
        const bool ok = res.wellFormed && matchesModel (res.events, expected, start, &why);
        INFO (describe (c, hs[0].bpm100, block) << " segments " << hs.size() << ": " << why);
        CHECK (ok);
        CHECK (steps >= 1);
        totalSteps += steps;
        ++checked;
    }
    CHECK (checked >= 60);
    CHECK (totalSteps > 500); // the comparison is not vacuous
}

TEST_CASE ("Timing, loop jumps: backward and forward jumps resynchronise on the new grid and release what sounds", "[seq][timing]")
{
    Rng rng (4242);
    int checked = 0;
    for (int i = 0; i < 90; ++i)
    {
        MCfg c;
        c.sr = kSampleRates[rng.below (6)];
        c.d = kD[rng.below (6)];
        c.swing16 = 8 + rng.below (5);
        c.gate16 = (i % 3 == 0) ? 16 : 1 + rng.below (16); // 100 % gates so that a note is usually sounding at the jump
        const int block = kBlocks[i % 6];
        const std::int64_t bpm100 = kTempi[rng.below (8)];
        const std::int64_t seg = block == 1 ? c.sr / 5 : c.sr / 2;
        // Jump times at arbitrary samples (not step boundaries); targets anywhere, also mid-step.
        std::vector<HostSeg> hs;
        std::vector<MSeg> ms;
        std::int64_t start = 0;
        std::int64_t anchor = 6144 * 300 + rng.below (6144 * 4);
        for (int k = 0; k < 4; ++k)
        {
            const std::int64_t len = seg + rng.below (997);
            hs.push_back (HostSeg { start, true, bpm100, anchor });
            ms.push_back (MSeg { start, len, bpm100, anchor, k > 0 });
            start += len;
            // Next stretch: loop back or jump forward by 200 ticks (more than the 96-tick continuity
            // tolerance) to several beats, relative to where playback would have continued.
            const std::int64_t delta = (rng.below (2) == 0 ? -1 : 1) * (200 + rng.below (6144 * 4) + 6144 * rng.below (8));
            anchor = anchor + ticksOver (len, bpm100, c.sr) + delta;
        }
        ArpSeq m;
        Run r;
        r.sr = c.sr;
        r.total = start;
        r.block = block;
        r.segs = hs;
        r.changes = { Change { 0, arpSettings (c.d, c.swing16, c.gate16) } };
        r.input = { noteOn (0, 60) };
        const RunResult res = run (m, r);
        std::int64_t steps = 0;
        const auto expected = model (c, ms, 0, { MStep {} }, steps);
        std::string why;
        const bool ok = res.wellFormed && matchesModel (res.events, expected, start, &why);
        INFO (describe (c, bpm100, block) << ": " << why);
        CHECK (ok);
        ++checked;
    }
    CHECK (checked == 90);
}

TEST_CASE ("Timing, loop jump with a note sounding: the note-off lands at offset 0 of the new block", "[seq][timing]")
{
    // 120 bpm, 48 kHz, 1/4 steps (24000 samples), gate 100 %: the note started at 0 is still on at 30000.
    ArpSeq m;
    Run r;
    r.sr = 48000;
    r.total = 60000;
    r.block = 4096;
    r.segs = { HostSeg { 0, true, 12000, 0 }, HostSeg { 30000, true, 12000, 6144 * 8 } };
    r.changes = { Change { 0, arpSettings (1, 8, 16) } };
    r.input = { noteOn (0, 60) };
    const auto notes = notesOnly (run (m, r).events);
    // on @0, off+on @24000 (gate 100 %), off @30000 (the jump), on @30000 (the grid point 8.0 is at the jump), ...
    REQUIRE (notes.size() >= 5);
    CHECK (notes[0].type == vs::EventType::NoteOn);
    CHECK (notes[0].offset == 0);
    CHECK (notes[1].type == vs::EventType::NoteOff);
    CHECK (notes[1].offset == 24000);
    CHECK (notes[2].type == vs::EventType::NoteOn);
    CHECK (notes[2].offset == 24000);
    CHECK (notes[3].type == vs::EventType::NoteOff);
    CHECK (notes[3].offset == 30000);
    CHECK (notes[4].type == vs::EventType::NoteOn);
    CHECK (notes[4].offset == 30000);
}

TEST_CASE ("Timing, internal clock: starts on the first key and runs at the module's own tempo, exact to the sample", "[seq][timing]")
{
    Rng rng (99);
    int checked = 0;
    for (int i = 0; i < 120; ++i)
    {
        MCfg c;
        c.sr = kSampleRates[rng.below (6)];
        c.d = kD[rng.below (6)];
        c.swing16 = 8 + rng.below (5);
        c.gate16 = 1 + rng.below (16);
        const int block = kBlocks[i % 6];
        const std::int64_t bpm100 = 2000 + rng.below (28001); // 20..300 bpm in 0.01 steps
        const std::int64_t keyDown = rng.below (5000);
        const std::int64_t total = keyDown + (block == 1 ? c.sr / 4 : c.sr * 2);

        ArpSeq m;
        Run r;
        r.sr = c.sr;
        r.total = total;
        r.block = block;
        r.segs = { HostSeg { 0, false, 12000, 0 } }; // transport stopped; its tempo is ignored
        Settings s = arpSettings (c.d, c.swing16, c.gate16);
        s.internalBpm = static_cast<double> (bpm100) / 100.0;
        r.changes = { Change { 0, s } };
        r.input = { noteOn (static_cast<int> (keyDown), 60) };
        const RunResult res = run (m, r);

        std::int64_t steps = 0;
        // The internal clock's position is 0 at the key's sample.
        const auto expected = model (c, { MSeg { keyDown, total - keyDown, bpm100, 0, true } }, keyDown, { MStep {} }, steps);
        std::string why;
        const bool ok = res.wellFormed && matchesModel (res.events, expected, total, &why);
        INFO (describe (c, bpm100, block) << " key " << keyDown << ": " << why);
        CHECK (ok);
        ++checked;
    }
    CHECK (checked == 120);
}

TEST_CASE ("Timing, internal clock: nothing is generated before the first key and after the last key is released", "[seq][timing]")
{
    ArpSeq m;
    Run r;
    r.sr = 48000;
    r.total = 48000 * 3;
    r.block = 128;
    r.segs = { HostSeg { 0, false, 12000, 0 } };
    Settings s = arpSettings (4, 8, 8);
    s.internalBpm = 120.0;
    r.changes = { Change { 0, s } };
    r.input = { noteOn (30000, 60), noteOff (60500, 60) };
    const auto notes = notesOnly (run (m, r).events);
    REQUIRE (! notes.empty());
    CHECK (notes.front().offset == 30000); // starts on the key, not on a grid
    CHECK (notes.front().type == vs::EventType::NoteOn);
    CHECK (notes.back().type == vs::EventType::NoteOff);
    CHECK (notes.back().offset < 60500 + 6000); // the last gate finishes, nothing more starts
    for (const auto& e : notes)
        CHECK (e.offset >= 30000);
    // 1/16 at 120 bpm is 6000 samples: steps at 30000, 36000, ..., 60000; the key goes up at 60500.
    int ons = 0;
    for (const auto& e : notes)
        ons += e.type == vs::EventType::NoteOn ? 1 : 0;
    CHECK (ons == 6);
}

TEST_CASE ("Timing, internal clock: a new key after silence restarts the clock at that key", "[seq][timing]")
{
    ArpSeq m;
    Run r;
    r.sr = 48000;
    r.total = 48000 * 4;
    r.block = 100;
    r.segs = { HostSeg { 0, false, 12000, 0 } };
    Settings s = arpSettings (4, 8, 8);
    s.internalBpm = 120.0;
    r.changes = { Change { 0, s } };
    r.input = { noteOn (1000, 60), noteOff (7000, 60), noteOn (100123, 62), noteOff (120000, 62) };
    const auto notes = notesOnly (run (m, r).events);
    std::vector<std::int64_t> onsAfter;
    for (const auto& e : notes)
        if (e.type == vs::EventType::NoteOn && e.offset >= 100000)
            onsAfter.push_back (e.offset);
    REQUIRE (onsAfter.size() >= 3);
    CHECK (onsAfter[0] == 100123);
    CHECK (onsAfter[1] == 100123 + 6000);
    CHECK (onsAfter[2] == 100123 + 12000);
}

TEST_CASE ("Timing, internal tempo changes between blocks continue from the current position", "[seq][timing]")
{
    // 1/4 steps: 24000 samples at 120 bpm. At sample 36000 (half way through step 1) the tempo
    // becomes 60 bpm: the position is 1.5 quarters, so step 2 (position 2.0) is 0.5 quarter =
    // 24000 samples later (at 60 bpm a quarter is 48000 samples), at 60000.
    ArpSeq m;
    Run r;
    r.sr = 48000;
    r.total = 130000;
    r.block = 500;
    r.segs = { HostSeg { 0, false, 12000, 0 } };
    Settings s = arpSettings (1, 8, 4);
    s.internalBpm = 120.0;
    Settings s2 = s;
    s2.internalBpm = 60.0;
    r.changes = { Change { 0, s }, Change { 36000, s2 } };
    r.input = { noteOn (0, 60) };
    std::vector<std::int64_t> ons;
    for (const auto& e : run (m, r).events)
        if (e.type == vs::EventType::NoteOn)
            ons.push_back (e.offset);
    REQUIRE (ons.size() >= 4);
    CHECK (ons[0] == 0);
    CHECK (ons[1] == 24000);
    CHECK (ons[2] == 60000);
    CHECK (ons[3] == 108000);
}

TEST_CASE ("Timing: transport start mid-bar and stop/start with the key held, the clock follows the source", "[seq][timing]")
{
    Rng rng (31337);
    int checked = 0;
    for (int i = 0; i < 60; ++i)
    {
        MCfg c;
        c.sr = kSampleRates[rng.below (6)];
        c.d = kD[rng.below (6)];
        c.swing16 = 8 + rng.below (5);
        c.gate16 = (i % 2 == 0) ? 16 : 1 + rng.below (16);
        const int block = kBlocks[i % 6];
        const std::int64_t hostBpm = kTempi[rng.below (8)];
        const std::int64_t intBpm = 3000 + rng.below (20000);
        const std::int64_t len = block == 1 ? c.sr / 8 : c.sr / 2;
        const std::int64_t keyDown = rng.below (500);
        const std::int64_t a1 = 6144 * 40 + rng.below (6144 * 4); // host start mid-bar
        const std::int64_t a2 = 6144 * 3 + rng.below (6144 * 4);

        // stopped (internal) -> playing (host) -> stopped (internal) with the key held throughout.
        std::vector<HostSeg> hs { HostSeg { 0, false, 12000, 0 }, HostSeg { len, true, hostBpm, a1 }, HostSeg { 2 * len, false, 12000, 0 },
                                  HostSeg { 3 * len, true, hostBpm, a2 } };
        std::vector<MSeg> ms { MSeg { keyDown, len - keyDown, intBpm, 0, true }, MSeg { len, len, hostBpm, a1, true },
                               MSeg { 2 * len, len, intBpm, 0, true }, MSeg { 3 * len, len, hostBpm, a2, true } };
        ArpSeq m;
        Run r;
        r.sr = c.sr;
        r.total = 4 * len;
        r.block = block;
        r.segs = hs;
        Settings s = arpSettings (c.d, c.swing16, c.gate16);
        s.internalBpm = static_cast<double> (intBpm) / 100.0;
        r.changes = { Change { 0, s } };
        r.input = { noteOn (static_cast<int> (keyDown), 60) };
        const RunResult res = run (m, r);
        std::int64_t steps = 0;
        const auto expected = model (c, ms, keyDown, { MStep {} }, steps);
        std::string why;
        const bool ok = res.wellFormed && matchesModel (res.events, expected, 4 * len, &why);
        INFO (describe (c, hostBpm, block) << " internal " << intBpm << ": " << why);
        CHECK (ok);
        ++checked;
    }
    CHECK (checked == 60);
}

TEST_CASE ("Timing: a rate or swing change acts at the next block on the new grid", "[seq][timing]")
{
    // 120 bpm, 48 kHz. 1/4 steps (24000 samples), gate 50 %: on @0, off @12000, on @24000, off @36000.
    // From sample 30000 the rate is 1/8 (12000 samples): the next grid points are 36000, 48000, ...
    ArpSeq m;
    Run r;
    r.sr = 48000;
    r.total = 80000;
    r.block = 256;
    r.segs = { HostSeg { 0, true, 12000, 0 } };
    r.changes = { Change { 0, arpSettings (1, 8, 8) }, Change { 30000, arpSettings (2, 8, 8) } };
    r.input = { noteOn (0, 60) };
    const auto notes = notesOnly (run (m, r).events);
    const std::vector<std::pair<std::int64_t, bool>> expected { { 0, true }, { 12000, false }, { 24000, true }, { 36000, false },
                                                                { 36000, true }, { 42000, false }, { 48000, true }, { 54000, false },
                                                                { 60000, true }, { 66000, false }, { 72000, true }, { 78000, false } };
    REQUIRE (notes.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        INFO ("event " << i);
        CHECK (notes[i].offset == expected[i].first);
        CHECK ((notes[i].type == vs::EventType::NoteOn) == expected[i].second);
    }
}

TEST_CASE ("Timing: a host position that jitters slightly (tempo ramps inside blocks) neither doubles nor drops steps", "[seq][timing]")
{
    // 120 bpm, 48 kHz, 1/16 (6000 samples), 480-sample blocks. Every block reports its position off by up to
    // +-0.004 quarter notes (about 96 samples), well under the jump tolerance of 1/64.
    for (const std::uint32_t seed : { 1u, 2u, 3u, 4u, 5u })
    {
        std::uint32_t r = seed * 7919u;
        const auto jitter = [&r] {
            r = r * 1664525u + 1013904223u;
            return (static_cast<double> ((r >> 8) % 2001u) - 1000.0) / 1000.0 * 0.004;
        };
        ArpSeq m;
        m.setSettings (arpSettings (4, 8, 8));
        std::vector<NoteEvent> out (256);
        std::vector<std::int64_t> ons;
        SoundLedger led;
        for (int block = 0; block < 1000; ++block)
        {
            const std::vector<NoteEvent> in = block == 0 ? std::vector<NoteEvent> { noteOn (0, 60) } : std::vector<NoteEvent> {};
            const double ppq = 2.0 + static_cast<double> (block) * 480.0 / 24000.0 + (block == 0 ? 0.0 : jitter());
            const int count = m.process (in, Transport { 48000.0, 120.0, ppq, true }, 480, out);
            for (int i = 0; i < count; ++i)
            {
                const NoteEvent& e = out[static_cast<std::size_t> (i)];
                led.feed (e);
                if (e.type == vs::EventType::NoteOn)
                    ons.push_back (static_cast<std::int64_t> (block) * 480 + e.offset);
            }
        }
        // 1000 blocks = 480000 samples = 80 steps of 6000 (positions 2.0 + 0.25 n).
        INFO ("seed " << seed);
        CHECK (ons.size() >= 79);
        CHECK (ons.size() <= 81);
        CHECK (led.doubleOns == 0);
        CHECK (led.orphanOffs == 0);
        for (std::size_t i = 1; i < ons.size(); ++i)
        {
            CHECK (ons[i] - ons[i - 1] > 5000);
            CHECK (ons[i] - ons[i - 1] < 7000);
        }
    }
}

TEST_CASE ("Timing: a block far longer than any host sends is handled in bounded work", "[seq][timing]")
{
    ArpSeq m;
    Settings s = arpSettings (8, 8, 8);
    s.internalBpm = 300.0;
    m.setSettings (s);
    constexpr int n = 20'000'000; // 7 minutes at 48 kHz: 20000 steps of 1/32 at 300 bpm
    std::vector<NoteEvent> out (100000);
    const std::vector<NoteEvent> in { noteOn (0, 60) };
    const int count = m.process (in, Transport { 48000.0, 120.0, 0.0, false }, n, out);
    CHECK (count > 1000);
    CHECK (count <= 2 * ArpSeq::kMaxStepsPerCall + 4); // capped
    SoundLedger led;
    for (int i = 0; i < count; ++i)
        led.feed (out[static_cast<std::size_t> (i)]);
    CHECK (led.doubleOns == 0);
    CHECK (led.sounding.empty()); // the last gate ended inside the block
    // And the module is usable afterwards.
    const int next = m.process ({}, Transport { 48000.0, 120.0, 0.0, false }, 4800, out);
    for (int i = 0; i < next; ++i)
        led.feed (out[static_cast<std::size_t> (i)]);
    CHECK (led.doubleOns == 0);
}

TEST_CASE ("Timing: gate 100 % releases exactly at the next step and the off precedes the on of the same pitch", "[seq][timing]")
{
    for (int swing16 : { 8, 10, 12 })
    {
        ArpSeq m;
        Run r;
        r.sr = 44100;
        r.total = 44100 * 2;
        r.block = 97;
        r.segs = { HostSeg { 0, true, 13337, 6144 * 5 + 11 } };
        r.changes = { Change { 0, arpSettings (4, swing16, 16) } };
        r.input = { noteOn (0, 60) };
        const auto res = run (m, r);
        const auto notes = notesOnly (res.events);
        REQUIRE (notes.size() > 10);
        // Strictly alternating off/on pairs at identical offsets, the off first.
        for (std::size_t i = 1; i + 1 < notes.size(); i += 2)
        {
            INFO ("swing16 " << swing16 << " pair " << i);
            CHECK (notes[i].type == vs::EventType::NoteOff);
            CHECK (notes[i + 1].type == vs::EventType::NoteOn);
            CHECK (notes[i].offset == notes[i + 1].offset);
        }
    }
}
