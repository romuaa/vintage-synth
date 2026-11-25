// The factory bank of Exitium B-Side (docs/architecture.md 8.21.2, docs/bside-factory-sounds.md): sixteen programs, A1 to B8.
//
//   "Bank: ..."          the bank as a whole: files, names, categories, the tables through the real reader
//   "Bank level rule"    ONE test case over the 16 programs: loads, sounds, finite, chord headroom, phrase within 2 dB of its
//                        group's median, the limiter's knee not reached, a state round-trip, a program applied while notes sound
//   "Bank A2" ... "B8"   one test per program: what its row of the specification says, shown by a measurement (the chip's
//                        registers read back while it plays, or the audio against the same program without the feature)
//   "[apply]"            a program applied while notes sound fades the part first (Part.h "PROGRAM APPLY"); a waveform changed
//                        by hand on a held note steps the output (a property)
//
// What these tests do is measure and compare; none of them says how a program sounds. VS_BANK_VERBOSE=1 prints the measured
// series (they are the figures of docs/bside-factory-sounds.md). The phrases and the measurements are BankSound.h's, shared
// with the hidden audition render (BankRender.cpp).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "BankSound.h"
#include "bside/program/TableReader.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace bsbank;

#define SAY(...)                      \
    do                                \
    {                                 \
        if (verbose())                \
            std::printf (__VA_ARGS__); \
    } while (false)

namespace
{
bool verbose()
{
    static const bool v = juce::SystemStats::getEnvironmentVariable ("VS_BANK_VERBOSE", {}).isNotEmpty();
    return v;
}

/** Level rule (8.21.2, 6b.4): the phrase within this many dB of its group's median; no chord more than this above Init's. */
constexpr double kGroupToleranceDb = 2.0;
constexpr double kChordHeadroomDb = 3.0;
/** Where the output limiter's knee starts at its defaults (ceiling -1 dBFS, knee 3 dB below it). */
constexpr double kLimiterKneeDbfs = -4.0;

double medianOf (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    return v.size() % 2 == 1 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}

struct Expected
{
    const char* slot;
    const char* name;
    const char* category;
};
/** The table of 8.21.2: slot, our own name, category. */
constexpr Expected kBank[] {
    { "A1", "Init", "Init" },
    { "A2", "Moving Pulse", "Lead" },
    { "A3", "Synced Climb", "Lead" },
    { "A4", "Glass Ring", "Lead" },
    { "A5", "Closing Door", "Bass" },
    { "A6", "Blip Bass", "Bass" },
    { "A7", "Major Steps", "Arpeggio" },
    { "A8", "Minor Steps", "Arpeggio" },
    { "B1", "Short Kick", "Percussion" },
    { "B2", "Dry Snare", "Percussion" },
    { "B3", "Tick Hat", "Percussion" },
    { "B4", "Slow Saw Pad", "Pad" },
    { "B5", "Shared Chord Pad", "Pad" },
    { "B6", "Band Sweep", "Sound FX" },
    { "B7", "Pulse Over Pad", "Dual" },
    { "B8", "Bass And Lead", "Split" },
};

/** Bitwise float equality, with -0 equal to 0 (GCC rejects float == under -Wfloat-equal). */
bool sameFloat (float a, float b)
{
    a += 0.0f;
    b += 0.0f;
    return std::memcmp (&a, &b, sizeof (float)) == 0;
}

float valueOf (const vs::presets::Program& p, const vs::presets::Schema& schema, const char* id)
{
    const int i = schema.indexOf (id);
    REQUIRE (i >= 0);
    return p.values[static_cast<size_t> (i)];
}

/** A program's table lines for a part (a program file without tables has none). */
std::vector<std::string> tableOf (const vs::presets::Program& p, int part)
{
    return static_cast<size_t> (part) < p.data.size() ? p.data[static_cast<size_t> (part)] : std::vector<std::string> {};
}

std::string joinLines (const std::vector<std::string>& lines)
{
    std::string text;
    for (const auto& l : lines)
        text += l + "\n";
    return text;
}

/** Hz of a MIDI note (A4 = 440, equal temperament: the engine's mapping). */
double hzOfNote (double note) { return 440.0 * std::exp2 ((note - 69.0) / 12.0); }
double semitonesBetween (double f, double ref) { return 12.0 * std::log2 (f / ref); }
double msOfTicks (std::uint32_t period, double ticks) { return 1000.0 * ticks * static_cast<double> (period) / 985248.0; }

/** Oscillator n (1 = oscillator 2, 2 = oscillator 3) doubles oscillator 1 (the drums, raised in their content to reach their level,
    8.21.2 after the drafting): its seven registers and its envelope counter are oscillator 1's, so the two sum in phase. */
bool doublesOscillator1 (const RegisterSample& s, int n = 1)
{
    for (int r = 0; r < 7; ++r)
        if (osc (s, n, r) != osc (s, 0, r))
            return false;
    return s.env[n] == s.env[0];
}

/** The table lines of a program's part with every occurrence of `token` removed (the same program without a feature). */
std::vector<std::string> withoutToken (std::vector<std::string> lines, const std::string& token)
{
    for (auto& l : lines)
        for (size_t at0 = l.find (token); at0 != std::string::npos; at0 = l.find (token))
            l.erase (at0, token.size());
    return lines;
}

/** A rig on `slot` (limiter off) whose Lower table is replaced by `lines`, through the editor's entry point. */
std::unique_ptr<Rig> rigWithTable (int slot, const std::vector<std::string>& lines)
{
    auto rig = std::make_unique<Rig> (slot, false);
    const auto error = rig->presets().setTable (0, lines);
    REQUIRE (error.isEmpty());
    return rig;
}

/** The phrase-window of a render: from `a` to `b` seconds (a Performance only carries the window). */
Performance windowOf (double a, double b)
{
    Performance w;
    w.firstOn = at (a);
    w.firstOff = w.lastOff = at (b);
    return w;
}

/** The largest difference between two renders' octave-band energies, in dB (the spectrum summary of BankSound.h), over the bands
    that hold at least -50 dB of either (a band of rounding noise says nothing about the sound). */
double bandDistanceDb (const Measured& a, const Measured& b)
{
    double worst = 0.0;
    for (int i = 0; i < kBands; ++i)
        if (std::max (a.bandDb[i], b.bandDb[i]) > -50.0)
            worst = std::max (worst, std::abs (a.bandDb[i] - b.bandDb[i]));
    return worst;
}

/** The share of the spectrum summary's energy at or above `hz` (a band edge). */
double shareAbove (const Measured& m, double hz)
{
    double above = 0.0, all = 0.0;
    for (int i = 0; i < kBands; ++i)
    {
        const double e = std::pow (10.0, m.bandDb[i] / 10.0);
        all += e;
        if (kBandEdges[i] >= hz)
            above += e;
    }
    return all > 0.0 ? above / all : 0.0;
}

double rmsDbOf (const Stereo& s, int from, int to)
{
    double acc = 0.0;
    for (int i = from; i < to; ++i)
        acc += 0.5 * (static_cast<double> (s.l[static_cast<size_t> (i)]) * s.l[static_cast<size_t> (i)] + static_cast<double> (s.r[static_cast<size_t> (i)]) * s.r[static_cast<size_t> (i)]);
    return 10.0 * std::log10 (std::max (acc / std::max (1, to - from), 1.0e-20));
}

/** The RMS of both channels over [from, to) with each channel's mean over the window removed. */
double acRmsDbOf (const Stereo& s, int from, int to)
{
    double acc = 0.0;
    for (const auto* ch : { &s.l, &s.r })
    {
        double mean = 0.0;
        for (int i = from; i < to; ++i)
            mean += static_cast<double> ((*ch)[static_cast<size_t> (i)]);
        mean /= std::max (1, to - from);
        for (int i = from; i < to; ++i)
        {
            const double d = static_cast<double> ((*ch)[static_cast<size_t> (i)]) - mean;
            acc += 0.5 * d * d;
        }
    }
    return 10.0 * std::log10 (std::max (acc / std::max (1, to - from), 1.0e-20));
}

/** A session saved from `slot` and restored into a new processor: every parameter and both tables the same, and saving again
    gives the same state. */
void checkStateRoundTrip (int slot, const vs::presets::Program& prog)
{
    Rig a (slot, false);
    juce::MemoryBlock saved;
    a.proc.getStateInformation (saved);
    REQUIRE (saved.getSize() > 0);
    Rig b (-1, false);
    b.proc.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    int different = 0;
    for (int i = 0; i < a.schema().numParams(); ++i)
    {
        const auto& spec = a.schema().spec (i);
        const float x = a.get (spec.id), y = b.get (spec.id);
        if (std::abs (x - y) > 2.0e-6f * (spec.hi - spec.lo))
        {
            ++different;
            INFO ("parameter " << spec.id << ": " << x << " saved, " << y << " restored");
        }
    }
    CHECK (different == 0);
    for (int part = 0; part < 2; ++part)
    {
        const auto want = vs::bside::program::readTable (joinLines (tableOf (prog, part)));
        auto& handover = b.proc.tableHandover (part);
        const auto* tables = handover.beginAcquire();
        REQUIRE (tables != nullptr);
        handover.commit();
        if (tableOf (prog, part).empty())
            CHECK (handover.inUse() == vs::bside::program::emptyProgram());
        else
        {
            REQUIRE (want.ok);
            CHECK (handover.inUse() == want.program);
        }
    }
    juce::MemoryBlock again;
    b.proc.getStateInformation (again);
    CHECK (again == saved);
}

/** A smooth sustained chord (triangles, a low cutoff) on Init, held; then `slot` is applied at 0.6 s with the keys still down
    (Vintage's "Applying a program while notes sound releases them without a click", PresetManagerTests.cpp). */
struct AppliedWhileSounding
{
    double before = 0.0, after = 0.0, soundingPeak = 0.0, endPeak = 0.0;
};
AppliedWhileSounding applyWhileSounding (int slot)
{
    Rig rig (0, false);
    rig.set ("lower.osc1.wave", 1.0f);
    rig.set ("lower.filter.cutoff", 300.0f);
    const int sw = at (0.6);
    std::vector<Midi> midi;
    for (int n : { 48, 55, 64 })
        midi.push_back (noteOn (at (0.05), n));
    auto out = rig.render (at (1.0), midi, kBlock, { { sw, [&] { REQUIRE (rig.presets().selectProgram (slot)); } } });
    // the released voices follow the new program's envelope (its release), so the render goes on for the tail it reports
    const auto rest = rig.render (at (rig.proc.getTailLengthSeconds() + 0.5), {}, kBlock);
    out.l.insert (out.l.end(), rest.l.begin(), rest.l.end());
    out.r.insert (out.r.end(), rest.r.begin(), rest.r.end());
    const size_t s0 = static_cast<size_t> (sw);
    AppliedWhileSounding r;
    r.before = maxStep (out.l, s0 - 9600, s0);
    r.after = maxStep (out.l, s0, s0 + 4800);
    r.soundingPeak = peakAbs (out.l, s0 - 4800, s0);
    r.endPeak = std::max (peakAbs (out.l, out.l.size() - 4800, out.l.size()), peakAbs (out.r, out.r.size() - 4800, out.r.size()));
    return r;
}
} // namespace

// ---- the bank ----------------------------------------------------------------------------------------------------

TEST_CASE ("Bank: 16 programs A1 to B8 in slot order, own names, known categories, tables through the real reader", "[bside][bank]")
{
    Rig rig (-1);
    juce::StringArray errors;
    const auto bank = bs::makeBSideFactoryBank (rig.schema(), &errors);
    INFO (errors.joinIntoString ("; "));
    CHECK (errors.isEmpty());
    REQUIRE (bank.programs.size() == static_cast<size_t> (bs::kPresetBankSize));
    CHECK (bank.name == "Factory");

    const auto init = rig.schema().defaults();
    std::set<juce::String> names;
    for (int i = 0; i < bs::kPresetBankSize; ++i)
    {
        const auto& p = bank.programs[static_cast<size_t> (i)];
        INFO (rig.presets().positionName (i) << " " << p.meta.name << " [" << p.meta.category << "]");
        if (i >= kBankSize)
        {
            // the rest of the bank is Init, as before the bank was designed
            CHECK (p.meta.name == "Init");
            CHECK (p.values == init);
            CHECK (tableOf (p, 0).empty());
            CHECK (tableOf (p, 1).empty());
            continue;
        }
        const auto& e = kBank[i];
        CHECK (rig.presets().positionName (i) == e.slot);
        CHECK (p.meta.name == e.name);
        CHECK (p.meta.category == e.category);
        CHECK (p.meta.author == "Exitium");
        CHECK (names.insert (p.meta.name).second);
        const auto info = categoryInfo (p.meta.category);
        CHECK (info.known);
        CHECK ((info.group == Group::Reference) == (i == 0));
        // mix.level is the only trim, inside its range; the shell's arpeggiator is off in every program (A7 and A8 use the table)
        for (const char* part : { "lower.", "upper." })
        {
            const float level = valueOf (p, rig.schema(), (std::string (part) + "mix.level").c_str());
            CHECK (level >= -24.0f);
            CHECK (level <= 6.0f);
        }
        CHECK (valueOf (p, rig.schema(), "arp.mode") == Catch::Approx (rig.schema().defaults()[static_cast<size_t> (rig.schema().indexOf ("arp.mode"))]));
        if (i == 0)
        {
            CHECK (tableOf (p, 0).empty());
            CHECK (tableOf (p, 1).empty());
            for (size_t k = 0; k < p.values.size(); ++k)
                CHECK (sameFloat (p.values[k], init[k]));
            continue;
        }
        // its own sound: different from Init in at least three settings (the drums change little besides their table) and from every
        // program before it
        int different = 0;
        for (size_t k = 0; k < p.values.size(); ++k)
            different += sameFloat (p.values[k], init[k]) ? 0 : 1;
        CHECK (different >= 3);
        CHECK ((! tableOf (p, 0).empty() || different >= 5)); // a program with no table moves at least five settings
        for (int j = 1; j < i; ++j)
            CHECK ((p.values != bank.programs[static_cast<size_t> (j)].values || tableOf (p, 0) != tableOf (bank.programs[static_cast<size_t> (j)], 0)));
        // the tables pass the real reader
        for (int part = 0; part < 2; ++part)
            if (! tableOf (p, part).empty())
            {
                const auto r = vs::bside::program::readTable (joinLines (tableOf (p, part)));
                INFO ("part " << part << ": " << r.message());
                CHECK (r.ok);
            }
        // key modes: Dual (B7) and Split (B8) only; split at G3 (55)
        const int mode = static_cast<int> (valueOf (p, rig.schema(), "key.mode"));
        CHECK (mode == (std::string (e.slot) == "B7" ? 1 : std::string (e.slot) == "B8" ? 2 : 0));
        if (mode == 2)
            CHECK (valueOf (p, rig.schema(), "key.split_point") == Catch::Approx (55.0f));
    }
    // A7 and A8 carry their arpeggio in the wave lane, not in the shell's arpeggiator
    for (const int slot : { 6, 7 })
    {
        const auto text = joinLines (tableOf (bank.programs[static_cast<size_t> (slot)], 0));
        CHECK (text.find ("[wave1]") != std::string::npos);
        CHECK (text.find ("pitch +") != std::string::npos);
    }
}

// ---- the level rule ----------------------------------------------------------------------------------------------

TEST_CASE ("Bank level rule: every program loads, sounds, is finite, keeps its chord headroom, sits within 2 dB of its group, stays below the limiter's knee, round-trips its state, and releases when applied over held notes",
           "[bside][bank][level]")
{
    // 8.21.2 after the drafting: sustained (the arpeggios too) and bass programs by their phrase RMS against their group's median;
    // percussion by its phrase PEAK against the median phrase peak of the sustained group, and its headroom with two keys down.
    const auto& bank = factoryBank();
    REQUIRE (bank.programs.size() >= static_cast<size_t> (kBankSize));
    std::vector<double> rms (static_cast<size_t> (kBankSize)), peak (static_cast<size_t> (kBankSize));
    std::map<Group, std::vector<double>> byGroup;
    std::vector<double> sustainedPeaks;
    double initChordRms = 0.0, initChordPeak = 0.0;
    for (int slot = 0; slot < kBankSize; ++slot)
    {
        const auto& prog = bank.programs[static_cast<size_t> (slot)];
        const auto info = categoryInfo (prog.meta.category);
        const auto perf = phraseFor (prog);
        Rig rig (slot, false); // levels with the limiter off: the rule compares programs, not the limiter
        INFO (rig.presets().positionName (slot) << " " << prog.meta.name << " [" << prog.meta.category << ", " << groupName (info.group) << "]");

        // loads: the program is the one selected (a file the reader rejected would leave an Init in its slot), sounding on its
        // first key without touching a control
        CHECK (rig.presets().currentIndex() == slot);
        CHECK (prog.meta.name == kBank[slot].name);
        CHECK (info.known);
        const double tail = rig.proc.getTailLengthSeconds();
        CHECK (tail <= 30.0);
        const auto out = rig.play (perf, tail + 0.75);
        const auto m = measure (out, perf);
        REQUIRE (m.finite);
        INFO ("phrase: RMS " << m.rmsDb << " dB, peak " << m.peakDb << " dBFS, last 100 ms " << m.endPeakDb << " dBFS");
        CHECK (m.peakDb > -50.0);    // sounds
        CHECK (m.rmsDb > -60.0);
        CHECK (m.peakDb < 0.0);      // below full scale at the default master volume
        CHECK (m.endPeakDb < -80.0); // nothing stuck: silent 0.75 s after the reported tail has passed
        rms[static_cast<size_t> (slot)] = m.rmsDb;
        peak[static_cast<size_t> (slot)] = m.peakDb;
        // a group's median is taken over the programs with offset 0 (8.21.2 after the owner's audition)
        if (info.group != Group::Reference && ! hasLevelOffset (slot))
            byGroup[info.group].push_back (judgedByPeak (info.group) ? m.peakDb : m.rmsDb);
        if (info.group == Group::Sustained && ! hasLevelOffset (slot))
            sustainedPeaks.push_back (m.peakDb);

        // chord headroom: eight notes at full velocity; for percussion two keys, below full scale only
        Rig chordRig (slot, false);
        auto chord = headroomChord (info.group);
        const auto chordOut = chordRig.play (chord, 0.0);
        chord.firstOn = at (0.55); // the steady part
        const auto cm = measure (chordOut, chord);
        INFO ((judgedByPeak (info.group) ? "two keys" : "eight-note chord") << ": RMS " << cm.rmsDb << " dB, peak " << cm.peakDb << " dBFS");
        CHECK (cm.peakDb < 0.0);
        if (slot == 0)
        {
            initChordRms = cm.rmsDb;
            initChordPeak = cm.peakDb;
        }
        else if (! judgedByPeak (info.group))
        {
            CHECK (cm.rmsDb <= initChordRms + kChordHeadroomDb);
            CHECK (cm.peakDb <= initChordPeak + kChordHeadroomDb);
        }

        // the limiter on at its defaults, as a new instance plays: the phrase does not reach the knee
        Rig onRig (slot, true);
        const auto mOn = measure (onRig.play (perf, 0.0), perf);
        INFO ("limiter on: phrase peak " << mOn.peakDb << " dBFS");
        CHECK (mOn.peakDb < kLimiterKneeDbfs);

        checkStateRoundTrip (slot, prog);

        // a program applied while notes sound: the held voices fall silent (the fade before the new program and Vintage's step
        // rule: the "[apply]" tests below)
        const auto applied = applyWhileSounding (slot);
        INFO ("applied at 0.6 s with the keys down: peak just before " << applied.soundingPeak << ", peak in the last 100 ms " << applied.endPeak);
        REQUIRE (applied.soundingPeak > 0.01);
        CHECK (applied.endPeak < 1.0e-3);
    }

    // the targets: each RMS group's median RMS; for percussion the sustained group's median PEAK
    std::map<Group, double> target;
    for (const auto& [g, v] : byGroup)
        target[g] = judgedByPeak (g) ? medianOf (sustainedPeaks) : medianOf (v);
    REQUIRE (sustainedPeaks.size() >= 3);
    for (int slot = 1; slot < kBankSize; ++slot)
    {
        const auto& prog = bank.programs[static_cast<size_t> (slot)];
        const auto g = categoryInfo (prog.meta.category).group;
        const bool byPeak = judgedByPeak (g);
        const double value = byPeak ? peak[static_cast<size_t> (slot)] : rms[static_cast<size_t> (slot)];
        const double offset = levelOffsetDb (slot);
        const double dev = value - (target[g] + offset);
        SAY ("%s %s [%s]: %s %.2f dB, target %.2f dB + offset %.1f dB, deviation %+.2f dB\n", kBank[slot].slot, kBank[slot].name, groupName (g),
             byPeak ? "phrase peak" : "phrase RMS", value, target[g], offset, dev);
        INFO (prog.meta.name << " [" << groupName (g) << "]: " << (byPeak ? "phrase peak " : "phrase RMS ") << value << " dB, target "
                             << (byPeak ? "(the sustained group's median peak, offset-0 programs) " : "(the group's median RMS, offset-0 programs) ")
                             << target[g] << " dB + the program's offset " << offset << " dB, deviation " << dev << " dB");
        CHECK (std::abs (dev) <= kGroupToleranceDb);
    }
}

// ---- A2 to A8 ----------------------------------------------------------------------------------------------------

TEST_CASE ("Bank A2 Moving Pulse: the pulse width sweeps by table, the vibrato starts after 8 ticks and runs at 8.35 Hz, Solo with glide", "[bside][bank][program]")
{
    Rig rig (1, false);
    CHECK (rig.get ("lower.assign.mode") == Catch::Approx (2.0f)); // Solo
    CHECK (rig.get ("lower.porta.time") > 0.0f);
    CHECK (rig.get ("lower.chip.model") == Catch::Approx (1.0f)); // Model B
    const auto p = probe (rig, 0, { 60 }, 1.6, 0.2);
    const auto t = tickSamples (p.samples, 72);
    for (const auto& s : t)
        REQUIRE (s.voiceFound);

    // the width read back from the chip, tick by tick: set 768, then +40 per tick up to 3328 and back down
    std::string series;
    for (int k = 0; k < 72; ++k)
        series += std::to_string (pulseWidth (t[static_cast<size_t> (k)], 0)) + " ";
    SAY ("A2 pulse width (register, 4096 = 100 %%) per tick: %s\n", series.c_str());
    CHECK (pulseWidth (t[0], 0) == 768);
    for (int k = 1; k <= 64; ++k)
        CHECK (pulseWidth (t[static_cast<size_t> (k)], 0) == 768 + 40 * k);
    CHECK (pulseWidth (t[64], 0) == 3328);
    CHECK (pulseWidth (t[65], 0) == 3288);
    CHECK (pulseWidth (t[71], 0) == 3328 - 40 * 7);

    // the frequency lane (after the owner's audition; it was 15 ticks of delay and +-20 cents every two ticks): no vibrato for 8 ticks,
    // then +-20 cents every three ticks
    const double f0 = freqHz (t[0], 0);
    CHECK (f0 == Catch::Approx (hzOfNote (60)).margin (0.06));
    for (int k = 0; k < 8; ++k)
        CHECK (freqRegister (t[static_cast<size_t> (k)], 0) == freqRegister (t[0], 0));
    std::string cents;
    for (int k = 5; k < 30; ++k)
    {
        const double c = 100.0 * semitonesBetween (freqHz (t[static_cast<size_t> (k)], 0), f0);
        cents += juce::String (c, 1).toStdString() + " ";
        if (k >= 8)
        {
            const double want = ((k - 8) / 3) % 2 == 0 ? 20.0 : -20.0;
            INFO ("tick " << k << ": " << c << " cents, want " << want);
            CHECK (std::abs (c - want) < 1.0);
        }
    }
    SAY ("A2 vibrato in cents (ticks 5 to 29): %s\n", cents.c_str());
    // the onset delay and the rate follow from the tick period: 8 ticks and a six-tick period (it was 15 ticks and a four-tick one)
    const double tickMs = msOfTicks (t[0].tickPeriod, 1.0);
    SAY ("A2 vibrato onset %.1f ms (%.1f ms before), rate %.2f Hz (%.2f Hz before); tick %.2f ms\n", 8.0 * tickMs, 15.0 * tickMs, 1000.0 / (6.0 * tickMs),
         1000.0 / (4.0 * tickMs), tickMs);
    CHECK (8.0 * tickMs == Catch::Approx (159.6).margin (0.5));
    CHECK (1000.0 / (6.0 * tickMs) == Catch::Approx (8.35).margin (0.02));

    // Solo with glide: a second key pressed while the first is held reaches its pitch over several tens of milliseconds,
    // where the same program without glide jumps at once
    const auto glide = [&] (float portaTime) {
        Rig g (1, false);
        g.set ("lower.porta.time", portaTime);
        const std::vector<Midi> midi { noteOn (at (0.1), 60), noteOn (at (0.4), 67), noteOff (at (0.41), 60) };
        const auto pr = probeMidi (g, 0, midi, 0.8);
        std::vector<double> semis; // the pitch of osc 1 in semitones above C4, every block from the second note-on
        for (const auto& s : pr.samples)
            if (s.seconds >= 0.4 && s.seconds < 0.9 && s.voiceFound)
                semis.push_back (semitonesBetween (freqHz (s, 0), hzOfNote (60)));
        return semis;
    };
    const auto withGlide = glide (0.3f), without = glide (0.0f);
    REQUIRE (withGlide.size() > 50);
    REQUIRE (without.size() > 50);
    int intermediate = 0;
    for (const double x : withGlide)
        intermediate += x > 0.5 && x < 6.5 ? 1 : 0;
    int jumpedAtOnce = 0;
    for (const double x : without)
        jumpedAtOnce += x > 0.5 && x < 6.5 ? 1 : 0;
    SAY ("A2 glide: %d blocks between C4 and G4 with porta.time 0.3, %d without; first/last %.2f %.2f st\n", intermediate, jumpedAtOnce, withGlide.front(),
         withGlide.back());
    CHECK (intermediate >= 100); // over 100 ms of the pitch between the two keys (porta.time 0.3: 99 % of the interval in 0.45 s)
    CHECK (jumpedAtOnce <= 2);
    CHECK (withGlide.back() > 6.5); // it arrives (the vibrato of the table moves it by 0.2 semitones either way)
}

TEST_CASE ("Bank A3 Synced Climb: oscillator 2 synced to 1, oscillator 1 silent, the synced pitch stepping up and down by table", "[bside][bank][program]")
{
    Rig rig (2, false);
    CHECK (rig.get ("lower.chip.model") == Catch::Approx (1.0f));
    CHECK (rig.get ("lower.osc2.sync") == Catch::Approx (1.0f));
    const auto p = probe (rig, 0, { 60 }, 1.5, 0.1);
    const auto t = tickSamples (p.samples, 64);
    for (const auto& s : t)
        REQUIRE (s.voiceFound);

    // the register bits: oscillator 2 is sawtooth, gated and synced (0x20 | 0x02 | 0x01); oscillator 1 has no control bits and
    // runs at the key's pitch, as the sync source
    CHECK (control (t[0], 1) == 0x23);
    CHECK (control (t[0], 0) == 0x00);
    CHECK (freqHz (t[0], 0) == Catch::Approx (hzOfNote (60)).margin (0.06));
    // the synced pitch per tick, in semitones above oscillator 1: +3 for two ticks, then +4, +5 ... +24 and back down
    std::string series;
    int mismatches = 0;
    for (int k = 0; k < 64; ++k)
    {
        const double st = semitonesBetween (freqHz (t[static_cast<size_t> (k)], 1), freqHz (t[static_cast<size_t> (k)], 0));
        series += juce::String (st, 2).toStdString() + " ";
        const int step = k / 2;         // each pitch holds two ticks
        const int want = step <= 21 ? 3 + step : 3 + 21 - (step - 21) + 0; // up to +24 (step 21), then down by one per step
        if (std::abs (st - want) > 0.05)
            ++mismatches;
        CHECK ((control (t[static_cast<size_t> (k)], 1) & 0x02) != 0); // sync stays on
    }
    SAY ("A3 oscillator 2 above oscillator 1 (semitones) per tick: %s\n", series.c_str());
    CHECK (mismatches == 0);


    // the same program without the sync bit (the table's `sync` token removed): the register bit is gone and the spectrum differs
    const auto lines = tableOf (factoryBank().programs[2], 0);
    const auto plainLines = withoutToken (lines, "+sync");
    REQUIRE (plainLines != lines);
    {
        auto registers = rigWithTable (2, plainLines);
        const auto q = probe (*registers, 0, { 60 }, 0.3, 0.0);
        CHECK ((control (tickSamples (q.samples, 2)[0], 1) & 0x02) == 0);
    }
    auto plain = rigWithTable (2, plainLines);

    Performance perf;
    addNote (perf, 0.05, 1.05, 60, kVelocity);
    finish (perf);
    Rig withSync (2, false);
    const auto a = measure (withSync.play (perf, 0.0), windowOf (0.2, 1.0));
    const auto b = measure (plain->play (perf, 0.0), windowOf (0.2, 1.0));
    const double distance = bandDistanceDb (a, b);
    SAY ("A3 with sync: centroid %.0f Hz; without: %.0f Hz; largest octave-band difference %.1f dB\n", a.centroidHz, b.centroidHz, distance);
    CHECK (distance > 3.0);
}

TEST_CASE ("Bank A4 Glass Ring: the triangle ring-modulated by oscillator 3 for 10 ticks, oscillator 3 falling in pitch, then a plain triangle", "[bside][bank][program]")
{
    Rig rig (3, false);
    CHECK (rig.get ("lower.chip.model") == Catch::Approx (1.0f));
    const auto p = probe (rig, 0, { 60 }, 1.0, 0.1);
    const auto t = tickSamples (p.samples, 40);
    for (const auto& s : t)
        REQUIRE (s.voiceFound);

    // oscillator 1: triangle, gated, ring on for ticks 0 to 9 (control 0x15), then off (0x11); its pitch never moves
    std::string bits, hz;
    for (int k = 0; k < 14; ++k)
    {
        bits += juce::String::toHexString (control (t[static_cast<size_t> (k)], 0)).toStdString() + " ";
        hz += juce::String (freqHz (t[static_cast<size_t> (k)], 2), 1).toStdString() + " ";
    }
    SAY ("A4 oscillator 1 control per tick: %s\nA4 oscillator 3 frequency (Hz) per tick: %s\n", bits.c_str(), hz.c_str());
    for (int k = 0; k < 40; ++k)
    {
        CHECK (control (t[static_cast<size_t> (k)], 0) == (k < 10 ? 0x15 : 0x11));
        CHECK (freqRegister (t[static_cast<size_t> (k)], 0) == freqRegister (t[0], 0));
        CHECK (control (t[static_cast<size_t> (k)], 2) == 0x10); // the ring source: a triangle with no gate
    }
    // oscillator 3 (the ring source) falls: +10.5, +8.5, +6.5 semitones above the key, 3, 3 and then to the end of the table
    for (int k = 0; k < 40; ++k)
    {
        const double want = k < 3 ? 10.5 : k < 6 ? 8.5 : 6.5;
        CHECK (freqHz (t[static_cast<size_t> (k)], 2) == Catch::Approx (hzOfNote (60 + want)).margin (0.06));
    }
    CHECK (freqHz (t[0], 2) > freqHz (t[3], 2));
    CHECK (freqHz (t[3], 2) > freqHz (t[6], 2));

    // the spectrum against the same program without ring (the table's `ring` token removed), early and late
    const auto plainLines = withoutToken (tableOf (factoryBank().programs[3], 0), "+ring");
    {
        auto registers = rigWithTable (3, plainLines);
        CHECK ((control (tickSamples (probe (*registers, 0, { 60 }, 0.3, 0.0).samples, 2)[0], 0) & 0x04) == 0);
    }
    auto plain = rigWithTable (3, plainLines);
    Performance perf;
    addNote (perf, 0.05, 1.05, 60, kVelocity);
    finish (perf);
    Rig ringRig (3, false);
    const auto ringOut = ringRig.play (perf, 0.0);
    const auto plainOut = plain->play (perf, 0.0);
    const auto ringEarly = measure (ringOut, windowOf (0.06, 0.2)), plainEarly = measure (plainOut, windowOf (0.06, 0.2));
    const auto ringLate = measure (ringOut, windowOf (0.5, 1.0)), plainLate = measure (plainOut, windowOf (0.5, 1.0));
    SAY ("A4 early (60-200 ms): with ring centroid %.0f Hz, without %.0f Hz, band difference %.1f dB; late (0.5-1 s): %.0f vs %.0f Hz, difference %.1f dB\n",
         ringEarly.centroidHz, plainEarly.centroidHz, bandDistanceDb (ringEarly, plainEarly), ringLate.centroidHz, plainLate.centroidHz,
         bandDistanceDb (ringLate, plainLate));
    CHECK (bandDistanceDb (ringEarly, plainEarly) > 3.0); // the ring changes the attack
    CHECK (bandDistanceDb (ringLate, plainLate) < 0.5);   // and nothing of it is left later: the same tone
    CHECK (bandDistanceDb (ringEarly, ringLate) > 3.0);   // the attack and the tone differ
}

TEST_CASE ("Bank A5 Closing Door: sawtooth and pulse through the low-pass, a table sweep closing the cutoff, velocity to cutoff", "[bside][bank][program]")
{
    Rig rig (4, false);
    CHECK (rig.get ("lower.vel.cutoff") > 0.0f);
    const auto p = probe (rig, 0, { 60 }, 1.0, 0.1);
    const auto t = tickSamples (p.samples, 30);
    for (const auto& s : t)
        REQUIRE (s.voiceFound);
    CHECK (control (t[0], 0) == 0x21); // sawtooth
    CHECK (control (t[0], 1) == 0x41); // pulse
    CHECK (freqHz (t[0], 1) == Catch::Approx (hzOfNote (48)).margin (0.06)); // an octave below
    CHECK (filterMode (t[0]) == 1);    // low-pass
    CHECK ((routing (t[0]) & 3) == 3); // both through the filter
    CHECK (resonance (t[0]) == 9);

    std::string series;
    for (int k = 0; k < 30; ++k)
        series += std::to_string (cutoffRegister (t[static_cast<size_t> (k)])) + " ";
    SAY ("A5 cutoff register per tick (velocity 100): %s\n", series.c_str());
    for (int k = 1; k < 20; ++k)
        CHECK (std::abs ((cutoffRegister (t[static_cast<size_t> (k - 1)]) - cutoffRegister (t[static_cast<size_t> (k)])) - 75) <= 1); // closes by 75 a tick
    for (int k = 20; k < 30; ++k)
        CHECK (cutoffRegister (t[static_cast<size_t> (k)]) == cutoffRegister (t[19])); // and stops after 20 steps
    CHECK (cutoffRegister (t[0]) - cutoffRegister (t[19]) == Catch::Approx (19 * 75).margin (19));

    // velocity to cutoff: the same note at four velocities starts the sweep at different cutoffs
    std::vector<int> start;
    for (const int velocity : { 127, 100, 64, 20 })
    {
        Rig v (4, false);
        start.push_back (cutoffRegister (tickSamples (probe (v, 0, { 60 }, 0.2, 0.0, velocity).samples, 1)[0]));
    }
    SAY ("A5 cutoff at tick 0 for velocity 127, 100, 64, 20: %d %d %d %d\n", start[0], start[1], start[2], start[3]);
    CHECK (start[0] > start[1]);
    CHECK (start[1] > start[2]);
    CHECK (start[2] > start[3]);
    CHECK (start[0] - start[2] > 250);

    // the audio closes: the spectrum is darker late in a held note than at its start
    Performance perf;
    addNote (perf, 0.05, 1.05, 60, kVelocity);
    finish (perf);
    Rig audio (4, false);
    const auto out = audio.play (perf, 0.0);
    const auto early = measure (out, windowOf (0.06, 0.25)), late = measure (out, windowOf (0.6, 1.0));
    SAY ("A5 centroid early %.0f Hz, late %.0f Hz\n", early.centroidHz, late.centroidHz);
    CHECK (early.centroidHz > 1.3 * late.centroidHz);
}

TEST_CASE ("Bank A6 Blip Bass: a two-tick octave-up blip from the wave lane, then a narrow pulse, no filter", "[bside][bank][program]")
{
    Rig rig (5, false);
    const auto p = probe (rig, 0, { 60 }, 0.3, 0.1);
    const auto t = tickSamples (p.samples, 12);
    for (const auto& s : t)
        REQUIRE (s.voiceFound);
    const double f0 = hzOfNote (60);
    CHECK (freqHz (t[0], 0) == Catch::Approx (2.0 * f0).margin (0.06)); // ticks 0 and 1: an octave up (one tick before the owner's audition)
    CHECK (freqHz (t[1], 0) == Catch::Approx (2.0 * f0).margin (0.06));
    for (int k = 2; k < 12; ++k)
        CHECK (freqHz (t[static_cast<size_t> (k)], 0) == Catch::Approx (f0).margin (0.06)); // tick 2 on: the key
    SAY ("A6 oscillator 1 frequency tick 0 %.2f Hz, tick 1 %.2f Hz, tick 2 %.2f Hz; pulse width %d (%.1f %%); routing %d\n", freqHz (t[0], 0), freqHz (t[1], 0),
         freqHz (t[2], 0), pulseWidth (t[2], 0), 100.0 * pulseWidth (t[2], 0) / 4096.0, routing (t[2]));
    // exactly two ticks long: the time from the first sample at 2 x f0 to the first at f0 is two tick periods
    double tBlip = -1.0, tBack = -1.0;
    for (const auto& s : p.samples)
    {
        if (tBlip < 0.0 && freqHz (s, 0) > 1.5 * f0)
            tBlip = s.seconds;
        if (tBlip >= 0.0 && tBack < 0.0 && freqHz (s, 0) < 1.5 * f0)
            tBack = s.seconds;
    }
    const double tickMs = msOfTicks (t[0].tickPeriod, 1.0);
    SAY ("A6 blip lasts %.1f ms (one tick = %.2f ms; 19.0 ms, one tick, before the owner's audition)\n", 1000.0 * (tBack - tBlip), tickMs);
    CHECK (std::abs (1000.0 * (tBack - tBlip) - 2.0 * tickMs) < 1.5); // two ticks, to within a block
    CHECK (control (t[2], 0) == 0x41);                                 // pulse, gated
    CHECK (pulseWidth (t[2], 0) == 512);                               // 12.5 %: narrow
    CHECK ((routing (t[2]) & 1) == 0);                                 // not through the filter: no filter
}

TEST_CASE ("Bank A7 and A8: the arpeggio is the wave lane's, at the tick rate, one key plays the chord; A8 is the minor counterpart at twice the rate with a pulse-width step per tick",
           "[bside][bank][program]")
{
    const double f0 = hzOfNote (60);
    // A7: a major chord up and down, one pitch per tick
    Rig a7 (6, false);
    CHECK (a7.get ("arp.mode") == Catch::Approx (0.0f));        // the shell's arpeggiator is off
    CHECK (a7.get ("lower.tick.rate") == Catch::Approx (2.0f)); // Tempo
    const auto p7 = probe (a7, 0, { 60 }, 1.2, 0.2);
    const auto t7 = tickSamples (p7.samples, 24);
    for (const auto& s : t7)
        REQUIRE (s.voiceFound);
    const int major[] { 0, 4, 7, 12, 7, 4 };
    std::string seq7;
    for (int k = 0; k < 24; ++k)
    {
        const double st = semitonesBetween (freqHz (t7[static_cast<size_t> (k)], 0), f0);
        seq7 += juce::String (st, 2).toStdString() + " ";
        CHECK (st == Catch::Approx (major[k % 6]).margin (0.01));
    }
    SAY ("A7 semitones above the key, per tick: %s\n", seq7.c_str());
    const std::uint32_t period7 = t7[0].tickPeriod;
    const double tempo = a7.proc.getTickTempo();
    CHECK (period7 == static_cast<std::uint32_t> (std::lround (985248.0 * 60.0 / (tempo * 12.0)))); // tick.sync 12 per beat, tick.mult 1
    SAY ("A7 tick %u cycles = %.2f ms at %.0f bpm\n", period7, msOfTicks (period7, 1.0), tempo);

    // the tick follows the tempo: at 90 bpm the period is 4/3 as long
    Rig slow (6, false);
    slow.set ("arp.tempo", 90.0f);
    const auto slowT = tickSamples (probe (slow, 0, { 60 }, 0.3, 0.0).samples, 2);
    CHECK (static_cast<double> (slowT[0].tickPeriod) / static_cast<double> (period7) == Catch::Approx (120.0 / 90.0).margin (0.001));

    // one key plays the chord: one voice sounds, with the chord's four pitches in turn
    {
        Rig one (6, false);
        const auto pr = probeMidi (one, 0, { noteOn (at (0.1), 60) }, 0.5);
        CHECK (one.proc.getLowerPart().numActiveVoices() == 1);
        std::set<int> pitches;
        for (const auto& s : pr.samples)
            if (s.voiceFound && s.seconds > 0.1)
                pitches.insert (freqRegister (s, 0));
        CHECK (pitches.size() == 4); // +0, +4, +7, +12
    }
    // after the note-off the gate is cleared and the lane has ended: the pitch holds
    const auto& last = p7.samples.back();
    CHECK ((control (last, 0) & 1) == 0);
    CHECK (freqRegister (p7.samples.back(), 0) == freqRegister (p7.samples[p7.samples.size() - 100], 0));

    // A8: minor, a tick twice as fast, the width steps +128 per tick
    Rig a8 (7, false);
    CHECK (a8.get ("arp.mode") == Catch::Approx (0.0f));
    const auto p8 = probe (a8, 0, { 60 }, 1.0, 0.1);
    const auto t8 = tickSamples (p8.samples, 40);
    for (const auto& s : t8)
        REQUIRE (s.voiceFound);
    const int minor[] { 0, 3, 7, 10, 12, 10, 7, 3 };
    std::string seq8, width8;
    for (int k = 0; k < 40; ++k)
    {
        const double st = semitonesBetween (freqHz (t8[static_cast<size_t> (k)], 0), f0);
        CHECK (st == Catch::Approx (minor[k % 8]).margin (0.01));
        if (k < 16)
        {
            seq8 += juce::String (st, 2).toStdString() + " ";
            width8 += std::to_string (pulseWidth (t8[static_cast<size_t> (k)], 0)) + " ";
        }
    }
    SAY ("A8 semitones above the key: %s\nA8 pulse width per tick: %s\n", seq8.c_str(), width8.c_str());
    CHECK (t8[0].tickPeriod * 2 == period7); // twice the rate
    CHECK (pulseWidth (t8[0], 0) == 640);
    for (int k = 1; k <= 20; ++k)
        CHECK (pulseWidth (t8[static_cast<size_t> (k)], 0) == 640 + 128 * k); // a step per tick
    CHECK (pulseWidth (t8[21], 0) == 3200 - 128);                              // it turns at 3200
}

// ---- B1 to B8 ----------------------------------------------------------------------------------------------------

TEST_CASE ("Bank B1 Short Kick: a noise tick, then a triangle falling in pitch by table within eight ticks, the same for every key", "[bside][bank][program]")
{
    const int notes[] { 96, 55, 43, 36, 33, 31, 29, 28, 27 }; // the table's absolute pitches (=C7, =55 ...)
    std::vector<double> fallHz;
    for (const int key : { 36, 60 })
    {
        Rig rig (8, false);
        const auto p = probe (rig, 0, { key }, 0.3, 0.1);
        const auto t = tickSamples (p.samples, 12);
        for (const auto& s : t)
            REQUIRE (s.voiceFound);
        CHECK (control (t[0], 0) == 0x81); // the noise tick: noise, gated
        std::string hz;
        for (int k = 0; k < 12; ++k)
        {
            const int m = k < 9 ? notes[k] : 27;
            CHECK (freqHz (t[static_cast<size_t> (k)], 0) == Catch::Approx (hzOfNote (m)).margin (0.06));
            if (k >= 1)
                CHECK (control (t[static_cast<size_t> (k)], 0) == 0x11); // then the triangle
            if (key == 36)
            {
                hz += juce::String (freqHz (t[static_cast<size_t> (k)], 0), 1).toStdString() + " ";
                fallHz.push_back (freqHz (t[static_cast<size_t> (k)], 0));
            }
        }
        CHECK (t[0].reg[5] == 0x08); // attack 0, decay 8
        CHECK (t[0].reg[6] == 0x08); // sustain 0, release 8
        for (const auto& s : p.samples)
            if (s.voiceFound)
                CHECK (doublesOscillator1 (s)); // the level: oscillator 2 plays the same drum in phase
        if (key == 36)
            SAY ("B1 oscillator 1 frequency (Hz) per tick (tick = %.2f ms): %s\n", msOfTicks (t[0].tickPeriod, 1.0), hz.c_str());
        CHECK (msOfTicks (t[0].tickPeriod, 1.0) < 10.5); // tick.mult 2
    }
    // the fall in Hz per tick, strictly down for ticks 1 to 8 and then flat
    for (size_t k = 2; k <= 8; ++k)
        CHECK (fallHz[k] < fallHz[k - 1]);
    CHECK (fallHz[9] == Catch::Approx (fallHz[8]));

    // Deeper than before the owner's audition (8.21.2): the same drum with the table it had then (the triangle falling from =64 to =38
    // in six ticks) put more of the energy of the phrase above 125 Hz. The low band is the 0 to 125 Hz band of the spectrum summary
    // (BankSound.h); the length is the time from the note-on to the last 5 ms of the hit within 40 dB of its peak.
    const std::vector<std::string> before {
        "bside-table 1",      "[restart]",         "mode classic",
        "lead 2",             "[wave1]",           "    adsr 0 8 0 8, wave noise+gate =C7",
        "    wave tri+gate =64", "    wave tri+gate =55", "    wave tri+gate =48",
        "    wave tri+gate =43", "    wave tri+gate =40", "    wave tri+gate =38 ticks 255",
        "    end",            "[wave2]",           "    adsr 0 8 0 8, wave noise+gate =C7",
        "    wave tri+gate =64", "    wave tri+gate =55", "    wave tri+gate =48",
        "    wave tri+gate =43", "    wave tri+gate =40", "    wave tri+gate =38 ticks 255",
        "    end"
    };
    const auto perf = phrase (Phrase::Percussion);
    // one hit (C3 held 0.2 s, 2 s of tail): its length is the time from the note-on to the end of the last 5 ms of the audio whose RMS is
    // within 43 dB of the hit's sample peak
    const auto lengthOf = [&] (const std::function<std::unique_ptr<Rig>()>& makeRig) {
        Performance one;
        addNote (one, 0.05, 0.25, 48, kVelocity);
        finish (one);
        const auto rig = makeRig();
        const auto out = rig->play (one, 2.0);
        double peak = 0.0;
        for (size_t i = 0; i < out.l.size(); ++i)
            peak = std::max ({ peak, static_cast<double> (std::abs (out.l[i])), static_cast<double> (std::abs (out.r[i])) });
        const int step = at (0.005);
        int last = one.firstOn;
        for (int i = one.firstOn; i + step <= static_cast<int> (out.l.size()); i += step)
            if (rmsDbOf (out, i, i + step) > dbOf (peak) - 43.0)
                last = i + step;
        return 1000.0 * (last - one.firstOn) / kFs;
    };
    const auto bandsOf = [&] (Rig& rig) { return measure (rig.play (perf, 0.5), perf); };
    Rig now (8, false);
    auto old = rigWithTable (8, before);
    const auto mNow = bandsOf (now), mOld = bandsOf (*old);
    const double lengthNow = lengthOf ([] { return std::make_unique<Rig> (8, false); });
    const double lengthOld = lengthOf ([&] { return rigWithTable (8, before); });
    SAY ("B1 low band (0 to 125 Hz) %.2f dB now, %.2f dB before; 125 to 250 Hz %.2f / %.2f dB; length %.0f ms now, %.0f ms before\n", mNow.bandDb[0], mOld.bandDb[0],
         mNow.bandDb[1], mOld.bandDb[1], lengthNow, lengthOld);
    CHECK (mNow.bandDb[0] > mOld.bandDb[0] + 1.5); // clearly more of the energy below 125 Hz
    CHECK (mNow.bandDb[0] > -3.0);                  // more than half of it
    CHECK (lengthNow <= 1.5 * lengthOld);          // still a short kick
}

TEST_CASE ("Bank B2 Dry Snare: noise and pulse alternating by table, then noise alone", "[bside][bank][program]")
{
    Rig rig (9, false);
    const auto p = probe (rig, 0, { 48 }, 0.3, 0.1);
    const auto t = tickSamples (p.samples, 14);
    for (const auto& s : t)
        REQUIRE (s.voiceFound);
    // control bits per tick: noise (0x81), pulse (0x41), noise, pulse, noise held until tick 10, then the gate is cleared
    const int wantBits[] { 0x81, 0x41, 0x81, 0x41, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x80, 0x80, 0x80, 0x80 };
    const int wantNote[] { 98, 55, 96, 52, 95, 95, 95, 95, 95, 95, 95, 95, 95, 95 }; // =D7 =G3 =C7 =E3 =B6
    std::string bits;
    for (int k = 0; k < 14; ++k)
    {
        bits += juce::String::toHexString (control (t[static_cast<size_t> (k)], 0)).toStdString() + " ";
        CHECK (control (t[static_cast<size_t> (k)], 0) == wantBits[k]);
        CHECK (freqHz (t[static_cast<size_t> (k)], 0) == Catch::Approx (hzOfNote (wantNote[k])).margin (0.06));
    }
    SAY ("B2 oscillator 1 control per tick: %s (tick = %.2f ms)\n", bits.c_str(), msOfTicks (t[0].tickPeriod, 1.0));
    CHECK (pulseWidth (t[1], 0) == 1536);
    CHECK (t[0].reg[5] == 0x07); // decay 7
    CHECK (t[0].reg[6] == 0x07); // sustain 0, release 7
    for (const auto& s : p.samples)
        if (s.voiceFound)
            CHECK (doublesOscillator1 (s)); // the level: oscillator 2 plays the same drum in phase
}

TEST_CASE ("Bank B3 Tick Hat: short noise at a high pitch, decay only, mostly above 4 kHz", "[bside][bank][program]")
{
    Rig rig (10, false);
    const auto p = probe (rig, 0, { 48 }, 0.5, 0.1);
    const auto t = tickSamples (p.samples, 3);
    REQUIRE (t[0].voiceFound);
    CHECK (control (t[0], 0) == 0x81);
    CHECK (freqHz (t[0], 0) == Catch::Approx (hzOfNote (100)).margin (0.06)); // =E7 (=C7 before the owner's audition)
    CHECK (t[0].reg[5] == 0x06); // attack 0, decay 6
    CHECK (t[0].reg[6] == 0x06); // sustain 0: decay only
    CHECK (filterMode (t[0]) == 4);    // high-pass
    CHECK (cutoffRegister (t[0]) == 500);
    CHECK (resonance (t[0]) == 2);
    CHECK (routing (t[0]) == 7); // oscillator 1 through it, and oscillators 2 and 3, which double it (the level)
    for (const auto& s : p.samples)
        if (s.voiceFound)
        {
            CHECK (doublesOscillator1 (s, 1));
            CHECK (doublesOscillator1 (s, 2));
        }
    // the envelope: it rises at once, falls through the decay and is at 0 long before the key goes up
    double peakAt = -1.0, below128 = -1.0, below16 = -1.0, zeroAt = -1.0;
    int top = 0;
    for (const auto& s : p.samples)
    {
        if (s.env[0] >= top)
        {
            top = s.env[0];
            peakAt = s.seconds;
        }
        if (peakAt >= 0.0 && s.seconds > peakAt)
        {
            if (below128 < 0.0 && s.env[0] <= 128)
                below128 = s.seconds;
            if (below16 < 0.0 && s.env[0] <= 16)
                below16 = s.seconds;
            if (zeroAt < 0.0 && s.env[0] == 0)
                zeroAt = s.seconds;
        }
    }
    SAY ("B3 envelope: peak %d at %.1f ms, <=128 at %.1f ms, <=16 at %.1f ms, 0 at %.1f ms (key up at 500 ms)\n", top, 1000.0 * peakAt, 1000.0 * below128, 1000.0 * below16,
         1000.0 * zeroAt);
    CHECK (top >= 250);
    CHECK (zeroAt > 0.0);
    CHECK (zeroAt < 0.5);
    CHECK (below16 < 0.35);
    // short: the audio of the last 150 ms before the key goes up is silent. The measure is the RMS of the window with its mean removed
    // (review, docs/decisions.md): the output carries the slow tail of the DC that the hit's envelope pushed through the board's
    // 1.2 Hz high-pass (a constant 2.8e-5 here, -91 dB, decaying by e every 0.13 s), which grows with the drum's level (-107 dB at the
    // level before the owner's audition, -92 dB at +5 dB and a third oscillator) and says nothing about how long the hat rings; the
    // threshold, -100 dB, is the one it had.
    const double lastDb = rmsDbOf (p.audio, at (0.1 + 0.35), at (0.1 + 0.5));
    const double lastAcDb = acRmsDbOf (p.audio, at (0.1 + 0.35), at (0.1 + 0.5));
    SAY ("B3 audio RMS in the last 150 ms before the key goes up: %.1f dB with its DC, %.1f dB without\n", lastDb, lastAcDb);
    CHECK (lastAcDb < -100.0);
    Performance perf;
    addNote (perf, 0.05, 0.5, 48, kVelocity);
    finish (perf);
    Rig audio (10, false);
    const auto m = measure (audio.play (perf, 0.0), windowOf (0.05, 0.3));
    SAY ("B3 centroid %.0f Hz, %.0f %% of the energy at or above 4 kHz\n", m.centroidHz, 100.0 * shareAbove (m, 4000.0));
    CHECK (shareAbove (m, 4000.0) > 0.5);
}

TEST_CASE ("Bank B1 to B3 in time: over the eight hits every onset delay is the same within 1 ms, every peak within 1 dB, and the three drums agree within 2 ms",
           "[bside][bank][program][time]")
{
    // 8.21.2 after the owner's audition: the onset of a hit is the first sample at or above -30 dB of that hit's peak, counted from its note-on.
    const auto perf = phrase (Phrase::Percussion);
    double meanDelay[3] {};
    for (int d = 0; d < 3; ++d)
    {
        Rig rig (8 + d, false);
        const auto hits = hitTimings (rig.play (perf, 0.5), perf);
        REQUIRE (hits.size() == 8);
        double loD = 1.0e9, hiD = -1.0e9, loP = 1.0e9, hiP = -1.0e9, sum = 0.0;
        std::string row;
        for (const auto& h : hits)
        {
            REQUIRE (h.onsetMs >= 0.0);
            loD = std::min (loD, h.onsetMs);
            hiD = std::max (hiD, h.onsetMs);
            loP = std::min (loP, h.peakDb);
            hiP = std::max (hiP, h.peakDb);
            sum += h.onsetMs;
            row += juce::String (h.onsetMs, 2).toStdString() + "/" + juce::String (h.peakDb, 2).toStdString() + " ";
        }
        meanDelay[d] = sum / 8.0;
        SAY ("%s hits (onset ms / peak dBFS): %s\n   onset spread %.3f ms, peak spread %.3f dB, mean onset %.3f ms\n", kBank[8 + d].slot, row.c_str(), hiD - loD,
             hiP - loP, meanDelay[d]);
        INFO (kBank[8 + d].slot << " " << kBank[8 + d].name << ": hits (onset ms / peak dBFS) " << row);
        CHECK (hiD - loD <= 1.0);
        CHECK (hiP - loP <= 1.0);
    }
    INFO ("mean onset delays B1 " << meanDelay[0] << " ms, B2 " << meanDelay[1] << " ms, B3 " << meanDelay[2] << " ms");
    CHECK (std::max ({ meanDelay[0], meanDelay[1], meanDelay[2] }) - std::min ({ meanDelay[0], meanDelay[1], meanDelay[2] }) <= 2.0);
}

TEST_CASE ("Bank B1 to B3 in time: the measurement sees a late hit and a weak hit", "[bside][bank][program][time]")
{
    // The gate above can fail: three synthetic hits (a noise burst each, 0.25 s apart), the second starting 34 ms late, the third 3 dB lower.
    Performance perf;
    for (int i = 0; i < 3; ++i)
        addNote (perf, 0.05 + 0.25 * i, 0.05 + 0.25 * i + 0.2, 48, kVelocity);
    finish (perf);
    Stereo s;
    s.l.assign (static_cast<size_t> (at (1.0)), 0.0f);
    s.r = s.l;
    juce::Random rnd (7);
    const double delayMs[] { 0.5, 34.5, 0.5 };
    const double gain[] { 0.1, 0.1, 0.1 * std::pow (10.0, -3.0 / 20.0) };
    for (int h = 0; h < 3; ++h)
        for (int i = 0; i < at (0.02); ++i)
        {
            const auto n = static_cast<size_t> (perf.midi[static_cast<size_t> (2 * h)].pos + at (delayMs[h] / 1000.0) + i);
            s.l[n] = s.r[n] = static_cast<float> (gain[h] * (2.0 * rnd.nextDouble() - 1.0) * (1.0 - i / static_cast<double> (at (0.02))));
        }
    const auto hits = hitTimings (s, perf);
    REQUIRE (hits.size() == 3);
    CHECK (hits[1].onsetMs - hits[0].onsetMs > 30.0);   // the late hit is 34 ms later: the 1 ms bound sees it
    CHECK (hits[0].peakDb - hits[2].peakDb > 2.0);      // the weak hit is about 3 dB down: the 1 dB bound sees it
    CHECK (std::abs (hits[1].peakDb - hits[0].peakDb) < 1.0); // and a hit that is only late is not weak
}

TEST_CASE ("Bank, Clean never waits: a hat with [restart] classic, lead 2 starts on time on a hit whose voice has had its full lead", "[bside][bank][program][time]")
{
    // Was the hidden known defect of the owner's audition round (8.21.2 "Clean never waits"). B3 with the restart section it had before the
    // owner's audition (two oscillators, mode classic, lead 2 of 19.95 ms ticks = 39.9 ms), two hits 0.5 s apart: after the first, the voice is
    // idle for about 240 ms, five times the lead, so the second note gets the full lead. Before the fix Clean cleared the envelope counters only
    // without the full lead, and this hit waited 32768 cycles (33.0 ms here): the idle counter loops over 0..8 in the restart state, and a gate
    // edge that finds it at 8 steps it past the attack's match (tests/bside/unit/ProgramEngineChipTests.cpp, "Clean never waits: the mechanism").
    const std::vector<std::string> classic {
        "bside-table 1", "[restart]", "mode classic", "lead 2", "[wave1]", "    adsr 0 6 0 6, wave noise+gate =E7", "    end",
        "[wave2]", "    adsr 0 6 0 6, wave noise+gate =E7", "    end",
    };
    auto rig = rigWithTable (10, classic);
    rig->set ("lower.osc3.wave", 0.0f);
    Performance perf;
    addNote (perf, 0.05, 0.25, 48, kVelocity);
    addNote (perf, 0.55, 0.75, 48, kVelocity);
    finish (perf);
    const auto hits = hitTimings (rig->play (perf, 0.5), perf);
    REQUIRE (hits.size() == 2);
    SAY ("hat, classic, lead 2: onset of hit 1 %.2f ms, onset of hit 2 %.2f ms\n", hits[0].onsetMs, hits[1].onsetMs);
    CHECK (std::abs (hits[1].onsetMs - hits[0].onsetMs) <= 1.0);
}

TEST_CASE ("Bank B2 in time at wider spacings: with its restart section (classic, lead 2), every onset is within 1 ms of the first at 0.35, 0.5 and 0.7 s",
           "[bside][bank][program][time]")
{
    // The spacings at which B2 waited 33 ms before Clean cleared the envelope counters at every note start (docs/bside-factory-sounds.md).
    for (const double spacing : { 0.35, 0.5, 0.7 })
    {
        Performance perf;
        for (int i = 0; i < 6; ++i)
            addNote (perf, 0.05 + spacing * i, 0.05 + spacing * i + 0.2, 48, kVelocity);
        finish (perf);
        Rig rig (9, false);
        const auto hits = hitTimings (rig.play (perf, 0.5), perf);
        REQUIRE (hits.size() == 6);
        std::string row;
        double spread = 0.0;
        for (const auto& h : hits)
        {
            row += juce::String (h.onsetMs, 2).toStdString() + " ";
            spread = std::max (spread, std::abs (h.onsetMs - hits[0].onsetMs));
        }
        SAY ("B2 at %.2f s: onsets (ms) %s, largest difference from the first %.2f ms\n", spacing, row.c_str(), spread);
        INFO ("B2 at " << spacing << " s: onsets (ms) " << row);
        for (const auto& h : hits)
        {
            CHECK (h.onsetMs >= 0.0);
            CHECK (std::abs (h.onsetMs - hits[0].onsetMs) <= 1.0);
        }
    }
}

TEST_CASE ("Bank B4 Slow Saw Pad: Model A, three detuned sawtooths through the low-pass with resonance, a slow attack, Per Note", "[bside][bank][program]")
{
    Rig rig (11, false);
    const auto p = probe (rig, 0, { 60, 64, 67, 72 }, 1.2, 0.2);
    // the configuration the audio thread used: Model A, Per Note
    const auto& lower = rig.proc.getBlockParams().lowerParams;
    CHECK (lower.chipModel == 0);
    CHECK (lower.filterTopology == 0);
    const auto* voice = newestVoice (rig.proc, 0);
    REQUIRE (voice != nullptr);
    CHECK (voice->filter.model() == vs::bside::chip::Model::A); // the voice's own filter, Model A

    const auto t = tickSamples (p.samples, 3);
    REQUIRE (t[0].voiceFound);
    for (int o = 0; o < 3; ++o)
        CHECK (control (t[0], o) == 0x21); // three sawtooths, gated
    const double c01 = 1200.0 * std::log2 (freqHz (t[0], 0) / freqHz (t[0], 1)), c21 = 1200.0 * std::log2 (freqHz (t[0], 2) / freqHz (t[0], 1));
    SAY ("B4 detune of oscillator 1 and 3 against 2 (cents): %.1f %.1f\n", c01, c21);
    CHECK (std::abs (c01 - -14.0) < 1.0);
    CHECK (std::abs (c21 - 14.0) < 1.0);
    CHECK (filterMode (t[0]) == 1);
    CHECK (resonance (t[0]) == 5);
    CHECK (routing (t[0]) == 7);
    CHECK (t[0].reg[5] == 0x90); // attack 9 (250 ms), decay 0
    // the slow attack: the envelope counter takes more than 150 ms to reach 200 (sustain 12 = 204)
    double reach = -1.0;
    for (const auto& s : p.samples)
        if (reach < 0.0 && s.voiceFound && s.env[0] >= 200)
            reach = s.seconds;
    SAY ("B4 envelope of the newest voice reaches 200 of 255 at %.0f ms\n", 1000.0 * reach);
    CHECK (reach > 0.15);
    CHECK (reach < 0.30);
}

TEST_CASE ("Bank B5 Shared Chord Pad: Model A, Shared topology, chords into one filter, moderate resonance", "[bside][bank][program]")
{
    Rig rig (12, false);
    probe (rig, 0, { 60, 64, 67, 72 }, 1.0, 0.2);
    const auto& lower = rig.proc.getBlockParams().lowerParams;
    CHECK (lower.chipModel == 0);
    CHECK (lower.filterTopology == 1);
    // while the chord is held: four voices, one shared filter, Model A, low-pass, resonance 6
    Rig held (12, false);
    probeMidi (held, 0, { noteOn (at (0.1), 60), noteOn (at (0.1), 64), noteOn (at (0.1), 67), noteOn (at (0.1), 72) }, 0.5);
    CHECK (held.proc.getLowerPart().numActiveVoices() == 4);
    const auto& shared = held.proc.getLowerPart().getSharedFilter();
    CHECK (shared.model() == vs::bside::chip::Model::A);
    CHECK (shared.mode() == 0x10); // low-pass (bits 4 to 6 of the volume register)
    CHECK (shared.resonance() == 6);
    SAY ("B5 shared filter: Model A %d, mode bits %d, resonance %d, cutoff register %.0f; %d voices\n", shared.model() == vs::bside::chip::Model::A ? 1 : 0, shared.mode(),
         shared.resonance(), static_cast<double> (shared.cutoff()), held.proc.getLowerPart().numActiveVoices());

    // the same chord through one filter per note (Per Note) is a different signal
    Performance perf;
    for (const int n : { 60, 64, 67, 72 })
        addNote (perf, 0.05, 1.05, n, kVelocity);
    finish (perf);
    Rig sharedRig (12, false);
    Rig perNote (12, false);
    perNote.set ("lower.filter.topology", 0.0f);
    const auto a = measure (sharedRig.play (perf, 0.0), windowOf (0.4, 1.0));
    const auto b = measure (perNote.play (perf, 0.0), windowOf (0.4, 1.0));
    SAY ("B5 chord: Shared RMS %.2f dB centroid %.0f Hz; Per Note RMS %.2f dB centroid %.0f Hz; band difference %.1f dB\n", a.rmsDb, a.centroidHz, b.rmsDb, b.centroidHz,
         bandDistanceDb (a, b));
    CHECK (std::abs (a.rmsDb - b.rmsDb) > 0.5);
}

TEST_CASE ("Bank B6 Band Sweep: noise through the band-pass, the cutoff swept by the filter lane, a long release", "[bside][bank][program]")
{
    Rig rig (13, false);
    const auto p = probe (rig, 0, { 60 }, 2.7, 4.0);
    const auto t = tickSamples (p.samples, 135);
    for (const auto& s : t)
        REQUIRE (s.voiceFound);
    CHECK (control (t[0], 0) == 0x81); // noise, gated
    CHECK (filterMode (t[0]) == 2);    // band-pass
    CHECK (routing (t[0]) == 1);       // oscillator 1 only
    CHECK (resonance (t[0]) == 10);
    std::string series;
    for (int k = 0; k < 135; ++k)
    {
        if (k % 15 == 0 || k == 120)
            series += std::to_string (cutoffRegister (t[static_cast<size_t> (k)])) + " ";
        const int want = std::min (100 + 16 * k, 100 + 16 * 120);
        CHECK (cutoffRegister (t[static_cast<size_t> (k)]) == want);
    }
    SAY ("B6 cutoff register at ticks 0, 15, 30 ... 120: %s (tick = %.2f ms)\n", series.c_str(), msOfTicks (t[0].tickPeriod, 1.0));
    // the long release: from the key going up (2.7 s) until the envelope counter is below 32 of 255
    double below = -1.0;
    for (const auto& s : p.samples)
        if (s.seconds > 2.7 && below < 0.0 && s.env[0] < 32)
            below = s.seconds - 2.7;
    SAY ("B6 envelope below 32 of 255 after %.2f s of release\n", below);
    CHECK ((t[0].reg[6] & 0x0f) == 11); // release 11: 2.4 s nominal
    CHECK (below > 1.0);
}

TEST_CASE ("Bank B7 Pulse Over Pad: Dual, Lower a Model B pulse lead, Upper a Model A pad, panned apart", "[bside][bank][program]")
{
    Rig rig (14, false);
    const auto lowerProbe = probeMidi (rig, 0, { noteOn (at (0.1), 60), noteOn (at (0.1), 64) }, 0.7);
    CHECK (rig.proc.getKeyRouter().getMode() == vs::KeyMode::Dual);
    const auto& params = rig.proc.getBlockParams();
    CHECK (params.lowerParams.chipModel == 1); // Model B
    CHECK (params.upperParams.chipModel == 0); // Model A
    CHECK (params.lowerParams.mixPan == Catch::Approx (-0.6f));
    CHECK (params.upperParams.mixPan == Catch::Approx (0.6f));
    // both parts answer both keys
    for (const int part : { 0, 1 })
    {
        const auto& pt = part == 0 ? rig.proc.getLowerPart() : rig.proc.getUpperPart();
        std::set<int> keys;
        for (int i = 0; i < bs::Part::kVoices; ++i)
            if (pt.getVoice (i).active)
                keys.insert (pt.getVoice (i).note);
        CHECK (keys == std::set<int> { 60, 64 });
    }
    // the registers: Lower pulse (0x41) with a table-swept width, Upper two sawtooths (0x21)
    const auto lo = tickSamples (lowerProbe.samples, 6);
    REQUIRE (lo[3].voiceFound);
    CHECK (control (lo[3], 0) == 0x41);
    CHECK (pulseWidth (lo[3], 0) == 768 + 40 * 3);
    Rig rig2 (14, false);
    const auto upperProbe = probeMidi (rig2, 1, { noteOn (at (0.1), 60) }, 0.3);
    const auto up = tickSamples (upperProbe.samples, 2);
    REQUIRE (up[0].voiceFound);
    CHECK (control (up[0], 0) == 0x21);
    CHECK (control (up[0], 1) == 0x21);
    CHECK (control (up[0], 2) == 0x00);

    // panned apart: with only the Lower part audible the left channel is louder; with only the Upper part the right is
    const auto balance = [&] (float lowerLevel, float upperLevel) {
        Rig r (14, false);
        r.set ("lower.mix.level", lowerLevel);
        r.set ("upper.mix.level", upperLevel);
        Performance perf;
        addNote (perf, 0.05, 0.8, 60, kVelocity);
        finish (perf);
        const auto out = r.play (perf, 0.0);
        return rmsDbOf ({ out.l, out.l }, at (0.3), at (0.8)) - rmsDbOf ({ out.r, out.r }, at (0.3), at (0.8));
    };
    const double lowerOnly = balance (0.0f, -24.0f), upperOnly = balance (-24.0f, 0.0f);
    SAY ("B7 left minus right (dB): Lower alone %.1f, Upper alone %.1f\n", lowerOnly, upperOnly);
    CHECK (lowerOnly > 3.0);
    CHECK (upperOnly < -3.0);
}

TEST_CASE ("Bank B8 Bass And Lead: Split at G3, a bass below it from Lower, a lead above it from Upper", "[bside][bank][program]")
{
    Rig rig (15, false);
    probeMidi (rig, 0, { noteOn (at (0.1), 48), noteOn (at (0.1), 64), noteOn (at (0.1), 54), noteOn (at (0.1), 55) }, 0.6);
    CHECK (rig.proc.getKeyRouter().getMode() == vs::KeyMode::Split);
    CHECK (rig.proc.getKeyRouter().getSplitPoint() == 55);
    std::set<int> lowerKeys, upperKeys;
    for (int i = 0; i < bs::Part::kVoices; ++i)
    {
        if (rig.proc.getLowerPart().getVoice (i).active)
            lowerKeys.insert (rig.proc.getLowerPart().getVoice (i).note);
        if (rig.proc.getUpperPart().getVoice (i).active)
            upperKeys.insert (rig.proc.getUpperPart().getVoice (i).note);
    }
    SAY ("B8 Lower answers keys:");
    for (const int k : lowerKeys)
        SAY (" %d", k);
    SAY ("; Upper answers keys:");
    for (const int k : upperKeys)
        SAY (" %d", k);
    SAY ("\n");
    // both parts are Solo: Lower sounds the last key below the split point (54), Upper the last key at or above it (55)
    CHECK (rig.proc.getBlockParams().lowerParams.assignMode == 2);
    CHECK (rig.proc.getBlockParams().upperParams.assignMode == 2);
    CHECK (lowerKeys == std::set<int> { 54 });
    CHECK (upperKeys == std::set<int> { 55 });

    // one key each, to see which part answers: 48 sounds only in Lower, 64 only in Upper
    for (const int key : { 48, 64 })
    {
        Rig r (15, false);
        const auto pr = probeMidi (r, key < 55 ? 0 : 1, { noteOn (at (0.1), key) }, 0.5);
        CHECK (r.proc.getLowerPart().numActiveVoices() == (key < 55 ? 1 : 0));
        CHECK (r.proc.getUpperPart().numActiveVoices() == (key < 55 ? 0 : 1));
        std::vector<RegisterSample> after;
        for (auto s : pr.samples)
            if (s.seconds > 0.1)
            {
                s.seconds -= 0.1;
                after.push_back (s);
            }
        const auto t = tickSamples (after, 24);
        REQUIRE (t[10].voiceFound);
        if (key < 55)
        {
            // the bass (the kind of A5): sawtooth and pulse through a closing low-pass
            CHECK (control (t[10], 0) == 0x21);
            CHECK (control (t[10], 1) == 0x41);
            CHECK (cutoffRegister (t[10]) < cutoffRegister (t[2]));
            CHECK (std::abs ((cutoffRegister (t[1]) - cutoffRegister (t[2])) - 75) <= 1);
        }
        else
        {
            // the lead (the kind of A2): a pulse with a swept width, vibrato after 15 ticks
            CHECK (control (t[10], 0) == 0x41);
            CHECK (pulseWidth (t[10], 0) == 768 + 40 * 10);
            CHECK (freqRegister (t[14], 0) == freqRegister (t[0], 0));
            CHECK (freqRegister (t[16], 0) != freqRegister (t[0], 0));
        }
    }
}

// ---- applying a program while notes sound (8.21.2 after the drafting) ------------------------------------------------

namespace
{
/** Vintage's rule for an apply over held notes (PresetManagerTests.cpp, "Applying a program while notes sound releases them
    without a click"): the largest sample step after the apply is at most 3 x the largest step before it + 0.002. */
bool stepRuleHolds (double before, double after) { return after <= 3.0 * before + 0.002; }

/** When the change comes, and how long the render runs (the windows of stepsOf fit: 0.2 s before, 0.1 s after). */
constexpr double kChangeAt = 0.3, kRenderEnd = 0.42;

/** Three keys (48 55 64) pressed 0.05 s into the render on `rig` as it is, `change` run at kChangeAt with the keys still down,
    rendered for kRenderEnd in blocks of `block` (times count from the rig's position: a rig can be used again). */
Stereo heldKeysThen (Rig& rig, const std::function<void()>& change, int block = kBlock)
{
    const auto t0 = static_cast<int> (rig.now);
    std::vector<Midi> midi;
    for (int n : { 48, 55, 64 })
        midi.push_back (noteOn (t0 + at (0.05), n));
    return rig.render (at (kRenderEnd), midi, block, { { t0 + at (kChangeAt), change } });
}

/** The smooth sound of Vintage's test on a part (triangles, the low-pass at 300). */
void smoothSound (Rig& rig, const char* part)
{
    rig.set (juce::String (part) + ".osc1.wave", 1.0f);
    rig.set (juce::String (part) + ".filter.cutoff", 300.0f);
}

/** Init in Dual with the smooth sound on both parts, Lower panned hard left and Upper hard right: each channel is one part. */
void smoothDual (Rig& rig)
{
    rig.set ("key.mode", 1.0f);
    smoothSound (rig, "lower");
    smoothSound (rig, "upper");
    rig.set ("lower.mix.pan", -1.0f);
    rig.set ("upper.mix.pan", 1.0f);
}

struct ApplySteps
{
    double before[2] {}, after[2] {}, peakBefore[2] {}, peakAfterFade[2] {};
    int faded[2] {};
    int active[2] {};
};

/** Per channel: the largest step in the 9600 samples before the change and in the 4800 after it (Vintage's windows), the peak
    before it and the peak from 10 ms after it; per part: the applies faded and the voices still active at the end. */
ApplySteps stepsOf (Rig& rig, const Stereo& out)
{
    const size_t s0 = static_cast<size_t> (at (kChangeAt));
    ApplySteps r;
    for (int c = 0; c < 2; ++c)
    {
        const auto& x = c == 0 ? out.l : out.r;
        r.before[c] = maxStep (x, s0 - 9600, s0);
        r.after[c] = maxStep (x, s0, s0 + 4800);
        r.peakBefore[c] = peakAbs (x, s0 - 4800, s0);
        r.peakAfterFade[c] = peakAbs (x, s0 + static_cast<size_t> (at (0.01)), x.size());
    }
    r.faded[0] = rig.proc.getLowerPart().appliesFaded();
    r.faded[1] = rig.proc.getUpperPart().appliesFaded();
    r.active[0] = rig.proc.getLowerPart().numActiveVoices();
    r.active[1] = rig.proc.getUpperPart().numActiveVoices();
    return r;
}
} // namespace

TEST_CASE ("Bank: a program applied while notes sound fades them first: Vintage's step rule for all 16 programs over three held keys, from Init (Dual, both parts) and from the previous program",
           "[bside][bank][apply]")
{
    // Two rigs, each used for all 16 (a rig costs more than its render): one goes back to Init in Dual before every apply, the
    // other plays every program in turn and applies the next one over it (from B8 to A1 first).
    int failures = 0;
    Rig fromInit (0, false), chain (kBankSize - 1, false);
    for (int slot = 0; slot < kBankSize; ++slot)
    {
        INFO (kBank[slot].slot << " " << kBank[slot].name);
        {
            // from Init, Dual: Lower on the left channel, Upper on the right; both parts sound and both are applied
            auto& rig = fromInit;
            REQUIRE (rig.presets().selectProgram (0));
            smoothDual (rig);
            const int fadedBefore[2] { rig.proc.getLowerPart().appliesFaded(), rig.proc.getUpperPart().appliesFaded() };
            const auto out = heldKeysThen (rig, [&] { REQUIRE (rig.presets().selectProgram (slot)); });
            auto r = stepsOf (rig, out);
            r.faded[0] -= fadedBefore[0];
            r.faded[1] -= fadedBefore[1];
            for (int part = 0; part < 2; ++part)
            {
                const bool holds = stepRuleHolds (r.before[part], r.after[part]);
                SAY ("apply from Init (Dual), %s %s, part %d: step before %.5f, after %.5f%s\n", kBank[slot].slot, kBank[slot].name, part,
                     r.before[part], r.after[part], holds ? "" : "  FAILS");
                INFO ("from Init (Dual), part " << part << ": step before " << r.before[part] << ", after " << r.after[part]);
                REQUIRE (r.peakBefore[part] > 0.01); // it sounded
                CHECK (holds);
                failures += holds ? 0 : 1;
                CHECK (r.faded[part] == 1);  // the part faded (its parameters changed with the program)
                CHECK (r.active[part] == 0); // the held keys did not retrigger: nothing sounds after the fade
                CHECK (r.peakAfterFade[part] < 1.0e-4);
            }
        }
        {
            // from the previous program as it is (A1 after B8)
            const int from = (slot + kBankSize - 1) % kBankSize;
            auto& rig = chain;
            REQUIRE (rig.presets().currentIndex() == from);
            const auto out = heldKeysThen (rig, [&] { REQUIRE (rig.presets().selectProgram (slot)); });
            const auto r = stepsOf (rig, out);
            for (int c = 0; c < 2; ++c)
            {
                const bool holds = stepRuleHolds (r.before[c], r.after[c]);
                SAY ("apply from %s, %s %s, channel %d: step before %.5f, after %.5f%s\n", kBank[from].slot, kBank[slot].slot, kBank[slot].name, c,
                     r.before[c], r.after[c], holds ? "" : "  FAILS");
                INFO ("from " << kBank[from].slot << ", channel " << c << ": step before " << r.before[c] << ", after " << r.after[c]);
                CHECK (holds);
                failures += holds ? 0 : 1;
            }
        }
    }
    SAY ("step rule failures: %d of %d\n", failures, 4 * kBankSize);
}

TEST_CASE ("Bank: a waveform changed by hand on a held note is a register write and steps the output (a property: no fade)", "[bside][bank][apply]")
{
    // docs/bside-factory-sounds.md: as on the chip, a waveform written to a sounding oscillator changes it at once; only a program
    // apply fades the part. A cutoff change (smoothed) does not step.
    const auto change = [] (const char* id, float value) {
        Rig rig (0, false);
        smoothSound (rig, "lower");
        const auto out = heldKeysThen (rig, [&] { rig.set (id, value); });
        return std::make_pair (stepsOf (rig, out), rig.proc.getLowerPart().switchesDone());
    };
    const auto [cutoff, cutoffSwitches] = change ("lower.filter.cutoff", 1500.0f);
    SAY ("cutoff 300 to 1500 by hand: step before %.5f, after %.5f\n", cutoff.before[0], cutoff.after[0]);
    INFO ("cutoff 300 to 1500: step before " << cutoff.before[0] << ", after " << cutoff.after[0]);
    CHECK (stepRuleHolds (cutoff.before[0], cutoff.after[0]));
    CHECK (cutoff.faded[0] == 0);
    CHECK (cutoffSwitches == 0);
    const auto [wave, waveSwitches] = change ("lower.osc1.wave", 3.0f);
    SAY ("waveform Triangle to Pulse by hand: step before %.5f, after %.5f\n", wave.before[0], wave.after[0]);
    INFO ("waveform Triangle to Pulse: step before " << wave.before[0] << ", after " << wave.after[0]);
    CHECK_FALSE (stepRuleHolds (wave.before[0], wave.after[0])); // it steps
    CHECK (wave.faded[0] == 0);                                  // no fade and no switch: the notes keep sounding
    CHECK (waveSwitches == 0);
    CHECK (wave.active[0] == 3);
    CHECK (wave.peakAfterFade[0] > 0.01);
}

TEST_CASE ("Bank: a note played during an apply's fade starts after it with the new program and keeps its length; held keys do not retrigger",
           "[bside][bank][apply]")
{
    // A4 Glass Ring (Poly, Model B: no structural change from Init) applied over three held keys on Init; a new key (72) pressed 40
    // host samples later, inside the 3 ms fade, and released 4800 samples after its note-on. The part is read after every sample.
    Rig rig (0, false);
    smoothSound (rig, "lower");
    std::vector<Midi> midi;
    for (int n : { 48, 55, 64 })
        midi.push_back (noteOn (at (0.05), n));
    rig.render (at (0.6), midi);
    REQUIRE (rig.presets().selectProgram (3));
    const long long sw = rig.now;
    const int onAt = 40, length = 4800;
    const std::vector<Midi> late { noteOn (static_cast<int> (sw) + onAt, 72), noteOff (static_cast<int> (sw) + onAt + length, 72) };
    long long started = -1, released = -1;
    int otherVoices = 0, startDelay = -1, control = -1;
    const auto& lower = rig.proc.getLowerPart();
    rig.render (onAt + length + 400, late, 1, {}, [&] (int) {
        for (int i = 0; i < bs::Part::kVoices; ++i)
        {
            const auto& v = lower.getVoice (i);
            if (! v.active)
                continue;
            if (v.note != 72)
            {
                if (rig.now > sw + at (0.004))
                    ++otherVoices; // a held key back after the fade
                continue;
            }
            if (started < 0)
            {
                started = rig.now - 1; // the sample in which it started
                startDelay = v.startDelay;
                control = v.chain.chip().registerValue (4);
            }
            if (released < 0 && ! v.held)
                released = rig.now - 1;
        }
    });
    const int fadeHost = at (bs::Part::kFadeSeconds);
    INFO ("apply at " << sw << ", note-on at +" << onAt << ", started at +" << (started - sw) << ", released at +" << (released - sw)
                      << ", start delay " << startDelay << " internal samples");
    CHECK (lower.appliesFaded() == 1);
    CHECK (otherVoices == 0);
    REQUIRE (started >= 0);
    REQUIRE (released > started);
    CHECK (started - sw >= fadeHost - 1); // at the fade's end, not before
    CHECK (started - sw <= fadeHost + 1);
    CHECK (startDelay > 0);
    CHECK (std::abs ((released - started) - length) <= 1); // played length = sounded length, to one host sample
    CHECK (control == 0x15);                                // A4's first row: triangle, ring, gate (the new program)
}

TEST_CASE ("Bank: a tone applied to one part fades that part only; the other part's output is bit-identical whatever the tone", "[bside][bank][apply]")
{
    // Dual, both parts sounding, panned apart. A tone applied to Lower (A4's, then A7's): Upper (the right channel) is the same bit
    // for bit in both renders and does not fade; Lower fades.
    const auto withTone = [] (int slot) {
        Rig rig (0, false);
        smoothDual (rig);
        auto out = heldKeysThen (rig, [&] { REQUIRE (rig.presets().applyToneFromSlot (slot, 0, 0).isEmpty()); });
        const auto r = stepsOf (rig, out);
        return std::make_pair (std::move (out.r), r);
    };
    const auto [rightA, a] = withTone (3);
    const auto [rightB, b] = withTone (6);
    CHECK (a.faded[0] == 1);
    CHECK (b.faded[0] == 1);
    CHECK (a.faded[1] == 0);
    CHECK (b.faded[1] == 0);
    CHECK (stepRuleHolds (a.before[0], a.after[0]));
    CHECK (stepRuleHolds (b.before[0], b.after[0]));
    REQUIRE (rightA.size() == rightB.size());
    CHECK (std::memcmp (rightA.data(), rightB.data(), rightA.size() * sizeof (float)) == 0);
    CHECK (a.peakBefore[1] > 0.01);
}

TEST_CASE ("Bank: an apply over held notes does not depend on the host's blocks (1, 61, 512): digital state exact, audio within -120 dB",
           "[bside][bank][apply]")
{
    struct Run
    {
        Stereo out;
        std::vector<std::uint64_t> state;
    };
    const auto run = [] (int block) {
        Rig rig (0, false);
        smoothDual (rig);
        const int sw = at (0.12);
        const std::vector<Midi> midi { noteOn (at (0.01), 48), noteOn (at (0.01), 55), noteOn (at (0.01), 64),
                                       noteOn (sw + 30, 72), noteOff (sw + 30 + 2000, 72) }; // 72 inside the fade
        Run r;
        r.out = rig.render (at (0.2), midi, block, { { sw, [&] { REQUIRE (rig.presets().selectProgram (14)); } } }); // B7: Dual, Upper Model A (a switch too)
        for (const auto* part : { &rig.proc.getLowerPart(), &rig.proc.getUpperPart() })
        {
            r.state.push_back (static_cast<std::uint64_t> (part->getRenderedSamples()));
            r.state.push_back (static_cast<std::uint64_t> (part->appliesFaded()));
            r.state.push_back (static_cast<std::uint64_t> (part->switchesDone()));
            for (int i = 0; i < bs::Part::kVoices; ++i)
            {
                const auto& v = part->getVoice (i);
                r.state.push_back (v.active ? 1u : 0u);
                r.state.push_back (static_cast<std::uint64_t> (v.note));
                r.state.push_back (static_cast<std::uint64_t> (v.startDelay));
                r.state.push_back (v.chain.chip().cycle());
                for (int reg = 0; reg < 0x19; ++reg)
                    r.state.push_back (v.chain.chip().registerValue (static_cast<std::uint8_t> (reg)));
            }
        }
        return r;
    };
    const auto ref = run (512);
    constexpr size_t kPartFields = 3 + bs::Part::kVoices * (4 + 0x19);
    REQUIRE (ref.state.size() == 2 * kPartFields);
    CHECK (ref.state[1] == 1);               // Lower faded for the apply
    CHECK (ref.state[kPartFields + 1] == 1); // and Upper
    for (int block : { 1, 61 })
    {
        const auto r = run (block);
        INFO ("blocks of " << block);
        CHECK (r.state == ref.state);
        double worst = 0.0;
        for (size_t i = 0; i < ref.out.l.size(); ++i)
            worst = std::max ({ worst, std::abs (static_cast<double> (r.out.l[i]) - ref.out.l[i]), std::abs (static_cast<double> (r.out.r[i]) - ref.out.r[i]) });
        INFO ("largest difference " << worst);
        CHECK (worst < 1.0e-6);
    }
}
