#include <catch2/catch_test_macros.hpp>

#include "PluginProcessor.h"
#include "params/ParameterLayout.h"

#include <cmath>

namespace
{
struct JuceEnv
{
    juce::ScopedJuceInitialiser_GUI init;
};
} // namespace

TEST_CASE ("Processor renders finite output for any block size", "[processor]")
{
    JuceEnv env;
    vs::VintageSynthProcessor proc;

    for (const double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        proc.setPlayConfigDetails (0, 2, sr, 512);
        proc.prepareToPlay (sr, 512);

        for (const int blockSize : { 1, 7, 64, 512 })
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);

            proc.processBlock (buffer, midi);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                for (int i = 0; i < blockSize; ++i)
                    REQUIRE (std::isfinite (buffer.getSample (ch, i)));
        }
        proc.releaseResources();
    }
}

TEST_CASE ("State save/load round-trips parameters", "[state]")
{
    JuceEnv env;

    juce::MemoryBlock saved;
    {
        vs::VintageSynthProcessor proc;
        auto* p = proc.getState().getParameter (vs::ParamID::masterVolume);
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (0.25f);
        proc.getStateInformation (saved);
    }

    vs::VintageSynthProcessor restored;
    restored.setStateInformation (saved.getData(), (int) saved.getSize());
    auto* p = restored.getState().getParameter (vs::ParamID::masterVolume);
    REQUIRE (p != nullptr);
    REQUIRE (std::abs (p->getValue() - 0.25f) < 1.0e-3f);
}

TEST_CASE ("Garbage state is ignored", "[state]")
{
    JuceEnv env;
    vs::VintageSynthProcessor proc;
    const char junk[] = "not a valid state";
    proc.setStateInformation (junk, (int) sizeof (junk));
    SUCCEED();
}
