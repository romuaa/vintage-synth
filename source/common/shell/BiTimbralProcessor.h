#pragma once

// The shared bi-timbral processor skeleton (docs/architecture.md 8.2): everything about a
// two-part instrument plugin that does not depend on the instrument. Host block chunking, MIDI
// parsing (exactly once per event), MIDI Program Change hand-over, the key router and both
// arpeggiator/sequencer instances, the host transport, the seqlock parameter read per block with
// the preset release, key-mode changes, Upper-part activity, the part mixer with its latch cuts,
// the non-finite guard, chorus -> delay -> reverb, master gain x trim, the soft limiter and the
// safety clamp, host bypass, the tail, the host program list, the preset manager, and the plugin
// state. The instrument supplies its parts, its parameters and its preset schema through the hooks
// below (CRTP: resolved at compile time, no virtual calls).
//
// Block algorithm (normative order, docs/architecture.md 8.2.3):
//   1. ScopedNoDenormals; back from bypass -> resetEngine.
//   2. blockStarted; seqlock read of the shared and instrument parameters; held -> previous set.
//   3. If consistent: applyInstrumentParams, master gain target, limiter ceiling and switch,
//      effect settings, arp/seq settings per part.
//   4. Transport for the block.
//   5. Release request of a preset apply -> releaseForPresetChange, acknowledge.
//   6. Key-mode change -> release both parts, resync both arp/seq, latch sounding parts; split point.
//   7. Upper resume rule; mixer targets.
//   8. Per chunk: parse, pedal tracking, route, arp/seq, render Lower (and Upper if active, else a
//      zero-length render), mixer with latch cuts, input guard, chorus, delay, reverb, gain x trim
//      with guard, limiter, clamp, copy or mono fold, guard reset, unlatch, Upper idle count.
// Hooks are called at most once per block or chunk, never per sample.
//
// What Derived provides (all noexcept except prepareParts; may be private with `friend Base;`):
//   static constexpr const char* kName;                         // the plugin's name (getName)
//   static constexpr const char* kStateTag;  static constexpr int kStateVersion;
//   static void migrateState (juce::ValueTree&);                // brings an older state up to date
//   juce::AudioProcessorValueTreeState::ParameterLayout passed to the constructor
//   void readInstrumentParams (InstrumentParams&) const;       // atomic loads only
//   void applyInstrumentParams (const InstrumentParams&);      // after a consistent read
//   float partPan (const InstrumentParams&, int part) const;   // mixer pan of part 0 (Lower) / 1 (Upper)
//   void prepareParts (double hostRate, int maxBlock, int osFactor, int realtimeOsFactor); // may allocate;
//                                                    // getBlockParams() holds the parameters just read
//   double synthTailSeconds() const;                 // any thread, from parameter atomics
//   Part& part (int p);  const Part& part (int p) const;  // 0 Lower, 1 Upper
// and calls initPresets (...) from its constructor once the parameters are bound.
// Contract checks (docs/architecture.md 8.2.5), the same in every build type: the hooks of the
// per-block path must be noexcept (static_assert); a missing shared parameter throws ConfigError from
// the constructor (BoundParam::bind); a missing initPresets call is found by verifyConstructed()
// (the plugin entry point, createPluginProcessor) or, at the latest, by the first use of the presets.

#include <juce_audio_processors/juce_audio_processors.h>

#include "common/engine/EventParser.h"
#include "common/engine/KeyRouter.h"
#include "common/engine/NoteEvent.h"
#include "common/engine/Oversampler.h"
#include "common/engine/PartMixer.h"
#include "common/fx/Chorus.h"
#include "common/fx/Delay.h"
#include "common/fx/Reverb.h"
#include "common/fx/SoftLimiter.h"
#include "common/params/FxArpParams.h"
#include "common/params/GlobalParams.h"
#include "common/params/ParamFactory.h"
#include "common/presets/ApplyGate.h"
#include "common/presets/PresetManager.h"
#include "common/seq/ArpSeq.h"
#include "common/shell/ShellSupport.h"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace vs::shell
{
/** What the skeleton needs from one part. Both instruments' parts satisfy it. */
template <class P>
concept PartEngine = requires (P& p, const P& c, float* out, int n, std::span<const NoteEvent> ev) {
    p.render (out, n, ev);   // host rate, mono, overwrites out[0..n); n == 0 applies the events only
    p.releaseAllNotes();     // key-mode change, preset apply, bypass: release, keep controllers
    p.reset();               // silence now, forget everything
    p.resume();              // Upper back from Whole: parameters at target, modulators restarted
    { c.isSounding() } -> std::same_as<bool>;
    { c.isSustainDown() } -> std::same_as<bool>;
    { c.getLatencySamples() } -> std::same_as<int>;
};

/** Every global the skeleton itself uses, read in the same seqlock read as the instrument's. */
struct SharedParams
{
    float masterDb = 0.0f, balance = 0.0f;
    bool limiterOn = true;
    float limiterCeilingDb = kDefaultLimiterCeilingDb;
    KeyMode mode = KeyMode::Whole;
    int splitPoint = kDefaultSplitPoint;
    FxSettings fx;
    ArpSettings arp;
};

/** CRTP base of a bi-timbral instrument processor. PartT and InstrumentParamsT are template
    arguments (not member types of Derived): the skeleton stores both, and Derived is still
    incomplete where the skeleton's members are declared. */
template <class Derived, class PartT, class InstrumentParamsT>
class BiTimbralProcessor : public juce::AudioProcessor
{
public:
    using Part = PartT;
    using InstrumentParams = InstrumentParamsT; // both parts' parameters + the instrument's globals
    static_assert (PartEngine<Part>, "the part type must satisfy shell::PartEngine");
    static_assert (std::is_trivially_copyable_v<InstrumentParams>, "copied once per block on the audio thread");

    /** Every parameter value one block uses, read together at the block's start (see processInternal). */
    struct BlockParams : SharedParams, InstrumentParams
    {
    };

    ~BiTimbralProcessor() override = default;

    // ---- Audio ----
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override
    {
        preparedBlockSize = juce::jmax (1, maximumExpectedSamplesPerBlock);
        sampleRateHz = sampleRate;

        // Oversampling by host rate (docs/architecture.md 4.4): the internal rate stays >= 88.2 kHz.
        const int osFactor = voiceOversamplingFactor (sampleRate, isNonRealtime());

        readBlockParams (blockParams); // not concurrent with an apply's hold in a correct host: no seqlock needed here
        self().prepareParts (sampleRate, preparedBlockSize, osFactor, voiceOversamplingFactor (sampleRate, false));

        const auto mode = detail::keyModeOf (keyMode);
        router.setMode (mode);
        router.setSplitPoint (splitPoint.index());
        router.reset();
        upperActive = mode != KeyMode::Whole;
        upperQuietSamples = 0;

        events.prepare (EventParser::kDefaultCapacity);
        lowerEvents.assign (static_cast<size_t> (events.capacity()), NoteEvent {});
        upperEvents.assign (static_cast<size_t> (events.capacity()), NoteEvent {});
        partBuffer.setSize (2, preparedBlockSize);
        mixBuffer.setSize (2, preparedBlockSize);
        partBuffer.clear();
        mixBuffer.clear();

        mixer.prepare (sampleRate);
        mixer.setTargets (balance.get(), self().partPan (blockParams, 0), self().partPan (blockParams, 1), mode == KeyMode::Whole);
        mixer.reset();
        masterGain.reset (sampleRate, 0.02);
        masterGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (masterVolume.get()));
        limiter.prepare (sampleRate);
        limiter.reset (blockParams.limiterCeilingDb, blockParams.limiterOn);

        // Effects (allocate here only).
        chorus.prepare (sampleRate, preparedBlockSize);
        delay.prepare (sampleRate, preparedBlockSize);
        reverb.prepare (sampleRate, preparedBlockSize);
        chorus.reset();
        delay.reset();
        reverb.reset();

        // Arp/seq: output buffers that can never overflow for a prepared-size chunk (every parsed event
        // passed on plus every step of the fastest rate at the highest host tempo, see ArpSeq).
        const int arpCapacity = seq::ArpSeq::minimumCapacity (events.capacity(), seq::ArpSeq::maxStepsIn (preparedBlockSize, sampleRate));
        lowerArpOut.assign (static_cast<size_t> (arpCapacity), NoteEvent {});
        upperArpOut.assign (static_cast<size_t> (arpCapacity), NoteEvent {});
        resyncScratch.assign (static_cast<size_t> (seq::ArpSeq::minimumCapacity (1, 2)), NoteEvent {});
        lowerArp.reset();
        upperArp.reset();
        playerPedal = false;
        lowerChunk = upperChunk = {};
        bypassed = false;
        bypassFadeLeft = 0;
        bypassFadeLength = juce::jmax (1, static_cast<int> (std::lround (kBypassFadeSeconds * sampleRate)));
        noMidi.clear();

        // Both parts share the factor, so they have the same latency.
        jassert (lowerPart().getLatencySamples() == upperPart().getLatencySamples());
        setLatencySamples (lowerPart().getLatencySamples());
    }

    void releaseResources() override {}

    /** Everything back to silence together: parts, router, mixer, arp/seq instances, effects. */
    void reset() override
    {
        if (preparedBlockSize > 0)
            resetEngine();
    }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto& out = layouts.getMainOutputChannelSet();
        return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        if (bypassed)
        {
            // Back from bypass: start clean (the fade may not have finished if bypass was short).
            bypassed = false;
            bypassFadeLeft = 0;
            if (preparedBlockSize > 0)
                resetEngine();
        }
        processInternal (buffer, midi);
    }
    using juce::AudioProcessor::processBlock;

    /**
        Host bypass is a clean stop: the first bypassed block fades the output out over kBypassFade
        (MIDI ignored), then everything is released as a panic (parts, router, both arp/seq
        instances incl. Hold and pedal, effects). While bypassed the output is silent and MIDI is
        ignored; the first normal block after it starts clean (held keys sound again when pressed
        again). Same latency and bus handling as processBlock.
    */
    void processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        const int numSamples = buffer.getNumSamples();
        if (preparedBlockSize == 0)
        {
            buffer.clear();
            return;
        }
        if (! bypassed)
        {
            // Entering bypass: nothing may start any more. The parts release what sounds (envelope
            // release, heard through the fade), the router and both arp/seq instances forget keys,
            // Hold and pedal at once; the full reset follows at the end of the fade.
            bypassed = true;
            bypassFadeLeft = bypassFadeLength;
            lowerPart().releaseAllNotes();
            upperPart().releaseAllNotes();
            router.reset();
            lowerArp.reset();
            upperArp.reset();
        }
        if (bypassFadeLeft <= 0)
        {
            // Bypassed: silence, MIDI ignored, nothing runs, so nothing sounds that a preset change
            // would have to release: acknowledge it.
            presetGate.blockStarted();
            if (presetGate.takeReleaseRequest())
                presetGate.acknowledgeRelease();
            buffer.clear();
            return;
        }
        // Fading out: render on without the host's MIDI, ramp the output to 0 over the fade (per
        // sample, so it does not depend on the block size), then release everything as a panic.
        processInternal (buffer, noMidi);
        const int fadeSamples = juce::jmin (numSamples, bypassFadeLeft);
        const float step = 1.0f / static_cast<float> (bypassFadeLength);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            float* d = buffer.getWritePointer (ch);
            for (int i = 0; i < fadeSamples; ++i)
                d[i] *= static_cast<float> (bypassFadeLeft - i) * step;
            std::fill (d + fadeSamples, d + numSamples, 0.0f);
        }
        bypassFadeLeft -= fadeSamples;
        if (bypassFadeLeft <= 0)
            resetEngine();
    }
    using juce::AudioProcessor::processBlockBypassed;
    static constexpr double kBypassFadeSeconds = 0.005;

    // ---- Editor: the preset bar above the generic editor until the instrument has its own UI ----
    juce::AudioProcessorEditor* createEditor() override { return createInstrumentEditor (*this, pm()); }
    bool hasEditor() const override { return true; }

    // ---- Info ----
    const juce::String getName() const override { return Derived::kName; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    /** The synth's tail (Derived::synthTailSeconds) plus the longest tail of the effects that are
        on (each block's static tailSeconds), capped at 30 s, from the parameter values: callable
        from any thread. */
    double getTailLengthSeconds() const override
    {
        // Synth release plus the longest tail of the effects that are on (an effect that is off fades
        // out in 20 ms and drops its tail), capped at 30 s. A pure function of the parameter atomics,
        // callable from any thread. Hosts are not told when it changes: JUCE has no call for that (a
        // host asks when it needs it, e.g. at activation or before a bounce).
        FxSettings f;
        fxBinding.read (f);
        double fxTail = 0.0;
        if (f.chorus.mode != fx::ChorusMode::Off && f.chorus.mix > 0.0f)
            fxTail = std::max (fxTail, fx::Chorus::tailSeconds (f.chorus));
        if (f.delay.level > 0.0f)
            fxTail = std::max (fxTail, fx::Delay::tailSeconds (f.delay));
        if (f.reverb.level > 0.0f)
            fxTail = std::max (fxTail, fx::Reverb::tailSeconds (f.reverb));
        return std::min (fx::kMaxReportedTailSeconds, self().synthTailSeconds() + fxTail);
    }

    // ---- Programs: the programs of the current bank (docs/architecture.md 6b.3). setCurrentProgram
    // is safe from any thread (applied on the message thread: at once if called there, else queued).
    int getNumPrograms() override { return pm().bankSize(); }
    int getCurrentProgram() override { return pm().currentIndex(); }
    void setCurrentProgram (int index) override { pm().requestProgram (index); }
    const juce::String getProgramName (int index) override { return pm().programName (index); }
    void changeProgramName (int index, const juce::String& name) override { pm().renameProgram (index, name); }

    /** Presets, banks and files (message thread). Owned by the processor: works without an editor. */
    presets::PresetManager& getPresets() { return pm(); } // throws ConfigError if the instrument forgot initPresets

    // ---- State ----
    void getStateInformation (juce::MemoryBlock& destData) override
    {
        // Under the presets' writer lock: never half an apply, and the parameters match the preset info.
        const juce::ScopedLock w (pm().writerLock());
        auto state = apvts.copyState();
        state.setProperty (kStateVersionAttr, Derived::kStateVersion, nullptr);
        state.appendChild (pm().createSessionInfo(), nullptr);
        if (auto tables = pm().createTablesInfo(); tables.isValid()) // only an instrument with tables has any
            state.appendChild (tables, nullptr);

        if (auto xml = state.createXml())
            copyXmlToBinary (*xml, destData);
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        const auto xml = getXmlFromBinary (data, sizeInBytes);
        // The tag is the constant, not apvts.state.getType(): that tree may be replaced right now by a
        // restore on another thread (this runs before the writer lock is taken).
        if (xml == nullptr || ! xml->hasTagName (Derived::kStateTag))
            return;

        auto state = juce::ValueTree::fromXml (*xml);
        // The preset info (bank, program, modified flag) rides along as a child; sessions saved before
        // presets existed have none. It is taken out before the parameters are restored.
        // A hostile or damaged session may carry several: the first is used and every one is removed
        // (a leftover child would be kept by the parameter tree and saved again with the next state).
        auto info = takeChildren (state, kPresetInfoType);
        // The same for the tables child (patch tables, 8.8); an instrument without tables ignores it.
        auto tablesInfo = takeChildren (state, presets::kTablesInfoType);
        Derived::migrateState (state);
        addMissingParameters (state, getParameters());

        // Hosts call this on any thread: no file I/O here (a user bank named by the session is looked
        // up later on the message thread). One writer lock around both steps, so no apply on another
        // thread can come between the parameters and the preset info; the audio thread keeps its
        // parameter values until all of them have been replaced.
        // The tables are checked before the hold (no table, or one that does not check out, leaves that
        // part empty with an error the editor can show) and published inside it, with the parameters.
        auto tables = pm().prepareTablesInfo (tablesInfo);
        {
            const juce::ScopedLock w (pm().writerLock());
            pm().withParametersHeld ([&] { apvts.replaceState (state); pm().restoreTables (std::move (tables)); });
            pm().restoreSessionInfo (info);
        }
        pm().sessionRestored();
    }

    juce::AudioProcessorValueTreeState& getState() noexcept { return apvts; }

    // ---- Read-only views for tests and diagnostics (not thread-safe: read between blocks) ----
    const Part& getLowerPart() const noexcept { return self().part (0); }
    const Part& getUpperPart() const noexcept { return self().part (1); }
    const KeyRouter& getKeyRouter() const noexcept { return router; }
    /** True while the Upper part is rendered (not Whole mode, or its release tail is still running). */
    bool isUpperRendering() const noexcept { return upperActive; }
    const seq::ArpSeq& getArpSeq (int part) const noexcept { return part == 0 ? lowerArp : upperArp; }
    /** The events part 0 (Lower) / 1 (Upper) received in the last rendered chunk (offsets relative
        to that chunk). With host blocks no longer than the prepared size: the last block. */
    std::span<const NoteEvent> getLastPartEvents (int part) const noexcept { return part == 0 ? lowerChunk : upperChunk; }
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

    /** Fixed output trim, applied with the master gain (docs/decisions.md, "Output level"):
        -6.02 dB, exactly 0.5, so a render differs from the untrimmed one by exactly that factor.
        Per-voice, per-part and effect gains are untouched: preset levels and send balance stay. */
    static constexpr float kOutputTrim = 0.5f;
    /** The safety clamp behind the limiter (+12 dBFS); never reached in normal use. */
    static constexpr float kOutputClamp = 4.0f;
    /** The limiter state, for tests and meters (read between blocks). */
    const fx::SoftLimiter& getLimiter() const noexcept { return limiter; }

protected:
    /** Binds the shared parameters. `layout` is the instrument's full parameter list. */
    explicit BiTimbralProcessor (juce::AudioProcessorValueTreeState::ParameterLayout layout)
        : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, Derived::kStateTag, std::move (layout))
    {
        masterVolume.bind (apvts, ParamID::masterVolume);
        keyMode.bind (apvts, ParamID::keyMode);
        splitPoint.bind (apvts, ParamID::keySplitPoint);
        balance.bind (apvts, ParamID::mixBalance);
        limiterSwitch.bind (apvts, ParamID::masterLimiter);
        limiterCeiling.bind (apvts, ParamID::masterLimiterCeiling);
        fxBinding.bind (apvts);
        arpBinding.bind (apvts);

        // The per-block hooks run on the audio thread; an exception there would unwind through the host.
        static_assert (std::is_convertible_v<decltype (Derived::kName), const char*>, "Derived::kName must be a C string");
        static_assert (noexcept (std::declval<Derived&>().readInstrumentParams (std::declval<InstrumentParams&>())),
                       "readInstrumentParams must be noexcept");
        static_assert (noexcept (std::declval<Derived&>().applyInstrumentParams (std::declval<const InstrumentParams&>())),
                       "applyInstrumentParams must be noexcept");
        static_assert (noexcept (std::declval<const Derived&>().partPan (std::declval<const InstrumentParams&>(), 0)),
                       "partPan must be noexcept");
        static_assert (noexcept (std::declval<const Derived&>().synthTailSeconds()), "synthTailSeconds must be noexcept");
        static_assert (noexcept (std::declval<Derived&>().part (0)) && noexcept (std::declval<const Derived&>().part (0)),
                       "part (int) must be noexcept");
    }

public:
    /** Throws ConfigError unless the instrument finished its construction (it called initPresets). */
    void verifyConstructed() const
    {
        if (presetManager == nullptr)
            throw ConfigError (std::string (Derived::kName) + " did not call initPresets at the end of its constructor");
    }

protected:

    /** Called once by Derived's constructor, after every parameter is bound and before any is
        changed (the schema's defaults are the parameters' current values). */
    void initPresets (presets::Schema schema, presets::Bank factoryBank, const juce::String& productFolder,
                      std::unique_ptr<presets::TableSink> tableSink = nullptr)
    {
        // The manager owns the sink: it outlives every thread that can reach it (the manager's own timer
        // and the host's state calls stop when the manager is destroyed, after the derived class's members).
        presetManager = std::make_unique<presets::PresetManager> (apvts, std::move (schema), std::move (factoryBank), presetGate,
                                                                  presets::Locations::userDefault (productFolder), std::move (tableSink));
    }

    /** The key mode from the parameter atomics (any thread; NaN-safe). */
    KeyMode currentKeyMode() const noexcept { return detail::keyModeOf (keyMode); }

    juce::AudioProcessorValueTreeState apvts;

private:
    presets::PresetManager& pm() const
    {
        verifyConstructed();
        return *presetManager;
    }
    Derived& self() noexcept { return static_cast<Derived&> (*this); }
    const Derived& self() const noexcept { return static_cast<const Derived&> (*this); }
    Part& lowerPart() noexcept { return self().part (0); }
    Part& upperPart() noexcept { return self().part (1); }

    /** Copies every parameter the block uses into `out` (atomic loads only). */
    void readBlockParams (BlockParams& out) const noexcept
    {
        self().readInstrumentParams (out);
        out.masterDb = masterVolume.get();
        out.balance = balance.get();
        out.limiterOn = limiterSwitch.index() != 0;
        out.limiterCeilingDb = limiterCeiling.get();
        out.mode = detail::keyModeOf (keyMode);
        out.splitPoint = splitPoint.index();
        fxBinding.read (out.fx);
        arpBinding.read (out.arp);
    }

    void resetEngine() noexcept
    {
        lowerPart().reset();
        upperPart().reset();
        router.reset(); // the parts forgot their keys
        mixer.reset();
        lowerArp.reset(); // forgets keys and owed notes without emitting: the parts were reset too
        upperArp.reset();
        chorus.reset(); // O(1): stale delay-line content is hidden by the blocks (section 5.1)
        delay.reset();
        reverb.reset();
        // The current parameter values, not the last block's (after a bypass they may be stale), so
        // the limiter starts at its ceiling and switch without a ramp or fade.
        limiter.reset (limiterCeiling.get(), limiterSwitch.index() != 0);
        playerPedal = false; // the parts cleared their pedal
    }

    /** The release a preset change asks for (ApplyGate): like a key-mode change, every voice of both
        parts is released, held keys are forgotten, both arp/seq instances are resynchronised. */
    void releaseForPresetChange() noexcept
    {
        // The same release as a key-mode change (processInternal): voices go into their release,
        // the router and both arp/seq instances forget keys, and a release tail keeps the gain it
        // was heard at until its part plays again, so the new program's balance and pan cannot
        // make it jump.
        lowerPart().releaseAllNotes();
        upperPart().releaseAllNotes();
        router.reset();
        resyncArpAfterKeyModeChange (lowerArp, lowerPart(), blockTransport);
        resyncArpAfterKeyModeChange (upperArp, upperPart(), blockTransport);
        if (lowerPart().isSounding())
            mixer.latch (0);
        if (upperActive && upperPart().isSounding())
            mixer.latch (1);
    }

    void readTransport() noexcept
    {
        // Every field is optional; without a playing position and tempo the transport counts as
        // stopped and the arp/seq runs on its own tempo (docs/architecture.md 6a.1).
        hostPlaying = false;
        if (auto* ph = getPlayHead())
        {
            if (const auto pos = ph->getPosition())
            {
                const auto bpm = pos->getBpm();
                const auto ppq = pos->getPpqPosition();
                if (pos->getIsPlaying() && bpm.hasValue() && ppq.hasValue())
                {
                    hostPlaying = true;
                    hostBpm = *bpm;
                    hostPpq = *ppq;
                }
            }
        }
    }

    seq::Transport transportAt (int sampleOffset) const noexcept
    {
        if (! hostPlaying)
            return { sampleRateHz, 120.0, 0.0, false };
        // A chunk of a host block larger than the prepared size: its position is advanced from the
        // block's start position at the block's tempo (the host reports one tempo per block).
        return { sampleRateHz, hostBpm, hostPpq + static_cast<double> (sampleOffset) * hostBpm / (60.0 * sampleRateHz), true };
    }

    /** Runs one part's arp/seq over its routed chunk; returns the events the part gets. */
    std::span<const NoteEvent> arpChunk (seq::ArpSeq& arp, bool selected, std::span<const NoteEvent> routed,
                                         std::vector<NoteEvent>& out, const seq::Transport& tr, int len) noexcept
    {
        // The module always sees the events, also while Off: it keeps the held keys and the pedal,
        // so switching it on (or selecting this part) hands them over cleanly.
        const int n = arp.process (routed, tr, len, out);
        const std::span<const NoteEvent> produced { out.data(), static_cast<size_t> (n) };

        // Off (or this part not selected): the part gets the router's events themselves, bit-exactly.
        // The module would move an event at or after the chunk's end onto its last sample, which a
        // part applies after the last sample (section 2). Anything the module adds (the releases and
        // pedal state of an on -> Off switch, an overflow marker) is not a pass-through: then its
        // output is used.
        if (! selected || arp.settings().mode == seq::Mode::Off)
            if (detail::isPassThrough (routed, produced, len))
                return routed;
        return produced;
    }

    /** After a key-mode change: the module forgets keys and notes like the part, and is put back
        in step with the part's pedal. */
    void resyncArpAfterKeyModeChange (seq::ArpSeq& arp, const Part& part, const seq::Transport& tr) noexcept
    {
        // A key-mode change releases every voice of the part and the router forgets the held keys
        // (their note-offs will never arrive), but the part keeps its pedal. The module must forget
        // the keys too, or a pattern would play on without any key held: reset it (it owes nothing
        // the part has not just released). Then put its pedal bookkeeping back in step with the part
        // (down only if the module passed it on, in mode Off) and with the player (consumed while
        // on). A zero-length call does that through the module's own event handling; its output
        // only repeats what the part already has and is discarded.
        const seq::Settings wanted = arp.settings();
        arp.reset();
        const bool partPedal = part.isSustainDown();
        if (partPedal || playerPedal)
        {
            const NoteEvent down { 0, EventType::SustainPedal, 0, 1.0f };
            seq::Settings s = wanted;
            if (partPedal)
                s.mode = seq::Mode::Off; // passed on: the module knows the part holds it
            else
                s.mode = wanted.mode != seq::Mode::Off ? wanted.mode : seq::Mode::Arp; // consumed
            arp.setSettings (s);
            arp.process ({ &down, 1 }, tr, 0, resyncScratch);
        }
        arp.setSettings (wanted);
    }

    void processInternal (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) noexcept
    {
        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();

        if (preparedBlockSize == 0)
        {
            buffer.clear();
            return;
        }

        // Every parameter this block uses is read here, at once, as a seqlock reader (presets/ApplyGate.h):
        // while a preset is being applied or a state restored, or if one began while this block was
        // reading, the copy is discarded and the block keeps the previous block's values and key mode,
        // so no block can mix old and new values. Wait-free: the audio thread never waits for a writer.
        presetGate.blockStarted();
        const auto token = presetGate.beginRead();
        bool holding = true;
        if ((token & 1u) == 0u)
        {
            readBlockParams (readScratch);
            holding = ! presetGate.endRead (token);
        }
        if (holding)
            ++heldBlocks;
        else
            blockParams = readScratch;
        const auto mode = holding ? router.getMode() : blockParams.mode;
        if (! holding)
        {
            self().applyInstrumentParams (blockParams);
            masterGain.setTargetValue (juce::Decibels::decibelsToGain (blockParams.masterDb));
            limiter.setCeilingDb (blockParams.limiterCeilingDb);
            limiter.setEnabled (blockParams.limiterOn); // crossfaded over 5 ms (docs/decisions.md)

            // Effects: once per block; the blocks smooth their continuous parameters themselves.
            fxSettings = blockParams.fx;
            chorus.setParams (fxSettings.chorus);
            delay.setParams (fxSettings.delay);
            reverb.setParams (fxSettings.reverb);

            // Arp/seq settings, once per block; a part the arp/seq does not sit in front of (arp.target,
            // or Upper in Whole mode) runs its module in Off, which releases what it played.
            arpSettings = blockParams.arp;
            for (int p = 0; p < 2; ++p)
            {
                seq::Settings s = arpSettings.settings;
                if (! detail::partSelected (arpSettings.target, mode, p))
                    s.mode = seq::Mode::Off;
                (p == 0 ? lowerArp : upperArp).setSettings (s);
            }
        }
        readTransport();
        blockTransport = transportAt (0);

        // A preset change asked for a release: everything sounding is released now, and the writer is
        // told. Acknowledged only after every parameter read of this block (all of them happened
        // above): the writer starts writing new values only after this point (see ApplyGate.h).
        if (presetGate.takeReleaseRequest())
        {
            releaseForPresetChange();
            presetGate.acknowledgeRelease();
        }

        // Key mode (docs/architecture.md 4.2). A change releases every voice of both parts (keeping
        // the controllers) and forgets the held keys. The split point only affects new notes.
        const bool leavingWhole = ! holding && router.getMode() == KeyMode::Whole && mode != KeyMode::Whole;
        if (! holding && router.setMode (mode))
        {
            lowerPart().releaseAllNotes();
            upperPart().releaseAllNotes();
            resyncArpAfterKeyModeChange (lowerArp, lowerPart(), blockTransport);
            resyncArpAfterKeyModeChange (upperArp, upperPart(), blockTransport);
            // Release tails keep the gain they were heard at until the part plays again (PartMixer).
            if (lowerPart().isSounding())
                mixer.latch (0);
            if (upperActive && upperPart().isSounding())
                mixer.latch (1);
        }
        if (! holding)
            router.setSplitPoint (blockParams.splitPoint);

        // Upper comes back from Whole mode clean: parameters at their targets (no glide from stale
        // values), LFO restarted, decimator cleared. Done whenever it is silent at that moment, not
        // only when it was idle long enough to stop being rendered, so the result does not depend on
        // the host block size (when it stopped). A release tail still running just continues.
        if (mode != KeyMode::Whole && (! upperActive || (leavingWhole && ! upperPart().isSounding())))
        {
            upperPart().resume();
            upperActive = true;
            upperQuietSamples = 0;
        }
        if (! holding)
            mixer.setTargets (blockParams.balance, self().partPan (blockParams, 0), self().partPan (blockParams, 1), mode == KeyMode::Whole);

        // Every event in the buffer is applied exactly once, whatever its position: negative
        // positions count as 0, positions at or past the end apply after the last sample, and a
        // zero-length block still applies its events (a lost note-off would hang a note).
        auto next = midi.cbegin();
        auto parseChunk = [&] (int start, int len, bool lastChunk) {
            // Raw bytes only: building a juce::MidiMessage could allocate (SysEx).
            events.clear();
            for (; next != midi.cend(); ++next)
            {
                const auto meta = *next;
                if (! lastChunk && meta.samplePosition >= start + len)
                    break;
                events.add (meta.data, meta.numBytes, juce::jlimit (0, len, meta.samplePosition - start));
            }
            events.finish();
            // MIDI Program Change: only handed over (atomic store, the latest wins); the message
            // thread applies it shortly afterwards (PresetManager::pollRequests). So notes in this
            // block start on the current program and are released when the new one is applied, and
            // offline the moment it takes effect depends on the message thread (not deterministic).
            if (const int program = events.latestProgramChange(); program >= 0)
                presetGate.postMidiProgram (program);
            for (const auto& e : events.events())
            {
                if (e.type == EventType::SustainPedal)
                    playerPedal = e.value >= 0.5f;
                else if (e.type == EventType::AllNotesOff || e.type == EventType::AllSoundOff || e.type == EventType::ResetControllers)
                    playerPedal = false;
            }

            // One event list per part; each is as long as the parser's, so nothing is dropped here.
            const auto counts = router.route (events.events(), lowerEvents, upperEvents);
            const std::span<const NoteEvent> routedLower { lowerEvents.data(), static_cast<size_t> (counts.lower) };
            const std::span<const NoteEvent> routedUpper { upperEvents.data(), static_cast<size_t> (counts.upper) };

            // Arp/seq in front of each part (after the router). Both instances run every chunk, so
            // their clocks follow the host; Upper's gets only controllers in Whole mode.
            const auto tr = transportAt (start);
            lowerChunk = arpChunk (lowerArp, detail::partSelected (arpSettings.target, mode, 0), routedLower, lowerArpOut, tr, len);
            upperChunk = arpChunk (upperArp, detail::partSelected (arpSettings.target, mode, 1), routedUpper, upperArpOut, tr, len);
        };

        if (numSamples <= 0)
        {
            parseChunk (0, 0, true);
            lowerPart().render (partBuffer.getWritePointer (0), 0, lowerChunk);
            upperPart().render (partBuffer.getWritePointer (1), 0, upperChunk);
            for (int p = 0; p < 2; ++p)
                if (mixer.isLatched (p) && detail::firstNoteOn (p == 0 ? lowerChunk : upperChunk) >= 0)
                    mixer.unlatch (p, false);
            return;
        }

        // Hosts may exceed the announced block size: render in prepared-size chunks.
        for (int start = 0; start < numSamples; start += preparedBlockSize)
        {
            const int len = juce::jmin (preparedBlockSize, numSamples - start);
            parseChunk (start, len, start + len >= numSamples);
            renderChunk (buffer.getArrayOfWritePointers(), numChannels, start, len);
        }
    }

    void renderChunk (float* const* out, int numChannels, int start, int len) noexcept
    {
        float* lower = partBuffer.getWritePointer (0);
        float* upper = partBuffer.getWritePointer (1);
        lowerPart().render (lower, len, lowerChunk);

        // Upper: rendered only while active. Otherwise it costs no CPU, but still receives the
        // controllers (zero-length render), so pedal, bend and wheel are right when it starts.
        if (upperActive)
            upperPart().render (upper, len, upperChunk);
        else
            upperPart().render (upper, 0, upperChunk);

        float* left = mixBuffer.getWritePointer (0);
        float* right = mixBuffer.getWritePointer (1);
        const float* upperIn = upperActive ? upper : nullptr;

        // A latched part (after a key-mode change) moves to its new gain from its first note-on.
        int cut[2] { -1, -1 };
        for (int p = 0; p < 2; ++p)
            if (mixer.isLatched (p))
                cut[p] = detail::firstNoteOn (p == 0 ? lowerChunk : upperChunk);
        int pos = 0;
        for (;;)
        {
            int next = len, which = -1;
            for (int p = 0; p < 2; ++p)
                if (cut[p] >= pos && cut[p] < next)
                {
                    next = cut[p];
                    which = p;
                }
            if (next > pos)
                mixer.process (lower + pos, upperIn != nullptr ? upperIn + pos : nullptr, left + pos, right + pos, next - pos);
            pos = next;
            if (which < 0)
                break;
            mixer.unlatch (which, false);
            cut[which] = -1;
        }
        for (int p = 0; p < 2; ++p)
            if (cut[p] >= 0)
                mixer.unlatch (p, false); // a note-on at the very end of the chunk

        // Effects on the stereo bus: chorus -> delay -> reverb (an effect that is off is not touched).
        // A non-finite sample from a part is caught here first: the effects replace bad input by 0,
        // which would otherwise hide it from the guard below (and the part would never be reset).
        bool nonFinite = false;
        const bool forced = forceNonFinite;
        if (forced)
        {
            forceNonFinite = false;
            left[0] = std::numeric_limits<float>::quiet_NaN();
        }
        for (int i = 0; i < len; ++i)
            if (! std::isfinite (left[i]) || ! std::isfinite (right[i]))
            {
                nonFinite = true;
                left[i] = right[i] = 0.0f;
            }
        chorus.process (left, right, len);
        delay.process (left, right, len);
        reverb.process (left, right, len);

        // Output stage (docs/decisions.md, "Output level"): master gain with the fixed trim (a power
        // of two: exactly the untrimmed output scaled), the soft limiter if on, the safety clamp.
        for (int i = 0; i < len; ++i)
        {
            const float g = masterGain.getNextValue() * kOutputTrim;
            float l = left[i] * g;
            float r = right[i] * g;
            if (! std::isfinite (l) || ! std::isfinite (r))
            {
                nonFinite = true;
                l = r = 0.0f;
            }
            left[i] = l;
            right[i] = r;
        }
        limiter.process (left, right, len); // returns at once when fully off
        for (int i = 0; i < len; ++i)
        {
            // Speaker/ear safety behind the limiter, never reached normally.
            left[i] = juce::jlimit (-kOutputClamp, kOutputClamp, left[i]);
            right[i] = juce::jlimit (-kOutputClamp, kOutputClamp, right[i]);
        }

        if (numChannels >= 2)
        {
            std::copy (left, left + len, out[0] + start);
            std::copy (right, right + len, out[1] + start);
            for (int ch = 2; ch < numChannels; ++ch)
                std::fill (out[ch] + start, out[ch] + start + len, 0.0f);
        }
        else if (numChannels == 1)
        {
            for (int i = 0; i < len; ++i)
                out[0][start + i] = 0.5f * (left[i] + right[i]); // mono bus: (L + R) / 2
        }

        if (nonFinite)
        {
            jassert (forced); // DSP produced NaN/Inf: find the cause, the guard only hides it in release builds
            ++guardTrips;
            resetEngine(); // parts, router, mixer, both arp/seq instances, all three effects
        }

        // A latched part that has gone silent takes its new gain at once (nothing audible to fade).
        if (mixer.isLatched (0) && ! lowerPart().isSounding())
            mixer.unlatch (0, true);
        if (mixer.isLatched (1) && ! (upperActive && upperPart().isSounding()))
            mixer.unlatch (1, true);

        // Whole mode: Upper stops being rendered once its release tails (and the decimator's) are over.
        if (upperActive && router.getMode() == KeyMode::Whole)
        {
            upperQuietSamples = upperPart().isSounding() ? 0 : upperQuietSamples + len;
            if (upperQuietSamples >= kUpperIdleSamples)
                upperActive = false;
        }
    }

    presets::ApplyGate presetGate; // message thread <-> audio thread hand-over (atomics only)

    BoundParam masterVolume, keyMode, splitPoint, balance; // sanitised reads (ParamFactory.h)
    BoundParam limiterSwitch, limiterCeiling;
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

    // Output stage: master gain x kOutputTrim -> soft limiter (if on) -> safety clamp.
    fx::SoftLimiter limiter;

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

    // Last member: destroyed first (it listens to the parameters). Derived's members (its parts and
    // bindings) are destroyed before it; it uses neither.
    std::unique_ptr<presets::PresetManager> presetManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BiTimbralProcessor)
};
} // namespace vs::shell
