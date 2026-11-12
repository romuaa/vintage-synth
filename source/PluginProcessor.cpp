#include "PluginProcessor.h"

#include "engine/Oversampler.h"

#include "params/ParameterLayout.h"
#include "params/StateVersion.h"

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
    masterVolume = apvts.getRawParameterValue (ParamID::masterVolume);
    vintage = apvts.getRawParameterValue (ParamID::vintage);
    keyMode = apvts.getRawParameterValue (ParamID::keyMode);
    splitPoint = apvts.getRawParameterValue (ParamID::keySplitPoint);
    balance = apvts.getRawParameterValue (ParamID::mixBalance);
    jassert (masterVolume != nullptr && vintage != nullptr && keyMode != nullptr && splitPoint != nullptr
             && balance != nullptr);
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

KeyMode keyModeOf (const std::atomic<float>* p) noexcept
{
    return static_cast<KeyMode> (juce::jlimit (0, 2, static_cast<int> (p->load (std::memory_order_relaxed))));
}
} // namespace

void VintageSynthProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    preparedBlockSize = juce::jmax (1, maximumExpectedSamplesPerBlock);

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
    router.setSplitPoint (static_cast<int> (splitPoint->load()));
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
    mixer.setTargets (balance->load(), lowerParams.pan, upperParams.pan, mode == KeyMode::Whole);
    mixer.reset();
    masterGain.reset (sampleRate, 0.02);
    masterGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (masterVolume->load()));

    // Both parts share the factor, so they have the same latency.
    jassert (lowerPart.getLatencySamples() == upperPart.getLatencySamples());
    setLatencySamples (lowerPart.getLatencySamples());
}

bool VintageSynthProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void VintageSynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    if (preparedBlockSize == 0)
    {
        buffer.clear();
        return;
    }

    const float vintageAmount = vintage->load (std::memory_order_relaxed);
    lowerBinding.read (lowerParams);
    upperBinding.read (upperParams);
    lowerPart.setParams (lowerParams);
    upperPart.setParams (upperParams);
    lowerPart.setVintage (vintageAmount);
    upperPart.setVintage (vintageAmount);
    masterGain.setTargetValue (juce::Decibels::decibelsToGain (masterVolume->load (std::memory_order_relaxed)));

    // Key mode (docs/architecture.md 4.2). A change releases every voice of both parts (keeping
    // the controllers) and forgets the held keys. The split point only affects new notes.
    const auto mode = keyModeOf (keyMode);
    const bool leavingWhole = router.getMode() == KeyMode::Whole && mode != KeyMode::Whole;
    if (router.setMode (mode))
    {
        lowerPart.releaseAllNotes();
        upperPart.releaseAllNotes();
        // Release tails keep the gain they were heard at until the part plays again (PartMixer).
        if (lowerPart.isSounding())
            mixer.latch (0);
        if (upperActive && upperPart.isSounding())
            mixer.latch (1);
    }
    router.setSplitPoint (static_cast<int> (splitPoint->load (std::memory_order_relaxed)));

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
    mixer.setTargets (balance->load (std::memory_order_relaxed), lowerParams.pan, upperParams.pan, mode == KeyMode::Whole);

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

        // One event list per part; each is as long as the parser's, so nothing is dropped here.
        const auto counts = router.route (events.events(), lowerEvents, upperEvents);
        lowerChunk = { lowerEvents.data(), static_cast<size_t> (counts.lower) };
        upperChunk = { upperEvents.data(), static_cast<size_t> (counts.upper) };
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

    bool nonFinite = false;
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
        jassertfalse; // DSP produced NaN/Inf: find the cause, the guard only hides it in release builds
        lowerPart.reset();
        upperPart.reset();
        router.reset(); // the parts forgot their keys
        mixer.reset();
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
