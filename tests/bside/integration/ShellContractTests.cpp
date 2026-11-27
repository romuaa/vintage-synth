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
#include "common/shell/InstrumentEditor.h"

#include <string>

namespace
{
struct JuceEnv
{
    juce::ScopedJuceInitialiser_GUI init;
};

/** A minimal second instrument on the skeleton, with switches for the two mistakes. It reuses
    B-Side's part, parameters and presets; only its identity differs. */
class MiniProcessor final : public vs::shell::BiTimbralProcessor<MiniProcessor, bs::Part, bs::BSideParams>
{
    using Base = vs::shell::BiTimbralProcessor<MiniProcessor, bs::Part, bs::BSideParams>;
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
    void prepareParts (double hostRate, int maxBlock, int osFactor, int)
    {
        for (auto& p : parts)
            p.prepare (hostRate, maxBlock, osFactor, bs::PartParams {}, 0);
    }
    double synthTailSeconds() const noexcept { return 0.0; }
    bs::Part& part (int p) noexcept { return parts[static_cast<size_t> (p == 0 ? 0 : 1)]; }
    const bs::Part& part (int p) const noexcept { return parts[static_cast<size_t> (p == 0 ? 0 : 1)]; }

    std::array<bs::Part, 2> parts;
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

TEST_CASE ("Product name: B-Side's placeholder editor shows getName() and the project version on one line above the preset bar", "[shell][identity][editor]")
{
    JuceEnv env;
    bs::BSideProcessor bside;
    std::unique_ptr<juce::AudioProcessorEditor> made (bside.createEditor());
    auto* editor = dynamic_cast<vs::shell::InstrumentEditor*> (made.get());
    REQUIRE (editor != nullptr);

    const auto& name = editor->getProductName();
    CHECK (name.fullName() == bside.getName());
    CHECK (name.company == "Exitium");
    CHECK (name.product == "B-Side");
    CHECK (name.version == JucePlugin_VersionString);
    CHECK (editor->getNameLine().getText() == bside.getName() + "  " + JucePlugin_VersionString);
    CHECK (editor->getNameLine().getTitle() == bside.getName() + ", version " + JucePlugin_VersionString);
    const auto handler = editor->getNameLine().createAccessibilityHandler();
    REQUIRE (handler != nullptr);
    CHECK (handler->getRole() == juce::AccessibilityRole::staticText);

    // One line, above the preset bar (not over it), inside the window.
    const auto line = editor->getNameLine().getBounds();
    CHECK (line.getY() == 0);
    CHECK (line.getBottom() <= editor->getBar().getY());
    CHECK (line.getWidth() == editor->getWidth());
    CHECK (line.getHeight() == vs::shell::InstrumentEditor::kNameLineHeight);

    // The name is the processor's own, never a literal of the shared editor: a second instrument shows its own.
    MiniProcessor mini (bs::createParameterLayout(), true);
    std::unique_ptr<juce::AudioProcessorEditor> other (mini.createEditor());
    auto* otherEditor = dynamic_cast<vs::shell::InstrumentEditor*> (other.get());
    REQUIRE (otherEditor != nullptr);
    CHECK (otherEditor->getProductName().fullName() == "Mini Test Instrument");
    CHECK (otherEditor->getNameLine().getText() == "Mini Test Instrument");
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
    CHECK_THROWS_AS (mini.getPresets(), vs::ConfigError);
    juce::MemoryBlock state;
    CHECK_THROWS_AS (mini.getStateInformation (state), vs::ConfigError);
}
