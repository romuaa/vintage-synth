#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "common/engine/EventParser.h"
#include "common/engine/KeyRouter.h"
#include "vintage/engine/Part.h"
#include "common/engine/PartMixer.h"
#include "common/presets/ApplyGate.h"
#include "common/presets/PresetManager.h"
#include "common/fx/Chorus.h"
#include "common/fx/Delay.h"
#include "common/fx/Reverb.h"
#include "vintage/params/ParameterLayout.h"
#include "common/seq/ArpSeq.h"

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
    /**
        Host bypass is a clean stop: the first bypassed block fades the output out over kBypassFade
        (MIDI ignored), then everything is released as a panic (parts, router, both arp/seq
        instances incl. Hold and pedal, effects). While bypassed the output is silent and MIDI is
        ignored; the first normal block after it starts clean (held keys sound again when pressed
        again). Same latency and bus handling as processBlock.
    */
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlockBypassed;
    static constexpr double kBypassFadeSeconds = 0.005;

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
    /** The synth's own part of the tail: the longest time a released voice can ring (ENV-2). */
    double getSynthTailSeconds() const noexcept;

    // Programs: the 64 programs of the current bank (docs/architecture.md 6b.3). setCurrentProgram
    // is safe from any thread (applied on the message thread: at once if called there, else queued).
    int getNumPrograms() override { return presetManager->bankSize(); }
    int getCurrentProgram() override { return presetManager->currentIndex(); }
    void setCurrentProgram (int index) override { presetManager->requestProgram (index); }
    const juce::String getProgramName (int index) override { return presetManager->programName (index); }
    void changeProgramName (int index, const juce::String& name) override { presetManager->renameProgram (index, name); }

    /** Presets, banks and files (message thread). Owned by the processor: works without an editor. */
    presets::PresetManager& getPresets() noexcept { return *presetManager; }

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
    /** Every parameter value one block uses, read together at the block's start (see processInternal). */
    struct BlockParams
    {
        PartParams lower, upper;
        float vintage = 0.0f, masterDb = 0.0f, balance = 0.0f;
        KeyMode mode = KeyMode::Whole;
        int splitPoint = 60;
        FxSettings fx;
        ArpSettings arp;
    };
    /** Test hook: the parameter set the last block used (read between blocks, on the audio thread). */
    const BlockParams& getBlockParams() const noexcept { return blockParams; }
    /** Test hook: blocks that kept the previous block's parameters because an apply was in progress. */
    std::uint64_t getHeldBlocks() const noexcept { return heldBlocks; }
    /** The effect settings applied at the start of the last block. */
    const FxSettings& getAppliedFxSettings() const noexcept { return fxSettings; }
    /** The transport the arp/seq got for the first chunk of the last block. */
    const seq::Transport& getLastTransport() const noexcept { return blockTransport; }
    /** Event capacity behind each arp/seq instance (ArpSeq::minimumCapacity for the prepared block). */
    int getArpEventCapacity() const noexcept { return static_cast<int> (lowerArpOut.size()); }
    /** How often the non-finite output guard has fired since construction. */
    int getGuardTrips() const noexcept { return guardTrips; }
    /** Test hook: the next rendered chunk's mix is replaced by NaN, so the guard path can be tested
        now that host parameter values are sanitised. Call between blocks. */
    void forceNonFiniteForTesting() noexcept { forceNonFinite = true; }
    bool isBypassed() const noexcept { return bypassed; }

    static constexpr std::uint32_t kLowerSeed = 0x1001u, kUpperSeed = 0x2001u;

private:
    void renderChunk (float* const* out, int numChannels, int start, int len) noexcept;
    void readTransport() noexcept;
    seq::Transport transportAt (int sampleOffset) const noexcept;
    /** Copies every parameter the block uses into `out` (atomic loads only). */
    void readBlockParams (BlockParams& out) const noexcept;
    /** Runs one part's arp/seq over its routed chunk; returns the events the part gets. */
    std::span<const NoteEvent> arpChunk (seq::ArpSeq& arp, bool selected, std::span<const NoteEvent> routed,
                                         std::vector<NoteEvent>& out, const seq::Transport& tr, int len) noexcept;
    /** After a key-mode change: the module forgets keys and notes like the part, and is put back
        in step with the part's pedal (see the definition). */
    void resyncArpAfterKeyModeChange (seq::ArpSeq& arp, const Part& part, const seq::Transport& tr) noexcept;
    /** The release a preset change asks for (ApplyGate): like a key-mode change, every voice of both
        parts is released, held keys are forgotten, both arp/seq instances are resynchronised. */
    void releaseForPresetChange() noexcept;
    void resetEngine() noexcept;
    void processInternal (juce::AudioBuffer<float>&, juce::MidiBuffer&) noexcept;

    juce::AudioProcessorValueTreeState apvts;
    presets::ApplyGate presetGate; // message thread <-> audio thread hand-over (atomics only)

    PartParamBinding lowerBinding, upperBinding;
    PartParams lowerParams, upperParams;
    Part lowerPart, upperPart;

    BoundParam masterVolume, vintage, keyMode, splitPoint, balance; // sanitised reads (ParameterLayout.h)
    FxParamBinding fxBinding;
    ArpParamBinding arpBinding;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> masterGain;
    BlockParams blockParams, readScratch; // the set in use; a fresh read until it is known to be consistent
    std::uint64_t heldBlocks = 0;

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

    // Host bypass (processBlockBypassed).
    bool bypassed = false;     // the last block was a bypassed one
    int bypassFadeLeft = 0;    // samples of the fade-out still to render
    int bypassFadeLength = 1;
    juce::MidiBuffer noMidi;   // empty: the bypassed path ignores the host's MIDI

    int guardTrips = 0;            // non-finite guard firings (tests and diagnostics)
    bool forceNonFinite = false;   // test hook, see forceNonFiniteForTesting()

    // Host transport, read once per block (docs/architecture.md 6a.1). Missing fields = stopped.
    bool hostPlaying = false;
    double hostBpm = 120.0, hostPpq = 0.0;
    seq::Transport blockTransport { 48000.0, 120.0, 0.0, false };

    // Upper is rendered only while it is needed: not in Whole mode, or until its voices (released
    // by a change to Whole) have been silent for kUpperIdleSamples. Otherwise it costs nothing.
    static constexpr int kUpperIdleSamples = 256;
    bool upperActive = false;
    int upperQuietSamples = 0;

    // Last member: destroyed first (it listens to the parameters above).
    std::unique_ptr<presets::PresetManager> presetManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VintageSynthProcessor)
};
} // namespace vs
