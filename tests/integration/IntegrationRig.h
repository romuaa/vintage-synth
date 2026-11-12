#pragma once

// Test rig for the Wave 3 integration tests (docs/architecture.md 6a.3): the real processor,
// driven like a host, with a test play head for the transport.
//
// - Positions are in host samples. Blocks are `blockSize` long and are also split at every
//   change (parameter or transport), so a change lands on the same sample for any block size
//   (the host-side behaviour of a sample-accurate automation lane at block granularity).
// - The host transport is a piecewise-linear musical timeline: play (bpm, ppq at a sample),
//   tempo changes that keep the position continuous, locate (loop jump) and stop. At the start of
//   every block the play head reports the position of that block's first sample, computed from the
//   timeline (never accumulated per block), so every block size sees the same musical time.
// - With blocks no longer than the prepared size, the events every part received are recorded at
//   absolute positions (`partEvents`), from the processor's per-chunk view.
// - Render buffers are bounds-checked (a test that overran its buffer corrupted the heap on
//   Windows in M2a).

#include <catch2/catch_test_macros.hpp>

#include "PluginProcessor.h"
#include "engine/NoteEvent.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

namespace vs::itest
{
struct Midi
{
    int pos; // host samples
    juce::MidiMessage msg;
};

inline Midi noteOn (int pos, int note, int vel = 100) { return { pos, juce::MidiMessage::noteOn (1, note, static_cast<juce::uint8> (vel)) }; }
inline Midi noteOff (int pos, int note) { return { pos, juce::MidiMessage::noteOff (1, note) }; }
inline Midi cc (int pos, int controller, int value) { return { pos, juce::MidiMessage::controllerEvent (1, controller, value) }; }
inline Midi pedal (int pos, bool down) { return cc (pos, 64, down ? 127 : 0); }

struct Stereo
{
    std::vector<float> l, r;
};

/** The play head a host would give: every field optional. */
struct TestPlayHead final : public juce::AudioPlayHead
{
    bool hasPosition = true;
    bool playing = false;
    std::optional<double> bpm, ppq;

    juce::Optional<PositionInfo> getPosition() const override
    {
        if (! hasPosition)
            return {};
        PositionInfo info;
        info.setIsPlaying (playing);
        if (bpm.has_value())
            info.setBpm (*bpm);
        if (ppq.has_value())
            info.setPpqPosition (*ppq);
        return info;
    }
};

/** Host transport timeline (what the host's play head reports). */
struct HostTransport
{
    bool attached = true;   // false: the processor has no play head at all
    bool hasPosition = true;
    bool reportBpm = true;  // false: the position has no tempo field
    bool reportPpq = true;  // false: the position has no musical position
    bool playing = false;
    double bpm = 120.0;
    double anchorPpq = 0.0;     // position at sample `anchor`
    long long anchor = 0;

    double ppqAt (long long sample, double fs) const
    {
        return anchorPpq + static_cast<double> (sample - anchor) * bpm / (60.0 * fs);
    }
};

struct AbsEvent
{
    long long pos; // absolute host sample (block start + offset)
    NoteEvent e;
};

struct Rig
{
    juce::ScopedJuceInitialiser_GUI init;
    VintageSynthProcessor proc;
    TestPlayHead playHead;
    HostTransport transport;
    double fs;
    int maxBlock;
    int channels;
    long long now = 0; // host samples rendered so far
    std::vector<AbsEvent> partEvents[2];
    bool eventsRecorded = true; // false once a block was longer than the prepared size
    KeyMode lastKeyMode = KeyMode::Whole;

    explicit Rig (double rate = 48000.0, int maxBlockSize = 512, int numChannels = 2)
        : fs (rate), maxBlock (maxBlockSize), channels (numChannels)
    {
    }

    int samples (double seconds) const { return static_cast<int> (std::lround (seconds * fs)); }

    void set (const juce::String& id, float plain)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
    }

    float get (const juce::String& id) { return proc.getState().getRawParameterValue (id)->load(); }

    void prepare()
    {
        juce::AudioProcessor::BusesLayout layout;
        layout.outputBuses.add (channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
        REQUIRE (proc.setBusesLayout (layout));
        proc.setNonRealtime (false);
        proc.setPlayConfigDetails (0, channels, fs, maxBlock);
        proc.prepareToPlay (fs, maxBlock);
        lastKeyMode = proc.getKeyRouter().getMode();
    }

    // ---- Transport (call from a Change, i.e. at a block boundary) ----
    void play (double bpm, double ppq)
    {
        transport.playing = true;
        transport.bpm = bpm;
        transport.anchorPpq = ppq;
        transport.anchor = now;
    }
    void stop() { transport.playing = false; }
    void tempo (double bpm)
    {
        transport.anchorPpq = transport.ppqAt (now, fs);
        transport.anchor = now;
        transport.bpm = bpm;
    }
    void locate (double ppq)
    {
        transport.anchorPpq = ppq;
        transport.anchor = now;
    }

    struct Change
    {
        int at; // host samples (absolute)
        std::function<void (Rig&)> apply;
    };

    void updatePlayHead()
    {
        proc.setPlayHead (transport.attached ? &playHead : nullptr);
        playHead.hasPosition = transport.hasPosition;
        playHead.playing = transport.playing;
        playHead.bpm.reset();
        playHead.ppq.reset();
        if (transport.reportBpm)
            playHead.bpm = transport.bpm;
        if (transport.reportPpq)
            playHead.ppq = transport.playing ? transport.ppqAt (now, fs) : 0.0;
    }

    /**
        Renders numSamples host samples from the current position (`now`), in blocks of blockSize
        (may exceed the prepared size: then the processor renders in chunks), split at changes.
        MIDI and change positions are absolute. afterBlock (rig) runs after every block.
    */
    Stereo render (int numSamples, std::vector<Midi> midi, int blockSize, std::vector<Change> changes = {},
                   const std::function<void (Rig&)>& afterBlock = {})
    {
        REQUIRE (blockSize >= 1);
        std::stable_sort (midi.begin(), midi.end(), [] (const Midi& a, const Midi& b) { return a.pos < b.pos; });
        std::stable_sort (changes.begin(), changes.end(), [] (const Change& a, const Change& b) { return a.at < b.at; });
        const long long begin = now, end = now + numSamples;

        Stereo out;
        out.l.assign (static_cast<size_t> (numSamples), 0.0f);
        out.r.assign (static_cast<size_t> (numSamples), 0.0f);
        juce::AudioBuffer<float> buffer (channels, blockSize);
        size_t nextMidi = 0, nextChange = 0;
        while (nextMidi < midi.size() && midi[nextMidi].pos < begin)
            ++nextMidi; // before this render: not delivered
        while (now < end)
        {
            while (nextChange < changes.size() && changes[nextChange].at <= now)
                changes[nextChange++].apply (*this);
            long long blockEnd = std::min<long long> (now + blockSize, end);
            if (nextChange < changes.size())
                blockEnd = std::min<long long> (blockEnd, changes[nextChange].at);
            const int len = static_cast<int> (blockEnd - now);
            REQUIRE (len >= 1);
            REQUIRE (static_cast<size_t> (blockEnd - begin) <= out.l.size());

            juce::MidiBuffer mb;
            while (nextMidi < midi.size() && midi[nextMidi].pos < blockEnd)
            {
                mb.addEvent (midi[nextMidi].msg, static_cast<int> (midi[nextMidi].pos - now));
                ++nextMidi;
            }
            updatePlayHead();
            buffer.setSize (channels, len, false, false, true);
            buffer.clear();
            proc.processBlock (buffer, mb);

            const auto at = static_cast<size_t> (now - begin);
            std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + len, out.l.begin() + static_cast<std::ptrdiff_t> (at));
            const int rch = channels > 1 ? 1 : 0;
            std::copy (buffer.getReadPointer (rch), buffer.getReadPointer (rch) + len, out.r.begin() + static_cast<std::ptrdiff_t> (at));

            // A key-mode change releases every voice of both parts without events (section 4.2):
            // mark it in the event record, so the note-on/note-off ledger (allReleased) knows.
            if (proc.getKeyRouter().getMode() != lastKeyMode)
            {
                lastKeyMode = proc.getKeyRouter().getMode();
                for (auto& pe : partEvents)
                    pe.push_back ({ now, NoteEvent { 0, EventType::AllNotesOff, 0, 0.0f } });
            }
            if (len <= maxBlock)
            {
                for (int p = 0; p < 2; ++p)
                    for (const auto& e : proc.getLastPartEvents (p))
                        partEvents[p].push_back ({ now + e.offset, e });
            }
            else
                eventsRecorded = false;

            now = blockEnd;
            if (afterBlock)
                afterBlock (*this);
        }
        while (nextChange < changes.size() && changes[nextChange].at <= now)
            changes[nextChange++].apply (*this); // changes at the very end
        return out;
    }

    /** Positions of the note-ons part p received in [from, to). */
    std::vector<long long> noteOnPositions (int p, long long from = 0, long long to = (1LL << 62)) const
    {
        std::vector<long long> v;
        for (const auto& a : partEvents[p])
            if (a.e.type == EventType::NoteOn && a.pos >= from && a.pos < to)
                v.push_back (a.pos);
        return v;
    }

    int countType (int p, EventType t, long long from = 0, long long to = (1LL << 62)) const
    {
        int n = 0;
        for (const auto& a : partEvents[p])
            n += a.e.type == t && a.pos >= from && a.pos < to ? 1 : 0;
        return n;
    }

    /** Note-ons minus note-offs per pitch, ignoring note-offs of pitches not sounding (a part
        ignores those) and clearing on panics. True if every note-on the part got was released. */
    bool allReleased (int p) const
    {
        int open[128] {};
        for (const auto& a : partEvents[p])
        {
            if (a.e.type == EventType::NoteOn)
                ++open[a.e.note];
            else if (a.e.type == EventType::NoteOff)
                open[a.e.note] = 0;
            else if (a.e.type == EventType::AllNotesOff || a.e.type == EventType::AllSoundOff)
                std::fill (std::begin (open), std::end (open), 0);
        }
        return std::all_of (std::begin (open), std::end (open), [] (int n) { return n == 0; });
    }
};

/** Exact sample of a musical position on a host timeline that plays from `startSample` at
    `startPpq` with a constant tempo: the first sample at or after it (independent model: long
    double, no tolerance tricks beyond 1e-9 samples). */
inline long long expectedSample (double ppq, long long startSample, double startPpq, double bpm, double fs)
{
    const long double spp = 60.0L * static_cast<long double> (fs) / static_cast<long double> (bpm);
    const long double x = (static_cast<long double> (ppq) - static_cast<long double> (startPpq)) * spp;
    return startSample + static_cast<long long> (std::ceil (x - 1.0e-9L));
}

inline double rms (const std::vector<float>& x, size_t from, size_t to)
{
    double acc = 0.0;
    to = std::min (to, x.size());
    for (size_t i = from; i < to; ++i)
        acc += static_cast<double> (x[i]) * static_cast<double> (x[i]);
    return to > from ? std::sqrt (acc / static_cast<double> (to - from)) : 0.0;
}

inline double toDb (double v) { return 20.0 * std::log10 (std::max (v, 1.0e-15)); }

/** Level of (a - b) relative to the level of a, in dB (both channels). */
inline double diffDb (const Stereo& a, const Stereo& b)
{
    REQUIRE (a.l.size() == b.l.size());
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < a.l.size(); ++i)
    {
        const double dl = static_cast<double> (a.l[i]) - static_cast<double> (b.l[i]);
        const double dr = static_cast<double> (a.r[i]) - static_cast<double> (b.r[i]);
        num += dl * dl + dr * dr;
        den += static_cast<double> (a.l[i]) * static_cast<double> (a.l[i]) + static_cast<double> (a.r[i]) * static_cast<double> (a.r[i]);
    }
    return 10.0 * std::log10 (std::max (num, 1.0e-30) / std::max (den, 1.0e-30));
}

inline bool sameBits (const std::vector<float>& a, const std::vector<float>& b)
{
    // memcmp of an empty vector would pass a null pointer (undefined behaviour, UBSan reports it).
    return a.size() == b.size() && (a.empty() || std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0);
}

inline bool bitIdentical (const Stereo& a, const Stereo& b) { return sameBits (a.l, b.l) && sameBits (a.r, b.r); }

inline double peakAbs (const std::vector<float>& x, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = from; i < std::min (to, x.size()); ++i)
        m = std::max (m, static_cast<double> (std::abs (x[i])));
    return m;
}
} // namespace vs::itest
