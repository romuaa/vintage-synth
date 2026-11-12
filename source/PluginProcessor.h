#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "engine/EventParser.h"
#include "engine/KeyRouter.h"
#include "engine/Part.h"
#include "engine/PartMixer.h"
#include "fx/Chorus.h"
#include "fx/Delay.h"
#include "fx/Reverb.h"
#include "params/ParameterLayout.h"

namespace vs
{
class VintageSynthProcessor final : public juce::AudioProcessor
{
public:
    VintageSynthProcessor();
    ~VintageSynthProcessor() override = default;

    // Audio
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    /** Everything back to silence together: parts, router, mixer, effects. */
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    // Editor
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // Info
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    /** Longest tail of the effects that are on (each block's static tailSeconds, capped at 30 s),
        from the parameter values: callable from any thread. 0 with every effect off. */
    double getTailLengthSeconds() const override;

    // Programs (patch memory replaces these in M6)
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Manual"; }
    void changeProgramName (int, const juce::String&) override {}

    // State
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getState() noexcept { return apvts; }

    /** Read-only views for tests and diagnostics (not thread-safe: read between blocks). */
    const Part& getLowerPart() const noexcept { return lowerPart; }
    const Part& getUpperPart() const noexcept { return upperPart; }
    const KeyRouter& getKeyRouter() const noexcept { return router; }
    /** True while the Upper part is rendered (not Whole mode, or its release tail is still running). */
    bool isUpperRendering() const noexcept { return upperActive; }
    int getOversamplingFactor() const noexcept { return lowerPart.getOversamplingFactor(); }
    /** The effect settings applied at the start of the last block. */
    const FxSettings& getAppliedFxSettings() const noexcept { return fxSettings; }

    static constexpr std::uint32_t kLowerSeed = 0x1001u, kUpperSeed = 0x2001u;

private:
    void renderChunk (float* const* out, int numChannels, int start, int len) noexcept;
    void resetEngine() noexcept;

    juce::AudioProcessorValueTreeState apvts;

    PartParamBinding lowerBinding, upperBinding;
    PartParams lowerParams, upperParams;
    Part lowerPart, upperPart;

    std::atomic<float>* masterVolume = nullptr;
    std::atomic<float>* vintage = nullptr;
    std::atomic<float>* keyMode = nullptr;
    std::atomic<float>* splitPoint = nullptr;
    std::atomic<float>* balance = nullptr;
    FxParamBinding fxBinding;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> masterGain;

    EventParser events; // parsed once per render chunk, preallocated in prepareToPlay
    KeyRouter router;
    std::vector<NoteEvent> lowerEvents, upperEvents; // routed, same capacity as the parser
    std::span<const NoteEvent> lowerChunk, upperChunk;
    PartMixer mixer;
    juce::AudioBuffer<float> partBuffer; // 0 = Lower, 1 = Upper (mono, host rate)
    juce::AudioBuffer<float> mixBuffer;  // 0 = left, 1 = right
    int preparedBlockSize = 0;

    // Effects on the stereo bus, after the mixer and before the master gain (docs/architecture.md 6a.1).
    fx::Chorus chorus;
    fx::Delay delay;
    fx::Reverb reverb;
    FxSettings fxSettings;

    // Upper is rendered only while it is needed: not in Whole mode, or until its voices (released
    // by a change to Whole) have been silent for kUpperIdleSamples. Otherwise it costs nothing.
    static constexpr int kUpperIdleSamples = 256;
    bool upperActive = false;
    int upperQuietSamples = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VintageSynthProcessor)
};
} // namespace vs
