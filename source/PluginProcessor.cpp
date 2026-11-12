#include "PluginProcessor.h"

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
    masterVolume = apvts.getRawParameterValue (ParamID::masterVolume);
    vintage = apvts.getRawParameterValue (ParamID::vintage);
    jassert (masterVolume != nullptr && vintage != nullptr);
}

void VintageSynthProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    preparedBlockSize = juce::jmax (1, maximumExpectedSamplesPerBlock);

    // Offline renders get 4x oversampling, real-time 2x.
    const int osFactor = isNonRealtime() ? 4 : 2;

    lowerBinding.read (lowerParams);
    lowerPart.setParams (lowerParams);
    lowerPart.prepare (sampleRate, preparedBlockSize, osFactor, 0x1001u);

    events.prepare (EventParser::kDefaultCapacity);
    partBuffer.setSize (1, preparedBlockSize);
    masterGain.reset (sampleRate, 0.02);
    masterGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (masterVolume->load()));

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

    lowerBinding.read (lowerParams);
    lowerPart.setParams (lowerParams);
    lowerPart.setVintage (vintage->load (std::memory_order_relaxed));
    masterGain.setTargetValue (juce::Decibels::decibelsToGain (masterVolume->load (std::memory_order_relaxed)));

    float* mono = partBuffer.getWritePointer (0);

    // Hosts may exceed the announced block size: render in prepared-size chunks.
    for (int start = 0; start < numSamples; start += preparedBlockSize)
    {
        const int len = juce::jmin (preparedBlockSize, numSamples - start);

        // Raw bytes only: building a juce::MidiMessage could allocate (SysEx).
        events.clear();
        for (auto it = midi.findNextSamplePosition (start); it != midi.cend(); ++it)
        {
            const auto meta = *it;
            if (meta.samplePosition >= start + len)
                break;
            events.add (meta.data, meta.numBytes, meta.samplePosition - start);
        }
        events.finish();

        lowerPart.render (mono, len, events.events());

        bool nonFinite = false;
        for (int i = 0; i < len; ++i)
        {
            float s = mono[i] * masterGain.getNextValue();
            if (! std::isfinite (s))
            {
                nonFinite = true;
                s = 0.0f;
            }
            s = juce::jlimit (-kOutputClamp, kOutputClamp, s); // speaker/ear safety, never reached normally
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.setSample (ch, start + i, s);
        }

        if (nonFinite)
        {
            jassertfalse; // DSP produced NaN/Inf: find the cause, the guard only hides it in release builds
            lowerPart.reset();
        }
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
