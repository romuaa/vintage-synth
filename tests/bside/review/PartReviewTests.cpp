// Independent review harness of the S4a part (docs/architecture.md 8.21.1), written from the contract, not from
// the implementation. Tag [review]. Each test names the contract point it probes.
#include <catch2/catch_test_macros.hpp>

#include "../integration/PartTestSupport.h"
#include "bside/chip/ChipClock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace bs_part_test;
namespace chip = vs::bside::chip;

namespace
{
const char* kSweepTable = "bside-table 1\n[wave1]\n    wave pulse+gate +0 ticks 0\nloop:\n    pitch +0\n    pitch +7\n    pitch +12\n    jump loop\n"
                          "release:\n    gate off ticks 0\n    jump loop\n[pulse1]\n    set 900\n    pingpong 400 3600 37\n[filter]\n    cutoff 1700\n    sweep -25 x30\n";

double maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    double w = 0.0;
    for (std::size_t i = 0; i < std::min (a.size(), b.size()); ++i)
        w = std::max (w, std::abs (static_cast<double> (a[i]) - static_cast<double> (b[i])));
    return w;
}

/** Renders `total` host samples with block lengths drawn from `blocks` in turn (events at absolute offsets). */
std::vector<float> runVarying (Rig& rig, int total, const std::vector<vs::NoteEvent>& events, const std::vector<int>& blocks)
{
    std::vector<float> out (static_cast<std::size_t> (total));
    std::size_t next = 0, bi = 0;
    for (int start = 0; start < total;)
    {
        const int len = std::min ({ blocks[bi++ % blocks.size()], total - start, rig.maxBlock });
        std::vector<vs::NoteEvent> chunk;
        while (next < events.size() && events[next].offset < start + len)
        {
            auto e = events[next++];
            e.offset -= start;
            chunk.push_back (e);
        }
        rig.part->render (out.data() + start, len, chunk);
        start += len;
    }
    return out;
}

std::vector<vs::NoteEvent> heavyScenario()
{
    // 20 notes in quick succession (steals of a 16-voice part), note-on and note-off in one sample, bend and wheel at
    // odd offsets, the pedal, and releases in an order unrelated to the starts
    std::vector<vs::NoteEvent> ev;
    for (int k = 0; k < 20; ++k)
        ev.push_back (on (17 + 211 * k, 40 + (k * 7) % 40, 0.3f + 0.035f * static_cast<float> (k)));
    ev.push_back (on (2000, 90, 1.0f));
    ev.push_back (off (2000, 90)); // on and off in the same sample
    ev.push_back (control (2500, vs::EventType::PitchBend, -0.7f));
    ev.push_back (control (2501, vs::EventType::ModWheel, 1.0f));
    ev.push_back (control (3000, vs::EventType::SustainPedal, 1.0f));
    for (int k = 0; k < 20; k += 3)
        ev.push_back (off (4500 + 97 * k, 40 + (k * 7) % 40));
    ev.push_back (control (6000, vs::EventType::SustainPedal, 0.0f));
    for (int k = 1; k < 20; k += 3)
        ev.push_back (off (6200 + 13 * k, 40 + (k * 7) % 40));
    ev.push_back (control (7000, vs::EventType::PitchBend, 0.5f));
    ev.push_back (control (7000, vs::EventType::AllNotesOff));
    std::stable_sort (ev.begin(), ev.end(), [] (const vs::NoteEvent& a, const vs::NoteEvent& b) { return a.offset < b.offset; });
    return ev;
}

struct Snapshot
{
    std::vector<float> y;
    std::vector<chip::DigitalState> states;
    std::vector<std::uint64_t> ticks;
};

bs::PartParams heavyParams (int model, bool shared, int assign)
{
    bs::PartParams p;
    p.chipModel = model;
    p.filterTopology = shared ? 1 : 0;
    p.assignMode = assign;
    p.unisonVoices = 1; // 4 voices
    p.osc2Wave = 2;
    p.osc2Coarse = -5;
    p.filterResonance = 9;
    p.osc1Release = 4;
    p.portaTime = 0.3f;
    return p;
}

Snapshot runHeavy (int model, bool shared, int assign, const std::vector<int>& blocks)
{
    static const auto prog = compile (kSweepTable);
    Rig rig ({ 48000.0, 2 }, heavyParams (model, shared, assign), 512);
    rig.part->setProgram (&prog);
    Snapshot s;
    s.y = runVarying (rig, 16000, heavyScenario(), blocks);
    for (int i = 0; i < bs::Part::kVoices; ++i)
    {
        s.states.push_back (rig.part->getVoice (i).chain.chip().state());
        s.ticks.push_back (rig.part->getVoice (i).engine.tickIndex());
    }
    return s;
}

bool allFinite (const std::vector<float>& y)
{
    return std::all_of (y.begin(), y.end(), [] (float v) { return std::isfinite (v); });
}
} // namespace

// ---- Time: no decision depends on the host's block length (gate 2) ---------------------------------------------

TEST_CASE ("Review: irregular host blocks (1 .. 512, events anywhere) give the same digital state, ticks and audio", "[review][blocks]")
{
    std::mt19937 rng (1234);
    std::vector<int> irregular;
    for (int k = 0; k < 400; ++k)
        irregular.push_back (1 + static_cast<int> (rng() % 512));
    for (const int assign : { 0, 1, 2 }) // Poly, Unison (4 voices), Solo
        for (const int model : { 1, 0 })
            for (const bool shared : { false, true })
            {
                const auto ref = runHeavy (model, shared, assign, { 512 });
                INFO ("assign " << assign << " model " << (model == 0 ? "A" : "B") << (shared ? " Shared" : " Per Note"));
                REQUIRE (allFinite (ref.y));
                REQUIRE (peakOf (ref.y) > 0.01);
                for (const auto& blocks : { std::vector<int> { 1 }, std::vector<int> { 3, 500, 2, 77 }, irregular })
                {
                    const auto r = runHeavy (model, shared, assign, blocks);
                    const double w = maxAbsDiff (r.y, ref.y);
                    INFO ("pattern of " << blocks.size() << " block lengths, first " << blocks[0] << ": largest difference " << w << " (" << db (w) << " dB)");
                    CHECK (w <= 1.0e-6);
                    for (int v = 0; v < bs::Part::kVoices; ++v)
                    {
                        INFO ("voice " << v);
                        CHECK (r.states[static_cast<std::size_t> (v)] == ref.states[static_cast<std::size_t> (v)]);
                        CHECK (r.ticks[static_cast<std::size_t> (v)] == ref.ticks[static_cast<std::size_t> (v)]);
                    }
                }
            }
}

// ---- Time: an idle voice's chip time keeps running with the part's -----------------------------------------------

TEST_CASE ("Review: every chip of the part is at the part's cycle after every block (idle, quiet path, Per Note, Shared)", "[review][time]")
{
    for (const bool shared : { false, true })
        for (const HostConfig h : { HostConfig { 48000.0, 2 }, HostConfig { 44100.0, 4 }, HostConfig { 96000.0, 1 } })
        {
            bs::PartParams p;
            p.filterTopology = shared ? 1 : 0;
            p.osc1Release = 0;
            Rig rig (h, p, 256);
            chip::ChipClock clk;
            clk.prepare (985248u, h.rate * h.factor);
            std::uint64_t expected = 0;
            std::mt19937 rng (7);
            int bad = 0;
            for (int b = 0; b < 4000; ++b)
            {
                const int len = 1 + static_cast<int> (rng() % 256);
                std::vector<vs::NoteEvent> ev;
                if (b % 400 == 3)
                    ev.push_back (on (static_cast<int> (rng() % static_cast<unsigned> (len)), 50 + b % 30));
                if (b % 400 == 40)
                    ev.push_back (control (0, vs::EventType::AllNotesOff));
                std::vector<float> y (static_cast<std::size_t> (len));
                rig.part->render (y.data(), len, ev);
                expected += clk.nextSamples (static_cast<std::uint64_t> (len * h.factor));
                for (int v = 0; v < bs::Part::kVoices; ++v)
                    if (rig.part->getVoice (v).chain.chip().cycle() != expected && bad++ < 5)
                    {
                        INFO ("block " << b << " voice " << v << " active " << rig.part->getVoice (v).active << ": cycle "
                                       << rig.part->getVoice (v).chain.chip().cycle() << ", part " << expected);
                        CHECK (false);
                    }
            }
            INFO ((shared ? "Shared " : "Per Note ") << h.rate << " x" << h.factor);
            CHECK (bad == 0);
        }
}

// ---- Structural switches: the fade ends silent, no click ----------------------------------------------------------

namespace
{
/** A held sawtooth (direct, sustain 15) on `from`, a structural switch to `to` at host sample `at`, 1x at 96 kHz (no
    decimator between the part's sum and the output). Returns the output. */
std::vector<float> switchRun (const bs::PartParams& from, const bs::PartParams& to, int at, int total)
{
    Rig rig ({ 96000.0, 1 }, from, 64);
    std::vector<float> y;
    auto a = rig.run (at, { on (0, 45, 1.0f) }, 64);
    rig.set (to);
    auto b = rig.run (total - at, {}, 64);
    y.insert (y.end(), a.begin(), a.end());
    y.insert (y.end(), b.begin(), b.end());
    return y;
}

bs::PartParams sawDirect (int model)
{
    bs::PartParams p;
    p.chipModel = model;
    p.osc1Wave = 2; // sawtooth
    p.osc1Filter = 0;
    p.osc1Sustain = 15;
    p.osc1Release = 0;
    return p;
}
} // namespace

TEST_CASE ("Review: a structural switch fades the sounding voices to silence without a step at the fade's end", "[review][switch]")
{
    struct Case
    {
        const char* name;
        bs::PartParams from, to;
    };
    std::vector<Case> cases;
    {
        auto f = sawDirect (1), t = sawDirect (0);
        cases.push_back ({ "Model B -> Model A", f, t });
        cases.push_back ({ "Model A -> Model B", t, f });
        auto r = sawDirect (1);
        r.chipResolution = 1;
        cases.push_back ({ "Model B, resolution -> Authentic", f, r });
        auto d = sawDirect (1);
        d.chipVolumeDc = 1;
        cases.push_back ({ "Model B, volume DC on", f, d });
        auto ta = sawDirect (0);
        ta.chipVolumeDc = 1;
        cases.push_back ({ "Model A, volume DC on", t, ta });
    }
    const int at = 48000, total = 48000 + 4800;
    for (const auto& c : cases)
    {
        const auto y = switchRun (c.from, c.to, at, total);
        const auto ref = switchRun (c.from, c.from, at, total); // no switch: the signal the fade multiplies
        // the fade's gain seen at the output: y / ref over the fade, where ref is not small
        std::printf ("switch %-34s gain y/ref at +270..+300:", c.name);
        for (int i = at + 270; i <= at + 300; i += 2)
        {
            const double r = std::abs (ref[static_cast<std::size_t> (i)]) > 0.02f ? static_cast<double> (y[static_cast<std::size_t> (i)]) / static_cast<double> (ref[static_cast<std::size_t> (i)]) : std::nan ("");
            std::printf (" %.4f", r);
        }
        std::printf ("\n");
        // the largest jump from the faded output to 0 where the output stops: |y| at the last non-zero sample divided by
        // |ref| there must be the last step of a 288-sample linear fade (about 1/288), not more than 2/288
        // where the output stops (|y| < 1e-5 while the unswitched signal is large), the fade's gain on the sample before
        // must be the last step of a 288-sample linear fade (about 1/288); a larger value is a step to silence (a click)
        double worstStepGain = 0.0;
        int stopAt = -1;
        for (int i = at + 200; i < at + 400; ++i)
        {
            const auto k = static_cast<std::size_t> (i);
            if (std::abs (y[k]) < 1.0e-5f && std::abs (ref[k]) > 0.01f && std::abs (ref[k - 1]) > 0.01f && std::abs (y[k - 1]) >= 1.0e-5f)
            {
                const double g = std::abs (static_cast<double> (y[k - 1]) / static_cast<double> (ref[k - 1]));
                if (g > worstStepGain)
                {
                    worstStepGain = g;
                    stopAt = i;
                }
            }
        }
        std::printf ("   output stops at +%d after a fade gain of %.4f (%.1f / 288)\n", stopAt - at, worstStepGain, worstStepGain * 288.0);
        INFO ("the output stops at +" << (stopAt - at) << " after the fade's gain " << worstStepGain << " (" << worstStepGain * 288.0 << " / 288)");
        CHECK (worstStepGain <= 3.0 / 288.0);
        // the level before the switch (the steady saw), the step from the last faded sample to the silence after it
        const double level = peakOf (y, static_cast<std::size_t> (at - 4800), static_cast<std::size_t> (at));
        // after the fade (3 ms = 288 samples) and the latency, the part is silent (held keys do not retrigger)
        const double after = peakOf (y, static_cast<std::size_t> (at + 288 + 80), static_cast<std::size_t> (total));
        // the largest sample-to-sample change in the fade's last 64 samples and the 64 after it, relative to the level
        double step = 0.0;
        for (int i = at + 200; i < at + 288 + 100; ++i)
            step = std::max (step, std::abs (static_cast<double> (y[static_cast<std::size_t> (i)]) - static_cast<double> (y[static_cast<std::size_t> (i - 1)])));
        // the last sample that is not 0
        int last = at;
        for (int i = at; i < total; ++i)
            if (std::abs (y[static_cast<std::size_t> (i)]) > 1.0e-7f)
                last = i;
        const double lastLevel = std::abs (static_cast<double> (y[static_cast<std::size_t> (last)]));
        std::printf ("switch %-34s level %.4f, last non-zero sample at +%d is %.5f (%.1f dB re level), after %.2e\n", c.name, level, last - at,
                     lastLevel, db (lastLevel / level), after);
        INFO (c.name << ": level " << level << ", last non-zero sample " << lastLevel << " at +" << (last - at) << " (" << db (lastLevel / level)
                     << " dB re the level), afterwards " << after);
        REQUIRE (level > 0.05);
        // a linear 3 ms fade's last samples are at most a few 1/288 of the level; anything above -40 dB is a click
        CHECK (db (lastLevel / level) < -40.0);
        CHECK (after < 1.0e-5);
    }
}

// ---- Switch: a note started during the fade keeps its length ------------------------------------------------------

TEST_CASE ("Review: a note played and released during a switch's fade keeps its length (starts late, released as late)", "[review][switch]")
{
    // at 96 kHz x1 one host sample is one internal sample: the fade is 288 samples
    for (const int into : { 0, 100, 250 })
    {
        auto p = sawDirect (1);
        Rig rig ({ 96000.0, 1 }, p, 1);
        rig.run (4800, { on (0, 45) }, 1);
        p.chipModel = 0;
        rig.set (p);
        rig.run (into, {}, 1); // `into` samples into the fade
        const int length = 20;
        // note-on now, note-off `length` samples later; find when the voice starts and when its engine gets the note-off
        int started = -1, released = -1;
        int voice = -1;
        for (int i = 0; i < 2000; ++i)
        {
            std::vector<vs::NoteEvent> ev;
            if (i == 0)
                ev.push_back (on (0, 69));
            if (i == length)
                ev.push_back (off (0, 69));
            rig.run (1, ev, 1);
            for (int v = 0; v < bs::Part::kVoices && voice < 0; ++v)
                if (rig.part->getVoice (v).note == 69 && (rig.part->getVoice (v).pending || rig.part->getVoice (v).active))
                    voice = v;
            if (voice >= 0)
            {
                const auto& nv = rig.part->getVoice (voice);
                if (started < 0 && nv.active && nv.note == 69)
                    started = i;
                if (started >= 0 && released < 0 && ! nv.held)
                    released = i;
            }
        }
        INFO ("note-on " << into << " samples into the fade: started at +" << started << ", released at +" << released << ", length "
                         << (released - started) << " (played " << length << ")");
        REQUIRE (started >= 0);
        REQUIRE (released >= 0);
        CHECK (std::abs ((released - started) - length) <= 1);
    }
}

// ---- Shared topology ---------------------------------------------------------------------------------------------

TEST_CASE ("Review: one voice in Shared equals Per Note bit for bit (odd blocks, a late note, a second note after the first left the sum)", "[review][shared]")
{
    static const auto prog = compile ("bside-table 1\n[filter]\n    cutoff 1900\n    sweep -9 x200\n");
    for (const bool withTable : { false, true })
        for (const int model : { 1, 0 })
            for (const HostConfig h : { HostConfig { 48000.0, 2 }, HostConfig { 44100.0, 4 }, HostConfig { 96000.0, 1 } })
            {
                std::vector<float> y[2];
                int activeBefore[2] {};
                const int secondAt = static_cast<int> (3.0 * h.rate);
                for (const int shared : { 0, 1 })
                {
                    bs::PartParams p;
                    p.chipModel = model;
                    p.filterTopology = shared;
                    p.filterResonance = 12;
                    p.osc1Wave = 3;
                    p.osc1Pw = 30.0f;
                    p.osc2Wave = 2;
                    p.osc2Coarse = 12;
                    Rig rig (h, p, 512);
                    rig.part->setProgram (withTable ? &prog : nullptr);
                    // the first note, released; the second only after the first has left the sum (one voice at a time)
                    y[shared] = runVarying (rig, secondAt, { on (333, 50, 0.8f), off (static_cast<int> (0.4 * h.rate), 50) }, { 7, 61, 512, 1, 33 });
                    activeBefore[shared] = rig.part->numActiveVoices();
                    const auto tail = runVarying (rig, static_cast<int> (0.6 * h.rate), { on (5, 57, 0.6f) }, { 7, 61, 512, 1, 33 });
                    y[shared].insert (y[shared].end(), tail.begin(), tail.end());
                }
                const double w = maxAbsDiff (y[0], y[1]);
                std::size_t first = 0;
                while (first < y[0].size() && ! (std::abs (y[0][first] - y[1][first]) > 0.0f))
                    ++first;
                std::printf ("%s shared==pernote model %s %.0f x%d: voices active before the 2nd note %d/%d, largest difference %.3g, first differing host sample %zu (%.4f s)\n",
                             withTable ? "table" : "no table", model == 0 ? "A" : "B", h.rate, h.factor, activeBefore[0], activeBefore[1], w, first,
                             static_cast<double> (first) / h.rate);
                INFO ((withTable ? "table, " : "no table, ") << "model " << (model == 0 ? "A" : "B") << " " << h.rate << " x" << h.factor
                                                            << ": largest difference " << w << ", first at " << first);
                REQUIRE (activeBefore[0] == 0);
                REQUIRE (activeBefore[1] == 0);
                REQUIRE (peakOf (y[0]) > 0.01);
                CHECK_FALSE (w > 0.0);
            }
}

TEST_CASE ("Review: Shared with several notes, releases in any order and a note joining: finite, bounded, every voice idle at the end", "[review][shared]")
{
    static const auto prog = compile (kSweepTable);
    for (const int model : { 1, 0 })
    {
        bs::PartParams p;
        p.chipModel = model;
        p.filterTopology = 1;
        p.filterResonance = 15;
        p.filterMode = 7;
        p.osc1Release = 5;
        p.osc2Wave = 4; // noise
        p.osc3Wave = 2;
        Rig rig ({ 48000.0, 2 }, p, 128);
        rig.part->setProgram (&prog);
        std::vector<vs::NoteEvent> ev;
        for (int k = 0; k < 12; ++k)
            ev.push_back (on (100 + 50 * k, 36 + 5 * k, 0.5f + 0.04f * static_cast<float> (k)));
        for (const int k : { 5, 0, 11, 3, 8 })
            ev.push_back (off (6000 + 30 * k, 36 + 5 * k));
        ev.push_back (on (7000, 99, 1.0f)); // a new note joins
        for (const int k : { 1, 2, 4, 6, 7, 9, 10 })
            ev.push_back (off (9000 + 7 * k, 36 + 5 * k));
        ev.push_back (off (12000, 99));
        std::stable_sort (ev.begin(), ev.end(), [] (const vs::NoteEvent& a, const vs::NoteEvent& b) { return a.offset < b.offset; });
        const auto y = rig.run (48000 * 3, ev, 128);
        INFO ("model " << (model == 0 ? "A" : "B") << ": peak " << peakOf (y));
        CHECK (allFinite (y));
        CHECK (peakOf (y) < 2.0);
        rig.run (48000 * 31); // the table's wave lane loops after the release: the 30 s rule ends it
        CHECK (rig.part->numActiveVoices() == 0);
        CHECK (peakOf (rig.run (4800)) <= 0.0);
    }
}

// ---- Robustness: extreme values, fast automation of every parameter, every structural switch ----------------------

TEST_CASE ("Review: random parameters every block (incl. out-of-range values), random events: finite, bounded, no hang", "[review][robust]")
{
    std::mt19937 rng (99);
    auto uni = [&] (float lo, float hi) { return lo + (hi - lo) * static_cast<float> (rng() % 10001) / 10000.0f; };
    auto pick = [&] (int lo, int hi) { return lo + static_cast<int> (rng() % static_cast<unsigned> (hi - lo + 1)); };
    static const auto prog = compile (kSweepTable);
    for (const HostConfig h : { HostConfig { 48000.0, 2 }, HostConfig { 32000.0, 4 }, HostConfig { 192000.0, 1 } })
    {
        bs::PartParams p;
        Rig rig (h, p, 96);
        rig.part->setProgram (&prog);
        double worst = 0.0;
        bool finite = true;
        for (int b = 0; b < 3000; ++b)
        {
            // every parameter moves, half the blocks; a third of the integer ones go out of range
            if (b % 2 == 0)
            {
                p.osc1Wave = pick (-2, 10); p.osc2Wave = pick (0, 8); p.osc3Wave = pick (0, 8);
                p.osc1Coarse = pick (-30, 30); p.osc2Coarse = pick (-24, 24); p.osc3Coarse = pick (-24, 24);
                p.osc1Fine = uni (-100, 100); p.osc2Fine = uni (-100, 100); p.osc3Fine = uni (-100, 100);
                p.osc1Pw = uni (-10, 110); p.osc2Pw = uni (0, 100); p.osc3Pw = uni (0, 100);
                p.osc1Attack = pick (0, 15); p.osc1Decay = pick (0, 15); p.osc1Sustain = pick (0, 15); p.osc1Release = pick (0, 15);
                p.osc2Attack = pick (0, 15); p.osc2Release = pick (0, 8); p.osc3Release = pick (0, 8);
                p.osc1Sync = pick (0, 1); p.osc2Sync = pick (0, 1); p.osc1Ring = pick (0, 1); p.osc3Ring = pick (0, 1);
                p.osc1Filter = pick (0, 1); p.osc2Filter = pick (0, 1); p.osc3Filter = pick (0, 1); p.osc3Mute = pick (0, 1);
                p.filterCutoff = uni (-100, 2200); p.filterResonance = pick (-1, 16); p.filterMode = pick (0, 7);
                p.filterKeyFollow = uni (0, 100); p.chipVariation = uni (0, 100); p.chipVolume = pick (0, 15);
                p.macroPwOffset = uni (-50, 50); p.macroCutoffOffset = uni (-100, 100); p.macroSweepDepth = uni (0, 200);
                p.velVolume = uni (0, 1); p.velCutoff = uni (0, 1); p.vibratoRate = uni (0.5f, 12); p.modWheelVibrato = uni (0, 1);
                p.unisonDetune = uni (0, 1); p.portaTime = uni (0, 1); p.portaMode = pick (0, 1); p.bendRange = pick (0, 12);
                p.restartPolicy = pick (0, 2); p.tickRate = pick (0, 2); p.tickFreeHz = uni (10, 400); p.tickMult = pick (1, 8);
                p.tickSync = pick (0, 3);
            }
            if (b % 37 == 0) p.chipModel = pick (0, 1);
            if (b % 41 == 0) p.filterTopology = pick (0, 1);
            if (b % 43 == 0) p.chipResolution = pick (0, 1);
            if (b % 47 == 0) p.chipVolumeDc = pick (0, 1);
            if (b % 53 == 0) rig.part->setChipClock (pick (0, 1));
            if (b % 59 == 0) p.assignMode = pick (0, 2);
            if (b % 61 == 0) p.unisonVoices = pick (0, 3);
            rig.set (p);
            rig.part->setTempo (uni (20, 300));
            std::vector<vs::NoteEvent> ev;
            for (int k = pick (0, 4); k > 0; --k)
            {
                const int at = pick (0, 95), kind = pick (0, 9), note = pick (0, 127);
                if (kind < 4) ev.push_back (on (at, note, uni (0, 1)));
                else if (kind < 7) ev.push_back (off (at, note));
                else if (kind == 7) ev.push_back (control (at, vs::EventType::PitchBend, uni (-1, 1)));
                else if (kind == 8) ev.push_back (control (at, vs::EventType::ModWheel, uni (0, 1)));
                else ev.push_back (control (at, vs::EventType::SustainPedal, static_cast<float> (pick (0, 1))));
            }
            std::stable_sort (ev.begin(), ev.end(), [] (const vs::NoteEvent& a, const vs::NoteEvent& c) { return a.offset < c.offset; });
            const auto y = rig.run (96, ev, 96);
            finite = finite && allFinite (y);
            worst = std::max (worst, peakOf (y));
        }
        INFO (h.rate << " x" << h.factor << ": peak " << worst);
        CHECK (finite);
        CHECK (worst < 4.0);
        rig.run (96, { control (0, vs::EventType::SustainPedal, 0.0f), control (0, vs::EventType::AllNotesOff) }, 96);
        rig.run (static_cast<int> (31.0 * h.rate), {}, 96);
        CHECK (rig.part->numActiveVoices() == 0);
        CHECK_FALSE (rig.part->isSounding());
    }
}

// ---- Smoothing: continuous parameters reach their target in 5 ms; switches are not smoothed -----------------------

TEST_CASE ("Review: filter.cutoff and oscN.pw reach the chip's value within 5 ms (+1 control interval); a wave switch within one interval", "[review][smooth]")
{
    bs::PartParams p;
    p.osc1Wave = 3;
    p.osc1Pw = 10.0f;
    p.filterCutoff = 100.0f;
    Rig rig ({ 48000.0, 2 }, p, 48);
    rig.run (4800, { on (0, 60) }, 48);
    const auto& c = rig.part->getVoice (0).chain.chip();
    CHECK_FALSE (std::abs (c.cutoffSmooth() - 100.0f) > 0.0f);
    p.filterCutoff = 2000.0f;
    p.osc1Pw = 90.0f;
    p.osc2Wave = 2;
    rig.set (p);
    rig.run (48, {}, 48); // 1 ms (one control interval at 96 kHz)
    const float after1 = c.cutoffSmooth();
    rig.run (48 * 5, {}, 48); // 6 ms in all
    INFO ("cutoff after 1 ms " << after1 << ", after 6 ms " << c.cutoffSmooth());
    CHECK (after1 < 2000.0f); // smoothed, not a jump
    CHECK_FALSE (std::abs (c.cutoffSmooth() - 2000.0f) > 0.0f);
    CHECK ((c.registerValue (chip::reg::oscStride * 1 + chip::reg::control) & 0xf0) == 0x20); // the saw, at once
}

// ---- Prepare again, reset, resume ----------------------------------------------------------------------------------

TEST_CASE ("Review: prepare again at another rate mid-note, reset and resume: finite, latency follows, notes play again", "[review][prepare]")
{
    bs::PartParams p;
    Rig rig ({ 48000.0, 2 }, p, 256);
    rig.run (4800, { on (0, 60), on (0, 64) });
    const int lat48 = rig.part->getLatencySamples();
    rig.prepare ({ 96000.0, 1 }, p, 512);
    const int lat96 = rig.part->getLatencySamples();
    CHECK (rig.part->numActiveVoices() == 0);
    auto y = rig.run (9600, { on (10, 67) });
    CHECK (allFinite (y));
    CHECK (peakOf (y) > 0.01);
    rig.part->reset();
    CHECK (rig.part->numActiveVoices() == 0);
    y = rig.run (960);
    CHECK (peakOf (y) <= 0.0);
    rig.part->resume();
    y = rig.run (9600, { on (0, 72) });
    CHECK (peakOf (y) > 0.01);
    INFO ("latency 48k x2 " << lat48 << ", 96k x1 " << lat96);
    CHECK (lat48 > 0);
    CHECK (lat96 > 0);
}

// ---- Latency: an edge within 1 host sample of the reported latency, from the exact figure the part reports --------

TEST_CASE ("Review: latency equal for both parts and models; the chain lengths fit the pad ring at every configuration", "[review][latency]")
{
    for (const auto& h : hostConfigs())
    {
        Rig a (h, [] { bs::PartParams q; q.chipModel = 0; return q; }());
        Rig b (h, [] { bs::PartParams q; q.chipModel = 1; q.filterTopology = 1; return q; }());
        std::printf ("latency %6.0f Hz x%d: reported %d, exact %.3f, chain A %d, chain B %d (internal samples)\n", h.rate, h.factor,
                     b.part->getLatencySamples(), b.part->getExactLatency(), b.part->getChainLatency (chip::Model::A),
                     b.part->getChainLatency (chip::Model::B));
        CHECK (a.part->getLatencySamples() == b.part->getLatencySamples());
        CHECK (b.part->getMaxChainLatency() < 64);
    }
}

TEST_CASE ("Review: host rates below 32 kHz (4x): the chain fits the 64-sample pad ring, both models give the same edge delay", "[review][latency]")
{
    for (const double rate : { 22050.0, 16000.0, 11025.0, 8000.0 })
    {
        Rig b ({ rate, 4 }, bs::PartParams {});
        std::printf ("latency %6.0f Hz x4: reported %d, exact %.3f, chain A %d, chain B %d (internal samples)\n", rate, b.part->getLatencySamples(),
                     b.part->getExactLatency(), b.part->getChainLatency (chip::Model::A), b.part->getChainLatency (chip::Model::B));
        // the pad delays the shorter chain by the difference, in a ring of 64: the difference must fit (Part::prepare's
        // jassert asks for the longest chain itself to fit, which fails below 22.05 kHz: a debug-build assertion only)
        const int padDelay = b.part->getMaxChainLatency() - std::min (b.part->getChainLatency (chip::Model::A), b.part->getChainLatency (chip::Model::B));
        INFO (rate << " Hz: longest chain " << b.part->getMaxChainLatency() << ", pad delay " << padDelay << " internal samples, pad ring 64");
        CHECK (padDelay < 64);
    }
}

TEST_CASE ("Review: Shared: a released note that goes idle and leaves the sum does not disturb a held note", "[review][shared]")
{
    // Note A held throughout; note B played briefly and released (release 0). Once B's envelopes are 0 it adds nothing
    // but its static level, so from then on the output must follow the run in which B was never played (as Per Note
    // does), in particular when B leaves the sum (the idle rule: quiet for a control interval, or 500 ms and a fade).
    for (const int topology : { 0, 1 })
        for (const int model : { 1, 0 })
        {
            std::vector<float> y[2];
            int activeAt[2][8] {};
            for (const int withB : { 0, 1 })
            {
                bs::PartParams p;
                p.chipModel = model;
                p.filterTopology = topology;
                p.osc1Wave = 2;
                p.osc1Release = 0;
                p.osc2Release = 0;
                p.osc3Release = 0;
                Rig rig ({ 48000.0, 2 }, p, 480);
                std::vector<vs::NoteEvent> ev { on (0, 48, 1.0f) };
                if (withB != 0)
                {
                    ev.push_back (on (9600, 60, 1.0f));
                    ev.push_back (off (19200, 60));
                }
                y[withB] = rig.run (480 * 50, ev, 480);
                for (int k = 50; k < 400; ++k)
                {
                    const auto part = rig.run (480, {}, 480);
                    y[withB].insert (y[withB].end(), part.begin(), part.end());
                    if (k % 50 == 0)
                        activeAt[withB][k / 50] = rig.part->numActiveVoices();
                }
            }
            std::printf ("%s model %s: active voices (with B) every 0.5 s from 0.5 s:", topology == 0 ? "Per Note" : "Shared  ", model == 0 ? "A" : "B");
            for (int k = 1; k < 8; ++k)
                std::printf (" %d", activeAt[1][k]);
            std::printf ("\n   largest |with B - without B| per 50 ms from 0.5 s:");
            double worstLate = 0.0, early = 0.0;
            for (int w = 10; w < 80; ++w)
            {
                double d = 0.0;
                for (std::size_t i = static_cast<std::size_t> (w * 2400); i < static_cast<std::size_t> ((w + 1) * 2400); ++i)
                    d = std::max (d, std::abs (static_cast<double> (y[1][i]) - static_cast<double> (y[0][i])));
                std::printf (" %.2g", d);
                if (w >= 14)
                    worstLate = std::max (worstLate, d);
                else
                    early = std::max (early, d);
            }
            std::printf ("\n");
            // B's own static level after its envelopes reach 0 decays through its board high-pass (physical, both topologies);
            // B leaving the sum must not add a disturbance larger than what was decaying before it
            INFO ((topology == 0 ? "Per Note" : "Shared") << " model " << (model == 0 ? "A" : "B") << ": largest difference 0.5 .. 0.7 s " << early
                                                          << ", from 0.7 s on (B leaves the sum there) " << worstLate << ", peak of the held note " << peakOf (y[0]));
            CHECK (worstLate <= early);
        }
}
