#include "PluginProcessor.h"

#include "params/ParameterLayout.h"
#include "params/StateVersion.h"

namespace vs
{
namespace
{
constexpr const char* kStateTag = "VintageSynthState";
constexpr const char* kVersionAttr = "stateVersion";
} // namespace

VintageSynthProcessor::VintageSynthProcessor()
    : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, kStateTag, createParameterLayout())
{
}

void VintageSynthProcessor::prepareToPlay (double, int)
{
}

bool VintageSynthProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void VintageSynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // M0: silent instrument. The voice engine arrives in M1.
    buffer.clear();
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
