// Envelopes and LFO pages, second round (docs/architecture.md 9.1.5, 9.3.2, 9.3.3, 9.5, 9.6):
// a drag the host contests, the LFO window rule, the delay handle on the fade-in's envelope line and
// its captions, Random and Noise examples, the envelope's time labels and the "key released" caption,
// the panel header and its accessibility title, nothing repaints at rest, and paint allocations of
// our own code through a graphics context that draws nothing.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EditorRig.h"
#include "../preset_unit/PresetTestHelpers.h"

#include "vintage/ui/EnvelopePicture.h"
#include "vintage/ui/EnvelopesPage.h"
#include "vintage/ui/LfoPage.h"

#include <algorithm>
#include <cmath>
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

juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<float> pos)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pos,
                             now, 1, false);
}

/** Mouse events on a graph handle at parent coordinates. */
struct HandleMouse
{
    ui::GraphHandle& h;
    juce::Point<float> local (juce::Point<float> parentPoint) const { return parentPoint - h.getBounds().getPosition().toFloat(); }
    void down (juce::Point<float> p) { h.mouseDown (mouseEvent (h, local (p))); }
    void drag (juce::Point<float> p) { h.mouseDrag (mouseEvent (h, local (p))); }
    void up (juce::Point<float> p) { h.mouseUp (mouseEvent (h, local (p))); }
};

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

juce::String textOf (juce::RangedAudioParameter& p)
{
    const auto label = p.getLabel();
    return p.getCurrentValueAsText() + (label.isEmpty() ? juce::String() : " " + label);
}

/** A graphics context that draws nothing and says its clip is empty, so Graphics skips text and path
    work: what is left to allocate is our own code (strings, paths, fonts made in paint()). Every method
    is a no-op, including those whose base-class defaults build a path. */
class NullContext final : public juce::LowLevelGraphicsContext
{
public:
    bool isVectorDevice() const override { return false; }
    void setOrigin (juce::Point<int>) override {}
    void addTransform (const juce::AffineTransform&) override {}
    float getPhysicalPixelScaleFactor() const override { return 1.0f; }
    bool clipToRectangle (const juce::Rectangle<int>&) override { return false; }
    bool clipToRectangleList (const juce::RectangleList<int>&) override { return false; }
    void excludeClipRectangle (const juce::Rectangle<int>&) override {}
    void clipToPath (const juce::Path&, const juce::AffineTransform&) override {}
    void clipToImageAlpha (const juce::Image&, const juce::AffineTransform&) override {}
    bool clipRegionIntersects (const juce::Rectangle<int>&) override { return false; }
    juce::Rectangle<int> getClipBounds() const override { return {}; }
    bool isClipEmpty() const override { return true; }
    void saveState() override {}
    void restoreState() override {}
    void beginTransparencyLayer (float) override {}
    void endTransparencyLayer() override {}
    void setFill (const juce::FillType&) override {}
    void setOpacity (float) override {}
    void setImageBlendMode (juce::BlendMode) override {}
    void setInterpolationQuality (juce::Graphics::ResamplingQuality) override {}
    void fillAll() override {}
    void fillRect (const juce::Rectangle<int>&, bool) override {}
    void fillRect (const juce::Rectangle<float>&) override {}
    void fillRectList (const juce::RectangleList<float>&) override {}
    void fillPath (const juce::Path&, const juce::AffineTransform&) override {}
    void drawRect (const juce::Rectangle<float>&, float) override {}
    void strokePath (const juce::Path&, const juce::PathStrokeType&, const juce::AffineTransform&) override {}
    void drawImage (const juce::Image&, const juce::AffineTransform&) override {}
    void drawLine (const juce::Line<float>&) override {}
    void drawLineWithThickness (const juce::Line<float>&, float) override {}
    void setFont (const juce::Font& f) override { font = f; }
    const juce::Font& getFont() override { return font; }
    void drawGlyphs (juce::Span<const uint16_t>, juce::Span<const juce::Point<float>>, const juce::AffineTransform&) override {}
    std::unique_ptr<juce::ImageType> getPreferredImageTypeForTemporaryImages() const override { return nullptr; }
    void drawRoundedRectangle (const juce::Rectangle<float>&, float, float) override {}
    void fillRoundedRectangle (const juce::Rectangle<float>&, float) override {}
    void drawEllipse (const juce::Rectangle<float>&, float) override {}
    void fillEllipse (const juce::Rectangle<float>&) override {}
    uint64_t getFrameId() const override { return 0; }

private:
    juce::Font font { juce::FontOptions() };
};
} // namespace

// =================================================================================================
// A drag the host contests (9.1.5)
// =================================================================================================

TEST_CASE ("Contested drag, envelope picture: the handles show the user's value and the marker; nothing is written at mouse-up", "[ui][editor][gestures][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    GestureCounter host (rig.proc);
    auto& view = rig.editor->getEnvelopesPage (0).getEnvelopeView (0);
    const auto layout = view.getLayout();

    SECTION ("peak handle (attack)")
    {
        auto& h = view.getHandle (ui::EnvelopeView::Peak);
        HandleMouse m { h };
        const auto c = h.getCentre();
        m.down (c);
        m.drag ({ c.x + 60.0f, c.y });
        rig.tick();
        hostWrite (rig.param ("lower.env1.attack"), 0.95f); // the host keeps writing (Read mode)
        rig.tick();
        CHECK (h.getCentre().x == Approx (c.x + 60.0f).margin (1.0f)); // still under the pointer
        CHECK (view.shownValues().attack == Approx (layout.attackFromX (c.x + 60.0f)).margin (1.0e-3));
        CHECK (h.getMarkerAlpha() > 0.0f);
        const int values = host.values;
        m.up ({ c.x + 60.0f, c.y });
        CHECK (host.values == values); // nothing written at mouse-up
        CHECK (rig.param ("lower.env1.attack").getValue() == Approx (0.95f));
        rig.tick();
        CHECK (view.shownValues().attack == Approx (0.95f));
        CHECK (h.getCentre().x == Approx (layout.points (view.shownValues()).peakX).margin (0.6));
    }
    SECTION ("decay and sustain handle (two parameters)")
    {
        auto& h = view.getHandle (ui::EnvelopeView::SustainStart);
        HandleMouse m { h };
        const auto c = h.getCentre();
        m.down (c);
        m.drag ({ c.x + 30.0f, c.y - 30.0f });
        const float mineDecay = view.shownValues().decay, mineSustain = view.shownValues().sustain;
        rig.tick();
        hostWrite (rig.param ("lower.env1.sustain"), 0.05f);
        hostWrite (rig.param ("lower.env1.decay"), 0.9f);
        rig.tick();
        CHECK (h.getCentre().x == Approx (c.x + 30.0f).margin (1.0f));
        CHECK (h.getCentre().y == Approx (c.y - 30.0f).margin (1.0f));
        CHECK (view.shownValues().decay == Approx (mineDecay).margin (1.0e-3));
        CHECK (view.shownValues().sustain == Approx (mineSustain).margin (1.0e-3));
        CHECK (h.getMarkerAlpha() > 0.0f);
        const int values = host.values;
        m.up ({ c.x + 30.0f, c.y - 30.0f });
        CHECK (host.values == values);
        CHECK (rig.param ("lower.env1.sustain").getValue() == Approx (0.05f));
        CHECK (rig.param ("lower.env1.decay").getValue() == Approx (0.9f));
    }
    rig.tick (400.0);
    CHECK (host.begins == host.ends);
}

TEST_CASE ("Contested drag, LFO picture: the handles show the user's value and the marker; nothing is written at mouse-up", "[ui][editor][gestures][lfo]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    GestureCounter host (rig.proc);
    auto& view = rig.editor->getLfoPage (0).getLfoView();
    const auto axis = view.getAxis();
    SECTION ("delay handle")
    {
        auto& h = view.getHandle (ui::LfoView::Delay);
        HandleMouse m { h };
        const auto c = h.getCentre();
        const float x = axis.xOf (0.3f);
        m.down (c);
        m.drag ({ x, c.y });
        rig.tick();
        hostWrite (rig.param ("lower.lfo.delay"), 0.9f);
        rig.tick();
        CHECK (h.getCentre().x == Approx (x).margin (1.0f));
        CHECK (view.getAxis().windowSec == Approx (axis.windowSec)); // kept through the drag
        CHECK (h.getMarkerAlpha() > 0.0f);
        const int values = host.values;
        m.up ({ x, c.y });
        CHECK (host.values == values);
        CHECK (rig.param ("lower.lfo.delay").getValue() == Approx (0.9f));
    }
    SECTION ("cycle handle")
    {
        auto& h = view.getHandle (ui::LfoView::Cycle);
        HandleMouse m { h };
        const auto c = h.getCentre();
        const float x = axis.xOf (0.45f);
        m.down (c);
        m.drag ({ x, c.y });
        rig.tick();
        hostWrite (rig.param ("lower.lfo.rate"), 0.05f);
        rig.tick();
        CHECK (h.getCentre().x == Approx (x).margin (1.0f));
        CHECK (h.getMarkerAlpha() > 0.0f);
        const int values = host.values;
        m.up ({ x, c.y });
        CHECK (host.values == values);
        CHECK (rig.param ("lower.lfo.rate").getValue() == Approx (0.05f));
    }
    rig.tick (400.0);
    CHECK (host.begins == host.ends);
}

// =================================================================================================
// The LFO window (9.3.3)
// =================================================================================================

TEST_CASE ("LFO window: kept through a drag and after it while both handles are in the middle; chosen again at the edge; a handle is never clamped", "[ui][editor][gestures][lfo]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& view = rig.editor->getLfoPage (1).getLfoView();
    const auto axis0 = view.getAxis();
    CHECK (axis0.windowSec == Approx (1.0f)); // default 3.85 Hz, no delay

    // A small drag of the delay handle: the released handle stays under the pointer.
    auto& dh = view.getHandle (ui::LfoView::Delay);
    HandleMouse dm { dh };
    auto c = dh.getCentre();
    float x = axis0.xOf (0.15f);
    dm.down (c);
    dm.drag ({ x, c.y });
    dm.up ({ x, c.y });
    CHECK (dh.getCentre().x == Approx (x).margin (1.0f)); // before the next tick ...
    rig.tick();
    CHECK (dh.getCentre().x == Approx (x).margin (1.0f)); // ... and after it
    CHECK (view.getAxis().windowSec == Approx (axis0.windowSec));

    // A small drag of the cycle handle likewise.
    auto& ch = view.getHandle (ui::LfoView::Cycle);
    HandleMouse cm { ch };
    c = ch.getCentre();
    x = axis0.xOf (0.6f);
    cm.down (c);
    cm.drag ({ x, c.y });
    cm.up ({ x, c.y });
    rig.tick();
    CHECK (ch.getCentre().x == Approx (x).margin (1.0f));
    CHECK (view.getAxis().windowSec == Approx (axis0.windowSec));

    // Dragged to the edge (past 90 %): the window is chosen again at release, and holds both handles.
    c = dh.getCentre();
    x = axis0.xOf (0.97f);
    dm.down (c);
    dm.drag ({ x, c.y });
    CHECK (view.getAxis().windowSec == Approx (axis0.windowSec)); // kept during the drag itself
    dm.up ({ x, c.y });
    rig.tick();
    const float delaySec = map::lfoDelaySec (rig.param ("upper.lfo.delay").getValue());
    const float rateHz = map::lfoRateHz (rig.param ("upper.lfo.rate").getValue());
    CHECK (view.getAxis().windowSec > axis0.windowSec);
    CHECK (ui::model::lfoWindowKeeps (view.getAxis().windowSec, delaySec, rateHz));
    CHECK (view.getAxis().windowSec == Approx (ui::model::lfoWindowFor (delaySec, rateHz)));

    // Delay 5 s with 0.05 Hz: the cycle ends at 25 s, beyond the old 20 s: the handle is at its true place.
    hostWrite (rig.param ("upper.lfo.delay"), 1.0f);
    hostWrite (rig.param ("upper.lfo.rate"), 0.0f);
    rig.tick();
    const auto& a = view.getAxis();
    CHECK (a.windowSec >= 25.0f);
    const auto cycle = view.getHandle (ui::LfoView::Cycle).getCentre();
    CHECK (cycle.x == Approx (a.xOf (5.0f + 20.0f)).margin (0.6));
    CHECK (cycle.x < a.x0 + a.width - 10.0f); // not at the plot edge
    CHECK (view.getHandle (ui::LfoView::Delay).getCentre().x == Approx (a.xOf (5.0f)).margin (0.6));
    juce::String why;
    for (const auto& e : rig.editor->getRegistry().entries())
        if (e.page == "lfo")
            CHECK (e.view->showsMirroredValue (why));
}

TEST_CASE ("LFO picture: the delay handle sits on the fade-in's envelope line; the start marker and captions say what it is; Random and Noise are examples", "[ui][editor][lfo]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& view = rig.editor->getLfoPage (0).getLfoView();
    auto& delay = rig.param ("lower.lfo.delay");

    CHECK (view.delayCaption().isEmpty()); // no delay: neither marker caption
    CHECK (view.startCaption().isEmpty());
    CHECK (view.cycleCaption().startsWith ("1 cycle (rate "));
    CHECK (view.cycleCaption().contains (textOf (rig.param ("lower.lfo.rate"))));

    hostWrite (delay, 0.4f); // 0.8 s
    rig.tick();
    CHECK (view.delayCaption() == "Full strength after " + textOf (delay) + " (drag)");
    CHECK (view.startCaption() == "starts");
    const auto& axis = view.getAxis();
    CHECK (view.startMarkerX() == Approx (axis.xOf (0.4f)).margin (0.6)); // half the delay: silent until here
    const auto dc = view.getHandle (ui::LfoView::Delay).getCentre();
    const auto cc = view.getHandle (ui::LfoView::Cycle).getCentre();
    CHECK (dc.x == Approx (axis.xOf (0.8f)).margin (0.6));
    // On the envelope line: the top of the wedge, 46 % of the plot's height above the zero line the cycle handle sits on.
    const float plotHeight = static_cast<float> (view.getHeight()) - 28.0f - 18.0f;
    CHECK (cc.y - dc.y == Approx (0.46f * plotHeight).margin (0.6));

    // Random and Noise are different every time; the others say nothing.
    for (int wave = 0; wave < 6; ++wave)
    {
        hostWrite (rig.param ("lower.lfo.waveform"), static_cast<float> (wave) / 5.0f);
        rig.tick();
        INFO ("wave " << wave);
        if (wave >= 4)
            CHECK (view.exampleCaption() == "Example: different every time");
        else
            CHECK (view.exampleCaption().isEmpty());
    }
}

TEST_CASE ("LFO picture: Random and Noise do not start flat (the real LFO is free-running)", "[ui][editor][lfo]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& view = rig.editor->getLfoPage (0).getLfoView();
    for (int wave : { 4, 5 })
    {
        hostWrite (rig.param ("lower.lfo.waveform"), static_cast<float> (wave) / 5.0f);
        rig.tick();
        const float rateHz = map::lfoRateHz (rig.param ("lower.lfo.rate").getValue());
        const int firstCycle = static_cast<int> (view.getAxis().windowSec / ui::model::LfoPreview::kPoints > 0.0f
                                                     ? (1.0f / rateHz) / (view.getAxis().windowSec / ui::model::LfoPreview::kPoints)
                                                     : 1.0f);
        int moving = 0;
        for (int i = 0; i < firstCycle; ++i)
            if (std::abs (view.shownPreview().y[static_cast<size_t> (i)]) > 1.0e-3f)
                ++moving;
        INFO ("wave " << wave << ": " << moving << " of the first " << firstCycle << " points are not 0");
        CHECK (moving > firstCycle * 9 / 10);
    }
}

// =================================================================================================
// Envelope picture: time labels, "key released", panel header
// =================================================================================================

TEST_CASE ("Envelope picture: the time labels are the parameters' own text, spread without overlap; \"key released\" is clear of every handle", "[ui][editor][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& view = rig.editor->getEnvelopesPage (0).getEnvelopeView (0);
    const float width = ui::EnvelopeView::timeLabelWidth();
    const auto check = [&] (const juce::String& when) {
        INFO (when);
        // attack, decay and release (not the sustain's text)
        CHECK (view.timeLabel (0) == textOf (rig.param ("lower.env1.attack")));
        CHECK (view.timeLabel (1) == textOf (rig.param ("lower.env1.decay")));
        CHECK (view.timeLabel (2) == textOf (rig.param ("lower.env1.release")));
        const float w = static_cast<float> (view.getWidth());
        for (int i = 0; i < 3; ++i)
        {
            CHECK (view.timeLabelCentre (i) - 0.5f * width >= -0.01f);
            CHECK (view.timeLabelCentre (i) + 0.5f * width <= w + 0.01f);
            if (i > 0)
                CHECK (view.timeLabelCentre (i) - view.timeLabelCentre (i - 1) >= width - 0.01f);
        }
        const auto area = view.keyReleasedArea();
        CHECK (view.getLocalBounds().toFloat().contains (area));
        for (int i = 0; i < view.numHandles(); ++i)
            CHECK_FALSE (area.intersects (view.getHandle (i).getBounds().toFloat()));
    };
    // Distinct values first (a label showing the wrong parameter shows), then every crowded corner.
    hostWrite (rig.param ("lower.env1.attack"), 0.2f);
    hostWrite (rig.param ("lower.env1.decay"), 0.5f);
    hostWrite (rig.param ("lower.env1.sustain"), 0.8f);
    hostWrite (rig.param ("lower.env1.release"), 0.7f);
    rig.tick();
    check ("distinct values");
    CHECK (view.timeLabel (2) != textOf (rig.param ("lower.env1.sustain")));
    std::mt19937 rng (4);
    std::uniform_real_distribution<float> uni (0.0f, 1.0f);
    for (int i = 0; i < 60; ++i)
    {
        const float t = i < 10 ? 0.0f : (i < 20 ? 1.0f : -1.0f);
        for (const char* id : { "attack", "decay", "release" })
            hostWrite (rig.param (juce::String ("lower.env1.") + id), t < 0.0f ? uni (rng) : t);
        hostWrite (rig.param ("lower.env1.sustain"), i % 3 == 0 ? 1.0f : (i % 3 == 1 ? 0.0f : uni (rng)));
        rig.tick();
        check ("setting " + juce::String (i));
    }
}

TEST_CASE ("Envelope panels: the header text and the accessibility title follow the filter source and are repainted", "[ui][editor][envelopes][repaint]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.editor->setSizePercent (100);
    rig.editor->selectPage (1, 0);
    rig.editor->setVisible (true);
    rig.tick();
    auto& ep = rig.editor->getEnvelopesPage (0);
    for (int n = 0; n < 2; ++n)
    {
        CHECK (ep.getPanel (n).getHeaderText() == ep.getPanelTitle (n).toUpperCase());
        CHECK (ep.getPanel (n).getTitle() == ep.getPanelTitle (n));
    }
    auto* recorder = new RepaintRecorder();
    rig.editor->setCachedComponentImage (recorder);
    rig.tick (3000.0);
    hostWrite (rig.param ("lower.vcf.env_source"), 1.0f);
    recorder->clear();
    rig.tick();
    for (int n = 0; n < 2; ++n)
    {
        const auto& panel = ep.getPanel (n);
        INFO ("panel " << n);
        CHECK (panel.getHeaderText() == "ENVELOPE " + juce::String (n + 1) + (n == 0 ? " (PITCH)" : " (LOUDNESS, FILTER)"));
        CHECK (panel.getTitle() == ep.getPanelTitle (n));
        CHECK (panel.getTitle() == (n == 0 ? "Envelope 1 (pitch)" : "Envelope 2 (loudness, filter)"));
        // The new header is asked to be drawn: its strip is inside what was repainted.
        juce::RectangleList<int> repainted;
        for (const auto& a : recorder->areas)
            repainted.add (a);
        const auto header = rig.editor->getLocalArea (&panel, panel.getLocalBounds().removeFromTop (ui::metrics::kHeader));
        CHECK (repainted.containsRectangle (header));
    }
    rig.editor->setCachedComponentImage (nullptr);
}

// =================================================================================================
// Nothing repaints at rest (9.5)
// =================================================================================================

TEST_CASE ("Envelopes and LFO pages: nothing repaints at rest", "[ui][editor][repaint][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.editor->setSizePercent (100);
    rig.editor->setVisible (true);
    auto* recorder = new RepaintRecorder();
    rig.editor->setCachedComponentImage (recorder);
    for (int page : { 1, 2 })
        for (int part = 0; part < 2; ++part)
        {
            rig.editor->selectPage (page, part);
            rig.tick (3000.0);
            rig.tick (3000.0); // markers gone, the page settled
            recorder->clear();
            for (int i = 0; i < 25; ++i)
                rig.tick (20.0);
            INFO ("page " << page << " part " << part);
            CHECK (recorder->all == 0);
            CHECK (recorder->areas.empty());
        }
    rig.editor->setCachedComponentImage (nullptr);
}

// =================================================================================================
// Paint allocations of our own code (9.5)
// =================================================================================================

#if VS_SEQ_COUNT_ALLOCATIONS
TEST_CASE ("Paint allocations: the pictures and pages build no string, path or font while painting (a context that draws nothing)", "[ui][editor][alloc][envelopes]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    // States that exercise every branch of paint(): an inverted Envelope 1, a delayed Random LFO (all captions).
    rig.set ("lower.env1.polarity", 1.0f);
    rig.set ("lower.lfo.waveform", 4.0f);
    rig.set ("lower.lfo.delay", 0.4f);
    rig.tick();
    NullContext context;
    juce::Graphics g (context);
    const auto allocations = [&] (const std::function<void()>& paint) {
        paint(); // warm-up: lazily made things are not paint()'s own
        vs::seq::test::AllocationCounter counter;
        paint();
        return counter.count();
    };
    auto& ep = rig.editor->getEnvelopesPage (0);
    auto& lp = rig.editor->getLfoPage (0);
    struct Item
    {
        const char* name;
        std::function<void()> paint;
    };
    const Item items[] {
        { "Envelope 1 picture", [&] { ep.getEnvelopeView (0).paint (g); } },
        { "Envelope 2 picture", [&] { ep.getEnvelopeView (1).paint (g); } },
        { "LFO picture", [&] { lp.getLfoView().paint (g); } },
        { "Envelopes page", [&] { ep.paint (g); ep.paintOverChildren (g); } },
        { "LFO page", [&] { lp.paint (g); lp.paintOverChildren (g); } },
    };
    for (const auto& item : items)
    {
        const int n = allocations (item.paint);
        INFO (item.name << ": " << n << " allocations in one paint");
        CHECK (n == 0);
    }
}
#endif
