#pragma once

// The standard phrases and measurements for B-Side's factory bank (docs/architecture.md 8.21.2, docs/bside-factory-sounds.md):
// which phrase each category is played with, which level group it belongs to, a program played through the real
// preset path on the real BSideProcessor (PresetManager::selectProgram), and the numbers used to check a program
// against its row of the specification (peak, RMS, a spectrum summary, and the chip's registers read back while it
// plays). Shared by the enforcing tests (BankTests.cpp) and the hidden audition render (BankRender.cpp), so both
// measure exactly the same thing. B-Side only: it includes nothing of Vintage, and Vintage's phrases are restated here
// (a copy that is cheap to compare with tests/preset_unit/FactorySound.h).

#include "bside/PluginProcessor.h"
#include "bside/engine/Part.h"
#include "bside/presets/BSidePresets.h"
#include "common/params/GlobalParams.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bsbank
{
inline constexpr double kFs = 48000.0;
inline constexpr int kBlock = 256;
inline constexpr int kVelocity = 100;
inline constexpr int kBankSize = 16; // the programs of the bank; slots 16 .. 63 are empty

/** What is played for a category. */
enum class Phrase
{
    Chord,      // C4 E4 G4 C5 held 3 s (Vintage's Chord phrase): pads, effects, Dual, Init
    Melody,     // eight legato notes G4 .. G5, 0.35 s each: leads
    BassLine,   // eight staccato notes C2 .. C3, 0.3 s apart: basses
    Percussion, // eight hits of C3 (note 48), 0.25 s apart, each held 0.2 s (8.21.2)
    Arpeggio,   // C4 held 4 s: one key plays the chord (8.21.2)
    SplitPhrase // ten staccato bass notes C2 .. C3 below the split point and C4 E4 G4 held 3 s above
};

/** The loudness groups of the level rule (at most three, plus the Init reference; 8.21.2 after the drafting). Sustained and
    bass programs are judged by their phrase's RMS against their group's median RMS; percussion is judged by its phrase PEAK
    against the median phrase peak of the sustained group (a drum hit sits as loud as a lead note). */
enum class Group { Reference, Sustained, Percussive, Bass };

/** The owner's level offsets (8.21.2 after the owner's audition, docs/bside-factory-sounds.md): a program may sit this many dB above its
    group's target, by the owner's ear. The level test holds each program within 2 dB of its group's target plus its offset, and a
    group's target (its median) is taken over the programs with offset 0. Indexed by slot (A1 = 0 ... B8 = 15). */
inline constexpr double kLevelOffsetDb[16] { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,  // A1 .. A8
                                             3.0, 0.0, 5.0, 3.0, 0.0, 0.0, 0.0, 0.0 }; // B1 Short Kick +3, B2, B3 Tick Hat +5, B4 Slow Saw Pad +3, B5 .. B8
inline double levelOffsetDb (int slot) { return slot >= 0 && slot < 16 ? kLevelOffsetDb[slot] : 0.0; }
inline bool hasLevelOffset (int slot) { return levelOffsetDb (slot) > 0.0 || levelOffsetDb (slot) < 0.0; }

/** Whether a group is judged by its phrase peak (against the sustained group's median peak) rather than its RMS. */
inline bool judgedByPeak (Group g) { return g == Group::Percussive; }

inline const char* groupName (Group g)
{
    switch (g)
    {
        case Group::Reference: return "reference";
        case Group::Sustained: return "sustained";
        case Group::Percussive: return "percussive";
        case Group::Bass: return "bass";
    }
    return "?";
}

struct CategoryInfo
{
    bool known = false;
    Phrase phrase = Phrase::Chord;
    Group group = Group::Sustained;
};

/** Every category the bank uses. An unknown category fails the bank test. */
inline CategoryInfo categoryInfo (const juce::String& c)
{
    struct Row
    {
        const char* name;
        Phrase phrase;
        Group group;
    };
    static const Row rows[] {
        { "Init", Phrase::Chord, Group::Reference },          { "Lead", Phrase::Melody, Group::Sustained },
        { "Bass", Phrase::BassLine, Group::Bass },            { "Arpeggio", Phrase::Arpeggio, Group::Sustained }, // one held voice
        { "Percussion", Phrase::Percussion, Group::Percussive }, { "Pad", Phrase::Chord, Group::Sustained },
        { "Sound FX", Phrase::Chord, Group::Sustained },      { "Dual", Phrase::Chord, Group::Sustained },
        { "Split", Phrase::SplitPhrase, Group::Sustained },
    };
    for (const auto& r : rows)
        if (c == r.name)
            return { true, r.phrase, r.group };
    return {};
}

struct Midi
{
    int pos; // host samples
    juce::MidiMessage msg;
};
inline Midi noteOn (int pos, int note, int vel = kVelocity) { return { pos, juce::MidiMessage::noteOn (1, note, static_cast<juce::uint8> (vel)) }; }
inline Midi noteOff (int pos, int note) { return { pos, juce::MidiMessage::noteOff (1, note) }; }

struct Stereo
{
    std::vector<float> l, r;
};

/** A phrase: MIDI at absolute host samples, plus the window the loudness is measured over. */
struct Performance
{
    std::vector<Midi> midi;
    int firstOn = 0;  // first note-on (sample)
    int firstOff = 0; // first note-off
    int lastOff = 0;  // last note-off (start of the release)
};

inline int at (double seconds) { return static_cast<int> (std::lround (seconds * kFs)); }

inline void addNote (Performance& p, double on, double off, int note, int vel)
{
    p.midi.push_back (noteOn (at (on), note, vel));
    p.midi.push_back (noteOff (at (off), note));
}

inline void finish (Performance& p)
{
    p.firstOn = 1 << 30;
    p.firstOff = 1 << 30;
    p.lastOff = 0;
    for (const auto& m : p.midi)
        if (m.msg.isNoteOn())
            p.firstOn = std::min (p.firstOn, m.pos);
        else
        {
            p.firstOff = std::min (p.firstOff, m.pos);
            p.lastOff = std::max (p.lastOff, m.pos);
        }
}

/** The standard phrase of a category (velocity 100). */
inline Performance phrase (Phrase kind, int velocity = kVelocity)
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
            // Legato: each note-off 10 ms after the next note-on.
            const int notes[] { 67, 72, 74, 76, 79, 76, 74, 72 };
            for (int i = 0; i < 8; ++i)
            {
                const double on = 0.05 + 0.35 * i;
                addNote (p, on, i < 7 ? on + 0.36 : on + 0.6, notes[i], velocity);
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
        case Phrase::Percussion:
            for (int i = 0; i < 8; ++i)
                addNote (p, 0.05 + 0.25 * i, 0.05 + 0.25 * i + 0.2, 48, velocity);
            break;
        case Phrase::Arpeggio:
            addNote (p, 0.05, 4.05, 60, velocity);
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

/** The headroom check of percussion (8.21.2 after the drafting): two keys down at full velocity, held 2 s (eight identical
    fixed-pitch hits summing in phase is not a playing situation). */
inline Performance twoKeyChord()
{
    Performance p;
    for (int n : { 48, 60 })
        addNote (p, 0.05, 2.05, n, 127);
    finish (p);
    return p;
}

/** The chord a program's headroom is checked with: two keys for percussion, eight notes for everything else. */
inline Performance headroomChord (Group g) { return judgedByPeak (g) ? twoKeyChord() : fullVelocityChord(); }

inline double dbOf (double v) { return 20.0 * std::log10 (std::max (v, 1.0e-10)); }

/** Octave-wide bands of the spectrum summary: lower edges in Hz (the last band runs to Nyquist). */
inline constexpr double kBandEdges[] { 0.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };
inline constexpr int kBands = 9;

struct Measured
{
    double peakDb = -200.0;     // sample peak of both channels, dBFS
    double rmsDb = -200.0;      // unweighted RMS of both channels from the first note-on to the last note-off, dBFS
    double centroidHz = 0.0;    // magnitude-weighted spectral centroid of the mid signal over the note window
    double bandDb[kBands] {};   // energy per octave band of the mid signal over the note window, dB re the sum of all bands
    bool finite = true;
    double endPeakDb = -200.0;  // peak of the last 100 ms of the render
};

inline Measured measure (const Stereo& s, const Performance& p)
{
    Measured m;
    const size_t n = s.l.size();
    double peak = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double l = s.l[i], r = s.r[i];
        m.finite = m.finite && std::isfinite (l) && std::isfinite (r);
        peak = std::max ({ peak, std::abs (l), std::abs (r) });
    }
    if (! m.finite)
        return m;
    m.peakDb = dbOf (peak);

    const size_t a = static_cast<size_t> (p.firstOn), b = std::min (n, static_cast<size_t> (p.lastOff));
    double acc = 0.0;
    for (size_t i = a; i < b; ++i)
        acc += 0.5 * (static_cast<double> (s.l[i]) * s.l[i] + static_cast<double> (s.r[i]) * s.r[i]);
    m.rmsDb = b > a ? 10.0 * std::log10 (std::max (acc / static_cast<double> (b - a), 1.0e-20)) : -200.0;

    // Hann-windowed 4096-point frames over the note window, power spectra summed into octave bands.
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> frame (2 * size);
    std::vector<double> mag (size / 2 + 1, 0.0), power (size / 2 + 1, 0.0);
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
        {
            mag[static_cast<size_t> (k)] += frame[static_cast<size_t> (k)];
            power[static_cast<size_t> (k)] += static_cast<double> (frame[static_cast<size_t> (k)]) * frame[static_cast<size_t> (k)];
        }
    }
    double num = 0.0, den = 0.0, bands[kBands] {}, total = 0.0;
    for (int k = 1; k <= size / 2; ++k)
    {
        const double hz = k * kFs / size;
        num += mag[static_cast<size_t> (k)] * hz;
        den += mag[static_cast<size_t> (k)];
        int band = kBands - 1;
        while (band > 0 && hz < kBandEdges[band])
            --band;
        bands[band] += power[static_cast<size_t> (k)];
        total += power[static_cast<size_t> (k)];
    }
    m.centroidHz = den > 0.0 ? num / den : 0.0;
    for (int i = 0; i < kBands; ++i)
        m.bandDb[i] = total > 0.0 ? 10.0 * std::log10 (std::max (bands[i] / total, 1.0e-12)) : -120.0;

    const size_t tail = static_cast<size_t> (at (0.1));
    double endPeak = 0.0;
    for (size_t i = n > tail ? n - tail : 0; i < n; ++i)
        endPeak = std::max ({ endPeak, static_cast<double> (std::abs (s.l[i])), static_cast<double> (std::abs (s.r[i])) });
    m.endPeakDb = dbOf (endPeak);
    return m;
}

/** One hit of a percussion phrase: how long after its note-on the sound begins, and how loud it is (8.21.2 after the owner's audition). */
struct HitTiming
{
    double onsetMs = -1.0;  // first sample at or above -30 dB of this hit's peak, minus the hit's note-on (ms); -1: the hit is silent
    double peakDb = -200.0; // this hit's sample peak (both channels), dBFS, from its note-on to the next hit's note-on (or the end)
};

/** The note-ons of a performance in time order. */
inline std::vector<int> noteOnPositions (const Performance& p)
{
    std::vector<int> v;
    for (const auto& m : p.midi)
        if (m.msg.isNoteOn())
            v.push_back (m.pos);
    std::sort (v.begin(), v.end());
    return v;
}

/** The timing and peak of every hit (note-on) of `p` in `s`: the window of a hit runs to the next note-on (the last to the end). */
inline std::vector<HitTiming> hitTimings (const Stereo& s, const Performance& p)
{
    const auto ons = noteOnPositions (p);
    std::vector<HitTiming> out;
    for (size_t h = 0; h < ons.size(); ++h)
    {
        const size_t a = static_cast<size_t> (ons[h]);
        const size_t b = std::min (h + 1 < ons.size() ? static_cast<size_t> (ons[h + 1]) : s.l.size(), s.l.size());
        double peak = 0.0;
        for (size_t i = a; i < b; ++i)
            peak = std::max ({ peak, std::abs (static_cast<double> (s.l[i])), std::abs (static_cast<double> (s.r[i])) });
        HitTiming t;
        t.peakDb = dbOf (peak);
        if (peak > 0.0)
        {
            const double threshold = peak * std::pow (10.0, -30.0 / 20.0);
            for (size_t i = a; i < b; ++i)
                if (std::max (std::abs (static_cast<double> (s.l[i])), std::abs (static_cast<double> (s.r[i]))) >= threshold)
                {
                    t.onsetMs = 1000.0 * static_cast<double> (i - a) / kFs;
                    break;
                }
        }
        out.push_back (t);
    }
    return out;
}

/** The largest sample-to-sample step of x over [from, to). */
inline double maxStep (const std::vector<float>& x, size_t from, size_t to)
{
    double worst = 0.0;
    for (size_t i = std::max<size_t> (from, 1); i < std::min (to, x.size()); ++i)
        worst = std::max (worst, std::abs (static_cast<double> (x[i]) - static_cast<double> (x[i - 1])));
    return worst;
}
inline double peakAbs (const std::vector<float>& x, size_t from, size_t to)
{
    double p = 0.0;
    for (size_t i = from; i < std::min (to, x.size()); ++i)
        p = std::max (p, std::abs (static_cast<double> (x[i])));
    return p;
}

struct TempDir
{
    juce::File dir;
    TempDir() : dir (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("bs_bank_test_" + juce::Uuid().toString())) {}
    ~TempDir() { dir.deleteRecursively(); }
};

/** The real BSideProcessor at 48 kHz in the real-time path, presets pointed at a temporary folder, the audio hand-over
    in its offline mode (nothing runs on another thread), on factory program `slot`. */
struct Rig
{
    juce::ScopedJuceInitialiser_GUI init;
    bs::BSideProcessor proc;
    TempDir temp;
    long long now = 0; // host samples rendered

    /** `slot` < 0: no program is selected (the rig as the constructor leaves it). limiter: the output limiter (a
        session setting, not part of a program) on at its defaults, or off so that levels compare programs and not the
        limiter. */
    explicit Rig (int slot = 0, bool limiter = true, const vs::presets::Program* program = nullptr)
    {
        presets().setLocations ({ temp.dir });
        presets().getGate().setTimeouts (0, 0);
        proc.setNonRealtime (false);
        proc.setPlayConfigDetails (0, 2, kFs, 512);
        proc.prepareToPlay (kFs, 512);
        if (program != nullptr) // a program that is not in the embedded bank (the developer's folder, BankRender.cpp)
            presets().applyProgram (*program, slot, false);
        else if (slot >= 0)
            REQUIRE (presets().selectProgram (slot));
        set ("master.limiter", limiter ? 1.0f : 0.0f);
    }

    vs::presets::PresetManager& presets() { return proc.getPresets(); }
    const vs::presets::Schema& schema() { return proc.getPresets().getSchema(); }

    void set (const juce::String& id, float plain)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
    }
    float get (const juce::String& id) { return proc.getState().getRawParameterValue (id)->load(); }

    /** Renders `numSamples` host samples from `now` in blocks of `blockSize`; midi positions are absolute. A change
        callback runs between blocks at its position (at a block boundary), `afterBlock` after every block. */
    Stereo render (int numSamples, std::vector<Midi> midi, int blockSize = kBlock, const std::map<long long, std::function<void()>>& changes = {},
                   const std::function<void (int)>& afterBlock = {})
    {
        std::stable_sort (midi.begin(), midi.end(), [] (const Midi& a, const Midi& b) { return a.pos < b.pos; });
        const long long begin = now, end = now + numSamples;
        Stereo out;
        out.l.assign (static_cast<size_t> (numSamples), 0.0f);
        out.r.assign (static_cast<size_t> (numSamples), 0.0f);
        juce::AudioBuffer<float> buffer (2, blockSize);
        size_t next = 0;
        while (next < midi.size() && midi[next].pos < begin)
            ++next;
        while (now < end)
        {
            auto change = changes.find (now);
            if (change != changes.end())
                change->second();
            long long blockEnd = std::min<long long> (now + blockSize, end);
            const auto after = changes.upper_bound (now);
            if (after != changes.end())
                blockEnd = std::min (blockEnd, after->first);
            const int len = static_cast<int> (blockEnd - now);
            juce::MidiBuffer mb;
            while (next < midi.size() && midi[next].pos < blockEnd)
            {
                mb.addEvent (midi[next].msg, static_cast<int> (midi[next].pos - now));
                ++next;
            }
            buffer.setSize (2, len, false, false, true);
            buffer.clear();
            proc.processBlock (buffer, mb);
            const auto o = static_cast<std::ptrdiff_t> (now - begin);
            std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + len, out.l.begin() + o);
            std::copy (buffer.getReadPointer (1), buffer.getReadPointer (1) + len, out.r.begin() + o);
            now = blockEnd;
            if (afterBlock)
                afterBlock (len);
        }
        return out;
    }

    /** Renders the performance, then `tailSeconds` after its last note-off. */
    Stereo play (const Performance& p, double tailSeconds) { return render (p.lastOff + at (tailSeconds), p.midi); }
};

/** The bank as the plugin embeds it (a fresh rig's schema; the factory files are parsed against it). */
inline const vs::presets::Bank& factoryBank()
{
    static const vs::presets::Bank bank = [] {
        Rig rig (-1);
        return bs::makeBSideFactoryBank (rig.schema());
    }();
    return bank;
}

/** The phrase for a program: its category's phrase. */
inline Performance phraseFor (const vs::presets::Program& prog) { return phrase (categoryInfo (prog.meta.category).phrase); }

/** WAV, 48 kHz / 24 bit, stereo. */
inline void writeWav (const juce::File& file, const std::vector<float>& l, const std::vector<float>& r)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (kFs).withNumChannels (2).withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, options);
    REQUIRE (writer != nullptr);
    const float* ch[] { l.data(), r.data() };
    REQUIRE (writer->writeFromFloatArrays (ch, 2, static_cast<int> (l.size())));
}

/** VS_RENDER_DIR, or empty: renders are written only when it is set (never into the repository). */
inline juce::File renderDir()
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("VS_RENDER_DIR", {});
    return env.isNotEmpty() ? juce::File (env) : juce::File();
}

// ---- the chip's registers, read back while a program plays ----------------------------------------------------------

/** One sample of a probe: after a block, the registers of the part's most recently started voice. */
struct RegisterSample
{
    double seconds = 0.0;       // host time since the probe started, at the end of the block
    std::uint64_t tick = 0;     // the index of the latest tick the voice has run (0 = the note-on tick); ProgramEngine::tickIndex() counts ticks run
    std::uint64_t cycle = 0;    // the chip's cycle
    bool voiceFound = false;
    std::uint8_t reg[0x19] {};  // registers 0x00 .. 0x18
    std::uint8_t env[3] {};     // the envelope counters (0 .. 255)
    float cutoffSmooth = 0.0f;
    std::uint32_t tickPeriod = 0;
};

inline int osc (const RegisterSample& s, int n, int r) { return s.reg[n * 7 + r]; }
/** Pulse width of oscillator n (0 .. 3) as the register holds it, 0 .. 4095. */
inline int pulseWidth (const RegisterSample& s, int n) { return osc (s, n, 2) | ((osc (s, n, 3) & 0x0f) << 8); }
/** Frequency register F of oscillator n; the output frequency is F x clock / 2^24. */
inline int freqRegister (const RegisterSample& s, int n) { return osc (s, n, 0) | (osc (s, n, 1) << 8); }
inline double freqHz (const RegisterSample& s, int n, double clockHz = 985248.0) { return freqRegister (s, n) * clockHz / 16777216.0; }
inline int control (const RegisterSample& s, int n) { return osc (s, n, 4); }
inline int cutoffRegister (const RegisterSample& s) { return (s.reg[0x15] & 7) | (s.reg[0x16] << 3); }
inline int resonance (const RegisterSample& s) { return s.reg[0x17] >> 4; }
inline int routing (const RegisterSample& s) { return s.reg[0x17] & 7; }
inline int filterMode (const RegisterSample& s) { return (s.reg[0x18] >> 4) & 7; }
inline int chipVolume (const RegisterSample& s) { return s.reg[0x18] & 15; }

/** The most recently started active voice of a part (0 Lower, 1 Upper), or nullptr. */
inline const bs::NoteVoice* newestVoice (const bs::BSideProcessor& proc, int part)
{
    const auto& p = part == 0 ? proc.getLowerPart() : proc.getUpperPart();
    const bs::NoteVoice* best = nullptr;
    for (int i = 0; i < bs::Part::kVoices; ++i)
    {
        const auto& v = p.getVoice (i);
        if (v.active && (best == nullptr || v.startOrder > best->startOrder))
            best = &v;
    }
    return best;
}

inline RegisterSample readRegisters (const bs::BSideProcessor& proc, int part, double seconds)
{
    RegisterSample s;
    s.seconds = seconds;
    if (const auto* v = newestVoice (proc, part))
    {
        s.voiceFound = true;
        s.tick = v->engine.tickIndex() > 0 ? v->engine.tickIndex() - 1 : 0;
        s.tickPeriod = v->engine.tickPeriod();
        const auto& chip = v->chain.chip();
        s.cycle = chip.cycle();
        for (int r = 0; r < 0x19; ++r)
            s.reg[r] = chip.registerValue (static_cast<std::uint8_t> (r));
        s.cutoffSmooth = chip.cutoffSmooth();
        for (int n = 0; n < 3; ++n)
            s.env[n] = chip.envelope (n);
    }
    return s;
}

/** What a probe returns: the registers after every block, and the audio. */
struct Probe
{
    std::vector<RegisterSample> samples;
    Stereo audio;
};

/** Renders `midi` (absolute host samples from the rig's `now`) for `seconds`, reading the registers of the part's newest voice
    after every block of `block` samples (default 1 ms: finer than any tick of the bank); the samples' `seconds` count from the
    rig's position when the call started. */
inline Probe probeMidi (Rig& rig, int part, const std::vector<Midi>& midi, double seconds, int block = 48)
{
    Probe out;
    const long long begin = rig.now;
    out.audio = rig.render (at (seconds), midi, block, {}, [&] (int) {
        out.samples.push_back (readRegisters (rig.proc, part, static_cast<double> (rig.now - begin) / kFs));
    });
    return out;
}

/** Plays `notes` (all pressed together, velocity 100 unless given) on `rig` for `holdSeconds`, then releases them and runs on
    for `tailSeconds`, after a lead-in of `leadInSeconds` that lets the smoothed parameters (5 ms) settle after the program was
    applied (a table that starts a sweep from a host value, a cutoff sweep with no cutoff row, would otherwise start from the
    value still moving). The samples' `seconds` count from the note-on; the audio starts at the lead-in. */
inline Probe probe (Rig& rig, int part, const std::vector<int>& notes, double holdSeconds, double tailSeconds, int velocity = kVelocity, int block = 48,
                    double leadInSeconds = 0.1)
{
    std::vector<Midi> midi;
    const int begin = at (leadInSeconds);
    for (int n : notes)
    {
        midi.push_back (noteOn (begin, n, velocity));
        midi.push_back (noteOff (begin + at (holdSeconds), n));
    }
    auto all = probeMidi (rig, part, midi, leadInSeconds + holdSeconds + tailSeconds, block);
    Probe out;
    out.audio = std::move (all.audio);
    for (auto s : all.samples)
        if (s.seconds > leadInSeconds)
        {
            s.seconds -= leadInSeconds;
            out.samples.push_back (s);
        }
    return out;
}

/** The first sample of each tick 0 .. n-1 (the registers right after the tick ran, to within one block); a tick the probe did not
    see has voiceFound false. */
inline std::vector<RegisterSample> tickSamples (const std::vector<RegisterSample>& s, int ticks)
{
    std::vector<RegisterSample> v (static_cast<size_t> (ticks));
    std::vector<bool> seen (static_cast<size_t> (ticks), false);
    for (const auto& x : s)
        if (x.voiceFound && x.tick < static_cast<std::uint64_t> (ticks) && ! seen[x.tick])
        {
            v[x.tick] = x;
            seen[x.tick] = true;
        }
    return v;
}
} // namespace bsbank
