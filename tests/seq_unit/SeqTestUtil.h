#pragma once

// Helpers shared by the arpeggiator / sequencer tests: event builders, a block driver that feeds
// a module with an absolute-time event list under arbitrary block sizes, and an exact integer
// model of the step grid (independent of the module's own arithmetic).

#include "seq/ArpSeq.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace vs::seq::test
{
using vs::EventType;
using vs::NoteEvent;

inline NoteEvent noteOn (int offset, int note, float vel = 0.8f)
{
    return NoteEvent { offset, EventType::NoteOn, static_cast<std::uint8_t> (note), vel };
}
inline NoteEvent noteOff (int offset, int note)
{
    return NoteEvent { offset, EventType::NoteOff, static_cast<std::uint8_t> (note), 0.0f };
}
inline NoteEvent ctrl (int offset, EventType type, float value = 0.0f)
{
    return NoteEvent { offset, type, 0, value };
}
inline NoteEvent pedal (int offset, bool down) { return ctrl (offset, EventType::SustainPedal, down ? 1.0f : 0.0f); }

inline bool sameEvent (const NoteEvent& a, const NoteEvent& b)
{
    std::uint32_t va = 0, vb = 0;
    std::memcpy (&va, &a.value, sizeof va);
    std::memcpy (&vb, &b.value, sizeof vb);
    return a.offset == b.offset && a.type == b.type && a.note == b.note && va == vb;
}

inline bool sameEvents (const std::vector<NoteEvent>& a, const std::vector<NoteEvent>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (! sameEvent (a[i], b[i]))
            return false;
    return true;
}

/** Only the note-ons and note-offs of a list. */
inline std::vector<NoteEvent> notesOnly (const std::vector<NoteEvent>& v)
{
    std::vector<NoteEvent> r;
    for (const auto& e : v)
        if (e.type == EventType::NoteOn || e.type == EventType::NoteOff)
            r.push_back (e);
    return r;
}

// ---- Driver -----------------------------------------------------------------------------------

/** Host transport from sample `start` on. The ppq at a block start is
    anchorTicks / kTick + (sample - start) * bpm / (60 * sr); one tick is 1/kTick of a quarter note
    (kTick = 6144 * 6400), the unit in which every grid position of every rate and every swing and
    gate that is a multiple of 1/80 is an integer. */
/** Ticks per quarter note of the exact model. */
inline constexpr std::int64_t kTick = 6144 * 6400;

struct HostSeg
{
    std::int64_t start = 0;
    bool playing = true;
    std::int64_t bpm100 = 12000; // bpm * 100
    std::int64_t anchorTicks = 0;
};

struct Change
{
    std::int64_t at;
    Settings s;
};

struct Run
{
    int sr = 48000;
    std::int64_t total = 0;
    int block = 64;
    int capacity = ArpSeq::kRecommendedCapacity;
    std::vector<HostSeg> segs { HostSeg {} };
    std::vector<Change> changes;        // sorted by time; the first one at 0
    std::vector<NoteEvent> input;       // absolute offsets, ascending
};

struct RunResult
{
    std::vector<NoteEvent> events; // absolute offsets
    bool wellFormed = true;        // per block: ascending, within [0, n - 1]
};

inline RunResult run (ArpSeq& m, const Run& r)
{
    RunResult res;
    std::set<std::int64_t> bounds { r.total };
    for (const auto& s : r.segs)
        bounds.insert (s.start);
    for (const auto& c : r.changes)
        bounds.insert (c.at);

    std::vector<NoteEvent> outBuf (static_cast<std::size_t> (r.capacity));
    std::size_t nextChange = 0;
    std::size_t inIdx = 0;
    std::vector<NoteEvent> blockIn;
    for (std::int64_t s = 0; s < r.total;)
    {
        while (nextChange < r.changes.size() && r.changes[nextChange].at <= s)
            m.setSettings (r.changes[nextChange++].s);
        const std::int64_t nextBound = *bounds.upper_bound (s);
        const std::int64_t end = std::min<std::int64_t> (s + r.block, nextBound);
        const int n = static_cast<int> (end - s);

        const HostSeg* seg = &r.segs[0];
        for (const auto& sg : r.segs)
            if (sg.start <= s)
                seg = &sg;
        Transport tr {};
        tr.sampleRate = static_cast<double> (r.sr);
        tr.playing = seg->playing;
        tr.bpm = static_cast<double> (seg->bpm100) / 100.0;
        tr.ppqPosition = static_cast<double> (seg->anchorTicks) / static_cast<double> (kTick)
                         + static_cast<double> (s - seg->start) * tr.bpm / (60.0 * static_cast<double> (r.sr));

        blockIn.clear();
        while (inIdx < r.input.size() && r.input[inIdx].offset < end)
        {
            NoteEvent e = r.input[inIdx++];
            e.offset = static_cast<int> (e.offset - s);
            blockIn.push_back (e);
        }
        const int count = m.process (blockIn, tr, n, outBuf);
        int prev = 0;
        for (int i = 0; i < count; ++i)
        {
            const NoteEvent& e = outBuf[static_cast<std::size_t> (i)];
            if (e.offset < prev || e.offset < 0 || e.offset >= n)
                res.wellFormed = false;
            prev = e.offset;
            NoteEvent a = e;
            a.offset = static_cast<int> (e.offset + s);
            res.events.push_back (a);
        }
        s = end;
    }
    return res;
}

// ---- Exact model of the step grid ----------------------------------------------------------------

inline std::int64_t floorDiv (std::int64_t a, std::int64_t b) // b > 0
{
    std::int64_t q = a / b;
    if ((a % b) != 0 && a < 0)
        --q;
    return q;
}
inline std::int64_t ceilDiv (std::int64_t a, std::int64_t b) { return -floorDiv (-a, b); }

struct MCfg
{
    int sr = 48000;
    int d = 4;     // steps per quarter note: 1 2 3 4 6 8
    int swing80 = 40; // swing = swing80 / 80 (40 = 50 % .. 60 = 75 %)
    int gate80 = 40;  // gate = gate80 / 80

    /** Start of step k in ticks (1 tick = 1/kTick quarter): pair start + swing * pair for odd k. */
    std::int64_t pos (std::int64_t k) const
    {
        const std::int64_t step = kTick / d;
        std::int64_t p = step * k;
        if ((k & 1) != 0)
            p += (step / 80) * (2 * swing80 - 80);
        return p;
    }
    std::int64_t gateEnd (std::int64_t k) const
    {
        const std::int64_t p = pos (k);
        return p + (gate80 * (pos (k + 1) - p)) / 80;
    }
};

/** A stretch of samples with one tempo and a known ppq anchor; `jump` = the host position is not
    the continuation of the previous stretch. */
struct MSeg
{
    std::int64_t start;
    std::int64_t len;
    std::int64_t bpm100;
    std::int64_t anchorTicks;
    bool jump;
};

struct MStep
{
    StepType kind = StepType::Note;
    int note = 60;
};

struct MEvent
{
    std::int64_t sample;
    bool on;
    int note;
};

/** Samples (relative to the segment start) of tick position p: ceil((p - anchor) * 60 * sr / (kTick * bpm)). */
inline std::int64_t relSample (const MCfg& c, const MSeg& s, std::int64_t ticks)
{
    return ceilDiv ((ticks - s.anchorTicks) * 6000 * c.sr, kTick * s.bpm100);
}

/** What a correct module emits (note-ons and note-offs, in output order) for a pattern of steps
    that cycles from the first step fired, with the pattern active from sample `activation`.

    The rules below are contract decisions, stated here once as the spec:
      - an event at musical time T happens at the first sample at or after T (a step that is already
        overdue, after a jump or a late activation, happens at the segment's first sample);
      - the first step is the first one at or after the activation sample;
      - at a jump (`MSeg::jump`) or a source change what sounds is released at the segment's first sample;
      - at a step, what is still sounding is released first (before the new note-on), except legato:
        gate = 100 % and a different pitch next puts the new note-on first;
      - a Tie extends a sounding note, the gate applying to the last tied step.
    Nothing here limits the spacing of steps or the length of a note: the tests use tempi where steps
    are thousands of samples apart (implementation guards such as "a note lasts at least one sample"
    never matter and are not modelled). */
inline std::vector<MEvent> model (const MCfg& c, const std::vector<MSeg>& segs, std::int64_t activation,
                                  const std::vector<MStep>& pattern, std::int64_t& stepsFired)
{
    std::vector<MEvent> out;
    std::int64_t count = 0; // steps fired (position in the pattern cycle)
    std::int64_t nextK = 0;
    bool pending = false, open = false;
    int pendingNote = 0;
    std::int64_t pendingEnd = 0;

    for (std::size_t i = 0; i < segs.size(); ++i)
    {
        const MSeg& sg = segs[i];
        if (i == 0 || sg.jump)
        {
            if (pending && i > 0)
                out.push_back ({ sg.start, false, pendingNote });
            if (i > 0)
                pending = open = false;
            // First step whose sample is at or after max(segment start, activation).
            const std::int64_t minRel = std::max<std::int64_t> (activation - sg.start, 0);
            nextK = floorDiv (sg.anchorTicks * c.d, kTick) - 4;
            while (relSample (c, sg, c.pos (nextK)) < minRel)
                ++nextK;
        }
        for (;;)
        {
            const std::int64_t zero = 0;
            const std::int64_t stepAbs = sg.start + std::max (relSample (c, sg, c.pos (nextK)), zero);
            std::int64_t offAbs = INT64_MAX;
            if (pending && ! open)
                offAbs = sg.start + std::max (relSample (c, sg, pendingEnd), zero);
            const std::int64_t endAbs = sg.start + sg.len;
            // Spec (legato): with gate = 100 % a note that ends exactly where the next step starts is
            // released by that step, after the next note-on if the pitch differs (handled at the step).
            const bool legatoHold = c.gate80 >= 80 && offAbs == stepAbs;
            if (offAbs < endAbs && offAbs <= stepAbs && ! legatoHold)
            {
                out.push_back ({ offAbs, false, pendingNote });
                pending = false;
                continue;
            }
            if (stepAbs >= endAbs)
                break;
            // Fire the step.
            const MStep& st = pattern[static_cast<std::size_t> (count % static_cast<std::int64_t> (pattern.size()))];
            const bool tieFollows = pattern[static_cast<std::size_t> ((count + 1) % static_cast<std::int64_t> (pattern.size()))].kind == StepType::Tie;
            ++count;
            const std::int64_t k = nextK++;
            if (st.kind == StepType::Tie && pending && open)
            {
                open = tieFollows;
                pendingEnd = c.gateEnd (k);
                continue;
            }
            const bool overlap = st.kind == StepType::Note && pending && ! open && c.gate80 >= 80 && st.note != pendingNote;
            if (overlap)
            {
                out.push_back ({ stepAbs, true, st.note });
                out.push_back ({ stepAbs, false, pendingNote });
                pending = false;
            }
            else if (pending)
            {
                out.push_back ({ stepAbs, false, pendingNote });
                pending = false;
            }
            if (st.kind == StepType::Note)
            {
                if (! overlap)
                    out.push_back ({ stepAbs, true, st.note });
                pending = true;
                open = tieFollows;
                pendingNote = st.note;
                pendingEnd = c.gateEnd (k);
            }
        }
    }
    stepsFired = count;
    return out;
}

/** Compare the module's note events (absolute offsets) with the model, to the sample. */
inline bool matchesModel (const std::vector<NoteEvent>& actual, const std::vector<MEvent>& expected, std::int64_t total, std::string* why = nullptr)
{
    std::vector<MEvent> exp;
    for (const auto& e : expected)
        if (e.sample < total)
            exp.push_back (e);
    const auto notes = notesOnly (actual);
    const auto fail = [&] (const std::string& s) {
        if (why)
            *why = s;
        return false;
    };
    if (notes.size() != exp.size())
        return fail ("count " + std::to_string (notes.size()) + " vs expected " + std::to_string (exp.size()));
    for (std::size_t i = 0; i < exp.size(); ++i)
    {
        const bool on = notes[i].type == EventType::NoteOn;
        if (notes[i].offset != exp[i].sample || on != exp[i].on || notes[i].note != exp[i].note)
            return fail ("event " + std::to_string (i) + ": got " + (on ? "on " : "off ") + std::to_string (notes[i].note) + " @"
                         + std::to_string (notes[i].offset) + ", expected " + (exp[i].on ? "on " : "off ") + std::to_string (exp[i].note) + " @"
                         + std::to_string (exp[i].sample));
    }
    return true;
}

// ---- Event stream checks -------------------------------------------------------------------------

/** Walks a note event stream and tracks what sounds. By default a panic event does not count as
    releasing anything: the module must emit its own note-offs. */
struct SoundLedger
{
    bool panicClears = false;
    std::set<int> sounding;
    int doubleOns = 0;   // note-on for a note that already sounds
    int orphanOffs = 0;  // note-off for a note that does not sound
    int ons = 0;
    int offs = 0;

    void feed (const NoteEvent& e)
    {
        if (e.type == EventType::NoteOn)
        {
            ++ons;
            if (! sounding.insert (e.note).second)
                ++doubleOns;
        }
        else if (e.type == EventType::NoteOff)
        {
            ++offs;
            if (sounding.erase (e.note) == 0)
                ++orphanOffs;
        }
        else if (panicClears && (e.type == EventType::AllNotesOff || e.type == EventType::AllSoundOff))
            sounding.clear();
    }
};

/** What a polyphonic part does with the events it receives (enough to see hanging notes and the
    sustain pedal's deferral of note-offs). */
struct PartModel
{
    std::set<int> sounding;
    std::set<int> heldByPedal;
    bool pedalDown = false;

    void feed (const NoteEvent& e)
    {
        switch (e.type)
        {
            case EventType::NoteOn:
                sounding.insert (e.note);
                heldByPedal.erase (e.note);
                break;
            case EventType::NoteOff:
                if (pedalDown)
                    heldByPedal.insert (e.note);
                else
                    sounding.erase (e.note);
                break;
            case EventType::SustainPedal:
                setPedal (e.value >= 0.5f);
                break;
            case EventType::AllNotesOff:
            case EventType::AllSoundOff:
                sounding.clear();
                heldByPedal.clear();
                pedalDown = false;
                break;
            case EventType::ResetControllers:
                setPedal (false);
                break;
            case EventType::PitchBend:
            case EventType::ModWheel:
                break;
        }
    }

private:
    void setPedal (bool down)
    {
        pedalDown = down;
        if (! down)
        {
            for (int n : heldByPedal)
                sounding.erase (n);
            heldByPedal.clear();
        }
    }
};

/** Checks an output stream event by event, remembering where each sounding note came from:
    `gen` = started by the module (mode on), `user` = passed through while Off. A user panic in
    Off must not be able to hide a note the module still owes. */
struct StreamChecker
{
    std::set<int> gen, user;
    PartModel part;
    std::string firstError;
    std::int64_t noteOns = 0, orphanOffsOff = 0;

    void fail (const std::string& s)
    {
        if (firstError.empty())
            firstError = s;
    }

    /** `mode`: the module's mode while this event was produced. `blockStart`: the event was written
        at offset 0 of a call (a pedal sync is allowed there). `markerPossible`: the call may have
        closed with an overflow marker (small output buffer), which clears everything downstream. */
    void feed (const NoteEvent& e, Mode mode, bool blockStart, bool markerPossible = false)
    {
        const std::string where = " (note " + std::to_string (e.note) + " @" + std::to_string (e.offset) + ")";
        switch (e.type)
        {
            case EventType::NoteOn:
                ++noteOns;
                if (e.note > 127 && mode != Mode::Off) // Off passes the player's malformed events on as they are
                    fail ("generated note out of range" + where);
                if (mode != Mode::Off && ! (e.value > 0.0f && e.value <= 1.0f))
                    fail ("generated velocity outside (0, 1]" + where);
                // The player's own repeated note-ons while Off are the player's business; the module must never
                // start a pitch the part holds because of it, nor let a player's note-on double one of its own.
                if (gen.count (e.note) > 0 || (mode != Mode::Off && user.count (e.note) > 0))
                    fail ("note-on for a pitch the part already holds" + where);
                if (mode == Mode::Off)
                {
                    if (e.note < 128) // note numbers the part cannot play are not tracked
                        user.insert (e.note);
                }
                else
                {
                    gen.insert (e.note);
                    if (part.pedalDown && ! markerPossible)
                        fail ("generated note-on while the part's sustain pedal is down" + where);
                }
                break;
            case EventType::NoteOff:
                if (gen.erase (e.note) == 0 && user.erase (e.note) == 0)
                {
                    if (mode != Mode::Off)
                        fail ("note-off without note-on outside Off" + where);
                    ++orphanOffsOff;
                }
                break;
            case EventType::AllNotesOff:
            case EventType::AllSoundOff:
                if (mode != Mode::Off && ! gen.empty() && ! markerPossible)
                    fail ("panic passed on while the module still owed note-offs" + where);
                if (mode == Mode::Off && ! gen.empty() && ! markerPossible)
                    fail ("user panic in Off while the module still owed note-offs" + where);
                if (mode != Mode::Off && ! user.empty() && ! markerPossible)
                    fail ("panic passed on while passed-through notes were never released" + where);
                gen.clear();
                user.clear();
                break;
            case EventType::SustainPedal:
                if (mode != Mode::Off && ! (e.value < 0.5f && blockStart))
                    fail ("sustain pedal passed on while the module is on" + where);
                break;
            case EventType::PitchBend:
            case EventType::ModWheel:
            case EventType::ResetControllers:
                break;
        }
        part.feed (e);
    }

    /** After the final panic with everything released. */
    bool clean() const { return gen.empty() && user.empty() && part.sounding.empty() && ! part.pedalDown && firstError.empty(); }
};
} // namespace vs::seq::test
