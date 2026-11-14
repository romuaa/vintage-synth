// Hidden "[.statedump]" test: writes everything the host can see of an instrument's identity and
// default state to $VS_DUMP_DIR/state.txt (and the default saved state to state-default.bin), so two
// builds can be compared byte for byte (`cmp`): the plugin name, every parameter (index, ID, name,
// range, default, step count, display text at 0, 0.25, 0.5, 0.75, 1) and the saved state of a
// freshly constructed instance. Proves that a refactor of the shared skeleton changes none of it.
// Never writes inside the repository; does nothing without VS_DUMP_DIR.
//
//   VS_DUMP_DIR=$TMPDIR/state-before render_regression "[.statedump]"
#include <catch2/catch_test_macros.hpp>

#include "RenderHarness.h"

using namespace vs::rr;

TEST_CASE ("State dump: name, parameter list and default saved state", "[.statedump]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VS_DUMP_DIR", {});
    if (dir.isEmpty())
        SKIP ("VS_DUMP_DIR not set");
    const juce::File out (dir);
    REQUIRE (out.createDirectory());

    juce::ScopedJuceInitialiser_GUI init;
    auto instance = makeDefaultInstance();
    auto& p = *instance.processor;

    juce::String text;
    text << "name: " << p.getName() << "\n";
    text << "latency: " << p.getLatencySamples() << "  tail: " << juce::String (p.getTailLengthSeconds(), 6)
         << "  programs: " << p.getNumPrograms() << "\n";
    int index = 0;
    for (auto* param : p.getParameters())
    {
        text << index++ << " | " << param->getName (128);
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (param))
            text << " | " << withId->paramID;
        text << " | default " << juce::String (param->getDefaultValue(), 9) << " | steps " << param->getNumSteps()
             << " | discrete " << (param->isDiscrete() ? 1 : 0) << " | label " << param->getLabel();
        for (const float v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            text << " | " << param->getText (v, 128);
        text << "\n";
    }
    REQUIRE (out.getChildFile ("state.txt").replaceWithText (text));

    juce::MemoryBlock state;
    p.getStateInformation (state);
    REQUIRE (out.getChildFile ("state-default.bin").replaceWithData (state.getData(), state.getSize()));
}
