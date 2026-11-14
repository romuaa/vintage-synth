#include "common/ui/ParamMirror.h"

#include "common/ui/models/ParamTarget.h"

#include <algorithm>

namespace vs::ui
{
ParamMirror::ParamMirror (juce::AudioProcessor& processor)
{
    for (auto* p : processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            Entry e;
            e.param = ranged;
            e.last = ranged->getValue();
            entries.push_back (std::move (e));
        }
}

ParamMirror::~ParamMirror()
{
    endAllGestures();
}

int ParamMirror::indexOf (const juce::String& paramId) const
{
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].param->paramID == paramId)
            return static_cast<int> (i);
    return -1;
}

void ParamMirror::subscribe (int index, Listener* l)
{
    jassert (! ticking);
    jassert (index >= 0 && index < size());
    if (index >= 0 && index < size() && l != nullptr)
        entries[static_cast<size_t> (index)].listeners.push_back (l);
}

void ParamMirror::unsubscribe (Listener* l)
{
    jassert (! ticking);
    for (auto& e : entries)
        e.listeners.erase (std::remove (e.listeners.begin(), e.listeners.end(), l), e.listeners.end());
}

void ParamMirror::addIdleClient (IdleClient* c)
{
    jassert (! ticking);
    idleClients.push_back (c);
}

void ParamMirror::removeIdleClient (IdleClient* c)
{
    jassert (! ticking);
    idleClients.erase (std::remove (idleClients.begin(), idleClients.end(), c), idleClients.end());
}

int ParamMirror::tick (double nowMs)
{
    const juce::ScopedValueSetter<bool> guard (ticking, true);
    ++tickCount;
    tickMs = nowMs;

    // Pass 1: load every value; count the changes (a burst sets no markers).
    int changed = 0;
    for (auto& e : entries)
    {
        e.loaded = e.param->getValue(); // one load per parameter and tick
        if (! model::sameBits (e.loaded, e.last))
            ++changed;
    }
    const bool markers = changed <= kBurstLimit;

    // Pass 2: store and notify. Every subscriber of this tick's changes is notified in this call,
    // so a program change is painted as one frame.
    for (auto& e : entries)
    {
        const float v = e.loaded;
        const bool valueChanged = ! model::sameBits (v, e.last);
        if (valueChanged)
        {
            const bool mine = e.ownPending && model::sameBits (v, e.own);
            e.last = v;
            if (! mine && markers)
            {
                e.lastExternalMs = nowMs;
                e.fading = true;
            }
        }
        e.ownPending = false;
        const bool markerMoving = e.fading && nowMs - e.lastExternalMs <= kMarkerHoldMs + kMarkerFadeMs + 50.0;
        if (valueChanged || markerMoving)
            for (auto* l : e.listeners)
                l->mirrorChanged (static_cast<int> (&e - entries.data()), valueChanged);
        if (e.fading && ! markerMoving)
        {
            e.fading = false;
            for (auto* l : e.listeners)
                l->mirrorChanged (static_cast<int> (&e - entries.data()), false); // marker gone
        }
    }

    for (auto* c : idleClients)
        c->mirrorIdle (nowMs);
    return changed;
}

void ParamMirror::beginGesture (int index)
{
    auto& e = entries[static_cast<size_t> (index)];
    jassert (e.depth == 0); // never two begins without an end
    if (e.depth++ == 0)
        e.param->beginChangeGesture();
}

void ParamMirror::setValue (int index, float normalised)
{
    auto& e = entries[static_cast<size_t> (index)];
    jassert (e.depth > 0); // never a value without a gesture
    e.param->setValueNotifyingHost (model::clamp01 (normalised));
    e.own = e.param->getValue();
    e.ownPending = true;
}

void ParamMirror::endGesture (int index)
{
    auto& e = entries[static_cast<size_t> (index)];
    jassert (e.depth > 0);
    if (e.depth > 0 && --e.depth == 0)
        e.param->endChangeGesture();
}

int ParamMirror::openGestures() const noexcept
{
    int n = 0;
    for (const auto& e : entries)
        n += e.depth;
    return n;
}

void ParamMirror::endAllGestures()
{
    for (auto& e : entries)
        while (e.depth > 0)
            endGesture (static_cast<int> (&e - entries.data()));
}

float ParamMirror::markerAlpha (int index, double nowMs) const noexcept
{
    const auto& e = entries[static_cast<size_t> (index)];
    const double age = nowMs - e.lastExternalMs;
    if (age < 0.0 || age > kMarkerHoldMs + kMarkerFadeMs)
        return 0.0f;
    if (age <= kMarkerHoldMs)
        return 1.0f;
    return static_cast<float> (1.0 - (age - kMarkerHoldMs) / kMarkerFadeMs);
}
} // namespace vs::ui
