// Hidden "[.bside-statedump]" test (docs/architecture.md 8.21.1, behaviour gates 1 and 7): writes what a host sees of
// B-Side's parameters and state to $VS_DUMP_DIR, so two builds can be compared byte for byte (`cmp`): every parameter
// (index, ID, name, default, steps, label, display text at 0, 0.25, 0.5, 0.75, 1), the default saved state, a seeded
// random parameter set (its values and its saved state) and a program file saved from it. With $VS_LOAD_DIR it also
// loads the random state and the program file saved by another build and writes the values they load to.
#include <catch2/catch_test_macros.hpp>

#include "bside/PluginProcessor.h"

#include <random>

namespace
{
juce::String values (bs::BSideProcessor& p)
{
    juce::String t;
    for (auto* param : p.getParameters())
        t << dynamic_cast<juce::AudioProcessorParameterWithID*> (param)->paramID << " " << juce::String (param->getValue(), 9) << "\n";
    return t;
}
} // namespace

TEST_CASE ("B-Side state dump: parameters, default and random state, a program file", "[.bside-statedump]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VS_DUMP_DIR", {});
    if (dir.isEmpty())
        SKIP ("VS_DUMP_DIR not set");
    const juce::File out (dir);
    REQUIRE (out.createDirectory());
    juce::ScopedJuceInitialiser_GUI init;
    {
        bs::BSideProcessor p;
        juce::String text;
        int index = 0;
        for (auto* param : p.getParameters())
        {
            text << index++ << " | " << param->getName (128) << " | " << dynamic_cast<juce::AudioProcessorParameterWithID*> (param)->paramID << " | default "
                 << juce::String (param->getDefaultValue(), 9) << " | steps " << param->getNumSteps() << " | discrete " << (param->isDiscrete() ? 1 : 0)
                 << " | label " << param->getLabel();
            for (const float v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                text << " | " << param->getText (v, 128);
            text << "\n";
        }
        REQUIRE (out.getChildFile ("params.txt").replaceWithText (text));
        juce::MemoryBlock state;
        p.getStateInformation (state);
        REQUIRE (out.getChildFile ("state-default.bin").replaceWithData (state.getData(), state.getSize()));
    }
    {
        bs::BSideProcessor p;
        std::mt19937 rng (20261124u);
        for (auto* param : p.getParameters())
            param->setValueNotifyingHost (static_cast<float> (rng() % 1001) / 1000.0f);
        juce::MemoryBlock state;
        p.getStateInformation (state);
        REQUIRE (out.getChildFile ("state-random.bin").replaceWithData (state.getData(), state.getSize()));
        REQUIRE (out.getChildFile ("values-random.txt").replaceWithText (values (p)));
        REQUIRE (p.getPresets().saveProgramFile (out.getChildFile ("random.bsprog"), "Dump").isEmpty());
    }
    const auto load = juce::SystemStats::getEnvironmentVariable ("VS_LOAD_DIR", {});
    if (load.isNotEmpty())
    {
        juce::MemoryBlock state;
        REQUIRE (juce::File (load).getChildFile ("state-random.bin").loadFileAsData (state));
        bs::BSideProcessor p;
        p.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        REQUIRE (out.getChildFile ("loaded-state-values.txt").replaceWithText (values (p)));
        bs::BSideProcessor q;
        REQUIRE (q.getPresets().loadProgramFile (juce::File (load).getChildFile ("random.bsprog")).isEmpty());
        REQUIRE (out.getChildFile ("loaded-program-values.txt").replaceWithText (values (q)));
    }
}
