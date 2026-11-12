// Seeded adversarial fuzz (ported from the independent reviewer's probe): random settings, hostile
// transports (NaN, infinite, zero and absurd tempo / position / sample rate), malformed events
// (out-of-range offsets and notes, NaN velocities), blocks from 0 to 300000 samples, and random
// output capacities, including far too small ones. Time-bounded: a fixed number of cases, each of
// bounded length. Checked with the origin-aware StreamChecker (what the module generated versus what
// it passed through while Off), plus a model of a part with a sustain pedal.
#include <catch2/catch_test_macros.hpp>

#include "SeqTestUtil.h"

#include <climits>
#include <cmath>
#include <limits>
#include <map>
#include <string>

using namespace vs::seq;
using namespace vs::seq::test;
using vs::EventType;

namespace
{
struct R
{
    std::uint64_t s;
    std::uint64_t next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    int below (int n) { return static_cast<int> (next() % static_cast<std::uint64_t> (n)); }
    bool chance (int p) { return below (100) < p; }
    double unit() { return static_cast<double> (next() >> 11) / 9007199254740992.0; }
};

const double kNaN = std::numeric_limits<double>::quiet_NaN();

Settings randSettings (R& g)
{
    Settings s;
    s.mode = static_cast<Mode> (g.below (3));
    s.rate = static_cast<Rate> (g.below (6));
    s.swing = 0.5 + 0.25 * g.unit();
    s.gate = g.chance (30) ? 1.0 : 0.05 + 0.95 * g.unit();
    s.hold = g.chance (40);
    s.internalBpm = g.chance (5) ? kNaN : 20.0 + 280.0 * g.unit();
    s.arpPattern = static_cast<ArpPattern> (g.below (5));
    s.arpOctaves = 1 + g.below (4);
    s.seed = static_cast<std::uint32_t> (g.next());
    s.seqLength = 1 + g.below (16);
    const int style = g.below (4);
    for (auto& st : s.seqSteps)
    {
        int r = g.below (100);
        if (style == 1)
            r = 60; // all Rest
        if (style == 2)
            r = 90; // all Tie
        st.type = r < 50 ? StepType::Note : r < 75 ? StepType::Rest : StepType::Tie;
        st.semitones = g.below (61) - 30;
        st.velocity = g.chance (5) ? std::nanf ("") : static_cast<float> (g.unit());
    }
    if (g.chance (5))
    {
        s.swing = 99.0;
        s.gate = -1.0;
        s.arpOctaves = 99;
        s.seqLength = -4;
    }
    return s;
}

struct Totals
{
    std::int64_t cases = 0, blocks = 0, ons = 0, drops = 0, smallCapacityBlocks = 0;
    std::map<std::string, int> failures;
};

void runCase (std::uint64_t seed, bool adversarialCapacity, Totals& tot)
{
    R g { seed * 0x9E3779B97F4A7C15ull + 1 };
    ArpSeq m;
    Settings cur = randSettings (g);
    m.setSettings (cur);
    double ppq = g.chance (20) ? -4.0 : g.unit() * 1000.0;
    double bpm = 40.0 + 200.0 * g.unit();
    const double sr = std::array<double, 4> { 44100, 48000, 96000, 192000 }[static_cast<std::size_t> (g.below (4))];
    bool playing = g.chance (50);
    std::array<bool, 256> held {};
    StreamChecker chk;
    const int blocks = 120 + g.below (200);
    const auto fail = [&] (const std::string& what) { ++tot.failures[what + " (seed " + std::to_string (seed) + ")"]; };

    for (int b = 0; b <= blocks + 4; ++b)
    {
        const bool settle = b >= blocks;
        if (! settle && g.chance (15))
        {
            cur = randSettings (g);
            m.setSettings (cur);
        }
        else if (! settle && g.chance (10))
        {
            cur.swing = 0.5 + 0.25 * g.unit();
            cur.gate = g.unit();
            cur.seqLength = 1 + g.below (16);
            m.setSettings (cur);
        }
        const int r = g.below (100);
        int n = r < 5 ? 0 : r < 10 ? 1 : r < 80 ? 1 + g.below (1024) : r < 97 ? 1 + g.below (8192) : 1 + g.below (300000);
        Transport tr { sr, bpm, ppq, playing };
        if (! settle)
        {
            if (g.chance (4))
            {
                playing = ! playing;
                tr.playing = playing;
            }
            if (g.chance (5))
            {
                ppq += (g.chance (50) ? -1.0 : 1.0) * g.unit() * 8.0;
                tr.ppqPosition = ppq;
            }
            if (g.chance (3))
            {
                ppq += (g.chance (50) ? 1.0 : -1.0) * g.unit() * 0.1; // around the drift limit
                tr.ppqPosition = ppq;
            }
            if (g.chance (8))
            {
                bpm = 20.0 + 400.0 * g.unit();
                tr.bpm = bpm;
            }
            if (g.chance (2))
                tr.bpm = std::array<double, 6> { 0, -1, kNaN, 1e9, 1e-310, HUGE_VAL }[static_cast<std::size_t> (g.below (6))];
            if (g.chance (2))
                tr.ppqPosition = std::array<double, 4> { kNaN, -1e12, 1e12, 5e8 }[static_cast<std::size_t> (g.below (4))];
            if (g.chance (2))
                tr.sampleRate = std::array<double, 3> { 0, kNaN, -48000 }[static_cast<std::size_t> (g.below (3))];
        }
        std::vector<NoteEvent> in;
        const int k = settle ? 0 : g.below (r < 50 ? 4 : 40);
        int off = 0;
        for (int i = 0; i < k; ++i)
        {
            off += g.chance (40) ? 0 : g.below (std::max (n, 1));
            int o = off;
            if (g.chance (2))
                o = g.chance (50) ? -5 : n + 7; // out of range
            NoteEvent e { o, EventType::NoteOn, 0, 0.8f };
            const int t = g.below (100);
            if (t < 40)
            {
                const int note = g.chance (3) ? 128 + g.below (128) : 36 + g.below (48);
                e.note = static_cast<std::uint8_t> (note);
                e.value = g.chance (3) ? std::nanf ("") : static_cast<float> (g.unit());
                held[static_cast<std::size_t> (note)] = true;
            }
            else if (t < 75)
            {
                int note = 36 + g.below (48);
                if (g.chance (3))
                    note = 128 + g.below (128);
                e.type = EventType::NoteOff;
                e.note = static_cast<std::uint8_t> (note);
                held[static_cast<std::size_t> (note)] = false;
            }
            else if (t < 85)
            {
                e.type = EventType::SustainPedal;
                e.value = g.chance (50) ? 1.0f : 0.0f;
            }
            else if (t < 88)
            {
                e.type = EventType::AllNotesOff;
                held.fill (false);
            }
            else if (t < 89)
            {
                e.type = EventType::AllSoundOff;
                held.fill (false);
            }
            else if (t < 91)
                e.type = EventType::ResetControllers;
            else
            {
                e.type = EventType::PitchBend;
                e.value = static_cast<float> (g.unit());
            }
            in.push_back (e);
        }
        bool userPanic = false;
        for (const auto& e : in)
            userPanic = userPanic || e.type == EventType::AllNotesOff || e.type == EventType::AllSoundOff;
        if (b == blocks) // final: all keys up, pedal up, panic
        {
            for (int nn = 0; nn < 256; ++nn)
                if (held[static_cast<std::size_t> (nn)])
                    in.push_back (NoteEvent { 0, EventType::NoteOff, static_cast<std::uint8_t> (nn), 0.0f });
            in.push_back (NoteEvent { 0, EventType::SustainPedal, 0, 0.0f });
            in.push_back (NoteEvent { 0, EventType::AllNotesOff, 0, 0.0f });
            userPanic = true;
            n = std::max (n, 1);
        }
        const bool small = adversarialCapacity && ! settle && g.chance (30);
        const int capacity = small ? g.below (12) : ArpSeq::kRecommendedCapacity;
        std::vector<NoteEvent> out (static_cast<std::size_t> (capacity) + 8);
        for (auto& e : out)
            e = NoteEvent { -999, EventType::PitchBend, 0, 0.0f };
        const Mode modeNow = m.settings().mode;
        const int d0 = m.droppedCount();
        const int c = m.process (in, tr, n, std::span<NoteEvent> (out.data(), static_cast<std::size_t> (capacity)));
        ++tot.blocks;
        tot.smallCapacityBlocks += small ? 1 : 0;
        if (c < 0 || c > capacity)
        {
            fail ("count out of range");
            continue;
        }
        if (out[static_cast<std::size_t> (capacity)].offset != -999)
            fail ("wrote past capacity");
        const bool dropped = m.droppedCount() != d0;
        if (dropped)
            tot.drops += m.droppedCount() - d0;
        if (dropped && ! small && k < 100)
            fail ("drop with the recommended capacity");
        int prev = 0;
        for (int i = 0; i < c; ++i)
        {
            const NoteEvent& e = out[static_cast<std::size_t> (i)];
            if (e.offset < 0 || e.offset < prev || (n > 0 && e.offset >= n) || (n == 0 && e.offset != 0))
                fail ("offset outside the block or not ascending");
            prev = e.offset;
            if (e.type == EventType::NoteOn)
                ++tot.ons;
            // The overflow marker (a panic nobody sent) legitimately clears everything downstream.
            const bool markerPossible = dropped && ! userPanic;
            chk.feed (e, modeNow, e.offset == 0 && true, markerPossible || small);
            if (! chk.firstError.empty())
            {
                fail (chk.firstError.substr (0, 40));
                chk.firstError.clear();
            }
        }
        ppq += static_cast<double> (n) * bpm / (60.0 * sr);
    }
    // Everything is released at the end (the settle blocks used the big capacity).
    if (! chk.gen.empty())
        fail ("generated note hanging at the end");
    if (! chk.user.empty())
        fail ("passed-through note hanging at the end");
    if (! chk.part.sounding.empty())
        fail ("part still sounding at the end");
    if (chk.part.pedalDown)
        fail ("part pedal still down at the end");
    if (m.isSounding())
        fail ("module still sounding at the end");
    ++tot.cases;
}
} // namespace

TEST_CASE ("Fuzz: hostile transports, malformed events and random output capacities never hang or double a note", "[seq][fuzz]")
{
    Totals tot;
    for (std::uint64_t seed = 0; seed < 600; ++seed)
        runCase (seed, seed % 3 == 0, tot);
    std::string all;
    int shown = 0;
    for (const auto& kv : tot.failures)
        if (shown++ < 12)
            all += "\n  " + kv.first + ": " + std::to_string (kv.second);
    INFO ("failures:" << all);
    CHECK (tot.failures.empty());
    CHECK (tot.cases == 600);
    CHECK (tot.ons > 3000);                 // plenty of generated notes
    CHECK (tot.smallCapacityBlocks > 500);  // and plenty of blocks with a buffer far too small
    CHECK (tot.drops > 100);                // which really did overflow
}
