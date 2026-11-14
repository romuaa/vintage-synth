// What the shared skeleton demands of an instrument (docs/architecture.md 8.2.5, "To do in S1b"),
// tested with a second instrument in a B-Side test build: the name comes from the instrument, a
// missing shared parameter or a missing initPresets call fails clearly at construction in every
// build type. (The static_asserts that the per-block hooks are noexcept are compile-time: a hook
// without noexcept does not build.)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "bside/PluginProcessor.h"
#include "bside/presets/BSidePresets.h"
#include "common/params/GlobalParams.h"

#include <string>

namespace
{
struct JuceEnv
{
    juce::ScopedJuceInitialiser_GUI init;
};

/** A minimal second instrument on the skeleton, with switches for the two mistakes. It reuses
    B-Side's part, parameters and presets; only its identity differs. */
class MiniProcessor final : public vs::shell::BiTimbralProcessor<MiniProcessor, bs::TestTonePart, bs::BSideParams>
{
    using Base = vs::shell::BiTimbralProcessor<MiniProcessor, bs::TestTonePart, bs::BSideParams>;
    friend Base;

public:
    static constexpr const char* kName = "Mini Test Instrument";
    static constexpr const char* kStateTag = "MiniTestState";
    static constexpr int kStateVersion = 1;
    static void migrateState (juce::ValueTree&) {}

    /** `layout` is the parameter list; `callInitPresets` false is the forgotten call. */
    MiniProcessor (juce::AudioProcessorValueTreeState::ParameterLayout layout, bool callInitPresets) : Base (std::move (layout))
    {
        if (callInitPresets)
        {
            auto schema = bs::makeBSideSchema (*this);
            auto factory = bs::makeBSideFactoryBank (schema);
            initPresets (std::move (schema), std::move (factory), "Mini Test");
        }
    }

private:
    void readInstrumentParams (bs::BSideParams&) const noexcept {}
    void applyInstrumentParams (const bs::BSideParams&) noexcept {}
    float partPan (const bs::BSideParams&, int) const noexcept { return 0.0f; }
    void prepareParts (double hostRate, int, int, int)
    {
        for (auto& p : parts)
            p.prepare (hostRate);
    }
    double synthTailSeconds() const noexcept { return 0.0; }
    bs::TestTonePart& part (int p) noexcept { return parts[static_cast<size_t> (p == 0 ? 0 : 1)]; }
    const bs::TestTonePart& part (int p) const noexcept { return parts[static_cast<size_t> (p == 0 ? 0 : 1)]; }

    std::array<bs::TestTonePart, 2> parts;
};

/** B-Side's layout without one group of the shared parameters (the appenders of common/params). */
enum class Group { MasterVolume, KeyMix, Effects, ArpSeq, Limiter };

juce::AudioProcessorValueTreeState::ParameterLayout layoutWithout (Group skipped)
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    if (skipped != Group::MasterVolume)
        vs::addMasterVolume (layout);
    if (skipped != Group::KeyMix)
        vs::addKeyMixParameters (layout);
    if (skipped != Group::Effects)
        vs::addFxParameters (layout);
    if (skipped != Group::ArpSeq)
        vs::addArpSeqParameters (layout);
    if (skipped != Group::Limiter)
        vs::addLimiterParameters (layout);
    return layout;
}
} // namespace

TEST_CASE ("Skeleton: the plugin name comes from the instrument, not from the build", "[shell][identity]")
{
    JuceEnv env;
    MiniProcessor mini (bs::createParameterLayout(), true);
    CHECK (mini.getName() == "Mini Test Instrument");
    CHECK (mini.getName() != JucePlugin_Name); // this test build is the B-Side plugin's

    bs::BSideProcessor bside;
    CHECK (bside.getName() == "Exitium B-Side");
    CHECK (bside.getName() == JucePlugin_Name);
    CHECK (juce::String (bs::BSideProcessor::kName) == JucePlugin_Name);
}

TEST_CASE ("Skeleton: a complete instrument constructs and verifies", "[shell][contract]")
{
    JuceEnv env;
    MiniProcessor mini (bs::createParameterLayout(), true);
    CHECK_NOTHROW (mini.verifyConstructed());
    CHECK (mini.getNumPrograms() == 64);
}

TEST_CASE ("Skeleton: a missing shared parameter fails at construction, naming it", "[shell][contract]")
{
    JuceEnv env;
    struct Case
    {
        Group skipped;
        const char* firstMissingId; // the first ID of that group the skeleton tries to bind
    };
    for (const auto& c : { Case { Group::MasterVolume, vs::ParamID::masterVolume }, Case { Group::KeyMix, vs::ParamID::keyMode },
                           Case { Group::Effects, "fx.chorus.mode" }, Case { Group::ArpSeq, "arp.mode" },
                           Case { Group::Limiter, vs::ParamID::masterLimiter } })
    {
        INFO ("missing group, first ID " << c.firstMissingId);
        try
        {
            MiniProcessor mini (layoutWithout (c.skipped), true);
            FAIL ("the constructor accepted a layout without " << c.firstMissingId);
        }
        catch (const vs::ConfigError& e)
        {
            CHECK (std::string (e.what()).find (c.firstMissingId) != std::string::npos);
        }
    }
}

TEST_CASE ("Skeleton: a forgotten initPresets call is reported by name at the first check", "[shell][contract]")
{
    JuceEnv env;
    MiniProcessor mini (bs::createParameterLayout(), false);
    REQUIRE_THROWS_AS (mini.verifyConstructed(), vs::ConfigError);
    try
    {
        mini.verifyConstructed();
    }
    catch (const vs::ConfigError& e)
    {
        CHECK (std::string (e.what()).find ("Mini Test Instrument") != std::string::npos);
        CHECK (std::string (e.what()).find ("initPresets") != std::string::npos);
    }
    // Every use of the presets reports the same mistake instead of dereferencing a null pointer.
    CHECK_THROWS_AS (mini.getNumPrograms(), vs::ConfigError);
    CHECK_THROWS_AS (mini.getProgramName (0), vs::ConfigError);
    juce::MemoryBlock state;
    CHECK_THROWS_AS (mini.getStateInformation (state), vs::ConfigError);
}
