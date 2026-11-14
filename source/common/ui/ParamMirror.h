#pragma once

// The editor's parameter mirror (docs/architecture.md 9.1.5): the live-automation rule.
//
// One entry per host parameter, built once. tick() loads every parameter's normalised value and
// notifies the components bound to a parameter whose value changed (bitwise) since the last tick,
// whatever changed it: host automation, MIDI, a program or state load, or this editor. A burst of
// changes between two ticks is one notification with the latest value. No parameter listeners
// (they run on whatever thread changed the parameter, often the audio thread): message thread only.
//
// It also owns the host gestures (begin / value / end, with a depth counter) and the external-change
// marker: "changed by something other than this editor in the last 1.5 s", fading over 0.5 s;
// a tick in which more than 8 parameters change (a program load, a restore, Init) sets none.

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

namespace vs::ui
{
class ParamMirror
{
public:
    /** A component bound to parameters. Notified on the message thread, inside tick(). */
    class Listener
    {
    public:
        virtual ~Listener() = default;
        /** `valueChanged`: the parameter took a new value; otherwise only its marker fades. */
        virtual void mirrorChanged (int index, bool valueChanged) = 0;
    };

    /** Called on every tick (timed gestures of wheel and keys end 300 ms after the last input). */
    class IdleClient
    {
    public:
        virtual ~IdleClient() = default;
        virtual void mirrorIdle (double nowMs) = 0;
    };

    static constexpr int kBurstLimit = 8;
    static constexpr double kMarkerHoldMs = 1500.0, kMarkerFadeMs = 500.0;

    explicit ParamMirror (juce::AudioProcessor& processor);
    ~ParamMirror();

    int size() const noexcept { return static_cast<int> (entries.size()); }
    /** Index of a parameter ID; -1 if unknown. */
    int indexOf (const juce::String& paramId) const;
    juce::RangedAudioParameter& parameter (int index) const noexcept { return *entries[static_cast<size_t> (index)].param; }
    /** The value the last tick saw (what every binding shows). */
    float value (int index) const noexcept { return entries[static_cast<size_t> (index)].last; }

    /** Bindings are registered when the components are built, never during a tick. */
    void subscribe (int index, Listener* l);
    void unsubscribe (Listener* l);
    void addIdleClient (IdleClient* c);
    void removeIdleClient (IdleClient* c);

    /** One pass: notifies the bindings of every changed parameter; returns how many changed. */
    int tick (double nowMs);
    int ticks() const noexcept { return tickCount; }
    /** The time the current (or last) tick was given. */
    double lastTickTime() const noexcept { return tickMs; }

    // ---- Gestures (ParamHandle). Never a value without a gesture; never two begins. ----
    void beginGesture (int index);
    void setValue (int index, float normalised);
    void endGesture (int index);
    int gestureDepth (int index) const noexcept { return entries[static_cast<size_t> (index)].depth; }
    int openGestures() const noexcept;
    /** Ends every open gesture (the editor's destructor). */
    void endAllGestures();

    /** 1 while the marker shows, fading to 0 (see above). */
    float markerAlpha (int index, double nowMs) const noexcept;

private:
    struct Entry
    {
        juce::RangedAudioParameter* param = nullptr;
        float last = 0.0f;
        float loaded = 0.0f;     // this tick's load
        float own = 0.0f;       // the value this editor wrote last since the previous tick
        bool ownPending = false;
        int depth = 0;
        double lastExternalMs = -1.0e12;
        bool fading = false;
        std::vector<Listener*> listeners;
    };

    std::vector<Entry> entries;
    std::vector<IdleClient*> idleClients;
    bool ticking = false;
    int tickCount = 0;
    double tickMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE (ParamMirror)
};
} // namespace vs::ui
