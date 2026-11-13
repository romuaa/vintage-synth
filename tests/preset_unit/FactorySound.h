#pragma once

// The standard phrases and measurements for the factory bank (M6b, docs/factory-sounds.md): which
// phrase each category is played with, which level group it belongs to, rendering a program
// through the real preset path (PresetManager::selectProgram on the processor), and the numbers
// used to check a program against its intent (peak, RMS, attack, release tail, spectral centroid,
// stereo width). Shared by the enforcing tests (FactoryBankTests.cpp) and the hidden audition
// render (FactoryRender.cpp), so both measure exactly the same thing.

#include "PresetTestHelpers.h"
#include "vintage/presets/VintagePresets.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace vs::ftest
{
using namespace vs::ptest;

inline constexpr double kFs = 48000.0;
inline constexpr int kBlock = 256;
inline constexpr int kTypicalVelocity = 100;

/** What is played for a category (docs/factory-sounds.md, "Standard phrases"). */
enum class Phrase
{
    Chord,      // four-note chord held 3 s: pads, strings, brass, organ, Dual, effects
    Melody,     // legato single-note line: leads
    BassLine,   // staccato line in the bass register
    Broken,     // broken chord then a short chord: keys, plucks, bells, percussion
    HeldChord,  // three-note chord held 4 s: arpeggiator
    HeldNote,   // one key held 4 s: step sequencer (transposed by the lowest key)
    SplitPhrase // bass line below the split point and a held chord above it
};

/** The loudness groups of the level rule (at most three, plus the Init reference). */
enum class LevelGroup
{
    Reference,
    Sustained,
    Percussive,
    Bass
};

inline const char* groupName (LevelGroup g)
{
    switch (g)
    {
        case LevelGroup::Reference: return "reference";
        case LevelGroup::Sustained: return "sustained";
        case LevelGroup::Percussive: return "percussive";
        case LevelGroup::Bass: return "bass";
    }
    return "?";
}

struct CategoryInfo
{
    bool known = false;
    Phrase phrase = Phrase::Chord;
    LevelGroup group = LevelGroup::Sustained;
};

/** Every category the factory bank uses. An unknown category fails the bank test. */
inline CategoryInfo categoryInfo (const juce::String& c)
{
    using P = Phrase;
    using G = LevelGroup;
    struct Row
    {
        const char* name;
        P phrase;
        G group;
    };
    static const Row rows[] {
        { "Init", P::Chord, G::Reference },        { "Brass", P::Chord, G::Sustained },
        { "Strings", P::Chord, G::Sustained },     { "Pad", P::Chord, G::Sustained },
        { "Organ", P::Chord, G::Sustained },       { "Sound FX", P::Chord, G::Sustained },
        { "Dual", P::Chord, G::Sustained },        { "Showcase", P::Chord, G::Sustained },
        { "Lead", P::Melody, G::Sustained },       { "Split", P::SplitPhrase, G::Sustained },
        { "Keys", P::Broken, G::Percussive },      { "Pluck", P::Broken, G::Percussive },
        { "Bell", P::Broken, G::Percussive },      { "Percussion", P::Broken, G::Percussive },
        { "Arpeggio", P::HeldChord, G::Percussive }, { "Sequence", P::HeldNote, G::Percussive },
        { "Bass", P::BassLine, G::Bass },
    };
    for (const auto& r : rows)
        if (c == r.name)
            return { true, r.phrase, r.group };
    return {};
}

/** A phrase: MIDI at absolute host samples, plus the window the loudness is measured over. */
struct Performance
{
    std::vector<Midi> midi;
    int firstOn = 0;   // first note-on (sample)
    int firstOff = 0;  // first note-off (end of the first note's attack window)
    int lastOff = 0;   // last note-off (start of the release)
    int maxNote = 0, minNote = 127;
};

inline int at (double seconds) { return static_cast<int> (std::lround (seconds * kFs)); }

inline void addNote (Performance& p, double on, double off, int note, int vel)
{
    p.midi.push_back (noteOn (at (on), note, vel));
    p.midi.push_back (noteOff (at (off), note));
    p.maxNote = std::max (p.maxNote, note);
    p.minNote = std::min (p.minNote, note);
}

inline void finish (Performance& p)
{
    p.firstOn = 1 << 30;
    p.firstOff = 1 << 30;
    p.lastOff = 0;
    for (const auto& m : p.midi)
    {
        if (m.msg.isNoteOn())
            p.firstOn = std::min (p.firstOn, m.pos);
        else
        {
            p.firstOff = std::min (p.firstOff, m.pos);
            p.lastOff = std::max (p.lastOff, m.pos);
        }
    }
}

/** The standard phrase of a category (velocity 100). */
inline Performance phrase (Phrase kind, int velocity = kTypicalVelocity)
{
    Performance p;
    switch (kind)
    {
        case Phrase::Chord:
            for (int n : { 60, 64, 67, 72 })
                addNote (p, 0.05, 3.05, n, velocity);
            break;
        case Phrase::Melody:
        {
            // Legato: each note-off 10 ms after the next note-on (glides in Legato portamento).
            const int notes[] { 67, 72, 74, 76, 79, 76, 74, 72 };
            for (int i = 0; i < 8; ++i)
            {
                const double on = 0.05 + 0.35 * i;
                const double off = i < 7 ? on + 0.36 : on + 0.6;
                addNote (p, on, off, notes[i], velocity);
            }
            break;
        }
        case Phrase::BassLine:
        {
            const int notes[] { 36, 36, 48, 36, 43, 41, 39, 43 };
            for (int i = 0; i < 8; ++i)
                addNote (p, 0.05 + 0.3 * i, 0.05 + 0.3 * i + 0.22, notes[i], velocity);
            break;
        }
        case Phrase::Broken:
        {
            const int notes[] { 60, 64, 67, 72, 76, 72, 67, 64 };
            for (int i = 0; i < 8; ++i)
                addNote (p, 0.05 + 0.25 * i, 0.05 + 0.25 * i + 0.2, notes[i], velocity);
            for (int n : { 60, 64, 67, 72 })
                addNote (p, 2.05, 3.05, n, velocity);
            break;
        }
        case Phrase::HeldChord:
            for (int n : { 60, 64, 67 })
                addNote (p, 0.05, 4.05, n, velocity);
            break;
        case Phrase::HeldNote:
            addNote (p, 0.05, 4.05, 48, velocity);
            break;
        case Phrase::SplitPhrase:
        {
            const int notes[] { 36, 36, 43, 36, 41, 43, 39, 41, 36, 48 };
            for (int i = 0; i < 10; ++i)
                addNote (p, 0.05 + 0.3 * i, 0.05 + 0.3 * i + 0.22, notes[i], velocity);
            for (int n : { 60, 64, 67 })
                addNote (p, 0.05, 3.05, n, velocity);
            break;
        }
    }
    finish (p);
    return p;
}

/** The headroom check: an eight-note chord at full velocity, held 2 s. */
inline Performance fullVelocityChord()
{
    Performance p;
    for (int n : { 48, 52, 55, 60, 64, 67, 72, 76 })
        addNote (p, 0.05, 2.05, n, 127);
    finish (p);
    return p;
}

struct Measured
{
    double peakDb = -200.0;      // sample peak, both channels, dBFS
    double rmsDb = -200.0;       // unweighted RMS of both channels from the first note-on to the last note-off, dBFS
    double attackMs = 0.0;       // first note-on until the 5 ms RMS envelope first reaches 90 % of its maximum before the first note-off
    double releaseMs = 0.0;      // last note-off until the envelope stays below -60 dB re its maximum (0 if the render ends first: see tailCut)
    bool tailCut = false;        // the render ended before the -60 dB point
    double centroidHz = 0.0;     // magnitude-weighted spectral centroid of the mid signal over the note window
    double widthDb = -99.0;      // side / mid energy over the whole render, dB (-99 = mono)
    bool finite = true;
    double endPeakDb = -200.0;   // peak of the last 100 ms of the render
};

inline double dbOf (double v) { return 20.0 * std::log10 (std::max (v, 1.0e-10)); }

inline Measured measure (const Stereo& s, const Performance& p)
{
    Measured m;
    const size_t n = s.l.size();
    double peak = 0.0, side = 0.0, mid = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double l = s.l[i], r = s.r[i];
        m.finite = m.finite && std::isfinite (l) && std::isfinite (r);
        peak = std::max ({ peak, std::abs (l), std::abs (r) });
        mid += 0.25 * (l + r) * (l + r);
        side += 0.25 * (l - r) * (l - r);
    }
    if (! m.finite)
        return m;
    m.peakDb = dbOf (peak);
    m.widthDb = side > 1.0e-20 && mid > 1.0e-20 ? 10.0 * std::log10 (side / mid) : -99.0;

    const size_t a = static_cast<size_t> (p.firstOn), b = std::min (n, static_cast<size_t> (p.lastOff));
    double acc = 0.0;
    for (size_t i = a; i < b; ++i)
        acc += 0.5 * (static_cast<double> (s.l[i]) * s.l[i] + static_cast<double> (s.r[i]) * s.r[i]);
    m.rmsDb = b > a ? 10.0 * std::log10 (std::max (acc / static_cast<double> (b - a), 1.0e-20)) : -200.0;

    // 5 ms RMS envelope.
    const size_t w = static_cast<size_t> (at (0.005));
    std::vector<double> env;
    for (size_t i = 0; i + w <= n; i += w)
    {
        double e = 0.0;
        for (size_t k = i; k < i + w; ++k)
            e += 0.5 * (static_cast<double> (s.l[k]) * s.l[k] + static_cast<double> (s.r[k]) * s.r[k]);
        env.push_back (std::sqrt (e / static_cast<double> (w)));
    }
    const size_t onW = a / w, offW = std::min (env.size(), static_cast<size_t> (p.firstOff) / w + 1);
    double envMax = 0.0;
    for (size_t i = onW; i < offW; ++i)
        envMax = std::max (envMax, env[i]);
    for (size_t i = onW; i < offW; ++i)
        if (env[i] >= 0.9 * envMax)
        {
            m.attackMs = 1000.0 * (static_cast<double> ((i + 1) * w) - static_cast<double> (a)) / kFs;
            break;
        }
    double allMax = 0.0;
    for (double e : env)
        allMax = std::max (allMax, e);
    const size_t relW = static_cast<size_t> (p.lastOff) / w;
    size_t lastLoud = relW;
    for (size_t i = relW; i < env.size(); ++i)
        if (env[i] > allMax * 1.0e-3)
            lastLoud = i;
    m.releaseMs = 1000.0 * static_cast<double> ((lastLoud + 1) * w - static_cast<size_t> (p.lastOff)) / kFs;
    m.tailCut = lastLoud + 1 >= env.size() && env.size() > relW;

    // Centroid: Hann-windowed 4096-point frames over the note window, magnitude spectra summed.
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> frame (2 * size);
    std::vector<double> mag (size / 2 + 1, 0.0);
    for (size_t start = a; start + size <= b; start += size / 2)
    {
        for (int k = 0; k < size; ++k)
        {
            const double hann = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * k / (size - 1));
            frame[static_cast<size_t> (k)] = static_cast<float> (0.5 * (s.l[start + static_cast<size_t> (k)] + s.r[start + static_cast<size_t> (k)]) * hann);
        }
        std::fill (frame.begin() + size, frame.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (frame.data());
        for (int k = 0; k <= size / 2; ++k)
            mag[static_cast<size_t> (k)] += frame[static_cast<size_t> (k)];
    }
    double num = 0.0, den = 0.0;
    for (int k = 1; k <= size / 2; ++k)
    {
        num += mag[static_cast<size_t> (k)] * k * kFs / size;
        den += mag[static_cast<size_t> (k)];
    }
    m.centroidHz = den > 0.0 ? num / den : 0.0;

    const size_t tail = static_cast<size_t> (at (0.1));
    double endPeak = 0.0;
    for (size_t i = n > tail ? n - tail : 0; i < n; ++i)
        endPeak = std::max ({ endPeak, static_cast<double> (std::abs (s.l[i])), static_cast<double> (std::abs (s.r[i])) });
    m.endPeakDb = dbOf (endPeak);
    return m;
}

/** A fresh processor (48 kHz, real-time path, default master volume) on factory program `slot`. */
struct FactoryRig : PresetRig
{
    explicit FactoryRig (int slot) : PresetRig (kFs, 512, 2)
    {
        prepare();
        REQUIRE (presets().selectProgram (slot));
    }

    /** Renders the performance, then `tailSeconds` after its last note-off. */
    Stereo play (const Performance& p, double tailSeconds)
    {
        const int len = p.lastOff + at (tailSeconds);
        return render (len, p.midi, kBlock);
    }
};

inline const presets::Bank& factoryBank()
{
    static const presets::Bank bank = [] {
        FactoryRig rig (0);
        return makeVintageFactoryBank (rig.schema());
    }();
    return bank;
}

/** The phrase for a program: its category's phrase, except that a chord category played by a mono
    Lower part (Unison or Solo, Whole mode) gets the melody, as a lead does. */
inline Phrase phraseKindFor (const presets::Program& prog, const presets::Schema& schema)
{
    auto kind = categoryInfo (prog.meta.category).phrase;
    const auto value = [&] (const char* id) { return prog.values[static_cast<size_t> (schema.indexOf (id))]; };
    if (kind == Phrase::Chord && value ("key.mode") < 0.5f && value ("lower.assign.mode") > 0.5f)
        kind = Phrase::Melody;
    return kind;
}

inline Performance phraseFor (const presets::Program& prog, const presets::Schema& schema)
{
    return phrase (phraseKindFor (prog, schema));
}
} // namespace vs::ftest
