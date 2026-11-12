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
#include "seq/ArpSeq.h"

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
    /** Everything back to silence together: parts, router, mixer, arp/seq instances, effects. */
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
    const seq::ArpSeq& getArpSeq (int part) const noexcept { return part == 0 ? lowerArp : upperArp; }
    /** The events part 0 (Lower) / 1 (Upper) received in the last rendered chunk (offsets relative
        to that chunk). With host blocks no longer than the prepared size: the last block. */
    std::span<const NoteEvent> getLastPartEvents (int part) const noexcept { return part == 0 ? lowerChunk : upperChunk; }
    /** The effect settings applied at the start of the last block. */
    const FxSettings& getAppliedFxSettings() const noexcept { return fxSettings; }
    /** The transport the arp/seq got for the first chunk of the last block. */
    const seq::Transport& getLastTransport() const noexcept { return blockTransport; }
    /** Event capacity behind each arp/seq instance (ArpSeq::minimumCapacity for the prepared block). */
    int getArpEventCapacity() const noexcept { return static_cast<int> (lowerArpOut.size()); }

    static constexpr std::uint32_t kLowerSeed = 0x1001u, kUpperSeed = 0x2001u;

private:
    void renderChunk (float* const* out, int numChannels, int start, int len) noexcept;
    void readTransport() noexcept;
    seq::Transport transportAt (int sampleOffset) const noexcept;
    /** Runs one part's arp/seq over its routed chunk; returns the events the part gets. */
    std::span<const NoteEvent> arpChunk (seq::ArpSeq& arp, bool selected, std::span<const NoteEvent> routed,
                                         std::vector<NoteEvent>& out, const seq::Transport& tr, int len) noexcept;
    /** After a key-mode change: the module forgets keys and notes like the part, and is put back
        in step with the part's pedal (see the definition). */
    void resyncArpAfterKeyModeChange (seq::ArpSeq& arp, const Part& part, const seq::Transport& tr) noexcept;
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
    ArpParamBinding arpBinding;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> masterGain;

    EventParser events; // parsed once per render chunk, preallocated in prepareToPlay
    KeyRouter router;
    std::vector<NoteEvent> lowerEvents, upperEvents; // routed, same capacity as the parser
    std::span<const NoteEvent> lowerChunk, upperChunk;
    PartMixer mixer;
    juce::AudioBuffer<float> partBuffer; // 0 = Lower, 1 = Upper (mono, host rate)
    juce::AudioBuffer<float> mixBuffer;  // 0 = left, 1 = right
    int preparedBlockSize = 0;
    double sampleRateHz = 48000.0;

    // Effects on the stereo bus, after the mixer and before the master gain (docs/architecture.md 6a.1).
    fx::Chorus chorus;
    fx::Delay delay;
    fx::Reverb reverb;
    FxSettings fxSettings;

    // One arpeggiator/sequencer in front of each part. Their outputs are what the parts get.
    seq::ArpSeq lowerArp, upperArp;
    ArpSettings arpSettings;
    std::vector<NoteEvent> lowerArpOut, upperArpOut; // ArpSeq::minimumCapacity for the prepared block
    std::vector<NoteEvent> resyncScratch;            // output of the key-mode resync calls (discarded)
    bool playerPedal = false;                        // the sustain pedal as the player holds it (parsed MIDI)

    // Host transport, read once per block (docs/architecture.md 6a.1). Missing fields = stopped.
    bool hostPlaying = false;
    double hostBpm = 120.0, hostPpq = 0.0;
    seq::Transport blockTransport { 48000.0, 120.0, 0.0, false };

    // Upper is rendered only while it is needed: not in Whole mode, or until its voices (released
    // by a change to Whole) have been silent for kUpperIdleSamples. Otherwise it costs nothing.
    static constexpr int kUpperIdleSamples = 256;
    bool upperActive = false;
    int upperQuietSamples = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VintageSynthProcessor)
};
} // namespace vs
