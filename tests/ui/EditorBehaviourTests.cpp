// Editor behaviour checks added after the M7a review (docs/architecture.md 9.1.2, 9.1.5, 9.2):
// repaint scope of one automated parameter, gestures of a page that goes away, the marker's own
// look, accessibility basics (focus order, checked states, handle values, meter announcements),
// keys, wheel, double-click and right-click behaviour, the AUTO badge layout, the high-pass handle
// over its whole range, self-oscillation on the curve, and the window size of a restored session.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EditorRig.h"

#include <set>
#include <thread>

using namespace vs;
using namespace vs::itest;
using Catch::Approx;

namespace
{
void hostWrite (juce::RangedAudioParameter& p, float normalised)
{
    std::thread ([&p, normalised] { p.setValueNotifyingHost (normalised); }).join();
}

const ui::ControlEntry* primaryOf (const ui::ControlRegistry& reg, const juce::String& id)
{
    for (const auto& e : reg.entries())
        if (e.paramId == id && e.placement == ui::Placement::Primary)
            return &e;
    return nullptr;
}

template <class T>
T& controlFor (ui::VintageEditor& ed, const juce::String& id)
{
    auto* e = primaryOf (ed.getRegistry(), id);
    REQUIRE (e != nullptr);
    auto* c = dynamic_cast<T*> (const_cast<ui::BoundControl*> (e->view));
    REQUIRE (c != nullptr);
    return *c;
}

/** Records every area the editor is asked to repaint (installed as its cached image). */
struct RepaintRecorder final : juce::CachedComponentImage
{
    void paint (juce::Graphics&) override {}
    bool invalidateAll() override
    {
        ++all;
        return false;
    }
    bool invalidate (const juce::Rectangle<int>& area) override
    {
        areas.push_back (area);
        return false;
    }
    void releaseResources() override {}
    void clear()
    {
        areas.clear();
        all = 0;
    }
    std::vector<juce::Rectangle<int>> areas;
    int all = 0;
};

juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<float> pos, juce::ModifierKeys mods, int clicks)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pos, now,
                             clicks, false);
}
} // namespace

TEST_CASE ("Repaint scope: one automated parameter repaints only its own controls and pictures, never the whole page", "[ui][editor][repaint]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& mirror = rig.editor->getMirror();
    rig.editor->setSizePercent (100); // repaint areas are in editor pixels, the page area in logical ones
    rig.editor->setVisible (true);    // an invisible component forwards no repaint request to its cached image
    auto* recorder = new RepaintRecorder();
    rig.editor->setCachedComponentImage (recorder); // owned by the editor
    const auto page = rig.editor->getFrame().pageArea();
    const int pageArea = page.getWidth() * page.getHeight();
    for (int part = 0; part < 2; ++part)
    {
        rig.editor->selectPage (0, part);
        rig.tick();
        std::set<juce::String> shown; // parameters with a control on the Sound page of this part
        for (const auto& e : rig.editor->getRegistry().entries())
            if (e.placement == ui::Placement::Primary && e.page == "sound" && e.part == part)
                shown.insert (e.paramId);
        for (int i = 0; i < mirror.size(); ++i)
        {
            auto& p = mirror.parameter (i);
            rig.tick (3000.0); // earlier markers have faded: only this change repaints
            hostWrite (p, p.getValue() > 0.5f ? 0.1f : 0.9f);
            recorder->clear();
            rig.tick();
            INFO ("part " << part << ": " << p.paramID);
            CHECK (recorder->all == 0);
            if (shown.count (p.paramID) > 0)
                CHECK (! recorder->areas.empty()); // a control that is on screen does repaint
            juce::RectangleList<int> repainted; // the union: overlapping requests count once
            for (const auto& a : recorder->areas)
            {
                CHECK (a.getWidth() * a.getHeight() < pageArea / 3); // the filter graph is the largest
                repainted.add (a);
            }
            long long total = 0;
            for (const auto& r : repainted)
                total += static_cast<long long> (r.getWidth()) * r.getHeight();
            CHECK (total < pageArea / 2);
        }
    }
    // Cross mod repaints its slider and the small caption only.
    rig.tick (3000.0);
    hostWrite (rig.param ("upper.vco1.cross_mod"), 0.0f);
    recorder->clear();
    rig.tick();
    CHECK (! recorder->areas.empty());
    for (const auto& a : recorder->areas)
        CHECK (a.getHeight() <= 160);
    rig.editor->setCachedComponentImage (nullptr);
}

TEST_CASE ("A page that goes away ends its gestures exactly once; nothing is written afterwards", "[ui][editor][gestures]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    GestureCounter host (rig.proc);
    auto& slider = controlFor<ui::Slider> (*rig.editor, "lower.vcf.cutoff");
    auto& m = slider.getModel();

    SECTION ("keyboard page switch during a drag")
    {
        m.mouseDown (slider.thumbBounds().getCentreX(), true, false);
        m.mouseDrag (slider.thumbBounds().getCentreX() + 20.0f, false);
        REQUIRE (host.openCount() == 1);
        const int values = host.values;
        rig.editor->selectPage (1, 0);
        CHECK (host.openCount() == 0);
        CHECK (host.ends == 1);
        m.mouseDrag (slider.thumbBounds().getCentreX() + 60.0f, false); // late events of the gone drag
        m.mouseUp();
        CHECK (host.values == values);
        CHECK (host.ends == 1);
    }
    SECTION ("a session restore changes the page during a drag")
    {
        rig.editor->selectPage (2, 0);
        juce::MemoryBlock state;
        rig.proc.getStateInformation (state);
        rig.editor->selectPage (0, 0);
        m.mouseDown (slider.thumbBounds().getCentreX(), true, false);
        m.mouseDrag (slider.thumbBounds().getCentreX() + 20.0f, false);
        std::thread ([&] { rig.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize())); }).join();
        rig.tick();
        CHECK (rig.editor->getFrame().getPage() == 2);
        CHECK (host.openCount() == 0);
        CHECK (host.ends == 1);
    }
    CHECK (host.begins == host.ends);
    CHECK (host.nestedBegins == 0);
}

TEST_CASE ("The external-change marker has its own token, not the focus colour", "[ui][editor][theme]")
{
    CHECK (ui::colour (ui::Tok::Marker) != ui::colour (ui::Tok::Focus));
    CHECK (ui::colour (ui::Tok::Marker) != ui::colour (ui::Tok::AccentLower));
    CHECK (ui::colour (ui::Tok::Marker) != ui::colour (ui::Tok::AccentUpper));
    // At least 3:1 against the panel (non-text contrast).
    const auto lum = [] (juce::Colour c) {
        const auto ch = [] (float v) { return v <= 0.03928f ? v / 12.92f : std::pow ((v + 0.055f) / 1.055f, 2.4f); };
        return 0.2126f * ch (c.getFloatRed()) + 0.7152f * ch (c.getFloatGreen()) + 0.0722f * ch (c.getFloatBlue());
    };
    const float ratio = (lum (ui::colour (ui::Tok::Marker)) + 0.05f) / (lum (ui::colour (ui::Tok::BgPanel)) + 0.05f);
    CHECK (ratio >= 3.0f);
}

TEST_CASE ("Accessibility basics: focus order starts at the rail, toggles and rail buttons report checked, handles have values, the meter is quiet", "[ui][editor][a11y]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& ed = *rig.editor;

    juce::KeyboardFocusTraverser traverser;
    const auto order = traverser.getAllComponents (&ed);
    REQUIRE (order.size() > 10);
    for (int i = 0; i < 7; ++i)
        CHECK (order[static_cast<size_t> (i)]->getComponentID().startsWith ("page."));
    CHECK (order[7]->getComponentID() == "ui.part");

    auto& sync = controlFor<ui::Toggle> (ed, "lower.vco2.sync");
    // (Off screen there is no live handler: create one as the platform would.)
    auto hOwned = sync.createAccessibilityHandler();
    auto* h = hOwned.get();
    REQUIRE (h != nullptr);
    CHECK (h->getCurrentState().isCheckable());
    CHECK_FALSE (h->getCurrentState().isChecked());
    sync.getModel().press();
    sync.getModel().release();
    CHECK (rig.param ("lower.vco2.sync").getValue() == Approx (1.0f));
    rig.tick();
    CHECK (h->getCurrentState().isChecked());

    auto& rail = ed.getFrame().getRail();
    REQUIRE (rail.getNumChildComponents() == 7);
    auto soundOwned = rail.getChildComponent (0)->createAccessibilityHandler();
    auto lfoOwned = rail.getChildComponent (2)->createAccessibilityHandler();
    auto* sound = soundOwned.get();
    auto* lfo = lfoOwned.get();
    REQUIRE (sound != nullptr);
    REQUIRE (lfo != nullptr);
    CHECK (sound->getCurrentState().isChecked());
    CHECK_FALSE (lfo->getCurrentState().isChecked());

    auto& handle = ed.getSoundPage (0).getCurveView().getHandle (0);
    auto hhOwned = handle.createAccessibilityHandler();
    auto* hh = hhOwned.get();
    REQUIRE (hh != nullptr);
    REQUIRE (hh->getValueInterface() != nullptr);
    CHECK (hh->getValueInterface()->getCurrentValueAsString() == handle.valueText());
    CHECK (handle.valueText().startsWith (rig.param ("lower.vcf.cutoff").getCurrentValueAsText()));
    hh->getValueInterface()->setValueAsString ("2000");
    CHECK (map::cutoffHz (rig.param ("lower.vcf.cutoff").getValue()) == Approx (2000.0f).epsilon (0.01));
    const float before = rig.param ("lower.vcf.cutoff").getValue();
    hh->getValueInterface()->setValueAsString ("bright please"); // unreadable: nothing changes
    CHECK (rig.param ("lower.vcf.cutoff").getValue() == Approx (before));

    // The meter speaks at most once per second however much the level moves.
    auto& meter = ed.getFrame().getMeter();
    const int before0 = meter.announcementCount();
    for (int i = 0; i < 60; ++i) // two seconds at 30 Hz
        meter.update (i % 2 == 0 ? 0.9f : 0.05f, 0.5f, 5000.0 + i * 33.4);
    CHECK (meter.announcementCount() - before0 <= 3);
}

TEST_CASE ("Keys, wheel, double-click and right-click behave as reviewed", "[ui][editor][input]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& ed = *rig.editor;

    // Delete / Backspace are not reset keys (a host may forward them meant for a clip).
    auto& res = controlFor<ui::Slider> (ed, "lower.vcf.resonance");
    hostWrite (rig.param ("lower.vcf.resonance"), 0.8f);
    rig.tick();
    CHECK_FALSE (res.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)));
    CHECK_FALSE (res.keyPressed (juce::KeyPress (juce::KeyPress::backspaceKey)));
    CHECK (rig.param ("lower.vcf.resonance").getValue() == Approx (0.8f));
    auto& wave = controlFor<ui::SegmentedSwitch> (ed, "lower.vco1.waveform");
    CHECK_FALSE (wave.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)));

    // Double-click on a segment selects that segment (no reset).
    const auto seg4 = wave.segmentBounds (4).getCentre();
    wave.mouseDown (mouseEvent (wave, seg4, {}, 1));
    wave.mouseUp (mouseEvent (wave, seg4, {}, 1));
    wave.mouseDown (mouseEvent (wave, seg4, {}, 2));
    wave.mouseDoubleClick (mouseEvent (wave, seg4, {}, 2));
    wave.mouseUp (mouseEvent (wave, seg4, {}, 2));
    rig.tick();
    CHECK (wave.litSegment() == 4);

    // Wheel on a switch: small trackpad deltas add up; reversed scrolling is honoured.
    juce::MouseWheelDetails w {};
    w.deltaY = 0.05f; // 0.4 notch
    wave.mouseWheelMove (mouseEvent (wave, seg4, {}, 0), w);
    wave.mouseWheelMove (mouseEvent (wave, seg4, {}, 0), w);
    CHECK (wave.getModel().index() == 4); // 0.8 notch: no step yet
    wave.mouseWheelMove (mouseEvent (wave, seg4, {}, 0), w);
    CHECK (wave.getModel().index() == 5); // 1.2 notches: one step
    // The component's wheel uses the real clock: end its gesture on that clock.
    rig.editor->tick (juce::Time::getMillisecondCounterHiRes() + 400.0);
    w.deltaY = 0.125f;
    w.isReversed = true;
    wave.mouseWheelMove (mouseEvent (wave, seg4, {}, 0), w);
    CHECK (wave.getModel().index() == 4); // reversed: down
    rig.editor->tick (juce::Time::getMillisecondCounterHiRes() + 400.0);

    // Right-click on a graph handle never drags.
    GestureCounter host (rig.proc);
    auto& handle = ed.getSoundPage (0).getCurveView().getHandle (0);
    const auto c = handle.getLocalBounds().getCentre().toFloat();
    handle.mouseDown (mouseEvent (handle, c, juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier | juce::ModifierKeys::popupMenuClickModifier), 1));
    handle.mouseDrag (mouseEvent (handle, c + juce::Point<float> (40.0f, -30.0f),
                                  juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier | juce::ModifierKeys::popupMenuClickModifier), 1));
    handle.mouseUp (mouseEvent (handle, c, {}, 1));
    CHECK (host.begins == 0);
    CHECK (host.values == 0);
    juce::PopupMenu::dismissAllActiveMenus();
}

TEST_CASE ("The AUTO badge never overlaps the label or the value, on every slider of every page", "[ui][editor][layout]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    int checked = 0;
    for (const auto& e : rig.editor->getRegistry().entries())
        if (auto* s = dynamic_cast<ui::Slider*> (const_cast<ui::BoundControl*> (e.view)))
        {
            auto& p = rig.param (e.paramId);
            hostWrite (p, p.getValue() > 0.5f ? 0.2f : 0.8f);
            rig.tick();
            INFO (e.paramId);
            REQUIRE (s->getMarkerAlpha() > 0.0f);
            const auto badge = s->badgeBounds();
            if (badge.isEmpty())
                continue; // narrow or vertical: ring only
            CHECK_FALSE (badge.intersects (s->labelBounds()));
            CHECK_FALSE (badge.intersects (s->valueBounds()));
            CHECK (s->getLocalBounds().toFloat().contains (badge));
            ++checked;
        }
    // Counted: the sliders whose AUTO badge fits (horizontal sliders wide enough), on this branch 97: Sound
    // (both parts), the placeholder pages, the Arp + Seq page's gate, swing, tempo and length. The step bars are
    // not sliders. A page that stops being a placeholder lowers this number; set it just under the real count.
    CHECK (checked > 96);
}

TEST_CASE ("High-pass handle: moves for every value change over its whole range and stays inside the graph", "[ui][editor][graphs]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& view = rig.editor->getSoundPage (0).getCurveView();
    auto& handle = view.getHandle (1);
    float lastX = -1.0e9f;
    for (int i = 0; i <= 40; ++i)
    {
        hostWrite (rig.param ("lower.hpf.cutoff"), static_cast<float> (i) / 40.0f);
        rig.tick();
        INFO ("step " << i);
        CHECK (handle.getCentre().x > lastX);
        CHECK (view.getLocalBounds().contains (handle.getBounds()));
        lastX = handle.getCentre().x;
    }
}

TEST_CASE ("Filter curve: self-oscillation is stated on the graph", "[ui][editor][graphs]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& view = rig.editor->getSoundPage (1).getCurveView();
    hostWrite (rig.param ("upper.vcf.resonance"), 1.0f);
    rig.tick();
    CHECK (view.getNote().contains ("self-oscillation"));
    hostWrite (rig.param ("upper.vcf.resonance"), 0.5f);
    rig.tick();
    CHECK (view.getNote().isEmpty());
}

TEST_CASE ("A session restored while the editor is open applies its window size", "[ui][editor][state]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.editor->setSizePercent (150);
    juce::MemoryBlock state;
    rig.proc.getStateInformation (state);
    rig.editor->setSizePercent (100);
    REQUIRE (rig.editor->getWidth() == 1360);
    std::thread ([&] { rig.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize())); }).join();
    rig.tick();
    CHECK (rig.editor->getWidth() == 2040);
    CHECK (rig.proc.getEditorState().sizePercent == 150);
    rig.tick(); // stable: nothing else changes
    CHECK (rig.editor->getWidth() == 2040);
}
