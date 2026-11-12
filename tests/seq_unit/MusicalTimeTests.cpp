// Timing properties derived from musical time alone, with no model of the module's state machine.
//
// The contract states the only rounding rule once, here: an event that belongs at musical time T
// happens at the first sample at or after T. From it:
//   P1  the note-ons are exactly the steps' musical times rounded up (none missing, none extra),
//   P2  no two note-ons are closer than the shortest (swung) step minus one sample,
//   P3  each note-off is at the gate end's musical time rounded up (or, at 100 % gate, with the next note-on),
//   P4  with 100 % gate a note-off is never earlier than the next note-on (off before on only for the same pitch).
// Positions are exact integers (SeqTestUtil.h ticks); nothing here knows about "one step per sample"
// or "off at least a sample after on".
#include <catch2/catch_test_macros.hpp>

#include "SeqTestUtil.h"

#include <string>

using namespace vs::seq;
using namespace vs::seq::test;
using vs::EventType;

namespace
{
constexpr int kD[] = { 1, 2, 3, 4, 6, 8 };
constexpr Rate kRates[] = { Rate::Quarter, Rate::Eighth, Rate::EighthTriplet, Rate::Sixteenth, Rate::SixteenthTriplet, Rate::ThirtySecond };
constexpr int kSampleRates[] = { 44100, 48000, 96000, 192000 };
constexpr std::int64_t kTempi[] = { 6000, 9750, 12000, 13337, 17321, 29999 };
constexpr int kBlocks[] = { 1, 37, 480, 4096 };

struct Rng
{
    std::uint64_t s;
    explicit Rng (std::uint64_t seed) : s (seed) {}
    int below (int n)
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<int> ((s >> 33) % static_cast<std::uint64_t> (n));
    }
};
} // namespace

TEST_CASE ("Musical time: note-ons and note-offs are the musical times rounded up to the sample", "[seq][timing][musical]")
{
    Rng rng (8675309);
    std::int64_t stepsChecked = 0;
    for (int i = 0; i < 160; ++i)
    {
        MCfg c;
        c.sr = kSampleRates[rng.below (4)];
        const int di = rng.below (6);
        c.d = kD[di];
        c.swing80 = 40 + rng.below (21);
        c.gate80 = (i % 5 == 0) ? 80 : 20 + rng.below (59);
        const std::int64_t bpm100 = kTempi[rng.below (6)];
        const int block = kBlocks[i % 4];
        const std::int64_t anchor = kTick * 40 + rng.below (static_cast<int> (kTick * 3));
        const std::int64_t keyDown = rng.below (2000);
        const std::int64_t stepSamples = 60 * static_cast<std::int64_t> (c.sr) * 100 / (bpm100 * c.d);
        const std::int64_t total = std::max<std::int64_t> (block == 1 ? c.sr / 5 : c.sr, keyDown + 6 * stepSamples);

        Settings s;
        s.mode = Mode::Arp;
        s.rate = kRates[di];
        s.swing = static_cast<double> (c.swing80) / 80.0;
        s.gate = static_cast<double> (c.gate80) / 80.0;
        ArpSeq m;
        Run r;
        r.sr = c.sr;
        r.total = total;
        r.block = block;
        r.segs = { HostSeg { 0, true, bpm100, anchor } };
        r.changes = { Change { 0, s } };
        r.input = { noteOn (static_cast<int> (keyDown), 60), noteOn (static_cast<int> (keyDown), 64) };
        const RunResult res = run (m, r);
        REQUIRE (res.wellFormed);

        const MSeg seg { 0, total, bpm100, anchor, false };
        std::vector<std::int64_t> ons, offs;
        std::vector<int> onNotes;
        for (const auto& e : res.events)
        {
            if (e.type == EventType::NoteOn)
            {
                ons.push_back (e.offset);
                onNotes.push_back (e.note);
            }
            else if (e.type == EventType::NoteOff)
                offs.push_back (e.offset);
        }

        // Musical times of the steps from the key on (the first step at or after the key's sample).
        std::vector<std::int64_t> stepSample, gateSample;
        std::int64_t k = anchor / (kTick / c.d) - 3;
        for (;; ++k)
        {
            const std::int64_t s0 = relSample (c, seg, c.pos (k));
            if (s0 < keyDown)
                continue;
            if (s0 >= total)
                break;
            stepSample.push_back (s0);
            gateSample.push_back (relSample (c, seg, c.gateEnd (k)));
        }
        INFO ("sr " << c.sr << " bpm100 " << bpm100 << " d " << c.d << " swing80 " << c.swing80 << " gate80 " << c.gate80 << " block " << block);
        // P1
        CHECK (ons == stepSample);
        // The arp plays 60, 64, 60, 64, ... (Up over two keys): one per step.
        for (std::size_t j = 0; j < onNotes.size(); ++j)
            CHECK (onNotes[j] == (j % 2 == 0 ? 60 : 64));
        // P2: the shortest (swung) step is step * (1 - (2 * swing - 1)).
        const double shortest = static_cast<double> (stepSamples) * (1.0 - (2.0 * static_cast<double> (c.swing80) / 80.0 - 1.0));
        for (std::size_t j = 1; j < ons.size(); ++j)
            CHECK (static_cast<double> (ons[j] - ons[j - 1]) >= shortest - 1.0 - 1.0e-9);
        // P3 / P4
        for (std::size_t j = 0; j < ons.size() && j < offs.size(); ++j)
        {
            if (gateSample[j] <= ons[j])
                continue; // a gate shorter than a sample is not what this property is about
            if (c.gate80 == 80)
            {
                // 100 %: the off belongs to the next step's sample (after its on); the last note's off is beyond the run.
                if (j + 1 < ons.size())
                    CHECK (offs[j] == ons[j + 1]);
                CHECK (offs[j] >= ons[j]);
            }
            else
                CHECK (offs[j] == gateSample[j]);
        }
        stepsChecked += static_cast<std::int64_t> (ons.size());
    }
    CHECK (stepsChecked > 1500);
}
