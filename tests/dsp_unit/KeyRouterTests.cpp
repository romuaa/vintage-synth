// KeyRouter (docs/architecture.md 4.2): every row of the key-mode table, note-off ownership,
// controllers and panic events to both parts, mode changes, and random streams that must never
// leave a key held in a part.
#include <catch2/catch_test_macros.hpp>

#include "common/engine/KeyRouter.h"

#include <array>
#include <cmath>
#include <iterator>
#include <random>
#include <set>
#include <vector>

using vs::EventType;
using vs::KeyMode;
using vs::KeyRouter;
using vs::NoteEvent;

namespace
{
NoteEvent on (int pos, int note, float vel = 0.8f) { return { pos, EventType::NoteOn, static_cast<std::uint8_t> (note), vel }; }
NoteEvent off (int pos, int note) { return { pos, EventType::NoteOff, static_cast<std::uint8_t> (note), 0.0f }; }
NoteEvent ctl (int pos, EventType t, float v = 0.0f) { return { pos, t, 0, v }; }

struct Routed
{
    std::vector<NoteEvent> lower, upper;
};

Routed route (KeyRouter& r, const std::vector<NoteEvent>& in)
{
    // Outputs exactly as long as the input (the processor's guarantee), plus a guard element
    // each that must stay untouched.
    std::vector<NoteEvent> lo (in.size() + 1, NoteEvent { -77, EventType::ResetControllers, 0, 0.0f });
    std::vector<NoteEvent> up (in.size() + 1, NoteEvent { -77, EventType::ResetControllers, 0, 0.0f });
    const auto c = r.route (in, std::span<NoteEvent> (lo.data(), in.size()), std::span<NoteEvent> (up.data(), in.size()));
    CHECK (lo.back().offset == -77);
    CHECK (up.back().offset == -77);
    REQUIRE (c.lower <= static_cast<int> (in.size()));
    REQUIRE (c.upper <= static_cast<int> (in.size()));
    return { { lo.begin(), lo.begin() + c.lower }, { up.begin(), up.begin() + c.upper } };
}

bool same (const NoteEvent& a, const NoteEvent& b)
{
    return a.offset == b.offset && a.type == b.type && a.note == b.note && std::abs (a.value - b.value) < 1.0e-9f;
}

bool same (const std::vector<NoteEvent>& a, const std::vector<NoteEvent>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (! same (a[i], b[i]))
            return false;
    return true;
}

KeyRouter router (KeyMode m, int split = 60)
{
    KeyRouter r;
    r.setMode (m);
    r.setSplitPoint (split);
    return r;
}

const std::array<EventType, 6> kSharedTypes { EventType::PitchBend, EventType::ModWheel, EventType::SustainPedal,
                                              EventType::AllNotesOff, EventType::AllSoundOff, EventType::ResetControllers };
} // namespace

TEST_CASE ("KeyRouter Whole: every note to Lower only, nothing to Upper", "[router]")
{
    auto r = router (KeyMode::Whole);
    const std::vector<NoteEvent> in { on (0, 0), on (3, 59), on (3, 60), on (9, 127), off (10, 60), off (12, 0) };
    const auto out = route (r, in);
    CHECK (same (out.lower, in));
    CHECK (out.upper.empty());
    CHECK (r.ownerOf (59) == KeyRouter::kLower);
    CHECK (r.ownerOf (127) == KeyRouter::kLower);
    CHECK (r.ownerOf (60) == 0);
}

TEST_CASE ("KeyRouter Dual: every note-on and note-off to both parts", "[router]")
{
    auto r = router (KeyMode::Dual);
    const std::vector<NoteEvent> in { on (0, 20, 0.3f), on (5, 60, 0.9f), off (7, 20), on (8, 127), off (9, 60) };
    const auto out = route (r, in);
    CHECK (same (out.lower, in));
    CHECK (same (out.upper, in)); // velocity and offsets kept
    CHECK (r.ownerOf (127) == (KeyRouter::kLower | KeyRouter::kUpper));
    CHECK (r.numOwnedKeys() == 1);
}

TEST_CASE ("KeyRouter Split: below the split point to Lower, at or above it to Upper", "[router]")
{
    SECTION ("split 60 (C4): 59 Lower, 60 Upper, extremes")
    {
        auto r = router (KeyMode::Split, 60);
        const auto out = route (r, { on (0, 0), on (1, 59), on (2, 60), on (3, 61), on (4, 127) });
        CHECK (same (out.lower, { on (0, 0), on (1, 59) }));
        CHECK (same (out.upper, { on (2, 60), on (3, 61), on (4, 127) }));
    }
    SECTION ("split 0: everything Upper")
    {
        auto r = router (KeyMode::Split, 0);
        const auto out = route (r, { on (0, 0), on (1, 64), on (2, 127) });
        CHECK (out.lower.empty());
        CHECK (out.upper.size() == 3);
    }
    SECTION ("split 127: only 127 Upper")
    {
        auto r = router (KeyMode::Split, 127);
        const auto out = route (r, { on (0, 0), on (1, 126), on (2, 127) });
        CHECK (same (out.lower, { on (0, 0), on (1, 126) }));
        CHECK (same (out.upper, { on (2, 127) }));
    }
    SECTION ("the split point is clamped to 0..127")
    {
        KeyRouter r;
        r.setSplitPoint (-5);
        CHECK (r.getSplitPoint() == 0);
        r.setSplitPoint (300);
        CHECK (r.getSplitPoint() == 127);
    }
}

TEST_CASE ("KeyRouter: controllers and panic events reach both parts in every mode, in order", "[router]")
{
    for (const auto mode : { KeyMode::Whole, KeyMode::Dual, KeyMode::Split })
        for (const auto t : kSharedTypes)
        {
            INFO ("mode " << static_cast<int> (mode) << ", type " << static_cast<int> (t));
            auto r = router (mode, 60);
            const auto e = ctl (17, t, t == EventType::PitchBend ? -0.25f : 1.0f);
            const auto out = route (r, { on (0, 40), on (1, 80), e, on (30, 41) });
            // The shared event is in both lists, at its position relative to the notes each got.
            auto contains = [&] (const std::vector<NoteEvent>& v) {
                for (const auto& x : v)
                    if (same (x, e))
                        return true;
                return false;
            };
            CHECK (contains (out.lower));
            CHECK (contains (out.upper));
            for (const auto* list : { &out.lower, &out.upper })
                for (size_t i = 1; i < list->size(); ++i)
                    CHECK ((*list)[i - 1].offset <= (*list)[i].offset);
        }
}

TEST_CASE ("KeyRouter: a note-off goes to the part that got the note-on when the split point moved", "[router]")
{
    auto r = router (KeyMode::Split, 60);
    route (r, { on (0, 62), on (0, 50) }); // 62 -> Upper, 50 -> Lower
    r.setSplitPoint (70);                  // 62 would now be Lower
    r.setSplitPoint (40);                  // 50 would now be Upper

    auto out = route (r, { off (5, 62), off (6, 50) });
    CHECK (same (out.lower, { off (6, 50) }));
    CHECK (same (out.upper, { off (5, 62) }));
    CHECK (r.numOwnedKeys() == 0);

    // The same keys pressed again follow the new split point.
    out = route (r, { on (0, 62), on (1, 39), off (2, 62), off (3, 39) });
    CHECK (same (out.upper, { on (0, 62), off (2, 62) })); // split 40: 62 is Upper
    CHECK (same (out.lower, { on (1, 39), off (3, 39) }));
}

TEST_CASE ("KeyRouter: a key pressed again with a different owner is released in both", "[router]")
{
    auto r = router (KeyMode::Split, 60);
    route (r, { on (0, 64) }); // Upper
    r.setSplitPoint (70);
    route (r, { on (0, 64) }); // Lower too (no note-off between: a broken stream)
    CHECK (r.ownerOf (64) == (KeyRouter::kLower | KeyRouter::kUpper));
    const auto out = route (r, { off (1, 64) });
    CHECK (same (out.lower, { off (1, 64) }));
    CHECK (same (out.upper, { off (1, 64) }));
}

TEST_CASE ("KeyRouter: a mode change forgets held keys; later note-offs are dropped", "[router]")
{
    auto r = router (KeyMode::Dual);
    route (r, { on (0, 60), on (0, 64) });
    CHECK_FALSE (r.setMode (KeyMode::Dual)); // unchanged: nothing forgotten
    CHECK (r.numOwnedKeys() == 2);

    CHECK (r.setMode (KeyMode::Split)); // the processor releases every voice of both parts here
    CHECK (r.numOwnedKeys() == 0);
    const auto out = route (r, { off (3, 60), off (4, 64) });
    CHECK (out.lower.empty());
    CHECK (out.upper.empty());

    // The new mode is in effect for the next note.
    const auto out2 = route (r, { on (0, 64) });
    CHECK (out2.lower.empty());
    CHECK (out2.upper.size() == 1);
}

TEST_CASE ("KeyRouter: All Notes Off and All Sound Off forget every key", "[router]")
{
    for (const auto t : { EventType::AllNotesOff, EventType::AllSoundOff })
    {
        auto r = router (KeyMode::Split, 60);
        route (r, { on (0, 30), on (0, 90), ctl (5, t) });
        CHECK (r.numOwnedKeys() == 0);
        const auto out = route (r, { off (0, 30), off (0, 90) });
        CHECK (out.lower.empty());
        CHECK (out.upper.empty());
    }
}

TEST_CASE ("KeyRouter: random event streams never leave a key held in a part", "[router][robust]")
{
    // Model: each part holds the set of keys whose last event *it received* was a note-on; the
    // processor's part-level semantics (All Notes Off / All Sound Off / mode change release
    // everything). The player presses and releases keys at random (sometimes pressing a held key
    // again or releasing one that is up), while the mode and split point change between chunks.
    // At the end the player lets go of every key: no part may still hold one.
    for (std::uint32_t seed = 1; seed <= 200; ++seed)
    {
        std::mt19937 rng (seed);
        auto uni = [&rng] (int a, int b) { return std::uniform_int_distribution<int> (a, b) (rng); };

        KeyRouter r;
        std::set<int> lowerHeld, upperHeld, playerDown;
        auto apply = [&] (const Routed& out) {
            for (const auto* list : { &out.lower, &out.upper })
            {
                auto& held = list == &out.lower ? lowerHeld : upperHeld;
                for (const auto& e : *list)
                {
                    if (e.type == EventType::NoteOn)
                        held.insert (e.note);
                    else if (e.type == EventType::NoteOff)
                        held.erase (e.note);
                    else if (e.type == EventType::AllNotesOff || e.type == EventType::AllSoundOff)
                        held.clear();
                }
            }
        };

        for (int chunk = 0; chunk < 60; ++chunk)
        {
            if (uni (0, 5) == 0 && r.setMode (static_cast<KeyMode> (uni (0, 2))))
            {
                lowerHeld.clear(); // the processor releases every voice of both parts
                upperHeld.clear();
            }
            if (uni (0, 2) == 0)
                r.setSplitPoint (uni (30, 90));

            std::vector<NoteEvent> in;
            const int n = uni (0, 12);
            for (int i = 0; i < n; ++i)
            {
                const int pos = i * 3;
                const int k = uni (0, 20);
                if (k < 9)
                {
                    const int note = uni (30, 90);
                    in.push_back (on (pos, note));
                    playerDown.insert (note);
                }
                else if (k < 17)
                {
                    int note = uni (30, 90);
                    if (! playerDown.empty() && uni (0, 3) != 0)
                        note = *std::next (playerDown.begin(), uni (0, static_cast<int> (playerDown.size()) - 1));
                    in.push_back (off (pos, note));
                    playerDown.erase (note);
                }
                else if (k == 17)
                    in.push_back (ctl (pos, EventType::SustainPedal, 1.0f));
                else if (k == 18)
                    in.push_back (ctl (pos, EventType::PitchBend, 0.5f));
                else if (k == 19 && uni (0, 4) == 0)
                    in.push_back (ctl (pos, EventType::AllNotesOff));
                else
                    in.push_back (ctl (pos, EventType::ModWheel, 0.2f));
            }
            apply (route (r, in));

            // Invariant at every step: a part holds a key only if the router knows it owns it.
            for (const int k : lowerHeld)
                REQUIRE ((r.ownerOf (k) & KeyRouter::kLower) != 0);
            for (const int k : upperHeld)
                REQUIRE ((r.ownerOf (k) & KeyRouter::kUpper) != 0);
        }

        std::vector<NoteEvent> release;
        for (const int note : playerDown)
            release.push_back (off (0, note));
        apply (route (r, release));
        INFO ("seed " << seed);
        CHECK (lowerHeld.empty());
        CHECK (upperHeld.empty());
        CHECK (r.numOwnedKeys() == 0);
    }
}
