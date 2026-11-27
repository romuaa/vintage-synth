// Paint allocations of the whole editor (docs/architecture.md 9.5): every component of every page, both
// parts, painted through a context that draws nothing, allocates nothing of our own. Complements the
// per-component tests of M7b-M7e (which stay): this one walks the tree, so a control or graph added
// later is covered without anyone remembering to add it.
//
// What is counted is only what our own paint code allocates (strings, paths, fonts built while
// painting): the context skips text layout and drawing, so JUCE's and the renderer's own allocations
// do not appear. Widgets that are JUCE's own (buttons, labels, text editors of the preset bar and
// the Browser page) are listed and not asserted: their drawing is the look-and-feel's.

#include <catch2/catch_test_macros.hpp>

#include "EditorWalk.h"
#include "NullGraphicsContext.h"
#include "common/ui/controls/Slider.h"

#include <set>

using namespace vs;
using namespace vs::itest;

#if VS_SEQ_COUNT_ALLOCATIONS
namespace
{
/** JUCE's own widgets (and what they contain): painted by the look-and-feel, not our code. */
bool isJuceWidget (const juce::Component& c)
{
    // A class of JUCE's own (TextButton, Label, TextEditor, ...): its name has the juce namespace in it, ours never does.
    // Our own components derived from a JUCE class (the preset name button) are measured.
    return typeNameOf (c).contains ("juce");
}

/** Allocations of two paints of `c` after a warm-up, or -1 when they differ. `focus`: with the focus ring. */
int allocationsOf (juce::Component& c, NullGraphicsContext& context, bool& stable)
{
    juce::Graphics g (context);
    const auto paintIt = [&] {
        c.paint (g);
        c.paintOverChildren (g);
    };
    paintIt(); // warm-up: lazily made things are not paint()'s own
    int counts[3];
    for (int& n : counts)
    {
        vs::seq::test::AllocationCounter counter;
        paintIt();
        n = counter.count();
    }
    stable = counts[0] == counts[1] && counts[1] == counts[2];
    return counts[0];
}

struct Walked
{
    int components = 0, measured = 0, juceOwn = 0;
    std::set<std::string> types, juceTypes;
};

/** Paints every visible component below the editor on the current page; returns what was covered. */
void measurePage (EditorRig& rig, Walked& walked, const juce::String& state)
{
    NullGraphicsContext context;
    forEachVisible (*rig.editor, [&] (juce::Component& c) {
        ++walked.components;
        if (isJuceWidget (c))
        {
            ++walked.juceOwn;
            walked.juceTypes.insert (typeNameOf (c).toStdString());
            return;
        }
        // The widgets inside a JUCE widget (a button's label) are JUCE's too.
        for (auto* p = c.getParentComponent(); p != nullptr; p = p->getParentComponent())
            if (isJuceWidget (*p))
                return;
        // The one known exception (docs/decisions.md, M7f): the dashed outline of the external-change marker is built in
        // paint() while a marker shows (2 s after a change from outside). The test lets that window pass before it
        // measures, and fails here if a marker is still showing, so a measured component never hides the exception.
        if (auto* bound = dynamic_cast<ui::BoundComponent*> (&c))
            CHECK (bound->getMarkerAlpha() <= 0.0f);
        ++walked.measured;
        walked.types.insert (typeNameOf (c).toStdString());
        for (const bool focused : { false, true })
        {
            if (focused)
            {
                if (! c.getWantsKeyboardFocus())
                    continue;
                c.grabKeyboardFocus();
            }
            bool stable = false;
            const int n = allocationsOf (c, context, stable);
            INFO (state << ", " << typeNameOf (c) << " \"" << c.getComponentID() << "\" \"" << c.getTitle() << "\"" << (focused ? ", focused" : "")
                        << ": " << n << " allocations per paint");
            CHECK (stable);
            CHECK (n == 0);
        }
    });
}
} // namespace

TEST_CASE ("Paint allocations: no component of any page builds a string, path or font while painting", "[ui][alloc][paint][allpages]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.editor->setVisible (true);
    Walked walked;

    const auto sweep = [&] (const juce::String& state) {
        for (const auto& [page, part] : allPages())
        {
            showPage (rig, page, part);
            rig.tick (2200.0); // the library draws a dashed marker (and allocates) for 2 s after a change from outside
            rig.tick (100.0);
            measurePage (rig, walked, state + ", page " + juce::String (page) + " part " + juce::String (part));
        }
    };

    sweep ("defaults");

    // A busy state: Split with held keys, an arpeggio and a sequencer step, all effects on, a part
    // detuned, so the branches that draw only in those states (marker aside) are reached.
    rig.set ("key.mode", 2.0f);
    rig.set ("arp.mode", 1.0f);
    rig.set ("seq.step03.type", 1.0f);
    rig.set ("seq.step05.type", 2.0f);
    rig.set ("fx.chorus.mode", 1.0f);
    rig.set ("fx.delay.level", 0.4f);
    rig.set ("fx.reverb.level", 0.4f);
    rig.set ("lower.lfo.waveform", 4.0f);
    rig.set ("lower.lfo.delay", 0.4f);
    rig.set ("lower.env1.polarity", 1.0f);
    rig.set ("lower.vco2.waveform", 2.0f);
    rig.set ("upper.vcf.cutoff", 0.2f);
    rig.render (rig.samples (0.05), { noteOn (static_cast<int> (rig.now), 50), noteOn (static_cast<int> (rig.now), 70) }, 128);
    sweep ("busy, arpeggiator");
    rig.set ("arp.mode", 2.0f);
    sweep ("busy, sequencer");

    // The popover, the hover help and the help bubble are on top of the page: measured over every page.
    {
        auto& frame = rig.editor->getFrame();
        for (const auto& [page, part] : allPages())
        {
            showPage (rig, page, part);
            frame.openOutputPopover();
            frame.hoverHelp (&frame.getMasterSlider()); // the help line shows the hovered control's text
            frame.getHelpBubble().showFor ("A help bubble: a few sentences long enough to wrap onto a second and a third line inside its box.",
                                           frame.getLocalArea (&frame.getMasterSlider(), frame.getMasterSlider().getLocalBounds()),
                                           frame.getLocalBounds().reduced (4));
            REQUIRE (frame.getHelpBubble().isVisible());
            rig.tick (2200.0);
            rig.tick (100.0);
            measurePage (rig, walked, "popover open, help bubble shown, hover, page " + juce::String (page) + " part " + juce::String (part));
            frame.closeOutputPopover();
        }
    }

    // The walk reached what it should: a few classes from each corner of the editor.
    for (const char* name : { "Slider", "SegmentedSwitch", "Toggle", "Panel", "ResponseCurveView", "ScopeView", "KeyboardStrip", "StepGrid", "ProgramGrid",
                              "ChipStrip", "PageRail", "ProductBlock", "PartSwitch", "OutputMeter", "HelpLine", "HelpBubble", "OutputPopover", "EnvelopeView", "LfoView", "PianoRollView" })
    {
        bool found = false;
        for (const auto& t : walked.types)
            found |= juce::String (t).contains (name);
        INFO ("class " << name << " was not reached by the walk");
        CHECK (found);
    }
    INFO (walked.components << " components visited, " << walked.measured << " measured, " << walked.juceOwn << " JUCE widgets skipped");
    CHECK (walked.measured > 200);
}
#endif

