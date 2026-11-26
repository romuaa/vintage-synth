// Wave 3 integration, arpeggiator/sequencer through the processor and the real parts
// (docs/architecture.md 6a.3 item 2). The module was tested against a model in M4; here:
// - step timing through the processor against musical time computed in the test (long double,
//   from the host timeline), at 48 kHz (2x), 96 kHz and 192 kHz (1x) and several block sizes;
// - host transport through a test play head: start, stop, loop jump, tempo change between blocks,
//   a tempo ramp (drift), missing play head and missing fields;
// - chunked rendering (host block > prepared size) is bit-identical to the same chunks delivered
//   as host blocks, whose steps are checked against the grid;
// - the real parts: pedal consumed (no pile-up in Poly), Off <-> on hand-over with held keys and
//   pedal, legato glide in Solo at 100 % gate, Hold, arp.target in Whole / Dual / Split, key-mode
//   change with the arp running, the 3 ms steal fade with fast arpeggios at 16 voices;
// - random streams never hang a note; block-size independence with arp and effects on; no
//   allocation in processBlock.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "IntegrationRig.h"
#include "vintage/params/PartMappings.h"
#include "vintage/params/ParameterLayout.h"

#if defined(__APPLE__)
 #include "../seq_unit/AllocationCounter.h"
#endif

#include <cstdio>
#include <random>

using namespace vs::itest;

namespace
{
/** Arp on Lower (Whole mode): Up, 1/16, gate 50 %, one key. */
void arpBasics (Rig& rig, int rate = 3)
{
    rig.set ("arp.mode", 1);
    rig.set ("arp.target", 0);
    rig.set ("arp.rate", static_cast<float> (rate));
}

/** Grid position (quarters) of step k: odd steps delayed by (swing - 0.5) of a step pair. */
double gridPpq (long long k, int spq, double swing)
{
    const double off = (k & 1) != 0 ? 2.0 * swing - 1.0 : 0.0;
    return (static_cast<double> (k) + off) / static_cast<double> (spq);
}

/** The internal clock: ppq 0 at sample `start`, `bpm`. */
long long internalStep (long long k, long long start, double bpm, double fs, int spq = 4, double swing = 0.5)
{
    return expectedSample (gridPpq (k, spq, swing), start, 0.0, bpm, fs);
}

std::string listOf (const std::vector<long long>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size() && i < 40; ++i)
        s += std::to_string (v[i]) + " ";
    return s;
}

int busyVoices (const vs::Part& part)
{
    int n = 0;
    for (int v = 0; v < vs::kMaxVoices; ++v)
        n += part.getAllocator().isVoiceBusy (v) ? 1 : 0;
    return n;
}
} // namespace

// =================================================================================================
// Step timing and host transport
// =================================================================================================

TEST_CASE ("Arp through the processor: steps on the host grid, sample-exact, at every rate configuration", "[wave3][arp][timing][slow]")
{
    const double fs = GENERATE (48000.0, 96000.0, 192000.0);
    const double bpm = GENERATE (120.0, 133.7);
    const double swing = GENERATE (0.5, 0.62);
    const int block = GENERATE (512, 61, 1);
    if (block == 1 && (swing > 0.5 || fs > 100000.0))
        return; // block size 1: one combination per rate is enough for the run time
    INFO ("rate " << fs << ", bpm " << bpm << ", swing " << swing << ", block " << block);

    Rig rig (fs, 512);
    arpBasics (rig);
    rig.set ("arp.swing", static_cast<float> (swing * 100.0));
    rig.prepare();
    const double startPpq = 2.37; // playback starts mid-bar
    rig.play (bpm, startPpq);
    const int key = rig.samples (0.1);
    const int total = rig.samples (block == 1 ? 0.6 : 1.2);
    rig.render (total, { noteOn (key, 60) }, block);

    // With the host playing, the key waits for the next grid step; then every step plays.
    std::vector<long long> wantOn, wantOff;
    for (long long k = 0; k < 400; ++k)
    {
        const auto p0 = gridPpq (k, 4, swing), p1 = gridPpq (k + 1, 4, swing);
        const auto s = expectedSample (p0, 0, startPpq, bpm, fs);
        if (s < key || p0 < startPpq)
            continue;
        if (s >= total)
            break;
        wantOn.push_back (s);
        const auto off = expectedSample (p0 + 0.5 * (p1 - p0), 0, startPpq, bpm, fs);
        if (off < total)
            wantOff.push_back (off);
    }
    const auto gotOn = rig.noteOnPositions (0);
    std::vector<long long> gotOff;
    for (const auto& a : rig.partEvents[0])
        if (a.e.type == vs::EventType::NoteOff)
            gotOff.push_back (a.pos);
    INFO ("want " << listOf (wantOn));
    INFO ("got  " << listOf (gotOn));
    CHECK (wantOn.size() >= 3);
    CHECK (gotOn == wantOn);
    CHECK (gotOff == wantOff);
    CHECK (rig.partEvents[1].empty()); // Whole mode, Upper gets nothing
}

TEST_CASE ("Host transport: start, stop, loop jump, tempo change and missing information", "[wave3][arp][transport][slow]")
{
    const double fs = GENERATE (48000.0, 96000.0, 192000.0);
    const int block = GENERATE (512, 37);
    INFO ("rate " << fs << ", block " << block);
    const double internalBpm = 100.0;

    auto makeRig = [&] {
        auto rig = std::make_unique<Rig> (fs, 512);
        arpBasics (*rig);
        rig->set ("arp.tempo", static_cast<float> (internalBpm));
        rig->prepare();
        return rig;
    };

    SECTION ("stopped: internal clock from the key; host start: the host grid from its position")
    {
        auto rig = makeRig();
        const int key = rig->samples (0.05), start = rig->samples (0.6);
        const int total = rig->samples (1.2);
        rig->render (total, { noteOn (key, 60) }, block, { { start, [] (Rig& r) { r.play (120.0, 0.0); } } });
        std::vector<long long> want;
        for (long long k = 0;; ++k)
        {
            const auto s = internalStep (k, key, internalBpm, fs);
            if (s >= start)
                break;
            want.push_back (s);
        }
        for (long long k = 0;; ++k)
        {
            const auto s = expectedSample (gridPpq (k, 4, 0.5), start, 0.0, 120.0, fs);
            if (s >= total)
                break;
            want.push_back (s);
        }
        INFO ("got " << listOf (rig->noteOnPositions (0)));
        CHECK (rig->noteOnPositions (0) == want);
    }
    SECTION ("host stop: released at the stop, then the internal clock from there")
    {
        auto rig = makeRig();
        rig->play (120.0, 0.0);
        const int key = rig->samples (0.02), stop = rig->samples (0.7);
        const int total = rig->samples (1.4);
        rig->render (total, { noteOn (key, 60) }, block, { { stop, [] (Rig& r) { r.stop(); } } });
        std::vector<long long> want;
        for (long long k = 0;; ++k)
        {
            const auto s = expectedSample (gridPpq (k, 4, 0.5), 0, 0.0, 120.0, fs);
            if (s >= stop)
                break;
            if (s >= key)
                want.push_back (s);
        }
        for (long long k = 0;; ++k)
        {
            const auto s = internalStep (k, stop, internalBpm, fs);
            if (s >= total)
                break;
            want.push_back (s);
        }
        INFO ("got " << listOf (rig->noteOnPositions (0)));
        CHECK (rig->noteOnPositions (0) == want);
    }
    SECTION ("loop jump back: what sounds is released at the jump, the grid restarts there")
    {
        auto rig = makeRig();
        rig->set ("arp.gate", 100.0f); // a note always sounds: the jump must release it
        rig->play (120.0, 0.0);
        const int jump = rig->samples (0.8125); // ppq 1.625: mid-step
        const int total = rig->samples (1.5);
        rig->render (total, { noteOn (0, 60), noteOn (0, 64) }, block, { { jump, [] (Rig& r) { r.locate (0.0); } } });
        std::vector<long long> want;
        for (long long k = 0;; ++k)
        {
            const auto s = expectedSample (gridPpq (k, 4, 0.5), 0, 0.0, 120.0, fs);
            if (s >= jump)
                break;
            want.push_back (s);
        }
        for (long long k = 0;; ++k)
        {
            const auto s = expectedSample (gridPpq (k, 4, 0.5), jump, 0.0, 120.0, fs);
            if (s >= total)
                break;
            want.push_back (s);
        }
        CHECK (rig->noteOnPositions (0) == want);
        // At the jump: the sounding note is released before the new step's note starts.
        int offsAtJump = 0;
        for (const auto& a : rig->partEvents[0])
            offsAtJump += a.pos == jump && a.e.type == vs::EventType::NoteOff ? 1 : 0;
        CHECK (offsAtJump >= 1);
    }
    SECTION ("tempo change between blocks: the grid continues at the new tempo, nothing released")
    {
        auto rig = makeRig();
        rig->play (120.0, 0.0);
        const int change = rig->samples (0.53); // ppq 1.06, mid-step
        const int total = rig->samples (1.5);
        rig->render (total, { noteOn (0, 60) }, block, { { change, [] (Rig& r) { r.tempo (90.0); } } });
        const double ppqAtChange = 0.53 * 2.0;
        std::vector<long long> want;
        for (long long k = 0;; ++k)
        {
            const double p = gridPpq (k, 4, 0.5);
            const auto s = p < ppqAtChange ? expectedSample (p, 0, 0.0, 120.0, fs) : expectedSample (p, change, ppqAtChange, 90.0, fs);
            if (s >= total)
                break;
            want.push_back (s);
        }
        CHECK (rig->noteOnPositions (0) == want);
        // Gate 50 %: every note-off half a step after its note-on, nothing extra at the change.
        std::vector<long long> wantOff, gotOff;
        for (long long k = 0;; ++k)
        {
            const double p = gridPpq (k, 4, 0.5) + 0.125;
            const auto s = p < ppqAtChange ? expectedSample (p, 0, 0.0, 120.0, fs) : expectedSample (p, change, ppqAtChange, 90.0, fs);
            if (s >= total)
                break;
            wantOff.push_back (s);
        }
        for (const auto& a : rig->partEvents[0])
            if (a.e.type == vs::EventType::NoteOff)
                gotOff.push_back (a.pos);
        CHECK (gotOff == wantOff);
    }
    SECTION ("missing play head, position or fields: the internal clock")
    {
        for (int variant = 0; variant < 4; ++variant)
        {
            INFO ("variant " << variant);
            auto rig = makeRig();
            rig->play (150.0, 3.0);
            if (variant == 0)
                rig->transport.attached = false;
            if (variant == 1)
                rig->transport.hasPosition = false;
            if (variant == 2)
                rig->transport.reportBpm = false;
            if (variant == 3)
                rig->transport.reportPpq = false;
            const int key = rig->samples (0.11);
            const int total = rig->samples (0.9);
            rig->render (total, { noteOn (key, 60) }, block);
            std::vector<long long> want;
            for (long long k = 0;; ++k)
            {
                const auto s = internalStep (k, key, internalBpm, fs);
                if (s >= total)
                    break;
                want.push_back (s);
            }
            CHECK (rig->noteOnPositions (0) == want);
            CHECK_FALSE (rig->proc.getLastTransport().playing);
        }
    }
}

TEST_CASE ("Host transport: a tempo ramp is drift, not a jump: no step repeated, skipped or cut", "[wave3][arp][transport]")
{
    const double fs = GENERATE (48000.0, 192000.0);
    const int block = GENERATE (256, 1024);
    INFO ("rate " << fs << ", block " << block);
    Rig rig (fs, 1024);
    arpBasics (rig);
    rig.prepare();
    // The host's tempo rises linearly from 100 to 160 bpm over 2 s; it reports the tempo at the
    // block start and the true (integrated) position, so every block start deviates a little
    // from what the previous block predicted.
    const double t1 = 2.0, b0 = 100.0, b1 = 160.0;
    auto bpmAt = [&] (double t) { return b0 + (b1 - b0) * std::min (t, t1) / t1; };
    auto ppqAt = [&] (double t) {
        const double tr = std::min (t, t1);
        return (b0 * tr + 0.5 * (b1 - b0) * tr * tr / t1) / 60.0 + (t > t1 ? b1 * (t - t1) / 60.0 : 0.0);
    };
    auto follow = [&] (Rig& r) {
        const double t = static_cast<double> (r.now) / fs;
        r.transport.anchor = r.now;
        r.transport.anchorPpq = ppqAt (t);
        r.transport.bpm = bpmAt (t);
    };
    rig.play (b0, 0.0);
    const int total = rig.samples (2.5);
    rig.render (total, { noteOn (0, 60) }, block, {}, follow);

    const auto on = rig.noteOnPositions (0);
    // One note per grid step passed: steps k with ppq k/4 inside the render.
    const auto steps = static_cast<size_t> (std::floor (ppqAt (2.5) * 4.0 - 1.0e-9)) + 1;
    CHECK (on.size() == steps);
    // Each step within one block of its true time, and in order.
    for (size_t k = 0; k < on.size(); ++k)
    {
        // True time of ppq k/4 (invert ppqAt numerically).
        double lo = 0.0, hi = 2.5;
        for (int it = 0; it < 60; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            (ppqAt (mid) < static_cast<double> (k) / 4.0 ? lo : hi) = mid;
        }
        const double trueSample = hi * fs;
        INFO ("step " << k);
        CHECK (std::abs (static_cast<double> (on[k]) - trueSample) <= static_cast<double> (block) + 1.0);
        if (k > 0)
            CHECK (on[k] > on[k - 1]);
    }
    // Gate 50 %: never cut early (no release at a block start) -> one note-off per note-on.
    CHECK (rig.countType (0, vs::EventType::NoteOff) == static_cast<int> (on.size()));
}

TEST_CASE ("Chunked rendering: a host block above the prepared size keeps the step timing", "[wave3][arp][transport][slow]")
{
    const double fs = GENERATE (48000.0, 96000.0);
    const int big = GENERATE (1500, 5000);
    INFO ("rate " << fs << ", host block " << big);
    auto setup = [&] (Rig& r) {
        arpBasics (r, 5); // 1/32: several steps per chunk
        r.set ("arp.swing", 58.0f);
        r.set ("arp.octaves", 2);
        r.set ("fx.delay.level", 0.3f);
        r.set ("fx.reverb.level", 0.2f);
        r.prepare();
        r.play (128.0, 0.75);
    };
    const int total = big * 40;
    const int tempoAt = big * 20; // a host block boundary
    std::vector<Midi> midi { noteOn (100, 60), noteOn (200, 67), noteOff (big * 33 + 17, 60), noteOff (big * 33 + 17, 67) };

    // Chunked: host blocks of `big`, the processor renders them in 512-sample chunks.
    Rig chunked (fs, 512);
    setup (chunked);
    const auto a = chunked.render (total, midi, big, { { tempoAt, [] (Rig& r) { r.tempo (97.0); } } });
    CHECK_FALSE (chunked.eventsRecorded);

    // Reference: the same chunks delivered as host blocks (512, 512, ..., rest), which report
    // their own positions; their steps are checked against the grid below.
    Rig ref (fs, 512);
    setup (ref);
    std::vector<Rig::Change> splits { { tempoAt, [] (Rig& r) { r.tempo (97.0); } } };
    for (int b = big; b < total; b += big)
        if (b != tempoAt)
            splits.push_back ({ b, [] (Rig&) {} });
    const auto b = ref.render (total, midi, 512, splits);
    REQUIRE (ref.eventsRecorded);
    CHECK (bitIdentical (a, b));

    std::vector<long long> want;
    const double ppqAtTempo = 0.75 + static_cast<double> (tempoAt) * 128.0 / (60.0 * fs);
    for (long long k = 0;; ++k)
    {
        const double p = gridPpq (k, 8, 0.58);
        if (p < 0.75)
            continue;
        const auto s = p < ppqAtTempo ? expectedSample (p, 0, 0.75, 128.0, fs) : expectedSample (p, tempoAt, ppqAtTempo, 97.0, fs);
        if (s >= big * 33 + 17)
            break;
        if (s >= 100)
            want.push_back (s);
    }
    // The second key arrives at 200: the steps from 100 on are the arpeggio's; all on the grid.
    CHECK (ref.noteOnPositions (0, 0, big * 33 + 17) == want);
}

// =================================================================================================
// Through the real parts
// =================================================================================================

TEST_CASE ("Arp and pedal through a Poly part: pedal consumed, no pile-up, clean hand-over", "[wave3][arp][pedal]")
{
    const double fs = GENERATE (48000.0, 96000.0, 192000.0);
    INFO ("rate " << fs);
    Rig rig (fs, 512);
    arpBasics (rig);
    rig.prepare();
    const auto& part = rig.proc.getLowerPart();
    int maxVoices = 0;
    bool partPedalSeen = false;
    auto watch = [&] (Rig& r) {
        maxVoices = std::max (maxVoices, busyVoices (r.proc.getLowerPart()));
        partPedalSeen = partPedalSeen || r.proc.getLowerPart().isSustainDown();
    };

    SECTION ("arp on: pedal held, keys released, the pattern goes on without piling up voices")
    {
        const int up = rig.samples (2.0);
        rig.render (rig.samples (3.0),
                    { pedal (0, true), noteOn (10, 60), noteOn (10, 64), noteOn (10, 67), noteOff (rig.samples (0.4), 60),
                      noteOff (rig.samples (0.4), 64), noteOff (rig.samples (0.4), 67), pedal (up, false) },
                    100, {}, watch);
        // The keys stay in the pattern while the pedal is down: steps continue after their release.
        CHECK (rig.noteOnPositions (0, rig.samples (0.5), up).size() >= 10);
        // Gate 50 % of 125 ms and a 63 ms release: at most two voices overlap. Passing the pedal
        // on would have held every note (16 voices).
        std::printf ("pedal consumed (arp on, Poly, %.0f Hz): most voices busy at once %d, part pedal seen %s, %d arp notes while pedal-held\n",
                     fs, maxVoices, partPedalSeen ? "yes" : "no", static_cast<int> (rig.noteOnPositions (0, rig.samples (0.5), up).size()));
        CHECK (maxVoices <= 2);
        CHECK_FALSE (partPedalSeen);
        CHECK (rig.countType (0, vs::EventType::SustainPedal) == 0);
        // Pedal up: the released keys leave the pattern; everything ends.
        CHECK (rig.noteOnPositions (0, up + 1).empty());
        CHECK_FALSE (part.isSounding());
        CHECK_FALSE (rig.proc.getArpSeq (0).isSounding());
        CHECK (rig.allReleased (0));
    }
    SECTION ("Off -> on with pedal-held notes; on -> Off hands the pedal back")
    {
        rig.set ("arp.mode", 0);
        const int on = rig.samples (0.5), off = rig.samples (1.5), up = rig.samples (2.0);
        rig.render (rig.samples (2.6),
                    { pedal (0, true), noteOn (10, 60), noteOn (10, 64), noteOff (rig.samples (0.2), 60),
                      noteOff (rig.samples (0.2), 64), noteOn (rig.samples (1.7), 72), noteOff (rig.samples (1.8), 72),
                      pedal (up, false) },
                    100,
                    { { on, [] (Rig& r) { r.set ("arp.mode", 1); } }, { off, [] (Rig& r) { r.set ("arp.mode", 0); } } },
                    [&] (Rig& r) {
                        if (r.now > on + 200 && r.now <= off)
                            watch (r);
                        if (r.now == on)
                            CHECK (busyVoices (r.proc.getLowerPart()) == 2); // the pedal holds the chord
                    });
        // Switching on: the part's pedal is lifted (it releases the held chord) and the pedal-held
        // keys are arpeggiated.
        CHECK (rig.noteOnPositions (0, on, off).size() >= 6);
        CHECK (maxVoices <= 2);
        CHECK_FALSE (partPedalSeen);
        // Switching Off with the pedal still down: the part gets the pedal back, so the key played
        // at 1.7 s rings until the pedal comes up at 2.0 s.
        int pedalDownAtOff = 0;
        for (const auto& a : rig.partEvents[0])
            pedalDownAtOff += a.pos == off && a.e.type == vs::EventType::SustainPedal && a.e.value > 0.5f ? 1 : 0;
        CHECK (pedalDownAtOff == 1);
        CHECK (rig.noteOnPositions (0, off + 1) == std::vector<long long> { rig.samples (1.7) });
        CHECK_FALSE (part.isSounding());
        CHECK_FALSE (part.isSustainDown());
        CHECK_FALSE (rig.proc.getArpSeq (0).isSounding());
    }
}

TEST_CASE ("Arp Off <-> on with held keys through the part: handed over, nothing hangs", "[wave3][arp]")
{
    Rig rig (48000.0, 512);
    rig.set ("arp.target", 0);
    rig.prepare();
    const int on = 24000, off = 60000, release = 80000;
    std::vector<int> voicesAtOn;
    rig.render (120000,
                { noteOn (100, 60), noteOn (100, 64), noteOn (100, 67), noteOff (release, 60), noteOff (release, 64),
                  noteOff (release, 67), noteOn (90000, 72), noteOff (95000, 72) },
                128,
                { { on, [] (Rig& r) { r.set ("arp.mode", 1); } }, { off, [] (Rig& r) { r.set ("arp.mode", 0); } } },
                [&] (Rig& r) {
                    if (r.now == on)
                        voicesAtOn.push_back (busyVoices (r.proc.getLowerPart()));
                });
    REQUIRE (voicesAtOn.size() == 1);
    CHECK (voicesAtOn[0] == 3); // Off: the chord itself
    // At the switch the chord is released (note-offs at `on`), then the arpeggio plays the held keys.
    int offsAtOn = 0;
    for (const auto& a : rig.partEvents[0])
        offsAtOn += a.pos == on && a.e.type == vs::EventType::NoteOff ? 1 : 0;
    CHECK (offsAtOn == 3);
    const auto arp = rig.noteOnPositions (0, on, off);
    CHECK (arp.size() == static_cast<size_t> ((off - on + 5999) / 6000)); // internal clock from the switch: on, on + 6000, ...
    for (size_t i = 0; i < arp.size(); ++i)
        CHECK (arp[i] == on + 6000 * static_cast<long long> (i));
    // Switching Off: the arpeggio's note is released; the held keys are not re-sent (they play
    // again when pressed again), and the new key plays normally.
    CHECK (rig.noteOnPositions (0, off, 90000).empty());
    CHECK (rig.noteOnPositions (0, 90000) == std::vector<long long> { 90000 });
    CHECK (rig.allReleased (0));
    CHECK_FALSE (rig.proc.getLowerPart().isSounding());
    CHECK_FALSE (rig.proc.getArpSeq (0).isSounding());
}

TEST_CASE ("Legato at 100 % gate: a Solo part with Legato portamento glides between arpeggio notes", "[wave3][arp][legato]")
{
    const double fs = GENERATE (48000.0, 96000.0, 192000.0);
    INFO ("rate " << fs);
    auto pitchAfterStep = [&] (float gatePercent, bool& overlapSeen) {
        Rig rig (fs, 512);
        arpBasics (rig, 1); // 1/8 at 120 bpm: 250 ms steps
        rig.set ("arp.gate", gatePercent);
        rig.set ("lower.assign.mode", 2); // Solo
        rig.set ("lower.porta.mode", 1);  // Legato: glides only over held (overlapping) notes
        rig.set ("lower.porta.time", vs::map::portaFromSec (0.2f));
        rig.prepare();
        // Keys 60 and 67: the arpeggio alternates 60, 67, 60, ...
        rig.render (rig.samples (0.5) + 480 * static_cast<int> (fs / 48000.0), { noteOn (0, 60), noteOn (0, 67) }, 32);
        overlapSeen = false;
        const auto& ev = rig.partEvents[0];
        for (size_t i = 0; i + 1 < ev.size(); ++i)
            overlapSeen = overlapSeen
                          || (ev[i].pos == ev[i + 1].pos && ev[i].e.type == vs::EventType::NoteOn && ev[i + 1].e.type == vs::EventType::NoteOff
                              && ev[i].e.note != ev[i + 1].e.note);
        // 10 ms after the third step (0.5 s, note 60 after 67): where is the pitch?
        const auto& part = rig.proc.getLowerPart();
        for (int v = 0; v < vs::kMaxVoices; ++v)
            if (part.getAllocator().isVoiceBusy (v))
                return static_cast<double> (part.getVoice (v).getPitch());
        return -1.0;
    };
    bool overlap100 = false, overlap95 = false;
    const double legato = pitchAfterStep (100.0f, overlap100);
    const double detached = pitchAfterStep (95.0f, overlap95);
    CHECK (overlap100);  // note-on of the next pitch before the previous note-off, same sample
    CHECK_FALSE (overlap95);
    INFO ("pitch 10 ms after the step: legato " << legato << ", detached " << detached);
    std::printf ("legato at 100 %% gate, %.0f Hz: pitch 10 ms after the 67 -> 60 step %.3f (gate 95 %%: %.3f)\n", fs, legato, detached);
    CHECK (legato > 61.0); // still gliding down from 67 (0.2 s glide)
    CHECK (legato < 66.5);
    CHECK (std::abs (detached - 60.0) < 0.01); // no overlap: no glide in Legato mode
}

TEST_CASE ("Sequencer through the processor: steps transposed by the key, tie and rest, step velocity", "[wave3][arp][seq]")
{
    Rig rig (48000.0, 512);
    rig.set ("arp.mode", 2);
    rig.set ("arp.target", 0);
    rig.set ("arp.rate", 3);  // 1/16 at 120 bpm (internal): 6000 samples per step
    rig.set ("arp.gate", 50.0f);
    rig.set ("seq.length", 4);
    const int types[] { 0, 2, 1, 0 };  // Note, Tie, Rest, Note
    const int notes[] { 0, 5, 0, 7 };  // the tie keeps the previous pitch
    const float vels[] { 0.5f, 1.0f, 1.0f, 0.9f };
    for (int st = 0; st < 4; ++st)
    {
        rig.set (vs::seqStepParamID (st + 1, vs::SeqStepField::Type), static_cast<float> (types[st]));
        rig.set (vs::seqStepParamID (st + 1, vs::SeqStepField::Note), static_cast<float> (notes[st]));
        rig.set (vs::seqStepParamID (st + 1, vs::SeqStepField::Velocity), vels[st]);
    }
    rig.prepare();
    rig.render (8 * 6000, { noteOn (0, 62) }, 128); // D4: the sequence is transposed up a tone
    std::vector<std::pair<long long, int>> ons;
    std::vector<long long> offs;
    for (const auto& a : rig.partEvents[0])
    {
        if (a.e.type == vs::EventType::NoteOn)
        {
            ons.push_back ({ a.pos, a.e.note });
            CHECK (std::abs (a.e.value - (a.e.note == 62 ? 0.5f : 0.9f)) < 1.0e-6f);
        }
        if (a.e.type == vs::EventType::NoteOff)
            offs.push_back (a.pos);
    }
    const std::vector<std::pair<long long, int>> wantOns { { 0, 62 }, { 18000, 69 }, { 24000, 62 }, { 42000, 69 } };
    CHECK (ons == wantOns);
    // The tie extends the first note through step 2's gate (no retrigger); step 3 is a rest.
    const std::vector<long long> wantOffs { 9000, 21000, 33000, 45000 };
    CHECK (offs == wantOffs);
}

TEST_CASE ("Hold: the pattern latches after release; a new chord after all keys were up starts a new one", "[wave3][arp]")
{
    Rig rig (48000.0, 512);
    arpBasics (rig);
    rig.set ("arp.hold", 1);
    rig.prepare();
    const int release = 12000, newChord = 48000, holdOff = 84000;
    rig.render (110000,
                { noteOn (0, 60), noteOn (0, 64), noteOff (release, 60), noteOff (release, 64), noteOn (newChord, 70),
                  noteOn (newChord, 74), noteOff (newChord + 3000, 70), noteOff (newChord + 3000, 74) },
                256, { { holdOff, [] (Rig& r) { r.set ("arp.hold", 0); } } });
    std::vector<int> notesBefore, notesAfter;
    for (const auto& a : rig.partEvents[0])
        if (a.e.type == vs::EventType::NoteOn)
            (a.pos < newChord ? notesBefore : notesAfter).push_back (a.e.note);
    // Latched: steps continue after the release, with the old notes only until the new chord.
    CHECK (rig.noteOnPositions (0, release + 1, newChord).size() >= 5);
    for (const int n : notesBefore)
        CHECK ((n == 60 || n == 64));
    for (const int n : notesAfter)
        CHECK ((n == 70 || n == 74));
    // Hold off with no key down: the pattern empties and everything ends.
    CHECK (rig.noteOnPositions (0, holdOff + 1).empty());
    CHECK_FALSE (rig.proc.getArpSeq (0).isSounding());
    CHECK_FALSE (rig.proc.getLowerPart().isSounding());
}

TEST_CASE ("arp.target routing in Whole, Dual and Split through the real parts", "[wave3][arp][target]")
{
    struct Case
    {
        int keyMode, target;
        bool lowerArp, upperArp; // which part gets arpeggiated notes
        bool upperPlain;         // Upper gets the played notes unchanged
        bool lowerPlain;
    };
    const Case cases[] {
        { 0, 2, true, false, false, false }, // Whole, Both: Upper not rendered, gets no notes
        { 0, 1, true, false, false, false }, // Whole, Upper: in Whole the arp/seq always plays Lower
        { 0, 0, true, false, false, false },
        { 1, 0, true, false, true, false },  // Dual, Lower: Upper plays the chord plainly
        { 1, 1, false, true, false, true },
        { 1, 2, true, true, false, false },
        { 2, 1, false, true, false, true },  // Split, Upper: the bass plays plainly
        { 2, 0, true, false, true, false },
    };
    for (const auto& c : cases)
    {
        INFO ("key mode " << c.keyMode << ", target " << c.target);
        Rig rig (48000.0, 512);
        arpBasics (rig);
        rig.set ("key.mode", static_cast<float> (c.keyMode));
        rig.set ("arp.target", static_cast<float> (c.target));
        rig.prepare();
        // Split at C4: 40/43 below (Lower), 72/76 above (Upper). Whole/Dual: all four.
        std::vector<Midi> midi { noteOn (500, 40), noteOn (500, 43), noteOn (500, 72), noteOn (500, 76) };
        for (const int n : { 40, 43, 72, 76 })
            midi.push_back (noteOff (30000, n));
        rig.render (60000, midi, 512, {}, [] (Rig& r) {
            if (r.proc.getKeyRouter().getMode() == vs::KeyMode::Whole)
                CHECK_FALSE (r.proc.getArpSeq (1).isSounding()); // nothing accumulates behind Upper
        });
        const auto expectPart = [&] (int p, bool arp, bool plain) {
            INFO ("part " << p);
            const auto on = rig.noteOnPositions (p);
            if (arp)
            {
                // Internal clock from the key at 500, 1/16 at 120 bpm: 500, 6500, ..., 24500 (keys up at 30000).
                CHECK (on.size() == 5);
                for (const auto s : on)
                    CHECK ((s - 500) % 6000 == 0);
            }
            else if (plain)
            {
                CHECK (on.size() >= 2);
                for (const auto s : on)
                    CHECK (s == 500);
            }
            else
                CHECK (on.empty());
        };
        expectPart (0, c.lowerArp, c.lowerPlain);
        expectPart (1, c.upperArp, c.upperPlain);
        for (int p = 0; p < 2; ++p)
        {
            CHECK (rig.allReleased (p));
            CHECK_FALSE (rig.proc.getArpSeq (p).isSounding());
        }
        CHECK_FALSE (rig.proc.getLowerPart().isSounding());
        CHECK_FALSE (rig.proc.getUpperPart().isSounding());
        if (c.keyMode == 0)
            CHECK_FALSE (rig.proc.isUpperRendering());
    }
}

TEST_CASE ("Key-mode change with the arp running leaves nothing hanging in the module or the parts", "[wave3][arp][keymode]")
{
    const double fs = GENERATE (48000.0, 96000.0, 192000.0);
    INFO ("rate " << fs);

    SECTION ("arp on, keys held through a Dual -> Split change")
    {
        Rig rig (fs, 512);
        arpBasics (rig);
        rig.set ("arp.target", 2);
        rig.set ("key.mode", 1);
        rig.set ("arp.gate", 100.0f); // a note always sounds
        rig.prepare();
        const int change = rig.samples (0.4);
        // The keys are never released: the router forgets them at the change, as the parts do.
        rig.render (rig.samples (1.5), { noteOn (0, 48), noteOn (0, 72), pedal (rig.samples (0.1), true) }, 256,
                    { { change, [] (Rig& r) { r.set ("key.mode", 2); } } });
        CHECK (rig.noteOnPositions (0, 0, change).size() >= 3);
        CHECK (rig.noteOnPositions (1, 0, change).size() >= 3);
        CHECK (rig.noteOnPositions (0, change).empty());
        CHECK (rig.noteOnPositions (1, change).empty());
        for (int p = 0; p < 2; ++p)
            CHECK_FALSE (rig.proc.getArpSeq (p).isSounding());
        CHECK_FALSE (rig.proc.getLowerPart().isSounding());
        CHECK_FALSE (rig.proc.getUpperPart().isSounding());
        // The pedal is still down (kept across the change) but consumed: the parts never see it.
        CHECK_FALSE (rig.proc.getLowerPart().isSustainDown());
        // Playing again works.
        const int t = static_cast<int> (rig.now);
        rig.render (rig.samples (0.5), { noteOn (t + 10, 50), noteOff (t + rig.samples (0.3), 50) }, 256);
        CHECK (rig.noteOnPositions (0, t).size() >= 2);
        // Pedal up and a little time: silence.
        rig.render (rig.samples (0.5), { pedal (static_cast<int> (rig.now) + 5, false) }, 256);
        CHECK_FALSE (rig.proc.getLowerPart().isSounding());
        CHECK_FALSE (rig.proc.getArpSeq (0).isSounding());
    }
    SECTION ("arp Off with the pedal down across a key-mode change, then switched on: no pile-up")
    {
        Rig rig (fs, 512);
        rig.set ("arp.target", 2);
        rig.set ("key.mode", 1);
        rig.prepare();
        const int change = rig.samples (0.3), on = rig.samples (0.5);
        int maxVoices = 0;
        rig.render (rig.samples (2.0), { pedal (0, true), noteOn (100, 60), noteOff (rig.samples (0.1), 60), noteOn (rig.samples (0.6), 64), noteOn (rig.samples (0.6), 67) },
                    256,
                    { { change, [] (Rig& r) { r.set ("key.mode", 0); } }, { on, [] (Rig& r) { r.set ("arp.mode", 1); } } },
                    [&] (Rig& r) {
                        if (r.now > rig.samples (0.8))
                            maxVoices = std::max (maxVoices, busyVoices (r.proc.getLowerPart()));
                    });
        // The part kept its pedal through the key-mode change; switching the arp on must lift it,
        // or every arpeggio note would be held by the pedal.
        CHECK_FALSE (rig.proc.getLowerPart().isSustainDown());
        CHECK (rig.noteOnPositions (0, rig.samples (0.6)).size() >= 8);
        CHECK (maxVoices <= 2);
    }
}

TEST_CASE ("Fast arpeggio at 16 voices: steals use the 3 ms fade, no clicks", "[wave3][arp][steal]")
{
    const double fs = GENERATE (48000.0, 192000.0);
    INFO ("rate " << fs);
    // Sine voices (both VCOs, open filter) at low pitches: the sum of 16 voices changes smoothly,
    // so its second difference |x[n] - 2 x[n-1] + x[n-2]| stays below 16 a (2 pi f / fs)^2 for
    // voices of amplitude a up to f = 349 Hz (0.033 a at 48 kHz). A voice cut without the fade
    // jumps by its instantaneous value (up to a); the 3 ms fade bends the slope by about a / 144.
    auto setup = [&] (Rig& rig) {
        rig.set ("lower.vco1.waveform", 0);
        rig.set ("lower.vco2.waveform", 0);
        rig.set ("lower.vcf.cutoff", 1.0f);
        rig.set (juce::String ("lower.vcf.") + "env_mod", 0.0f);
        rig.set ("lower.vcf.resonance", 0.0f);
        rig.set ("lower.vcf.key_follow", 0.0f);
        rig.set ("lower.env2.release", 1.0f); // 10 s: every voice keeps sounding, so all 16 fill up
        rig.set ("lower.env2.sustain", 1.0f);
        rig.set ("lower.vel.vca", 0.0f);
    };
    // The amplitude of one voice: the highest note alone.
    double a = 0.0;
    {
        Rig one (fs, 512);
        setup (one);
        one.prepare();
        const auto o = one.render (one.samples (0.3), { noteOn (0, 65) }, 512);
        a = peakAbs (o.l, static_cast<size_t> (one.samples (0.1)), o.l.size());
    }
    REQUIRE (a > 0.01);

    Rig rig (fs, 512);
    setup (rig);
    arpBasics (rig, 5);            // 1/32
    rig.set ("arp.tempo", 300.0f); // 25 ms steps
    rig.set ("arp.gate", 30.0f);
    rig.set ("arp.octaves", 4);
    rig.prepare();
    int full = 0, fadingBlocks = 0;
    // Six keys over four octaves: 24 different notes in the cycle, so new notes keep stealing.
    std::vector<Midi> midi;
    for (const int n : { 24, 26, 28, 29, 31, 33 })
        midi.push_back (noteOn (0, n));
    const auto out = rig.render (rig.samples (1.5), midi, 32, {}, [&] (Rig& r) {
        const auto& part = r.proc.getLowerPart();
        full = std::max (full, busyVoices (part));
        for (int v = 0; v < vs::kMaxVoices; ++v)
            if (part.getVoice (v).getFadeRemaining() > 0)
            {
                ++fadingBlocks;
                break;
            }
    });
    CHECK (full == 16);
    CHECK (fadingBlocks > 20); // steals happen, each with its fade
    CHECK (rig.noteOnPositions (0).size() == static_cast<size_t> (rig.samples (1.5) / rig.samples (0.025)));
    double maxD2 = 0.0;
    for (size_t i = static_cast<size_t> (0.45 * fs); i < out.l.size(); ++i)
        maxD2 = std::max (maxD2, std::abs (static_cast<double> (out.l[i]) - 2.0 * static_cast<double> (out.l[i - 1]) + static_cast<double> (out.l[i - 2])));
    INFO ("one voice " << a << ", largest second difference " << maxD2 << " (" << maxD2 / a << " a)");
    std::printf ("steal fade with a 1/32 arpeggio at 300 bpm, %.0f Hz: 16 voices busy %s, fading blocks %d, "
                 "largest second difference %.4f of one voice (bound 0.15)\n", fs, full == 16 ? "yes" : "no", fadingBlocks, maxD2 / a);
    CHECK (maxD2 < 0.15 * a);
}

// =================================================================================================
// Random streams, block-size independence, allocation
// =================================================================================================

TEST_CASE ("Block-size independence with arp and effects on, both parts, full Vintage", "[wave3][arp][fx][blocksize][slow]")
{
    const double fs = GENERATE (48000.0, 96000.0, 192000.0);
    const bool hostSynced = GENERATE (true, false);
    INFO ("rate " << fs << (hostSynced ? ", host transport" : ", internal clock"));
    auto render = [&] (int block, int prepared) {
        Rig rig (fs, prepared);
        rig.set ("global.vintage", 1.0f);
        rig.set ("key.mode", 1); // Dual: both parts
        rig.set ("lower.assign.mode", 2); // Solo with glide: the voice is reused
        rig.set ("lower.porta.time", 0.15f);
        rig.set ("lower.env2.release", 0.3f);
        rig.set ("upper.vco1.waveform", 3);
        rig.set ("upper.env2.release", 0.6f); // Poly voices reused after idle
        rig.set ("upper.mix.pan", 0.5f);
        rig.set ("arp.mode", 1);
        rig.set ("arp.target", 2);
        rig.set ("arp.octaves", 2);
        rig.set ("arp.rate", 3);
        rig.set ("arp.swing", 60.0f);
        rig.set ("arp.gate", 80.0f);
        rig.set ("arp.tempo", 140.0f);
        rig.set ("fx.chorus.mode", 1);
        rig.set ("fx.delay.level", 0.4f);
        rig.set ("fx.delay.time", vs::map::delayTimeFromSec (0.123f));
        rig.set ("fx.delay.feedback", 0.5f);
        rig.set ("fx.reverb.level", 0.3f);
        rig.set ("fx.reverb.decay", vs::map::reverbDecayFromSec (1.2f));
        rig.prepare();
        if (hostSynced)
            rig.play (128.0, 1.3);
        const auto s = [&] (double t) { return rig.samples (t); };
        std::vector<Midi> midi { noteOn (s (0.013), 48), noteOn (s (0.2), 55), noteOff (s (0.5), 48), noteOn (s (0.7), 60),
                                 noteOff (s (0.9), 55), noteOff (s (1.1), 60), noteOn (s (1.25), 52), noteOff (s (1.4), 52) };
        std::vector<Rig::Change> changes { { s (0.6), [hostSynced] (Rig& r) {
                                                if (hostSynced)
                                                    r.tempo (101.0);
                                                r.set ("arp.rate", 4);
                                            } } };
        return rig.render (s (1.8), midi, block, changes);
    };
    const auto ref = render (512, 512);
    CHECK (peakAbs (ref.l, 0, ref.l.size()) > 0.05);
    for (const auto& [block, prepared] : { std::pair { 1, 512 }, std::pair { 37, 512 }, std::pair { 160, 512 },
                                           std::pair { 1500, 512 }, std::pair { 333, 128 } })
    {
        if (block == 1 && fs > 100000.0)
            continue; // block size 1 at 48 kHz and 96 kHz (run time)
        INFO ("block " << block << ", prepared " << prepared);
        const auto got = render (block, prepared);
        const double d = diffDb (ref, got);
        INFO ("difference " << d << " dB");
        std::printf ("block-size independence (arp + fx, Dual, Vintage 1): %6.0f Hz %-8s block %4d / prepared %3d vs 512: %8.1f dB\n",
                     fs, hostSynced ? "host" : "internal", block, prepared, d);
        CHECK (d < -100.0);
    }
}

#if defined(__APPLE__) && VS_SEQ_COUNT_ALLOCATIONS
TEST_CASE ("No allocation in processBlock with the arp, transport and effects running", "[wave3][arp][fx][realtime]")
{
    Rig rig (48000.0, 512);
    rig.set ("arp.mode", 1);
    rig.set ("key.mode", 1);
    rig.set ("fx.chorus.mode", 2);
    rig.set ("fx.delay.level", 0.5f);
    rig.set ("fx.reverb.level", 0.5f);
    rig.prepare();
    rig.play (120.0, 0.0);
    rig.updatePlayHead();
    juce::AudioBuffer<float> buffer (2, 512);
    std::vector<juce::MidiBuffer> blocks (200);
    for (int b = 0; b < 200; ++b)
    {
        if (b % 7 == 0)
            blocks[static_cast<size_t> (b)].addEvent (juce::MidiMessage::noteOn (1, 48 + b % 24, static_cast<juce::uint8> (100)), b % 512);
        if (b % 11 == 0)
            blocks[static_cast<size_t> (b)].addEvent (juce::MidiMessage::noteOff (1, 48 + (b + 5) % 24), 3);
        if (b % 13 == 0)
            blocks[static_cast<size_t> (b)].addEvent (juce::MidiMessage::controllerEvent (1, 64, (b / 13) % 2 == 0 ? 127 : 0), 100);
    }
    auto* keyMode = rig.proc.getState().getRawParameterValue ("key.mode");
    auto* arpMode = rig.proc.getState().getRawParameterValue ("arp.mode");
    vs::seq::test::AllocationCounter counter;
    {
        auto* probe = new int (3); // the counter must see an allocation on this thread
        vs::seq::test::gEscape = probe;
        delete probe;
    }
    const int base = counter.count();
    REQUIRE (base > 0);
    for (int b = 0; b < 200; ++b)
    {
        // Parameter values change through the atomics (as a host's automation does on this thread).
        if (b == 60)
            keyMode->store (2.0f);
        if (b == 90)
            arpMode->store (2.0f);
        if (b == 120)
            arpMode->store (0.0f);
        if (b == 150)
            rig.playHead.ppq = 0.0; // loop jump
        else if (rig.playHead.ppq.has_value())
            *rig.playHead.ppq += 512.0 * 120.0 / (60.0 * 48000.0);
        buffer.clear();
        rig.proc.processBlock (buffer, blocks[static_cast<size_t> (b)]);
    }
    CHECK (counter.count() == base);
}
#endif
