#pragma once

// The arpeggiator preview (docs/architecture.md 9.3.5, 9.4): what the engine will play for a set of
// keys, computed by running the engine's own `seq::ArpSeq` offline. There is no second
// implementation of the pattern logic: the settings go into real instances, the chord goes in as
// note-ons at offset 0, the transport plays at 120 bpm from position 0 at 48 kHz, and the output
// events are collected in chunks of 4096 samples into a preallocated buffer, exactly as the
// processor feeds its parts. JUCE-free. Message thread (it allocates; it runs only when an input
// changed).

#include "common/engine/NoteEvent.h"
#include "common/seq/ArpSeq.h"

#include <algorithm>
#include <array>
#include <vector>

namespace vs::ui::model
{
/** One note the arpeggiator plays, in beats (quarter notes) from the start of the preview. */
struct PreviewNote
{
    int part = 0; // 0 Lower, 1 Upper
    int note = 0;
    float velocity = 0.8f;
    double startBeat = 0.0;
    double lengthBeats = 0.0;
};

struct ArpPreviewInput
{
    seq::Settings settings;                 // the settings the engine uses (mode is forced to Arp here)
    bool partOn[2] { true, false };         // the parts the arp/seq sits in front of
    std::vector<int> keys[2];               // keys of each part in press order
    float velocity = 0.8f;
};

class ArpPreviewModel
{
public:
    static constexpr double kSampleRate = 48000.0, kBpm = 120.0;
    static constexpr int kChunk = 4096;
    static constexpr double kMinBeats = 4.0, kMinSteps = 8.0;

    /** The window: max (4 beats, 8 steps of the rate). */
    static double windowBeats (seq::Rate rate) noexcept
    {
        return std::max (kMinBeats, kMinSteps / static_cast<double> (seq::stepsPerQuarter (rate)));
    }

    /** Runs the engine; replaces `notes()`. */
    void compute (const ArpPreviewInput& in)
    {
        notes_.clear();
        beats_ = windowBeats (in.settings.rate);
        const double samplesPerBeat = kSampleRate * 60.0 / kBpm;
        const auto total = static_cast<long long> (beats_ * samplesPerBeat);

        for (int part = 0; part < 2; ++part)
        {
            if (! in.partOn[part] || in.keys[part].empty())
                continue;
            seq::Settings s = in.settings;
            s.mode = seq::Mode::Arp;
            arp_.reset();
            arp_.setSettings (s);

            // Held note-ons, all at offset 0 of the first chunk.
            std::array<NoteEvent, 128> keyEvents;
            int nKeys = 0;
            for (int k : in.keys[part])
                if (k >= 0 && k < 128 && nKeys < static_cast<int> (keyEvents.size()))
                    keyEvents[static_cast<size_t> (nKeys++)] = NoteEvent { 0, EventType::NoteOn, static_cast<std::uint8_t> (k), in.velocity };

            std::array<int, 128> open;
            open.fill (-1);
            for (long long start = 0; start < total; start += kChunk)
            {
                const int len = static_cast<int> (std::min<long long> (kChunk, total - start));
                const seq::Transport tr { kSampleRate, kBpm, static_cast<double> (start) / samplesPerBeat, true };
                const std::span<const NoteEvent> chunkIn { keyEvents.data(), static_cast<size_t> (start == 0 ? nKeys : 0) };
                const int n = arp_.process (chunkIn, tr, len, out_);
                for (int i = 0; i < n; ++i)
                {
                    const auto& e = out_[static_cast<size_t> (i)];
                    const double beat = static_cast<double> (start + e.offset) / samplesPerBeat;
                    if (e.type == EventType::NoteOn)
                    {
                        notes_.push_back ({ part, e.note, e.value, beat, beats_ - beat });
                        open[e.note] = static_cast<int> (notes_.size()) - 1;
                    }
                    else if (e.type == EventType::NoteOff && open[e.note] >= 0)
                    {
                        auto& n0 = notes_[static_cast<size_t> (open[e.note])];
                        n0.lengthBeats = std::max (0.0, beat - n0.startBeat);
                        open[e.note] = -1;
                    }
                }
            }
        }
    }

    const std::vector<PreviewNote>& notes() const noexcept { return notes_; }
    double beats() const noexcept { return beats_; }

private:
    seq::ArpSeq arp_;
    std::array<NoteEvent, seq::ArpSeq::kRecommendedCapacity> out_ {}; // preallocated (a member, never grown)
    std::vector<PreviewNote> notes_;
    double beats_ = kMinBeats;
};
} // namespace vs::ui::model
