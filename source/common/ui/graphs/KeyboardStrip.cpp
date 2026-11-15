#include "common/ui/graphs/KeyboardStrip.h"

namespace vs::ui
{
namespace
{
double now() { return juce::Time::getMillisecondCounterHiRes(); }

constexpr float kPillWidth = 92.0f, kPillHeight = 22.0f;

juce::Colour whiteTint (int part) { return accentFor (part).interpolatedWith (juce::Colours::white, 0.6f); }
juce::Colour blackTint (int part) { return accentFor (part).interpolatedWith (colour (Tok::BgWindow), 0.62f); }

/** Value interface for screen readers: the note name from the parameter, set through its parser. */
class StripValue final : public juce::AccessibilityTextValueInterface
{
public:
    StripValue (KeyboardStrip& s, ParamHandle& h) : strip (s), handle (h) {}
    bool isReadOnly() const override { return false; }
    juce::String getCurrentValueAsString() const override { return strip.shownText(); }
    void setValueAsString (const juce::String& t) override
    {
        float v = 0.0f;
        if (handle.parse (t, v))
            strip.getModel().typed (v);
    }

private:
    KeyboardStrip& strip;
    ParamHandle& handle;
};
} // namespace

KeyboardStrip::KeyboardStrip (UiContext& c, const UiFeed& f, UiNotes* n, const juce::String& splitId, const juce::String& modeId)
    : BoundComponent (c, splitId, "Split key", -1, "KeyboardStrip"), feed (f), notes (n), model (handle), fonts (c.theme)
{
    dragHint = juce::String::fromUTF8 ("\xe2\x97\x82 drag \xe2\x96\xb8");
    setAccent (colour (Tok::AccentGlobal));
    modeIndex = ctx.mirror.indexOf (modeId);
    jassert (modeIndex >= 0);
    if (modeIndex >= 0)
    {
        ctx.mirror.subscribe (modeIndex, this);
        mode = model::stepIndex (ctx.mirror.value (modeIndex), 3);
    }
    BoundComponent::setInactive (mode != 2);
    refresh();
}

KeyboardStrip::~KeyboardStrip()
{
    releaseNote();
    model.cancel();
}

void KeyboardStrip::cancelInteraction()
{
    releaseNote();
    model.cancel();
}

void KeyboardStrip::visibilityChanged()
{
    if (! isVisible())
        releaseNote();
}

bool KeyboardStrip::shownInTree() const noexcept
{
    // Visible up to (not including) the editor itself: a page that is switched away hides an
    // ancestor. The editor's own window is the host's (closing it destroys the strip).
    for (const juce::Component* c = this; c != nullptr && c->getParentComponent() != nullptr; c = c->getParentComponent())
        if (! c->isVisible())
            return false;
    return true;
}

void KeyboardStrip::playAt (juce::Point<float> p)
{
    const int note = geometry.noteAt (p.x, p.y);
    if (note == playing)
        return;
    releaseNote();
    if (note < 0)
        return;
    playing = note;
    playingVel = geometry.velocityAt (note, p.y);
    if (notes != nullptr)
        notes->noteOn (note, playingVel);
}

void KeyboardStrip::releaseNote()
{
    if (playing < 0)
        return;
    if (notes != nullptr)
        notes->noteOff (playing);
    playing = -1;
    playingVel = 0;
}

float KeyboardStrip::shownValue() const
{
    return model.isDragging() ? model::SplitKeyModel::valueOf (model.dragNote()) : BoundComponent::shownValue();
}

bool KeyboardStrip::showsMirroredValue (juce::String& why) const
{
    if (modeIndex >= 0 && mode != model::stepIndex (ctx.mirror.value (modeIndex), 3))
    {
        why << getComponentID() << ": draws key mode " << mode << ", parameter says " << ctx.mirror.value (modeIndex);
        return false;
    }
    return BoundComponent::showsMirroredValue (why);
}

void KeyboardStrip::mirrorChanged (int index, bool valueChanged)
{
    if (index == modeIndex)
    {
        mode = model::stepIndex (ctx.mirror.value (modeIndex), 3);
        BoundComponent::setInactive (mode != 2);
        refresh(); // the bars and the keys' colours follow the mode (setInactive refreshes only when it changes)
        if (index != getParamIndex())
            return;
    }
    if (index == getParamIndex() && ownNoteValid && model::SplitKeyModel::noteOf (ctx.mirror.value (index)) != ownNote)
        ownNoteValid = false; // changed by something else (automation, a program): the window follows it again
    BoundComponent::mirrorChanged (index, valueChanged);
}

void KeyboardStrip::valueChanged()
{
    const int note = model::SplitKeyModel::noteOf (drawnValue());
    splitNote = note;
    // The window follows the split key (an octave to spare), except for a note this strip's own
    // drag just set: the keys must not slide under the pointer (mirrorChanged ends that).
    if (! ownNoteValid || ! geometry.inWindow (note))
        geometry.setWindow (model::KeyboardGeometry::windowFor (note));
    prepare();
    repaint();
}

void KeyboardStrip::resized()
{
    using namespace metrics;
    auto r = getLocalBounds().toFloat();
    pill = {};
    r.removeFromTop (static_cast<float> (kPillRow));
    barArea = r.removeFromTop (static_cast<float> (kBarRow));
    r.removeFromTop (2.0f);
    keys = r;
    geometry.setSize (keys.getWidth(), keys.getHeight());
    prepare();
}

void KeyboardStrip::prepare()
{
    // Window labels, bar texts and the marker pill: strings are made here, never in paint().
    octaveLabels.clear();
    for (int n = geometry.low(); n <= geometry.high(); n += 12)
        octaveLabels.add (juce::String (model::noteLabel (n)));
    for (int p = 0; p < 2; ++p)
    {
        auto& bar = bars[static_cast<size_t> (p)];
        bar.span = model::partSpan (p, mode, splitNote, geometry.low(), geometry.high());
        bar.text = juce::String (p == 0 ? "LOWER" : "UPPER");
        if (! bar.span.empty())
            bar.text << " " << juce::String (model::noteLabel (bar.span.first)) << " - " << juce::String (model::noteLabel (bar.span.last));
        else if (p == 1 && mode == 0)
            bar.text << " not played in Whole";
    }
    pillText = "Split " + drawnText();
    const float x = keys.getX() + geometry.markerX (splitNote);
    const float w = static_cast<float> (getWidth());
    pill = juce::Rectangle<float> (juce::jlimit (0.0f, juce::jmax (0.0f, w - kPillWidth), x - kPillWidth * 0.5f), 2.0f, kPillWidth, kPillHeight);
    markerTriangle.clear();
    markerTriangle.addTriangle (x - 6.0f, keys.getBottom() + 5.0f, x + 6.0f, keys.getBottom() + 5.0f, x, keys.getBottom() - 4.0f);
}

bool KeyboardStrip::has (const Masks& m, int slot, int note) noexcept
{
    return note >= 0 && note < 128 && ((m[static_cast<size_t> (slot * 2 + (note >> 6))] >> (note & 63)) & 1u) != 0;
}

bool KeyboardStrip::drawsHeld (int partIndex, int note) const noexcept { return has (masks, partIndex & 1, note); }
bool KeyboardStrip::drawsSounding (int partIndex, int note) const noexcept { return has (masks, 2 + (partIndex & 1), note); }

juce::Rectangle<float> KeyboardStrip::keyBounds (int note) const noexcept
{
    const auto k = geometry.keyRect (note);
    return { keys.getX() + k.x, keys.getY() + k.y, k.w, k.h };
}

void KeyboardStrip::idle (double nowMs)
{
    model.idle (nowMs);
    // A page switch hides an ancestor (and cancels the page); this also catches any other way the
    // strip stops being shown while a key is down. Offs that found the queue full go out now.
    if (playing >= 0 && ! shownInTree())
    {
        playGesture = false;
        releaseNote();
    }
    if (notes != nullptr)
        notes->retryPendingOffs();
    // The feed: held and sounding keys of both parts. Repaint only when a mask changed.
    Masks m {};
    for (int p = 0; p < 2; ++p)
        for (int w = 0; w < 2; ++w)
        {
            m[static_cast<size_t> (p * 2 + w)] = feed.part[p].held[w].load (std::memory_order_relaxed);
            m[static_cast<size_t> (4 + p * 2 + w)] = feed.part[p].sounding[w].load (std::memory_order_relaxed);
        }
    if (m != masks)
    {
        masks = m;
        ++repaints;
        repaint (keys.toNearestInt());
    }
}

juce::Point<float> KeyboardStrip::local (const juce::MouseEvent& e) const noexcept
{
    return e.position - keys.getPosition();
}

void KeyboardStrip::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showContextMenu();
        return;
    }
    // The pill moves the split key (one gesture, dragged by its grab point); the keys play.
    if (pill.contains (e.position))
    {
        releaseNote();
        playGesture = false;
        grabOffset = e.position.x - (keys.getX() + geometry.markerX (splitNote));
        ownNote = splitNote;
        ownNoteValid = true;
        model.press (splitNote);
        refresh();
        return;
    }
    playGesture = keys.contains (e.position);
    if (playGesture)
        playAt (local (e));
}

void KeyboardStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (playGesture)
    {
        playAt (local (e)); // onto another key: the old note off, the new one on; off the keys: silent
        return;
    }
    if (! model.isDragging())
        return;
    ownNote = geometry.markerNoteNearest (local (e).x - grabOffset);
    model.drag (ownNote);
    refresh();
}

void KeyboardStrip::mouseUp (const juce::MouseEvent&)
{
    if (playGesture)
    {
        playGesture = false;
        releaseNote();
        return;
    }
    if (! model.isDragging())
        return;
    model.release();
    refresh();
}

void KeyboardStrip::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Only the pill resets the split key: a double-click on the keys plays the key twice.
    if (e.mods.isPopupMenu() || ! pill.contains (e.position))
        return;
    model.cancel();
    ownNoteValid = false;
    model.doubleClick();
    refresh();
}

void KeyboardStrip::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const float delta = (std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
    ownNoteValid = false;
    model.wheel (delta * 8.0f, now());
    refresh();
}

bool KeyboardStrip::keyPressed (const juce::KeyPress& k)
{
    const int code = k.getKeyCode();
    ownNoteValid = false;
    if (code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey)
        model.nudge (1, now());
    else if (code == juce::KeyPress::leftKey || code == juce::KeyPress::downKey)
        model.nudge (-1, now());
    else if (code == juce::KeyPress::pageUpKey)
        model.nudge (12, now());
    else if (code == juce::KeyPress::pageDownKey)
        model.nudge (-12, now());
    else if (code == juce::KeyPress::homeKey)
        model.toEnd (false, now());
    else if (code == juce::KeyPress::endKey)
        model.toEnd (true, now());
    else if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
        model.doubleClick();
    else if (code == juce::KeyPress::returnKey)
    {
        showTypedEntry (pill.toNearestInt());
        return true;
    }
    else
        return false;
    refresh();
    return true;
}

std::unique_ptr<juce::AccessibilityHandler> KeyboardStrip::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::slider, juce::AccessibilityActions{},
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<StripValue> (*this, handle) });
}

// ---- Painting ----

void KeyboardStrip::paintKey (juce::Graphics& g, int note) const
{
    const auto r = keyBounds (note);
    const bool black = model::isBlackKey (note);
    const int owner = model::ownerMask (mode, splitNote, note);
    const auto tint = [&] (int partIndex) { return black ? blackTint (partIndex) : whiteTint (partIndex); };
    if (owner == 3)
    {
        g.setColour (tint (0));
        g.fillRect (r.withHeight (r.getHeight() * 0.5f));
        g.setColour (tint (1));
        g.fillRect (r.withTrimmedTop (r.getHeight() * 0.5f));
    }
    else
    {
        g.setColour (tint (owner == 2 ? 1 : 0));
        g.fillRect (r);
    }
    g.setColour (colour (Tok::BgWindow));
    g.drawRect (r, 1.0f);
}

void KeyboardStrip::paintDots (juce::Graphics& g, int note) const
{
    const bool held[2] { drawsHeld (0, note), drawsHeld (1, note) };
    const bool sounding[2] { drawsSounding (0, note), drawsSounding (1, note) };
    if (! (held[0] || held[1] || sounding[0] || sounding[1]))
        return;
    const auto r = keyBounds (note);
    const bool black = model::isBlackKey (note);
    const float y = r.getBottom() - (black ? 11.0f : 26.0f);
    const bool both = (held[0] || sounding[0]) && (held[1] || sounding[1]);
    const float radius = black ? 3.5f : 5.0f;
    for (int p = 0; p < 2; ++p)
    {
        if (! (held[p] || sounding[p]))
            continue;
        // Lower stands left of centre, Upper right, when both play the key: the part is a position, not a colour.
        const float x = r.getCentreX() + (both ? (p == 0 ? -1.0f : 1.0f) * radius * 0.9f : 0.0f);
        const juce::Rectangle<float> dot (x - radius, y - radius, 2.0f * radius, 2.0f * radius);
        if (held[p])
        {
            g.setColour (colour (Tok::TextPrimary));
            g.fillEllipse (dot.expanded (1.0f));
            g.setColour (colour (Tok::BgWindow));
            g.fillEllipse (dot);
        }
        if (sounding[p])
        {
            g.setColour (colour (Tok::TextPrimary));
            g.drawEllipse (dot.expanded (held[p] ? 3.0f : 1.0f), 2.0f);
            g.setColour (colour (Tok::BgWindow));
            g.drawEllipse (dot.expanded (held[p] ? 4.5f : 2.5f), 1.0f);
        }
    }
}

void KeyboardStrip::paint (juce::Graphics& g)
{
    using namespace metrics;
    const bool split = mode == 2;

    // Bars over the keys: each part's range. Dual: two stacked bars over every key.
    for (int p = 0; p < 2; ++p)
    {
        const auto& bar = bars[static_cast<size_t> (p)];
        if (bar.span.empty())
        {
            if (mode == 0 && p == 1) // Upper does not play in Whole mode: a quiet line says so
            {
                g.setColour (colour (Tok::TextDisabled));
                g.setFont (fonts.small);
                drawFittedText (g, bar.text, barArea.withTrimmedLeft (4.0f).withTop (barArea.getCentreY()), juce::Justification::centredLeft);
            }
            continue;
        }
        auto row = barArea;
        if (mode == 1)
            row = p == 0 ? barArea.withHeight (barArea.getHeight() * 0.5f - 1.0f) : barArea.withTrimmedTop (barArea.getHeight() * 0.5f + 1.0f);
        else if (mode == 0)
            row = barArea.withHeight (barArea.getHeight() * 0.5f);
        // In Split the two bars meet at the marker.
        const float markerEdge = keys.getX() + geometry.markerX (splitNote);
        const float x0 = (mode == 2 && p == 1) ? markerEdge : keyBounds (bar.span.first).getX();
        const float x1 = (mode == 2 && p == 0) ? markerEdge : keyBounds (bar.span.last).getRight();
        const auto r = juce::Rectangle<float>::leftTopRightBottom (x0, row.getY(), x1, row.getBottom());
        g.setColour (accentFor (p));
        g.fillRect (r);
        g.setColour (colour (Tok::TextOnAccent));
        g.setFont (mode == 1 ? fonts.smallBold : fonts.textBold);
        drawFittedText (g, bar.text, r.reduced (6.0f, 0.0f), juce::Justification::centredLeft);
    }

    // Keys: white first, then the black keys over them.
    for (int n = geometry.low(); n <= geometry.high(); ++n)
        if (! model::isBlackKey (n))
            paintKey (g, n);
    for (int n = geometry.low(); n <= geometry.high(); ++n)
        if (model::isBlackKey (n))
            paintKey (g, n);
    for (int n = geometry.low(); n <= geometry.high(); ++n)
        paintDots (g, n);

    // Octave names on the C keys.
    g.setColour (colour (Tok::BgWindow));
    g.setFont (fonts.smallBold);
    for (int i = 0, n = geometry.low(); n <= geometry.high(); n += 12, ++i)
    {
        const auto r = keyBounds (n);
        g.drawText (octaveLabels[i], r.withTop (r.getBottom() - 16.0f).withTrimmedLeft (3.0f), juce::Justification::centredLeft, false);
    }

    // The split marker: pill above, line through bars and keys, a triangle under the keys.
    const float mx = keys.getX() + geometry.markerX (splitNote);
    const float alpha = split ? 1.0f : 0.45f;
    g.setColour (colour (Tok::TextPrimary).withAlpha (alpha));
    g.fillRect (mx - 1.0f, pill.getBottom(), 2.0f, keys.getBottom() - pill.getBottom());
    g.fillPath (markerTriangle);
    g.setColour (colour (Tok::TextPrimary).withAlpha (alpha));
    g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
    g.setColour (colour (Tok::TextOnAccent));
    g.setFont (fonts.textBold);
    drawFittedText (g, pillText, pill, juce::Justification::centred);
    g.setColour (colour (Tok::TextSecondary));
    g.setFont (fonts.small);
    const bool roomRight = pill.getRight() + 72.0f < static_cast<float> (getWidth());
    g.drawText (dragHint,
                roomRight ? juce::Rectangle<float> (pill.getRight() + 6.0f, pill.getY(), 64.0f, pill.getHeight())
                          : juce::Rectangle<float> (pill.getX() - 70.0f, pill.getY(), 64.0f, pill.getHeight()),
                roomRight ? juce::Justification::centredLeft : juce::Justification::centredRight, false);

    if (markerAlpha > 0.0f)
        drawExternalMarker (g, pill, {}, markerAlpha, ctx.theme, false);
    if (hasKeyboardFocus (false))
        drawFocusRing (g, pill, pill.getHeight() * 0.5f);
}
} // namespace vs::ui
