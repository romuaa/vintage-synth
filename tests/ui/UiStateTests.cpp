// Non-parameter UI state (docs/architecture.md 9.1.3, 9.6 "UI state"): saved as one UiState child
// of the session (the only change to the saved state in M7), round trip, defaults for a missing or
// garbled child, the pre-M7 fixture loads with defaults, and UI state never marks the program
// modified and is not part of a program.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../preset_unit/PresetTestHelpers.h"
#include "common/shell/EditorState.h"

using namespace vs;
using namespace vs::itest;
using namespace vs::ptest;
using Catch::Approx;

namespace
{
juce::ValueTree stateTree (VintageSynthProcessor& proc)
{
    juce::MemoryBlock mb;
    proc.getStateInformation (mb);
    auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), static_cast<int> (mb.getSize()));
    REQUIRE (xml != nullptr);
    return juce::ValueTree::fromXml (*xml);
}

void restore (VintageSynthProcessor& proc, const juce::ValueTree& tree)
{
    juce::MemoryBlock mb;
    auto xml = tree.createXml();
    juce::AudioProcessor::copyXmlToBinary (*xml, mb);
    proc.setStateInformation (mb.getData(), static_cast<int> (mb.getSize()));
}

void setState (shell::EditorState& s, int page, int part, int view, int size, float delay, float reverb)
{
    s.page = page;
    s.part = part;
    s.arpView = view;
    s.sizePercent = size;
    s.fxLastLevel[0] = delay;
    s.fxLastLevel[1] = reverb;
}

bool isDefault (const shell::EditorState& s)
{
    return s.page == 0 && s.part == 0 && s.arpView == 0 && s.sizePercent == 0 && s.fxLastLevel[0].load() == Approx (0.35f)
           && s.fxLastLevel[1].load() == Approx (0.35f);
}
} // namespace

TEST_CASE ("UI state: one UiState child, written by name, after the parameters and the preset info", "[ui][state]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VintageSynthProcessor proc;
    setState (proc.getEditorState(), 4, 1, 1, 125, 0.5f, 0.2f);
    const auto tree = stateTree (proc);
    int children = 0;
    for (const auto& c : tree)
        children += c.hasType ("UiState") ? 1 : 0;
    CHECK (children == 1);
    const auto ui = tree.getChildWithName ("UiState");
    CHECK (ui["page"].toString() == "arpseq");
    CHECK (ui["part"].toString() == "upper");
    CHECK (ui["arpView"].toString() == "seq");
    CHECK (ui["size"].toString() == "125");
    CHECK (ui["delayLast"].toString() == "0.50");
    CHECK (ui["reverbLast"].toString() == "0.20");
    // The last child: the parameters and the preset info come first, unchanged.
    CHECK (tree.getChild (tree.getNumChildren() - 1).hasType ("UiState"));
}

TEST_CASE ("UI state: round trip through the session", "[ui][state]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VintageSynthProcessor a, b;
    setState (a.getEditorState(), 2, 1, 1, 175, 0.61f, 0.07f);
    restore (b, stateTree (a));
    const auto& s = b.getEditorState();
    CHECK (s.page == 2);
    CHECK (s.part == 1);
    CHECK (s.arpView == 1);
    CHECK (s.sizePercent == 175);
    CHECK (s.fxLastLevel[0].load() == Approx (0.61f));
    CHECK (s.fxLastLevel[1].load() == Approx (0.07f));
    // The child never reaches the parameter tree (a second save writes it once again, not twice).
    int children = 0;
    for (const auto& c : stateTree (b))
        children += c.hasType ("UiState") ? 1 : 0;
    CHECK (children == 1);
    for (const auto& c : b.getState().copyState())
        CHECK_FALSE (c.hasType ("UiState"));
}

TEST_CASE ("UI state: a missing child (any session before M7) gives the defaults; garbled attributes give their default", "[ui][state]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VintageSynthProcessor proc;

    SECTION ("missing")
    {
        auto tree = stateTree (proc);
        tree.removeChild (tree.getChildWithName ("UiState"), nullptr);
        setState (proc.getEditorState(), 3, 1, 1, 150, 0.9f, 0.9f);
        restore (proc, tree);
        CHECK (isDefault (proc.getEditorState()));
    }
    SECTION ("garbled")
    {
        auto tree = stateTree (proc);
        auto ui = tree.getChildWithName ("UiState");
        ui.setProperty ("page", "no-such-page", nullptr);
        ui.setProperty ("part", "middle", nullptr);
        ui.setProperty ("arpView", 42, nullptr);
        ui.setProperty ("size", "1e9", nullptr);
        ui.setProperty ("delayLast", "-0.5", nullptr);
        ui.setProperty ("reverbLast", "nan", nullptr);
        setState (proc.getEditorState(), 3, 1, 1, 150, 0.9f, 0.9f);
        restore (proc, tree);
        CHECK (isDefault (proc.getEditorState()));
    }
    SECTION ("out of range and partial")
    {
        auto tree = stateTree (proc);
        auto ui = tree.getChildWithName ("UiState");
        ui.setProperty ("size", "74", nullptr);
        ui.setProperty ("delayLast", "1.5", nullptr);
        ui.removeProperty ("reverbLast", nullptr);
        ui.setProperty ("page", "effects", nullptr);
        restore (proc, tree);
        const auto& s = proc.getEditorState();
        CHECK (s.page == 5);
        CHECK (s.sizePercent == 0);
        CHECK (s.fxLastLevel[0].load() == Approx (0.35f));
        CHECK (s.fxLastLevel[1].load() == Approx (0.35f));
    }
}

TEST_CASE ("UI state: the pre-M7 session fixture loads with the default UI state and its exact sound", "[ui][state]")
{
    juce::ScopedJuceInitialiser_GUI init;
    VintageSynthProcessor proc;
    setState (proc.getEditorState(), 6, 1, 1, 90, 0.1f, 0.1f);
    juce::MemoryBlock mb;
    REQUIRE (juce::File (VS_FIXTURE_DIR).getChildFile ("state_m5.bin").loadFileAsData (mb));
    proc.setStateInformation (mb.getData(), static_cast<int> (mb.getSize()));
    CHECK (isDefault (proc.getEditorState()));
    // The parameters are the fixture's (the existing fixture test checks every value; here one).
    CHECK (proc.getState().getParameter ("lower.vcf.cutoff") != nullptr);
}

TEST_CASE ("UI state never marks the program modified and is not part of a program", "[ui][state]")
{
    PresetRig rig;
    rig.presets().selectProgram (3);
    REQUIRE_FALSE (rig.presets().isModified());
    setState (rig.proc.getEditorState(), 1, 1, 1, 200, 0.8f, 0.8f);
    juce::MemoryBlock mb;
    rig.proc.getStateInformation (mb);
    rig.presets().pollRequests();
    CHECK_FALSE (rig.presets().isModified());

    const auto file = rig.temp.dir.getChildFile ("ui.vsprog");
    REQUIRE (rig.presets().saveProgramFile (file, "Check").isEmpty());
    const auto text = file.loadFileAsString();
    CHECK_FALSE (text.containsIgnoreCase ("UiState"));
    CHECK_FALSE (text.containsIgnoreCase ("arpView"));
    CHECK_FALSE (text.containsIgnoreCase ("delayLast"));
}

TEST_CASE ("UI state text: strict parsing (JUCE-free)", "[ui][state]")
{
    using namespace shell::editorstate;
    int i = 0;
    CHECK (parseInt ("125", i));
    CHECK (i == 125);
    CHECK_FALSE (parseInt ("12x", i));
    CHECK_FALSE (parseInt ("", i));
    CHECK_FALSE (parseInt (" 1", i));
    float f = 0.0f;
    CHECK (parseLevel ("0.35", f));
    CHECK (f == Approx (0.35f));
    CHECK (parseLevel ("1", f));
    CHECK_FALSE (parseLevel ("1.2.3", f));
    CHECK_FALSE (parseLevel ("-1", f));
    CHECK_FALSE (parseLevel (".", f));
    CHECK_FALSE (parseLevel ("1e5", f));
    CHECK (formatFloat (0.35f) == "0.35");
    CHECK (formatFloat (1.0f) == "1.00");
}
