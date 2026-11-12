#pragma once

// Arpeggiator / step sequencer front-end: one instance sits in front of one part.
//
//   int process (std::span<const NoteEvent> in, const Transport&, int numSamples, std::span<NoteEvent> out)
//
// One object with a mode (Off / Arp / Seq) because both generators need the same things: the
// clock, the held-key and pedal bookkeeping, the note-off ledger, pass-through of controllers
// and the output capacity rule. Only the choice of the next note differs (Arpeggiator,
// StepSequencer), and switching between the three modes is then one code path that releases what
// sounds. Design notes: docs/architecture.md section 6; the order of events at one sample is
// documented at `process`.
//
// Real time: no allocation, locks, exceptions or unbounded loops; all state is fixed size.
// Call `setSettings` and `process` from the same (audio) thread.
//
// Tempo: a `Transport` describes a whole call (one tempo, one position). The module cannot see
// a tempo change or a loop jump inside a block, and the processor cannot split blocks at tempo
// changes (hosts report tempo once per block). What happens instead: every block re-anchors on the
// position it reports. A deviation from where the previous block ended that is smaller than
// `driftLimitPpq` (Clock.h) is drift (tempo ramp): nothing is released, no step is fired twice
// or skipped (an overdue step fires once at offset 0). A larger deviation, a start/stop, or a
// source change is a jump: what sounds is released at offset 0 and the grid is resynchronised.
//
// Sustain pedal: while the mode is Arp or Seq the module consumes the pedal (it keeps released
// keys in the pattern) and does NOT pass it on, so the part never defers the arpeggio's note-offs.
// Switching Off -> on with the pedal passed on sends a pedal-up first; on -> Off sends the current
// pedal state, so the part is always in step with what the player holds.

#include "Arpeggiator.h"
#include "Clock.h"
#include "KeyState.h"
#include "Settings.h"
#include "StepSequencer.h"
#include "engine/NoteEvent.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <span>

namespace vs::seq
{
class ArpSeq
{
public:
    /** Hard cap on clock steps handled in one call (a guard against absurd block sizes). */
    static constexpr int kMaxStepsPerCall = 2048;

    /** Upper bound on the steps one call of `numSamples` can contain (host tempo <= 1000 bpm,
        rate <= 1/32, plus a step or two for rounding, overdue and swing). */
    static constexpr int maxStepsIn (int numSamples, double sampleRate) noexcept
    {
        return static_cast<int> (std::min (static_cast<double> (kMaxStepsPerCall),
                                           static_cast<double> (numSamples) * kMaxHostBpm * 8.0 / (60.0 * sampleRate) + 3.0));
    }

    /** Output capacity that can never overflow in one call: every input event may be passed on,
        every step writes a note-on and a note-off (a legato overlap writes both of the previous
        note too), plus one note-off carried over from the previous call, a pedal sync, the
        overflow marker, and up to 128 releases when switching Off -> on. With less, the overflow
        rules apply: nothing hangs (owed note-offs are retried in the next call, and an All Notes
        Off marker closes any call that lost a pass-through event), but notes may be skipped. */
    static constexpr int minimumCapacity (int numInputEvents, int stepsInBlock) noexcept
    {
        return numInputEvents + 2 * stepsInBlock + 3 + 128;
    }

    /** A capacity that suffices for blocks of up to 8192 samples at any sample rate up to 192 kHz
        with 2048 input events (see `minimumCapacity`). */
    static constexpr int kRecommendedCapacity = 4096;

    ArpSeq() noexcept { reset(); }

    /** Forget everything (keys, pattern, clock, owed notes, settings stay). Does NOT emit
        note-offs: use it only when the part is reset as well. */
    void reset() noexcept
    {
        keys_.reset();
        arp_ = Arpeggiator {};
        seq_ = StepSequencer {};
        clock_.reset();
        view_ = Clock::View {};
        active_ = false;
        nextIdx_ = 0;
        haveLast_ = false;
        sounding_ = Sounding {};
        passedOn_.fill (false);
        passedCount_ = 0;
        late_.fill (false);
        lateCount_ = 0;
        pedalPassed_ = false;
        cfg_ = Settings {}; // the state is "Off" until the next process call applies the settings
        arp_.reseed (cfg_.seed);
    }

    /** Takes effect at the start of the next `process` call. Out-of-range values are clamped. */
    void setSettings (const Settings& s) noexcept { settings_ = s.sanitised(); }
    const Settings& settings() const noexcept { return settings_; }

    /** Number of events dropped for lack of output capacity since the last `resetDropCount`. */
    int droppedCount() const noexcept { return droppedCount_; }
    void resetDropCount() noexcept { droppedCount_ = 0; }

    /** True while the module still owes a note-off (a note is sounding, or its note-off could not
        be written yet because the output was full). */
    bool isSounding() const noexcept { return sounding_.active || lateCount_ > 0; }

    /**
        Transforms the block's input events. Returns the number of events written to `out`.

        Output contract: offsets are ascending and inside [0, numSamples - 1] (0 for an empty
        block). In mode Off the input is passed on bit-exactly as long as it is itself ascending and
        inside the block; an event that is not (before the block, after it, out of order) is moved to
        the nearest offset that keeps the output ascending and inside the block.

        Order of simultaneous events at one sample offset:
          1. note-offs that fall due (gate end),
          2. the input events of that offset, in input order (key bookkeeping; controllers other than
             the sustain pedal are passed on; for a panic event the module's own note-off precedes it),
          3. the clock step (so a key pressed at the step's sample is in that step's pattern).
        Within a step the note the module still owes is released before the new note starts, except
        legato: with gate = 100 % and a different pitch next, the new note-on comes BEFORE the old
        note-off (an overlap a Solo/Unison part can glide through); the same pitch is always
        note-off, then note-on.
        Mode, hold, rate, tempo and transport changes are acted on at offset 0.
        Swing changes keep the step index (a step that is now in the past fires late, at offset 0,
        once); a rate change continues with the first step of the new grid after the last one fired.
    */
    int process (std::span<const NoteEvent> in, const Transport& tr, int numSamples, std::span<NoteEvent> out) noexcept
    {
        out_ = out;
        outCount_ = 0;
        droppedPass_ = false;
        lastOffset_ = 0;
        n_ = std::max (numSamples, 0);
        tLast_ = std::max (n_ - 1, 0);
        curT_ = 0;
        lastStepT_ = -1;
        stepsThisCall_ = 0;

        const Clock::Block cb = clock_.begin (tr, n_, settings_.internalBpm, 1.0 / static_cast<double> (stepsPerQuarter (settings_.rate)));
        view_ = cb.view;
        blockStart (cb);

        const std::size_t nIn = in.size();
        std::size_t i = 0;
        // Every iteration consumes an input event, a due note-off or a step; the cap is a guard only.
        const std::size_t maxIterations = nIn + 4 * static_cast<std::size_t> (kMaxStepsPerCall) + 16;
        for (std::size_t iter = 0; iter < maxIterations; ++iter)
        {
            const int ti = i < nIn ? std::clamp (in[i].offset, curT_, tLast_) : kNever;
            const int ts = nextStepSample();
            const int to = nextOffSample();
            const int t = std::min ({ ti, ts, to });
            if (t == kNever || t > tLast_)
                break;
            curT_ = t;

            emitDueOffs (t);
            while (i < nIn && std::clamp (in[i].offset, curT_, tLast_) <= t)
                handleInput (in[i++], t);
            emitDueOffs (t); // a legato note whose step no longer comes (the pattern just emptied)
            if (nextStepSample() <= t)
                fireStep (t);
        }

        blockEnd();
        return outCount_;
    }

private:
    static constexpr int kNever = INT_MAX;

    struct Sounding
    {
        bool active = false;
        bool open = false;   // waiting for the next step to extend (Tie) or end it; no note-off scheduled
        bool legato = false; // gate 100 %: the note-off belongs to the next step (after its note-on)
        int note = 0;
        double endPpq = 0.0; // gate end
        int minOff = 0;      // earliest sample of the note-off (block relative): always after the note-on
    };

    bool modeOff() const noexcept { return cfg_.mode == Mode::Off; }
    bool effectiveHold() const noexcept { return cfg_.hold && ! modeOff(); }
    Grid grid() const noexcept { return Grid { stepsPerQuarter (cfg_.rate), cfg_.swing }; }

    // ---- Output -----------------------------------------------------------------------------
    /** One slot of the buffer is always kept for the overflow marker. `owed` events (note-offs,
        panics) may use everything else; the rest also leaves room for the note-off(s) we may owe. */
    bool tryEmit (const NoteEvent& e, bool owed, bool isNoteOn) noexcept
    {
        int need = outCount_ + 2;
        if (! owed)
            need += (sounding_.active ? 1 : 0) + (isNoteOn ? 1 : 0);
        if (need > static_cast<int> (out_.size()))
        {
            ++droppedCount_;
            return false;
        }
        out_[static_cast<std::size_t> (outCount_++)] = e;
        lastOffset_ = e.offset;
        return true;
    }

    bool writeOff (int t, int note) noexcept
    {
        return tryEmit (NoteEvent { t, EventType::NoteOff, static_cast<std::uint8_t> (note), 0.0f }, true, false);
    }

    /** Release `note` downstream, or keep owing it (it is retried at the start of the next call). */
    void emitOff (int t, int note) noexcept
    {
        if (! writeOff (t, note))
            markLate (note);
    }

    void flushSounding (int t) noexcept
    {
        if (sounding_.active)
        {
            sounding_.active = false;
            emitOff (t, sounding_.note);
        }
    }

    void markLate (int note) noexcept
    {
        if (! late_[static_cast<std::size_t> (note)])
        {
            late_[static_cast<std::size_t> (note)] = true;
            ++lateCount_;
        }
    }

    /** Write the note-offs that could not be written earlier. */
    void retryLate (int t) noexcept
    {
        for (int n = 0; n < 128 && lateCount_ > 0; ++n)
            if (late_[static_cast<std::size_t> (n)])
            {
                if (! writeOff (t, n))
                    return;
                late_[static_cast<std::size_t> (n)] = false;
                --lateCount_;
            }
    }

    /** Make sure the part is not still holding `note` because of us (so a new note-on of that pitch
        cannot double it). False: the release could not be written, hold the note back. */
    bool ensureReleased (int note, int t) noexcept
    {
        if (late_[static_cast<std::size_t> (note)])
        {
            if (! writeOff (t, note))
                return false;
            late_[static_cast<std::size_t> (note)] = false;
            --lateCount_;
        }
        if (! modeOff() && passedOn_[static_cast<std::size_t> (note)])
        {
            if (! writeOff (t, note))
                return false;
            clearPassed (note);
        }
        return true;
    }

    void releasePassedOn (int t) noexcept
    {
        for (int n = 0; n < 128 && passedCount_ > 0; ++n)
            if (passedOn_[static_cast<std::size_t> (n)])
            {
                if (! writeOff (t, n))
                    return; // keep owing it: retried in the next call
                clearPassed (n);
            }
    }

    /** The pedal state the part should hold: the player's while Off, released otherwise. */
    void syncPedal (int t) noexcept
    {
        const bool want = modeOff() ? keys_.pedal() : false;
        if (want != pedalPassed_
            && tryEmit (NoteEvent { t, EventType::SustainPedal, 0, want ? 1.0f : 0.0f }, true, false))
            pedalPassed_ = want;
    }

    bool passThrough (const NoteEvent& original, int t, bool owed) noexcept
    {
        NoteEvent e = original;
        if (! modeOff())
            e.offset = t;
        else
            e.offset = std::clamp (e.offset, lastOffset_, tLast_); // bit-exact when already ascending and inside the block
        if (tryEmit (e, owed, false))
            return true;
        droppedPass_ = true;
        return false;
    }

    // ---- Block start and end ------------------------------------------------------------------
    void blockStart (const Clock::Block& cb) noexcept
    {
        const Settings prev = cfg_;
        cfg_ = settings_;
        const bool modeChanged = cfg_.mode != prev.mode;
        const bool rateChanged = cfg_.rate != prev.rate;

        // Owed releases first, then the pedal: a note-off written after a pedal-down would be deferred.
        retryLate (0);
        if (cb.jumped || modeChanged)
            flushSounding (0);
        if (cb.jumped || cb.sourceChanged)
            haveLast_ = false;
        if (modeChanged || cfg_.hold != prev.hold)
            keys_.prune (effectiveHold());
        syncPedal (0); // Off -> on: pedal-up (the part releases what its pedal holds); on -> Off: the player's state
        if (passedCount_ > 0 && cfg_.mode != Mode::Off)
            releasePassedOn (0); // notes we let through while Off: the part holds them no more after the pedal-up
        if (cfg_.seed != prev.seed)
            arp_.reseed (cfg_.seed);

        if (modeOff())
        {
            active_ = false;
            return;
        }
        if (modeChanged)
        {
            active_ = false;
            arp_.restart (cfg_.seed);
            seq_.restart();
        }
        if (active_ && keys_.empty())
            deactivate (0);
        if (! active_)
        {
            if (! keys_.empty())
                activate (0);
            return;
        }
        if (cb.sourceChanged)
        {
            arp_.restart (cfg_.seed);
            seq_.restart();
        }
        if (! view_.hostSynced && ! clock_.internalRunning())
        {
            clock_.startInternal (view_, 0);
            nextIdx_ = 0;
        }
        else if (cb.jumped)
            nextIdx_ = firstStepAtOrAfter (0);
        else if (rateChanged)
            nextIdx_ = firstStepAfterLast();
        // A swing change keeps the step index: a step that is now in the past fires once, late.
    }

    void blockEnd() noexcept
    {
        // `droppedPass_`: pass-through events were lost; like the event parser's overflow rule, an
        // All Notes Off marker makes sure nothing can hang downstream. Its slot is always reserved.
        if (droppedPass_ && outCount_ < static_cast<int> (out_.size()))
        {
            out_[static_cast<std::size_t> (outCount_++)] = NoteEvent { std::clamp (lastOffset_, 0, tLast_), EventType::AllNotesOff, 0, 0.0f };
            sounding_ = Sounding {};
            passedOn_.fill (false);
            passedCount_ = 0;
            late_.fill (false);
            lateCount_ = 0;
            pedalPassed_ = false;
        }
        sounding_.minOff = std::max (0, sounding_.minOff - n_);
        if (! active_ && ! sounding_.active)
            clock_.stopInternal();
        clock_.end (n_);
    }

    // ---- Activation ---------------------------------------------------------------------------
    /** The pattern became non-empty at sample t: the clock starts and the pattern restarts. */
    void activate (int t) noexcept
    {
        active_ = true;
        haveLast_ = false;
        arp_.restart (cfg_.seed);
        seq_.restart();
        if (! view_.hostSynced)
        {
            flushSounding (t); // a pending gate lives in the old internal time base
            clock_.startInternal (view_, t);
            nextIdx_ = 0;
        }
        else
            nextIdx_ = firstStepAtOrAfter (t);
    }

    /** The pattern became empty at sample t: a note held open for a Tie ends now; a gate that is
        already scheduled finishes on its own. */
    void deactivate (int t) noexcept
    {
        active_ = false;
        if (sounding_.active && sounding_.open)
            flushSounding (t);
    }

    // ---- Steps --------------------------------------------------------------------------------
    std::int64_t stepSampleOf (std::int64_t k) const noexcept { return view_.sampleOf (grid().pos (k)); }

    /** First step whose sample is >= t. Bounded loops: the estimate is within a step or two. */
    std::int64_t firstStepAtOrAfter (int t) noexcept
    {
        const Grid g = grid();
        const double ppq = view_.ppqAt (t);
        auto k = static_cast<std::int64_t> (std::floor (ppq * static_cast<double> (g.stepsPerQuarter))) - 2;
        for (int guard = 0; guard < 8 && stepSampleOf (k - 1) >= t; ++guard)
            --k;
        for (int guard = 0; guard < 16 && stepSampleOf (k) < t; ++guard)
            ++k;
        return k;
    }

    /** After a rate change: the first step of the new grid after the last step that fired (at most
        one of them overdue, so a coarse-to-fine change cannot fire a burst). */
    std::int64_t firstStepAfterLast() noexcept
    {
        const std::int64_t first = firstStepAtOrAfter (0);
        if (! haveLast_)
            return first;
        const Grid g = grid();
        auto k = static_cast<std::int64_t> (std::floor (lastFiredPpq_ * static_cast<double> (g.stepsPerQuarter))) - 2;
        for (int guard = 0; guard < 16 && g.pos (k) <= lastFiredPpq_ + 1.0e-9; ++guard)
            ++k;
        return std::max (k, first - 1);
    }

    int nextStepSample() const noexcept
    {
        if (! active_ || n_ == 0)
            return kNever;
        const std::int64_t s = std::max ({ stepSampleOf (nextIdx_), static_cast<std::int64_t> (curT_), static_cast<std::int64_t> (lastStepT_) + 1 });
        return s >= n_ ? kNever : static_cast<int> (s);
    }

    int sampleOfOff() const noexcept
    {
        const std::int64_t s = std::max ({ view_.sampleOf (sounding_.endPpq), static_cast<std::int64_t> (sounding_.minOff), static_cast<std::int64_t> (curT_) });
        return s >= n_ ? kNever : static_cast<int> (s);
    }

    int nextOffSample() const noexcept
    {
        if (! sounding_.active || sounding_.open || n_ == 0)
            return kNever;
        return sampleOfOff();
    }

    void emitDueOffs (int t) noexcept
    {
        if (! sounding_.active || sounding_.open || sampleOfOff() > t)
            return;
        if (sounding_.legato && active_ && nextStepSample() <= t)
            return; // the step that fires now releases it, after starting the next note
        flushSounding (t);
    }

    void fireStep (int t) noexcept
    {
        if (++stepsThisCall_ > kMaxStepsPerCall)
        {
            nextIdx_ = firstStepAtOrAfter (n_); // absurdly long block: skip to its end
            return;
        }
        const Grid g = grid();
        const std::int64_t k = nextIdx_++;
        lastStepT_ = t;
        const double p0 = g.pos (k);
        const double p1 = g.pos (k + 1);
        lastFiredPpq_ = p0;
        haveLast_ = true;
        const double endPpq = cfg_.gate >= 1.0 ? p1 : p0 + cfg_.gate * (p1 - p0);

        const StepAction a = cfg_.mode == Mode::Arp ? arp_.step (keys_, cfg_.arpPattern, cfg_.arpOctaves)
                                                     : seq_.step (keys_, cfg_);

        if (a.kind == StepAction::Kind::Tie && sounding_.active && sounding_.open)
        {
            sounding_.open = a.tieFollows;
            sounding_.legato = ! a.tieFollows && cfg_.gate >= 1.0;
            sounding_.endPpq = endPpq;
            sounding_.minOff = t + 1;
            return;
        }
        const bool overlap = a.kind == StepAction::Kind::Note && sounding_.active && sounding_.legato && ! sounding_.open
                             && a.note != sounding_.note;
        if (! overlap)
            flushSounding (t); // before any new note-on: everything still owed goes first
        if (a.kind != StepAction::Kind::Note)
            return;
        const int oldNote = sounding_.note;
        if (! ensureReleased (a.note, t))
        {
            if (overlap)
                flushSounding (t);
            return; // the part still holds this pitch and the release does not fit: hold the note back
        }
        if (tryEmit (NoteEvent { t, EventType::NoteOn, static_cast<std::uint8_t> (a.note), a.velocity }, false, true))
        {
            if (overlap)
                emitOff (t, oldNote); // legato: the old note ends after the new one has started
            sounding_ = Sounding { true, a.tieFollows, ! a.tieFollows && cfg_.gate >= 1.0, a.note, endPpq, t + 1 };
        }
        else if (overlap)
            flushSounding (t);
    }

    // ---- Input events -------------------------------------------------------------------------
    void handleInput (const NoteEvent& ev, int t) noexcept
    {
        const bool on = ! modeOff();
        switch (ev.type)
        {
            case EventType::NoteOn:
            {
                if (! on)
                {
                    keys_.noteOn (ev.note, ev.value, false);
                    if (ev.note < 128 && ! ensureReleased (ev.note, t))
                    {
                        droppedPass_ = true; // the part still holds this pitch because of us: hold the user's note back
                        return;
                    }
                    if (passThrough (ev, t, false) && ev.note < 128)
                        markPassed (ev.note);
                    return;
                }
                if (ev.note >= 128)
                    return;
                const bool wasEmpty = keys_.empty();
                const bool newPattern = keys_.noteOn (ev.note, ev.value, effectiveHold());
                if (wasEmpty)
                    activate (t);
                else if (newPattern)
                    arp_.restart (cfg_.seed); // Hold: a new pattern; the sequencer keeps running (re-transposes)
                break;
            }
            case EventType::NoteOff:
            {
                if (! on)
                {
                    keys_.noteOff (ev.note, false);
                    const bool passed = passThrough (ev, t, true);
                    if (ev.note < 128)
                    {
                        const bool sounding = passedOn_[static_cast<std::size_t> (ev.note)];
                        clearPassed (ev.note);
                        if (! passed && sounding)
                            markLate (ev.note); // the player's release was lost: we owe it to the part
                    }
                    return;
                }
                if (ev.note >= 128)
                    return;
                keys_.noteOff (ev.note, effectiveHold());
                if (active_ && keys_.empty())
                    deactivate (t);
                break;
            }
            case EventType::SustainPedal:
            {
                const bool down = ev.value >= 0.5f;
                keys_.setPedal (down, effectiveHold());
                if (! on)
                {
                    if (passThrough (ev, t, false))
                        pedalPassed_ = down;
                }
                else if (active_ && keys_.empty())
                    deactivate (t);
                break;
            }
            case EventType::ResetControllers:
            {
                keys_.setPedal (false, effectiveHold());
                if (passThrough (ev, t, true))
                    pedalPassed_ = false;
                if (on && active_ && keys_.empty())
                    deactivate (t);
                break;
            }
            case EventType::AllNotesOff:
            case EventType::AllSoundOff:
            {
                flushSounding (t); // our own note-off first, then the panic itself
                keys_.clearAll();
                active_ = false;
                if (passThrough (ev, t, true))
                {
                    // The part releases everything and clears its pedal: nothing is owed any more.
                    passedOn_.fill (false);
                    passedCount_ = 0;
                    late_.fill (false);
                    lateCount_ = 0;
                    pedalPassed_ = false;
                }
                break;
            }
            case EventType::PitchBend:
            case EventType::ModWheel:
            default:
                passThrough (ev, t, false);
                break;
        }
    }

    void markPassed (int note) noexcept
    {
        if (! passedOn_[static_cast<std::size_t> (note)])
        {
            passedOn_[static_cast<std::size_t> (note)] = true;
            ++passedCount_;
        }
    }

    void clearPassed (int note) noexcept
    {
        if (passedOn_[static_cast<std::size_t> (note)])
        {
            passedOn_[static_cast<std::size_t> (note)] = false;
            --passedCount_;
        }
    }

    // ---- State --------------------------------------------------------------------------------
    Settings settings_ {}; // as set by the user
    Settings cfg_ {};      // as applied at the start of the current call

    KeyState keys_;
    Arpeggiator arp_;
    StepSequencer seq_;
    Clock clock_;
    Clock::View view_;

    bool active_ = false;       // mode on and the pattern is not empty: steps are being generated
    std::int64_t nextIdx_ = 0;  // next grid step to fire
    double lastFiredPpq_ = 0.0; // position of the last step that fired (valid if haveLast_)
    bool haveLast_ = false;
    Sounding sounding_;
    std::array<bool, 128> passedOn_ {}; // note-ons passed through while Off and not yet released
    int passedCount_ = 0;
    std::array<bool, 128> late_ {};     // note-offs we owe the part but could not write yet
    int lateCount_ = 0;
    bool pedalPassed_ = false;          // the pedal state the part has been sent

    // Per-call scratch (valid inside `process`).
    std::span<NoteEvent> out_;
    int outCount_ = 0;
    int lastOffset_ = 0;
    bool droppedPass_ = false;
    int droppedCount_ = 0;
    int n_ = 0;
    int tLast_ = 0;
    int curT_ = 0;
    int lastStepT_ = -1;
    int stepsThisCall_ = 0;
};
} // namespace vs::seq
