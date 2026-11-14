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
        const float window = ui::model::lfoWindowFor (delaySec, rateHz);
        REQUIRE (lv.getAxis().windowSec == Approx (window));
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

TEST_CASE ("LFO handles: the delay and cycle handles write delay and rate; the window is kept during a drag and chosen again after it", "[ui][editor][gestures][lfo]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    GestureCounter host (rig.proc);
    auto& view = rig.editor->getLfoPage (1).getLfoView();
    const auto axis = view.getAxis();
    CHECK (axis.windowSec == Approx (1.0f)); // default rate 3.85 Hz, no delay: two cycles = 0.52 s

    // Delay: dragged to about 0.8 s, far enough that a fresh window would be 2 s.
    auto& dh = view.getHandle (ui::LfoView::Delay);
    HandleMouse dm { dh };
    const auto c = dh.getCentre();
    const float x = axis.xOf (0.8f);
    dm.down (c);
    dm.drag ({ x, c.y });
    CHECK (host.openCount() == 1);
    const float wantDelay = ui::model::lfoDelayFromHandle (axis.timeAt (x));
    CHECK (rig.param ("upper.lfo.delay").getValue() == Approx (wantDelay).margin (1.0e-4));
    CHECK (view.getAxis().windowSec == Approx (1.0f)); // kept while dragging: the handle does not run away from the pointer
    CHECK (dh.getCentre().x == Approx (x).margin (0.6));
    dm.drag ({ axis.xOf (3.0f), c.y }); // past the window: the handle stops at its edge, the value is clamped to what the window shows
    CHECK (rig.param ("upper.lfo.delay").getValue() == Approx (ui::model::lfoDelayFromHandle (1.0f)).margin (1.0e-4));
    dm.drag ({ x, c.y });
    dm.up ({ x, c.y });
    CHECK (host.openCount() == 0);
    rig.tick();
    const float delaySec = map::lfoDelaySec (rig.param ("upper.lfo.delay").getValue());
    const float rateHz = map::lfoRateHz (rig.param ("upper.lfo.rate").getValue());
    CHECK (view.getAxis().windowSec == Approx (ui::model::lfoWindowFor (delaySec, rateHz))); // chosen again
    CHECK (view.getAxis().windowSec > 1.5f);

    // Cycle: dragged to a cycle of 0.25 s after the delay (4 Hz).
    auto& ch = view.getHandle (ui::LfoView::Cycle);
    HandleMouse cm { ch };
    const auto a2 = view.getAxis();
    const auto cc = ch.getCentre();
    const float cx = a2.xOf (delaySec + 0.25f);
    cm.down (cc);
    cm.drag ({ cx, cc.y });
    const float wantRate = ui::model::lfoRateFromHandle (a2.timeAt (cx), rig.param ("upper.lfo.delay").getValue());
    CHECK (rig.param ("upper.lfo.rate").getValue() == Approx (wantRate).margin (1.0e-4));
    CHECK (map::lfoRateHz (rig.param ("upper.lfo.rate").getValue()) == Approx (4.0f).epsilon (0.02));
    CHECK (view.getAxis().windowSec == Approx (a2.windowSec)); // kept
    cm.up ({ cx, cc.y });
    rig.tick();
    CHECK (view.getAxis().windowSec == Approx (ui::model::lfoWindowFor (delaySec, 4.0f)));
    juce::String why;
    for (const auto& e : rig.editor->getRegistry().entries())
        if (e.page == "lfo")
            CHECK (e.view->showsMirroredValue (why));

    rig.tick (400.0);
    CHECK (host.begins == host.ends);
    CHECK (host.valuesOutsideGesture == 0);
    CHECK (host.nestedBegins == 0);
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
            long long total = 0;
            for (const auto& a : recorder->areas)
            {
                CHECK (a.getWidth() * a.getHeight() < pageArea / 3); // the biggest picture is a third of the page; a whole panel (the title) would be more
                total += static_cast<long long> (a.getWidth()) * a.getHeight();
            }
            CHECK (total < pageArea * 4 / 10);
        }
    }
    rig.editor->setCachedComponentImage (nullptr);
}

#if VS_SEQ_COUNT_ALLOCATIONS
TEST_CASE ("Paint allocations: repeated paints of the envelope and LFO pictures cost the same every time, whatever the shape", "[ui][editor][alloc][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    // The software renderer itself allocates while it draws (about 35 times for every line of text, 5 - 30 for a
    // path), so the contract's "<= 8 per component" cannot hold for any component that draws text. What this test
    // shows: a steady-state paint costs the same every time (nothing accumulates or is rebuilt lazily), and the
    // cost is the same whatever the shape. What it cannot show: that a path was prepared and not built in paint()
    // (checked: moving the LFO's waveform stroke into paint() adds only 19 of about 550 allocations), so that part
    // stays a review item (the strings, paths and strokes are made in refresh()).
    const auto counts = [&] (juce::Component& c) {
        juce::Image image (juce::Image::ARGB, c.getWidth(), c.getHeight(), true, juce::SoftwareImageType());
        juce::Graphics g (image);
        std::vector<int> n;
        for (int i = 0; i < 6; ++i)
        {
            vs::seq::test::AllocationCounter counter;
            c.paintEntireComponent (g, false);
            n.push_back (counter.count());
        }
        return n;
    };
    struct Item
    {
        const char* name;
        juce::Component* c;
    };
    for (const int program : { 3, 40 })
    {
        rig.proc.getPresets().selectProgram (program);
        rig.tick();
        auto& ev = rig.editor->getEnvelopesPage (0).getEnvelopeView (0);
        auto& ev2 = rig.editor->getEnvelopesPage (0).getEnvelopeView (1);
        auto& lv = rig.editor->getLfoPage (0).getLfoView();
        for (const Item item : { Item { "Envelope 1 picture", &ev }, Item { "Envelope 2 picture", &ev2 }, Item { "LFO picture", &lv } })
        {
            const auto n = counts (*item.c);
            INFO (item.name << ", program " << program << ": allocations per paint " << n[2] << ", " << n[3] << ", " << n[4] << ", " << n[5]);
            CHECK (n[3] == n[2]);
            CHECK (n[4] == n[2]);
            CHECK (n[5] == n[2]);
            CHECK (n[2] > 0); // the probe sees the renderer (a probe that counts nothing proves nothing)
            CHECK (n[2] < 800);
        }
    }

    // A steady-state paint does not grow with the shape: a 20-point and a 1000-point LFO picture cost the same.
    auto& lv = rig.editor->getLfoPage (0).getLfoView();
    hostWrite (rig.param ("lower.lfo.waveform"), 0.0f); // sine
    hostWrite (rig.param ("lower.lfo.rate"), 0.0f);     // 0.05 Hz: one slow wave
    rig.tick();
    const auto slow = counts (lv);
    hostWrite (rig.param ("lower.lfo.rate"), 1.0f);     // 40 Hz: the window is full of cycles
    rig.tick();
    const auto fast = counts (lv);
    INFO ("slow " << slow[2] << ", fast " << fast[2]);
    CHECK (std::abs (fast[2] - slow[2]) <= slow[2] / 10);
}
#endif

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

TEST_CASE ("UI shots: Envelopes with an inverted Envelope 1, LFO with a delay and a sine wave, as PNG", "[.uishots]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VS_UISHOT_DIR", {});
    if (dir.isEmpty())
        SKIP ("VS_UISHOT_DIR not set");
    const juce::File out (dir);
    REQUIRE (out.createDirectory());
    EditorRig rig;
    rig.set ("upper.env1.polarity", 1.0f);
    rig.set ("upper.env1.attack", 0.3f);
    rig.set ("upper.env1.decay", 0.5f);
    rig.set ("upper.env1.sustain", 0.45f);
    rig.set ("upper.env1.release", 0.6f);
    rig.set ("upper.vcf.env_source", 1.0f);
    rig.set ("upper.lfo.waveform", 0.0f);
    rig.set ("upper.lfo.delay", 0.4f);
    rig.set ("upper.lfo.rate", 0.5f);
    rig.set ("upper.vco2.waveform", 4.0f);
    rig.tick();
    const auto write = [&] (int page, const char* name) {
        rig.editor->setSizePercent (100);
        rig.editor->selectPage (page, 1);
        rig.tick();
        const auto image = rig.editor->createComponentSnapshot (rig.editor->getLocalBounds(), true, 1.0f);
        const auto file = out.getChildFile (name);
        file.deleteFile();
        juce::FileOutputStream stream (file);
        REQUIRE (stream.openedOk());
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    };
    write (1, "envelopes_upper_inverted_100.png");
    write (2, "lfo_upper_delay_sine_100.png");
}

