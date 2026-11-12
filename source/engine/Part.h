#pragma once

#include "engine/Modulation.h"
#include "engine/NoteEvent.h"
#include "engine/Voice.h"
#include "params/PartParamList.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <memory>
#include <span>

namespace vs
{
/**
    One timbre (Lower or Upper): parameters, LFO, note handling, voices.

    The whole voice path runs at the oversampled rate and is decimated once per part.
    Events are applied sample-accurately: the block is split at each event's offset.
    Between events, voices render in sub-blocks of up to kMaxSubBlock oversampled samples;
    parameter smoothing, the LFO and other shared modulation are computed once per sub-block
    for all voices (PartModulator).

    M1: a single voice, last-note priority (legato pitch change, envelopes retrigger
    on every new note). M2 adds polyphony, unison and portamento.
*/
class Part
{
public:
    /** Allocates. Call from prepareToPlay only. */
    void prepare (double sampleRate, int maxBlockSize, int oversamplingFactor, std::uint32_t seed);
    void reset() noexcept;

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
    int getOversamplingFactor() const noexcept { return osFactor; }
    bool isSounding() const noexcept { return voice.isActive(); }

    /** Controller state (M2a stores it; M2b uses it for vibrato and deferred releases). */
    float getModWheel() const noexcept { return modWheel; }
    bool isSustainDown() const noexcept { return sustainDown; }

private:
    void renderOversampled (float* os, int numOsSamples) noexcept;
    void applyBlockParams() noexcept;
    void handleEvent (const NoteEvent& e) noexcept;
    void noteOn (int note, float velocity) noexcept;
    void noteOff (int note) noexcept;
    void allNotesOff() noexcept;
    void resetControllers() noexcept;

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::AudioBuffer<float> silence;
    int maxBlock = 0;
    int osFactor = 2;
    int latencySamples = 0;
    float osRate = 96000.0f;

    PartParams target;
    float vintage = 0.0f;
    float bendRangeSemis = 2.0f;
    float modWheel = 0.0f;
    bool sustainDown = false;

    int maxSubBlock = kMaxSubBlock;
    PartModulator modulator;
    ModBlock modBlock;
    Voice voice;

    std::array<int, 128> held {};
    int numHeld = 0;
};
} // namespace vs
