#pragma once

#include "dsp/Lfo.h"
#include "engine/Voice.h"
#include "params/PartParamList.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <memory>

namespace vs
{
/**
    One timbre (Lower or Upper): parameters, LFO, note handling, voices.

    The whole voice path runs at the oversampled rate and is decimated once per part.
    MIDI is applied sample-accurately: the block is split at each event's position.

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

    /** Raw MIDI bytes (no MidiMessage construction: SysEx would allocate). Ignores non-channel messages. */
    void handleMidi (const juce::uint8* data, int numBytes) noexcept;

    /**
        Renders numSamples (<= maxBlockSize) into out (overwrites it). MIDI events
        with positions in [startSample, startSample + numSamples) are applied at
        their exact position.
    */
    void render (float* out, int numSamples, const juce::MidiBuffer& midi, int startSample) noexcept;

    int getLatencySamples() const noexcept { return latencySamples; }
    int getOversamplingFactor() const noexcept { return osFactor; }
    bool isSounding() const noexcept { return voice.isActive(); }

private:
    void renderOversampled (float* os, int numOsSamples) noexcept;
    void applyBlockParams() noexcept;
    void noteOn (int note, float velocity) noexcept;
    void noteOff (int note) noexcept;
    void allNotesOff() noexcept;

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::AudioBuffer<float> silence;
    int maxBlock = 0;
    int osFactor = 2;
    int latencySamples = 0;
    float osRate = 96000.0f;

    PartParams target, smoothed;
    float smoothCoef = 1.0f;
    float vintage = 0.0f;
    float bendTarget = 0.0f, bend = 0.0f;
    float bendRangeSemis = 2.0f;

    dsp::Lfo lfo;
    Voice voice;

    std::array<int, 128> held {};
    int numHeld = 0;
};
} // namespace vs
