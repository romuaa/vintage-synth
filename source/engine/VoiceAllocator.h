#pragma once

#include <array>
#include <cstdint>

namespace vs
{
/** Voices per part (docs/architecture.md 3.2). */
inline constexpr int kMaxVoices = 16;

/** Order == the "Assign Mode" parameter's choice order. */
enum class AssignMode : int
{
    Poly = 0,
    Unison = 1,
    Solo = 2
};

/**
    One decision of the allocator, executed by the Part.

    - Start:    (re)trigger `voice` with `note`. If the voice is sounding, its envelopes
                restart from their current level (no reset).
    - Retarget: legato pitch change of a sounding voice to `note`, no envelope retrigger.
    - Release:  key release (envelopes go to their release stage).
    - Steal:    the voice is busy with another note: fade it out (3 ms), reset it, then start `note`.

    glideFrom is a note number to glide from, kGlideFromCurrent (glide from the voice's current,
    possibly still gliding, pitch) or kNoGlide. The Part ignores it when portamento is off.
*/
struct VoiceAction
{
    enum class Type : std::uint8_t
    {
        Start,
        Retarget,
        Release,
        Steal
    };

    static constexpr int kNoGlide = -1;
    static constexpr int kGlideFromCurrent = -2;

    Type type = Type::Start;
    int voice = 0;
    int note = 0;
    float velocity = 0.0f;
    int glideFrom = kNoGlide;
};

/**
    Note -> voice decisions for one part (docs/architecture.md 3.2 / 3.3). Pure logic: no DSP,
    no JUCE, no allocation, deterministic. Every mutating call takes a sink, any callable
    `void (const VoiceAction&)`, which receives the actions in order; the Part executes them
    immediately. The Part reports back with voiceIdle() when a voice's ENV-2 has finished.

    Poly:   one voice per note. New note: (1) a busy voice already playing that note is retriggered,
            (2) else the voice that has been idle longest, (3) else steal: the voice that entered
            its release first; if none is releasing, the held voice that started first (never the
            most recently started one).
    Unison: voices 0 .. n-1 all play the same note; monophonic, last-note priority.
    Solo:   voice 0, last-note priority.
    In Unison and Solo a new note retriggers the envelopes; releasing the sounding key while
    other keys are held returns to the most recent of them without a retrigger (Retarget).

    Sustain pedal: while it is down, key releases are deferred. Poly: the voice is released when
    the pedal comes up. Unison/Solo: a released key stays on the key stack (marked "not physical"),
    so the sounding note is held; pedal up removes those entries and the part follows the
    remaining keys (or releases).

    Portamento: glide on a Start is allowed in mode Always, or in Legato when another key is
    physically held. Poly glides from the previously played note; Unison/Solo glide from the
    sounding pitch. A Retarget always glides (a key is held by definition).
*/
class VoiceAllocator
{
public:
    VoiceAllocator() noexcept { reset(); }

    /** Everything idle, no keys, pedal up, no previous note. Keeps the mode settings. */
    void reset() noexcept
    {
        for (int v = 0; v < kMaxVoices; ++v)
            voices[idx (v)] = VoiceState { -1, false, false, false, false, 0u, 0u, static_cast<std::uint32_t> (v) };
        clock = static_cast<std::uint32_t> (kMaxVoices);
        numKeys = 0;
        pedal = false;
        lastNote = -1;
    }

    /** Sets the mode without emitting anything (prepare/reset time). */
    void initMode (AssignMode m, int unisonVoices) noexcept
    {
        mode = m;
        unisonCount = clampVoices (unisonVoices);
    }

    /**
        Per block. A change of the mode (or of the unison count while in Unison) releases every
        sounding voice and forgets the held keys (their later note-offs are ignored).
    */
    template <typename Sink>
    void setMode (AssignMode m, int unisonVoices, Sink&& sink) noexcept
    {
        const int count = clampVoices (unisonVoices);
        const bool changed = m != mode || (m == AssignMode::Unison && count != unisonCount);
        mode = m;
        unisonCount = count;
        if (! changed)
            return;
        numKeys = 0;
        releaseAll (sink);
    }

    void setLegatoGlideOnly (bool legatoOnly) noexcept { glideLegatoOnly = legatoOnly; }

    template <typename Sink>
    void noteOn (int note, float velocity, Sink&& sink) noexcept
    {
        if (note < 0 || note > 127)
            return;
        const bool glideOk = ! glideLegatoOnly || anyPhysicalKeyExcept (note);
        pushKey (note);

        if (mode == AssignMode::Poly)
            polyNoteOn (note, velocity, glideOk, sink);
        else
            monoNoteOn (note, velocity, glideOk, sink);
        lastNote = note;
    }

    template <typename Sink>
    void noteOff (int note, Sink&& sink) noexcept
    {
        const int k = findKey (note);
        if (k < 0)
            return; // not held (or forgotten by a mode change / All Notes Off)

        if (mode == AssignMode::Poly)
        {
            removeKey (k);
            for (int v = 0; v < kMaxVoices; ++v)
            {
                auto& s = voices[idx (v)];
                if (s.busy && s.keyDown && s.note == note)
                {
                    s.keyDown = false;
                    if (pedal)
                        s.sustained = true;
                    else
                        release (v, sink);
                }
            }
            return;
        }

        if (pedal)
        {
            keys[idx (k)].physical = false; // the pedal holds it
            return;
        }
        removeKey (k);
        monoFollowKeys (sink);
    }

    template <typename Sink>
    void setSustain (bool down, Sink&& sink) noexcept
    {
        if (down == pedal)
            return;
        pedal = down;
        if (down)
            return;

        if (mode == AssignMode::Poly)
        {
            for (int v = 0; v < kMaxVoices; ++v)
            {
                auto& s = voices[idx (v)];
                if (s.busy && s.sustained && ! s.released)
                    release (v, sink);
            }
            return;
        }

        int n = 0;
        for (int k = 0; k < numKeys; ++k)
            if (keys[idx (k)].physical)
                keys[idx (n++)] = keys[idx (k)];
        numKeys = n;
        monoFollowKeys (sink);
    }

    /** All Notes Off: every key released, the pedal cleared (so nothing can hang). */
    template <typename Sink>
    void allNotesOff (Sink&& sink) noexcept
    {
        pedal = false;
        numKeys = 0;
        releaseAll (sink);
    }

    /**
        Key-mode change (docs/architecture.md 4.2): every key forgotten (later note-offs are
        ignored) and every sounding voice released, but the pedal keeps its state: notes played
        after the change are sustained as usual while it stays down.
    */
    template <typename Sink>
    void releaseAllKeepPedal (Sink&& sink) noexcept
    {
        numKeys = 0;
        releaseAll (sink);
    }

    /** All Sound Off: like allNotesOff, and every voice is idle at once (the Part resets them). */
    void killAll() noexcept
    {
        pedal = false;
        numKeys = 0;
        for (int v = 0; v < kMaxVoices; ++v)
            voiceIdle (v);
    }

    /** From the Part: the voice's envelope has finished. */
    void voiceIdle (int v) noexcept
    {
        if (v < 0 || v >= kMaxVoices)
            return;
        auto& s = voices[idx (v)];
        if (! s.busy)
            return;
        s.busy = s.keyDown = s.sustained = s.released = false;
        s.idleAge = ++clock;
    }

    // ---- Queries ----
    bool isVoiceBusy (int v) const noexcept { return voices[idx (v)].busy; }
    int getVoiceNote (int v) const noexcept { return voices[idx (v)].note; }
    bool isVoiceReleased (int v) const noexcept { return voices[idx (v)].released; }
    bool isPedalDown() const noexcept { return pedal; }
    AssignMode getMode() const noexcept { return mode; }
    int getUnisonCount() const noexcept { return unisonCount; }

    /** True if a key other than `note` is physically down (LFO delay retrigger, Legato glide). */
    bool anyPhysicalKeyExcept (int note) const noexcept
    {
        for (int k = 0; k < numKeys; ++k)
            if (keys[idx (k)].physical && keys[idx (k)].note != note)
                return true;
        return false;
    }

    /**
        True if a note other than `note` is held by a key or by the sustain pedal (LFO delay
        restart rule: ringing pedal-held notes count, so delayed vibrato does not drop out when
        the next note is played with all keys up). Notes merely in their release do not count.
    */
    bool anyNoteHeldExcept (int note) const noexcept
    {
        for (int k = 0; k < numKeys; ++k) // Poly: keys down; Unison/Solo: keys down or pedal-held
            if (keys[idx (k)].note != note)
                return true;
        if (mode == AssignMode::Poly)
            for (int v = 0; v < kMaxVoices; ++v)
            {
                const auto& s = voices[idx (v)];
                if (s.busy && s.sustained && ! s.released && s.note != note)
                    return true;
            }
        return false;
    }

    int numPhysicalKeys() const noexcept
    {
        int n = 0;
        for (int k = 0; k < numKeys; ++k)
            n += keys[idx (k)].physical ? 1 : 0;
        return n;
    }

    /** Voices used by the current mode: 16 in Poly, the unison count, or 1 in Solo. */
    int groupSize() const noexcept
    {
        return mode == AssignMode::Poly ? kMaxVoices : (mode == AssignMode::Unison ? unisonCount : 1);
    }

private:
    struct VoiceState
    {
        int note;
        bool busy;      // started, not yet reported idle
        bool keyDown;   // Poly: its key is down
        bool sustained; // Poly: key released while the pedal was down
        bool released;  // in its release stage
        std::uint32_t startAge, releaseAge, idleAge;
    };

    struct Key
    {
        int note;
        bool physical; // false: released while the pedal was down (Unison/Solo only)
    };

    static constexpr std::size_t idx (int i) noexcept { return static_cast<std::size_t> (i); }

    static int clampVoices (int n) noexcept
    {
        if (n <= 2)
            return 2;
        if (n <= 4)
            return 4;
        if (n <= 8)
            return 8;
        return kMaxVoices;
    }

    int findKey (int note) const noexcept
    {
        for (int k = 0; k < numKeys; ++k)
            if (keys[idx (k)].note == note)
                return k;
        return -1;
    }

    void removeKey (int k) noexcept
    {
        for (int i = k; i < numKeys - 1; ++i)
            keys[idx (i)] = keys[idx (i + 1)];
        --numKeys;
    }

    /** Moves (or adds) the note to the top of the key stack as a physical key. */
    void pushKey (int note) noexcept
    {
        const int k = findKey (note);
        if (k >= 0)
            removeKey (k);
        if (numKeys < static_cast<int> (keys.size())) // 128 distinct notes: always true
            keys[idx (numKeys++)] = Key { note, true };
    }

    template <typename Sink>
    void emit (Sink& sink, VoiceAction::Type t, int v, int note, float velocity, int glideFrom) noexcept
    {
        VoiceAction a;
        a.type = t;
        a.voice = v;
        a.note = note;
        a.velocity = velocity;
        a.glideFrom = glideFrom;
        sink (a);
    }

    template <typename Sink>
    void release (int v, Sink& sink) noexcept
    {
        auto& s = voices[idx (v)];
        s.keyDown = s.sustained = false;
        s.released = true;
        s.releaseAge = ++clock;
        emit (sink, VoiceAction::Type::Release, v, s.note, 0.0f, VoiceAction::kNoGlide);
    }

    template <typename Sink>
    void releaseAll (Sink& sink) noexcept
    {
        for (int v = 0; v < kMaxVoices; ++v)
            if (voices[idx (v)].busy && ! voices[idx (v)].released)
                release (v, sink);
    }

    void markStarted (int v, int note) noexcept
    {
        auto& s = voices[idx (v)];
        s.note = note;
        s.busy = true;
        s.keyDown = true;
        s.sustained = s.released = false;
        s.startAge = ++clock;
    }

    template <typename Sink>
    void polyNoteOn (int note, float velocity, bool glideOk, Sink& sink) noexcept
    {
        const int fromPrevious = glideOk && lastNote >= 0 ? lastNote : VoiceAction::kNoGlide;

        // (1) A busy voice already playing this note (held, pedal-held or releasing): retrigger it.
        int v = -1;
        for (int i = 0; i < kMaxVoices; ++i)
        {
            const auto& s = voices[idx (i)];
            if (s.busy && s.note == note && (v < 0 || s.startAge > voices[idx (v)].startAge))
                v = i;
        }
        if (v >= 0)
        {
            markStarted (v, note);
            emit (sink, VoiceAction::Type::Start, v, note, velocity,
                  glideOk ? VoiceAction::kGlideFromCurrent : VoiceAction::kNoGlide);
            return;
        }

        // (2) The voice that has been idle longest.
        for (int i = 0; i < kMaxVoices; ++i)
        {
            const auto& s = voices[idx (i)];
            if (! s.busy && (v < 0 || s.idleAge < voices[idx (v)].idleAge))
                v = i;
        }
        if (v >= 0)
        {
            markStarted (v, note);
            emit (sink, VoiceAction::Type::Start, v, note, velocity, fromPrevious);
            return;
        }

        // (3) Steal.
        v = stealCandidate();
        markStarted (v, note);
        emit (sink, VoiceAction::Type::Steal, v, note, velocity, fromPrevious);
    }

    /** All voices are busy here. */
    int stealCandidate() const noexcept
    {
        int best = -1;
        for (int i = 0; i < kMaxVoices; ++i)
        {
            const auto& s = voices[idx (i)];
            if (s.released && (best < 0 || s.releaseAge < voices[idx (best)].releaseAge))
                best = i;
        }
        if (best >= 0)
            return best;

        int newest = 0;
        for (int i = 1; i < kMaxVoices; ++i)
            if (voices[idx (i)].startAge > voices[idx (newest)].startAge)
                newest = i;
        for (int i = 0; i < kMaxVoices; ++i)
            if (i != newest && (best < 0 || voices[idx (i)].startAge < voices[idx (best)].startAge))
                best = i;
        return best;
    }

    template <typename Sink>
    void monoNoteOn (int note, float velocity, bool glideOk, Sink& sink) noexcept
    {
        const int n = groupSize();
        for (int v = 0; v < n; ++v)
        {
            int from = VoiceAction::kNoGlide;
            if (glideOk)
                from = voices[idx (v)].busy ? VoiceAction::kGlideFromCurrent : (lastNote >= 0 ? lastNote : VoiceAction::kNoGlide);
            markStarted (v, note);
            emit (sink, VoiceAction::Type::Start, v, note, velocity, from);
        }
    }

    /** Unison/Solo after a key release or pedal up: follow the top of the stack, or release. */
    template <typename Sink>
    void monoFollowKeys (Sink& sink) noexcept
    {
        const int n = groupSize();
        if (numKeys == 0)
        {
            for (int v = 0; v < n; ++v)
                if (voices[idx (v)].busy && ! voices[idx (v)].released)
                    release (v, sink);
            return;
        }

        const int top = keys[idx (numKeys - 1)].note;
        for (int v = 0; v < n; ++v)
        {
            auto& s = voices[idx (v)];
            if (s.busy && ! s.released && s.note != top)
            {
                s.note = top;
                emit (sink, VoiceAction::Type::Retarget, v, top, 0.0f, VoiceAction::kGlideFromCurrent);
            }
        }
    }

    std::array<VoiceState, kMaxVoices> voices {};
    std::array<Key, 128> keys {};
    int numKeys = 0;
    std::uint32_t clock = 0;
    bool pedal = false;
    bool glideLegatoOnly = false;
    AssignMode mode = AssignMode::Poly;
    int unisonCount = 4;
    int lastNote = -1;
};
} // namespace vs
