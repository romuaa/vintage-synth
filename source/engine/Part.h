#pragma once

#include "engine/Modulation.h"
#include "engine/NoteEvent.h"
#include "engine/Voice.h"
#include "engine/VoiceAllocator.h"
#include "params/PartParamList.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <memory>
#include <span>

namespace vs
{
/**
    One timbre (Lower or Upper): parameters, LFO, note handling, kMaxVoices voices.

    The whole voice path runs at the oversampled rate and is decimated once per part.
    Events are applied sample-accurately: the block is split at each event's offset.
    Between events, voices render in sub-blocks of up to kMaxSubBlock oversampled samples;
    parameter smoothing, the LFO and other shared modulation are computed once per sub-block
    for all voices (PartModulator), then all sounding voices are rendered interleaved.

    Note handling (docs/architecture.md 3.2): the VoiceAllocator decides, the Part executes.
    A stolen voice fades out over kStealFadeSeconds; the sub-block is split at the end of the
    fade, where the voice is reset and the new note starts (so it starts 3 ms late).
*/
class Part
{
public:
    static constexpr float kStealFadeSeconds = 0.003f;

    /**
        Allocates. Call from prepareToPlay only. Voice i is seeded seed + i.
        @param oversamplingFactor 1, 2 or 4. At 1 the voices render straight into the output
                                  (no decimator); the latency is then the BLEP delay only.
        @param realTimeFactor     the factor real-time rendering uses at this rate (0: the same).
                                  When oversamplingFactor is twice that (offline), notes that start
                                  with Cross Mod up render at the real-time rate, so a bounce sounds
                                  like playback (Voice::setRealTimePairRatio); the latency then
                                  includes Voice::kRtPathExtraDelay.
    */
    void prepare (double sampleRate, int maxBlockSize, int oversamplingFactor, std::uint32_t seed, int realTimeFactor = 0);
    void reset() noexcept;

    /**
        Key-mode change (docs/architecture.md 4.2): releases every sounding voice and forgets the
        held keys, like All Notes Off, but keeps the controllers (pitch bend, mod wheel, sustain
        pedal). Envelope release, no click.
    */
    void releaseAllNotes() noexcept;

    /**
        For a part that was not rendered for a while (Upper in Whole mode, its voices idle):
        clears the decimator, jumps the smoothed parameters to the current targets (setParams
        first) and restarts the LFO, so it starts as cleanly as after prepare(). Keeps the
        controllers (pitch bend, mod wheel and pedal, which it kept receiving through zero-length
        renders) and the allocator's voice ages.
    */
    void resume() noexcept;

    void setParams (const PartParams& p) noexcept { target = p; }
    void setVintage (float v) noexcept { vintage = v; }

    /**
        Renders numSamples (<= maxBlockSize) into out (overwrites it). Each event is applied
        at its offset (relative to out[0], ascending). Offsets past the end are applied after
        the last sample, so a note-off is never lost. With numSamples == 0 the events are
        applied (state change only) and nothing is rendered.

        Controller semantics (docs/architecture.md section 2):
        - AllNotesOff (CC 123, and the event-buffer overflow marker) releases every key and
          clears the sustain pedal, so a dropped pedal-up can never hold notes.
        - AllSoundOff (CC 120) does the same and silences the voices immediately.
        - ResetControllers (CC 121) resets pitch bend, mod wheel and the sustain pedal.
    */
    void render (float* out, int numSamples, std::span<const NoteEvent> events) noexcept;

    /** Longest sub-block in oversampled samples, 1..kMaxSubBlock (default kMaxSubBlock). For tests. */
    void setMaxSubBlock (int n) noexcept { maxSubBlock = juce::jlimit (1, kMaxSubBlock, n); }

    int getLatencySamples() const noexcept { return latencySamples; }
    /** The latency before rounding to whole samples (decimator phase delay + BLEP delay). */
    double getExactLatency() const noexcept { return exactLatency; }
    /** Samples rendered since prepare() (zero-length renders don't count). For tests: CPU use. */
    std::int64_t getRenderedSamples() const noexcept { return renderedSamples; }
    int getOversamplingFactor() const noexcept { return osFactor; }
    bool isSounding() const noexcept { return numSoundingVoices() > 0; }
    /** Voices that are sounding or waiting for the end of a steal fade. */
    int numSoundingVoices() const noexcept;

    float getModWheel() const noexcept { return modWheel; }
    bool isSustainDown() const noexcept { return allocator.isPedalDown(); }

    /** Read-only views for tests and diagnostics. */
    const VoiceAllocator& getAllocator() const noexcept { return allocator; }
    const Voice& getVoice (int i) const noexcept { return voices[static_cast<size_t> (i)]; }

private:
    struct Pending
    {
        bool active = false; // a steal fade is running; `action` starts when it ends
        VoiceAction action;
    };

    void renderOversampled (float* os, int numOsSamples) noexcept;
    void afterSubBlock() noexcept;
    void applyBlockParams() noexcept;
    void handleEvent (const NoteEvent& e) noexcept;
    void execute (const VoiceAction& a) noexcept;
    void startVoice (const VoiceAction& a) noexcept;
    void allSoundOff() noexcept;

    auto sink() noexcept
    {
        return [this] (const VoiceAction& a) { execute (a); };
    }

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;

    // Offline with Cross Mod notes at the real-time rate (see prepare): their sum, and the delay
    // line that aligns the ordinary notes with them.
    static constexpr int kAlignRingSize = 16;
    static_assert (Voice::kRtPathExtraDelay < kAlignRingSize, "alignment delay must fit the ring");
    bool rtPathOn = false;
    std::array<float, kMaxSubBlock> rtPathOut {};
    std::array<float, kAlignRingSize> alignRing {};
    unsigned alignPos = 0;
    juce::AudioBuffer<float> silence;
    int maxBlock = 0;
    int osFactor = 2;
    int latencySamples = 0;
    double exactLatency = 0.0;
    std::int64_t renderedSamples = 0;
    float osRate = 96000.0f;
    int stealFadeSamples = 288;

    PartParams target;
    float vintage = 0.0f;
    float bendValue = 0.0f; // last pitch-bend event, -1..1
    float bendRangeSemis = 2.0f;
    int bendRangeIndex = 2;
    float modWheel = 0.0f;

    int maxSubBlock = kMaxSubBlock;
    PartModulator modulator;
    ModBlock modBlock;
    VoiceAllocator allocator;
    std::array<Voice, kMaxVoices> voices;
    std::array<Voice*, kMaxVoices> voicePtrs {};
    std::array<Pending, kMaxVoices> pending {};

    // A stolen note starts kStealFadeSeconds late. Its releases are delayed by the same time
    // (oversampled samples, -1 = none), so it keeps its gate length: a key released during the
    // fade still gives a note as long as the key was held.
    std::array<int, kMaxVoices> releaseCountdown {};
    std::array<bool, kMaxVoices> late {};

    // Unison: one random base phase per note for the group (deterministic sequence).
    dsp::Random unisonRng;
    std::uint32_t partSeed = 0;
    float unisonBase1 = 0.0f, unisonBase2 = 0.0f;
};
} // namespace vs
