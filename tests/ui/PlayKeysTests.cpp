// The Play page's keys play notes (docs/architecture.md 9.3.4, "Playing the keys"; 9.6): a press,
// a glide across keys and the release produce exactly the expected notes in the processor; the
// velocity rule; hiding the page, closing the editor or cancelling releases the note; a burst of
// presses with a stalled audio thread leaves nothing sounding; and the processor's paths (block
// longer than the prepared size, zero-length block, bypass, reset, prepare) add the editor's notes
// once or drop them. The split pill's own rules are in PlayEffectsTests.cpp.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EditorRig.h"

#include "common/ui/models/KeyboardModel.h"
#include "vintage/ui/PageList.h"
#include "vintage/ui/PlayPage.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <random>

using namespace vs;
using namespace vs::itest;
using Catch::Approx;

namespace
{
juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> p)
{
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 0.0f, 0.0f,
                             0.0f, 0.0f, 0.0f, &c, &c, juce::Time::getCurrentTime(), p, juce::Time::getCurrentTime(), 1, false);
}

/** A point on `note`'s key at `depth` of its height (0 = top, 1 = bottom), in the strip's coordinates. */
juce::Point<float> onKey (const ui::KeyboardStrip& s, int note, float depth = 0.85f)
{
    const auto k = s.getGeometry().keyRect (note);
    return { s.keysArea().getX() + k.x + k.w * 0.5f, s.keysArea().getY() + k.y + k.h * depth };
}

struct NoteRec
{
    long long pos;
    bool on;
    int note;
    float value;
    bool operator== (const NoteRec& o) const { return pos == o.pos && on == o.on && note == o.note; }
};

std::vector<NoteRec> notesOf (const Rig& rig, int part)
{
    std::vector<NoteRec> v;
    for (const auto& a : rig.partEvents[part])
        if (a.e.type == EventType::NoteOn || a.e.type == EventType::NoteOff)
            v.push_back ({ a.pos, a.e.type == EventType::NoteOn, a.e.note, a.e.value });
    return v;
}

float peak (const Stereo& s, size_t from = 0)
{
    float p = 0.0f;
    for (size_t i = from; i < s.l.size(); ++i)
        p = std::max ({ p, std::abs (s.l[i]), std::abs (s.r[i]) });
    return p;
}

/** The editor on the Play page, effects off (so "silent" means the synth's own tail is over). */
struct PlayRig : EditorRig
{
    explicit PlayRig (int maxBlockSize = 512) : EditorRig (48000.0, maxBlockSize)
    {
        set ("fx.chorus.mode", 0.0f);
        set ("fx.delay.level", 0.0f);
        set ("fx.reverb.level", 0.0f);
        editor->selectPage (static_cast<int> (ui::VintagePage::Play), 0);
        tick();
    }
    ui::KeyboardStrip& strip() { return editor->getPlayPage().getStrip(); }
    Stereo run (double seconds) { return render (samples (seconds), {}, 256); }
};
} // namespace

TEST_CASE ("Play keys: a press, a glide across three keys and the release play exactly those notes, then silence", "[ui][m7c][play][keys]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    PlayRig rig;
    auto& strip = rig.strip();
    REQUIRE (rig.get ("key.mode") == Approx (0.0f)); // Whole: every key plays Lower

    const long long t0 = rig.now;
    strip.mouseDown (mouseAt (strip, onKey (strip, 60)));
    CHECK (strip.playingNote() == 60);
    const auto held60 = rig.run (0.1);
    const long long t1 = rig.now;
    strip.mouseDrag (mouseAt (strip, onKey (strip, 62)));
    strip.mouseDrag (mouseAt (strip, onKey (strip, 62))); // the same key again: nothing new
    const auto held62 = rig.run (0.1);
    const long long t2 = rig.now;
    strip.mouseDrag (mouseAt (strip, onKey (strip, 64)));
    const auto held64 = rig.run (0.1);
    const long long t3 = rig.now;
    strip.mouseUp (mouseAt (strip, onKey (strip, 64)));
    CHECK (strip.playingNote() == -1);
    const auto tail = rig.run (3.0);

    const std::vector<NoteRec> expected { { t0, true, 60, 0.0f }, { t1, false, 60, 0.0f }, { t1, true, 62, 0.0f },
                                          { t2, false, 62, 0.0f }, { t2, true, 64, 0.0f }, { t3, false, 64, 0.0f } };
    const auto got = notesOf (rig, 0);
    CHECK (got == expected);
    CHECK (notesOf (rig, 1).empty());
    // Velocity by the height in the key (0.85 of a white key: 40 + 87 x 0.85 = 113.95 -> 114).
    for (const auto& r : got)
        if (r.on)
            CHECK (r.value == Approx (114.0f / 127.0f));

    CHECK (peak (held60) > 1.0e-3f);
    CHECK (peak (held62) > 1.0e-3f);
    CHECK (peak (held64) > 1.0e-3f);
    CHECK_FALSE (rig.proc.getLowerPart().isSounding());
    CHECK (std::fpclassify (peak (tail, tail.l.size() - static_cast<size_t> (rig.samples (0.1)))) == FP_ZERO);
    CHECK (rig.allReleased (0));
}

TEST_CASE ("Play keys: velocity follows the height in the key, 40 at the top to 127 at the bottom; outside the keys nothing plays",
           "[ui][m7c][play][keys]")
{
    // The rule (JUCE-free model).
    ui::model::KeyboardGeometry g;
    g.setSize (430.0f, 100.0f);
    const auto white = g.keyRect (60), black = g.keyRect (61);
    CHECK (g.velocityAt (60, 0.0f) == 40);
    CHECK (g.velocityAt (60, white.h) == 127);
    CHECK (g.velocityAt (60, white.h * 0.5f) == 84); // 83.5 rounds up
    CHECK (g.velocityAt (60, -5.0f) == 40);          // clamped
    CHECK (g.velocityAt (61, black.h) == 127);       // a black key: over its own height
    CHECK (g.velocityAt (61, black.h * 0.5f) == 84);
    for (float y = 0.0f; y < white.h; y += 1.0f)
        CHECK (g.velocityAt (60, y) == static_cast<int> (std::lround (40.0f + 87.0f * y / white.h)));

    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    PlayRig rig;
    auto& strip = rig.strip();
    strip.mouseDown (mouseAt (strip, onKey (strip, 60, 0.0f)));
    CHECK (strip.playingVelocity() == 40);
    strip.mouseUp (mouseAt (strip, onKey (strip, 60, 0.0f)));
    strip.mouseDown (mouseAt (strip, onKey (strip, 61, 0.999f)));
    CHECK (strip.playingVelocity() == 127);
    // Gliding takes each new key's own height; the first note keeps its velocity.
    strip.mouseDrag (mouseAt (strip, onKey (strip, 62, 0.5f)));
    CHECK (strip.playingVelocity() == 84);
    strip.mouseUp (mouseAt (strip, onKey (strip, 62, 0.5f)));
    rig.run (0.05);
    const auto got = notesOf (rig, 0);
    REQUIRE (got.size() == 6);
    CHECK (got[0].value == Approx (40.0f / 127.0f));
    CHECK (got[2].value == Approx (127.0f / 127.0f));
    CHECK (got[4].value == Approx (84.0f / 127.0f));

    // A press above the keys (bars, pill row away from the pill) plays nothing; a drag from a key
    // off the keys is silent until it comes back onto a key.
    const long long before = static_cast<long long> (notesOf (rig, 0).size());
    const juce::Point<float> aboveKeys { strip.keysArea().getX() + 3.0f, strip.keysArea().getY() - 4.0f };
    strip.mouseDown (mouseAt (strip, aboveKeys));
    CHECK (strip.playingNote() == -1);
    strip.mouseDrag (mouseAt (strip, onKey (strip, 48)));
    strip.mouseUp (mouseAt (strip, onKey (strip, 48)));
    rig.run (0.05);
    CHECK (static_cast<long long> (notesOf (rig, 0).size()) == before);
    strip.mouseDown (mouseAt (strip, onKey (strip, 48)));
    strip.mouseDrag (mouseAt (strip, aboveKeys));
    CHECK (strip.playingNote() == -1);
    strip.mouseDrag (mouseAt (strip, onKey (strip, 50)));
    CHECK (strip.playingNote() == 50);
    strip.mouseUp (mouseAt (strip, onKey (strip, 50)));
    rig.run (0.05);
    CHECK (rig.allReleased (0));
}

TEST_CASE ("Play keys: hiding the page, cancelling, hiding the strip or closing the editor mid-press releases the note", "[ui][m7c][play][keys]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    PlayRig rig;
    auto pressAndHold = [&] (int note) {
        auto& strip = rig.strip();
        strip.mouseDown (mouseAt (strip, onKey (strip, note)));
        rig.run (0.05);
        REQUIRE (rig.proc.getLowerPart().isSounding());
        REQUIRE_FALSE (rig.allReleased (0));
    };

    SECTION ("page change")
    {
        pressAndHold (60);
        rig.editor->selectPage (static_cast<int> (ui::VintagePage::Effects), 0);
        CHECK (rig.strip().playingNote() == -1);
    }
    SECTION ("cancelInteraction")
    {
        pressAndHold (60);
        rig.strip().cancelInteraction();
    }
    SECTION ("an ancestor hidden without a cancel: the next tick finds it")
    {
        pressAndHold (60);
        rig.editor->getPlayPage().setVisible (false);
        CHECK (rig.strip().playingNote() == 60);
        rig.tick();
        CHECK (rig.strip().playingNote() == -1);
    }
    SECTION ("the strip itself hidden")
    {
        pressAndHold (60);
        rig.strip().setVisible (false);
    }
    SECTION ("editor closed")
    {
        pressAndHold (60);
        rig.editor.reset();
    }
    rig.run (0.05);
    CHECK (rig.allReleased (0));
    const auto got = notesOf (rig, 0);
    REQUIRE_FALSE (got.empty());
    CHECK ((! got.back().on && got.back().note == 60));
    if (rig.editor != nullptr)
        CHECK (rig.strip().playingNote() == -1);
}

TEST_CASE ("Play keys: a burst of 1000 presses while the audio thread stalls leaves no note sounding once it drains", "[ui][m7c][play][keys]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    PlayRig rig;
    auto& strip = rig.strip();
    auto& queue = rig.proc.getUiNotes();
    std::mt19937 rng (7);
    std::uniform_int_distribution<int> key (36, 84);

    SECTION ("presses and glides")
    {
        for (int i = 0; i < 1000; ++i)
        {
            strip.mouseDown (mouseAt (strip, onKey (strip, key (rng))));
            if ((rng() & 3u) == 0)
                strip.mouseDrag (mouseAt (strip, onKey (strip, key (rng))));
            strip.mouseUp (mouseAt (strip, onKey (strip, key (rng))));
        }
        CHECK (queue.droppedNoteOns() > 900); // the queue was full nearly all the time
    }
    SECTION ("a key is held while the queue fills up, then the editor closes: its off is the last writer call")
    {
        strip.mouseDown (mouseAt (strip, onKey (strip, 60)));
        REQUIRE (queue.isOn (60));
        // A second writer on the same (message) thread fills the queue around the held key.
        for (int i = 0; i < 40; ++i)
        {
            queue.noteOn (40 + i % 12, 100);
            queue.noteOff (40 + i % 12);
        }
        CHECK (queue.droppedNoteOns() > 0);
        REQUIRE (strip.playingNote() == 60);
        rig.editor.reset(); // the off still fits: its slot was reserved by the note on
        CHECK_FALSE (queue.anyOn());
    }
    // The audio thread comes back: one block drains everything, then the release tails end.
    rig.run (0.02);
    CHECK (rig.allReleased (0));
    CHECK (queue.drain ([] (int, int) noexcept {}) == 0);
    const auto tail = rig.run (3.0);
    CHECK_FALSE (rig.proc.getLowerPart().isSounding());
    CHECK (std::fpclassify (peak (tail, tail.l.size() - static_cast<size_t> (rig.samples (0.1)))) == FP_ZERO);
}

TEST_CASE ("Play keys: the processor adds the editor's notes once per block, before the host's, and drops them where it ignores MIDI",
           "[ui][keys]")
{
    Rig rig (48000.0, 128);
    rig.prepare();
    auto& queue = rig.proc.getUiNotes();

    SECTION ("offset 0, before the host's events of that block")
    {
        REQUIRE (queue.noteOn (60, 100));
        rig.render (128, { noteOff (0, 60), noteOn (0, 64, 90) }, 128);
        const auto got = notesOf (rig, 0);
        REQUIRE (got.size() == 3);
        CHECK ((got[0].on && got[0].note == 60 && got[0].pos == 0));
        CHECK ((! got[1].on && got[1].note == 60 && got[1].pos == 0));
        CHECK ((got[2].on && got[2].note == 64));
        queue.noteOff (60); // releases nothing new
        rig.render (128, { noteOff (128, 64) }, 128);
        CHECK (rig.allReleased (0));
    }
    SECTION ("a host block longer than the prepared size: added at the start of its first chunk, once")
    {
        // The same block with the host's note at offset 0 instead: bit-identical output. Added in a
        // later chunk the note would start 128 samples late; added twice it would retrigger.
        REQUIRE (queue.noteOn (60, 100));
        juce::AudioBuffer<float> editorBlock (2, 4 * 128);
        juce::MidiBuffer none;
        rig.proc.processBlock (editorBlock, none);
        for (const auto& e : rig.proc.getLastPartEvents (0))
            CHECK (e.type != EventType::NoteOn); // the last chunk got nothing

        Rig host (48000.0, 128);
        host.prepare();
        juce::AudioBuffer<float> hostBlock (2, 4 * 128);
        juce::MidiBuffer hostMidi;
        hostMidi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 0);
        host.proc.processBlock (hostBlock, hostMidi);
        CHECK (editorBlock.getMagnitude (0, 0, 128) > 0.0f);
        int differing = 0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 4 * 128; ++i)
                differing += std::memcmp (editorBlock.getReadPointer (ch) + i, hostBlock.getReadPointer (ch) + i, sizeof (float)) != 0 ? 1 : 0;
        CHECK (differing == 0);
    }
    SECTION ("a zero-length block applies them")
    {
        REQUIRE (queue.noteOn (60, 100));
        juce::AudioBuffer<float> empty (2, 0);
        juce::MidiBuffer none;
        rig.proc.processBlock (empty, none);
        CHECK (rig.proc.getLowerPart().isSounding());
    }
    SECTION ("bypassed blocks drain and drop them")
    {
        REQUIRE (queue.noteOn (60, 100));
        rig.bypass = true;
        rig.render (512, {}, 128);
        rig.bypass = false;
        rig.render (512, {}, 128);
        CHECK (rig.countType (0, EventType::NoteOn) == 0);
        CHECK_FALSE (rig.proc.getLowerPart().isSounding());
    }
    SECTION ("reset empties the queue")
    {
        REQUIRE (queue.noteOn (60, 100));
        rig.hostReset();
        rig.render (256, {}, 128);
        CHECK (rig.countType (0, EventType::NoteOn) == 0);
    }
    SECTION ("prepareToPlay empties the queue")
    {
        REQUIRE (queue.noteOn (60, 100));
        rig.prepare();
        rig.render (256, {}, 128);
        CHECK (rig.countType (0, EventType::NoteOn) == 0);
    }
}

TEST_CASE ("Play keys: a page switch and back during one press ends that press; the next drag plays nothing", "[ui][m7c][play][keys]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    PlayRig rig;
    auto& strip = rig.strip();
    SECTION ("page switch (cancel)")
    {
        strip.mouseDown (mouseAt (strip, onKey (strip, 60)));
        rig.editor->selectPage (static_cast<int> (ui::VintagePage::Effects), 0);
        rig.editor->selectPage (static_cast<int> (ui::VintagePage::Play), 0);
    }
    SECTION ("the strip hidden and shown again")
    {
        strip.mouseDown (mouseAt (strip, onKey (strip, 60)));
        strip.setVisible (false);
        strip.setVisible (true);
    }
    SECTION ("an ancestor hidden, found by the tick, shown again")
    {
        strip.mouseDown (mouseAt (strip, onKey (strip, 60)));
        rig.editor->getPlayPage().setVisible (false);
        rig.tick();
        rig.editor->getPlayPage().setVisible (true);
    }
    CHECK (strip.playingNote() == -1);
    // The button is still down: dragging over the keys plays nothing until the next press.
    strip.mouseDrag (mouseAt (strip, onKey (strip, 64)));
    CHECK (strip.playingNote() == -1);
    strip.mouseUp (mouseAt (strip, onKey (strip, 64)));
    rig.run (0.05);
    const auto got = notesOf (rig, 0);
    REQUIRE (got.size() == 2);
    CHECK ((got[0].on && got[0].note == 60 && ! got[1].on && got[1].note == 60));
    strip.mouseDown (mouseAt (strip, onKey (strip, 64)));
    CHECK (strip.playingNote() == 64); // a new press plays again
    strip.mouseUp (mouseAt (strip, onKey (strip, 64)));
}

TEST_CASE ("Play keys: the editor's timer releases a held key once the editor is no longer showing (host hid the window)",
           "[ui][m7c][play][keys]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    PlayRig rig;
    auto& strip = rig.strip();
    strip.mouseDown (mouseAt (strip, onKey (strip, 60)));
    rig.run (0.05);
    REQUIRE (rig.proc.getLowerPart().isSounding());
    // The test editor is not on the desktop, so it is never showing: what a hidden host window looks like.
    REQUIRE_FALSE (rig.editor->isShowing());
    rig.editor->releaseKeysIfHidden();
    CHECK (strip.playingNote() == -1);
    rig.run (0.05);
    CHECK (rig.allReleased (0));
    strip.mouseDrag (mouseAt (strip, onKey (strip, 62)));
    CHECK (strip.playingNote() == -1);
}

TEST_CASE ("Play keys: the editor and the host play the same pitch: one voice, retriggered; the first off releases it; nothing hangs",
           "[ui][m7c][play][keys]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    PlayRig rig;
    auto& strip = rig.strip();
    strip.mouseDown (mouseAt (strip, onKey (strip, 60)));
    rig.run (0.05);
    // The host plays 60 too: a voice already playing that note is retriggered (Poly, VoiceAllocator).
    rig.render (rig.samples (0.05), { noteOn (static_cast<int> (rig.now), 60, 90) }, 256);
    CHECK (rig.proc.getLowerPart().numSoundingVoices() == 1);
    // The editor lets go: that off releases the shared voice, although the host still holds 60.
    strip.mouseUp (mouseAt (strip, onKey (strip, 60)));
    rig.run (3.0);
    CHECK_FALSE (rig.proc.getLowerPart().isSounding());
    // The host's own off comes later and finds nothing to release.
    const auto tail = rig.render (rig.samples (0.2), { noteOff (static_cast<int> (rig.now), 60) }, 256);
    CHECK_FALSE (rig.proc.getLowerPart().isSounding());
    CHECK (std::fpclassify (peak (tail)) == FP_ZERO);
    // What the part received: both ons, and one off (the router forwards one off per held key; the
    // host's later off finds the key already released and is not passed on).
    CHECK (rig.countType (0, EventType::NoteOn) == 2);
    CHECK (rig.countType (0, EventType::NoteOff) == 1);
}

TEST_CASE ("Play keys: the editor's notes feed the arpeggiator and obey the hold pedal", "[ui][m7c][play][keys]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    PlayRig rig;
    auto& strip = rig.strip();
    SECTION ("arpeggiator")
    {
        rig.set ("arp.mode", 1.0f); // Arpeggiator, on the Lower part
        rig.tick();
        strip.mouseDown (mouseAt (strip, onKey (strip, 60)));
        rig.run (1.0);
        const int steps = rig.countType (0, EventType::NoteOn);
        CHECK (steps >= 3); // the held key repeats at the arpeggiator's rate
        strip.mouseUp (mouseAt (strip, onKey (strip, 60)));
        rig.run (0.5);
        const long long released = rig.now;
        rig.run (1.0);
        CHECK (rig.countType (0, EventType::NoteOn, released) == 0); // stopped with the key
        rig.run (3.0);
        CHECK_FALSE (rig.proc.getLowerPart().isSounding());
    }
    SECTION ("hold pedal")
    {
        rig.render (256, { pedal (static_cast<int> (rig.now), true) }, 256);
        strip.mouseDown (mouseAt (strip, onKey (strip, 60)));
        rig.run (0.05);
        strip.mouseUp (mouseAt (strip, onKey (strip, 60)));
        rig.run (3.0);
        CHECK (rig.proc.getLowerPart().isSounding()); // held by the pedal
        rig.render (256, { pedal (static_cast<int> (rig.now), false) }, 256);
        rig.run (3.0);
        CHECK_FALSE (rig.proc.getLowerPart().isSounding());
    }
}

TEST_CASE ("Play keys: a pressed key is named to screen readers with the part that plays it, once per new note", "[ui][play][keys][a11y]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    PlayRig rig;
    auto& strip = rig.strip();
    std::vector<juce::String> heard;
    strip.announce = [&] (const juce::String& s) { heard.push_back (s); };

    rig.set ("key.mode", 2.0f); // Split at C4 (60)
    rig.set ("key.split_point", 60.0f);
    rig.tick();
    strip.mouseDown (mouseAt (strip, onKey (strip, 48)));
    strip.mouseDrag (mouseAt (strip, onKey (strip, 48))); // the same key: nothing new
    strip.mouseDrag (mouseAt (strip, onKey (strip, 52))); // keys crossed on the way are not announced
    strip.mouseDrag (mouseAt (strip, onKey (strip, 62)));
    CHECK (heard.size() == 1);
    strip.mouseDrag (mouseAt (strip, onKey (strip, 67)));
    CHECK (heard.size() == 1);
    strip.mouseUp (mouseAt (strip, onKey (strip, 67)));
    REQUIRE (heard.size() == 2); // the press, and the note where the glide ends
    CHECK (heard[0] == "C3, Lower");
    CHECK (heard[1] == "G4, Upper");

    // Pressing and releasing on the same note says it once; a glide that comes back to its first key says nothing more.
    strip.mouseDown (mouseAt (strip, onKey (strip, 50)));
    strip.mouseDrag (mouseAt (strip, onKey (strip, 53)));
    strip.mouseDrag (mouseAt (strip, onKey (strip, 50)));
    strip.mouseUp (mouseAt (strip, onKey (strip, 50)));
    CHECK (heard.size() == 3);
    CHECK (heard[2] == "D3, Lower");

    rig.set ("key.mode", 1.0f); // Dual: both parts
    rig.tick();
    strip.mouseDown (mouseAt (strip, onKey (strip, 72)));
    strip.mouseUp (mouseAt (strip, onKey (strip, 72)));
    REQUIRE (heard.size() == 4);
    CHECK (heard[3] == "C5, Lower and Upper");
    CHECK (strip.lastAnnouncement() == heard[3]);
}
