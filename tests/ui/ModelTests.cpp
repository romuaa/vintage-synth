// Interaction models of the editor's controls (docs/architecture.md 9.2.1, 9.1.5, 9.6 "Models" and
// "Gestures"): value <-> position, drag, fine drag, wheel, keys, snapping, graph axes and handle
// maths, and the gesture rules (begin before the first value, end after the last, balanced, never
// nested), driven headless through a recording target. JUCE-free code only.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "common/ui/models/GraphModels.h"
#include "common/ui/models/SegmentModel.h"
#include "common/ui/models/SliderModel.h"

#include <string>
#include <vector>

using namespace vs::ui;
using namespace vs::ui::model;
using Catch::Approx;

namespace
{
/** A parameter that records every gesture call and checks the rules as it goes. */
struct Recorder final : ParamTarget
{
    explicit Recorder (float initial = 0.5f, int stepsIn = 0, float def = 0.25f) : v (initial), steps (stepsIn), defValue (def) {}

    float value() const noexcept override { return v; }
    float defaultValue() const noexcept override { return defValue; }
    int numSteps() const noexcept override { return steps; }
    void beginGesture() override
    {
        if (open)
            ++nested;
        open = true;
        ++begins;
        log.push_back ("b");
    }
    void setValue (float x) override
    {
        if (! open)
            ++outside;
        v = x;
        ++values;
        log.push_back ("v");
    }
    void endGesture() override
    {
        if (! open)
            ++unmatchedEnds;
        open = false;
        ++ends;
        log.push_back ("e");
    }
    /** Every value inside a gesture, gestures balanced and never nested. */
    bool clean() const { return ! open && nested == 0 && outside == 0 && unmatchedEnds == 0 && begins == ends; }

    float v;
    int steps;
    float defValue;
    bool open = false;
    int begins = 0, ends = 0, values = 0, nested = 0, outside = 0, unmatchedEnds = 0;
    std::vector<std::string> log;
};
} // namespace

TEST_CASE ("Slider model: value <-> position over the track", "[ui][models]")
{
    Recorder r;
    SliderModel m (r);
    m.setTrack (10.0f, 200.0f);
    CHECK (m.positionOf (0.0f) == Approx (10.0f));
    CHECK (m.positionOf (1.0f) == Approx (210.0f));
    CHECK (m.positionOf (0.25f) == Approx (60.0f));
    CHECK (m.valueAt (110.0f) == Approx (0.5f));
    CHECK (m.valueAt (-50.0f) == Approx (0.0f)); // clamped
    CHECK (m.valueAt (900.0f) == Approx (1.0f));
}

TEST_CASE ("Slider model: drag is relative, full track = full range, fine x 0.1, a click on the track jumps", "[ui][models]")
{
    Recorder r (0.5f);
    SliderModel m (r);
    m.setTrack (0.0f, 200.0f);

    SECTION ("grab the thumb: no jump, relative drag")
    {
        m.mouseDown (103.0f, true, false); // 3 px beside the thumb centre
        CHECK (r.values == 0);
        CHECK (r.begins == 1);
        m.mouseDrag (143.0f, false); // +40 px = +0.2
        CHECK (r.v == Approx (0.7f));
        m.mouseDrag (143.0f + 2000.0f, false);
        CHECK (r.v == Approx (1.0f)); // clamped
        m.mouseUp();
        CHECK (r.clean());
        CHECK (r.log.front() == "b");
        CHECK (r.log.back() == "e");
    }
    SECTION ("fine modifier: x 0.1, switching mid-drag continues from where the value is")
    {
        m.mouseDown (100.0f, true, true);
        m.mouseDrag (140.0f, true); // +40 px fine = +0.02
        CHECK (r.v == Approx (0.52f));
        m.mouseDrag (160.0f, false); // modifier released: re-anchored, +20 px coarse = +0.1
        CHECK (r.v == Approx (0.52f));
        m.mouseDrag (180.0f, false);
        CHECK (r.v == Approx (0.62f));
        m.mouseUp();
        CHECK (r.clean());
    }
    SECTION ("a press on the track jumps there, then drags from there")
    {
        m.mouseDown (20.0f, false, false);
        CHECK (r.v == Approx (0.1f));
        CHECK (r.log == std::vector<std::string> { "b", "v" });
        m.mouseDrag (40.0f, false);
        CHECK (r.v == Approx (0.2f));
        m.mouseUp();
        CHECK (r.clean());
    }
    SECTION ("during a drag the user's value is shown")
    {
        m.mouseDown (100.0f, true, false);
        m.mouseDrag (120.0f, false);
        r.v = 0.9f; // the host writes meanwhile
        CHECK (m.isDragging());
        CHECK (m.dragValue() == Approx (0.6f));
        m.mouseUp();
        CHECK (r.v == Approx (0.9f)); // nothing written at mouse-up
        CHECK (r.clean());
    }
}

TEST_CASE ("Slider model: stepped parameters snap to their grid", "[ui][models]")
{
    Recorder r (0.0f, 5); // 0, .25, .5, .75, 1
    SliderModel m (r);
    m.setTrack (0.0f, 100.0f);
    m.mouseDown (0.0f, true, false);
    m.mouseDrag (10.0f, false);
    CHECK (r.v == Approx (0.0f));
    m.mouseDrag (14.0f, false);
    CHECK (r.v == Approx (0.25f));
    m.mouseDrag (60.0f, false);
    CHECK (r.v == Approx (0.5f));
    m.mouseUp();
    CHECK (r.clean());
    CHECK (snap (0.37f, 5) == Approx (0.25f));
    CHECK (snap (0.38f, 5) == Approx (0.5f));
    CHECK (stepIndex (0.74f, 5) == 3);
    CHECK (snap (0.37f, 0) == Approx (0.37f));
}

TEST_CASE ("Slider model: double-click resets, typed values write once", "[ui][models]")
{
    Recorder r (0.9f, 0, 0.3f);
    SliderModel m (r);
    m.doubleClick();
    CHECK (r.v == Approx (0.3f));
    CHECK (r.log == std::vector<std::string> { "b", "v", "e" });
    m.typed (0.42f);
    CHECK (r.v == Approx (0.42f));
    CHECK (r.begins == 2);
    CHECK (r.clean());
}

TEST_CASE ("Slider model: wheel 1 % per notch (0.1 % fine), one step for discrete, gesture ends 300 ms after the last notch", "[ui][models]")
{
    Recorder r (0.5f);
    SliderModel m (r);
    m.wheel (1.0f, false, 1000.0);
    CHECK (r.v == Approx (0.51f));
    m.wheel (-1.0f, true, 1100.0);
    CHECK (r.v == Approx (0.509f));
    m.wheel (2.0f, false, 1200.0);
    CHECK (r.v == Approx (0.529f));
    CHECK (r.begins == 1); // one gesture for the burst
    m.idle (1499.0);
    CHECK (r.ends == 0);
    m.idle (1500.0);
    CHECK (r.ends == 1);
    CHECK (r.clean());

    Recorder d (0.5f, 3); // 0, .5, 1
    SliderModel md (d);
    md.wheel (0.2f, false, 0.0);
    CHECK (d.v == Approx (1.0f));
    md.wheel (-0.1f, false, 10.0);
    CHECK (d.v == Approx (0.5f));
    md.idle (400.0);
    CHECK (d.clean());
}

TEST_CASE ("Slider model: keys +-1 % (fine 0.1 %), Page +-10 %, Home/End, Delete = default; discrete one step", "[ui][models]")
{
    Recorder r (0.5f, 0, 0.2f);
    SliderModel m (r);
    m.key (Key::Increase, false, 0.0);
    CHECK (r.v == Approx (0.51f));
    m.key (Key::Decrease, true, 10.0);
    CHECK (r.v == Approx (0.509f));
    m.key (Key::PageUp, false, 20.0);
    CHECK (r.v == Approx (0.609f));
    m.key (Key::PageDown, false, 30.0);
    CHECK (r.v == Approx (0.509f));
    m.key (Key::End, false, 40.0);
    CHECK (r.v == Approx (1.0f));
    m.key (Key::Home, false, 50.0);
    CHECK (r.v == Approx (0.0f));
    m.key (Key::Reset, false, 60.0);
    CHECK (r.v == Approx (0.2f));
    CHECK (r.begins == 1);
    m.idle (360.0);
    CHECK (r.clean());

    Recorder d (0.0f, 13); // bend range 0..12
    SliderModel md (d);
    md.key (Key::Increase, false, 0.0);
    CHECK (stepIndex (d.v, 13) == 1);
    md.key (Key::PageUp, false, 1.0);
    CHECK (stepIndex (d.v, 13) == 2); // a tenth of 12 steps, rounded: 1 step
    md.cancel();
    CHECK (d.clean());
}

TEST_CASE ("Slider model: cancel ends an open drag or wheel gesture (editor closing mid-drag)", "[ui][models]")
{
    Recorder r;
    SliderModel m (r);
    m.setTrack (0.0f, 100.0f);
    m.mouseDown (50.0f, true, false);
    m.mouseDrag (60.0f, false);
    m.cancel();
    CHECK (r.clean());
    m.wheel (1.0f, false, 0.0);
    m.cancel();
    CHECK (r.clean());
}

TEST_CASE ("Segment model: hit test, one gesture per press, arrows to the neighbour, double-click resets", "[ui][models]")
{
    Recorder r (0.0f, 6, 0.4f);
    SegmentModel m (r, 6);
    CHECK (m.indexAt (0.0f, 600.0f) == 0);
    CHECK (m.indexAt (99.9f, 600.0f) == 0);
    CHECK (m.indexAt (100.0f, 600.0f) == 1);
    CHECK (m.indexAt (599.0f, 600.0f) == 5);
    CHECK (m.indexAt (600.0f, 600.0f) == -1);
    CHECK (m.indexAt (-1.0f, 600.0f) == -1);

    m.press (3);
    CHECK (r.log == std::vector<std::string> { "b", "v" }); // begin at mouse-down, one value
    CHECK (m.index() == 3);
    m.release();
    CHECK (r.log.back() == "e");
    m.step (1, 0.0);
    CHECK (m.index() == 4);
    m.step (1, 10.0);
    m.step (1, 20.0); // at the end: nothing (no wrap)
    CHECK (m.index() == 5);
    m.key (Key::Home, 30.0);
    CHECK (m.index() == 0);
    m.idle (400.0);
    m.doubleClick();
    CHECK (m.index() == 2); // default 0.4 -> segment 2 of 6
    CHECK (r.clean());
}

TEST_CASE ("Toggle model: a press flips the value inside one gesture", "[ui][models]")
{
    Recorder r (0.0f, 2, 0.0f);
    ToggleModel t (r);
    CHECK_FALSE (t.isOn());
    t.press();
    CHECK (t.isOn());
    t.release();
    t.press();
    t.release();
    CHECK_FALSE (t.isOn());
    CHECK (r.log == std::vector<std::string> { "b", "v", "e", "b", "v", "e" });
    t.key (Key::Increase, 0.0);
    CHECK (t.isOn());
    t.idle (500.0);
    CHECK (r.clean());
}

TEST_CASE ("Graph axes: log frequency 20 Hz - 20 kHz and the dB axis round-trip", "[ui][models]")
{
    LogAxis x { 10.0f, 1000.0f, 20.0, 20000.0 };
    CHECK (x.positionOf (20.0) == Approx (10.0f));
    CHECK (x.positionOf (20000.0) == Approx (1010.0f));
    CHECK (x.positionOf (632.455532) == Approx (510.0f).margin (0.01f)); // geometric centre
    for (double hz : { 31.0, 440.0, 5000.0, 19000.0 })
        CHECK (x.valueAt (x.positionOf (hz)) == Approx (hz).epsilon (1e-5));
    CHECK (x.clampPx (-5.0f) == Approx (10.0f));

    LinearAxis y { 0.0f, 720.0f, 24.0, -48.0 };
    CHECK (y.positionOf (24.0) == Approx (0.0f));
    CHECK (y.positionOf (0.0) == Approx (240.0f));
    CHECK (y.positionOf (-48.0) == Approx (720.0f));
    CHECK (y.valueAt (480.0f) == Approx (-24.0));
}

TEST_CASE ("Handle model: x keeps the grab offset and maps through the axis, y is relative (200 px = full range)", "[ui][models]")
{
    Recorder xr (0.5f), yr (0.2f);
    HandleModel h;
    h.setX (&xr, [] (float px) { return clamp01 (px / 1000.0f); }); // 1000 px = full range
    h.setY (&yr, 200.0f);

    // Grab 4 px right of the handle at 500 px: no change until the pointer moves.
    h.mouseDown (504.0f, 300.0f, 500.0f, false);
    CHECK (xr.values == 0);
    CHECK (xr.begins == 0); // a parameter's gesture begins when the drag first changes it
    h.mouseDrag (604.0f, 300.0f, false);
    CHECK (xr.v == Approx (0.6f));
    CHECK (xr.begins == 1);
    CHECK (yr.begins == 0); // y untouched: no gesture for it
    h.mouseDrag (604.0f, 260.0f, false); // up 40 px = +0.2
    CHECK (yr.v == Approx (0.4f));
    CHECK (yr.begins == 1);
    // Fine: x 0.1, re-anchored at the switch.
    h.mouseDrag (604.0f, 260.0f, true);
    h.mouseDrag (704.0f, 160.0f, true);
    CHECK (xr.v == Approx (0.61f));
    CHECK (yr.v == Approx (0.45f));
    h.mouseUp();
    CHECK (xr.clean());
    CHECK (yr.clean());
    CHECK (xr.log.back() == "e");

    // Double-click: both back to their defaults, one gesture each.
    h.doubleClick();
    CHECK (xr.v == Approx (0.25f));
    CHECK (yr.v == Approx (0.25f));
    // Arrow keys: timed gestures, closed by idle.
    h.nudge (1, -1, false, 0.0);
    CHECK (xr.v == Approx (0.26f));
    CHECK (yr.v == Approx (0.24f));
    h.idle (299.0);
    CHECK (xr.open);
    h.idle (300.0);
    CHECK (xr.clean());
    CHECK (yr.clean());
}

TEST_CASE ("Handle model: cancel mid-drag ends every touched gesture", "[ui][models]")
{
    Recorder xr (0.5f), yr (0.5f);
    HandleModel h;
    h.setX (&xr, [] (float px) { return clamp01 (px / 100.0f); });
    h.setY (&yr, 200.0f);
    h.mouseDown (50.0f, 50.0f, 50.0f, false);
    h.mouseDrag (60.0f, 20.0f, false);
    REQUIRE (xr.open);
    REQUIRE (yr.open);
    h.cancel();
    CHECK (xr.clean());
    CHECK (yr.clean());
}

TEST_CASE ("Gesture rules hold for every interaction of every model (random interaction fuzz)", "[ui][models]")
{
    // 9.1.5: never a value without a gesture, never two begins without an end, balanced at the end.
    std::uint32_t seed = 12345u;
    auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (int round = 0; round < 200; ++round)
    {
        Recorder rs (0.5f, (round % 3 == 0) ? 7 : 0), rg (0.3f, 4), rx (0.5f), ry (0.5f);
        SliderModel s (rs);
        s.setTrack (0.0f, 150.0f);
        SegmentModel g (rg, 4);
        HandleModel h;
        h.setX (&rx, [] (float px) { return clamp01 (px / 300.0f); });
        h.setY (&ry, 200.0f);
        double t = 0.0;
        for (int step = 0; step < 300; ++step)
        {
            t += static_cast<double> (next() % 200);
            const float px = static_cast<float> (next() % 400) - 50.0f;
            switch (next() % 14)
            {
                case 0: s.mouseDown (px, next() % 2 == 0, next() % 2 == 0); break;
                case 1: s.mouseDrag (px, next() % 2 == 0); break;
                case 2: s.mouseUp(); break;
                case 3: s.wheel (static_cast<float> (static_cast<int> (next() % 5) - 2), next() % 2 == 0, t); break;
                case 4: s.key (static_cast<Key> (next() % 7), next() % 2 == 0, t); break;
                case 5: s.doubleClick(); break;
                case 6: g.press (static_cast<int> (next() % 5) - 1); break;
                case 7: g.release(); break;
                case 8: g.key (static_cast<Key> (next() % 7), t); break;
                case 9: h.mouseDown (px, px, 150.0f, next() % 2 == 0); break;
                case 10: h.mouseDrag (px, static_cast<float> (next() % 300), next() % 2 == 0); break;
                case 11: h.mouseUp(); break;
                case 12: h.nudge (static_cast<int> (next() % 3) - 1, static_cast<int> (next() % 3) - 1, false, t); break;
                default:
                    s.idle (t);
                    g.idle (t);
                    h.idle (t);
                    break;
            }
            REQUIRE (rs.nested == 0);
            REQUIRE (rs.outside == 0);
            REQUIRE (rg.nested == 0);
            REQUIRE (rg.outside == 0);
            REQUIRE (rx.outside == 0);
            REQUIRE (ry.outside == 0);
            REQUIRE (rx.nested == 0);
        }
        s.cancel();
        g.cancel();
        h.cancel();
        CHECK (rs.clean());
        CHECK (rg.clean());
        CHECK (rx.clean());
        CHECK (ry.clean());
    }
}
