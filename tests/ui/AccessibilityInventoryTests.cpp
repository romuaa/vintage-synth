// Accessibility and keyboard inventory (docs/architecture.md 9.2.5): walks the whole component tree of
// the editor on every page and fails for any component that takes mouse clicks but has no accessibility
// handler with a role and a non-empty name, or cannot take keyboard focus, or cannot be reached by
// walking the focus order (Tab). Pure display components and containers are listed with a reason.

#include <catch2/catch_test_macros.hpp>

#include "EditorWalk.h"

#include <set>

using namespace vs;
using namespace vs::itest;

namespace
{
enum class Kind
{
    Container, // takes clicks only to pass them on or to dismiss: its children are the interactive things and are checked themselves
    Alternative, // takes clicks for the mouse only; the keyboard has another control that does the same
    Display    // shows something and has an accessibility handler with a name, but no action: no focus needed
};

struct Allowed
{
    const char* typeName; // the class's own name (not its enclosing classes)
    Kind kind;
    const char* reason;
};

// Every entry says why the component is not itself a keyboard target.
const Allowed kAllowList[] {
    { "VintageEditor", Kind::Container, "the editor window: the frame and the pages inside it are checked" },
    { "EditorFrame", Kind::Container, "the frame takes clicks only for the hover help; every control in it is checked" },
    { "SoundPage", Kind::Container, "a page: its controls are checked" },
    { "EnvelopesPage", Kind::Container, "a page: its controls are checked" },
    { "LfoPage", Kind::Container, "a page: its controls are checked" },
    { "PlayPage", Kind::Container, "a page: its controls are checked" },
    { "ArpSeqPage", Kind::Container, "a page: its controls are checked" },
    { "EffectsPage", Kind::Container, "a page: its controls are checked" },
    { "BrowserPage", Kind::Container, "a page: its controls are checked" },
    { "PageRail", Kind::Container, "groups the page buttons, each checked" },
    { "PresetBar", Kind::Container, "the bar groups the preset buttons, each checked" },
    { "SignalPath", Kind::Container, "the signal-path picture; its one button is checked" },
    { "StepGrid", Kind::Container, "its pitch and velocity bars, Rest/Tie toggles and length flag are the focus targets and are checked" },
    { "StepTypeToggle", Kind::Container, "its two buttons (Rest, Tie) are the focus targets and are checked" },
    { "ResponseCurveView", Kind::Container, "a graph: its handles are the focus targets and are checked" },
    { "EnvelopeView", Kind::Container, "a graph: its handles are the focus targets and are checked" },
    { "LfoView", Kind::Container, "a graph: its handles are the focus targets and are checked" },
    { "ChorusView", Kind::Display, "a picture of the chorus (role image, text summary); its handle, if any, is checked" },
    { "DelayView", Kind::Container, "a graph: its handles are the focus targets and are checked" },
    { "ReverbView", Kind::Container, "a graph: its handles are the focus targets and are checked" },
    { "ScopeView", Kind::Display, "oscilloscope picture (role image, named by the waveform); nothing to operate" },
    { "PianoRollView", Kind::Display, "the arpeggio preview (role image with a text summary of the notes); nothing to operate" },
    { "OutputMeter", Kind::Display, "level meter (progress bar with the level as its value); its click opens the output settings, which the LIMIT button and the arrow button open from the keyboard" },
    { "ResizableCornerComponent", Kind::Alternative, "the corner resize grip is for the mouse; the Size button and its menu set the same sizes from the keyboard" },
    { "OutputPopover", Kind::Container, "the output settings dialog: its controls are checked, Esc closes it" },
    { "Scrim", Kind::Container, "hidden from accessibility; a click outside the open output settings closes them, Esc does the same from the keyboard" },
};

const Allowed* allowedFor (const juce::Component& c)
{
    const auto name = classNameOf (c);
    for (const auto& a : kAllowList)
        if (name == a.typeName)
            return &a;
    return nullptr;
}

bool takesClicks (const juce::Component& c)
{
    bool onThis = false, onChildren = false;
    c.getInterceptsMouseClicks (onThis, onChildren);
    return onThis && c.isEnabled();
}

/** The components the Tab key visits, starting from the editor's default component. */
std::set<const juce::Component*> focusOrder (juce::Component& root)
{
    std::set<const juce::Component*> visited;
    auto traverser = root.createFocusTraverser();
    REQUIRE (traverser != nullptr);
    auto* c = traverser->getDefaultComponent (&root);
    for (int guard = 0; c != nullptr && guard < 2000 && visited.insert (c).second; ++guard)
        c = traverser->getNextComponent (c);
    return visited;
}

struct Counts
{
    int clickable = 0, containers = 0, displays = 0, targets = 0, nameBlocks = 0;
    std::set<std::string> targetTypes;
};

void inspect (EditorRig& rig, Counts& counts, const juce::String& where)
{
    const auto order = focusOrder (*rig.editor);
    // The product name (9.3.8) is static text, announced once per editor: not clickable, so the walk below does not list it.
    int nameBlocks = 0;
    forEachVisible (*rig.editor, [&] (juce::Component& c) {
        if (classNameOf (c) != "ProductBlock")
            return;
        ++nameBlocks;
        INFO (where << ": the product name block");
        const auto owned = c.createAccessibilityHandler();
        REQUIRE (owned != nullptr);
        CHECK (owned->getRole() == juce::AccessibilityRole::staticText);
        CHECK (owned->getTitle() == juce::String (VintageSynthProcessor::kName) + ", version " + JucePlugin_VersionString);
        CHECK (c.isAccessible());
        CHECK (! takesClicks (c));
        CHECK (! c.getWantsKeyboardFocus());
        CHECK (order.count (&c) == 1); // in the traversal order, so a screen reader reaches it (it is not a Tab stop: it does not want focus)
    });
    CHECK (nameBlocks == 1);
    counts.nameBlocks += nameBlocks;
    forEachVisible (*rig.editor, [&] (juce::Component& c) {
        if (! takesClicks (c))
            return;
        ++counts.clickable;
        const auto* allowed = allowedFor (c);
        if (allowed != nullptr && (allowed->kind == Kind::Container || allowed->kind == Kind::Alternative))
        {
            ++counts.containers;
            return;
        }
        INFO (where << ": " << typeNameOf (c) << " id \"" << c.getComponentID() << "\" title \"" << c.getTitle() << "\"");
        // (a handler is attached only to components on the desktop: ask the component for the one it would attach)
        const auto owned = c.createAccessibilityHandler();
        const auto* handler = owned.get();
        CHECK (c.isAccessible());
        if (handler == nullptr)
        {
            FAIL_CHECK ("no accessibility handler");
            return;
        }
        CHECK (handler->getRole() != juce::AccessibilityRole::unspecified);
        CHECK (handler->getRole() != juce::AccessibilityRole::ignored);
        CHECK (handler->getTitle().isNotEmpty());
        if (allowed != nullptr)
        {
            ++counts.displays;
            return;
        }
        ++counts.targets;
        counts.targetTypes.insert (typeNameOf (c).toStdString());
        CHECK (c.getWantsKeyboardFocus());
        CHECK (order.count (&c) == 1); // Tab reaches it
    });
}
} // namespace

TEST_CASE ("Accessibility inventory: every clickable component on every page has a role and a name and is reached by Tab", "[ui][a11y][inventory]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.editor->setVisible (true);
    rig.editor->setSize (1360, 784);
    Counts counts;
    const auto sweep = [&] (const juce::String& state) {
        for (const auto& [page, part] : allPages())
        {
            showPage (rig, page, part);
            inspect (rig, counts, state + ", page " + juce::String (page) + " part " + juce::String (part));
        }
    };
    sweep ("defaults");
    rig.set ("arp.mode", 2.0f);
    rig.set ("key.mode", 2.0f);
    sweep ("sequencer, split");
    rig.set ("arp.mode", 1.0f);
    sweep ("arpeggiator");
    rig.editor->getFrame().openOutputPopover();
    sweep ("output settings open");
    rig.editor->getFrame().closeOutputPopover();

    INFO (counts.clickable << " clickable components, " << counts.containers << " containers, " << counts.displays << " displays, " << counts.targets
                           << " keyboard targets");
    CHECK (counts.targets > 300);
    CHECK (counts.nameBlocks > 30); // one on each page of each sweep
    // The inventory reaches the kinds of control this test exists for.
    for (const char* name : { "Slider", "SegmentedSwitch", "Toggle", "PowerSwitch", "KeyboardStrip", "ProgramGrid", "ChipStrip", "ToneSourceSwitch",
                              "PartSwitch", "Button", "GraphHandle" })
    {
        bool found = false;
        for (const auto& t : counts.targetTypes)
            found |= juce::String (t).contains (name);
        INFO ("class " << name << " was not reached by the inventory");
        CHECK (found);
    }
}
