#pragma once

// The placeholder part of Exitium B-Side's plugin shell (docs/architecture.md 8.3): one sine per
// note, 16 voices (the shared VoiceAllocator, Poly), a 5 ms linear attack and release, host rate,
// no latency. It exists so that the whole chain around a part (key router, arpeggiator/sequencer,
// mixer, effects, limiter, presets, state, bypass) is tested from the first day; S2 and S4 replace
// it with the chip-based part. JUCE-free. Real-time safe: no allocation, locks or exceptions.
//
// Satisfies shell::PartEngine. Sample-accurate: a block is split at the events' offsets, and an
// event at or after the end of the block (offset == n) is applied after the last sample. A render of
// length 0 only applies the events (the skeleton does that for Upper while it is not rendered).

#include "common/engine/NoteEvent.h"
#include "common/engine/VoiceAllocator.h"

#include <array>
#include <span>

namespace bs
{
class TestTonePart
{
public:
    static constexpr int kVoices = vs::kMaxVoices;
    static constexpr double kAttackSeconds = 0.005;
    static constexpr double kReleaseSeconds = 0.005;
    static constexpr float kVoiceGain = 0.3f;        // x velocity, per voice
    static constexpr double kBendRangeSemitones = 2.0;

    /** Sets the host rate and clears everything. May be called again on every prepareToPlay. */
    void prepare (double hostSampleRate) noexcept;

    /** Renders `n` mono samples into out[0..n) (overwritten) after applying `events` at their offsets. */
    void render (float* out, int n, std::span<const vs::NoteEvent> events) noexcept;

    /** Releases every voice (their 5 ms release is heard) and forgets the held keys; the pedal stays. */
    void releaseAllNotes() noexcept;
    /** Silence now; forgets keys, pedal and controllers. */
    void reset() noexcept;
    /** Upper part back from Whole mode: nothing to restart in a test tone. */
    void resume() noexcept {}

    bool isSounding() const noexcept;
    bool isSustainDown() const noexcept { return allocator.isPedalDown(); }
    int getLatencySamples() const noexcept { return 0; }

private:
    enum class Stage : unsigned char
    {
        Idle,
        Attack, // rising to full level; a held voice stays at full level afterwards
        Release
    };

    struct Voice
    {
        Stage stage = Stage::Idle;
        int note = 0;
        float velocity = 0.0f;
        float level = 0.0f; // 0..1 envelope
        double phase = 0.0; // 0..1
        double increment = 0.0;
    };

    void apply (const vs::NoteEvent& e) noexcept;
    void execute (const vs::VoiceAction& a) noexcept;
    void updateIncrements() noexcept;
    void renderSegment (float* out, int len) noexcept;
    double incrementFor (int note) const noexcept;

    std::array<Voice, kVoices> voices {};
    vs::VoiceAllocator allocator;
    double sampleRate = 48000.0;
    float attackStep = 1.0f, releaseStep = 1.0f; // level change per sample
    double bend = 0.0;                           // -1..1
};
} // namespace bs
