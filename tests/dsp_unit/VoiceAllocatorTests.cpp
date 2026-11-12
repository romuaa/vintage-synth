// VoiceAllocator: every behaviour row of docs/architecture.md 3.2 at decision level (no audio).
#include <catch2/catch_test_macros.hpp>

#include "common/engine/VoiceAllocator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

namespace
{
using vs::AssignMode;
using vs::VoiceAction;
using Type = VoiceAction::Type;
constexpr int kNoGlide = VoiceAction::kNoGlide;
constexpr int kFromCurrent = VoiceAction::kGlideFromCurrent;

/** Allocator plus a log of every action, and a model of which voices are sounding. */
struct Harness
{
    vs::VoiceAllocator alloc;
    std::vector<VoiceAction> log;

    explicit Harness (AssignMode mode = AssignMode::Poly, int unison = 4)
    {
        alloc.initMode (mode, unison);
    }

    auto sink()
    {
        return [this] (const VoiceAction& a) { log.push_back (a); };
    }

    std::vector<VoiceAction> on (int note, float vel = 0.8f)
    {
        log.clear();
        alloc.noteOn (note, vel, sink());
        return log;
    }
    std::vector<VoiceAction> off (int note)
    {
        log.clear();
        alloc.noteOff (note, sink());
        return log;
    }
    std::vector<VoiceAction> pedal (bool down)
    {
        log.clear();
        alloc.setSustain (down, sink());
        return log;
    }
    std::vector<VoiceAction> mode (AssignMode m, int unison = 4)
    {
        log.clear();
        alloc.setMode (m, unison, sink());
        return log;
    }
    std::vector<VoiceAction> allOff()
    {
        log.clear();
        alloc.allNotesOff (sink());
        return log;
    }

    /** The single voice an action list started (Start or Steal). */
    static int startedVoice (const std::vector<VoiceAction>& a)
    {
        REQUIRE (a.size() == 1);
        REQUIRE ((a[0].type == Type::Start || a[0].type == Type::Steal));
        return a[0].voice;
    }
};

std::vector<int> voicesOf (const std::vector<VoiceAction>& a, Type t)
{
    std::vector<int> v;
    for (const auto& x : a)
        if (x.type == t)
            v.push_back (x.voice);
    return v;
}
} // namespace

// ---- Poly ------------------------------------------------------------------------------------

TEST_CASE ("Allocator Poly: 16 notes get 16 different voices, starting from voice 0", "[alloc][poly]")
{
    Harness h;
    std::set<int> used;
    for (int i = 0; i < vs::kMaxVoices; ++i)
    {
        const auto a = h.on (48 + i);
        REQUIRE (a.size() == 1);
        CHECK (a[0].type == Type::Start);
        CHECK (a[0].note == 48 + i);
        CHECK (std::abs (a[0].velocity - 0.8f) < 1.0e-7f);
        used.insert (a[0].voice);
        if (i == 0)
            CHECK (a[0].voice == 0); // all idle equally long: lowest index (deterministic)
    }
    CHECK (used.size() == static_cast<size_t> (vs::kMaxVoices));
}

TEST_CASE ("Allocator Poly: a note already sounding retriggers its own voice (held, pedal-held or releasing)", "[alloc][poly]")
{
    Harness h;
    const int v60 = Harness::startedVoice (h.on (60));
    h.on (64);

    SECTION ("held: double note-on")
    {
        const auto a = h.on (60, 0.5f);
        CHECK (Harness::startedVoice (a) == v60);
        CHECK (a[0].type == Type::Start);
        CHECK (std::abs (a[0].velocity - 0.5f) < 1.0e-7f);
    }
    SECTION ("releasing")
    {
        CHECK (voicesOf (h.off (60), Type::Release) == std::vector<int> { v60 });
        CHECK (Harness::startedVoice (h.on (60)) == v60);
        CHECK_FALSE (h.alloc.isVoiceReleased (v60));
    }
    SECTION ("held by the pedal")
    {
        h.pedal (true);
        CHECK (h.off (60).empty());
        CHECK (Harness::startedVoice (h.on (60)) == v60);
        // Its key is down again: pedal up must not release it.
        CHECK (h.pedal (false).empty());
    }
}

TEST_CASE ("Allocator Poly: a new note takes the voice that has been idle longest", "[alloc][poly]")
{
    Harness h;
    for (int i = 0; i < 4; ++i)
        h.on (60 + i); // voices 0..3
    for (int i = 0; i < 4; ++i)
        h.off (60 + i);
    // Voices finish their release in the order 2, 0, 3, 1.
    for (const int v : { 2, 0, 3, 1 })
        h.alloc.voiceIdle (v);

    // Voices 4..15 have never played: idle since the start, so longer than 0..3.
    for (int v = 4; v < vs::kMaxVoices; ++v)
        CHECK (Harness::startedVoice (h.on (70 + v)) == v);
    // Then the order in which 0..3 went idle.
    for (const int v : { 2, 0, 3, 1 })
        CHECK (Harness::startedVoice (h.on (100 + v)) == v);
}

TEST_CASE ("Allocator Poly: a busy voice in release is not reused while another voice is idle", "[alloc][poly]")
{
    Harness h;
    h.on (60);
    h.off (60); // voice 0 releasing, not reported idle
    CHECK (Harness::startedVoice (h.on (62)) == 1);
}

TEST_CASE ("Allocator Poly: steal the voice that entered its release first", "[alloc][poly][steal]")
{
    Harness h;
    for (int i = 0; i < vs::kMaxVoices; ++i)
        h.on (40 + i); // voice i plays 40 + i
    // Release three notes, the newest-started first: release order 15, 7, 3.
    h.off (55);
    h.off (47);
    h.off (43);

    auto a = h.on (80);
    REQUIRE (a.size() == 1);
    CHECK (a[0].type == Type::Steal);
    CHECK (a[0].voice == 15); // first into release (not the oldest-started voice 3)
    CHECK (a[0].note == 80);
    CHECK (Harness::startedVoice (h.on (81)) == 7);
    CHECK (Harness::startedVoice (h.on (82)) == 3);
}

TEST_CASE ("Allocator Poly: with no voice in release, steal the oldest held voice, never the newest", "[alloc][poly][steal]")
{
    Harness h;
    for (int i = 0; i < vs::kMaxVoices; ++i)
        h.on (40 + i);
    // Retrigger voice 0's note: voice 0 is now the most recently started, voice 1 the oldest.
    CHECK (Harness::startedVoice (h.on (40)) == 0);

    auto a = h.on (80);
    CHECK (a[0].type == Type::Steal);
    CHECK (a[0].voice == 1);
    // The voice just stolen (now the newest, its fade still running) is protected; next is voice 2.
    a = h.on (81);
    CHECK (a[0].type == Type::Steal);
    CHECK (a[0].voice == 2);
    CHECK (h.alloc.getVoiceNote (1) == 80);
    CHECK (h.alloc.getVoiceNote (0) == 40); // the re-pressed note survived
}

TEST_CASE ("Allocator Poly: key release releases the voice; a stolen note's key-up does nothing", "[alloc][poly]")
{
    Harness h;
    for (int i = 0; i < vs::kMaxVoices; ++i)
        h.on (40 + i);
    h.on (80); // steals voice 0 (note 40)
    CHECK (h.off (40).empty()); // note 40 no longer has a voice
    CHECK (voicesOf (h.off (80), Type::Release) == std::vector<int> { 0 });
    CHECK (h.off (80).empty()); // second key-up: nothing
}

// ---- Unison and Solo -------------------------------------------------------------------------

TEST_CASE ("Allocator Unison: n voices play the note; last-note priority; return without retrigger", "[alloc][unison]")
{
    for (const int n : { 2, 4, 8, 16 })
    {
        Harness h (AssignMode::Unison, n);
        CHECK (h.alloc.groupSize() == n);
        auto a = h.on (60);
        REQUIRE (a.size() == static_cast<size_t> (n));
        for (int v = 0; v < n; ++v)
        {
            CHECK (a[static_cast<size_t> (v)].type == Type::Start);
            CHECK (a[static_cast<size_t> (v)].voice == v);
            CHECK (a[static_cast<size_t> (v)].note == 60);
        }

        a = h.on (67); // new note retriggers all n
        CHECK (voicesOf (a, Type::Start).size() == static_cast<size_t> (n));

        a = h.off (67); // back to 60: retarget, no retrigger
        REQUIRE (a.size() == static_cast<size_t> (n));
        for (const auto& x : a)
        {
            CHECK (x.type == Type::Retarget);
            CHECK (x.note == 60);
        }
        CHECK (h.off (99).empty()); // not held
        a = h.off (60);
        CHECK (voicesOf (a, Type::Release).size() == static_cast<size_t> (n));
    }
}

TEST_CASE ("Allocator Unison: the count is normalised to 2/4/8/16", "[alloc][unison]")
{
    vs::VoiceAllocator a;
    a.initMode (AssignMode::Unison, 3);
    CHECK (a.getUnisonCount() == 4);
    a.initMode (AssignMode::Unison, 100);
    CHECK (a.getUnisonCount() == 16);
    a.initMode (AssignMode::Unison, 0);
    CHECK (a.getUnisonCount() == 2);
}

TEST_CASE ("Allocator Solo: voice 0 only; a new note retriggers; releasing returns to the held note without retrigger", "[alloc][solo]")
{
    Harness h (AssignMode::Solo);
    auto a = h.on (60);
    REQUIRE (a.size() == 1);
    CHECK ((a[0].type == Type::Start && a[0].voice == 0));

    a = h.on (64);
    REQUIRE (a.size() == 1);
    CHECK ((a[0].type == Type::Start && a[0].voice == 0 && a[0].note == 64)); // retrigger

    a = h.on (67);
    a = h.off (64); // not the sounding note: nothing happens
    CHECK (a.empty());
    a = h.off (67); // sounding note released: back to the most recent held key (60)
    REQUIRE (a.size() == 1);
    CHECK ((a[0].type == Type::Retarget && a[0].note == 60));
    a = h.off (60);
    REQUIRE (a.size() == 1);
    CHECK (a[0].type == Type::Release);
}

// ---- Mode changes ----------------------------------------------------------------------------

TEST_CASE ("Allocator: changing the mode or the unison count releases the sounding voices", "[alloc][mode]")
{
    Harness h;
    h.on (60);
    h.on (64);
    h.on (67);
    auto a = h.mode (AssignMode::Solo);
    CHECK (voicesOf (a, Type::Release) == std::vector<int> { 0, 1, 2 });
    CHECK (a.size() == 3);
    // Keys are forgotten: their note-offs do nothing, and the next note is a fresh Solo note.
    CHECK (h.off (64).empty());
    a = h.on (70);
    CHECK ((a.size() == 1 && a[0].voice == 0 && a[0].type == Type::Start));

    CHECK (h.mode (AssignMode::Solo).empty()); // unchanged: nothing
    CHECK (h.mode (AssignMode::Solo, 16).empty()); // count is irrelevant outside Unison

    a = h.mode (AssignMode::Unison, 4);
    CHECK (voicesOf (a, Type::Release) == std::vector<int> { 0 });
    h.on (50);
    a = h.mode (AssignMode::Unison, 8);
    CHECK (voicesOf (a, Type::Release) == std::vector<int> { 0, 1, 2, 3 });
    CHECK (h.alloc.getUnisonCount() == 8);
}

// ---- Sustain pedal ---------------------------------------------------------------------------

TEST_CASE ("Allocator Poly: the pedal defers key releases until it comes up", "[alloc][pedal]")
{
    Harness h;
    h.on (60);
    h.on (64);
    h.pedal (true);
    CHECK (h.off (60).empty());
    h.on (67);
    CHECK (h.off (67).empty());
    CHECK (h.alloc.isPedalDown());
    // 64 is still physically held: pedal up releases 60 and 67 only.
    const auto a = h.pedal (false);
    CHECK (voicesOf (a, Type::Release) == std::vector<int> { 0, 2 });
    CHECK (voicesOf (h.off (64), Type::Release) == std::vector<int> { 1 });
}

TEST_CASE ("Allocator Solo/Unison: the pedal holds the sounding note; pedal up follows the keys", "[alloc][pedal]")
{
    for (const auto mode : { AssignMode::Solo, AssignMode::Unison })
    {
        Harness h (mode, 2);
        const size_t n = mode == AssignMode::Solo ? 1u : 2u;

        // Hold C, press E, release E with the pedal down: E keeps sounding.
        h.on (60);
        h.on (64);
        h.pedal (true);
        CHECK (h.off (64).empty());
        // Pedal up: E is gone, C is still held -> legato return to C.
        auto a = h.pedal (false);
        REQUIRE (a.size() == n);
        CHECK ((a[0].type == Type::Retarget && a[0].note == 60));

        // Pedal down, release C (the last key): the note keeps sounding until pedal up.
        h.pedal (true);
        CHECK (h.off (60).empty());
        a = h.pedal (false);
        REQUIRE (a.size() == n);
        CHECK (a[0].type == Type::Release);

        // A key pressed again while pedal-held becomes physical again: pedal up does not release it.
        h.on (50);
        h.pedal (true);
        h.off (50);
        h.on (50);
        CHECK (h.pedal (false).empty());
        CHECK (voicesOf (h.off (50), Type::Release).size() == n);
    }
}

TEST_CASE ("Allocator: All Notes Off releases everything and clears the pedal", "[alloc][pedal][panic]")
{
    for (const auto mode : { AssignMode::Poly, AssignMode::Unison, AssignMode::Solo })
    {
        Harness h (mode, 4);
        h.on (60);
        h.on (64);
        h.pedal (true);
        h.off (60);
        const auto a = h.allOff();
        CHECK_FALSE (h.alloc.isPedalDown());
        CHECK (voicesOf (a, Type::Release).size() == static_cast<size_t> (mode == AssignMode::Poly ? 2 : h.alloc.groupSize()));
        CHECK (h.alloc.numPhysicalKeys() == 0);
        CHECK (h.off (64).empty());
        CHECK (h.pedal (false).empty());
    }
}

TEST_CASE ("Allocator: killAll makes every voice idle at once", "[alloc][panic]")
{
    Harness h;
    for (int i = 0; i < 20; ++i)
        h.on (40 + i);
    h.alloc.killAll();
    for (int v = 0; v < vs::kMaxVoices; ++v)
        CHECK_FALSE (h.alloc.isVoiceBusy (v));
    CHECK (h.on (90)[0].type == Type::Start);
}

// ---- Portamento sources ----------------------------------------------------------------------

TEST_CASE ("Allocator: glide sources (Poly from the previous note; Solo/Unison from the sounding pitch; Legato)", "[alloc][porta]")
{
    SECTION ("Poly, Always")
    {
        Harness h;
        CHECK (h.on (60)[0].glideFrom == kNoGlide); // no previous note
        h.off (60);
        CHECK (h.on (67)[0].glideFrom == 60);       // from the previously played note, even if released
        CHECK (h.on (72)[0].glideFrom == 67);
        CHECK (h.on (67)[0].glideFrom == kFromCurrent); // retrigger of a sounding note
    }
    SECTION ("Poly, Legato: only while another key is held")
    {
        Harness h;
        h.alloc.setLegatoGlideOnly (true);
        h.on (60);
        h.off (60);
        CHECK (h.on (67)[0].glideFrom == kNoGlide); // nothing held
        CHECK (h.on (72)[0].glideFrom == 67);       // 67 held
        h.pedal (true);
        h.off (67);
        h.off (72);
        CHECK (h.on (74)[0].glideFrom == kNoGlide); // held only by the pedal: not legato
    }
    SECTION ("Solo, Always")
    {
        Harness h (AssignMode::Solo);
        CHECK (h.on (60)[0].glideFrom == kNoGlide);
        CHECK (h.on (64)[0].glideFrom == kFromCurrent);
        CHECK (h.off (64)[0].glideFrom == kFromCurrent); // retarget to 60 glides
        h.off (60);
        // Voice still busy (releasing): glide from where it is.
        CHECK (h.on (67)[0].glideFrom == kFromCurrent);
        h.off (67);
        h.alloc.voiceIdle (0);
        CHECK (h.on (55)[0].glideFrom == 67); // idle: from the previous note
    }
    SECTION ("Unison, Legato")
    {
        Harness h (AssignMode::Unison, 2);
        h.alloc.setLegatoGlideOnly (true);
        auto a = h.on (60);
        CHECK ((a[0].glideFrom == kNoGlide && a[1].glideFrom == kNoGlide));
        a = h.on (64);
        CHECK ((a[0].glideFrom == kFromCurrent && a[1].glideFrom == kFromCurrent));
        h.off (64);
        h.off (60);
        a = h.on (62); // no key held
        CHECK (a[0].glideFrom == kNoGlide);
    }
}

TEST_CASE ("Allocator: physical-key queries (LFO delay retrigger)", "[alloc]")
{
    Harness h;
    CHECK_FALSE (h.alloc.anyPhysicalKeyExcept (60));
    h.on (60);
    CHECK_FALSE (h.alloc.anyPhysicalKeyExcept (60)); // only itself
    CHECK (h.alloc.anyPhysicalKeyExcept (62));
    h.pedal (true);
    h.off (60);
    CHECK_FALSE (h.alloc.anyPhysicalKeyExcept (62)); // pedal-held notes are not keys
    CHECK (h.alloc.numPhysicalKeys() == 0);
}

TEST_CASE ("Allocator: held-note query for the LFO delay counts pedal-held notes, not releasing ones", "[alloc][lfo]")
{
    for (const auto mode : { AssignMode::Poly, AssignMode::Unison, AssignMode::Solo })
    {
        Harness h (mode, 2);
        CHECK_FALSE (h.alloc.anyNoteHeldExcept (62));
        h.on (60);
        CHECK (h.alloc.anyNoteHeldExcept (62));
        CHECK_FALSE (h.alloc.anyNoteHeldExcept (60)); // only itself
        h.pedal (true);
        h.off (60);
        CHECK (h.alloc.anyNoteHeldExcept (62));       // ringing on the pedal: counts
        CHECK_FALSE (h.alloc.anyPhysicalKeyExcept (62)); // but not for Legato glide
        h.pedal (false);
        CHECK_FALSE (h.alloc.anyNoteHeldExcept (62)); // released (in release): does not count
    }
}

TEST_CASE ("Allocator: out-of-range notes are ignored", "[alloc]")
{
    Harness h;
    CHECK (h.on (-1).empty());
    CHECK (h.on (128).empty());
    CHECK (h.off (200).empty());
}

// ---- Determinism -----------------------------------------------------------------------------

TEST_CASE ("Allocator: the same event sequence gives the same decisions", "[alloc]")
{
    auto run = [] {
        Harness h;
        std::vector<VoiceAction> all;
        std::uint32_t r = 12345u;
        auto next = [&r] { r = r * 1664525u + 1013904223u; return r >> 8; };
        for (int i = 0; i < 5000; ++i)
        {
            const auto k = next() % 100;
            const int note = 36 + static_cast<int> (next() % 40);
            std::vector<VoiceAction> a;
            if (k < 45)
                a = h.on (note, static_cast<float> (next() % 127) / 127.0f);
            else if (k < 85)
                a = h.off (note);
            else if (k < 90)
                a = h.pedal ((next() & 1u) != 0);
            else if (k < 93)
                a = h.mode (static_cast<AssignMode> (next() % 3), 2 << (next() % 4));
            else
            {
                h.alloc.voiceIdle (static_cast<int> (next() % 16)); // the Part reports idle voices
                continue;
            }
            all.insert (all.end(), a.begin(), a.end());
            // Invariants: every action addresses a valid voice of the current group.
            for (const auto& x : a)
            {
                REQUIRE (x.voice >= 0);
                REQUIRE (x.voice < vs::kMaxVoices);
                if (x.type != Type::Release)
                    REQUIRE (x.voice < h.alloc.groupSize());
            }
        }
        return all;
    };
    const auto a = run(), b = run();
    REQUIRE (a.size() == b.size());
    REQUIRE (a.size() > 1000);
    bool same = true;
    for (size_t i = 0; i < a.size(); ++i)
        same = same && a[i].type == b[i].type && a[i].voice == b[i].voice && a[i].note == b[i].note
               && a[i].glideFrom == b[i].glideFrom;
    CHECK (same);
}
