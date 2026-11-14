// The Envelopes and LFO pages (docs/architecture.md 9.3.2, 9.3.3, 9.6): automation follow of every
// control, handle and picture (from another thread, after programs, Init, tones and a session
// restore), the derived pictures against what the processor itself reads, handle drags and their
// gestures, the derived panel titles, the inactive pulse width controls, the inverted ghost, repaint
// scope, paint allocations and the layout snapshot (100 % and 150 %, both parts).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EditorRig.h"
#include "../preset_unit/PresetTestHelpers.h"

#include "vintage/ui/EnvelopePicture.h"
#include "vintage/ui/EnvelopesPage.h"
#include "vintage/ui/LfoPage.h"

#include <algorithm>
#include <cstring>
#include <random>
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

juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<float> pos, int clicks = 1)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pos,
                             now, clicks, false);
}

/** Mouse events on a graph handle at parent coordinates `p` (what the handle's own code reads). */
struct HandleMouse
{
    ui::GraphHandle& h;
    juce::Point<float> local (juce::Point<float> parentPoint) const { return parentPoint - h.getBounds().getPosition().toFloat(); }
    void down (juce::Point<float> parentPoint) { h.mouseDown (mouseEvent (h, local (parentPoint))); }
    void drag (juce::Point<float> parentPoint) { h.mouseDrag (mouseEvent (h, local (parentPoint))); }
    void up (juce::Point<float> parentPoint) { h.mouseUp (mouseEvent (h, local (parentPoint))); }
    void doubleClick (juce::Point<float> parentPoint) { h.mouseDoubleClick (mouseEvent (h, local (parentPoint), 2)); }
};

const std::vector<juce::String>& pageSuffixes()
{
    static const auto all = [] {
        auto s = ui::envelopeSuffixes();
        for (auto& l : ui::lfoSuffixes())
            s.push_back (l);
        return s;
    }();
    return all;
}

/** What the processor itself reads (its block parameters) is what every control, handle and
    picture of the two pages shows, and the pictures were recomputed from it. */
void checkPagesFollow (EditorRig& rig, const juce::String& when)
{
    rig.render (64, {}, 64); // the engine reads the parameters (seqlock block read)
    auto& ed = *rig.editor;
    for (const auto& e : ed.getRegistry().entries())
    {
        if (e.page != "envelopes" && e.page != "lfo")
            continue;
        juce::String why;
        INFO (when << ": " << e.paramId << " on " << e.page << " (" << e.control << ")");
        REQUIRE (e.view->showsMirroredValue (why));
    }
    const auto& block = rig.proc.getBlockParams();
    for (int part = 0; part < 2; ++part)
    {
        const PartParams& engine = part == 0 ? block.lower : block.upper;
        INFO (when << ": part " << part);

        auto& ep = ed.getEnvelopesPage (part);
        const ui::model::EnvelopeValues expected[2] { { engine.env1Attack, engine.env1Decay, engine.env1Sustain, engine.env1Release },
                                                      { engine.env2Attack, engine.env2Decay, engine.env2Sustain, engine.env2Release } };
        for (int n = 0; n < 2; ++n)
        {
            auto& view = ep.getEnvelopeView (n);
            const auto& v = view.shownValues();
            REQUIRE (v.attack == Approx (expected[n].attack).margin (1.0e-6));
            REQUIRE (v.decay == Approx (expected[n].decay).margin (1.0e-6));
            REQUIRE (v.sustain == Approx (expected[n].sustain).margin (1.0e-6));
            REQUIRE (v.release == Approx (expected[n].release).margin (1.0e-6));
            const auto want = ui::computeEnvelopeShape (expected[n]); // the times through the engine's own mapping
            const auto& got = view.shownShape();
            REQUIRE (got.sustain == Approx (want.sustain).margin (1.0e-6));
            for (size_t i = 0; i < want.attack.size(); ++i)
            {
                REQUIRE (got.attack[i] == Approx (want.attack[i]).margin (1.0e-6));
                REQUIRE (got.decay[i] == Approx (want.decay[i]).margin (1.0e-6));
                REQUIRE (got.release[i] == Approx (want.release[i]).margin (1.0e-6));
            }
            REQUIRE (view.showsGhost() == (n == 0 && engine.env1Polarity == 1));
            REQUIRE (ep.getPanelTitle (n) == ep.expectedTitle (n));
        }
        const bool filterOnEnv1 = engine.vcfEnvSource == 0;
        REQUIRE (ep.getPanelTitle (0) == (filterOnEnv1 ? "Envelope 1 (filter, pitch)" : "Envelope 1 (pitch)"));
        REQUIRE (ep.getPanelTitle (1) == (filterOnEnv1 ? "Envelope 2 (loudness)" : "Envelope 2 (loudness, filter)"));

        auto& lp = ed.getLfoPage (part);
        auto& lv = lp.getLfoView();
        const float rateHz = map::lfoRateHz (engine.lfoRate), delaySec = map::lfoDelaySec (engine.lfoDelay);
        REQUIRE (lv.shownWave() == engine.lfoWave);
        REQUIRE (lv.shownRateValue() == Approx (engine.lfoRate).margin (1.0e-6));
        REQUIRE (lv.shownDelayValue() == Approx (engine.lfoDelay).margin (1.0e-6));
        // The window is kept while it suits (9.3.3), so it is not always the fresh choice: but outside a
        // drag both handles always lie inside it (never clamped to the edge).
        const float window = lv.getAxis().windowSec;
        REQUIRE (ui::model::lfoWindowKeeps (window, delaySec, rateHz));
        REQUIRE (lv.getHandle (ui::LfoView::Cycle).getCentre().x < lv.getAxis().x0 + lv.getAxis().width);
        REQUIRE ((delaySec > 0.0f) == lv.delayCaption().isNotEmpty());
        REQUIRE ((delaySec > 0.0f) == lv.startCaption().isNotEmpty());
        REQUIRE ((engine.lfoWave >= 4) == lv.exampleCaption().isNotEmpty()); // Random and Noise
        ui::model::LfoPreview want;
        ui::computeLfoPreview (engine.lfoWave, rateHz, delaySec, window, want);
        for (size_t i = 0; i < want.y.size(); ++i)
            REQUIRE (std::memcmp (&lv.shownPreview().y[i], &want.y[i], sizeof (float)) == 0);
        const bool pulse = engine.vco1Wave == 4 || engine.vco2Wave == 4;
        REQUIRE (lp.pulseWidthInactive() == ! pulse);
        REQUIRE (lp.pulseWidthExpectedInactive() == ! pulse);
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
    std::vector<juce::Rectangle<int>> areas;
    int all = 0;
};
} // namespace

// =================================================================================================
// Automation follow
// =================================================================================================

TEST_CASE ("Envelopes + LFO pages follow every parameter set from another thread (min, default, random, max), within one tick", "[ui][editor][follow][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    std::mt19937 rng (21);
    std::uniform_real_distribution<float> uni (0.0f, 1.0f);
    for (const char* part : { "lower.", "upper." })
        for (const auto& suffix : pageSuffixes())
        {
            auto& p = rig.param (juce::String (part) + suffix);
            // Four values in a row, 20 ms apart (inside the marker window): each must be drawn.
            for (float v : { 0.0f, p.getDefaultValue(), uni (rng), 1.0f })
            {
                hostWrite (p, v);
                rig.tick();
                checkPagesFollow (rig, p.paramID + " = " + juce::String (v));
            }
        }
}

TEST_CASE ("Envelopes + LFO pages follow program changes, Init, tone loads and a session restore", "[ui][editor][follow][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& pm = rig.proc.getPresets();
    for (int program : { 9, 2, 40, 63, 17 })
    {
        pm.selectProgram (program);
        rig.tick();
        checkPagesFollow (rig, "program " + juce::String (program));
    }
    pm.initialise();
    rig.tick();
    checkPagesFollow (rig, "Init");

    pm.selectProgram (12);
    const auto tone = pm.captureTone (0);
    pm.selectProgram (5);
    pm.applyTone (tone, 1);
    rig.tick();
    checkPagesFollow (rig, "tone into Upper");

    juce::MemoryBlock state;
    pm.selectProgram (20);
    rig.proc.getStateInformation (state);
    pm.selectProgram (0);
    rig.tick();
    std::thread ([&] { rig.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize())); }).join();
    rig.tick();
    checkPagesFollow (rig, "session restore");
}

TEST_CASE ("Envelopes page: the panel titles follow the filter source; Envelope 1 shows its mirrored ghost when inverted", "[ui][editor][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& ep = rig.editor->getEnvelopesPage (0);
    CHECK (ep.getPanelTitle (0) == "Envelope 1 (filter, pitch)");
    CHECK (ep.getPanelTitle (1) == "Envelope 2 (loudness)");
    hostWrite (rig.param ("lower.vcf.env_source"), 1.0f);
    rig.tick();
    CHECK (ep.getPanelTitle (0) == "Envelope 1 (pitch)");
    CHECK (ep.getPanelTitle (1) == "Envelope 2 (loudness, filter)");
    CHECK (rig.editor->getEnvelopesPage (1).getPanelTitle (0) == "Envelope 1 (filter, pitch)"); // the other part is its own

    auto& view = ep.getEnvelopeView (0);
    const float plainBase = view.getLayout().yBase;
    CHECK_FALSE (view.showsGhost());
    hostWrite (rig.param ("lower.env1.polarity"), 1.0f);
    rig.tick();
    CHECK (view.showsGhost());
    CHECK (view.getLayout().yBase < plainBase - 20.0f); // the axis moved up to make room under it
    // The handles sit on the shape above the axis; the peak is still at the top.
    CHECK (view.getHandle (ui::EnvelopeView::Peak).getCentre().y == Approx (view.getLayout().yTop));
    CHECK (view.getHandle (ui::EnvelopeView::ReleaseEnd).getCentre().y == Approx (view.getLayout().yBase));
    CHECK_FALSE (ep.getEnvelopeView (1).showsGhost()); // Envelope 2 has no inverted mode
    hostWrite (rig.param ("lower.env1.polarity"), 0.0f);
    rig.tick();
    CHECK_FALSE (view.showsGhost());
    CHECK (view.getLayout().yBase == Approx (plainBase));
}

TEST_CASE ("LFO page: the pulse width controls are dimmed (still editable) while neither oscillator is set to Pulse", "[ui][editor][lfo]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& lp = rig.editor->getLfoPage (1);
    // Default waveforms are Saw (not Pulse).
    CHECK (lp.pulseWidthInactive());
    hostWrite (rig.param ("upper.vco2.waveform"), 4.0f / 5.0f); // Pulse
    rig.tick();
    CHECK_FALSE (lp.pulseWidthInactive());
    CHECK (rig.editor->getLfoPage (0).pulseWidthInactive()); // the other part is its own
    hostWrite (rig.param ("upper.vco1.waveform"), 4.0f / 5.0f);
    hostWrite (rig.param ("upper.vco2.waveform"), 0.0f);
    rig.tick();
    CHECK_FALSE (lp.pulseWidthInactive()); // either oscillator is enough
    hostWrite (rig.param ("upper.vco1.waveform"), 3.0f / 5.0f);
    rig.tick();
    CHECK (lp.pulseWidthInactive());
    // Dimmed, not disabled: the values stay editable.
    const auto e = std::find_if (rig.editor->getRegistry().entries().begin(), rig.editor->getRegistry().entries().end(), [] (const ui::ControlEntry& entry) {
        return entry.paramId == "upper.vcomod.pwm" && entry.placement == ui::Placement::Primary;
    });
    REQUIRE (e != rig.editor->getRegistry().entries().end());
    auto* slider = dynamic_cast<const juce::Component*> (e->view);
    REQUIRE (slider != nullptr);
    CHECK (slider->isEnabled());
    CHECK (slider->getDescription().endsWith ("(no effect in this mode)"));
}

// =================================================================================================
// Handle drags and gestures
// =================================================================================================

TEST_CASE ("Envelope handles: a drag writes the time or level that puts the handle there, inside host gestures, and the picture follows live", "[ui][editor][gestures][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    GestureCounter host (rig.proc);
    auto& view = rig.editor->getEnvelopesPage (0).getEnvelopeView (0);
    const auto layout = view.getLayout();
    const auto before = layout.points (view.shownValues());

    // Peak: x writes the attack only.
    {
        auto& h = view.getHandle (ui::EnvelopeView::Peak);
        HandleMouse m { h };
        const auto c = h.getCentre();
        m.down (c);
        m.drag ({ c.x + 60.0f, c.y + 25.0f }); // y is ignored: the peak is always at level 1
        CHECK (host.openCount() == 1);
        const float want = layout.attackFromX (c.x + 60.0f);
        CHECK (rig.param ("lower.env1.attack").getValue() == Approx (want).margin (1.0e-4));
        // Live: before any tick the handle already sits where the dragged value puts it.
        CHECK (h.getCentre().x == Approx (layout.points ({ want, view.shownValues().decay, view.shownValues().sustain, view.shownValues().release }).peakX).margin (0.6));
        CHECK (h.getCentre().y == Approx (before.topY));
        m.up ({ c.x + 60.0f, c.y });
        CHECK (host.openCount() == 0);
        rig.tick();
        juce::String why;
        for (const auto& e : rig.editor->getRegistry().entries())
            if (e.page == "envelopes")
                CHECK (e.view->showsMirroredValue (why));
    }
    // Decay handle: x writes the decay measured from the peak where it now is, y the sustain.
    {
        auto& h = view.getHandle (ui::EnvelopeView::SustainStart);
        HandleMouse m { h };
        const auto c = h.getCentre();
        const float peakNow = layout.points (view.shownValues()).peakX;
        const float sustain0 = rig.param ("lower.env1.sustain").getValue();
        host.begins = host.ends = 0;
        m.down (c);
        m.drag ({ c.x + 30.0f, c.y - 37.2f });
        CHECK (host.openCount() == 2); // decay and sustain, each in its own gesture
        CHECK (rig.param ("lower.env1.decay").getValue() == Approx (layout.decayFromX (c.x + 30.0f, peakNow)).margin (1.0e-4));
        CHECK (rig.param ("lower.env1.sustain").getValue() == Approx (sustain0 + 37.2f / (layout.yBase - layout.yTop)).margin (1.0e-4));
        m.up ({ c.x + 30.0f, c.y - 37.2f });
        CHECK (host.openCount() == 0);
        CHECK (host.begins == 2);
        CHECK (host.ends == 2);
    }
    // Sustain end: y only, the sustain level.
    {
        auto& h = view.getHandle (ui::EnvelopeView::SustainEnd);
        HandleMouse m { h };
        const auto c = h.getCentre();
        const float decay0 = rig.param ("lower.env1.decay").getValue();
        const float sustain0 = rig.param ("lower.env1.sustain").getValue();
        m.down (c);
        m.drag ({ c.x + 80.0f, c.y + 18.6f });
        CHECK (rig.param ("lower.env1.decay").getValue() == Approx (decay0));
        CHECK (rig.param ("lower.env1.sustain").getValue() == Approx (sustain0 - 18.6f / (layout.yBase - layout.yTop)).margin (1.0e-4));
        m.up (c);
    }
    // Release end: x writes the release measured from the end of the plateau.
    {
        auto& h = view.getHandle (ui::EnvelopeView::ReleaseEnd);
        HandleMouse m { h };
        rig.tick();
        const auto c = h.getCentre();
        const auto p = layout.points (view.shownValues());
        m.down (c);
        m.drag ({ c.x - 15.0f, c.y });
        CHECK (rig.param ("lower.env1.release").getValue() == Approx (layout.releaseFromX (c.x - 15.0f, p.plateauEndX)).margin (1.0e-4));
        m.up (c);
    }
    // Double-click on the decay handle resets both its parameters.
    {
        auto& h = view.getHandle (ui::EnvelopeView::SustainStart);
        HandleMouse m { h };
        rig.tick();
        m.doubleClick (h.getCentre());
        CHECK (rig.param ("lower.env1.decay").getValue() == Approx (rig.param ("lower.env1.decay").getDefaultValue()));
        CHECK (rig.param ("lower.env1.sustain").getValue() == Approx (rig.param ("lower.env1.sustain").getDefaultValue()));
    }
    // An arrow key moves the attack by 1 % in a gesture that ends 300 ms later.
    {
        auto& h = view.getHandle (ui::EnvelopeView::Peak);
        const float a0 = rig.param ("lower.env1.attack").getValue();
        CHECK (h.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
        CHECK (rig.param ("lower.env1.attack").getValue() == Approx (a0 + 0.01f).margin (1.0e-5));
        CHECK (host.openCount() == 1);
        juce::Thread::sleep (350); // the handle times its keys with the real clock
        rig.editor->tick();
        CHECK (host.openCount() == 0);
    }
    CHECK (host.begins == host.ends);
    CHECK (host.valuesOutsideGesture == 0);
    CHECK (host.nestedBegins == 0);
    CHECK (rig.editor->getMirror().openGestures() == 0);
}

TEST_CASE ("Envelope and LFO handles: the editor destroyed mid-drag ends every open gesture", "[ui][editor][gestures][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    GestureCounter host (rig.proc);
    {
        auto& h = rig.editor->getEnvelopesPage (1).getEnvelopeView (1).getHandle (ui::EnvelopeView::SustainStart);
        HandleMouse m { h };
        const auto c = h.getCentre();
        m.down (c);
        m.drag ({ c.x + 20.0f, c.y - 10.0f });
    }
    {
        auto& h = rig.editor->getLfoPage (0).getLfoView().getHandle (ui::LfoView::Cycle);
        HandleMouse m { h };
        const auto c = h.getCentre();
        m.down (c);
        m.drag ({ c.x + 20.0f, c.y });
    }
    CHECK (host.openCount() == 3);
    rig.editor.reset();
    CHECK (host.openCount() == 0);
    CHECK (host.begins == host.ends);
}

TEST_CASE ("Envelopes and LFO pages: a page that goes away ends its handle drags exactly once", "[ui][editor][gestures][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    GestureCounter host (rig.proc);
    rig.editor->selectPage (1, 0);
    auto& h = rig.editor->getEnvelopesPage (0).getEnvelopeView (0).getHandle (ui::EnvelopeView::Peak);
    HandleMouse m { h };
    const auto c = h.getCentre();
    m.down (c);
    m.drag ({ c.x + 40.0f, c.y });
    REQUIRE (host.openCount() == 1);
    const int values = host.values;
    rig.editor->selectPage (2, 0); // keyboard page switch during the drag
    CHECK (host.openCount() == 0);
    CHECK (host.ends == 1);
    m.drag ({ c.x + 90.0f, c.y }); // late events of the gone drag
    m.up (c);
    CHECK (host.values == values);
    CHECK (host.ends == 1);
    CHECK (host.nestedBegins == 0);
}

// =================================================================================================
// Repaint scope, paint allocations
// =================================================================================================

TEST_CASE ("Envelopes and LFO pages: one automated parameter repaints only its own controls and pictures, never the whole page", "[ui][editor][repaint][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.editor->setSizePercent (100); // areas are in editor pixels, the page area in logical ones
    rig.editor->setVisible (true);    // an invisible component forwards no repaint request to its cached image
    auto* recorder = new RepaintRecorder();
    rig.editor->setCachedComponentImage (recorder); // owned by the editor
    const auto page = rig.editor->getFrame().pageArea();
    const int pageArea = page.getWidth() * page.getHeight();
    for (int which : { 1, 2 })
    {
        rig.editor->selectPage (which, 0);
        rig.tick();
        for (const auto& suffix : which == 1 ? ui::envelopeSuffixes() : ui::lfoSuffixes())
        {
            auto& p = rig.param ("lower." + suffix);
            rig.tick (3000.0); // the markers of earlier changes have faded: only this change repaints
            hostWrite (p, p.getValue() > 0.5f ? 0.1f : 0.9f);
            recorder->areas.clear();
            recorder->all = 0;
            rig.tick();
            INFO ("page " << which << ": " << suffix);
            CHECK (recorder->all == 0);
            juce::RectangleList<int> repainted; // the union: overlapping requests count once
            for (const auto& a : recorder->areas)
            {
                CHECK (a.getWidth() * a.getHeight() < pageArea / 3); // the biggest picture is a third of the page; a whole panel (the title) would be more
                repainted.add (a);
            }
            long long total = 0;
            for (const auto& r : repainted)
                total += static_cast<long long> (r.getWidth()) * r.getHeight();
            CHECK (total < pageArea * 4 / 10);
        }
    }
    rig.editor->setCachedComponentImage (nullptr);
}

// =================================================================================================
// Layout snapshot (text, exact): Envelopes and LFO pages, both parts, 100 % and 150 %
// =================================================================================================

TEST_CASE ("Layout snapshot: the Envelopes and LFO pages' component bounds equal the golden files", "[ui][editor][layout][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    const bool update = juce::SystemStats::getEnvironmentVariable ("VS_UPDATE_UI_GOLDENS", {}) == "1";
    for (int page : { 1, 2 })
        for (int part = 0; part < 2; ++part)
            for (int scale : { 100, 150 })
            {
                rig.editor->setSizePercent (scale);
                rig.editor->selectPage (page, part);
                rig.tick();
                juce::String dump;
                dump << "editor " << rig.editor->getBounds().toString() << " scale " << juce::String (rig.editor->getScale(), 4) << "\n";
                dumpTree (rig.editor->getFrame(), 0, dump);
                const auto file = juce::File (VS_UI_GOLDEN_DIR).getChildFile (juce::String (page == 1 ? "envelopes_" : "lfo_") + (part == 0 ? "lower" : "upper")
                                                                              + "_" + juce::String (scale) + ".txt");
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
// Hidden: pictures of the states the default shots do not show ($VS_UISHOT_DIR, never compared)
// =================================================================================================

TEST_CASE ("UI shots: Envelopes and LFO in the states the default shots do not show, at 75, 100 and 200 %, as PNG", "[.uishots]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VS_UISHOT_DIR", {});
    if (dir.isEmpty())
        SKIP ("VS_UISHOT_DIR not set");
    const juce::File out (dir);
    REQUIRE (out.createDirectory());
    EditorRig rig;
    const auto write = [&] (int page, int part, int percent, const juce::String& name) {
        rig.editor->setSizePercent (percent);
        rig.editor->selectPage (page, part);
        rig.tick();
        const auto image = rig.editor->createComponentSnapshot (rig.editor->getLocalBounds(), true, 1.0f);
        const auto file = out.getChildFile (name + "_" + juce::String (percent) + ".png");
        file.deleteFile();
        juce::FileOutputStream stream (file);
        REQUIRE (stream.openedOk());
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    };
    // The default sound at 75 % (the shared shots have 100 and 200).
    write (1, 0, 75, "x_envelopes_default");
    write (2, 0, 75, "x_lfo_default");

    // Envelope 1 inverted, Envelope 2 feeding the filter.
    rig.set ("upper.env1.polarity", 1.0f);
    rig.set ("upper.env1.attack", 0.3f);
    rig.set ("upper.env1.decay", 0.5f);
    rig.set ("upper.env1.sustain", 0.45f);
    rig.set ("upper.env1.release", 0.6f);
    rig.set ("upper.vcf.env_source", 1.0f);
    rig.tick();
    for (int percent : { 75, 100, 200 })
        write (1, 1, percent, "x_envelopes_inverted");

    // A delayed sine, a delayed Random, a Noise.
    rig.set ("upper.lfo.delay", 0.4f);
    rig.set ("upper.lfo.rate", 0.5f);
    rig.set ("upper.vco2.waveform", 4.0f);
    for (int percent : { 75, 100, 200 })
    {
        rig.set ("upper.lfo.waveform", 0.0f);
        write (2, 1, percent, "x_lfo_delayed_sine");
    }
    rig.set ("upper.lfo.waveform", 4.0f);
    write (2, 1, 100, "x_lfo_random");
    rig.set ("upper.lfo.waveform", 5.0f);
    rig.set ("upper.lfo.delay", 0.0f);
    write (2, 1, 100, "x_lfo_noise");

    // Extremes: every time at its maximum with full sustain; the fastest LFO; the slowest with the longest delay.
    rig.set ("lower.env1.attack", 1.0f);
    rig.set ("lower.env1.decay", 1.0f);
    rig.set ("lower.env1.sustain", 1.0f);
    rig.set ("lower.env1.release", 1.0f);
    rig.set ("lower.env2.attack", 0.0f);
    rig.set ("lower.env2.decay", 0.0f);
    rig.set ("lower.env2.sustain", 0.0f);
    rig.set ("lower.env2.release", 0.0f);
    write (1, 0, 100, "x_envelopes_extreme");
    rig.set ("lower.lfo.rate", 1.0f);
    rig.set ("lower.lfo.waveform", 3.0f);
    write (2, 0, 100, "x_lfo_fast_square");
    rig.set ("lower.lfo.rate", 0.0f);
    rig.set ("lower.lfo.delay", 1.0f);
    rig.set ("lower.lfo.waveform", 1.0f);
    write (2, 0, 100, "x_lfo_slowest_longest_delay");
}
