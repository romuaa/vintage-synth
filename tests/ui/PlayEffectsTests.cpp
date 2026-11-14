// M7c: the Play and Effects pages (docs/architecture.md 9.3.4, 9.3.6, 9.4, 9.6): the keyboard
// strip's geometry and split-key model, the effect pictures against the real effect blocks, the
// power switches, the pages' inactive rules and automation follow, handle drags, layout goldens,
// paint allocations, and the hidden [.uishots] pictures of both pages in a lively state.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EditorRig.h"

#include "common/engine/KeyRouter.h"
#include "common/fx/Chorus.h"
#include "common/fx/Delay.h"
#include "common/ui/models/FxPictureModels.h"
#include "common/ui/models/KeyboardModel.h"
#include "vintage/ui/EffectsPage.h"
#include "vintage/ui/PlayPage.h"

#include <cmath>
#include <random>
#include <thread>

using namespace vs;
using namespace vs::itest;
using namespace vs::ui::model;
using Catch::Approx;

namespace
{
/** A parameter that records gestures and checks the rules as it goes. */
struct Rec final : vs::ui::ParamTarget
{
    explicit Rec (float initial = 0.5f, int stepsIn = 128, float def = 60.0f / 127.0f) : v (initial), steps (stepsIn), defValue (def) {}
    float value() const noexcept override { return v; }
    float defaultValue() const noexcept override { return defValue; }
    int numSteps() const noexcept override { return steps; }
    void beginGesture() override
    {
        nested += open ? 1 : 0;
        open = true;
        ++begins;
    }
    void setValue (float x) override
    {
        outside += open ? 0 : 1;
        v = x;
        ++values;
    }
    void endGesture() override
    {
        ends += open ? 1 : 0;
        open = false;
    }
    float v;
    int steps;
    float defValue;
    bool open = false;
    int begins = 0, ends = 0, values = 0, outside = 0, nested = 0;
};

void hostWrite (juce::RangedAudioParameter& p, float normalised)
{
    std::thread ([&p, normalised] { p.setValueNotifyingHost (normalised); }).join();
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

const ui::ControlEntry* primaryOf (const ui::ControlRegistry& reg, const juce::String& id)
{
    for (const auto& e : reg.entries())
        if (e.paramId == id && e.placement == ui::Placement::Primary)
            return &e;
    return nullptr;
}

/** A mouse event at `p` (in `c`'s coordinates) as the component would receive it. */
juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> p, bool popup = false)
{
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p,
                             popup ? juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier | juce::ModifierKeys::popupMenuClickModifier) : juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                             0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, juce::Time::getCurrentTime(), p, juce::Time::getCurrentTime(), 1, false);
}

/** The centre of a note's key (a black key: of its upper part) in the strip's coordinates. */
juce::Point<float> keyCentre (const ui::KeyboardStrip& s, int note)
{
    const auto k = s.getGeometry().keyRect (note);
    const float y = isBlackKey (note) ? k.h * 0.4f : k.h * 0.85f;
    return { s.keysArea().getX() + k.x + k.w * 0.5f, s.keysArea().getY() + y };
}

/** The handle stands where the page's mapping puts it for the mirror's values. */
bool handleFollows (ui::GraphHandle& h) { return h.getCentre().getDistanceFrom (h.expectedCentre()) < 0.5f; }

float plain (juce::RangedAudioParameter& p) { return p.convertFrom0to1 (p.getValue()); }
} // namespace

// =================================================================================================
// Models
// =================================================================================================

TEST_CASE ("Keyboard geometry: 73 keys, 43 white; windows shift by octaves with an octave to spare", "[ui][m7c][models]")
{
    KeyboardGeometry g;
    g.setSize (430.0f, 100.0f);
    CHECK (g.low() == 24);
    CHECK (g.high() == 96);
    int whites = 0, blacks = 0;
    for (int n = g.low(); n <= g.high(); ++n)
        (isBlackKey (n) ? blacks : whites)++;
    CHECK (whites == KeyboardGeometry::kWhiteKeys);
    CHECK (blacks == 30);
    CHECK (g.whiteWidth() == Approx (10.0f));
    // C1 is the first key, C7 the last: flush with both edges.
    CHECK (g.keyRect (24).x == Approx (0.0f));
    CHECK (g.keyRect (96).x + g.keyRect (96).w == Approx (430.0f));
    // Black keys stand on the boundary between their neighbours, over their upper part only.
    const auto cs = g.keyRect (25), c = g.keyRect (24), d = g.keyRect (26);
    CHECK (cs.x + cs.w * 0.5f == Approx (c.x + c.w));
    CHECK (d.x == Approx (c.x + c.w));
    CHECK (cs.h < c.h);
    // Hit test: black keys first, then white; outside is -1.
    CHECK (g.noteAt (cs.x + 1.0f, 5.0f) == 25);
    CHECK (g.noteAt (cs.x + 1.0f, 90.0f) == 24); // under the black key: the white key
    CHECK (g.noteAt (-1.0f, 5.0f) == -1);
    CHECK (g.noteAt (430.0f, 5.0f) == -1);
    // Dragging beyond the ends picks the first and last keys.
    CHECK (g.noteNearest (-50.0f, 500.0f) == 24);
    CHECK (g.noteNearest (9000.0f, -5.0f) == 96);
    // Notes outside the window have no rectangle.
    CHECK (g.keyRect (23).w == Approx (0.0f));

    // Windows: C1-C7 normally; the split key is always inside with an octave to spare where possible.
    CHECK (KeyboardGeometry::windowFor (60) == 24);
    CHECK (KeyboardGeometry::windowFor (36) == 24);
    CHECK (KeyboardGeometry::windowFor (35) == 12);
    CHECK (KeyboardGeometry::windowFor (0) == 0);
    CHECK (KeyboardGeometry::windowFor (84) == 24);
    CHECK (KeyboardGeometry::windowFor (85) == 36);
    for (int split = 0; split <= 127; ++split)
    {
        const int lo = KeyboardGeometry::windowFor (split);
        INFO ("split " << split);
        CHECK (lo % 12 == 0);
        CHECK (lo >= 0);
        CHECK (lo + KeyboardGeometry::kSpan <= 127);
        if (split >= 12 && split <= 108)
        {
            CHECK (split >= lo + 12);
            CHECK (split <= lo + KeyboardGeometry::kSpan - 12);
        }
        else if (split < 12)
            CHECK (lo == 0);
        else
            CHECK (lo == 48); // the highest window: C4 ... C10 would pass MIDI 127
    }
    CHECK (noteLabel (60) == "C4");
    CHECK (noteLabel (61) == "C#4");
    CHECK (noteLabel (24) == "C1");
}

TEST_CASE ("Keyboard ownership is the KeyRouter's rule in every mode and for every split point", "[ui][m7c][models]")
{
    KeyRouter router;
    for (int mode = 0; mode < 3; ++mode)
        for (int split : { 0, 1, 24, 59, 60, 61, 96, 127 })
        {
            router.reset();
            router.setMode (static_cast<KeyMode> (mode));
            router.setSplitPoint (split);
            for (int note = 0; note < 128; ++note)
            {
                INFO ("mode " << mode << " split " << split << " note " << note);
                REQUIRE (ownerMask (mode, split, note) == router.targetFor (note));
            }
            // The bars' spans cover exactly the keys of each part inside the window.
            for (int part = 0; part < 2; ++part)
            {
                const auto span = partSpan (part, mode, split, 24, 96);
                for (int note = 24; note <= 96; ++note)
                {
                    const bool owns = (ownerMask (mode, split, note) & (part == 0 ? 1 : 2)) != 0;
                    INFO ("part " << part << " note " << note);
                    REQUIRE ((! span.empty() && note >= span.first && note <= span.last) == owns);
                }
            }
        }
}

TEST_CASE ("Split key model: a press is one gesture, dragging moves the key, keys and wheel share a timed gesture", "[ui][m7c][models]")
{
    Rec r (60.0f / 127.0f);
    SplitKeyModel m (r);
    CHECK (m.note() == 60);
    m.press (72);
    CHECK (r.begins == 1);
    CHECK (SplitKeyModel::noteOf (r.v) == 72);
    m.drag (73);
    m.drag (500); // clamped to the top key
    CHECK (SplitKeyModel::noteOf (r.v) == 127);
    CHECK (m.dragNote() == 127);
    m.drag (-4);
    CHECK (SplitKeyModel::noteOf (r.v) == 0);
    CHECK (r.ends == 0);
    m.release();
    CHECK (r.begins == 1);
    CHECK (r.ends == 1);
    CHECK (r.outside == 0);
    m.drag (50); // after the release a stray drag writes nothing
    CHECK (SplitKeyModel::noteOf (r.v) == 0);

    r.v = SplitKeyModel::valueOf (60);
    m.nudge (1, 1000.0);
    m.nudge (12, 1100.0);
    m.nudge (-1, 1200.0);
    CHECK (SplitKeyModel::noteOf (r.v) == 72);
    CHECK (r.begins == 2); // one timed gesture for the three nudges
    m.idle (1400.0);
    CHECK (r.ends == 1); // 300 ms not yet
    m.idle (1600.0);
    CHECK (r.ends == 2);
    m.toEnd (true, 2000.0);
    CHECK (SplitKeyModel::noteOf (r.v) == 127);
    m.nudge (1, 2010.0);
    CHECK (SplitKeyModel::noteOf (r.v) == 127); // no wrap
    m.toEnd (false, 2020.0);
    CHECK (SplitKeyModel::noteOf (r.v) == 0);
    m.cancel();
    CHECK (r.begins == r.ends);
    m.wheel (0.4f, 3000.0);
    CHECK (SplitKeyModel::noteOf (r.v) == 0); // a fraction waits
    m.wheel (0.7f, 3010.0);
    CHECK (SplitKeyModel::noteOf (r.v) == 1);
    m.cancel();
    m.doubleClick();
    CHECK (SplitKeyModel::noteOf (r.v) == 60);
    CHECK (r.begins == r.ends);
    CHECK (r.nested == 0);
    CHECK (r.outside == 0);
}

TEST_CASE ("Delay picture equals the real delay: echo times, heights and lanes (normal and ping-pong)", "[ui][m7c][pictures]")
{
    constexpr double fs = 48000.0;
    for (const bool pingPong : { false, true })
        for (const auto [time, feedback] : { std::pair<float, float> { 0.05f, 0.5f }, { 0.12f, 0.8f }, { 0.31f, 0.2f } })
        {
            INFO ("pingPong " << pingPong << " time " << time << " feedback " << feedback);
            fx::Delay delay;
            delay.prepare (fs, 512);
            fx::DelayParams p;
            p.timeSeconds = time;
            p.feedback = feedback;
            p.damping = 0.0f; // flat repeats: only the picture's geometry is under test
            p.pingPong = pingPong;
            p.level = 1.0f;
            delay.setParams (p);
            const int n = static_cast<int> (fs * 2.0);
            std::vector<float> l (static_cast<size_t> (n), 0.0f), r (static_cast<size_t> (n), 0.0f);
            // Let the engine settle (it fades in) with silence, then an impulse in both channels.
            for (int block = 0; block < 4; ++block)
                delay.process (l.data(), r.data(), 512);
            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            l[0] = r[0] = 1.0f;
            delay.process (l.data(), r.data(), n);

            const auto pic = delayPicture (static_cast<double> (time), feedback, pingPong, 2.0);
            REQUIRE (pic.count >= 3);
            for (int i = 0; i < pic.count; ++i)
            {
                const auto& e = pic.echoes[static_cast<size_t> (i)];
                const int at = static_cast<int> (std::lround (e.timeSec * fs));
                // The engine's echo n: gain feedback^(n-1) (the first echo is 1), at n x time, within a sample.
                float lp = 0.0f, rp = 0.0f;
                for (int d = -2; d <= 2; ++d)
                {
                    lp = std::max (lp, std::abs (l[static_cast<size_t> (at + d)]));
                    rp = std::max (rp, std::abs (r[static_cast<size_t> (at + d)]));
                }
                INFO ("echo " << i + 1 << " at " << e.timeSec);
                if (e.lane == 2 || e.lane == 0)
                    CHECK (lp == Approx (e.height).margin (0.02));
                else
                    CHECK (lp < 0.02f);
                if (e.lane == 2 || e.lane == 1)
                    CHECK (rp == Approx (e.height).margin (0.02));
                else
                    CHECK (rp < 0.02f);
            }
        }
}

TEST_CASE ("Delay picture: echoes are cut at the axis end and when they are inaudible; the axes are monotonic and invertible", "[ui][m7c][pictures]")
{
    const auto pic = delayPicture (0.7, 0.35f, false, 6.0);
    CHECK (pic.count == 6); // 0.7 ... 5.6 s would fit, but the echo after 0.35^5 = 0.005 is below 1/256: the list ends
    for (int i = 1; i < pic.count; ++i)
    {
        CHECK (pic.echoes[static_cast<size_t> (i)].timeSec > pic.echoes[static_cast<size_t> (i) - 1].timeSec);
        CHECK (pic.echoes[static_cast<size_t> (i)].height < pic.echoes[static_cast<size_t> (i) - 1].height);
    }
    CHECK (delayPicture (2.0, 0.9f, false, 6.0).count == 3); // 2, 4, 6 s
    CHECK (delayPicture (0.001, 0.0f, true, 6.0).count == 1); // no feedback: one echo
    CHECK (delayPicture (0.5, 0.95f, true, 100.0).count == DelayPicture::kMaxEchoes);

    const auto axis = delayTimeAxis (10.0f, 300.0f);
    float last = -1.0f;
    for (double t : { 0.0, 0.001, 0.01, 0.1, 0.5, 1.0, 2.0, 4.0, 6.0 })
    {
        const float x = axis.positionOf (t);
        CHECK (x > last);
        last = x;
        CHECK (axis.valueAt (x) == Approx (t).margin (1.0e-6));
    }
    CHECK (axis.positionOf (6.0) == Approx (310.0f));
    CHECK (axis.positionOf (0.0) == Approx (10.0f));
    // A short delay's echoes stay apart (at least 8 px for 12 echoes of 50 ms), a long one's too (2 s: two echoes 100+ px apart).
    CHECK (axis.positionOf (0.1) - axis.positionOf (0.05) > 8.0f);
    CHECK (axis.positionOf (4.0) - axis.positionOf (2.0) > 40.0f);

    const ReverbTimeAxis rev (10.0f, 300.0f);
    last = -1.0f;
    for (double t : { 0.0, 0.02, 0.2, 1.0, 2.5, 20.2, 30.0 })
    {
        const float x = rev.positionOf (t);
        CHECK (x > last);
        last = x;
        CHECK (rev.timeAt (x) == Approx (t).margin (1.0e-5));
    }
    CHECK (rev.positionOf (0.0) == Approx (10.0f));
    CHECK (rev.positionOf (30.0) == Approx (310.0f));
}

TEST_CASE ("Chorus picture equals the real chorus: the tap delays follow the block's own LFO", "[ui][m7c][pictures]")
{
    constexpr double fs = 48000.0;
    for (const auto mode : { fx::ChorusMode::I, fx::ChorusMode::II })
    {
        fx::Chorus chorus;
        chorus.prepare (fs, 512);
        fx::ChorusParams p;
        p.mode = mode;
        p.mix = 0.5f;
        chorus.setParams (p);
        const auto pic = chorusPicture (mode == fx::ChorusMode::I ? 1 : 2);
        CHECK (pic.active());
        std::vector<float> l (512, 0.0f), r (512, 0.0f);
        const float msToSamples = static_cast<float> (fs / 1000.0);
        double worst = 0.0;
        for (int block = 0; block < 400; ++block) // 4.3 s: more than the picture's window
        {
            chorus.process (l.data(), r.data(), 512);
            const double sec = (static_cast<double> (block + 1) * 512.0 + 1.0) / fs; // the next sample's LFO step
            const double a = static_cast<double> (chorus.delaySamplesA()) / msToSamples;
            const double b = static_cast<double> (chorus.delaySamplesB()) / msToSamples;
            worst = std::max ({ worst, std::abs (a - static_cast<double> (chorusTapMs (pic, sec, false))),
                                std::abs (b - static_cast<double> (chorusTapMs (pic, sec, true))) });
        }
        INFO ("mode " << (mode == fx::ChorusMode::I ? "I" : "II"));
        CHECK (worst < 2.0e-3);
    }
    CHECK_FALSE (chorusPicture (0).active());
    CHECK (chorusTapMs (chorusPicture (0), 1.234, false) == Approx (fx::Chorus::kCentreMs));
}

TEST_CASE ("Reverb picture: pre-delay gap, then -60 dB at pre-delay + decay time", "[ui][m7c][pictures]")
{
    for (const float vPre : { 0.0f, 0.1f, 1.0f })
        for (const float vDecay : { 0.0f, 0.5f, 1.0f })
        {
            const float pre = map::reverbPreDelaySec (vPre), decay = map::reverbDecaySec (vDecay);
            const auto pic = reverbPicture (pre, decay);
            CHECK (pic.preDelaySec == Approx (static_cast<double> (pre)));
            CHECK (pic.endSec == Approx (static_cast<double> (pre) + static_cast<double> (decay)));
            CHECK (pic.endSec <= ReverbTimeAxis::kEndSec); // the longest tail (0.2 s + 20 s) fits the axis
        }
}

TEST_CASE ("Power switch model: off remembers the level, on restores it (0.35 if none)", "[ui][m7c][models]")
{
    float stored = 0.35f;
    CHECK (PowerSwitchModel::isOn (0.01f));
    CHECK_FALSE (PowerSwitchModel::isOn (0.0f));
    CHECK (PowerSwitchModel::press (0.6f, stored) == Approx (0.0f));
    CHECK (stored == Approx (0.6f));
    CHECK (PowerSwitchModel::press (0.0f, stored) == Approx (0.6f));
    stored = 0.0f;
    CHECK (PowerSwitchModel::press (0.0f, stored) == Approx (0.35f));
    CHECK (stored == Approx (0.0f)); // switching on stores nothing
}

// =================================================================================================
// The Play page
// =================================================================================================

TEST_CASE ("Play page: 20 primary controls, the strip is the split key's primary, every part control binds its own part", "[ui][m7c][play]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& play = rig.editor->getPlayPage();
    const auto* e = primaryOf (rig.editor->getRegistry(), "key.split_point");
    REQUIRE (e != nullptr);
    CHECK (e->page == "play");
    CHECK (e->control == "KeyboardStrip");
    CHECK (e->view == &play.getStrip());
    CHECK (play.getStrip().getTitle() == "Split key");
    CHECK (! play.getStrip().getDescription().isEmpty());
    for (int part = 0; part < 2; ++part)
    {
        const juce::String prefix = part == 0 ? "lower." : "upper.";
        auto& pc = play.getPart (part);
        CHECK (pc.voices->getComponentID() == prefix + "assign.mode");
        CHECK (pc.unisonVoices->getComponentID() == prefix + "assign.unison_voices");
        CHECK (pc.glideMode->getCaptions().size() == 2);
        CHECK (pc.glideMode->getCaptions()[1] == "Legato only");
        CHECK (pc.pan->getTitle().startsWith (part == 0 ? "Lower" : "Upper"));
    }
    CHECK (play.getModeSwitch().getCaptions().joinIntoString ("/") == "Whole/Dual/Split");
}

TEST_CASE ("Play page: the inactive rules of 9.3.9 follow the parameters", "[ui][m7c][play]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& play = rig.editor->getPlayPage();
    rig.set ("key.mode", 0.0f);
    rig.set ("lower.assign.mode", 0.0f);
    rig.set ("upper.assign.mode", 1.0f);
    rig.set ("lower.porta.time", 0.0f);
    rig.set ("upper.porta.time", 0.5f);
    rig.tick();
    CHECK (play.getStrip().isInactive()); // Whole: the split key has no effect
    CHECK (play.getBalance().isInactive());
    CHECK (play.getPart (0).unisonVoices->isInactive());
    CHECK (play.getPart (0).detune->isInactive());
    CHECK_FALSE (play.getPart (1).unisonVoices->isInactive());
    CHECK_FALSE (play.getPart (1).detune->isInactive());
    CHECK (play.getPart (0).glideMode->isInactive());
    CHECK_FALSE (play.getPart (1).glideMode->isInactive());
    CHECK (play.getStrip().getDescription().endsWith ("(no effect in this mode)"));
    CHECK (play.modeSentence().startsWith ("Whole"));

    hostWrite (rig.param ("key.mode"), 0.5f); // Dual
    rig.tick();
    CHECK (play.getStrip().isInactive());
    CHECK_FALSE (play.getBalance().isInactive());
    CHECK (play.modeSentence().startsWith ("Dual"));

    hostWrite (rig.param ("key.mode"), 1.0f); // Split
    rig.tick();
    CHECK_FALSE (play.getStrip().isInactive());
    CHECK_FALSE (play.getBalance().isInactive());
    CHECK (play.modeSentence().startsWith ("Split"));
    CHECK_FALSE (play.getStrip().getDescription().contains ("no effect"));

    rig.set ("lower.assign.mode", 1.0f);
    rig.set ("upper.porta.time", 0.0f);
    rig.tick();
    CHECK_FALSE (play.getPart (0).unisonVoices->isInactive());
    CHECK (play.getPart (1).glideMode->isInactive());
    // Display only: an inactive control still takes its value.
    play.getPart (1).glideMode->getModel().press (1);
    play.getPart (1).glideMode->getModel().release();
    CHECK (rig.param ("upper.porta.mode").getValue() == Approx (1.0f));
}

TEST_CASE ("Keyboard strip: held keys, sounding notes and the part colours come from the feed and the key mode", "[ui][m7c][play]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& strip = rig.editor->getPlayPage().getStrip();
    rig.set ("key.mode", 2.0f);
    rig.set ("key.split_point", 60.0f);
    rig.tick();
    CHECK (strip.drawnKeyMode() == 2);
    CHECK (strip.drawnSplitNote() == 60);
    CHECK (strip.barText (0) == "LOWER C1 - B3");
    CHECK (strip.barText (1) == "UPPER C4 - C7");

    // Hold C3 (Lower side) and G4 (Upper side).
    rig.render (rig.samples (0.05), { noteOn (static_cast<int> (rig.now), 48), noteOn (static_cast<int> (rig.now), 67) }, 128);
    rig.tick();
    CHECK (strip.drawsHeld (0, 48));
    CHECK (strip.drawsHeld (1, 67));
    CHECK_FALSE (strip.drawsHeld (1, 48));
    CHECK_FALSE (strip.drawsHeld (0, 67));
    CHECK (strip.drawsSounding (0, 48));
    CHECK (strip.drawsSounding (1, 67));
    const int repaints = strip.repaintCount();
    rig.tick();
    rig.tick();
    CHECK (strip.repaintCount() == repaints); // nothing repaints at rest

    // Release C3: the key is no longer held; the note keeps sounding through its release.
    rig.render (rig.samples (0.01), { noteOff (static_cast<int> (rig.now), 48) }, 128);
    rig.tick();
    CHECK_FALSE (strip.drawsHeld (0, 48));
    CHECK (strip.repaintCount() > repaints);

    // Dual: both parts play a held key.
    rig.set ("key.mode", 1.0f);
    rig.render (rig.samples (0.02), { noteOn (static_cast<int> (rig.now), 72) }, 128);
    rig.tick();
    CHECK (strip.drawnKeyMode() == 1);
    CHECK (strip.drawsHeld (0, 72));
    CHECK (strip.drawsHeld (1, 72));
    CHECK (strip.barText (0) == "LOWER C1 - C7");
    CHECK (strip.barText (1) == "UPPER C1 - C7");
    rig.set ("key.mode", 0.0f);
    rig.tick();
    CHECK (strip.barText (1).contains ("not played"));
    CHECK (strip.barText (0) == "LOWER C1 - C7");
}

TEST_CASE ("Keyboard strip: the window follows the split key by octaves; its own drag never moves the keys under the pointer", "[ui][m7c][play]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& strip = rig.editor->getPlayPage().getStrip();
    rig.set ("key.mode", 2.0f);
    rig.tick();
    CHECK (strip.windowLow() == 24);
    hostWrite (rig.param ("key.split_point"), 20.0f / 127.0f);
    rig.tick();
    CHECK (strip.windowLow() == 0);
    CHECK (strip.getGeometry().keyRect (20).w > 0.0f);
    hostWrite (rig.param ("key.split_point"), 110.0f / 127.0f);
    rig.tick();
    CHECK (strip.windowLow() == 48);
    hostWrite (rig.param ("key.split_point"), 60.0f / 127.0f);
    rig.tick();
    CHECK (strip.windowLow() == 24);

    // Drag the marker to the last key of the window and let go: the window stays.
    const auto down = mouseAt (strip, keyCentre (strip, 60));
    strip.mouseDown (down);
    const auto edge = mouseAt (strip, keyCentre (strip, 96));
    strip.mouseDrag (edge);
    strip.mouseUp (edge);
    rig.tick();
    CHECK (plain (rig.param ("key.split_point")) == Approx (96.0f));
    CHECK (strip.windowLow() == 24); // 96 is within an octave of the edge: the windowFor rule alone would shift it
    // A change from elsewhere (automation) moves it again.
    hostWrite (rig.param ("key.split_point"), 97.0f / 127.0f);
    rig.tick();
    CHECK (strip.windowLow() == 48);
}

TEST_CASE ("Keyboard strip: press and drag on the keys set the split key in one host gesture; keys and double-click work", "[ui][m7c][play][gestures]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    GestureCounter host (rig.proc);
    auto& strip = rig.editor->getPlayPage().getStrip();
    rig.set ("key.mode", 2.0f);
    rig.tick();
    const int beginsBefore = host.begins, outsideBefore = host.valuesOutsideGesture;
    strip.mouseDown (mouseAt (strip, keyCentre (strip, 62)));
    CHECK (plain (rig.param ("key.split_point")) == Approx (62.0f));
    CHECK (strip.drawnSplitNote() == 62); // shown at once, not at the next tick
    strip.mouseDrag (mouseAt (strip, keyCentre (strip, 66)));
    strip.mouseDrag (mouseAt (strip, keyCentre (strip, 66))); // a black key (F#)
    CHECK (plain (rig.param ("key.split_point")) == Approx (66.0f));
    CHECK (host.openCount() == 1);
    strip.mouseUp (mouseAt (strip, keyCentre (strip, 66)));
    CHECK (host.openCount() == 0);
    CHECK (host.begins == beginsBefore + 1);
    CHECK (host.valuesOutsideGesture == outsideBefore);
    rig.tick();
    CHECK (rig.param ("key.split_point").getCurrentValueAsText() == "F#4");
    CHECK (strip.shownText() == "F#4");

    // Right-click opens the menu and never drags.
    strip.mouseDown (mouseAt (strip, keyCentre (strip, 80), true));
    juce::PopupMenu::dismissAllActiveMenus();
    CHECK (plain (rig.param ("key.split_point")) == Approx (66.0f));

    // Keys (focused): arrows one key, Page keys an octave, Home/End the ends; a timed gesture.
    strip.keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
    strip.keyPressed (juce::KeyPress (juce::KeyPress::pageUpKey));
    CHECK (plain (rig.param ("key.split_point")) == Approx (79.0f));
    strip.keyPressed (juce::KeyPress (juce::KeyPress::homeKey));
    CHECK (plain (rig.param ("key.split_point")) == Approx (0.0f));
    CHECK (host.openCount() == 1); // one timed gesture for the arrow, the Page key and Home
    strip.getModel().idle (juce::Time::getMillisecondCounterHiRes() + 400.0);
    CHECK (host.openCount() == 0);
    strip.mouseDoubleClick (mouseAt (strip, keyCentre (strip, 60)));
    CHECK (plain (rig.param ("key.split_point")) == Approx (60.0f));
    CHECK (host.nestedBegins == 0);
    CHECK (host.valuesOutsideGesture == outsideBefore);

    // The editor closing mid-drag ends the gesture.
    strip.mouseDown (mouseAt (strip, keyCentre (strip, 70)));
    CHECK (host.openCount() == 1);
    rig.editor.reset();
    CHECK (host.openCount() == 0);
}

TEST_CASE ("Keyboard strip: screen readers read the note name and can set it", "[ui][m7c][play]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& strip = rig.editor->getPlayPage().getStrip();
    auto owned = strip.createAccessibilityHandler();
    auto* handler = owned.get();
    REQUIRE (handler != nullptr);
    CHECK (handler->getRole() == juce::AccessibilityRole::slider);
    auto* value = handler->getValueInterface();
    REQUIRE (value != nullptr);
    CHECK (value->getCurrentValueAsString() == "C4");
    value->setValueAsString ("D5");
    CHECK (plain (rig.param ("key.split_point")) == Approx (74.0f));
    rig.tick();
    CHECK (value->getCurrentValueAsString() == "D5");
    value->setValueAsString ("nonsense"); // unreadable: nothing changes
    CHECK (plain (rig.param ("key.split_point")) == Approx (74.0f));
}

// =================================================================================================
// The Effects page
// =================================================================================================

TEST_CASE ("Effects page: 11 primary controls; handles and power switches are secondary bindings on the same page", "[ui][m7c][effects]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& fx = rig.editor->getEffectsPage();
    int primaries = 0, handles = 0, powers = 0;
    for (const auto& e : rig.editor->getRegistry().entries())
        if (e.page == "effects")
        {
            CHECK (e.part == -1);
            if (e.placement == ui::Placement::Primary)
                ++primaries;
            else if (e.control == "GraphHandle")
                ++handles;
            else if (e.control == "PowerSwitch")
                ++powers;
        }
    CHECK (primaries == 11);
    CHECK (handles == 4); // delay time + repeats, reverb pre-delay, decay time
    CHECK (powers == 2);
    CHECK (fx.getChorusMode().getCaptions().joinIntoString ("/") == "Off/I/II");
    CHECK (fx.getPingPong().getCaptions().joinIntoString ("/") == "Off/On");
    CHECK (fx.getDelayPower().getDescription().startsWith ("Switches this effect on or off"));
    CHECK (fx.getDelayHandle().getTitle() == "Delay echoes");
    CHECK (fx.getPreDelayHandle().getTitle() == "Reverb pre-delay");
    CHECK (fx.getDecayHandle().getTitle() == "Reverb decay time");
}

TEST_CASE ("Effects page: the pictures are recomputed from the parameters' own mappings, once per tick, only when an input changed", "[ui][m7c][effects]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& fx = rig.editor->getEffectsPage();
    rig.editor->selectPage (5, 0);
    rig.tick();

    const auto expectPictures = [&] (const juce::String& when) {
        INFO (when);
        const auto d = fx.expectedDelay();
        REQUIRE (fx.getDelayView().picture().count == d.count);
        for (int i = 0; i < d.count; ++i)
        {
            REQUIRE (fx.getDelayView().picture().echoes[static_cast<size_t> (i)].timeSec == Approx (d.echoes[static_cast<size_t> (i)].timeSec));
            REQUIRE (fx.getDelayView().picture().echoes[static_cast<size_t> (i)].height == Approx (d.echoes[static_cast<size_t> (i)].height));
            REQUIRE (fx.getDelayView().picture().echoes[static_cast<size_t> (i)].lane == d.echoes[static_cast<size_t> (i)].lane);
        }
        const auto rv = fx.expectedReverb();
        REQUIRE (fx.getReverbView().picture().preDelaySec == Approx (rv.preDelaySec));
        REQUIRE (fx.getReverbView().picture().endSec == Approx (rv.endSec));
        const int mode = static_cast<int> (std::lround (plain (rig.param ("fx.chorus.mode"))));
        REQUIRE (fx.getChorusView().drawnMode() == mode);
        REQUIRE (fx.getDelayView().isOn() == (rig.param ("fx.delay.level").getValue() > 0.0f));
        REQUIRE (fx.getReverbView().isOn() == (rig.param ("fx.reverb.level").getValue() > 0.0f));
    };
    expectPictures ("initial");

    std::mt19937 rng (11);
    std::uniform_real_distribution<float> uni (0.0f, 1.0f);
    for (const auto* id : { "fx.delay.time", "fx.delay.feedback", "fx.delay.ping_pong", "fx.delay.level", "fx.reverb.predelay", "fx.reverb.decay",
                            "fx.reverb.level", "fx.chorus.mode", "fx.chorus.mix" })
    {
        auto& p = rig.param (id);
        for (float v : { 0.0f, p.getDefaultValue(), uni (rng), 1.0f })
        {
            hostWrite (p, v);
            rig.tick();
            expectPictures (juce::String (id) + " = " + juce::String (v));
        }
    }

    // One recompute per tick however many inputs changed; none when only unrelated parameters change.
    int before = fx.pictureComputations();
    hostWrite (rig.param ("fx.delay.time"), 0.3f);
    hostWrite (rig.param ("fx.delay.feedback"), 0.6f);
    hostWrite (rig.param ("fx.reverb.decay"), 0.7f);
    hostWrite (rig.param ("fx.chorus.mode"), 1.0f);
    rig.tick();
    CHECK (fx.pictureComputations() == before + 1);
    before = fx.pictureComputations();
    hostWrite (rig.param ("fx.delay.damping"), 0.9f);
    hostWrite (rig.param ("fx.reverb.damping"), 0.1f);
    hostWrite (rig.param ("lower.vcf.cutoff"), 0.4f);
    rig.tick();
    rig.tick();
    CHECK (fx.pictureComputations() == before);

    // Programs, Init and a session restore redraw the pictures too.
    auto& pm = rig.proc.getPresets();
    for (int program : { 3, 27, 55 })
    {
        pm.selectProgram (program);
        rig.tick();
        expectPictures ("program " + juce::String (program));
    }
    pm.initialise();
    rig.tick();
    expectPictures ("Init");
}

TEST_CASE ("Effects page: delay and reverb controls dim while the level is 0; Level and the power switch never do", "[ui][m7c][effects]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& fx = rig.editor->getEffectsPage();
    rig.set ("fx.delay.level", 0.0f);
    rig.set ("fx.reverb.level", 0.4f);
    rig.set ("fx.chorus.mode", 0.0f);
    rig.tick();
    CHECK (fx.getSlider ("fx.chorus.mix").isInactive());
    for (const char* id : { "fx.delay.time", "fx.delay.feedback", "fx.delay.damping" })
        CHECK (fx.getSlider (id).isInactive());
    CHECK (fx.getPingPong().isInactive());
    CHECK_FALSE (fx.getSlider ("fx.delay.level").isInactive());
    CHECK_FALSE (fx.getDelayPower().isInactive());
    for (const char* id : { "fx.reverb.predelay", "fx.reverb.decay", "fx.reverb.damping", "fx.reverb.level" })
        CHECK_FALSE (fx.getSlider (id).isInactive());
    CHECK (fx.delayHeader().contains ("OFF"));
    CHECK_FALSE (fx.reverbHeader().contains ("OFF"));
    CHECK_FALSE (fx.getDelayView().isOn());
    CHECK (fx.getReverbView().isOn());

    rig.set ("fx.delay.level", 0.3f);
    rig.set ("fx.reverb.level", 0.0f);
    rig.set ("fx.chorus.mode", 2.0f);
    rig.tick();
    CHECK_FALSE (fx.getSlider ("fx.chorus.mix").isInactive());
    CHECK_FALSE (fx.getSlider ("fx.delay.time").isInactive());
    CHECK_FALSE (fx.getPingPong().isInactive());
    CHECK (fx.getSlider ("fx.reverb.decay").isInactive());
    CHECK_FALSE (fx.delayHeader().contains ("OFF"));
    CHECK (fx.reverbHeader().contains ("OFF"));
    CHECK (fx.getDelayView().isOn());
    CHECK_FALSE (fx.getReverbView().isOn());
    // Display only: a dimmed slider still takes a value.
    fx.getSlider ("fx.reverb.decay").getModel().typed (0.25f);
    CHECK (rig.param ("fx.reverb.decay").getValue() == Approx (0.25f));
}

TEST_CASE ("Power switch: off stores the level and writes 0, on writes it back (0.35 if none); one gesture per press; session state", "[ui][m7c][effects][gestures]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    GestureCounter host (rig.proc);
    auto& fx = rig.editor->getEffectsPage();
    auto& state = rig.proc.getEditorState();
    auto& level = rig.param ("fx.delay.level");
    auto& power = fx.getDelayPower();

    rig.tick();
    CHECK_FALSE (power.isOn());
    CHECK (level.getValue() == Approx (0.0f));
    int outside = host.valuesOutsideGesture; // host writes of the test itself are outside any gesture; the switch's are not
    power.press(); // off at 0, nothing stored: the default
    CHECK (level.getValue() == Approx (0.35f));
    CHECK (host.begins == 1);
    CHECK (host.ends == 1);
    rig.tick();
    CHECK (power.isOn());

    hostWrite (level, 0.62f);
    rig.tick();
    power.press();
    CHECK (level.getValue() == Approx (0.0f));
    CHECK (state.fxLastLevel[0].load() == Approx (0.62f));
    CHECK (state.fxLastLevel[1].load() == Approx (0.35f)); // the reverb's slot is untouched
    rig.tick();
    CHECK_FALSE (power.isOn());
    CHECK (fx.delayHeader().contains ("OFF"));
    power.press();
    CHECK (level.getValue() == Approx (0.62f));
    rig.tick();
    CHECK (power.isOn());
    CHECK_FALSE (fx.delayHeader().contains ("OFF"));
    CHECK (host.valuesOutsideGesture == outside + 1); // only the test's own host write of 0.62
    CHECK (host.nestedBegins == 0);
    CHECK (host.openCount() == 0);

    // The switch acts on what the parameter holds now, even before the next tick.
    hostWrite (level, 0.0f);
    power.press();
    CHECK (level.getValue() == Approx (0.62f)); // 0.0 was written by the host, never stored: the last stored level
    // Reverb has its own slot; the session text keeps both.
    auto& rlevel = rig.param ("fx.reverb.level");
    hostWrite (rlevel, 0.8f);
    rig.tick();
    fx.getReverbPower().press();
    CHECK (rlevel.getValue() == Approx (0.0f));
    CHECK (state.fxLastLevel[1].load() == Approx (0.8f));
    // Keyboard: Space and Return press it; an unrelated key does not.
    CHECK (power.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)));
    CHECK (level.getValue() == Approx (0.0f));
    CHECK_FALSE (power.keyPressed (juce::KeyPress ('x')));
    auto owned = power.createAccessibilityHandler();
    auto* handler = owned.get();
    REQUIRE (handler != nullptr);
    CHECK (handler->getRole() == juce::AccessibilityRole::toggleButton);
    rig.tick();
    CHECK_FALSE (handler->getCurrentState().isChecked());
    power.press();
    rig.tick();
    CHECK (handler->getCurrentState().isChecked());
}

TEST_CASE ("Effects handles: the delay handle writes time and repeats, the reverb handles pre-delay and decay; gestures balanced", "[ui][m7c][effects][gestures]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    GestureCounter host (rig.proc);
    auto& fx = rig.editor->getEffectsPage();
    rig.set ("fx.delay.level", 0.4f);
    rig.set ("fx.reverb.level", 0.4f);
    rig.editor->selectPage (5, 0);
    rig.tick();
    const int outsideBefore = host.valuesOutsideGesture; // the test's own host writes above

    // Delay handle: x moves the time (the second echo at twice the time), y the repeats (relative).
    auto& dh = fx.getDelayHandle();
    auto& dm = dh.getModel();
    const auto c = dh.getCentre();
    dm.mouseDown (c.x, c.y, c.x, false);
    dm.mouseDrag (c.x + 30.0f, c.y - 20.0f, false);
    const auto& dview = fx.getDelayView();
    const float expectedTime = map::delayTimeFromSec (static_cast<float> (dview.timeAtHandleX (c.x + 30.0f)));
    const float expectedFb = rig.param ("fx.delay.feedback").getDefaultValue() + 20.0f / (dview.laneHeight() / map::kDelayMaxFeedback);
    CHECK (rig.param ("fx.delay.time").getValue() == Approx (expectedTime).margin (1.0e-4));
    CHECK (rig.param ("fx.delay.feedback").getValue() == Approx (expectedFb).margin (1.0e-4));
    CHECK (rig.param ("fx.delay.time").getValue() > rig.param ("fx.delay.time").getDefaultValue());
    CHECK (host.openCount() == 2);
    dm.mouseUp();
    CHECK (host.openCount() == 0);
    rig.tick();
    CHECK (handleFollows (fx.getDelayHandle()));
    // The handle stands where the second echo's top is: the picture and the handle agree.
    const auto pic = fx.getDelayView().picture();
    CHECK (fx.getDelayHandle().getCentre().x == Approx (fx.getDelayView().timeAxis().positionOf (pic.echoes[1].timeSec)).margin (0.01));
    CHECK (fx.getDelayHandle().getCentre().y
           == Approx (fx.getDelayView().handleAt (static_cast<double> (map::delayTimeSec (rig.param ("fx.delay.time").getValue())),
                                                  pic.echoes[1].height, rig.param ("fx.delay.ping_pong").getValue() > 0.5f).y).margin (0.01));

    // Reverb: pre-delay handle (x only), then the decay handle (x only; the tail's end = pre-delay + decay).
    auto& ph = fx.getPreDelayHandle();
    auto& pm = ph.getModel();
    const auto pc = ph.getCentre();
    pm.mouseDown (pc.x, pc.y, pc.x, false);
    pm.mouseDrag (pc.x + 40.0f, pc.y + 90.0f, false); // y does nothing
    pm.mouseUp();
    const float expectedPre = map::reverbPreDelayFromSec (static_cast<float> (fx.getReverbView().timeAxis().timeAt (pc.x + 40.0f)));
    CHECK (rig.param ("fx.reverb.predelay").getValue() == Approx (expectedPre).margin (1.0e-4));
    rig.tick();
    auto& eh = fx.getDecayHandle();
    auto& em = eh.getModel();
    const auto ec = eh.getCentre();
    em.mouseDown (ec.x, ec.y, ec.x, false);
    em.mouseDrag (ec.x - 25.0f, ec.y, false);
    em.mouseUp();
    const double pre = static_cast<double> (map::reverbPreDelaySec (rig.param ("fx.reverb.predelay").getValue()));
    const float expectedDecay = map::reverbDecayFromSec (static_cast<float> (fx.getReverbView().timeAxis().timeAt (ec.x - 25.0f) - pre));
    CHECK (rig.param ("fx.reverb.decay").getValue() == Approx (expectedDecay).margin (1.0e-4));
    CHECK (rig.param ("fx.reverb.decay").getValue() < rig.param ("fx.reverb.decay").getDefaultValue());
    rig.tick();
    CHECK (handleFollows (fx.getPreDelayHandle()));
    CHECK (handleFollows (fx.getDecayHandle()));
    CHECK (host.openCount() == 0);
    CHECK (host.valuesOutsideGesture == outsideBefore);
    CHECK (host.nestedBegins == 0);

    // Double-click resets what the handle writes.
    dm.doubleClick();
    CHECK (rig.param ("fx.delay.time").getValue() == Approx (rig.param ("fx.delay.time").getDefaultValue()));
    CHECK (rig.param ("fx.delay.feedback").getValue() == Approx (rig.param ("fx.delay.feedback").getDefaultValue()));
    // The editor closing mid-drag ends the gesture.
    const auto c2 = fx.getDelayHandle().getCentre();
    fx.getDelayHandle().getModel().mouseDown (c2.x, c2.y, c2.x, false);
    fx.getDelayHandle().getModel().mouseDrag (c2.x + 10.0f, c2.y + 10.0f, false);
    CHECK (host.openCount() >= 1);
    rig.editor.reset();
    CHECK (host.openCount() == 0);
}

TEST_CASE ("Effects page: a handle is reachable at both ends of its range, the pictures stay inside their plots", "[ui][m7c][effects]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& fx = rig.editor->getEffectsPage();
    rig.editor->selectPage (5, 0);
    for (const float t : { 0.0f, 1.0f })
        for (const float fb : { 0.0f, 1.0f })
            for (const float pre : { 0.0f, 1.0f })
                for (const float decay : { 0.0f, 1.0f })
                {
                    hostWrite (rig.param ("fx.delay.time"), t);
                    hostWrite (rig.param ("fx.delay.feedback"), fb);
                    hostWrite (rig.param ("fx.reverb.predelay"), pre);
                    hostWrite (rig.param ("fx.reverb.decay"), decay);
                    rig.tick();
                    const auto dp = fx.getDelayView().plotArea();
                    const auto dc = fx.getDelayHandle().getCentre();
                    CHECK (dc.x >= dp.getX() - 0.5f);
                    CHECK (dc.x <= dp.getRight() + 0.5f);
                    CHECK (dc.y >= dp.getY() - 0.5f);
                    CHECK (dc.y <= dp.getBottom() + 0.5f);
                    const auto rp = fx.getReverbView().plotArea();
                    for (auto* h : { &fx.getPreDelayHandle(), &fx.getDecayHandle() })
                    {
                        CHECK (h->getCentre().x >= rp.getX() - 0.5f);
                        CHECK (h->getCentre().x <= rp.getRight() + 0.5f);
                        CHECK (h->getCentre().y >= rp.getY() - 0.5f);
                        CHECK (h->getCentre().y <= rp.getBottom() + 0.5f);
                    }
                    CHECK (handleFollows (fx.getDelayHandle()));
                    CHECK (handleFollows (fx.getDecayHandle()));
                }
}

// =================================================================================================
// Layout snapshots, paint allocations
// =================================================================================================

TEST_CASE ("Layout snapshot: the Play and Effects pages' component bounds equal the golden files", "[ui][m7c][layout]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    const bool update = juce::SystemStats::getEnvironmentVariable ("VS_UPDATE_UI_GOLDENS", {}) == "1";
    for (const auto& [page, name] : { std::pair<int, const char*> { 3, "play" }, { 5, "effects" } })
        for (int scale : { 100, 150 })
        {
            rig.editor->setSizePercent (scale);
            rig.editor->selectPage (page, 0);
            rig.tick();
            juce::String dump;
            dump << "editor " << rig.editor->getBounds().toString() << " scale " << juce::String (rig.editor->getScale(), 4) << "\n";
            dumpTree (rig.editor->getFrame(), 0, dump);
            const auto file = juce::File (VS_UI_GOLDEN_DIR).getChildFile (juce::String (name) + "_" + juce::String (scale) + ".txt");
            if (update)
            {
                REQUIRE (file.replaceWithText (dump, false, false, "\n"));
                continue;
            }
            INFO (file.getFileName() << " (VS_UPDATE_UI_GOLDENS=1 rewrites it after an intended layout change)");
            REQUIRE (file.existsAsFile());
            CHECK (file.loadFileAsString().removeCharacters ("\r") == dump);
        }
}

#if VS_SEQ_COUNT_ALLOCATIONS
namespace
{
int countComponents (const juce::Component& c)
{
    int n = 1;
    for (auto* child : c.getChildren())
        if (child->isVisible())
            n += countComponents (*child);
    return n;
}

/** Allocations of one paint of `page` into an image that already exists. */
int paintAllocations (juce::Component& page, juce::Image& image)
{
    juce::LowLevelGraphicsSoftwareRenderer renderer (image);
    juce::Graphics g (renderer);
    vs::seq::test::AllocationCounter counter;
    page.paintEntireComponent (g, true);
    return counter.count();
}
} // namespace

TEST_CASE ("Paint allocations: repeated paints of the Play and Effects pages allocate the same number of times, no more per component than the Sound page", "[ui][m7c][alloc]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.set ("key.mode", 2.0f);
    rig.set ("fx.delay.level", 0.4f);
    rig.set ("fx.reverb.level", 0.4f);
    rig.set ("fx.chorus.mode", 1.0f);
    rig.render (rig.samples (0.05), { noteOn (static_cast<int> (rig.now), 50), noteOn (static_cast<int> (rig.now), 70) }, 128);
    // The renderer allocates inside its own drawing (edge tables per shape), so "at most 8 per
    // component" (9.5) cannot be measured on a software renderer (the Sound page itself makes
    // ~260 per component). What our code must not do is allocate more when painting again, or
    // far more than the reference page: strings, paths and caches are prepared on value change.
    double soundPerComponent = 0.0;
    for (const auto page : { 0, 3, 5 })
    {
        rig.editor->selectPage (page, 0);
        rig.tick();
        auto* comp = rig.editor->getFrame().currentPageComponent();
        REQUIRE (comp != nullptr);
        juce::Image image (juce::Image::ARGB, comp->getWidth(), comp->getHeight(), true);
        paintAllocations (*comp, image); // warm-up: fonts, glyph caches
        const int a = paintAllocations (*comp, image);
        const int b = paintAllocations (*comp, image);
        const int c = paintAllocations (*comp, image);
        const double perComponent = static_cast<double> (a) / static_cast<double> (countComponents (*comp));
        INFO ("page " << page << ": " << a << ", " << b << ", " << c << " allocations, " << perComponent << " per component");
        CHECK (a == b);
        CHECK (b == c);
        if (page == 0)
            soundPerComponent = perComponent;
        else
            CHECK (perComponent <= 1.5 * soundPerComponent);
    }
}
#endif

// =================================================================================================
// Hidden: pictures of both pages in a lively state for review
// =================================================================================================

TEST_CASE ("UI shots M7c: Play (Split with keys held) and Effects (everything on) at 100 % and 200 %", "[.uishots]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VS_UISHOT_DIR", {});
    if (dir.isEmpty())
        SKIP ("VS_UISHOT_DIR not set");
    const juce::File out (dir);
    REQUIRE (out.createDirectory());

    EditorRig rig;
    rig.set ("key.mode", 2.0f);
    rig.set ("key.split_point", 60.0f);
    rig.set ("mix.balance", 0.3f);
    rig.set ("global.vintage", 0.4f);
    rig.set ("upper.assign.mode", 1.0f);
    rig.set ("upper.assign.unison_voices", 2.0f);
    rig.set ("lower.porta.time", 0.3f);
    rig.set ("fx.chorus.mode", 1.0f);
    rig.set ("fx.delay.level", 0.45f);
    rig.set ("fx.delay.time", 0.35f);
    rig.set ("fx.reverb.level", 0.35f);
    rig.render (rig.samples (0.08), { noteOn (static_cast<int> (rig.now), 48), noteOn (static_cast<int> (rig.now), 52), noteOn (static_cast<int> (rig.now), 55),
                                      noteOn (static_cast<int> (rig.now), 67), noteOn (static_cast<int> (rig.now), 72) }, 128);
    rig.render (rig.samples (0.02), { noteOff (static_cast<int> (rig.now), 52) }, 128);
    for (int scale : { 100, 200 })
    {
        rig.editor->setSizePercent (scale);
        for (const auto& [page, name] : { std::pair<int, const char*> { 3, "play" }, { 5, "effects" } })
        {
            rig.editor->selectPage (page, 0);
            rig.tick();
            const auto image = rig.editor->createComponentSnapshot (rig.editor->getLocalBounds(), true, 1.0f);
            const auto file = out.getChildFile (juce::String ("m7c_") + name + "_" + juce::String (scale) + ".png");
            file.deleteFile();
            juce::FileOutputStream stream (file);
            REQUIRE (stream.openedOk());
            REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
        }
    }
    // The Effects page with everything off, as a new program opens it.
    rig.proc.getPresets().initialise();
    rig.editor->setSizePercent (100);
    rig.editor->selectPage (5, 0);
    rig.tick();
    const auto image = rig.editor->createComponentSnapshot (rig.editor->getLocalBounds(), true, 1.0f);
    const auto file = out.getChildFile ("m7c_effects_off_100.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (stream.openedOk());
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}
