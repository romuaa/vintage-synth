// part_bench (docs/architecture.md 8.21, the CPU gates of S4): the share of one core that B-Side's part takes, best of
// 3 runs of 5 s of audio each, non-LTO (bs_test_core). A benchmark, not a test (no ctest entry, no sanitizer run).
//
// The gate patch (8.5, 8.21; the same patch as chip_bench's chain cases): oscillator 1 pulse with a width write every
// tick (a ping-pong pulse lane), oscillator 2 sawtooth an octave up, oscillator 3 the triangle+saw combined waveform a
// fifth up; envelopes attack 0, decay 9, sustain 10, release 9; oscillators 1 and 2 through the filter, low-pass,
// cutoff 1024, resonance 8, volume 15; every voice held (16 notes, D3 .. F4: around A3, see lowNote). Host blocks of 512
// samples.
//
//   part16B / part16A     16 note voices, one part, through Part::render, 48 kHz host (2x)      gate 5 % / 11 %
//   dual32B / dual32A     32 note voices (two parts, Dual), 48 kHz host                          gate 10 % / 22 %
//   proc32B / proc32A     32 note voices with chorus, delay and reverb on, through processBlock    gate 15 % / 27 %
//   hi16B / hi16A         16 note voices at a 192 kHz host (1x)                                   gate 15 % / 30 %
//   idle16B / idle16A     16 idle voices, one part (played, released, every voice idle)          gate 0.1 % / 0.1 %
// Also reported (no gate): switch, the cost of one structural switch per voice (microseconds; 8.21.1: at most 50); noteStart, the cost of a note start after a long idle (microseconds); vibrato16B, part16B with the mod wheel
// up (a frequency write per voice and control point); variation16A, part16A with chip.variation moving; memory per part.
// Arguments: case names to run only those (default: all).

#include "../../support/Env.h"
#include "bside/PluginProcessor.h"
#include "bside/engine/Part.h"
#include "bside/program/TableReader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
constexpr int kBlock = 512;
constexpr double kSeconds = 5.0;
constexpr int kRepeats = 3;

/** The lowest of the 16 held notes: D3 by default, so the 16 notes (D3 .. F4) lie around A3, the pitch of chip_bench's
    chain cases (oscillator 1 at 220 Hz), whose per-chip figures the gates were derived from. PART_BENCH_LOW_NOTE moves
    them (57: A3 .. C5, about a third more edges per second). */
int lowNote()
{
    static const int n = [] {
        const auto e = testenv::get ("PART_BENCH_LOW_NOTE");
        return e ? std::atoi (e->c_str()) : 50;
    }();
    return n;
}

const char* kGateTable = "bside-table 1\n[pulse1]\n    set 1024\n    pingpong 1024 3072 37\n";

vs::bside::program::Program gateProgram()
{
    const auto r = vs::bside::program::readTable (kGateTable);
    if (! r.ok)
        std::printf ("gate table rejected: %s\n", r.message().c_str());
    return r.program;
}

bs::PartParams gateParams (int model)
{
    bs::PartParams p;
    p.chipModel = model; // 0 Model A, 1 Model B
    p.osc1Wave = 3;      // Pulse
    p.osc2Wave = 2;      // Saw
    p.osc2Coarse = 12;
    p.osc3Wave = 5;      // Tri+Saw
    p.osc3Coarse = 7;
    p.osc1Attack = p.osc2Attack = p.osc3Attack = 0;
    p.osc1Decay = p.osc2Decay = p.osc3Decay = 9;
    p.osc1Sustain = p.osc2Sustain = p.osc3Sustain = 10;
    p.osc1Release = p.osc2Release = p.osc3Release = 9;
    p.osc1Filter = p.osc2Filter = 1;
    p.osc3Filter = 0;
    p.filterMode = 1; // Low
    p.filterCutoff = 1024.0f;
    p.filterResonance = 8;
    p.chipVolume = 15;
    return p;
}

std::vector<vs::NoteEvent> chord (int count)
{
    std::vector<vs::NoteEvent> e;
    for (int i = 0; i < count; ++i)
        e.push_back ({ 0, vs::EventType::NoteOn, static_cast<std::uint8_t> (lowNote() + i), 1.0f });
    return e;
}

volatile float gSink = 0.0f;

struct Timing
{
    double runs[kRepeats] {};
    double best = 1e300;
};

void print (const char* name, const Timing& t, const char* what)
{
    std::printf ("%-12s best %7.3f %%   runs %7.3f %7.3f %7.3f   (%s)\n", name, t.best, t.runs[0], t.runs[1], t.runs[2], what);
}

/** % of one core: `parts` parts with `notes` held notes each (or all idle), `seconds` of audio per run. */
Timing partCase (int model, double rate, int factor, int parts, int notes, bool idle, float wheel = 0.0f, bool moveVariation = false)
{
    Timing t;
    const auto prog = gateProgram();
    for (int run = 0; run < kRepeats; ++run)
    {
        std::vector<std::unique_ptr<bs::Part>> ps;
        for (int i = 0; i < parts; ++i)
        {
            ps.push_back (std::make_unique<bs::Part>());
            auto p = gateParams (model);
            ps.back()->setParams (p);
            ps.back()->setProgram (&prog);
            ps.back()->prepare (rate, kBlock, factor, p, 0);
        }
        std::vector<float> out (kBlock);
        auto events = chord (notes);
        if (wheel > 0.0f)
            events.push_back ({ 0, vs::EventType::ModWheel, 0, wheel });
        for (auto& p : ps)
            p->render (out.data(), kBlock, events);
        if (idle)
        {
            std::vector<vs::NoteEvent> offs;
            for (int i = 0; i < notes; ++i)
                offs.push_back ({ 0, vs::EventType::NoteOff, static_cast<std::uint8_t> (lowNote() + i), 0.0f });
            for (auto& p : ps)
                p->render (out.data(), kBlock, offs);
            for (int b = 0; b < static_cast<int> (10.0 * rate / kBlock) && ps[0]->numActiveVoices() > 0; ++b)
                for (auto& p : ps)
                    p->render (out.data(), kBlock, {});
            if (ps[0]->numActiveVoices() > 0)
                std::printf ("idle case: %d voices still active\n", ps[0]->numActiveVoices());
        }
        else
            for (int b = 0; b < static_cast<int> (0.5 * rate / kBlock); ++b) // settle (the decay to sustain)
                for (auto& p : ps)
                    p->render (out.data(), kBlock, {});
        const int blocks = static_cast<int> (kSeconds * rate / kBlock);
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
        {
            if (moveVariation)
            {
                auto p = gateParams (model);
                p.chipVariation = 50.0f + 40.0f * static_cast<float> ((b / 8) % 2);
                for (auto& q : ps)
                    q->setParams (p);
            }
            for (auto& p : ps)
                p->render (out.data(), kBlock, {});
            gSink = gSink + out[0];
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double audio = static_cast<double> (blocks) * kBlock / rate;
        t.runs[run] = 100.0 * std::chrono::duration<double> (t1 - t0).count() / audio;
        t.best = std::min (t.best, t.runs[run]);
    }
    return t;
}

void setParam (bs::BSideProcessor& proc, const juce::String& id, float plain)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
    if (p == nullptr)
    {
        std::printf ("missing parameter %s\n", id.toRawUTF8());
        return;
    }
    p->setValueNotifyingHost (p->convertTo0to1 (plain));
}

Timing processorCase (int model)
{
    Timing t;
    const auto prog = gateProgram();
    for (int run = 0; run < kRepeats; ++run)
    {
        auto proc = std::make_unique<bs::BSideProcessor>();
        const auto p = gateParams (model);
        for (const char* prefix : { "lower.", "upper." })
        {
            const juce::String pre (prefix);
            setParam (*proc, pre + "chip.model", static_cast<float> (p.chipModel));
            setParam (*proc, pre + "osc1.wave", 3.0f);
            setParam (*proc, pre + "osc2.wave", 2.0f);
            setParam (*proc, pre + "osc2.coarse", 12.0f);
            setParam (*proc, pre + "osc3.wave", 5.0f);
            setParam (*proc, pre + "osc3.coarse", 7.0f);
            for (const char* n : { "osc1.", "osc2.", "osc3." })
            {
                setParam (*proc, pre + n + "attack", 0.0f);
                setParam (*proc, pre + n + "decay", 9.0f);
                setParam (*proc, pre + n + "sustain", 10.0f);
                setParam (*proc, pre + n + "release", 9.0f);
            }
            setParam (*proc, pre + "osc3.filter", 0.0f);
            setParam (*proc, pre + "filter.resonance", 8.0f);
        }
        setParam (*proc, "key.mode", 1.0f); // Dual
        setParam (*proc, "fx.chorus.mode", 2.0f);
        setParam (*proc, "fx.delay.level", 0.5f);
        setParam (*proc, "fx.reverb.level", 0.5f);
        proc->tableHandover (0).publish (prog);
        proc->tableHandover (1).publish (prog);
        proc->setNonRealtime (false);
        proc->setPlayConfigDetails (0, 2, 48000.0, kBlock);
        proc->prepareToPlay (48000.0, kBlock);
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer notes;
        for (int i = 0; i < 16; ++i)
            notes.addEvent (juce::MidiMessage::noteOn (1, lowNote() + i, static_cast<juce::uint8> (127)), 0);
        buffer.clear();
        proc->processBlock (buffer, notes);
        juce::MidiBuffer none;
        for (int b = 0; b < static_cast<int> (0.5 * 48000.0 / kBlock); ++b)
            proc->processBlock (buffer, none);
        const int blocks = static_cast<int> (kSeconds * 48000.0 / kBlock);
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
        {
            proc->processBlock (buffer, none);
            gSink = gSink + buffer.getSample (0, 0);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double audio = static_cast<double> (blocks) * kBlock / 48000.0;
        t.runs[run] = 100.0 * std::chrono::duration<double> (t1 - t0).count() / audio;
        t.best = std::min (t.best, t.runs[run]);
        const int sounding = proc->getLowerPart().numActiveVoices() + proc->getUpperPart().numActiveVoices();
        if (sounding != 32)
            std::printf ("processor case: %d voices sounding, not 32\n", sounding);
    }
    return t;
}

/** Microseconds per voice of a structural switch itself (every voice reset into the new configuration from the
    prototypes: ChipVoice copy, reconfigure when the clock, resolution or volume DC differ, filter, engine): the block
    that makes it minus a plain block, the part silent (with voices sounding the switch only adds its 3 ms fade first).
    `what`: 0 chip.model, 1 chip.clock, 2 chip.volume_dc. Best and worst of 20 switches, divided by 16. */
std::pair<double, double> switchMicros (int model, int what)
{
    double best = 1e300, worst = 0.0;
    auto part = std::make_unique<bs::Part>();
    auto p = gateParams (model);
    int clock = 0;
    part->setParams (p);
    part->prepare (48000.0, kBlock, 2, p, 0);
    std::vector<float> out (kBlock);
    for (int b = 0; b < 20; ++b)
        part->render (out.data(), kBlock, {});
    for (int k = 0; k < 20; ++k)
    {
        auto t0 = std::chrono::steady_clock::now();
        part->render (out.data(), kBlock, {});
        auto t1 = std::chrono::steady_clock::now();
        const double plain = std::chrono::duration<double, std::micro> (t1 - t0).count();
        if (what == 0)
            p.chipModel = 1 - p.chipModel;
        else if (what == 1)
            clock = 1 - clock;
        else
            p.chipVolumeDc = 1 - p.chipVolumeDc;
        part->setParams (p);
        part->setChipClock (clock);
        const int before = part->switchesDone();
        t0 = std::chrono::steady_clock::now();
        part->render (out.data(), kBlock, {});
        t1 = std::chrono::steady_clock::now();
        if (part->switchesDone() != before + 1)
            std::printf ("switch did not complete in one block\n");
        const double with = std::chrono::duration<double, std::micro> (t1 - t0).count();
        best = std::min (best, (with - plain) / 16.0);
        worst = std::max (worst, (with - plain) / 16.0);
    }
    return { best, worst };
}

/** Microseconds of a note start on a voice that was idle for `idleSeconds` (its chip time caught up first; the
    catch-up runs once per block, so this is the first block's extra cost over a plain block). */
double noteStartMicros (double idleSeconds)
{
    double best = 1e300;
    const auto prog = gateProgram();
    for (int run = 0; run < kRepeats; ++run)
    {
        auto part = std::make_unique<bs::Part>();
        auto p = gateParams (1);
        part->setParams (p);
        part->setProgram (&prog);
        part->prepare (48000.0, kBlock, 2, p, 0);
        std::vector<float> out (kBlock);
        for (int b = 0; b < static_cast<int> (idleSeconds * 48000.0 / kBlock); ++b)
            part->render (out.data(), kBlock, {});
        auto t0 = std::chrono::steady_clock::now();
        part->render (out.data(), kBlock, {});
        auto t1 = std::chrono::steady_clock::now();
        const double plain = std::chrono::duration<double, std::micro> (t1 - t0).count();
        t0 = std::chrono::steady_clock::now();
        part->render (out.data(), kBlock, chord (1));
        t1 = std::chrono::steady_clock::now();
        best = std::min (best, std::chrono::duration<double, std::micro> (t1 - t0).count() - plain);
    }
    return best;
}

bool wanted (int argc, char** argv, const char* name)
{
    if (argc <= 1)
        return true;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp (argv[i], name) == 0)
            return true;
    return false;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    std::printf ("part_bench: share of one core, best of %d runs of %.0f s audio, blocks of %d\n", kRepeats, kSeconds, kBlock);
    if (wanted (argc, argv, "part16B"))
        print ("part16B", partCase (1, 48000.0, 2, 1, 16, false), "16 voices, one part, Model B, 48 kHz (2x); gate 5 %");
    if (wanted (argc, argv, "part16A"))
        print ("part16A", partCase (0, 48000.0, 2, 1, 16, false), "16 voices, one part, Model A, 48 kHz (2x); gate 11 %");
    if (wanted (argc, argv, "dual32B"))
        print ("dual32B", partCase (1, 48000.0, 2, 2, 16, false), "32 voices, two parts, Model B; gate 10 %");
    if (wanted (argc, argv, "dual32A"))
        print ("dual32A", partCase (0, 48000.0, 2, 2, 16, false), "32 voices, two parts, Model A; gate 22 %");
    if (wanted (argc, argv, "proc32B"))
        print ("proc32B", processorCase (1), "32 voices, Dual, chorus + delay + reverb, processBlock, Model B; gate 15 %");
    if (wanted (argc, argv, "proc32A"))
        print ("proc32A", processorCase (0), "32 voices, Dual, chorus + delay + reverb, processBlock, Model A; gate 27 %");
    if (wanted (argc, argv, "hi16B"))
        print ("hi16B", partCase (1, 192000.0, 1, 1, 16, false), "16 voices, 192 kHz host (1x), Model B; gate 15 %");
    if (wanted (argc, argv, "hi16A"))
        print ("hi16A", partCase (0, 192000.0, 1, 1, 16, false), "16 voices, 192 kHz host (1x), Model A; gate 30 %");
    if (wanted (argc, argv, "idle16B"))
        print ("idle16B", partCase (1, 48000.0, 2, 1, 16, true), "16 idle voices, one part, Model B; gate 0.1 %");
    if (wanted (argc, argv, "idle16A"))
        print ("idle16A", partCase (0, 48000.0, 2, 1, 16, true), "16 idle voices, one part, Model A; gate 0.1 %");
    if (wanted (argc, argv, "vibrato16B"))
        print ("vibrato16B", partCase (1, 48000.0, 2, 1, 16, false, 1.0f), "part16B with the mod wheel up (vibrato writes)");
    if (wanted (argc, argv, "variation16A"))
        print ("variation16A", partCase (0, 48000.0, 2, 1, 16, false, 0.0f, true), "part16A with chip.variation moving all the time");
    if (wanted (argc, argv, "switch"))
        for (const int what : { 0, 1, 2 })
        {
            const auto r = switchMicros (1, what);
            std::printf ("%-12s best %7.2f worst %7.2f us per voice   (a %s switch, 16 voices reset; 8.21.1: at most 50)\n", "switch", r.first, r.second,
                         what == 0 ? "chip.model" : (what == 1 ? "chip.clock" : "chip.volume_dc"));
        }
    if (wanted (argc, argv, "noteStart"))
        for (const double s : { 1.0, 60.0 })
            std::printf ("%-12s %7.2f us   (a note start after %.0f s idle, over a plain block)\n", "noteStart", noteStartMicros (s), s);
    if (wanted (argc, argv, "catchup"))
    {
        // the idle voices' cost: Chip::skipSamples over one block (1024 internal samples) for a chip in the state an
        // idle gate-patch voice has, and with every oscillator's frequency 0 (no accumulator and noise events)
        for (const bool frozen : { false, true })
        {
            vs::bside::chip::Chip c;
            vs::bside::chip::Config cfg;
            cfg.internalRate = 96000.0;
            c.prepare (cfg);
            for (int n = 0; n < 3; ++n)
            {
                vs::bside::chip::OscInput in;
                in.frequency = frozen ? 0.0 : 3740.0 * (1.0 + 0.5 * n);
                in.control = static_cast<std::uint8_t> (n == 2 ? 0x30 : (n == 1 ? 0x20 : 0x40));
                in.attackDecay = 0x09;
                in.sustainRelease = 0xa9;
                c.setOscillator (n, in);
            }
            c.advanceCycles (2000000);
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < 20000; ++b)
                c.skipSamples (1024);
            const auto t1 = std::chrono::steady_clock::now();
            std::printf ("%-12s %7.3f us per 1024 samples (%s)\n", "catchup", std::chrono::duration<double, std::micro> (t1 - t0).count() / 20000.0,
                         frozen ? "frequencies 0" : "the gate patch released");
        }
    }
    if (wanted (argc, argv, "memory"))
        std::printf ("%-12s %zu bytes per part (object, 16 voices, 2 prototypes)\n", "memory", bs::Part::memoryBytes());
    return 0;
}
