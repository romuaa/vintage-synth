#include "PluginProcessor.h"

#include "dsp/Drift.h"
#include "engine/Oversampler.h"

#include "params/ParameterLayout.h"
#include "params/StateVersion.h"

#include <cstring>
#include <limits>

namespace vs
{
namespace
{
constexpr const char* kStateTag = "VintageSynthState";
constexpr const char* kVersionAttr = "stateVersion";
constexpr float kOutputClamp = 4.0f; // +12 dBFS
} // namespace

VintageSynthProcessor::VintageSynthProcessor()
    : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, kStateTag, createParameterLayout())
{
    lowerBinding.bind (apvts, kLowerPrefix);
    upperBinding.bind (apvts, kUpperPrefix);
    masterVolume.bind (apvts, ParamID::masterVolume);
    vintage.bind (apvts, ParamID::vintage);
    keyMode.bind (apvts, ParamID::keyMode);
    splitPoint.bind (apvts, ParamID::keySplitPoint);
    balance.bind (apvts, ParamID::mixBalance);
    fxBinding.bind (apvts);
    arpBinding.bind (apvts);
}

namespace
{
/** Offset of the first note-on in a routed chunk, or -1. */
int firstNoteOn (std::span<const NoteEvent> events) noexcept
{
    for (const auto& e : events)
        if (e.type == EventType::NoteOn)
            return e.offset;
    return -1;
}

KeyMode keyModeOf (const BoundParam& p) noexcept
{
    return static_cast<KeyMode> (p.index()); // NaN-safe, clamped to 0..2
}

bool sameBits (float a, float b) noexcept { return std::memcmp (&a, &b, sizeof (float)) == 0; }

/** True if `out` is exactly what the arp/seq module writes when it only passes `in` on in mode
    Off: the same events in the same order, offsets moved inside [0, len - 1] at most. */
bool isPassThrough (std::span<const NoteEvent> in, std::span<const NoteEvent> out, int len) noexcept
{
    if (in.size() != out.size())
        return false;
    const int last = juce::jmax (len - 1, 0);
    for (size_t i = 0; i < in.size(); ++i)
        if (in[i].type != out[i].type || in[i].note != out[i].note || ! sameBits (in[i].value, out[i].value)
            || juce::jlimit (0, last, in[i].offset) != out[i].offset)
            return false;
    return true;
}

/** Whether the arp/seq sits in front of `part` (0 Lower, 1 Upper). In Whole mode only Lower
    plays, so the arp/seq applies to Lower whatever arp.target says; the selector matters in Dual
    and Split. */
bool partSelected (ArpTarget target, KeyMode mode, int part) noexcept
{
    if (mode == KeyMode::Whole)
        return part == 0;
    return target == ArpTarget::Both || static_cast<int> (target) == part;
}
} // namespace

void VintageSynthProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    preparedBlockSize = juce::jmax (1, maximumExpectedSamplesPerBlock);
    sampleRateHz = sampleRate;

    // Oversampling by host rate (docs/architecture.md 4.4): the internal rate stays >= 88.2 kHz.
    const int osFactor = voiceOversamplingFactor (sampleRate, isNonRealtime());

    lowerBinding.read (lowerParams);
    upperBinding.read (upperParams);
    lowerPart.setParams (lowerParams);
    upperPart.setParams (upperParams);
    lowerPart.prepare (sampleRate, preparedBlockSize, osFactor, kLowerSeed, voiceOversamplingFactor (sampleRate, false));
    upperPart.prepare (sampleRate, preparedBlockSize, osFactor, kUpperSeed, voiceOversamplingFactor (sampleRate, false));

    const auto mode = keyModeOf (keyMode);
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
    mixer.setTargets (balance.get(), lowerParams.pan, upperParams.pan, mode == KeyMode::Whole);
    mixer.reset();
    masterGain.reset (sampleRate, 0.02);
    masterGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (masterVolume.get()));

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
    jassert (lowerPart.getLatencySamples() == upperPart.getLatencySamples());
    setLatencySamples (lowerPart.getLatencySamples());
}

void VintageSynthProcessor::resetEngine() noexcept
{
    lowerPart.reset();
    upperPart.reset();
    router.reset(); // the parts forgot their keys
    mixer.reset();
    lowerArp.reset(); // forgets keys and owed notes without emitting: the parts were reset too
    upperArp.reset();
    chorus.reset(); // O(1): stale delay-line content is hidden by the blocks (section 5.1)
    delay.reset();
    reverb.reset();
    playerPedal = false; // the parts cleared their pedal
}

void VintageSynthProcessor::reset()
{
    if (preparedBlockSize > 0)
        resetEngine();
}

double VintageSynthProcessor::getSynthTailSeconds() const noexcept
{
    // A released voice rings until ENV-2 falls below 1e-5 (-100 dB, Envelope.h): the mapped release
    // time covers 99 % (-40 dB), so that takes 2.5 release times. Worst cases that lengthen it:
    // ENV-2 key follow (times double every two octaves below C4: x 2^(60/24) at MIDI note 0) and
    // Vintage (envelope times up to +8 % per voice, Drift::kEnvTimeSpread).
    const auto mode = keyModeOf (keyMode);
    const float vintageAmount = vintage.get();
    double longest = 0.0;
    for (int p = 0; p < 2; ++p)
    {
        if (p == 1 && mode == KeyMode::Whole)
            continue; // Upper cannot sound in Whole mode
        PartParams params;
        (p == 0 ? lowerBinding : upperBinding).read (params);
        double t = static_cast<double> (map::envTimeSec (params.env2Release)) * 2.5;
        if (params.env2KeyFollow == 1)
            t *= std::exp2 (60.0 / 24.0);
        t *= 1.0 + static_cast<double> (dsp::Drift::kEnvTimeSpread) * static_cast<double> (vintageAmount);
        longest = std::max (longest, t);
    }
    return longest;
}

double VintageSynthProcessor::getTailLengthSeconds() const
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
    return std::min (fx::kMaxReportedTailSeconds, getSynthTailSeconds() + fxTail);
}

void VintageSynthProcessor::readTransport() noexcept
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

seq::Transport VintageSynthProcessor::transportAt (int sampleOffset) const noexcept
{
    if (! hostPlaying)
        return { sampleRateHz, 120.0, 0.0, false };
    // A chunk of a host block larger than the prepared size: its position is advanced from the
    // block's start position at the block's tempo (the host reports one tempo per block).
    return { sampleRateHz, hostBpm, hostPpq + static_cast<double> (sampleOffset) * hostBpm / (60.0 * sampleRateHz), true };
}

std::span<const NoteEvent> VintageSynthProcessor::arpChunk (seq::ArpSeq& arp, bool selected, std::span<const NoteEvent> routed,
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
        if (isPassThrough (routed, produced, len))
            return routed;
    return produced;
}

void VintageSynthProcessor::resyncArpAfterKeyModeChange (seq::ArpSeq& arp, const Part& part, const seq::Transport& tr) noexcept
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

bool VintageSynthProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void VintageSynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
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

void VintageSynthProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
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
        lowerPart.releaseAllNotes();
        upperPart.releaseAllNotes();
        router.reset();
        lowerArp.reset();
        upperArp.reset();
    }
    if (bypassFadeLeft <= 0)
    {
        buffer.clear(); // bypassed: silence, MIDI ignored, nothing runs
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

void VintageSynthProcessor::processInternal (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) noexcept
{

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    if (preparedBlockSize == 0)
    {
        buffer.clear();
        return;
    }

    const float vintageAmount = vintage.get();
    lowerBinding.read (lowerParams);
    upperBinding.read (upperParams);
    lowerPart.setParams (lowerParams);
    upperPart.setParams (upperParams);
    lowerPart.setVintage (vintageAmount);
    upperPart.setVintage (vintageAmount);
    masterGain.setTargetValue (juce::Decibels::decibelsToGain (masterVolume.get()));

    // Effects: once per block; the blocks smooth their continuous parameters themselves.
    fxBinding.read (fxSettings);
    chorus.setParams (fxSettings.chorus);
    delay.setParams (fxSettings.delay);
    reverb.setParams (fxSettings.reverb);

    // Arp/seq settings, once per block; a part the arp/seq does not sit in front of (arp.target,
    // or Upper in Whole mode) runs its module in Off, which releases what it played.
    const auto mode = keyModeOf (keyMode);
    arpBinding.read (arpSettings);
    for (int p = 0; p < 2; ++p)
    {
        seq::Settings s = arpSettings.settings;
        if (! partSelected (arpSettings.target, mode, p))
            s.mode = seq::Mode::Off;
        (p == 0 ? lowerArp : upperArp).setSettings (s);
    }
    readTransport();
    blockTransport = transportAt (0);

    // Key mode (docs/architecture.md 4.2). A change releases every voice of both parts (keeping
    // the controllers) and forgets the held keys. The split point only affects new notes.
    const bool leavingWhole = router.getMode() == KeyMode::Whole && mode != KeyMode::Whole;
    if (router.setMode (mode))
    {
        lowerPart.releaseAllNotes();
        upperPart.releaseAllNotes();
        resyncArpAfterKeyModeChange (lowerArp, lowerPart, blockTransport);
        resyncArpAfterKeyModeChange (upperArp, upperPart, blockTransport);
        // Release tails keep the gain they were heard at until the part plays again (PartMixer).
        if (lowerPart.isSounding())
            mixer.latch (0);
        if (upperActive && upperPart.isSounding())
            mixer.latch (1);
    }
    router.setSplitPoint (splitPoint.index());

    // Upper comes back from Whole mode clean: parameters at their targets (no glide from stale
    // values), LFO restarted, decimator cleared. Done whenever it is silent at that moment, not
    // only when it was idle long enough to stop being rendered, so the result does not depend on
    // the host block size (when it stopped). A release tail still running just continues.
    if (mode != KeyMode::Whole && (! upperActive || (leavingWhole && ! upperPart.isSounding())))
    {
        upperPart.resume();
        upperActive = true;
        upperQuietSamples = 0;
    }
    mixer.setTargets (balance.get(), lowerParams.pan, upperParams.pan, mode == KeyMode::Whole);

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
        lowerChunk = arpChunk (lowerArp, partSelected (arpSettings.target, mode, 0), routedLower, lowerArpOut, tr, len);
        upperChunk = arpChunk (upperArp, partSelected (arpSettings.target, mode, 1), routedUpper, upperArpOut, tr, len);
    };

    if (numSamples <= 0)
    {
        parseChunk (0, 0, true);
        lowerPart.render (partBuffer.getWritePointer (0), 0, lowerChunk);
        upperPart.render (partBuffer.getWritePointer (1), 0, upperChunk);
        for (int p = 0; p < 2; ++p)
            if (mixer.isLatched (p) && firstNoteOn (p == 0 ? lowerChunk : upperChunk) >= 0)
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

void VintageSynthProcessor::renderChunk (float* const* out, int numChannels, int start, int len) noexcept
{
    float* lower = partBuffer.getWritePointer (0);
    float* upper = partBuffer.getWritePointer (1);
    lowerPart.render (lower, len, lowerChunk);

    // Upper: rendered only while active. Otherwise it costs no CPU, but still receives the
    // controllers (zero-length render), so pedal, bend and wheel are right when it starts.
    if (upperActive)
        upperPart.render (upper, len, upperChunk);
    else
        upperPart.render (upper, 0, upperChunk);

    float* left = mixBuffer.getWritePointer (0);
    float* right = mixBuffer.getWritePointer (1);
    const float* upperIn = upperActive ? upper : nullptr;

    // A latched part (after a key-mode change) moves to its new gain from its first note-on.
    int cut[2] { -1, -1 };
    for (int p = 0; p < 2; ++p)
        if (mixer.isLatched (p))
            cut[p] = firstNoteOn (p == 0 ? lowerChunk : upperChunk);
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

    for (int i = 0; i < len; ++i)
    {
        const float g = masterGain.getNextValue();
        float l = left[i] * g;
        float r = right[i] * g;
        if (! std::isfinite (l) || ! std::isfinite (r))
        {
            nonFinite = true;
            l = r = 0.0f;
        }
        // Speaker/ear safety, never reached normally.
        left[i] = juce::jlimit (-kOutputClamp, kOutputClamp, l);
        right[i] = juce::jlimit (-kOutputClamp, kOutputClamp, r);
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
    if (mixer.isLatched (0) && ! lowerPart.isSounding())
        mixer.unlatch (0, true);
    if (mixer.isLatched (1) && ! (upperActive && upperPart.isSounding()))
        mixer.unlatch (1, true);

    // Whole mode: Upper stops being rendered once its release tails (and the decimator's) are over.
    if (upperActive && router.getMode() == KeyMode::Whole)
    {
        upperQuietSamples = upperPart.isSounding() ? 0 : upperQuietSamples + len;
        if (upperQuietSamples >= kUpperIdleSamples)
            upperActive = false;
    }
}

juce::AudioProcessorEditor* VintageSynthProcessor::createEditor()
{
    // Placeholder until the custom panel UI (M7).
    return new juce::GenericAudioProcessorEditor (*this);
}

void VintageSynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (kVersionAttr, kStateVersion, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void VintageSynthProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    // Migrations go here: if ((int) state.getProperty (kVersionAttr, 1) < N) { ... }
    apvts.replaceState (state);
}
} // namespace vs

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new vs::VintageSynthProcessor();
}
