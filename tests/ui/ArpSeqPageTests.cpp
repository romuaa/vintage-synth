// The Arp + Seq page (docs/architecture.md 9.3.5, 9.4, 9.6; milestone M7d):
//   models      the step grid (mappings, painting across columns, gestures, length flag, R / T),
//               the feed readers (alive, playing step, held keys)
//   preview     the preview equals what the PROCESSOR delivers to the parts for the same chord and
//               settings (rendered offline through the real processor), for the model alone and for
//               the page (example chord routed by the key mode, and the player's keys from the feed)
//   feed        seqStep equals the sequencer's step and the pitch it played; the held-key order
//   page        painting steps through the real components (gestures balanced per parameter),
//               bars follow automation, the grid highlights the playing step, only changed inputs
//               recompute the preview, layout goldens, paint allocations, hidden screenshots.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EditorRig.h"
#include "NullGraphicsContext.h"
#include "common/ui/models/ArpFeedModels.h"
#include "common/ui/models/ArpPreviewModel.h"
#include "common/ui/models/StepGridModel.h"
#include "vintage/ui/ArpSeqPage.h"

#include <array>
#include <random>
#include <thread>

using namespace vs;
using namespace vs::itest;
using namespace vs::ui;
using namespace vs::ui::model;
using Catch::Approx;

namespace
{
// ---- A recording target for the model tests (same rules as ModelTests.cpp) ----
struct Rec final : ParamTarget
{
    Rec() = default;
    void init (int stepsIn, float def)
    {
        steps = stepsIn;
        defValue = def;
        v = def;
    }
    float value() const noexcept override { return v; }
    float defaultValue() const noexcept override { return defValue; }
    int numSteps() const noexcept override { return steps; }
    void beginGesture() override
    {
        if (open)
            ++nested;
        open = true;
        ++begins;
    }
    void setValue (float x) override
    {
        if (! open)
            ++outside;
        v = x;
        ++values;
    }
    void endGesture() override
    {
        if (! open)
            ++unmatched;
        open = false;
        ++ends;
    }
    bool clean() const { return ! open && nested == 0 && outside == 0 && unmatched == 0 && begins == ends; }
    float v = 0.5f;
    int steps = 0;
    float defValue = 0.5f;
    bool open = false;
    int begins = 0, ends = 0, values = 0, nested = 0, outside = 0, unmatched = 0;
};

/** 49 recorders shaped like the real parameters: pitch 49 steps, type 3, velocity continuous, length 16. */
struct GridRig
{
    std::array<Rec, StepGridModel::kNumTargets> rec;
    StepGridModel grid;
    static std::array<ParamTarget*, StepGridModel::kNumTargets> pointers (std::array<Rec, StepGridModel::kNumTargets>& r)
    {
        std::array<ParamTarget*, StepGridModel::kNumTargets> p {};
        for (int s = 0; s < StepGridModel::kSteps; ++s)
        {
            r[static_cast<size_t> (StepGridModel::indexOf (s, StepGridModel::Note))].init (49, 0.5f);
            r[static_cast<size_t> (StepGridModel::indexOf (s, StepGridModel::Type))].init (3, 0.0f);
            r[static_cast<size_t> (StepGridModel::indexOf (s, StepGridModel::Velocity))].init (0, 0.8f);
        }
        r[StepGridModel::kLengthIndex].init (16, 1.0f);
        for (size_t i = 0; i < p.size(); ++i)
            p[i] = &r[i];
        return p;
    }
    GridRig() : grid (pointers (rec))
    {
        StepGridModel::Geometry g;
        g.x0 = 100.0f;
        g.colW = 60.0f;
        g.pitchTop = 18.0f;
        g.pitchH = 144.0f;
        g.velTop = 172.0f;
        g.velH = 64.0f;
        grid.setGeometry (g);
    }
    Rec& at (int step, int field) { return rec[static_cast<size_t> (StepGridModel::indexOf (step, field))]; }
    float colX (int col) const { return 100.0f + 60.0f * static_cast<float> (col) + 30.0f; } // the middle of a column
    bool allClean() const
    {
        for (const auto& r : rec)
            if (! r.clean())
                return false;
        return true;
    }
    int totalBegins() const
    {
        int n = 0;
        for (const auto& r : rec)
            n += r.begins;
        return n;
    }
};

void hostWrite (juce::RangedAudioParameter& p, float normalised)
{
    std::thread ([&p, normalised] { p.setValueNotifyingHost (normalised); }).join();
}

juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<float> pos, juce::ModifierKeys mods = {}, int clicks = 1)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pos, now,
                             clicks, false);
}

/** A point given in the grid's coordinates, as an event on `c` (a child of the grid). */
juce::MouseEvent gridEvent (StepGrid& grid, juce::Component& c, juce::Point<float> inGrid, juce::ModifierKeys mods = {}, int clicks = 1)
{
    return mouseEvent (c, c.getLocalPoint (&grid, inGrid), mods, clicks);
}

// ---- What the engine plays, as the preview shows it ----
struct Played
{
    int note;
    double start, length;
    float velocity;
};

/** Pairs the note-ons and note-offs a part received into notes over a window of `beats` beats at 120 bpm / 48 kHz. */
std::vector<Played> playedBy (const std::vector<AbsEvent>& events, double beats)
{
    const double perBeat = ArpPreviewModel::kSampleRate * 60.0 / ArpPreviewModel::kBpm;
    std::vector<Played> out;
    std::array<int, 128> open;
    open.fill (-1);
    for (const auto& pe : events)
    {
        if (pe.pos >= static_cast<long long> (beats * perBeat))
            continue;
        const double beat = static_cast<double> (pe.pos) / perBeat;
        if (pe.e.type == EventType::NoteOn)
        {
            out.push_back ({ pe.e.note, beat, beats - beat, pe.e.value });
            open[pe.e.note] = static_cast<int> (out.size()) - 1;
        }
        else if (pe.e.type == EventType::NoteOff && open[pe.e.note] >= 0)
        {
            out[static_cast<size_t> (open[pe.e.note])].length = beat - out[static_cast<size_t> (open[pe.e.note])].start;
            open[pe.e.note] = -1;
        }
    }
    return out;
}

std::vector<Played> previewOf (const ArpPreviewModel& m, int part)
{
    std::vector<Played> out;
    for (const auto& n : m.notes())
        if (n.part == part)
            out.push_back ({ n.note, n.startBeat, n.lengthBeats, n.velocity });
    return out;
}

void requireSame (const std::vector<Played>& preview, const std::vector<Played>& engine, const juce::String& what)
{
    INFO (what);
    REQUIRE (preview.size() == engine.size());
    for (size_t i = 0; i < preview.size(); ++i)
    {
        INFO ("note " << i);
        CHECK (preview[i].note == engine[i].note);
        CHECK (preview[i].start == Approx (engine[i].start).margin (1.0e-9));
        CHECK (preview[i].length == Approx (engine[i].length).margin (1.0e-9));
        CHECK (preview[i].velocity == Approx (engine[i].velocity).margin (0.005)); // the preview plays 0.8, the keys here 102 / 127
    }
}

/** The settings the engine itself reads for the processor's parameters. */
ArpSettings engineSettings (VintageSynthProcessor& proc)
{
    ArpParamBinding binding;
    binding.bind (proc.getState());
    ArpSettings s;
    binding.read (s);
    return s;
}

struct Setup
{
    int pattern, octaves, rate;
    float gate, swing;
};

void configure (Rig& rig, const Setup& c, int mode, int target)
{
    rig.set ("arp.mode", static_cast<float> (mode));
    rig.set ("arp.target", static_cast<float> (target));
    rig.set ("arp.pattern", static_cast<float> (c.pattern));
    rig.set ("arp.octaves", static_cast<float> (c.octaves));
    rig.set ("arp.rate", static_cast<float> (c.rate));
    rig.set ("arp.gate", c.gate);
    rig.set ("arp.swing", c.swing);
}

const Setup kSetups[] {
    { 0, 1, 3, 50.0f, 50.0f },  // Up, 1/16
    { 1, 2, 1, 80.0f, 60.0f },  // Down, 2 octaves, 1/8, swing
    { 2, 3, 2, 100.0f, 50.0f }, // Up + Down, 1/8T, legato
    { 3, 2, 4, 30.0f, 75.0f },  // Random, 1/16T, short, hard swing
    { 4, 1, 0, 50.0f, 50.0f },  // As played, 1/4
    { 0, 4, 5, 50.0f, 50.0f },  // Up, 4 octaves, 1/32
    { 2, 2, 3, 100.0f, 65.0f }, // Up + Down, legato, swing
};

int windowSamples (int rateIndex)
{
    return static_cast<int> (ArpPreviewModel::windowBeats (static_cast<seq::Rate> (rateIndex)) * ArpPreviewModel::kSampleRate * 60.0 / ArpPreviewModel::kBpm);
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
// Models
// =================================================================================================

TEST_CASE ("Step grid model: pointer to pitch, velocity, column and length", "[ui][models][arpseq]")
{
    GridRig r;
    auto& g = r.grid;
    // Pitch lane: top 18, 144 px tall, so 3 px per semitone and 0 in the middle (y = 90).
    CHECK (g.geometry().pxPerSemitone() == Approx (3.0f));
    CHECK (g.semitonesAt (90.0f, false) == 0);
    CHECK (g.semitonesAt (90.0f - 21.0f, false) == 7);
    CHECK (g.semitonesAt (90.0f + 36.0f, false) == -12);
    CHECK (g.semitonesAt (90.0f - 21.4f, false) == 7); // rounds to the nearest semitone
    CHECK (g.semitonesAt (90.0f - 22.6f, false) == 8);
    CHECK (g.semitonesAt (-500.0f, false) == 24); // clamped to the range
    CHECK (g.semitonesAt (900.0f, false) == -24);
    // Shift snaps to octaves.
    CHECK (g.semitonesAt (90.0f - 21.0f, true) == 12);
    CHECK (g.semitonesAt (90.0f - 15.0f, true) == 0);
    CHECK (g.semitonesAt (90.0f + 51.0f, true) == -12);
    CHECK (g.semitonesAt (-500.0f, true) == 24);
    // And back: the y of a pitch is where that pitch is read from.
    for (int st = -24; st <= 24; ++st)
        CHECK (g.semitonesAt (g.pitchY (st), false) == st);
    CHECK (StepGridModel::pitchNormalised (-24) == Approx (0.0f));
    CHECK (StepGridModel::pitchNormalised (0) == Approx (0.5f));
    CHECK (StepGridModel::pitchNormalised (24) == Approx (1.0f));
    for (int st = -24; st <= 24; ++st)
        CHECK (StepGridModel::semitonesOfNormalised (StepGridModel::pitchNormalised (st)) == st);
    // Velocity lane: top 172, 64 tall.
    CHECK (g.velocityAt (172.0f) == Approx (1.0f));
    CHECK (g.velocityAt (236.0f) == Approx (0.0f));
    CHECK (g.velocityAt (204.0f) == Approx (0.5f));
    CHECK (g.velocityAt (0.0f) == Approx (1.0f));
    CHECK (g.velocityAt (999.0f) == Approx (0.0f));
    CHECK (g.velocityY (0.5f) == Approx (204.0f));
    // Columns clamp: a drag past either end keeps painting the end column.
    CHECK (g.columnAt (99.0f) == 0);
    CHECK (g.columnAt (r.colX (5)) == 5);
    CHECK (g.columnAt (100.0f + 60.0f * 5.0f) == 5); // the boundary belongs to the column on its right
    CHECK (g.columnAt (100000.0f) == 15);
    // The length flag snaps to the nearest step boundary.
    CHECK (g.lengthAt (100.0f + 60.0f * 12.0f + 20.0f) == 12);
    CHECK (g.lengthAt (100.0f + 60.0f * 12.0f + 40.0f) == 13);
    CHECK (g.lengthAt (-10.0f) == 1);
    CHECK (g.lengthAt (99999.0f) == 16);
    CHECK (StepGridModel::lengthNormalised (1) == Approx (0.0f));
    CHECK (StepGridModel::lengthNormalised (16) == Approx (1.0f));
    CHECK (StepGridModel::lengthNormalised (9) == Approx (8.0f / 15.0f));
}

TEST_CASE ("Step grid model: a drag across columns paints every crossed step; one gesture per parameter touched", "[ui][models][arpseq][gestures]")
{
    GridRig r;
    auto& g = r.grid;
    g.pressBar (StepGridModel::Lane::Pitch, r.colX (2), g.pitchY (5), false);
    CHECK (r.at (2, StepGridModel::Note).v == Approx (StepGridModel::pitchNormalised (5)));
    CHECK (r.at (3, StepGridModel::Note).values == 0); // not touched yet
    g.dragBar (r.colX (4), g.pitchY (9), false);       // crosses 3 and 4; 2 follows the pointer's value too
    for (int c : { 2, 3, 4 })
        CHECK (r.at (c, StepGridModel::Note).v == Approx (StepGridModel::pitchNormalised (9)));
    g.dragBar (r.colX (3), g.pitchY (-4), false); // back over 3 and 4 (the last column and the one under the pointer)
    CHECK (r.at (3, StepGridModel::Note).v == Approx (StepGridModel::pitchNormalised (-4)));
    CHECK (r.at (4, StepGridModel::Note).v == Approx (StepGridModel::pitchNormalised (-4)));
    CHECK (r.at (2, StepGridModel::Note).v == Approx (StepGridModel::pitchNormalised (9))); // not crossed again
    CHECK (r.totalBegins() == 3);                                                          // begun when first touched
    CHECK (g.isInGesture());
    CHECK (r.at (2, StepGridModel::Note).open);
    CHECK_FALSE (r.at (5, StepGridModel::Note).open);
    g.release();
    CHECK_FALSE (g.isInGesture());
    CHECK (r.allClean());
    CHECK (r.totalBegins() == 3);
    CHECK (r.at (5, StepGridModel::Note).values == 0);
    // Other fields of the same steps were never touched.
    CHECK (r.at (2, StepGridModel::Velocity).values == 0);
    CHECK (r.at (2, StepGridModel::Type).values == 0);

    // A fast sweep from the first column to the last and beyond sets all 16.
    GridRig s;
    s.grid.pressBar (StepGridModel::Lane::Velocity, s.colX (0), s.grid.velocityY (0.25f), false);
    s.grid.dragBar (100000.0f, s.grid.velocityY (0.75f), false);
    for (int c = 0; c < 16; ++c)
        CHECK (s.at (c, StepGridModel::Velocity).v == Approx (0.75f));
    s.grid.release();
    CHECK (s.allClean());
    CHECK (s.totalBegins() == 16);

    // Painting to the left works the same way.
    GridRig t;
    t.grid.pressBar (StepGridModel::Lane::Pitch, t.colX (8), t.grid.pitchY (0), true);
    t.grid.dragBar (t.colX (5), t.grid.pitchY (12), true);
    for (int c = 5; c <= 8; ++c)
        CHECK (t.at (c, StepGridModel::Note).v == Approx (StepGridModel::pitchNormalised (12)));
    t.grid.release();
    CHECK (t.allClean());
    CHECK (t.totalBegins() == 4);
}

TEST_CASE ("Step grid model: Shift snaps to octaves while painting; velocity follows the pointer", "[ui][models][arpseq]")
{
    GridRig r;
    r.grid.pressBar (StepGridModel::Lane::Pitch, r.colX (1), r.grid.pitchY (7), true);
    CHECK (r.at (1, StepGridModel::Note).v == Approx (StepGridModel::pitchNormalised (12)));
    r.grid.dragBar (r.colX (1), r.grid.pitchY (7), false); // Shift released: the pointer's own value
    CHECK (r.at (1, StepGridModel::Note).v == Approx (StepGridModel::pitchNormalised (7)));
    r.grid.release();
    GridRig v;
    v.grid.pressBar (StepGridModel::Lane::Velocity, v.colX (3), 172.0f + 16.0f, false); // a quarter from the top
    CHECK (v.at (3, StepGridModel::Velocity).v == Approx (0.75f));
    v.grid.release();
    CHECK (v.allClean());
}

TEST_CASE ("Step grid model: drag without press, press in the wrong lane, double-click, cancel", "[ui][models][arpseq][gestures]")
{
    GridRig r;
    r.grid.dragBar (r.colX (4), 50.0f, false); // no press: nothing happens
    CHECK (r.totalBegins() == 0);
    r.grid.pressBar (StepGridModel::Lane::Flag, r.colX (4), 50.0f, false); // a bar press needs a bar lane
    r.grid.pressBar (StepGridModel::Lane::None, r.colX (4), 50.0f, false);
    CHECK (r.totalBegins() == 0);

    // Double-click: the default as one gesture.
    r.at (6, StepGridModel::Note).v = 0.9f;
    r.grid.reset (6, StepGridModel::Note);
    CHECK (r.at (6, StepGridModel::Note).v == Approx (0.5f));
    CHECK (r.at (6, StepGridModel::Note).begins == 1);
    CHECK (r.at (6, StepGridModel::Note).values == 1);
    CHECK (r.allClean());
    r.grid.reset (99, 0); // out of range: ignored
    r.grid.reset (0, 7);
    CHECK (r.totalBegins() == 1);

    // The page closes mid-drag: every open gesture ends, once.
    r.grid.pressBar (StepGridModel::Lane::Pitch, r.colX (0), 90.0f, false);
    r.grid.dragBar (r.colX (7), 70.0f, false);
    CHECK (r.at (3, StepGridModel::Note).open);
    r.grid.cancel();
    CHECK (r.allClean());
    CHECK_FALSE (r.grid.isInGesture());
    r.grid.cancel(); // twice is harmless
    CHECK (r.allClean());
    // A second press without a release (lost mouse-up) ends the first drag.
    r.grid.pressBar (StepGridModel::Lane::Velocity, r.colX (0), 180.0f, false);
    r.grid.pressBar (StepGridModel::Lane::Velocity, r.colX (1), 180.0f, false);
    CHECK (r.at (0, StepGridModel::Velocity).clean());
    r.grid.release();
    CHECK (r.allClean());
}

TEST_CASE ("Step grid model: a contested drag shows the user's value and mouse-up writes nothing", "[ui][models][arpseq][gestures]")
{
    GridRig r;
    r.grid.pressBar (StepGridModel::Lane::Pitch, r.colX (2), r.grid.pitchY (5), false);
    CHECK (r.grid.writtenValue (StepGridModel::indexOf (2, StepGridModel::Note)) == Approx (StepGridModel::pitchNormalised (5)));
    r.at (2, StepGridModel::Note).v = 0.9f; // the host keeps writing this parameter
    const int valuesBefore = r.at (2, StepGridModel::Note).values;
    CHECK (r.grid.writtenValue (StepGridModel::indexOf (2, StepGridModel::Note)) == Approx (StepGridModel::pitchNormalised (5))); // still the user's
    r.grid.release();
    CHECK (r.at (2, StepGridModel::Note).values == valuesBefore); // mouse-up wrote nothing
    CHECK (r.at (2, StepGridModel::Note).v == Approx (0.9f));
    CHECK (r.allClean());
}

TEST_CASE ("Step grid model: the length flag snaps to steps; R and T toggle between Note and Rest or Tie", "[ui][models][arpseq][gestures]")
{
    GridRig r;
    r.grid.pressFlag (100.0f + 60.0f * 12.0f + 10.0f);
    CHECK (r.rec[StepGridModel::kLengthIndex].v == Approx (StepGridModel::lengthNormalised (12)));
    r.grid.dragFlag (100.0f + 60.0f * 7.0f - 25.0f);
    CHECK (r.rec[StepGridModel::kLengthIndex].v == Approx (StepGridModel::lengthNormalised (7)));
    r.grid.dragFlag (-500.0f);
    CHECK (r.rec[StepGridModel::kLengthIndex].v == Approx (0.0f));
    CHECK (r.rec[StepGridModel::kLengthIndex].begins == 1);
    r.grid.release();
    CHECK (r.allClean());

    // R / T: the lit button returns the step to Note. (current type, button) -> new type
    CHECK (StepGridModel::typeAfterPress (0, 0) == 1); // Note, R -> Rest
    CHECK (StepGridModel::typeAfterPress (1, 0) == 0); // Rest, R -> Note
    CHECK (StepGridModel::typeAfterPress (2, 0) == 1); // Tie, R -> Rest
    CHECK (StepGridModel::typeAfterPress (0, 1) == 2); // Note, T -> Tie
    CHECK (StepGridModel::typeAfterPress (2, 1) == 0); // Tie, T -> Note
    CHECK (StepGridModel::typeAfterPress (1, 1) == 2); // Rest, T -> Tie
}

TEST_CASE ("Feed readers: audio alive, the playing step, the held-key memory", "[ui][models][arpseq]")
{
    FeedAlive alive;
    CHECK_FALSE (alive.alive()); // nothing seen yet
    alive.update (10, 1000.0);
    CHECK (alive.alive());
    alive.update (10, 1150.0);
    CHECK (alive.alive());
    alive.update (10, 1201.0); // the counter has not moved for more than 200 ms
    CHECK_FALSE (alive.alive());
    alive.update (11, 1210.0);
    CHECK (alive.alive());
    alive.update (11, 5.0); // a clock that went backwards (tests) counts as moving
    CHECK (alive.alive());

    // Which part's step is shown: Sequencer mode, audio alive; Lower for Whole; Both: Lower, else Upper.
    CHECK (playingStepOf (true, true, 0, 1, 5, 9) == 5);
    CHECK (playingStepOf (false, true, 0, 1, 5, 9) == -1);
    CHECK (playingStepOf (true, false, 0, 1, 5, 9) == -1);
    CHECK (playingStepOf (true, true, 1, 1, 5, 9) == 9);  // Upper only
    CHECK (playingStepOf (true, true, 1, 0, 5, 9) == 5);  // Whole: only Lower plays
    CHECK (playingStepOf (true, true, 2, 1, 5, 9) == 5);  // Both: Lower
    CHECK (playingStepOf (true, true, 2, 1, -1, 9) == 9); // Both, Lower silent: Upper
    CHECK (playingStepOf (true, true, 2, 1, -1, -1) == -1);
    CHECK (playingStepOf (true, true, 0, 1, 16, 0) == -1); // out of range

    HeldKeysMemory mem;
    UiFeed::Order none;
    none.fill (0xFF);
    UiFeed::Order keys = none;
    keys[0] = 67;
    keys[1] = 60;
    CHECK_FALSE (mem.update (none, 100.0));
    CHECK (mem.asVector().empty());
    CHECK (mem.update (keys, 200.0));
    CHECK (mem.asVector() == std::vector<int> { 67, 60 }); // press order
    CHECK_FALSE (mem.update (keys, 210.0));                // unchanged
    CHECK_FALSE (mem.update (none, 1500.0));               // released 1.3 s ago: still remembered
    CHECK (mem.asVector() == std::vector<int> { 67, 60 });
    CHECK (mem.update (none, 2300.0)); // more than 2 s after the last time a key was held: forgotten
    CHECK (mem.asVector().empty());
}

// =================================================================================================
// The preview equals what the processor plays
// =================================================================================================

TEST_CASE ("Preview: the model equals the events the processor delivers to the part (real processor, offline)", "[ui][arpseq][preview]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    int comparedNotes = 0;
    for (const auto& c : kSetups)
        for (int block : { 480, 64 })
        {
            Rig rig;
            rig.prepare();
            configure (rig, c, 1, 0);
            rig.set ("key.mode", 0.0f);
            rig.play (120.0, 0.0);
            const double beats = ArpPreviewModel::windowBeats (static_cast<seq::Rate> (c.rate));
            // Pressed in this order: the arpeggiator's "As played" shows it.
            rig.render (windowSamples (c.rate), { noteOn (0, 67, 102), noteOn (0, 60, 102), noteOn (0, 64, 102) }, block);
            REQUIRE (rig.eventsRecorded);

            ArpPreviewInput in;
            in.settings = engineSettings (rig.proc).settings;
            in.partOn[0] = true;
            in.keys[0] = { 67, 60, 64 };
            ArpPreviewModel m;
            m.compute (in);
            const auto engine = playedBy (rig.partEvents[0], beats);
            comparedNotes += static_cast<int> (engine.size());
            INFO ("pattern " << c.pattern << " octaves " << c.octaves << " rate " << c.rate << " gate " << c.gate << " swing " << c.swing << " block " << block);
            CHECK (engine.size() >= 4); // a comparison of nothing proves nothing
            requireSame (previewOf (m, 0), engine, "Lower");
            CHECK (previewOf (m, 1).empty());
        }
    CHECK (comparedNotes > 150);
}

TEST_CASE ("Preview: the page's example chord, routed by the key mode, equals what both parts play", "[ui][arpseq][preview]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    struct Mode
    {
        int keyMode, split, target;
        const char* name;
    };
    const Mode modes[] { { 0, 60, 2, "Whole" }, { 1, 60, 2, "Dual, both" }, { 1, 60, 1, "Dual, Upper only" }, { 2, 62, 2, "Split at D4, both" }, { 2, 66, 0, "Split, Lower only" } };
    for (const auto& mode : modes)
        for (const auto& c : { kSetups[0], kSetups[2], kSetups[3] })
        {
            EditorRig rig;
            configure (rig, c, 1, mode.target);
            rig.set ("key.mode", static_cast<float> (mode.keyMode));
            rig.set ("key.split_point", static_cast<float> (mode.split));
            rig.tick();
            auto& page = rig.editor->getArpSeqPage();
            REQUIRE_FALSE (page.previewUsesPlayerKeys());
            CHECK (page.previewSource() == "example chord C E G");
            const auto before = page.preview().notes();

            rig.play (120.0, 0.0);
            rig.render (windowSamples (c.rate), { noteOn (0, 60, 102), noteOn (0, 64, 102), noteOn (0, 67, 102) }, 480);
            REQUIRE (rig.eventsRecorded);
            const double beats = ArpPreviewModel::windowBeats (static_cast<seq::Rate> (c.rate));
            INFO (mode.name << ", pattern " << c.pattern << " rate " << c.rate);
            for (int part = 0; part < 2; ++part)
            {
                // A part the arpeggiator does not sit in front of just gets the keys: nothing to preview.
                if (! page.previewInput().partOn[part])
                {
                    CHECK (previewOf (page.preview(), part).empty());
                    continue;
                }
                requireSame (previewOf (page.preview(), part), playedBy (rig.partEvents[static_cast<size_t> (part)], beats), part == 0 ? "Lower" : "Upper");
            }
            CHECK (page.preview().notes().size() == before.size());
            CHECK_FALSE (page.preview().notes().empty());
        }
}

TEST_CASE ("Preview: the keys the player holds come from the feed, in press order, and the page equals the processor", "[ui][arpseq][preview]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    configure (rig, kSetups[4], 1, 2); // As played, both parts
    rig.set ("key.mode", 1.0f);        // Dual: both parts get every key
    rig.tick();
    auto& page = rig.editor->getArpSeqPage();
    CHECK_FALSE (page.previewUsesPlayerKeys());
    const int runsBefore = page.previewComputations();

    rig.play (120.0, 0.0);
    rig.render (windowSamples (kSetups[4].rate), { noteOn (0, 69, 102), noteOn (0, 55, 102), noteOn (0, 62, 102) }, 256);
    REQUIRE (rig.eventsRecorded);
    rig.tick();
    CHECK (page.previewUsesPlayerKeys());
    CHECK (page.previewSource() == "your keys");
    CHECK (page.previewComputations() > runsBefore);
    CHECK (page.previewInput().keys[0] == std::vector<int> { 69, 55, 62 });
    CHECK (page.previewInput().keys[1] == std::vector<int> { 69, 55, 62 });
    const double beats = ArpPreviewModel::windowBeats (static_cast<seq::Rate> (kSetups[4].rate));
    for (int part = 0; part < 2; ++part)
    {
        const auto engine = playedBy (rig.partEvents[static_cast<size_t> (part)], beats);
        CHECK (engine.size() >= 4);
        requireSame (previewOf (page.preview(), part), engine, part == 0 ? "Lower" : "Upper");
    }

    // The keys stay in the preview for 2 s after release, then the example chord returns.
    const int at = static_cast<int> (rig.now);
    rig.render (480, { noteOff (at, 69), noteOff (at, 55), noteOff (at, 62) }, 480); // (MIDI positions are absolute)
    rig.tick();
    CHECK (page.previewUsesPlayerKeys());
    rig.tick (1500.0);
    CHECK (page.previewUsesPlayerKeys());
    rig.tick (700.0);
    CHECK_FALSE (page.previewUsesPlayerKeys());
    CHECK (page.previewSource() == "example chord C E G");
}

TEST_CASE ("Preview: the page recomputes only when one of its inputs changed", "[ui][arpseq][preview]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.tick();
    auto& page = rig.editor->getArpSeqPage();
    const int base = page.previewComputations();
    rig.tick();
    rig.tick();
    CHECK (page.previewComputations() == base); // nothing changed

    // Pattern, octaves, rate, gate, swing, target, key mode, split point, hold, mode: each one recomputes it.
    int expected = base;
    for (const auto& [id, plain] : std::vector<std::pair<const char*, float>> { { "arp.pattern", 2.0f }, { "arp.octaves", 3.0f }, { "arp.rate", 1.0f },
                                                                                { "arp.gate", 90.0f }, { "arp.swing", 70.0f }, { "arp.target", 1.0f },
                                                                                { "key.mode", 2.0f }, { "key.split_point", 50.0f }, { "arp.hold", 1.0f },
                                                                                { "arp.mode", 1.0f } })
    {
        hostWrite (rig.param (id), rig.param (id).convertTo0to1 (plain));
        rig.tick();
        ++expected;
        INFO (id);
        CHECK (page.previewComputations() == expected);
        rig.tick();
        CHECK (page.previewComputations() == expected);
    }
    // Things the preview does not depend on: the sequencer steps, the tempo, the length, other pages.
    for (const auto& [id, plain] : std::vector<std::pair<const char*, float>> { { "seq.step03.note", 7.0f }, { "seq.step03.velocity", 0.2f },
                                                                                { "seq.length", 5.0f }, { "arp.tempo", 90.0f }, { "lower.vcf.cutoff", 0.3f } })
    {
        hostWrite (rig.param (id), rig.param (id).convertTo0to1 (plain));
        rig.tick();
        INFO (id);
        CHECK (page.previewComputations() == expected);
    }
    // The picture itself follows: rebuilt exactly when the preview was.
    CHECK (page.getRoll().recomputeCount() >= expected - base);
}

TEST_CASE ("Preview: the picture states its source and a text summary, and what it shows follows the notes", "[ui][arpseq][preview]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    configure (rig, kSetups[0], 1, 0); // Up, 1/16, Lower
    rig.set ("key.mode", 0.0f);
    rig.tick();
    auto& page = rig.editor->getArpSeqPage();
    auto& roll = page.getRoll();
    CHECK (roll.summary().startsWith ("C4 E4 G4"));
    CHECK (roll.summary().contains ("example chord C E G"));
    CHECK (roll.getDescription() == roll.summary());
    CHECK (roll.getTitle() == "Arpeggiator preview");
    // 4 beats at 1/16 = 16 steps, one note each, all inside the picture.
    CHECK (roll.numBars() == 16);
    CHECK (page.preview().notes().size() == 16);
    for (int i = 0; i < roll.numBars(); ++i)
    {
        INFO ("bar " << i);
        CHECK (roll.getLocalBounds().toFloat().contains (roll.barBounds (i)));
        CHECK (roll.barNote (i) >= roll.lowestRow());
        CHECK (roll.barNote (i) <= roll.highestRow());
        // A bar starts where its step is: step i at 1/16 starts at beat i/4 (the picture is linear in time).
        CHECK (roll.barBounds (i).getX() > (i > 0 ? roll.barBounds (i - 1).getX() : -1.0f));
    }
    // The held-key marks never cover the octave labels (the "C4" label was half hidden once).
    CHECK (roll.numKeyMarks() == 3);
    for (int i = 0; i < roll.numKeyMarks(); ++i)
    {
        CHECK (roll.keyMarkBounds (i).getX() >= PianoRollView::kLabelRight);
        CHECK (roll.getLocalBounds().toFloat().contains (roll.keyMarkBounds (i)));
        CHECK (roll.keyMarkBounds (i).getRight() <= roll.barBounds (0).getX());
    }
    // Both parts play (Dual): every note shows twice, Lower's half above Upper's, never on top of each other.
    hostWrite (rig.param ("key.mode"), rig.param ("key.mode").convertTo0to1 (1.0f));
    hostWrite (rig.param ("arp.target"), rig.param ("arp.target").convertTo0to1 (2.0f));
    rig.tick();
    REQUIRE (roll.numBars() == 32);
    for (int i = 0; i < 16; ++i)
    {
        INFO ("note " << i);
        CHECK (roll.barNote (i) == roll.barNote (i + 16));
        CHECK (roll.barBounds (i).getX() == Approx (roll.barBounds (i + 16).getX()));
        CHECK_FALSE (roll.barBounds (i).intersects (roll.barBounds (i + 16)));
        CHECK (roll.barBounds (i).getY() < roll.barBounds (i + 16).getY());
    }
    hostWrite (rig.param ("key.mode"), 0.0f);
    hostWrite (rig.param ("arp.target"), 0.0f);
    rig.tick();
    // 1/4 notes need 8 steps = 8 beats.
    hostWrite (rig.param ("arp.rate"), rig.param ("arp.rate").convertTo0to1 (0.0f));
    rig.tick();
    CHECK (roll.numBars() == 8);
    CHECK (page.preview().beats() == Approx (8.0));
}

// =================================================================================================
// The feed: playing step and held-key order
// =================================================================================================

TEST_CASE ("Feed: seqStep equals the sequencer's step and the pitch it played; the grid highlights it", "[ui][arpseq][feed]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.set ("arp.mode", 2.0f);
    rig.set ("arp.target", 0.0f);
    rig.set ("arp.rate", 3.0f);
    rig.set ("key.mode", 0.0f);
    rig.set ("seq.length", 8.0f);
    for (int i = 1; i <= 8; ++i)
    {
        rig.set (juce::String ("seq.step") + juce::String (i).paddedLeft ('0', 2) + ".note", static_cast<float> (i)); // step i-1 plays root + i
        rig.set (juce::String ("seq.step") + juce::String (i).paddedLeft ('0', 2) + ".type", 0.0f);
    }
    rig.tick();
    auto& feed = rig.proc.getUiFeed();
    auto& grid = rig.editor->getArpSeqPage().getGrid();
    CHECK (feed.part[0].seqStep.load() == -1); // nothing has played yet
    rig.play (120.0, 0.0);

    int seen = 0, lastStep = -1, changes = 0;
    for (int block = 0; block < 400; ++block) // 4 s: 32 steps, four rounds of the 8-step pattern
    {
        const std::vector<Midi> midi = block == 0 ? std::vector<Midi> { noteOn (0, 60) } : std::vector<Midi> {};
        rig.render (480, midi, 480);
        rig.tick (10.0);
        const int step = feed.part[0].seqStep.load();
        CHECK (step == rig.proc.getArpSeq (0).lastSeqStep());
        CHECK (feed.part[1].seqStep.load() == -1); // Upper's arp/seq is Off (target Lower): no step
        int lastPitch = -1;
        for (auto it = rig.partEvents[0].rbegin(); it != rig.partEvents[0].rend(); ++it)
            if (it->e.type == EventType::NoteOn)
            {
                lastPitch = it->e.note;
                break;
            }
        if (lastPitch < 0)
        {
            CHECK (step == -1);
            continue;
        }
        // The step that fired last played root + (step + 1): the feed names that very step.
        CHECK (step == lastPitch - 61);
        CHECK (grid.playingStep() == step);
        ++seen;
        changes += step != lastStep ? 1 : 0;
        lastStep = step;
    }
    CHECK (seen > 300);
    CHECK (changes >= 30); // all steps, round after round

    // The audio stops (no block for a while): the highlight goes out. It returns with the next block.
    rig.tick (300.0);
    CHECK (grid.playingStep() == -1);
    rig.render (480, {}, 480);
    rig.tick (10.0);
    CHECK (grid.playingStep() == feed.part[0].seqStep.load());

    // Not in Sequencer mode: no step in the feed, none highlighted.
    rig.set ("arp.mode", 1.0f);
    rig.render (480, {}, 480);
    rig.render (480, {}, 480);
    rig.tick (10.0);
    CHECK (feed.part[0].seqStep.load() == -1);
    CHECK (grid.playingStep() == -1);
    rig.set ("arp.mode", 0.0f);
    rig.render (480, {}, 480);
    CHECK (feed.part[0].seqStep.load() == -1);
}

TEST_CASE ("Feed: Both highlights Lower's step, or Upper's when Lower is silent", "[ui][arpseq][feed]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.set ("arp.mode", 2.0f);
    rig.set ("arp.target", 2.0f);
    rig.set ("key.mode", 2.0f); // Split
    rig.set ("key.split_point", 64.0f);
    rig.set ("arp.rate", 3.0f);
    rig.tick();
    rig.play (120.0, 0.0);
    // Only an Upper key: Lower's sequencer has nothing to play, Upper's does.
    rig.render (6000 * 3, { noteOn (0, 70) }, 480);
    rig.tick (10.0);
    auto& feed = rig.proc.getUiFeed();
    CHECK (feed.part[0].seqStep.load() == -1);
    CHECK (feed.part[1].seqStep.load() >= 0);
    CHECK (rig.editor->getArpSeqPage().getGrid().playingStep() == feed.part[1].seqStep.load());
    // A Lower key as well: Lower's step is the one shown.
    rig.render (6000 * 3, { noteOn (static_cast<int> (rig.now), 50) }, 480);
    rig.tick (10.0);
    CHECK (feed.part[0].seqStep.load() >= 0);
    CHECK (rig.editor->getArpSeqPage().getGrid().playingStep() == feed.part[0].seqStep.load());
}

TEST_CASE ("Feed: the held-key order is published in press order per part, and only when it changed", "[ui][arpseq][feed]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    Rig rig;
    rig.prepare();
    rig.set ("key.mode", 2.0f);
    rig.set ("key.split_point", 64.0f);
    auto& feed = rig.proc.getUiFeed();
    UiFeed::Order o;
    rig.render (480, { noteOn (0, 67), noteOn (10, 50), noteOn (20, 72), noteOn (30, 60) }, 480);
    REQUIRE (feed.readOrder (0, o));
    CHECK (o[0] == 50);
    CHECK (o[1] == 60);
    CHECK (o[2] == 0xFF);
    REQUIRE (feed.readOrder (1, o));
    CHECK (o[0] == 67);
    CHECK (o[1] == 72);
    CHECK (o[2] == 0xFF);
    const auto seqAfterPress = feed.part[0].orderSeq.load();
    rig.render (480, {}, 480);
    CHECK (feed.part[0].orderSeq.load() == seqAfterPress); // unchanged order: nothing written
    rig.render (480, { noteOff (static_cast<int> (rig.now), 50) }, 480);
    CHECK (feed.part[0].orderSeq.load() != seqAfterPress);
    REQUIRE (feed.readOrder (0, o));
    CHECK (o[0] == 60);
    CHECK (o[1] == 0xFF);
    // Pressing a held key again moves it to the end of the order.
    rig.render (480, { noteOn (static_cast<int> (rig.now), 67) }, 480);
    REQUIRE (feed.readOrder (1, o));
    CHECK (o[0] == 72);
    CHECK (o[1] == 67);
    const int end = static_cast<int> (rig.now);
    rig.render (480, { noteOff (end, 67), noteOff (end, 72), noteOff (end, 60) }, 480);
    REQUIRE (feed.readOrder (0, o));
    CHECK (o[0] == 0xFF);
    REQUIRE (feed.readOrder (1, o));
    CHECK (o[0] == 0xFF);
}

TEST_CASE ("Feed: audio output is unchanged by the feed's new writes (arp and seq render the same for any block size)", "[ui][arpseq][feed]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    // Two renders of a Sequencer pattern with different block sizes are identical: the accessors and
    // the feed only read the engine's state.
    std::vector<float> reference;
    for (int block : { 512, 97, 64 })
    {
        Rig rig;
        rig.prepare();
        rig.set ("arp.mode", 2.0f);
        rig.set ("arp.rate", 3.0f);
        rig.set ("seq.step02.note", 5.0f);
        rig.play (130.0, 0.0);
        const auto out = rig.render (24000, { noteOn (0, 55), noteOn (100, 62) }, block);
        if (reference.empty())
            reference = out.l;
        else
        {
            REQUIRE (out.l.size() == reference.size());
            CHECK (std::memcmp (out.l.data(), reference.data(), reference.size() * sizeof (float)) == 0);
        }
    }
}

TEST_CASE ("Preview: keys held on a part the arp/seq does not play through leave the example chord", "[ui][arpseq][preview]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    configure (rig, kSetups[0], 1, 1); // plays through Upper only
    rig.set ("key.mode", 2.0f);        // Split at D4
    rig.set ("key.split_point", 62.0f);
    rig.tick();
    auto& page = rig.editor->getArpSeqPage();
    rig.play (120.0, 0.0);
    rig.render (4800, { noteOn (0, 48, 102) }, 480); // a Lower key: not on a target part
    rig.tick();
    UiFeed::Order o;
    REQUIRE (rig.proc.getUiFeed().readOrder (0, o));
    CHECK (o[0] == 48); // the feed does carry it ...
    CHECK_FALSE (page.previewUsesPlayerKeys()); // ... the preview does not use it
    CHECK (page.previewSource() == "example chord C E G");
    CHECK (page.getRoll().summary().contains ("example chord C E G"));
    CHECK (page.previewInput().keys[1] == std::vector<int> { 64, 67 });
    CHECK_FALSE (page.preview().notes().empty());

    rig.render (4800, { noteOn (static_cast<int> (rig.now), 72, 102) }, 480); // an Upper key
    rig.tick();
    CHECK (page.previewUsesPlayerKeys());
    CHECK (page.previewSource() == "your keys");
    CHECK (page.previewInput().keys[1] == std::vector<int> { 72 });
}

TEST_CASE ("Page: a contested paint drag shows the contest and mouse-up writes nothing", "[ui][arpseq][gestures]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.editor->selectPage (4, 0);
    rig.set ("arp.mode", 2.0f);
    rig.tick();
    auto& grid = rig.editor->getArpSeqPage().getGrid();
    auto& m = grid.getModel();
    GestureCounter host (rig.proc);
    const auto point = [&] (int col, float y) { return juce::Point<float> (m.geometry().x0 + m.geometry().colW * (static_cast<float> (col) + 0.5f), y); };
    auto& bar = grid.pitchBar (3);
    bar.mouseDown (gridEvent (grid, bar, point (3, m.pitchY (7))));
    CHECK (rig.param ("seq.step04.note").getCurrentValueAsText() == "+7 st");
    hostWrite (rig.param ("seq.step04.note"), rig.param ("seq.step04.note").convertTo0to1 (-10.0f)); // the host keeps writing (Read mode)
    rig.tick();
    CHECK (grid.barMarkerAlpha (3, true) > 0.0f);   // the contest is marked ...
    CHECK (grid.drawnSemitones (3) == 7);           // ... and the bar shows the user's value
    const int valuesBefore = host.values;
    bar.mouseUp (gridEvent (grid, bar, point (3, m.pitchY (7))));
    CHECK (host.values == valuesBefore); // mouse-up writes nothing
    CHECK (rig.param ("seq.step04.note").getCurrentValueAsText() == "-10 st");
    CHECK (host.begins == host.ends);
    CHECK (host.openCount() == 0);
    CHECK (host.valuesOutsideGesture == 0);
    rig.tick();
    CHECK (grid.drawnSemitones (3) == -10); // afterwards whatever the parameter holds
}

TEST_CASE ("Feed: seqStep is -1 while the plugin is bypassed, and stays -1 until a key is pressed again", "[ui][arpseq][feed]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.set ("arp.mode", 2.0f);
    rig.set ("arp.target", 0.0f);
    rig.set ("arp.rate", 3.0f);
    rig.set ("key.mode", 0.0f);
    rig.tick();
    auto& feed = rig.proc.getUiFeed();
    rig.play (120.0, 0.0);
    rig.render (6000 * 3, { noteOn (0, 60, 102) }, 480);
    REQUIRE (feed.part[0].seqStep.load() >= 0);
    rig.tick (10.0);
    CHECK (rig.editor->getArpSeqPage().getGrid().playingStep() >= 0);

    rig.bypass = true;
    for (int block = 0; block < 40; ++block) // through the 5 ms fade and well beyond it
    {
        rig.render (480, {}, 480);
        INFO ("bypassed block " << block);
        CHECK (feed.part[0].seqStep.load() == -1);
        CHECK (feed.part[1].seqStep.load() == -1);
    }
    rig.tick (10.0);
    CHECK (rig.editor->getArpSeqPage().getGrid().playingStep() == -1);
    rig.bypass = false;
    rig.render (6000 * 2, {}, 480); // held keys are forgotten: nothing plays until a key is pressed again
    CHECK (feed.part[0].seqStep.load() == -1);
    rig.render (6000 * 2, { noteOn (static_cast<int> (rig.now), 60, 102) }, 480);
    CHECK (feed.part[0].seqStep.load() >= 0);
}

TEST_CASE ("Page: the host caption and the inactive tempo need the audio to be running", "[ui][arpseq][page]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& page = rig.editor->getArpSeqPage();
    hostWrite (rig.param ("arp.mode"), rig.param ("arp.mode").convertTo0to1 (1.0f));
    rig.play (128.0, 0.0);
    rig.render (480, {}, 480);
    rig.tick();
    CHECK (page.hostCaption() == "host: 128 bpm");
    CHECK (page.getTempoSlider().isInactive());
    rig.tick (300.0); // no block for 300 ms: the feed is stale (the host stopped calling, or the plugin is suspended)
    CHECK (page.hostCaption().isEmpty());
    CHECK_FALSE (page.getTempoSlider().isInactive());
    rig.render (480, {}, 480);
    rig.tick (10.0);
    CHECK (page.hostCaption() == "host: 128 bpm");
    CHECK (page.getTempoSlider().isInactive());
}

TEST_CASE ("Step grid: a tie shows the pitch it holds; rests and ties look different from steps past the length", "[ui][arpseq][grid]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.editor->selectPage (4, 0);
    rig.set ("arp.mode", 2.0f);
    const auto step = [&] (int n, float note, float type) {
        const auto id = "seq.step" + juce::String (n).paddedLeft ('0', 2);
        rig.set (id + ".note", note);
        rig.set (id + ".type", type);
    };
    rig.set ("seq.length", 8.0f);
    step (1, 3.0f, 0.0f);   // Note +3
    step (2, 12.0f, 2.0f);  // Tie (its own stored pitch +12 never sounds)
    step (3, 0.0f, 2.0f);   // Tie of a tie
    step (4, 0.0f, 1.0f);   // Rest
    step (5, 0.0f, 2.0f);   // Tie after a rest: holds nothing
    step (6, 7.0f, 0.0f);   // Note +7
    step (7, 0.0f, 0.0f);
    step (8, 0.0f, 0.0f);
    rig.tick();
    auto& grid = rig.editor->getArpSeqPage().getGrid();
    CHECK (grid.tiedSemitones (1) == 3);
    CHECK (grid.tiedSemitones (2) == 3);
    CHECK (grid.tiedSemitones (4) == StepGrid::kNoTiedNote);
    // The sequence wraps: a tie on step 1 holds the last step before it.
    step (1, 0.0f, 2.0f);
    step (8, -9.0f, 0.0f);
    rig.tick();
    CHECK (grid.tiedSemitones (0) == -9);
    // Beyond the length nothing wraps.
    rig.set ("seq.length", 4.0f);
    step (7, 0.0f, 2.0f);
    step (6, 5.0f, 0.0f);
    rig.tick();
    CHECK (grid.tiedSemitones (6) == 5);

    // What is drawn: the tie's bar continues the held pitch (dimmed), not its stored one.
    rig.set ("seq.length", 8.0f);
    step (1, 5.0f, 0.0f);
    step (2, 12.0f, 2.0f);
    step (3, 0.0f, 1.0f);
    rig.tick();
    const auto snapshot = [&] (juce::Component& c) { return c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f); };
    const auto& m = grid.getModel();
    {
        auto& bar = grid.pitchBar (1);
        const auto image = snapshot (bar);
        const int x = bar.getWidth() / 2;
        const auto held = image.getPixelAt (x, juce::roundToInt (m.pitchY (5)) + 2 - bar.getY());
        const auto stored = image.getPixelAt (x, juce::roundToInt (m.pitchY (12)) + 2 - bar.getY());
        CHECK (held.getRed() > held.getBlue() + 30);                     // the dimmed bar of the held +5
        CHECK (std::abs (stored.getRed() - stored.getBlue()) < 12);      // nothing at the stored +12
    }
    // A rest in the pattern keeps its frame; a step past the length is faded as a whole.
    rig.set ("seq.length", 4.0f);
    step (9, 0.0f, 0.0f);
    rig.tick();
    const auto frameBrightness = [&] (int stepIndex) {
        auto& bar = grid.pitchBar (stepIndex);
        const auto c = snapshot (bar).getPixelAt (0, bar.getHeight() / 2);
        return c.getRed() + c.getGreen() + c.getBlue();
    };
    CHECK (frameBrightness (2) > frameBrightness (8) + 40); // rest inside the length vs a note beyond it
    CHECK (frameBrightness (0) > frameBrightness (8) + 40);
}


// =================================================================================================
// The page and its components
// =================================================================================================

TEST_CASE ("Page: painting steps with the real bars; gestures begin and end once per parameter", "[ui][arpseq][gestures]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.editor->selectPage (4, 0);
    rig.set ("arp.mode", 2.0f);
    rig.tick();
    auto& page = rig.editor->getArpSeqPage();
    REQUIRE (page.shownView() == 1);
    auto& grid = page.getGrid();
    auto& m = grid.getModel();
    REQUIRE (grid.columnWidth() > 8);
    GestureCounter host (rig.proc);

    // Mouse down on step 3's pitch bar, drag right across steps 4 and 5, then back to step 4, release.
    const auto point = [&] (int col, float y) { return juce::Point<float> (m.geometry().x0 + m.geometry().colW * (static_cast<float> (col) + 0.5f), y); };
    auto& bar3 = grid.pitchBar (2);
    bar3.mouseDown (gridEvent (grid, bar3, point (2, m.pitchY (7))));
    bar3.mouseDrag (gridEvent (grid, bar3, point (4, m.pitchY (-5))));
    bar3.mouseDrag (gridEvent (grid, bar3, point (3, m.pitchY (-5))));
    CHECK (host.begins == 3); // steps 3, 4, 5: begun when first touched
    CHECK (host.ends == 0);
    CHECK (host.nestedBegins == 0);
    CHECK (host.valuesOutsideGesture == 0);
    bar3.mouseUp (gridEvent (grid, bar3, point (3, m.pitchY (-5))));
    CHECK (host.begins == 3);
    CHECK (host.ends == 3);
    CHECK (host.openCount() == 0);
    CHECK (host.valuesOutsideGesture == 0);
    CHECK (rig.param ("seq.step03.note").getCurrentValueAsText() == "-5 st");
    CHECK (rig.param ("seq.step04.note").getCurrentValueAsText() == "-5 st");
    CHECK (rig.param ("seq.step05.note").getCurrentValueAsText() == "-5 st");
    CHECK (rig.param ("seq.step06.note").getCurrentValueAsText() == "0 st"); // not crossed
    CHECK (rig.param ("seq.step02.note").getCurrentValueAsText() == "0 st");
    CHECK (rig.param ("seq.step03.velocity").getValue() == Approx (0.8f)); // other fields untouched

    // The bars show it (the gesture's own value at once, the mirror's at the next tick).
    rig.tick();
    CHECK (grid.drawnSemitones (2) == -5);
    CHECK (grid.drawnSemitones (4) == -5);
    CHECK (grid.drawnSemitones (5) == 0);

    // Shift snaps to octaves, painting velocity across steps.
    auto& v1 = grid.velocityBar (0);
    host.begins = host.ends = 0;
    v1.mouseDown (gridEvent (grid, v1, point (0, m.velocityY (0.25f))));
    v1.mouseDrag (gridEvent (grid, v1, point (2, m.velocityY (0.5f))));
    v1.mouseUp (gridEvent (grid, v1, point (2, m.velocityY (0.5f))));
    CHECK (host.begins == 3);
    CHECK (host.ends == 3);
    for (int s = 1; s <= 3; ++s)
        CHECK (rig.param (juce::String ("seq.step0") + juce::String (s) + ".velocity").getValue() == Approx (0.5f).margin (0.01));
    auto& p0 = grid.pitchBar (0);
    p0.mouseDown (gridEvent (grid, p0, point (0, m.pitchY (7)), juce::ModifierKeys (juce::ModifierKeys::shiftModifier)));
    p0.mouseUp (gridEvent (grid, p0, point (0, m.pitchY (7))));
    CHECK (rig.param ("seq.step01.note").getCurrentValueAsText() == "+12 st");

    // Double-click resets one bar to its default as one gesture; a right-click writes nothing.
    host.begins = host.ends = host.values = 0;
    p0.mouseDoubleClick (gridEvent (grid, p0, point (0, m.pitchY (0)), {}, 2));
    CHECK (rig.param ("seq.step01.note").getCurrentValueAsText() == "0 st");
    CHECK (host.begins == 1);
    CHECK (host.ends == 1);
    CHECK (host.values == 1);
    CHECK (host.valuesOutsideGesture == 0);

    // The page goes away mid-drag: every gesture ends.
    host.begins = host.ends = 0;
    auto& b9 = grid.pitchBar (8);
    b9.mouseDown (gridEvent (grid, b9, point (8, m.pitchY (3))));
    b9.mouseDrag (gridEvent (grid, b9, point (11, m.pitchY (3))));
    CHECK (host.openCount() == 4);
    rig.editor.reset();
    CHECK (host.openCount() == 0);
    CHECK (host.begins == host.ends);
    CHECK (host.nestedBegins == 0);
}

TEST_CASE ("Page: R and T buttons and the length flag, through the real components", "[ui][arpseq][gestures]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.editor->selectPage (4, 0);
    rig.set ("arp.mode", 2.0f);
    rig.tick();
    auto& grid = rig.editor->getArpSeqPage().getGrid();
    GestureCounter host (rig.proc);
    auto& step6 = grid.typeToggle (5);
    CHECK (step6.getTitle() == "Step 6 type");
    CHECK_FALSE (step6.isRest());
    CHECK_FALSE (step6.isTie());

    const auto text = [&] { return rig.param ("seq.step06.type").getCurrentValueAsText(); };
    step6.press (0);
    CHECK (host.begins == 1);
    CHECK (host.ends == 0);
    CHECK (text() == "Rest");
    step6.release();
    CHECK (host.ends == 1);
    rig.tick();
    CHECK (step6.isRest());
    step6.press (0); // the lit button returns the step to Note
    step6.release();
    CHECK (text() == "Note");
    step6.press (1);
    step6.release();
    CHECK (text() == "Tie");
    rig.tick();
    CHECK (step6.isTie());
    step6.press (0); // R while Tie is lit: Rest
    step6.release();
    CHECK (text() == "Rest");
    CHECK (host.begins == host.ends);
    CHECK (host.valuesOutsideGesture == 0);
    CHECK (host.nestedBegins == 0);
    // Its two buttons are real components with their own accessible names.
    REQUIRE (step6.getNumChildComponents() == 2);
    CHECK (step6.getChildComponent (0)->getTitle() == "Step 6 rest");
    CHECK (step6.getChildComponent (1)->getTitle() == "Step 6 tie");
    CHECK (step6.getChildComponent (0)->createAccessibilityHandler() != nullptr); // (a handler is only attached on screen)

    // The length flag: drag it to the boundary after step 4; every other binding follows.
    auto& flag = grid.lengthFlag();
    const auto& g = grid.getModel().geometry();
    host.begins = host.ends = 0;
    // The flag is grabbed where it is (not on its boundary); it keeps that offset while it moves.
    const float grab = static_cast<float> (flag.getX() + flag.getWidth() / 2) - (g.x0 + g.colW * 16.0f);
    flag.mouseDown (mouseEvent (flag, flag.getLocalBounds().getCentre().toFloat()));
    const auto toLocal = [&] (float gridX) { return juce::Point<float> (gridX - static_cast<float> (flag.getX()), static_cast<float> (flag.getHeight()) * 0.5f); };
    flag.mouseDrag (mouseEvent (flag, toLocal (g.x0 + g.colW * 4.0f + grab)));
    CHECK (rig.param ("seq.length").getCurrentValueAsText() == "4");
    CHECK (grid.shownLength() == 4);
    flag.mouseUp (mouseEvent (flag, toLocal (g.x0 + g.colW * 4.0f + grab)));
    CHECK (host.begins == 1);
    CHECK (host.ends == 1);
    rig.tick();
    CHECK (grid.stepIsDimmed (4));
    CHECK_FALSE (grid.stepIsDimmed (3));
    CHECK (rig.editor->getArpSeqPage().getLengthSlider().drawnText() == "4"); // the Length slider shows it too
}

TEST_CASE ("Page: bars, toggles and the flag sit where their parameter's value puts them, for every value", "[ui][arpseq][follow]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& page = rig.editor->getArpSeqPage();
    auto& grid = page.getGrid();
    auto& m = grid.getModel();
    std::mt19937 rng (11);
    std::uniform_real_distribution<float> uni (0.0f, 1.0f);

    const auto check = [&] (const juce::String& when) {
        rig.tick();
        for (int s = 0; s < 16; ++s)
        {
            const auto n = juce::String (s + 1).paddedLeft ('0', 2);
            INFO (when << ", step " << s + 1);
            const float note = rig.param ("seq.step" + n + ".note").getValue();
            const int st = StepGridModel::semitonesOfNormalised (note);
            CHECK (grid.drawnSemitones (s) == st);
            CHECK (grid.barValueY (s, true) == Approx (m.pitchY (st)));
            CHECK (grid.barText (s, true) == rig.param ("seq.step" + n + ".note").getCurrentValueAsText());
            const float vel = rig.param ("seq.step" + n + ".velocity").getValue();
            CHECK (grid.barValueY (s, false) == Approx (m.velocityY (vel)));
            const int type = model::stepIndex (rig.param ("seq.step" + n + ".type").getValue(), 3);
            CHECK (grid.typeToggle (s).drawnType() == type);
        }
        const int len = 1 + static_cast<int> (std::lround (rig.param ("seq.length").getValue() * 15.0f));
        CHECK (grid.shownLength() == len);
        CHECK (grid.getLocalBounds().contains (grid.lengthFlag().getBounds()));
        for (int s = 0; s < 16; ++s)
            CHECK (grid.stepIsDimmed (s) == (s >= len));
        juce::String why;
        CHECK (grid.lengthFlag().showsMirroredValue (why));
    };
    for (int round = 0; round < 6; ++round)
    {
        for (int s = 1; s <= 16; ++s)
        {
            const auto n = juce::String (s).paddedLeft ('0', 2);
            hostWrite (rig.param ("seq.step" + n + ".note"), round == 0 ? 0.0f : (round == 1 ? 1.0f : uni (rng)));
            hostWrite (rig.param ("seq.step" + n + ".velocity"), round == 0 ? 0.0f : (round == 1 ? 1.0f : uni (rng)));
            hostWrite (rig.param ("seq.step" + n + ".type"), static_cast<float> ((round + s) % 3) / 2.0f);
        }
        hostWrite (rig.param ("seq.length"), round == 0 ? 0.0f : (round == 1 ? 1.0f : uni (rng)));
        check ("round " + juce::String (round));
    }
    auto& pm = rig.proc.getPresets();
    for (int program : { 9, 40, 63 })
    {
        pm.selectProgram (program);
        check ("program " + juce::String (program));
    }
    pm.initialise();
    check ("Init");

    // A session restore (any thread), as the host does it.
    juce::MemoryBlock state;
    pm.selectProgram (20);
    rig.proc.getStateInformation (state);
    pm.selectProgram (0);
    rig.tick();
    std::thread ([&] { rig.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize())); }).join();
    check ("session restore");
}

TEST_CASE ("Page: the view follows the mode; Off dims everything and keeps the last view; tempo is inactive while the host plays", "[ui][arpseq][page]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& page = rig.editor->getArpSeqPage();
    auto& es = rig.proc.getEditorState();
    rig.tick();
    CHECK (page.isOff());
    CHECK (page.shownView() == 0);
    CHECK (page.getPatternSwitch().isInactive());
    CHECK (page.getGrid().isOff());
    CHECK_FALSE (page.getModeSwitch().isInactive());

    // (a component is shown when its panel is)
    const auto shown = [] (juce::Component& c) { return c.isVisible() && c.getParentComponent()->isVisible(); };
    hostWrite (rig.param ("arp.mode"), rig.param ("arp.mode").convertTo0to1 (1.0f));
    rig.tick();
    CHECK_FALSE (page.isOff());
    CHECK (page.shownView() == 0);
    CHECK_FALSE (page.getPatternSwitch().isInactive());
    CHECK (es.arpView.load() == 0);
    CHECK (shown (page.getRoll()));
    CHECK_FALSE (shown (page.getGrid()));
    CHECK_FALSE (shown (page.getLengthSlider()));

    hostWrite (rig.param ("arp.mode"), rig.param ("arp.mode").convertTo0to1 (2.0f));
    rig.tick();
    CHECK (page.shownView() == 1);
    CHECK (es.arpView.load() == 1); // the last non-Off view is kept
    CHECK (shown (page.getGrid()));
    CHECK_FALSE (shown (page.getRoll()));
    CHECK (shown (page.getLengthSlider()));

    hostWrite (rig.param ("arp.mode"), 0.0f);
    rig.tick();
    CHECK (page.isOff());
    CHECK (page.shownView() == 1); // still the sequencer, dimmed
    CHECK (page.getGrid().isOff());
    CHECK (page.getLengthSlider().isInactive());

    // Tempo: inactive while the host plays, with the host's tempo as caption.
    hostWrite (rig.param ("arp.mode"), rig.param ("arp.mode").convertTo0to1 (1.0f));
    rig.tick();
    CHECK_FALSE (page.getTempoSlider().isInactive());
    CHECK (page.hostCaption().isEmpty());
    rig.play (128.0, 0.0);
    rig.render (480, {}, 480);
    rig.tick();
    CHECK (page.getTempoSlider().isInactive());
    CHECK (page.hostCaption() == "host: 128 bpm");
    rig.stop();
    rig.render (480, {}, 480);
    rig.tick();
    CHECK_FALSE (page.getTempoSlider().isInactive());
    CHECK (page.hostCaption().isEmpty());
}

TEST_CASE ("Page: the Plays-through switch and pattern cards are registered as segmented switches with all their captions", "[ui][arpseq][page]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& page = rig.editor->getArpSeqPage();
    CHECK (page.getPatternSwitch().numSegments() == 5);
    CHECK (page.getPatternSwitch().getCaptions().size() == 5);
    CHECK (page.getTargetSwitch().getCaptions() == juce::StringArray { "Lower", "Upper", "Both" });
    CHECK (page.getRateSwitch().getCaptions().size() == 6);
    // Every bar and toggle has its own help text (the step number is put into it).
    auto& help = rig.editor->getHelpTexts();
    CHECK (help.find ("ui.arp_preview") != nullptr);
    CHECK (help.find ("ui.seq_grid") != nullptr);
    CHECK (help.find ("seq.step06.type")->find ("Note plays the step") == 0);
    CHECK (page.getGrid().typeToggle (5).getProperties()[helpIdProperty()].toString() == "seq.step06.type");
    CHECK (page.getGrid().pitchBar (5).getProperties()[helpLabelProperty()].toString() == "Step 6 pitch");
    CHECK (page.getGrid().velocityBar (5).getProperties()[helpLabelProperty()].toString() == "Step 6 velocity");
    CHECK (page.getRoll().getProperties()[helpIdProperty()].toString() == "ui.arp_preview");
}

TEST_CASE ("Layout snapshot: the Arp + Seq page in both views, 100 % and 150 %, equals the golden files", "[ui][editor][layout][arpseq]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    const bool update = juce::SystemStats::getEnvironmentVariable ("VS_UPDATE_UI_GOLDENS", {}) == "1";
    for (int view = 0; view < 2; ++view)
        for (int scale : { 100, 150 })
        {
            rig.editor->setSizePercent (scale);
            hostWrite (rig.param ("arp.mode"), rig.param ("arp.mode").convertTo0to1 (static_cast<float> (view + 1)));
            rig.editor->selectPage (4, 0);
            rig.tick();
            REQUIRE (rig.editor->getArpSeqPage().shownView() == view);
            juce::String dump;
            dump << "editor " << rig.editor->getBounds().toString() << " scale " << juce::String (rig.editor->getScale(), 4) << "\n";
            dumpTree (rig.editor->getArpSeqPage(), 0, dump);
            const auto file = juce::File (VS_UI_GOLDEN_DIR).getChildFile (juce::String ("arpseq_") + (view == 0 ? "arp" : "seq") + "_" + juce::String (scale) + ".txt");
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
/** Allocations of painting `c` (and its visible children) through the null context, per paint, over 4 paints. */
std::array<int, 4> allocationsPerPaint (juce::Component& c)
{
    NullGraphicsContext context (c.getLocalBounds(), true); // text is laid out: JUCE's per-drawText allocations are counted
    juce::Graphics g (context);
    c.paintEntireComponent (g, false); // warm-up: font and typeface caches
    std::array<int, 4> counts {};
    for (auto& n : counts)
    {
        vs::seq::test::AllocationCounter counter;
        c.paintEntireComponent (g, false);
        n = counter.count();
    }
    return counts;
}
} // namespace

TEST_CASE ("Paint: our own paint code allocates nothing it could prepare; every repeated paint costs the same", "[ui][arpseq][paint]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    rig.editor->selectPage (4, 0);
    hostWrite (rig.param ("arp.mode"), rig.param ("arp.mode").convertTo0to1 (2.0f));
    rig.set ("seq.step03.type", 1.0f);
    rig.set ("seq.step05.type", 2.0f);
    rig.tick();
    auto& page = rig.editor->getArpSeqPage();
    auto& grid = page.getGrid();

    // What is left after the null context is the layout of the text each paint draws (JUCE builds a
    // GlyphArrangement per drawText: 5 allocations, plus 1 for the font) and nothing else. A label
    // is therefore 6, a bar that draws no text 1, a rest or tie 11 (two texts). Any string or path
    // our paint code builds adds to these and fails the budget.
    struct Leaf
    {
        juce::Component* c;
        int budget;
    };
    const std::vector<Leaf> leaves {
        { &grid.pitchBar (0), 6 },  // a note: its value
        { &grid.pitchBar (2), 11 }, // a rest: value and "rest"
        { &grid.pitchBar (4), 11 }, // a tie: value and "tie"
        { &grid.velocityBar (0), 1 },
        { grid.typeToggle (0).getChildComponent (0), 6 }, // "R"
        { &grid.lengthFlag(), 6 },
        { &page.getRoll(), 26 }, // 4 beat labels, 2 octave labels
    };
    for (const auto& leaf : leaves)
    {
        const auto n = allocationsPerPaint (*leaf.c);
        INFO (leaf.c->getComponentID() << " " << leaf.c->getTitle() << ": " << n[0] << " " << n[1] << " " << n[2] << " " << n[3]);
        CHECK (n[0] == n[1]);
        CHECK (n[1] == n[2]);
        CHECK (n[2] == n[3]);
        CHECK (n[0] <= leaf.budget);
    }
    // The whole page, in the sequencer view and in the arpeggiator view: repeatable.
    for (int mode : { 2, 1, 0 })
    {
        hostWrite (rig.param ("arp.mode"), rig.param ("arp.mode").convertTo0to1 (static_cast<float> (mode)));
        rig.tick();
        const auto n = allocationsPerPaint (page);
        INFO ("page, mode " << mode << ": " << n[0] << " " << n[1] << " " << n[2] << " " << n[3]);
        CHECK (n[0] == n[1]);
        CHECK (n[1] == n[2]);
        CHECK (n[2] == n[3]);
    }
}
#endif

TEST_CASE ("Arp + Seq page: both views to PNG for human review", "[.uishots]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VS_UISHOT_DIR", {});
    if (dir.isEmpty())
        SKIP ("VS_UISHOT_DIR not set");
    const juce::File out (dir);
    REQUIRE (out.createDirectory());
    const int notes[] { 0, 3, 0, 12, 7, 3, 0, -5, 0, 10, 10, 12, 0, 0, 0, 0 };
    for (int scale : { 75, 100, 200 })
        for (int mode : { 0, 1, 2 })
        {
            EditorRig rig; // a fresh processor per shot: no held key leaks from one shot into the next
            rig.set ("arp.pattern", 2.0f);
            rig.set ("arp.octaves", 2.0f);
            rig.set ("arp.swing", 58.0f);
            rig.set ("arp.gate", 60.0f);
            rig.set ("key.mode", 1.0f);
            rig.set ("seq.length", 12.0f);
            for (int i = 0; i < 16; ++i)
            {
                const auto n = juce::String (i + 1).paddedLeft ('0', 2);
                rig.set ("seq.step" + n + ".note", static_cast<float> (notes[i]));
                rig.set ("seq.step" + n + ".velocity", 0.35f + 0.5f * static_cast<float> ((i * 7) % 5) / 4.0f);
                rig.set ("seq.step" + n + ".type", i == 2 || i == 9 ? 1.0f : (i == 5 || i == 6 ? 2.0f : 0.0f));
            }
            rig.set ("arp.mode", static_cast<float> (mode));
            rig.editor->setSizePercent (scale);
            rig.editor->selectPage (4, 0);
            if (mode == 2)
            {
                rig.play (120.0, 0.0);
                rig.render (6000 * 5 + 100, { noteOn (0, 60) }, 480); // the sequencer is on its sixth step
            }
            rig.tick();
            const auto image = rig.editor->createComponentSnapshot (rig.editor->getLocalBounds(), true, 1.0f);
            const auto file = out.getChildFile (juce::String ("arpseq_") + (mode == 0 ? "off" : (mode == 1 ? "arp" : "seq")) + "_" + juce::String (scale) + ".png");
            file.deleteFile();
            juce::FileOutputStream stream (file);
            REQUIRE (stream.openedOk());
            REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
        }
}
