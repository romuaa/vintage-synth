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
    /** Output buffers should hold at least this many events: the input events plus the notes and
        note-offs of the steps in one block. On overflow new notes and pass-through events are
        dropped (an All Notes Off marker is appended), owed note-offs are always emitted. */
    static constexpr int kRecommendedCapacity = 4096;

    /** Hard cap on clock steps handled in one call (a guard against absurd block sizes). */
    static constexpr int kMaxStepsPerCall = 2048;

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
        sounding_ = Sounding {};
        passedOn_.fill (false);
        passedCount_ = 0;
        cfg_ = Settings {}; // the state is "Off" until the next process call applies the settings
        arp_.reseed (cfg_.seed);
    }

    /** Takes effect at the start of the next `process` call. Out-of-range values are clamped. */
    void setSettings (const Settings& s) noexcept { settings_ = s.sanitised(); }
    const Settings& settings() const noexcept { return settings_; }

    /** Number of events dropped for lack of output capacity since the last `resetDropCount`. */
    int droppedCount() const noexcept { return droppedCount_; }
    void resetDropCount() noexcept { droppedCount_ = 0; }

    /** True while the module still owes a note-off (a note is sounding). */
    bool isSounding() const noexcept { return sounding_.active; }

    /**
        Transforms the block's input events. Returns the number of events written to `out`
        (ascending offsets, all within [0, numSamples - 1], or 0 for an empty block).

        Order of simultaneous events at one sample offset:
          1. note-offs that fall due (gate end),
          2. the input events of that offset, in input order (key and controller bookkeeping;
             controllers are passed on; for a panic event the module's own note-off precedes it),
          3. the clock step (so a key pressed at the step's sample is in that step's pattern;
             before a new note-on every note the module still owes is released first).
        Mode, hold, rate, swing, tempo and transport changes are acted on at offset 0.
    */
    int process (std::span<const NoteEvent> in, const Transport& tr, int numSamples, std::span<NoteEvent> out) noexcept
    {
        out_ = out;
        outCount_ = 0;
        droppedPass_ = false;
        lastOffset_ = 0;
        n_ = std::max (numSamples, 0);
        curT_ = 0;
        lastStepT_ = -1;
        stepsThisCall_ = 0;

        const Clock::Block cb = clock_.begin (tr, n_, settings_.internalBpm);
        view_ = cb.view;
        blockStart (cb);

        const int tLast = std::max (n_ - 1, 0);
        const std::size_t nIn = in.size();
        std::size_t i = 0;
        // Every iteration consumes an input event, a due note-off or a step; the cap is a guard only.
        const std::size_t maxIterations = nIn + 4 * static_cast<std::size_t> (kMaxStepsPerCall) + 16;
        for (std::size_t iter = 0; iter < maxIterations; ++iter)
        {
            const int ti = i < nIn ? std::clamp (in[i].offset, curT_, tLast) : kNever;
            const int ts = nextStepSample();
            const int to = nextOffSample();
            const int t = std::min ({ ti, ts, to });
            if (t == kNever || t > tLast)
                break;
            curT_ = t;

            emitDueOffs (t);
            while (i < nIn && std::clamp (in[i].offset, curT_, tLast) <= t)
                handleInput (in[i++], t);
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
        bool open = false; // waiting for the next step to extend (Tie) or end it; no note-off scheduled
        int note = 0;
        double endPpq = 0.0; // gate end
        int minOff = 0;      // earliest sample of the note-off (block relative): always after the note-on
    };

    bool modeOff() const noexcept { return cfg_.mode == Mode::Off; }
    bool effectiveHold() const noexcept { return cfg_.hold && ! modeOff(); }
    Grid grid() const noexcept { return Grid { stepsPerQuarter (cfg_.rate), static_cast<double> (cfg_.swing) }; }

    // ---- Output -----------------------------------------------------------------------------
    /** `owed` events (note-offs, panic pass-through) may use the whole buffer; everything else
        leaves room for the note-off(s) we may owe and an overflow marker. */
    bool tryEmit (const NoteEvent& e, bool owed, bool isNoteOn) noexcept
    {
        int need = outCount_ + 1;
        if (! owed)
            need += 1 + (sounding_.active ? 1 : 0) + (isNoteOn ? 1 : 0);
        if (need > static_cast<int> (out_.size()))
        {
            ++droppedCount_;
            return false;
        }
        out_[static_cast<std::size_t> (outCount_++)] = e;
        lastOffset_ = e.offset;
        return true;
    }

    void emitOff (int t, int note) noexcept
    {
        tryEmit (NoteEvent { t, EventType::NoteOff, static_cast<std::uint8_t> (note), 0.0f }, true, false);
    }

    void flushSounding (int t) noexcept
    {
        if (sounding_.active)
        {
            emitOff (t, sounding_.note);
            sounding_.active = false;
        }
    }

    void releasePassedOn (int t) noexcept
    {
        for (int n = 0; n < 128 && passedCount_ > 0; ++n)
            if (passedOn_[static_cast<std::size_t> (n)])
            {
                if (outCount_ >= static_cast<int> (out_.size()))
                    return; // keep owing it: retried in the next block
                emitOff (t, n);
                passedOn_[static_cast<std::size_t> (n)] = false;
                --passedCount_;
            }
    }

    bool passThrough (const NoteEvent& original, int t, bool owed) noexcept
    {
        NoteEvent e = original;
        if (! modeOff())
            e.offset = t; // Off passes the event bit-exactly; otherwise it is clamped into the block
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
        const bool gridChanged = cfg_.rate != prev.rate || cfg_.swing < prev.swing || cfg_.swing > prev.swing;

        if (passedCount_ > 0 && cfg_.mode != Mode::Off)
            releasePassedOn (0); // notes we let through while Off: the Part still holds them

        if (cb.jumped || modeChanged)
            flushSounding (0);
        if (modeChanged || cfg_.hold != prev.hold)
            keys_.prune (effectiveHold());
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
        if (! view_.hostSynced)
        {
            if (! clock_.internalRunning())
            {
                clock_.startInternal (view_, 0);
                nextIdx_ = 0;
            }
            else if (gridChanged)
                nextIdx_ = firstStepAtOrAfter (0);
        }
        else if (cb.jumped || gridChanged)
            nextIdx_ = firstStepAtOrAfter (0);
    }

    void blockEnd() noexcept
    {
        // `droppedPass_`: pass-through events were lost; like the event parser's overflow rule, an
        // All Notes Off marker makes sure nothing can hang downstream.
        if (droppedPass_ && outCount_ < static_cast<int> (out_.size()))
        {
            out_[static_cast<std::size_t> (outCount_++)] = NoteEvent { lastOffset_, EventType::AllNotesOff, 0, 0.0f };
            sounding_ = Sounding {};
            passedOn_.fill (false);
            passedCount_ = 0;
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
        if (sounding_.active && ! sounding_.open && sampleOfOff() <= t)
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
        const double gate = static_cast<double> (cfg_.gate);
        const double endPpq = gate >= 1.0 ? p1 : p0 + gate * (p1 - p0);

        const StepAction a = cfg_.mode == Mode::Arp ? arp_.step (keys_, cfg_.arpPattern, cfg_.arpOctaves)
                                                     : seq_.step (keys_, cfg_);

        if (a.kind == StepAction::Kind::Tie && sounding_.active && sounding_.open)
        {
            sounding_.open = a.tieFollows;
            sounding_.endPpq = endPpq;
            sounding_.minOff = t + 1;
            return;
        }
        flushSounding (t); // before any new note-on: everything still owed goes first
        if (a.kind != StepAction::Kind::Note)
            return;
        if (tryEmit (NoteEvent { t, EventType::NoteOn, static_cast<std::uint8_t> (a.note), a.velocity }, false, true))
            sounding_ = Sounding { true, a.tieFollows, a.note, endPpq, t + 1 };
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
                    passThrough (ev, t, true);
                    if (ev.note < 128)
                        clearPassed (ev.note);
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
                keys_.setPedal (ev.value >= 0.5f, effectiveHold());
                passThrough (ev, t, false);
                if (on && active_ && keys_.empty())
                    deactivate (t);
                break;
            }
            case EventType::ResetControllers:
            {
                keys_.setPedal (false, effectiveHold());
                passThrough (ev, t, true);
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
                passedOn_.fill (false);
                passedCount_ = 0;
                passThrough (ev, t, true);
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
    Sounding sounding_;
    std::array<bool, 128> passedOn_ {}; // note-ons passed through while Off and not yet released
    int passedCount_ = 0;

    // Per-call scratch (valid inside `process`).
    std::span<NoteEvent> out_;
    int outCount_ = 0;
    int lastOffset_ = 0;
    bool droppedPass_ = false;
    int droppedCount_ = 0;
    int n_ = 0;
    int curT_ = 0;
    int lastStepT_ = -1;
    int stepsThisCall_ = 0;
};
} // namespace vs::seq
