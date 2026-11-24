// Second independent review of the S4a fix round (docs/architecture.md 8.21.1, "After the independent review
// (2026-11-24 ...)"), written from the contract and the code under review, not from the author's tests. Tag [review2];
// the long randomised scans are hidden behind [.review2-full].
//
//  1. The chain's block path (ChipVoice::fastBlock): randomised differential tests against the per-sample path
//     (ChipVoice::blockPathEnabled (false)), at the chain and through the part.
//  2. Shared shares by weight: host-block independence with ramps in flight, the one-voice identity after joins and
//     leaves, the idle-leave times against Per Note, the stated steal dip.
//  3. Late notes, the switch fade after the pad: no note lost or hung, lengths kept, block independence.
#include <catch2/catch_test_macros.hpp>

#include "../integration/PartTestSupport.h"
#include "bside/chip/ChipVoice.h"
#include "bside/chip/Filter.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace bs_part_test;
namespace chip = vs::bside::chip;

namespace
{
// ---- 1. the chain: block path against per-sample path -----------------------------------------------------------

bool sameBitsD (double a, double b) { return std::bit_cast<std::uint64_t> (a) == std::bit_cast<std::uint64_t> (b); }

struct ChainPair
{
    std::unique_ptr<chip::ChipVoice> fast = std::make_unique<chip::ChipVoice>();
    std::unique_ptr<chip::ChipVoice> slow = std::make_unique<chip::ChipVoice>();
    std::unique_ptr<chip::Filter> ff = std::make_unique<chip::Filter>();
    std::unique_ptr<chip::Filter> fs = std::make_unique<chip::Filter>();
};

struct ChainScanResult
{
    std::uint64_t samples = 0, fastSamples = 0, mismatches = 0, ops = 0;
    double worst = 0.0;
    std::string first;
};

/** One randomised run: the same operations on two chains, one with the block path, one without; every output sample
    and the chain's observable state compared bit for bit after every call. */
void chainScan (chip::Model m, double rate, std::uint32_t seed, int steps, ChainScanResult& r)
{
    std::mt19937 rng (seed);
    auto uni = [&] (int lo, int hi) { return lo + static_cast<int> (rng() % static_cast<std::uint32_t> (hi - lo + 1)); };
    ChainPair p;
    chip::Config cfg;
    cfg.model = m;
    cfg.internalRate = rate;
    cfg.volumeDc = (seed & 1u) != 0;
    cfg.authentic = (seed & 2u) != 0;
    cfg.variation = 0.5f;
    p.fast->prepare (cfg);
    p.slow->prepare (cfg);
    p.ff->prepare (m, rate, 0.5f);
    p.fs->prepare (m, rate, 0.5f);
    p.fast->setFilter (p.ff.get());
    p.slow->setFilter (p.fs.get());
    p.slow->blockPathEnabled (false);
    auto both = [&] (auto&& f) {
        f (*p.fast);
        f (*p.slow);
    };
    // a sounding start: three gated oscillators, routing, cutoff, mode and volume
    for (int n = 0; n < 3; ++n)
    {
        const auto base = static_cast<std::uint8_t> (7 * n);
        const int fr = uni (800, 9000);
        both ([&] (chip::ChipVoice& v) {
            v.write (base, static_cast<std::uint8_t> (fr & 0xff));
            v.write (static_cast<std::uint8_t> (base + 1), static_cast<std::uint8_t> (fr >> 8));
            v.write (static_cast<std::uint8_t> (base + 2), 0x00);
            v.write (static_cast<std::uint8_t> (base + 3), 0x08);
            v.write (static_cast<std::uint8_t> (base + 5), 0x09);
            v.write (static_cast<std::uint8_t> (base + 6), 0xa9);
            v.write (static_cast<std::uint8_t> (base + 4), static_cast<std::uint8_t> (n == 0 ? 0x21 : (n == 1 ? 0x41 : 0x11)));
        });
    }
    both ([&] (chip::ChipVoice& v) {
        v.write (0x15, 3);
        v.write (0x16, 0x50);
        v.write (0x17, 0x53);
        v.write (0x18, 0x1f);
    });

    std::vector<double> a (2048), b (2048);
    std::vector<float> af (2048), bf (2048);
    for (int s = 0; s < steps; ++s)
    {
        const int kind = uni (0, 99);
        ++r.ops;
        if (kind < 40)
        {
            // a render call: any length, sometimes stopped at a cycle (the part's ticks)
            int len = uni (0, 9) < 3 ? uni (1, 4) : (uni (0, 3) == 0 ? uni (300, 2000) : uni (1, 200));
            std::uint64_t until = ~std::uint64_t { 0 };
            if (uni (0, 3) == 0)
                until = p.fast->chip().cycle() + static_cast<std::uint64_t> (uni (0, 3000));
            const bool asFloat = uni (0, 15) == 0;
            int na = 0, nb = 0;
            if (asFloat)
            {
                std::fill (af.begin(), af.begin() + len, 0.0f);
                std::fill (bf.begin(), bf.begin() + len, 0.0f);
                na = p.fast->renderUntil (af.data(), len, until);
                nb = p.slow->renderUntil (bf.data(), len, until);
                for (int i = 0; i < na; ++i)
                {
                    a[static_cast<std::size_t> (i)] = static_cast<double> (af[static_cast<std::size_t> (i)]);
                    b[static_cast<std::size_t> (i)] = static_cast<double> (bf[static_cast<std::size_t> (i)]);
                }
            }
            else
            {
                std::fill (a.begin(), a.begin() + len, 0.0);
                std::fill (b.begin(), b.begin() + len, 0.0);
                na = p.fast->renderUntil (a.data(), len, until);
                nb = p.slow->renderUntil (b.data(), len, until);
            }
            if (na != nb && r.first.empty())
                r.first = "render counts differ at step " + std::to_string (s);
            for (int i = 0; i < std::min (na, nb); ++i)
            {
                const auto j = static_cast<std::size_t> (i);
                ++r.samples;
                if (! sameBitsD (a[j], b[j]))
                {
                    ++r.mismatches;
                    r.worst = std::max (r.worst, std::abs (a[j] - b[j]));
                    if (r.first.empty())
                        r.first = "seed " + std::to_string (seed) + " step " + std::to_string (s) + " sample " + std::to_string (i) + ": " + std::to_string (a[j]) + " vs " + std::to_string (b[j]);
                }
                if (! std::isfinite (a[j]) && r.first.empty())
                    r.first = "non-finite output";
            }
            if ((! sameBitsD (p.fast->lastFilterInput(), p.slow->lastFilterInput()) || ! sameBitsD (p.fast->lastFilterOutput(), p.slow->lastFilterOutput())
                 || p.fast->cutoffMoving() != p.slow->cutoffMoving() || p.fast->configsActive() != p.slow->configsActive()
                 || ! (p.fast->chip().state() == p.slow->chip().state()))
                && r.first.empty())
                r.first = "observable state differs after step " + std::to_string (s);
        }
        else if (kind < 70)
        {
            // a register write of any kind (oscillator frequency, pulse width, control with test, sync and ring bits,
            // envelopes; cutoff low and high, resonance and routing, mode and volume)
            const int reg = uni (0, 9) < 4 ? uni (0x15, 0x18) : uni (0x00, 0x14);
            auto val = static_cast<std::uint8_t> (uni (0, 255));
            if (reg % 7 == 4 && reg < 0x15 && uni (0, 2) != 0)
                val = static_cast<std::uint8_t> ((val & 0xf0) | (uni (0, 1) != 0 ? 0x01 : 0x00) | (uni (0, 5) == 0 ? 0x08 : 0x00) | (uni (0, 3) == 0 ? 0x06 : 0x00));
            if (reg == 0x18 && uni (0, 1) != 0)
                val = static_cast<std::uint8_t> (val | 0x0f);
            both ([&] (chip::ChipVoice& v) { v.write (static_cast<std::uint8_t> (reg), val); });
        }
        else if (kind < 78)
        {
            // a cutoff burst (the cutoff moves), then nothing: it stands still again later
            const int count = uni (2, 12);
            for (int k = 0; k < count; ++k)
            {
                const int fc = uni (0, 2047);
                const int gap = uni (1, 80);
                both ([&] (chip::ChipVoice& v) {
                    v.write (0x15, static_cast<std::uint8_t> (fc & 7));
                    v.write (0x16, static_cast<std::uint8_t> (fc >> 3));
                });
                std::fill (a.begin(), a.begin() + gap, 0.0);
                std::fill (b.begin(), b.begin() + gap, 0.0);
                const int na = p.fast->renderUntil (a.data(), gap, ~std::uint64_t { 0 });
                const int nb = p.slow->renderUntil (b.data(), gap, ~std::uint64_t { 0 });
                for (int i = 0; i < std::min (na, nb); ++i)
                {
                    ++r.samples;
                    if (! sameBitsD (a[static_cast<std::size_t> (i)], b[static_cast<std::size_t> (i)]))
                    {
                        ++r.mismatches;
                        if (r.first.empty())
                            r.first = "cutoff burst at step " + std::to_string (s);
                    }
                }
            }
        }
        else if (kind < 83)
        {
            chip::OscInput in;
            in.frequency = static_cast<double> (uni (0, 60000)) + (cfg.authentic ? 0.0 : 0.37);
            in.pulseWidth = static_cast<float> (uni (0, 4095)) + (cfg.authentic ? 0.0f : 0.5f);
            in.control = static_cast<std::uint8_t> ((uni (1, 15) << 4) | uni (0, 15));
            in.attackDecay = static_cast<std::uint8_t> (uni (0, 255));
            in.sustainRelease = static_cast<std::uint8_t> (uni (0, 255));
            const int n = uni (0, 2);
            both ([&] (chip::ChipVoice& v) { v.setOscillator (n, in); });
        }
        else if (kind < 87)
        {
            chip::FilterInput fi;
            fi.cutoff = static_cast<float> (uni (0, 2047)) + (cfg.authentic ? 0.0f : 0.25f);
            fi.resonance = static_cast<std::uint8_t> (uni (0, 15));
            fi.routing = static_cast<std::uint8_t> (uni (0, 15));
            fi.modeVolume = static_cast<std::uint8_t> (uni (0, 255));
            both ([&] (chip::ChipVoice& v) { v.setFilterInput (fi); });
        }
        else if (kind < 90 && m == chip::Model::A)
        {
            // the owner moves chip.variation on the lent filter between calls (Per Note, Model A)
            const float var = static_cast<float> (uni (0, 100)) / 100.0f;
            p.ff->setVariation (var);
            p.fs->setVariation (var);
        }
        else if (kind < 93)
        {
            // suspend (the voice leaves), its chip time skipped, then it renders again (settled)
            both ([&] (chip::ChipVoice& v) { v.suspend(); });
            const int skip = uni (0, 5000);
            if (skip > 0)
            {
                p.fast->skipSamples (skip);
                p.slow->skipSamples (skip);
            }
        }
        else if (kind < 95)
            both ([&] (chip::ChipVoice& v) { v.powerOnKeepingTime(); });
        else if (kind < 97)
            both ([&] (chip::ChipVoice& v) { v.reset(); });
        else
        {
            const std::uint32_t clk = uni (0, 1) != 0 ? 985248u : 1022727u;
            const bool au = uni (0, 1) != 0, vdc = uni (0, 1) != 0;
            cfg.authentic = au;
            both ([&] (chip::ChipVoice& v) { v.reconfigure (clk, au, vdc); });
        }
    }
    r.fastSamples += p.fast->blockPathSamples();
    if (p.slow->blockPathSamples() != 0 && r.first.empty())
        r.first = "the per-sample chain took the block path";
}

void chainScanAll (int seeds, int steps, std::uint32_t seed0)
{
    ChainScanResult total;
    int runs = 0;
    for (const chip::Model m : { chip::Model::B, chip::Model::A })
        for (const double rate : { 48000.0, 88200.0, 96000.0, 128000.0, 176400.0, 192000.0 })
            for (int s = 0; s < seeds; ++s)
            {
                ChainScanResult r;
                const auto seed = seed0 + static_cast<std::uint32_t> (s * 7919 + static_cast<int> (rate) / 100 + (m == chip::Model::A ? 1000003 : 0));
                chainScan (m, rate, seed, steps, r);
                ++runs;
                INFO ((m == chip::Model::A ? "Model A" : "Model B") << " at " << rate << " Hz, seed " << seed << ": " << r.mismatches << " of " << r.samples
                                                                     << " samples differ (largest " << r.worst << "); " << r.first);
                CHECK (r.mismatches == 0);
                CHECK (r.first.empty());
                total.samples += r.samples;
                total.fastSamples += r.fastSamples;
                total.ops += r.ops;
                total.mismatches += r.mismatches;
            }
    std::printf ("[review2 chain] %d runs, %llu operations, %llu samples compared, %llu took the block path, %llu differ\n", runs,
                 static_cast<unsigned long long> (total.ops), static_cast<unsigned long long> (total.samples),
                 static_cast<unsigned long long> (total.fastSamples), static_cast<unsigned long long> (total.mismatches));
    CHECK (total.fastSamples > total.samples / 4); // the block path is really exercised
}

// ---- the part driven with scheduled host-side actions --------------------------------------------------------------

struct Action
{
    std::int64_t at = 0; // host sample, before the render that starts there
    enum Kind { params, reset, resume, releaseAll } kind = params;
    bs::PartParams p {};
};

struct Scenario
{
    bs::PartParams start {};
    std::vector<vs::NoteEvent> events; // absolute host offsets, ascending
    std::vector<Action> actions;       // ascending
    int total = 0;
    const vs::bside::program::Program* program = nullptr;
};

struct Outcome
{
    std::vector<float> y;
    std::vector<chip::DigitalState> states;
    std::vector<std::uint64_t> ticks;
    std::vector<bool> active, pending;
    bool finite = true;
};

/** Renders the scenario with random host block lengths from `blockSeed` (0: blocks of 512), every action at its host
    sample (blocks are broken there). blockPathOff: every voice's chain runs its per-sample path. */
Outcome runScenario (HostConfig h, const Scenario& sc, std::uint32_t blockSeed, bool blockPathOff = false, int maxBlock = 512)
{
    Rig rig (h, sc.start, maxBlock);
    rig.part->setProgram (sc.program);
    std::mt19937 rng (blockSeed);
    Outcome o;
    o.y.assign (static_cast<std::size_t> (sc.total), 0.0f);
    std::size_t nextE = 0, nextA = 0;
    std::vector<vs::NoteEvent> chunk;
    for (std::int64_t start = 0; start < sc.total;)
    {
        while (nextA < sc.actions.size() && sc.actions[nextA].at <= start)
        {
            const auto& a = sc.actions[nextA++];
            switch (a.kind)
            {
                case Action::params: rig.set (a.p); break;
                case Action::reset: rig.part->reset(); break;
                case Action::resume: rig.part->resume(); break;
                case Action::releaseAll: rig.part->releaseAllNotes(); break;
            }
        }
        int len = blockSeed == 0 ? maxBlock : (rng() % 4 == 0 ? 1 + static_cast<int> (rng() % 5) : 1 + static_cast<int> (rng() % static_cast<std::uint32_t> (maxBlock)));
        len = static_cast<int> (std::min<std::int64_t> (len, sc.total - start));
        if (nextA < sc.actions.size())
            len = static_cast<int> (std::min<std::int64_t> (len, sc.actions[nextA].at - start));
        len = std::max (1, len);
        chunk.clear();
        while (nextE < sc.events.size() && sc.events[nextE].offset < start + len)
        {
            auto e = sc.events[nextE++];
            e.offset -= static_cast<int> (start);
            chunk.push_back (e);
        }
        if (blockPathOff)
            for (int i = 0; i < bs::Part::kVoices; ++i)
                const_cast<bs::NoteVoice&> (rig.part->getVoice (i)).chain.blockPathEnabled (false);
        rig.part->render (o.y.data() + start, len, chunk);
        start += len;
    }
    for (int i = 0; i < bs::Part::kVoices; ++i)
    {
        const auto& v = rig.part->getVoice (i);
        o.states.push_back (v.chain.chip().state());
        o.ticks.push_back (v.engine.tickIndex());
        o.active.push_back (v.active);
        o.pending.push_back (v.pending);
        if (blockPathOff && v.chain.blockPathSamples() != 0)
            o.finite = false; // flagged below: the hook did not hold
    }
    for (const float v : o.y)
        o.finite = o.finite && std::isfinite (v) && std::abs (v) < 8.0f;
    return o;
}

const char* kEndingTable = "bside-table 1\n[wave1]\n    wave pulse+gate +0 ticks 0\nloop:\n    pitch +0\n    pitch +7 ticks 2\n    wave saw+gate +12\n    jump loop\n"
                           "release:\n    gate off ticks 0\n    end\n[pulse1]\n    set 900\n    pingpong 400 3600 37\n[filter]\n    cutoff 1700\n    sweep -25 x30\n";
const char* kLeadTable = "bside-table 1\n[wave1]\n    wave pulse+gate +0 ticks 0\nloop:\n    pitch +0\n    pitch +7 ticks 2\n    wave saw+gate +12\n    jump loop\n"
                         "release:\n    gate off ticks 0\n    jump loop\n[pulse1]\n    set 900\n    pingpong 400 3600 37\n[filter]\n    cutoff 1700\n    sweep -25 x30\n";

bs::PartParams randomParams (std::mt19937& rng, bs::PartParams p, bool structural)
{
    auto uni = [&] (int lo, int hi) { return lo + static_cast<int> (rng() % static_cast<std::uint32_t> (hi - lo + 1)); };
    p.osc1Wave = uni (1, 8);
    p.osc2Wave = uni (0, 8);
    p.osc3Wave = uni (0, 3);
    p.osc2Coarse = uni (-12, 12);
    p.osc1Pw = static_cast<float> (uni (0, 100));
    p.osc2Pw = static_cast<float> (uni (0, 100));
    p.osc1Attack = uni (0, 4);
    p.osc1Decay = uni (0, 9);
    p.osc1Sustain = uni (0, 15);
    p.osc1Release = uni (0, 8);
    p.osc2Release = uni (0, 8);
    p.osc2Sync = uni (0, 1);
    p.osc3Ring = uni (0, 1);
    p.osc1Filter = uni (0, 1);
    p.osc2Filter = uni (0, 1);
    p.osc3Mute = uni (0, 1);
    p.filterCutoff = static_cast<float> (uni (100, 2047));
    p.filterResonance = uni (0, 15);
    p.filterMode = uni (0, 7);
    p.chipVolume = uni (5, 15);
    p.chipVariation = static_cast<float> (uni (0, 100));
    p.velVolume = 0.7f;
    p.portaTime = uni (0, 3) == 0 ? 0.2f : 0.0f;
    if (structural)
    {
        p.chipModel = uni (0, 1);
        p.filterTopology = uni (0, 1);
        p.chipResolution = uni (0, 1);
        p.chipVolumeDc = uni (0, 1);
    }
    return p;
}

/** Random notes (overlapping: steals of the 16 voices), same-sample on/off, pedal, bend, wheel, All Sound Off;
    parameter actions every so often (structural switches when `structural`), and optionally reset and resume. */
Scenario randomScenario (std::uint32_t seed, int total, bs::PartParams base, bool structural, bool resets, int assignMode = -1, bool allSoundOff = true)
{
    std::mt19937 rng (seed);
    auto uni = [&] (int lo, int hi) { return lo + static_cast<int> (rng() % static_cast<std::uint32_t> (hi - lo + 1)); };
    Scenario sc;
    sc.total = total;
    base.assignMode = assignMode >= 0 ? assignMode : uni (0, 9) < 7 ? 0 : uni (1, 2);
    base.unisonVoices = uni (0, 3);
    sc.start = randomParams (rng, base, false);
    std::vector<int> heldNotes;
    for (int t = uni (0, 50); t < total - 2000; t += uni (1, 400))
    {
        const int k = uni (0, 99);
        if (k < 45 || heldNotes.empty())
        {
            const int n = uni (36, 84);
            sc.events.push_back (on (t, n, static_cast<float> (uni (10, 100)) / 100.0f));
            heldNotes.push_back (n);
            if (uni (0, 9) == 0)
            {
                sc.events.push_back (off (t, n)); // on and off in the same sample
                heldNotes.pop_back();
            }
        }
        else if (k < 85)
        {
            const auto i = static_cast<std::size_t> (uni (0, static_cast<int> (heldNotes.size()) - 1));
            sc.events.push_back (off (t, heldNotes[i]));
            heldNotes.erase (heldNotes.begin() + static_cast<std::ptrdiff_t> (i));
        }
        else if (k < 90)
            sc.events.push_back (control (t, vs::EventType::SustainPedal, static_cast<float> (uni (0, 1))));
        else if (k < 94)
            sc.events.push_back (control (t, vs::EventType::PitchBend, static_cast<float> (uni (-100, 100)) / 100.0f));
        else if (k < 98)
            sc.events.push_back (control (t, vs::EventType::ModWheel, static_cast<float> (uni (0, 100)) / 100.0f));
        else if (k < 99 && allSoundOff)
            sc.events.push_back (control (t, vs::EventType::AllSoundOff));
        else if (k < 99)
            sc.events.push_back (control (t, vs::EventType::ModWheel, 0.0f));
        else
            sc.events.push_back (control (t, vs::EventType::AllNotesOff));
    }
    // everything released and the pedal up at the end: then every voice must go idle
    sc.events.push_back (control (total - 2000, vs::EventType::SustainPedal, 0.0f));
    for (const int n : heldNotes)
        sc.events.push_back (off (total - 2000, n));
    std::stable_sort (sc.events.begin(), sc.events.end(), [] (const vs::NoteEvent& a, const vs::NoteEvent& b) { return a.offset < b.offset; });

    bs::PartParams cur = sc.start;
    for (int t = uni (500, 3000); t < total - 3000; t += uni (500, 4000))
    {
        Action a;
        a.at = t;
        const int k = uni (0, 9);
        if (resets && k == 0)
            a.kind = Action::reset;
        else if (resets && k == 1)
            a.kind = Action::resume;
        else
        {
            a.kind = Action::params;
            cur = randomParams (rng, cur, structural && uni (0, 2) == 0);
            a.p = cur;
        }
        sc.actions.push_back (a);
    }
    return sc;
}

double maxDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    double w = 0.0;
    for (std::size_t i = 0; i < std::min (a.size(), b.size()); ++i)
        w = std::max (w, std::abs (static_cast<double> (a[i]) - static_cast<double> (b[i])));
    return w;
}

bool sameBitsF (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
}
} // namespace

// ==== 1. the block path ============================================================================================

TEST_CASE ("Review2: the chain's block path gives the same bits as the per-sample path (randomised: every write kind, any call length, suspend, reset, reconfigure, variation)", "[review2][chain]")
{
    chainScanAll (2, 700, 11u);
}

TEST_CASE ("Review2 (full): the chain's block path against the per-sample path, many seeds", "[.review2-full][chain]")
{
    chainScanAll (25, 1500, 90001u);
}

TEST_CASE ("Review2: Per Note through the part: every voice's block path off gives the same bits (random notes, steals, parameters)", "[review2][blockpath]")
{
    static const auto prog = compile (kLeadTable);
    int compared = 0;
    for (const int model : { 1, 0 })
        for (const HostConfig h : { HostConfig { 48000.0, 2 }, HostConfig { 44100.0, 4 }, HostConfig { 96000.0, 1 } })
            for (std::uint32_t seed : { 3u, 4u })
            {
                bs::PartParams base;
                base.chipModel = model;
                base.chipVolumeDc = static_cast<int> (seed & 1u);
                Scenario sc = randomScenario (seed * 31u + static_cast<std::uint32_t> (model), static_cast<int> (0.5 * h.rate), base, false, false);
                if (seed == 4u)
                    sc.program = &prog;
                const auto fast = runScenario (h, sc, 77u + seed);
                const auto slow = runScenario (h, sc, 77u + seed, true);
                INFO ("Model " << (model == 0 ? "A" : "B") << " " << h.rate << " Hz x" << h.factor << " seed " << seed << ": largest difference " << maxDiff (fast.y, slow.y));
                REQUIRE (fast.finite);
                REQUIRE (slow.finite);
                CHECK (peakOf (fast.y) > 1.0e-3);
                CHECK (sameBitsF (fast.y, slow.y));
                ++compared;
            }
    CHECK (compared == 12);
}

TEST_CASE ("Review2 (full): Per Note through the part, block path off against on, many seeds", "[.review2-full][blockpath]")
{
    static const auto prog = compile (kLeadTable);
    int bad = 0, runs = 0;
    for (std::uint32_t seed = 100; seed < 140; ++seed)
        for (const HostConfig h : hostConfigs())
        {
            bs::PartParams base;
            base.chipModel = static_cast<int> (seed % 2u);
            base.chipVolumeDc = static_cast<int> ((seed / 2u) % 2u);
            base.chipResolution = static_cast<int> ((seed / 4u) % 2u);
            Scenario sc = randomScenario (seed, static_cast<int> (0.4 * h.rate), base, false, false);
            if (seed % 3u == 0)
                sc.program = &prog;
            const auto fast = runScenario (h, sc, seed);
            const auto slow = runScenario (h, sc, seed, true);
            ++runs;
            if (! sameBitsF (fast.y, slow.y) || ! fast.finite || ! slow.finite)
            {
                ++bad;
                INFO ("seed " << seed << " " << h.rate << " x" << h.factor << ": largest difference " << maxDiff (fast.y, slow.y));
                CHECK (false);
            }
        }
    std::printf ("[review2 blockpath full] %d runs, %d differ\n", runs, bad);
}

// ==== 2. host-block independence with shares, steals, switches, resets in flight ==================================

namespace
{
void splitScan (std::uint32_t seed0, int seeds, bool structural, bool resets, bool topologyShared, double& worstOut, int& stateMismatch, bool allSoundOff = true)
{
    static const auto prog = compile (kLeadTable);
    for (int s = 0; s < seeds; ++s)
        for (const int model : { 1, 0 })
        {
            const HostConfig h = s % 3 == 0 ? HostConfig { 48000.0, 2 } : (s % 3 == 1 ? HostConfig { 44100.0, 4 } : HostConfig { 96000.0, 1 });
            bs::PartParams base;
            base.chipModel = model;
            base.filterTopology = topologyShared ? 1 : 0;
            base.chipVolumeDc = s % 2;
            const auto seed = seed0 + static_cast<std::uint32_t> (s * 101 + model);
            Scenario sc = randomScenario (seed, static_cast<int> (0.6 * h.rate), base, structural, resets, -1, allSoundOff);
            if (s % 2 == 1)
                sc.program = &prog;
            const auto ref = runScenario (h, sc, 0);
            REQUIRE (ref.finite);
            for (const std::uint32_t bseed : { seed + 1u, seed + 2u })
            {
                const auto r = runScenario (h, sc, bseed);
                const double w = maxDiff (r.y, ref.y);
                worstOut = std::max (worstOut, w);
                INFO ("seed " << seed << " (Model " << (model == 0 ? "A" : "B") << ", " << h.rate << " x" << h.factor << ", block seed " << bseed
                              << "): largest difference " << w << " (" << db (w) << " dB)");
                CHECK (r.finite);
                CHECK (w <= 1.0e-6);
                for (std::size_t v = 0; v < r.states.size(); ++v)
                {
                    const bool same = r.states[v] == ref.states[v] && r.ticks[v] == ref.ticks[v] && r.active[v] == ref.active[v] && r.pending[v] == ref.pending[v];
                    if (! same)
                        ++stateMismatch;
                    INFO ("voice " << v);
                    CHECK (same);
                }
                // nothing hangs: every key is up at total - 2000 host samples (pedal up), so after the tail (release 8
                // at most is about 0.3 s: not all are idle yet at the end of a 0.6 s render; checked separately)
            }
        }
}
} // namespace

TEST_CASE ("Review2: Shared with shares in flight (joins, leaves, steals, unison, no All Sound Off): host blocks change no digital state, audio within -120 dB", "[review2][blocks][shared]")
{
    double worst = 0.0;
    int states = 0;
    splitScan (500u, 3, false, false, true, worst, states, false);
    std::printf ("[review2 split Shared, no All Sound Off] largest audio difference %.3g, %d voice states differ\n", worst, states);
}

TEST_CASE ("Review2: Shared with shares in flight and All Sound Off in the stream: host blocks change no digital state, audio within -120 dB", "[review2][blocks][shared][allsoundoff]")
{
    double worst = 0.0;
    int states = 0;
    splitScan (500u, 3, false, false, true, worst, states, true);
    std::printf ("[review2 split Shared, with All Sound Off] largest audio difference %.3g, %d voice states differ\n", worst, states);
}

TEST_CASE ("Review2: structural switches, reset and resume mid-ramp in random streams: host blocks change no digital state, audio within -120 dB", "[review2][blocks][switch]")
{
    double worst = 0.0;
    int states = 0;
    splitScan (900u, 2, true, true, true, worst, states);
    splitScan (950u, 2, true, true, false, worst, states);
    std::printf ("[review2 split switches] largest audio difference %.3g, %d voice states differ\n", worst, states);
}

TEST_CASE ("Review2 (full): host-block independence, many seeds, every topology, switches and resets", "[.review2-full][blocks]")
{
    double worst = 0.0;
    int states = 0;
    splitScan (2000u, 20, false, false, true, worst, states, false);
    splitScan (3000u, 20, true, true, true, worst, states);
    splitScan (4000u, 20, true, true, false, worst, states);
    std::printf ("[review2 split full] largest audio difference %.3g, %d voice states differ\n", worst, states);
}

// ==== 3. nothing waits forever, nothing hangs ======================================================================

namespace
{
/** A random stream, then a long silent tail: every voice idle, nothing pending, the output silent at the end. */
void hangScan (std::uint32_t seed0, int seeds, int& hung)
{
    std::string err;
    const auto prog = compile (kEndingTable, &err);
    INFO ("table: " << err);
    REQUIRE (err.empty());
    for (int s = 0; s < seeds; ++s)
    {
        const auto seed = seed0 + static_cast<std::uint32_t> (s);
        const HostConfig h = s % 2 == 0 ? HostConfig { 48000.0, 2 } : HostConfig { 32000.0, 4 };
        bs::PartParams base;
        base.chipModel = static_cast<int> (seed % 2u);
        base.filterTopology = static_cast<int> ((seed / 2u) % 2u);
        base.chipVolumeDc = static_cast<int> ((seed / 4u) % 2u);
        Scenario sc = randomScenario (seed, static_cast<int> (0.6 * h.rate), base, true, false);
        if (s % 3 == 0)
            sc.program = &prog;
        sc.total += static_cast<int> (3.0 * h.rate); // release 8 at most; idle rule 0.5 s + fade
        const auto o = runScenario (h, sc, seed + 5u);
        bool anyActive = false, anyPending = false;
        for (std::size_t v = 0; v < o.active.size(); ++v)
        {
            anyActive = anyActive || o.active[v];
            anyPending = anyPending || o.pending[v];
        }
        const double tail = peakOf (o.y, o.y.size() - static_cast<std::size_t> (0.1 * h.rate));
        INFO ("seed " << seed << ": active " << anyActive << " pending " << anyPending << " tail peak " << tail);
        CHECK (o.finite);
        CHECK_FALSE (anyActive);
        CHECK_FALSE (anyPending);
        CHECK (tail <= 1.0e-6);
        if (anyActive || anyPending)
            ++hung;
    }
}
} // namespace

TEST_CASE ("Review2: random streams with switches during fades, steals during fades: after all keys are up every voice goes idle, nothing waits", "[review2][hang]")
{
    int hung = 0;
    hangScan (7000u, 8, hung);
    std::printf ("[review2 hang] %d of 8 runs left a voice active or waiting\n", hung);
}

TEST_CASE ("Review2 (full): no hang, many seeds", "[.review2-full][hang]")
{
    int hung = 0;
    hangScan (8000u, 80, hung);
    std::printf ("[review2 hang full] %d of 80 runs left a voice active or waiting\n", hung);
}

// ==== 4. late notes ================================================================================================

namespace
{
/** The internal sample at which voice 0's engine got its note-off, through sinceNoteOff (counts internal samples
    from the note-off on). Returns -1 when it never did. */
struct LateResult
{
    std::int64_t onAt = -1, offAt = -1; // internal samples, part time, of the sounding note's start and release
};

LateResult lateNote (HostConfig h, int topology, int noteOnHost, int noteOffHost, int block, bool secondSwitch)
{
    bs::PartParams p;
    p.filterTopology = topology;
    p.osc1Release = 0;
    Rig rig (h, p, 512);
    // a sounding note so that the switch fades (a switch with nothing sounding is instant)
    rig.run (static_cast<int> (0.02 * h.rate), { on (0, 40) }, block);
    auto q = p;
    q.chipVolumeDc = 1; // a structural switch at the next render
    rig.set (q);
    LateResult r;
    const int os = rig.part->getOversamplingFactor();
    std::vector<vs::NoteEvent> ev { on (noteOnHost, 64), off (noteOffHost, 64) };
    // render host sample by host sample in blocks of `block`, watching voice 1 (the late note's voice)
    const int total = static_cast<int> (0.05 * h.rate);
    bool switchedAgain = false;
    for (int t = 0; t < total;)
    {
        int len = std::min (block, total - t);
        if (t < 40)
            len = std::min (len, 40 - t); // the second switch comes at host sample 40 whatever the block length
        std::vector<vs::NoteEvent> chunk;
        for (const auto& e : ev)
            if (e.offset >= t && e.offset < t + len)
            {
                auto c = e;
                c.offset -= t;
                chunk.push_back (c);
            }
        if (secondSwitch && ! switchedAgain && t >= 40)
        {
            auto q2 = q;
            q2.chipResolution = 1; // a second switch while the first one fades (it waits for the first)
            rig.set (q2);
            switchedAgain = true;
        }
        std::vector<float> out (static_cast<std::size_t> (len));
        rig.part->render (out.data(), len, chunk);
        t += len;
        for (int i = 0; i < bs::Part::kVoices; ++i)
        {
            const auto& v = rig.part->getVoice (i);
            if (v.active && v.note == 64 && r.onAt < 0)
                r.onAt = rig.part->getRenderedSamples() - 1; // at most a block late; refined below
            if (v.active && v.note == 64 && ! v.held && r.offAt < 0)
                r.offAt = rig.part->getRenderedSamples() - v.sinceNoteOff;
        }
        (void) os;
    }
    return r;
}
} // namespace

TEST_CASE ("Review2: a late note's release lands on the same part sample whatever the host's block length, Per Note and Shared, a second switch inside the fade", "[review2][late]")
{
    for (const int topology : { 0, 1 })
        for (const bool second : { false, true })
            for (const int onAt : { 0, 37, 70 })
                for (const int len : { 0, 25 })
                {
                    const HostConfig h { 48000.0, 2 };
                    const auto ref = lateNote (h, topology, onAt, onAt + len, 128, second);
                    INFO ((topology == 0 ? "Per Note" : "Shared") << (second ? ", a second switch inside the fade" : "") << ": note-on at host " << onAt << ", length " << len
                                                                  << " host samples; reference released at internal sample " << ref.offAt);
                    REQUIRE (ref.offAt >= 0);
                    for (const int block : { 1, 7, 64 })
                    {
                        const auto r = lateNote (h, topology, onAt, onAt + len, block, second);
                        INFO ("blocks of " << block << ": released at " << r.offAt);
                        CHECK (r.offAt == ref.offAt);
                    }
                }
}

TEST_CASE ("Review2 (full): late-note release timing, every offset in the fade, both topologies, three rates", "[.review2-full][late]")
{
    int bad = 0, runs = 0;
    for (const HostConfig h : { HostConfig { 48000.0, 2 }, HostConfig { 44100.0, 4 }, HostConfig { 96000.0, 1 } })
        for (const int topology : { 0, 1 })
            for (const bool second : { false, true })
                for (int onAt = 0; onAt < 140; onAt += 3)
                    for (const int len : { 0, 1, 2, 30, 200 })
                    {
                        const auto ref = lateNote (h, topology, onAt, onAt + len, 128, second);
                        for (const int block : { 1, 5, 33 })
                        {
                            const auto r = lateNote (h, topology, onAt, onAt + len, block, second);
                            ++runs;
                            if (r.offAt != ref.offAt || ref.offAt < 0)
                            {
                                ++bad;
                                INFO (h.rate << " x" << h.factor << (topology == 0 ? " Per Note" : " Shared") << " on " << onAt << " len " << len << " block " << block << ": " << r.offAt << " vs " << ref.offAt);
                                CHECK (false);
                            }
                        }
                    }
    std::printf ("[review2 late full] %d runs, %d differ\n", runs, bad);
}

// ==== 5. Shared shares =============================================================================================

TEST_CASE ("Review2: Shared: an idle voice leaves no later than Per Note while another note sounds (low-pass and band-pass, volume DC off and on, both models)", "[review2][shared][idle]")
{
    // The contract: judged on its own contribution, an idle voice in Shared leaves within the same times as Per Note.
    // A held note A and a second note B, played for 0.3 s and released; the time from B's note-off until it leaves.
    for (const int model : { 1, 0 })
        for (const int mode : { 1, 2 })
            for (const int vdc : { 0, 1 })
            {
                int leftMs[2] { -1, -1 };
                for (const int topology : { 0, 1 })
                {
                    bs::PartParams p;
                    p.chipModel = model;
                    p.filterTopology = topology;
                    p.filterMode = mode;
                    p.chipVolumeDc = vdc;
                    p.osc1Release = 1;
                    const HostConfig h { 48000.0, 2 };
                    Rig rig (h, p, 48);
                    rig.run (4800, { on (0, 48) }, 48);
                    rig.run (14400, { on (0, 67) }, 48);
                    REQUIRE (rig.part->numActiveVoices() == 2);
                    rig.run (0, { off (0, 67) });
                    for (int ms = 0; ms < 900 && leftMs[topology] < 0; ++ms)
                    {
                        rig.run (48, {}, 48);
                        if (rig.part->numActiveVoices() == 1)
                            leftMs[topology] = ms;
                    }
                }
                std::printf ("[review2 idle] Model %s, mode %d, volume DC %d: B leaves %d ms after its note-off Per Note, %d ms in Shared\n", model == 0 ? "A" : "B", mode, vdc,
                             leftMs[0], leftMs[1]);
                INFO ("Model " << (model == 0 ? "A" : "B") << ", mode " << mode << ", volume DC " << vdc << ": Per Note " << leftMs[0] << " ms, Shared " << leftMs[1] << " ms");
                CHECK (leftMs[0] >= 0);
                CHECK (leftMs[1] >= 0);
                CHECK (leftMs[1] <= leftMs[0] + 5);
            }
}

TEST_CASE ("Review2: the stated steal dip: the weights' arithmetic of Part::sharedWeights over a steal's fade, n carriers", "[review2][shared][dip]")
{
    // Part.h "SHARES" states: during a steal's 3 ms fade the audible sum dips by at most a quarter of the fading chip's
    // share, at mid-fade. The part's rule (sharedWeights, mixVoice): every carrier at share 1; the stolen chip's share
    // and its output fade both fall linearly from 1 to 0 over the fade; weight = share / sum. What the other notes'
    // filtered sound is heard at: sum over chips of weight x fade. Its dip from 1, in units of the stolen chip's
    // weight before the steal (1 / n), the largest over the fade, and where.
    for (const int n : { 2, 3, 4, 8, 16 })
    {
        const int fade = 288; // 3 ms at 96 kHz
        double worst = 0.0, worstAt = 0.0;
        for (int i = 0; i <= fade; ++i)
        {
            const double t = static_cast<double> (i) / fade;
            const double s = 1.0 - t;
            const double heard = (s * s + (n - 1)) / (s + (n - 1));
            const double dip = (1.0 - heard) * n; // in units of the chip's weight 1 / n
            if (dip > worst)
            {
                worst = dip;
                worstAt = t;
            }
        }
        std::printf ("[review2 dip] %2d carriers: the heard filter output dips by at most %.3f of the stolen chip's weight (%.4f of the whole, %.2f dB), at %.0f %% of the fade\n", n,
                     worst, worst / n, db (1.0 - worst / n), 100.0 * worstAt);
        CHECK (worst > 0.25); // the statement "at most a quarter" does not hold for any finite n
        CHECK (worst < 0.35);
    }
}

// ==== 6. All Sound Off: chip time and host-block independence (outside this round's diff; found by the scans) ======

namespace
{
/** Eight held notes, All Sound Off, nine more notes (the ninth lands on a chip that All Sound Off stopped): the reduced
    case of the random scan's failures (seed 500), velocities fixed. */
Scenario allSoundOffScenario (int model, int topology)
{
    Scenario sc;
    sc.total = 28800;
    sc.start.chipModel = model;
    sc.start.filterTopology = topology;
    sc.start.osc2Wave = 8;
    sc.start.osc3Wave = 2;
    sc.start.osc2Sync = 1;
    sc.start.osc3Ring = 1;
    sc.start.osc1Decay = 7;
    sc.start.osc1Sustain = 13;
    sc.start.osc1Release = 5;
    sc.start.filterMode = 5;
    sc.start.filterResonance = 3;
    sc.start.filterCutoff = 594.0f;
    sc.start.osc2Filter = 0;
    const std::vector<std::pair<int, int>> notes { { 1575, 40 }, { 1885, 37 }, { 2782, 59 }, { 3741, 82 }, { 3949, 49 }, { 4317, 54 }, { 5012, 36 }, { 5828, 58 }, { 6588, 59 },
                                                   { 7628, 46 }, { 7894, 56 }, { 8312, 44 }, { 8764, 62 }, { 9928, 60 }, { 10617, 71 }, { 10731, 79 }, { 11211, 65 } };
    for (const auto& n : notes)
        sc.events.push_back (on (n.first, n.second, 0.7f));
    sc.events.push_back (control (6284, vs::EventType::AllSoundOff));
    std::stable_sort (sc.events.begin(), sc.events.end(), [] (const vs::NoteEvent& a, const vs::NoteEvent& b) { return a.offset < b.offset; });
    return sc;
}
} // namespace

TEST_CASE ("Review2: after All Sound Off every chip of the part is at the part's cycle (Per Note and Shared)", "[review2][time][allsoundoff]")
{
    // 8.21.1 "Time": an idle voice's chip time keeps running with the part's. A chip stopped by All Sound Off
    // (Part::killVoice: the engine's note-off and idle writes at the chip's cycle, then ChipVoice::suspend) is found
    // one internal sample (about 11 cycles) ahead of the chips that went idle any other way, from the block after the
    // event on, whatever the block length.
    for (const int topology : { 0, 1 })
        for (const int block : { 1, 512 })
        {
            const Scenario sc = allSoundOffScenario (1, topology);
            Rig rig ({ 48000.0, 2 }, sc.start, 512);
            std::size_t next = 0;
            std::vector<float> y (512);
            std::int64_t worst = 0;
            int firstAt = -1;
            for (int t = 0; t < 8000; t += block)
            {
                std::vector<vs::NoteEvent> chunk;
                while (next < sc.events.size() && sc.events[next].offset < t + block)
                {
                    auto e = sc.events[next++];
                    e.offset -= t;
                    chunk.push_back (e);
                }
                rig.part->render (y.data(), block, chunk);
                const auto c15 = static_cast<std::int64_t> (rig.part->getVoice (15).chain.chip().cycle()); // never played
                for (int i = 0; i < bs::Part::kVoices; ++i)
                {
                    const auto d = static_cast<std::int64_t> (rig.part->getVoice (i).chain.chip().cycle()) - c15;
                    if (d != 0 && firstAt < 0)
                        firstAt = t + block;
                    worst = std::max (worst, d < 0 ? -d : d);
                }
            }
            INFO ((topology == 0 ? "Per Note" : "Shared") << ", blocks of " << block << ": largest cycle difference between chips " << worst << ", first after host sample "
                                                          << firstAt << " (All Sound Off at 6284)");
            CHECK (worst == 0);
        }
}

TEST_CASE ("Review2: Shared, a note on a chip that All Sound Off stopped: the output does not depend on the host's blocks", "[review2][blocks][allsoundoff]")
{
    // Gate 2 (8.21.1): audio within -120 dB for any block length. Fails by up to about -22 dB (Model B) and -26 dB (Model A), Shared only,
    // the random scan's case is the same on 43d8d08 (before this round): the stopped chip is a sample ahead of the part's clock (test above), so
    // in Shared's lockstep (Part::renderShared) its renderBusesUntil can stop short of the segment (`jassert (got == m)`,
    // compiled out in Release) and the rest of the segment's buses are those of an earlier call.
    for (const int model : { 1, 0 })
        for (const int topology : { 0, 1 })
        {
            const Scenario sc = allSoundOffScenario (model, topology);
            const HostConfig h { 48000.0, 2 };
            Rig r0 (h, sc.start, 512);
            const auto ref = r0.run (sc.total, sc.events, 512);
            for (const int block : { 1, 64 })
            {
                Rig r1 (h, sc.start, 512);
                const auto y = r1.run (sc.total, sc.events, block);
                const double w = maxDiff (y, ref);
                INFO ("Model " << (model == 0 ? "A" : "B") << (topology == 0 ? ", Per Note" : ", Shared") << ", blocks of " << block << " against 512: largest difference " << w << " ("
                               << db (w) << " dB)");
                CHECK (w <= 1.0e-6);
            }
        }
}

// ==== 7. Shared: the held notes' level when a note of another velocity joins =======================================

TEST_CASE ("Review2: Shared: a soft note that joins does not change the level of a held loud note's filtered sound", "[review2][shared][velocity]")
{
    // The contract: a note that joins does not disturb a held note beyond its own sound. Each chip's output, its share of
    // the shared filter's output included, is multiplied by its OWN note gain (Part::mixVoice): the held note's
    // filtered sound is heard at the share-weighted mean of the note gains. Measured on the held note's fundamental
    // (130.8 Hz; the joining note's partials are multiples of 277.2 Hz, none near it), over whole periods, before and
    // after the join, Per Note and Shared.
    const auto partial = [] (const std::vector<float>& y, std::size_t from, double hz, double rate) {
        const auto period = rate / hz;
        const auto n = static_cast<std::size_t> (std::floor (40.0 * period));
        double re = 0.0, im = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double ph = 6.283185307179586 * hz * static_cast<double> (i) / rate;
            re += static_cast<double> (y[from + i]) * std::cos (ph);
            im += static_cast<double> (y[from + i]) * std::sin (ph);
        }
        return 2.0 * std::sqrt (re * re + im * im) / static_cast<double> (n);
    };
    for (const int model : { 1, 0 })
        for (const float velVolume : { 0.5f, 1.0f })
        {
            double ratio[2] { 0.0, 0.0 };
            for (const int topology : { 0, 1 })
            {
                bs::PartParams p;
                p.chipModel = model;
                p.filterTopology = topology;
                p.filterMode = 1;
                p.filterCutoff = 1400.0f;
                p.velVolume = velVolume;
                p.osc1Wave = 2; // saw
                const HostConfig h { 48000.0, 2 };
                Rig rig (h, p, 512);
                const auto y = rig.run (96000, { on (0, 48, 1.0f), on (48000, 61, 0.01f) });
                const double before = partial (y, 30000, 130.8127826502993, h.rate);
                const double after = partial (y, 60000, 130.8127826502993, h.rate);
                ratio[topology] = after / before;
            }
            std::printf ("[review2 velocity] Model %s, vel.volume %.1f: the held note's fundamental after a soft note joins: Per Note %+.2f dB, Shared %+.2f dB\n",
                         model == 0 ? "A" : "B", static_cast<double> (velVolume), db (ratio[0]), db (ratio[1]));
            INFO ("Model " << (model == 0 ? "A" : "B") << ", vel.volume " << velVolume << ": Per Note " << db (ratio[0]) << " dB, Shared " << db (ratio[1]) << " dB");
            CHECK (std::abs (db (ratio[0])) < 0.2);
            CHECK (std::abs (db (ratio[1]) - db (ratio[0])) < 0.5);
        }
}
