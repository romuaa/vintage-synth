// The custom editor (docs/architecture.md 9.6), not on screen, driven by calling tick():
//   registry completeness (172 primaries, each once; secondaries beside their primary; captions),
//   automation follow (every parameter, from a background thread, and after programs, Init, tones,
//   session restore), gestures through the controls (every interaction row of 9.1.5, contested
//   drag, editor destroyed mid-drag), external-change markers, UI state, size rules, help, meter,
//   open/close and two instances, and the layout snapshot of the Sound page.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EditorRig.h"
#include "../preset_unit/PresetTestHelpers.h"

#include <chrono>
#include <map>
#include <random>
#include <set>
#include <thread>

using namespace vs;
using namespace vs::itest;
using namespace vs::ptest;
using Catch::Approx;

namespace
{
bool sameBits (float a, float b) { return std::memcmp (&a, &b, sizeof (float)) == 0; }

/** Sets a parameter's normalised value from another thread, as host automation does (JUCE's
    VST3 wrapper calls setValueNotifyingHost, often on the audio thread). */
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

/** Every binding of every parameter draws the mirror's value (its prepared drawing data, not a
    fresh read), the mirror holds the parameter's value, the primaries show the parameter's own text,
    and the pictures were recomputed from what the engine itself reads (its block parameters). */
void checkEverythingFollows (EditorRig& rig, const juce::String& when)
{
    rig.render (64, {}, 64); // the engine reads the parameters (seqlock block read)
    auto& ed = *rig.editor;
    auto& mirror = ed.getMirror();
    for (int i = 0; i < mirror.size(); ++i)
    {
        auto& p = mirror.parameter (i);
        INFO (when << ": " << p.paramID);
        REQUIRE (sameBits (mirror.value (i), p.getValue()));
    }
    for (const auto& e : ed.getRegistry().entries())
    {
        juce::String why;
        INFO (when << ": " << e.paramId << " on " << e.page);
        REQUIRE (e.view->showsMirroredValue (why));
        if (auto* b = dynamic_cast<const ui::BoundComponent*> (e.view))
        {
            auto& p = mirror.parameter (mirror.indexOf (e.paramId));
            const auto label = p.getLabel();
            REQUIRE (b->shownText() == p.getCurrentValueAsText() + (label.isEmpty() ? juce::String() : " " + label));
        }
    }
    const auto& block = rig.proc.getBlockParams();
    const double rate = static_cast<double> (rig.proc.getUiFeed().internalRate.load());
    for (int part = 0; part < 2; ++part)
    {
        INFO (when << ": pictures of part " << part);
        auto& page = ed.getSoundPage (part);
        const PartParams& engine = part == 0 ? block.lower : block.upper;
        REQUIRE (page.curveSettings() == dsp::filterCurveSettings (engine.vcfCutoff, engine.vcfResonance, engine.vcfSlope, engine.hpfCutoff, rate));
        REQUIRE (page.scopeSettings() == oscScopeSettingsFor (engine));
    }
}

void dumpTree (const juce::Component& c, int depth, juce::String& out)
{
    if (! c.isVisible())
        return;
    auto name = c.getComponentID();
    if (name.isEmpty())
        name = c.getTitle();
    if (name.isEmpty())
        name = c.getName();
    if (name.isEmpty())
        name = "-";
    out << juce::String::repeatedString ("  ", depth) << name << " " << c.getBounds().toString() << "\n";
    for (auto* child : c.getChildren())
        dumpTree (*child, depth + 1, out);
}
} // namespace

// =================================================================================================
// Registry
// =================================================================================================

TEST_CASE ("Registry: every parameter has exactly one primary control; secondaries sit beside their primary; captions match", "[ui][editor][registry]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    for (int part = 0; part < 2; ++part)
    {
        rig.editor->selectPage (0, part);
        rig.tick();
        const auto& reg = rig.editor->getRegistry();
        INFO ("edited part " << part);

        std::map<juce::String, int> primaries;
        std::map<juce::String, int> perPage;
        for (const auto& e : reg.entries())
            if (e.placement == ui::Placement::Primary)
            {
                ++primaries[e.paramId];
                ++perPage[e.page + (e.part == 1 ? "/upper" : (e.part == 0 ? "/lower" : ""))];
                // A part page binds only its own part's parameters.
                if (e.part == 0)
                    CHECK (e.paramId.startsWith ("lower."));
                if (e.part == 1)
                    CHECK (e.paramId.startsWith ("upper."));
            }
        int params = 0;
        for (auto* p : rig.proc.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
            {
                ++params;
                INFO (r->paramID);
                CHECK (primaries[r->paramID] == 1);
            }
        CHECK (params == 172);
        CHECK (primaries.size() == 172);

        // 9.3.10: 15 per part on Sound, 16 on Envelopes, 9 on LFO, 4 + 8 x 2 on Play, 58, 11, 1 + 2 in the frame.
        CHECK (perPage["sound/lower"] == 15);
        CHECK (perPage["sound/upper"] == 15);
        CHECK (perPage["envelopes/lower"] == 16);
        CHECK (perPage["lfo/upper"] == 9);
        CHECK (perPage["play"] == 20);
        CHECK (perPage["arpseq"] == 58);
        CHECK (perPage["effects"] == 11);
        CHECK (perPage["frame"] == 3);

        int secondaries = 0;
        for (const auto& e : reg.entries())
        {
            if (e.placement == ui::Placement::Secondary)
            {
                ++secondaries;
                const auto* p = primaryOf (reg, e.paramId);
                REQUIRE (p != nullptr);
                INFO (e.paramId);
                CHECK ((p->page == "frame" || (p->page == e.page && p->part == e.part)));
            }
            if (e.control == "SegmentedSwitch")
            {
                INFO (e.paramId);
                const int steps = rig.editor->getMirror().parameter (rig.editor->getMirror().indexOf (e.paramId)).getNumSteps();
                CHECK (e.captions == (e.choices > 0 ? e.choices : steps));
            }
        }
        // The filter graph's cutoff, resonance and high-pass, per part; on Effects the delay handle (time, repeats), the reverb
        // handles (pre-delay, decay time) and the two power switches (delay level, reverb level).
        CHECK (secondaries == 2 * 3 + 6);
    }
}

// =================================================================================================
// Automation follow
// =================================================================================================

TEST_CASE ("Automation follow: every parameter at min, default and a random value, set from another thread, shows within one tick", "[ui][editor][follow]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& mirror = rig.editor->getMirror();
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> uni (0.0f, 1.0f);
    for (int i = 0; i < mirror.size(); ++i)
    {
        auto& p = mirror.parameter (i);
        // Four values in a row, 20 ms apart (inside the marker window): each must be drawn.
        for (float v : { 0.0f, p.getDefaultValue(), uni (rng), 1.0f })
        {
            hostWrite (p, v);
            rig.tick();
            checkEverythingFollows (rig, p.paramID + " = " + juce::String (v));
        }
    }
}

TEST_CASE ("Automation follow: after program changes, Init, tone loads and a session restore", "[ui][editor][follow]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& pm = rig.proc.getPresets();
    for (int program : { 9, 2, 40, 63 })
    {
        pm.selectProgram (program);
        rig.tick();
        checkEverythingFollows (rig, "program " + juce::String (program));
    }
    pm.initialise();
    rig.tick();
    checkEverythingFollows (rig, "Init");

    pm.selectProgram (12);
    const auto tone = pm.captureTone (0);
    pm.selectProgram (5);
    pm.applyTone (tone, 1);
    rig.tick();
    checkEverythingFollows (rig, "tone into Upper");

    juce::MemoryBlock state;
    pm.selectProgram (20);
    rig.proc.getStateInformation (state);
    pm.selectProgram (0);
    rig.tick();
    std::thread ([&] { rig.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize())); }).join();
    rig.tick();
    checkEverythingFollows (rig, "session restore");
}

TEST_CASE ("Automation follow: a burst of changes sets no external markers; a single host change does, and it fades", "[ui][editor][follow]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& cutoff = controlFor<ui::Slider> (*rig.editor, "lower.vcf.cutoff");
    rig.proc.getPresets().selectProgram (7); // a program load changes far more than 8 parameters
    rig.tick();
    CHECK (cutoff.getMarkerAlpha() <= 0.0f);

    hostWrite (rig.param ("lower.vcf.cutoff"), 0.2f);
    rig.tick();
    CHECK (cutoff.getMarkerAlpha() == Approx (1.0f));
    rig.tick (1600.0); // 1.5 s hold, then fading
    CHECK (cutoff.getMarkerAlpha() > 0.0f);
    CHECK (cutoff.getMarkerAlpha() < 1.0f);
    rig.tick (600.0);
    CHECK (cutoff.getMarkerAlpha() <= 0.0f);

    // The editor's own writes set no marker.
    auto& m = cutoff.getModel();
    m.key (ui::model::Key::Increase, false, rig.clockMs);
    rig.tick();
    CHECK (cutoff.getMarkerAlpha() <= 0.0f);
    rig.tick (400.0);
}

// =================================================================================================
// Gestures through the editor's controls
// =================================================================================================

TEST_CASE ("Gestures: every interaction writes inside a host gesture; balanced; multi-parameter drags; Ctrl-free", "[ui][editor][gestures]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    GestureCounter host (rig.proc);
    auto& ed = *rig.editor;

    auto& resonance = controlFor<ui::Slider> (ed, "lower.vcf.resonance");
    resonance.getModel().mouseDown (resonance.trackBounds().getCentreX(), false, false); // a click on the track jumps
    resonance.getModel().mouseDrag (resonance.trackBounds().getCentreX() + 30.0f, false);
    resonance.getModel().mouseDrag (resonance.trackBounds().getCentreX() + 31.0f, true);
    CHECK (host.openCount() == 1);
    resonance.getModel().mouseUp();
    resonance.getModel().doubleClick();
    resonance.getModel().typed (0.3f);
    resonance.getModel().wheel (1.0f, false, rig.clockMs);
    resonance.getModel().key (ui::model::Key::PageUp, false, rig.clockMs);
    rig.tick (100.0);
    CHECK (host.openCount() == 1); // wheel/keys: still open 100 ms after the last notch
    rig.tick (300.0);
    CHECK (host.openCount() == 0);

    auto& wave = controlFor<ui::SegmentedSwitch> (ed, "upper.vco1.waveform");
    wave.getModel().press (4);
    wave.getModel().release();
    auto& sync = controlFor<ui::Toggle> (ed, "lower.vco2.sync");
    sync.getModel().press();
    sync.getModel().release();
    CHECK (rig.param ("upper.vco1.waveform").getValue() == Approx (0.8f));
    CHECK (rig.param ("lower.vco2.sync").getValue() == Approx (1.0f));

    // The filter handle: one drag writes cutoff and resonance, each in its own gesture, ended at mouse-up.
    auto& handle = ed.getSoundPage (0).getCurveView().getHandle (0);
    auto& hm = handle.getModel();
    const auto c = handle.getCentre();
    hm.mouseDown (c.x, c.y, c.x, false);
    hm.mouseDrag (c.x + 50.0f, c.y - 20.0f, false);
    CHECK (host.openCount() == 2);
    hm.mouseUp();
    CHECK (host.openCount() == 0);

    rig.tick (400.0);
    CHECK (host.begins == host.ends);
    CHECK (host.valuesOutsideGesture == 0);
    CHECK (host.nestedBegins == 0);
    CHECK (ed.getMirror().openGestures() == 0);
}

TEST_CASE ("Gestures: a drag the host contests shows the user's value and the marker, and writes nothing at mouse-up", "[ui][editor][gestures]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& slider = controlFor<ui::Slider> (*rig.editor, "lower.vcf.key_follow");
    auto& p = rig.param ("lower.vcf.key_follow");
    auto& m = slider.getModel();
    m.mouseDown (slider.thumbBounds().getCentreX(), true, false);
    m.mouseDrag (slider.thumbBounds().getCentreX() + 40.0f, false);
    const float mine = m.dragValue();
    rig.tick();
    CHECK (slider.shownValue() == Approx (mine));
    CHECK (slider.getMarkerAlpha() <= 0.0f);

    hostWrite (p, 0.123f); // the host keeps writing (Read mode)
    rig.tick();
    CHECK (slider.shownValue() == Approx (mine)); // the user's value while dragging
    CHECK (slider.getMarkerAlpha() > 0.0f);       // ... with the external-change marker
    m.mouseUp();
    CHECK (p.getValue() == Approx (0.123f)); // nothing written at mouse-up
    rig.tick();
    CHECK (slider.shownValue() == Approx (0.123f));
    juce::String why;
    CHECK (slider.showsMirroredValue (why));
}

TEST_CASE ("Gestures: the editor destroyed mid-drag ends the open gestures", "[ui][editor][gestures]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    GestureCounter host (rig.proc);
    auto& slider = controlFor<ui::Slider> (*rig.editor, "upper.vca.level");
    slider.getModel().mouseDown (slider.thumbBounds().getCentreX(), true, false);
    slider.getModel().mouseDrag (slider.thumbBounds().getCentreX() + 10.0f, false);
    auto& hm = rig.editor->getSoundPage (1).getCurveView().getHandle (1).getModel();
    const auto c = rig.editor->getSoundPage (1).getCurveView().getHandle (1).getCentre();
    hm.mouseDown (c.x, c.y, c.x, false);
    hm.mouseDrag (c.x + 60.0f, c.y, false);
    controlFor<ui::Slider> (*rig.editor, "master.volume").getModel().wheel (1.0f, false, rig.clockMs);
    CHECK (host.openCount() == 3);
    rig.editor.reset();
    CHECK (host.openCount() == 0);
    CHECK (host.begins == host.ends);
}

// =================================================================================================
// UI state, size, help, meter
// =================================================================================================

TEST_CASE ("Editor UI state: page and part are written on change, read back after a session restore, and survive reopening", "[ui][editor][state]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& es = rig.proc.getEditorState();
    rig.editor->selectPage (2, 1);
    CHECK (es.page == 2);
    CHECK (es.part == 1);
    CHECK (rig.editor->getFrame().getPage() == 2);

    juce::MemoryBlock state;
    rig.proc.getStateInformation (state);
    rig.editor->selectPage (5, 0);
    std::thread ([&] { rig.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize())); }).join();
    rig.tick();
    CHECK (rig.editor->getFrame().getPage() == 2);
    CHECK (rig.editor->getFrame().getPart() == 1);
    CHECK (rig.editor->getFrame().getHelpLine().getBanner().contains ("Upper is not heard")); // Whole mode

    rig.editor->setSizePercent (150);
    CHECK (es.sizePercent == 150);
    rig.editor.reset();
    rig.open();
    CHECK (rig.editor->getWidth() == 2040);
    CHECK (rig.editor->getHeight() == 1176);
    CHECK (rig.editor->getScale() == Approx (1.5f));
    CHECK (rig.editor->getFrame().getPage() == 2);
    CHECK (rig.editor->getFrame().getPart() == 1);
}

TEST_CASE ("Editor size: first open picks 100 % if it fits 90 % of the screen, else the largest menu size that fits", "[ui][editor][state]")
{
    using E = ui::VintageEditor;
    CHECK (E::initialSizePercent (175, {}) == 175);
    CHECK (E::initialSizePercent (0, {}) == 100);
    CHECK (E::initialSizePercent (0, { 0, 0, 2560, 1440 }) == 100);
    CHECK (E::initialSizePercent (0, { 0, 0, 1440, 900 }) == 90);
    CHECK (E::initialSizePercent (0, { 0, 0, 1280, 720 }) == 75);
    CHECK (E::initialSizePercent (0, { 0, 0, 800, 600 }) == 75);
    CHECK (E::initialSizePercent (300, { 0, 0, 2560, 1440 }) == 100); // invalid saved size: chosen again
}

TEST_CASE ("Editor help: hovering or focusing a control shows 'Label: text' in the help line, else the page hint", "[ui][editor][help]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& frame = rig.editor->getFrame();
    auto& cutoff = controlFor<ui::Slider> (*rig.editor, "lower.vcf.cutoff");
    frame.hoverHelp (&cutoff);
    CHECK (frame.getHelpLine().getText() == "Cutoff: The brightness of the sound. Lower settings are darker.");
    CHECK (cutoff.getDescription() == "The brightness of the sound. Lower settings are darker.");
    CHECK (cutoff.getTitle() == "Lower Cutoff");
    frame.hoverHelp (nullptr);
    CHECK (frame.getHelpLine().getText().startsWith ("Sound:"));
    frame.hoverHelp (&frame.getPartSwitch());
    CHECK (frame.getHelpLine().getText().startsWith ("Edited part: Chooses which part"));
}

TEST_CASE ("Editor meter: the output meter follows the feed at most 30 times per second and repaints only when it moves", "[ui][editor][feed]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& meter = rig.editor->getFrame().getMeter();
    rig.render (rig.samples (0.2), { { static_cast<int> (rig.now), juce::MidiMessage::noteOn (1, 60, 1.0f) } }, 256);
    rig.tick (40.0);
    CHECK (meter.levelDb (0) > -30.0f);
    const int repaints = meter.repaintCount();
    rig.tick (40.0); // no new audio: the bars fall by 0.8 dB
    rig.tick (5.0);  // inside 33 ms: not even looked at
    CHECK (meter.repaintCount() <= repaints + 1);
}

// =================================================================================================
// Lifetime
// =================================================================================================

TEST_CASE ("Editor open/close 200 times; two instances with open editors stay independent", "[ui][editor][lifetime]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig a;
    for (int i = 0; i < 200; ++i)
    {
        a.editor.reset();
        a.open();
        a.tick();
    }
    CHECK (a.editor->getMirror().openGestures() == 0);
    checkEverythingFollows (a, "after 200 open/close cycles");

    EditorRig b;
    hostWrite (a.param ("upper.vcf.cutoff"), 0.11f);
    a.tick();
    b.tick();
    CHECK (controlFor<ui::Slider> (*a.editor, "upper.vcf.cutoff").shownValue() == Approx (0.11f));
    CHECK (controlFor<ui::Slider> (*b.editor, "upper.vcf.cutoff").shownValue() == Approx (b.param ("upper.vcf.cutoff").getValue()));
    a.editor->selectPage (4, 0);
    CHECK (b.editor->getFrame().getPage() == 0);
    // Audio keeps running with an editor open on both (the editor never touches the audio thread).
    a.render (1024, {}, 256);
    b.render (1024, {}, 256);
}

// =================================================================================================
// Layout snapshot (text, exact): Sound page, both parts, 100 % and 150 %
// =================================================================================================

TEST_CASE ("Layout snapshot: the Sound page's component bounds equal the golden files", "[ui][editor][layout]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    const bool update = juce::SystemStats::getEnvironmentVariable ("VS_UPDATE_UI_GOLDENS", {}) == "1";
    for (int part = 0; part < 2; ++part)
        for (int scale : { 100, 150 })
        {
            rig.editor->setSizePercent (scale);
            rig.editor->selectPage (0, part);
            rig.tick();
            juce::String dump;
            dump << "editor " << rig.editor->getBounds().toString() << " scale " << juce::String (rig.editor->getScale(), 4) << "\n";
            dumpTree (rig.editor->getFrame(), 0, dump);
            const auto file = juce::File (VS_UI_GOLDEN_DIR).getChildFile (juce::String ("sound_") + (part == 0 ? "lower" : "upper") + "_"
                                                                          + juce::String (scale) + ".txt");
            if (update)
            {
                REQUIRE (file.replaceWithText (dump, false, false, "\n"));
                continue;
            }
            INFO (file.getFileName() << " (VS_UPDATE_UI_GOLDENS=1 rewrites it after an intended layout change)");
            REQUIRE (file.existsAsFile());
            CHECK (file.loadFileAsString().removeCharacters ("\r") == dump); // line ends may be converted on Windows
        }
}

// =================================================================================================
// UI bench (hidden): 9.5 budgets
// =================================================================================================

TEST_CASE ("UI bench: mirror tick with all 172 changed, page paint at 100 % / 200 %, editor open", "[.uibench]")
{
    EditorRig rig;
    auto& mirror = rig.editor->getMirror();
    using clock = std::chrono::steady_clock;
    double best = 1e9;
    for (int round = 0; round < 50; ++round)
    {
        for (int i = 0; i < mirror.size(); ++i)
            mirror.parameter (i).setValue ((round % 2) == 0 ? 0.0f : 1.0f);
        const auto t0 = clock::now();
        const int changed = mirror.tick (rig.clockMs += 20.0);
        const double us = std::chrono::duration<double, std::micro> (clock::now() - t0).count();
        if (round > 0)
            REQUIRE (changed == mirror.size());
        best = std::min (best, us);
    }
    WARN ("editor tick, all parameters changed (mirror + every binding's text and drawing data + both Sound pages' pictures): best "
          << best << " us");

    // The mirror alone (loads, compares, notifications to no one): the contract's 50 us budget.
    {
        ui::ParamMirror bare (rig.proc);
        double bestBare = 1e9;
        for (int round = 0; round < 50; ++round)
        {
            for (int i = 0; i < bare.size(); ++i)
                bare.parameter (i).setValue ((round % 2) == 0 ? 0.0f : 1.0f);
            const auto t0 = clock::now();
            bare.tick (rig.clockMs += 20.0);
            bestBare = std::min (bestBare, std::chrono::duration<double, std::micro> (clock::now() - t0).count());
        }
        WARN ("mirror tick alone, all 172 changed: best " << bestBare << " us (budget 50 us)");
        CHECK (bestBare < 50.0);
    }

    for (int scale : { 100, 200 })
    {
        rig.editor->setSizePercent (scale);
        rig.editor->selectPage (0, 0);
        rig.tick();
        double bestPaint = 1e9;
        for (int i = 0; i < 10; ++i)
        {
            juce::Image image (juce::Image::ARGB, rig.editor->getWidth(), rig.editor->getHeight(), true, juce::SoftwareImageType());
            juce::Graphics g (image);
            const auto t0 = clock::now();
            rig.editor->paintEntireComponent (g, true);
            bestPaint = std::min (bestPaint, std::chrono::duration<double, std::milli> (clock::now() - t0).count());
        }
        WARN ("Sound page full paint at " << scale << " %, software renderer: best " << bestPaint << " ms (budget " << (scale == 100 ? 4 : 10) << " ms)");
    }

    double bestOpen = 1e9;
    for (int i = 0; i < 5; ++i)
    {
        rig.editor.reset();
        const auto t0 = clock::now();
        rig.open();
        juce::Image image (juce::Image::ARGB, rig.editor->getWidth(), rig.editor->getHeight(), true, juce::SoftwareImageType());
        juce::Graphics g (image);
        rig.editor->paintEntireComponent (g, true);
        bestOpen = std::min (bestOpen, std::chrono::duration<double, std::milli> (clock::now() - t0).count());
    }
    WARN ("editor open to first paint (software renderer): best " << bestOpen << " ms (budget 250 ms)");
}
