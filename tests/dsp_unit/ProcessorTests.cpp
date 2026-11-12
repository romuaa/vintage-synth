#include <catch2/catch_test_macros.hpp>

#include "PluginProcessor.h"
#include "params/Mappings.h"
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

TEST_CASE ("Parameter layout: 96 parameters, the M2 parameters appended with their defaults", "[processor][state]")
{
    JuceEnv env;
    vs::VintageSynthProcessor proc;
    const auto& params = proc.getParameters();
    CHECK (params.size() == 96); // master + 2 x 47 per part + vintage

    auto* state = &proc.getState();
    const char* m2Floats[] { "porta.time", "unison.detune", "vel.vca", "vel.vcf", "mod.wheel_vco" };
    const char* m2Choices[] { "assign.mode", "assign.unison_voices", "porta.mode", "bend.range" };
    for (const juce::String prefix : { "lower.", "upper." })
    {
        for (const auto* id : m2Floats)
            CHECK (dynamic_cast<juce::AudioParameterFloat*> (state->getParameter (prefix + id)) != nullptr);
        for (const auto* id : m2Choices)
            CHECK (dynamic_cast<juce::AudioParameterChoice*> (state->getParameter (prefix + id)) != nullptr);
    }
    auto choice = [&] (const char* id) { return dynamic_cast<juce::AudioParameterChoice*> (state->getParameter (id)); };
    auto plainDefault = [&] (const char* id) {
        auto* p = state->getParameter (id);
        return p->convertFrom0to1 (p->getDefaultValue());
    };
    CHECK (choice ("lower.assign.mode")->getCurrentChoiceName() == "Poly");
    CHECK (choice ("lower.assign.unison_voices")->getCurrentChoiceName() == "4");
    CHECK (choice ("lower.porta.mode")->getCurrentChoiceName() == "Always");
    CHECK (choice ("lower.bend.range")->getIndex() == 2);
    CHECK (choice ("lower.bend.range")->choices.size() == 13);
    CHECK (std::abs (plainDefault ("lower.porta.time")) < 1.0e-6f);
    CHECK (std::abs (plainDefault ("lower.unison.detune") - 0.3f) < 1.0e-6f);
    CHECK (std::abs (plainDefault ("lower.vel.vca") - 0.5f) < 1.0e-6f);
    CHECK (std::abs (plainDefault ("lower.vel.vcf")) < 1.0e-6f);
    CHECK (std::abs (plainDefault ("lower.mod.wheel_vco") - 0.3f) < 1.0e-6f);

    // Appended: the five floats follow ENV-2 Release, the four choices end the part's list.
    int idxDepth = -1, idxEnv2Rel = -1, idxBend = -1, idxUpperFirst = -1;
    for (int i = 0; i < params.size(); ++i)
    {
        const auto id = dynamic_cast<juce::AudioProcessorParameterWithID*> (params[i])->paramID;
        if (id == "lower.mod.wheel_vco")
            idxDepth = i;
        if (id == "lower.env2.release")
            idxEnv2Rel = i;
        if (id == "lower.bend.range")
            idxBend = i;
        if (id == "upper.lfo.rate")
            idxUpperFirst = i;
    }
    CHECK (idxDepth == idxEnv2Rel + 5);
    CHECK (idxBend == idxUpperFirst - 1);

    // Display text and parsing agree with the mappings.
    auto* porta = state->getParameter ("lower.porta.time");
    CHECK (porta->getText (0.0f, 32) == "Off");
    CHECK (porta->getText (vs::map::portaFromSec (0.5f), 32) == "500 ms");
    CHECK (std::abs (porta->getValueForText ("1.25 s") - vs::map::portaFromSec (1.25f)) < 1.0e-5f);
    auto* detune = state->getParameter ("lower.unison.detune");
    CHECK (detune->getText (0.3f, 32) == "15.0 ct");
    CHECK (std::abs (detune->getValueForText ("25 ct") - 0.5f) < 1.0e-6f);
    CHECK (state->getParameter ("lower.vel.vca")->getText (0.5f, 32) == "50 %");
    CHECK (state->getParameter ("lower.vel.vca")->getName (64) == juce::String::fromUTF8 ("Lower Velocity \xe2\x86\x92 VCA"));
}
