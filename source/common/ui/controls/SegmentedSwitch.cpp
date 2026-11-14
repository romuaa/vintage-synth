#include "common/ui/controls/SegmentedSwitch.h"

namespace vs::ui
{
namespace
{
double now() { return juce::Time::getMillisecondCounterHiRes(); }

int choiceCount (ParamHandle& h)
{
    const int steps = h.numSteps();
    return steps >= 2 ? steps : 2;
}

juce::StringArray defaultCaptions (ParamHandle& h)
{
    if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (&h.param()))
        return c->choices;
    juce::StringArray s;
    const int n = choiceCount (h);
    for (int i = 0; i < n; ++i)
        s.add (h.param().getText (static_cast<float> (i) / static_cast<float> (n - 1), 1024));
    return s;
}

/** Screen readers: the full choice text as the value. */
class ChoiceValue final : public juce::AccessibilityTextValueInterface
{
public:
    ChoiceValue (BoundComponent& c, ParamHandle& h) : comp (c), handle (h) {}
    bool isReadOnly() const override { return false; }
    juce::String getCurrentValueAsString() const override { return comp.shownText(); }
    void setValueAsString (const juce::String& t) override
    {
        float v = 0.0f;
        if (handle.parse (t, v))
            model::writeOnce (handle, v);
    }

private:
    BoundComponent& comp;
    ParamHandle& handle;
};

model::Key keyFor (int code, bool& known)
{
    known = true;
    if (code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey)
        return model::Key::Increase;
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::downKey)
        return model::Key::Decrease;
    if (code == juce::KeyPress::homeKey)
        return model::Key::Home;
    if (code == juce::KeyPress::endKey)
        return model::Key::End;
    known = false;
    return model::Key::Reset;
}
} // namespace

SegmentedSwitch::SegmentedSwitch (UiContext& c, const juce::String& paramId, const juce::String& labelText, int partIndex,
                                  juce::StringArray aliases, Placement placement)
    : BoundComponent (c, paramId, labelText, partIndex, "SegmentedSwitch", placement, 0),
      captions (aliases.isEmpty() ? defaultCaptions (handle) : aliases),
      model (handle, choiceCount (handle))
{
    // The registry entry was made by the base; record the caption count now that it is known.
    ctx.registry.setCaptions (this, captions.size());
    jassert (captions.size() == model.numSegments()); // alias count must equal the choice count
    refresh();
}

SegmentedSwitch::~SegmentedSwitch()
{
    model.cancel();
}

float SegmentedSwitch::shownValue() const
{
    // While this switch writes, the live value (the next tick shows it everywhere else too).
    return model.isInGesture() ? handle.value() : BoundComponent::shownValue();
}

int SegmentedSwitch::litSegment() const
{
    return model.indexOf (drawnValue());
}

bool SegmentedSwitch::showsMirroredValue (juce::String& why) const
{
    if (! BoundComponent::showsMirroredValue (why))
        return false;
    const int expected = model.indexOf (ctx.mirror.value (handle.getIndex()));
    if (litSegment() != expected)
    {
        why << getComponentID() << ": lit segment " << litSegment() << ", expected " << expected;
        return false;
    }
    return true;
}

void SegmentedSwitch::resized()
{
    auto r = getLocalBounds().toFloat();
    labelArea = label.isNotEmpty() ? r.removeFromTop (static_cast<float> (metrics::kLabelRow)) : juce::Rectangle<float>();
    row = r.withSizeKeepingCentre (r.getWidth(), juce::jmin (r.getHeight(), static_cast<float> (metrics::kSegment)));
}

juce::Rectangle<float> SegmentedSwitch::segmentBounds (int index) const
{
    const float w = row.getWidth() / static_cast<float> (model.numSegments());
    return { row.getX() + w * static_cast<float> (index), row.getY(), w, row.getHeight() };
}

void SegmentedSwitch::paint (juce::Graphics& g)
{
    using namespace metrics;
    if (! labelArea.isEmpty())
    {
        g.setFont (ctx.theme.font (kText));
        g.setColour (inactive ? colour (Tok::TextDisabled) : colour (Tok::TextSecondary));
        drawFittedText (g, label, labelArea, juce::Justification::centredLeft);
    }
    g.setColour (colour (Tok::BgWindow));
    g.fillRoundedRectangle (row, 4.0f);
    const int lit = litSegment();
    const auto acc = inactive ? accent().withAlpha (0.4f) : accent();
    for (int i = 0; i < model.numSegments(); ++i)
    {
        const auto seg = segmentBounds (i).reduced (1.0f);
        const bool on = i == lit;
        if (on)
        {
            g.setColour (acc);
            g.fillRoundedRectangle (seg, 3.0f);
        }
        g.setColour (on ? colour (Tok::TextOnAccent) : textColour());
        g.setFont (ctx.theme.font (kText, on));
        drawFittedText (g, captions[i], seg.reduced (2.0f, 0.0f), juce::Justification::centred);
        if (i > 0 && ! on && i - 1 != lit)
        {
            g.setColour (colour (Tok::BgTrack));
            g.drawVerticalLine (static_cast<int> (seg.getX() - 1.0f), seg.getY() + 6.0f, seg.getBottom() - 6.0f);
        }
    }
    g.setColour (colour (Tok::BgTrack));
    g.drawRoundedRectangle (row.reduced (0.5f), 4.0f, 1.0f);
    if (markerAlpha > 0.0f)
        drawExternalMarker (g, segmentBounds (lit).reduced (2.0f), {}, markerAlpha, ctx.theme, false);
    if (hasKeyboardFocus (false))
        drawFocusRing (g, row, 4.0f);
}

void SegmentedSwitch::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showContextMenu();
        return;
    }
    const int i = model.indexAt (e.position.x - row.getX(), row.getWidth());
    if (i >= 0 && row.expanded (0.0f, 4.0f).contains (e.position))
    {
        model.press (i);
        refresh();
    }
}

void SegmentedSwitch::mouseUp (const juce::MouseEvent&)
{
    model.release();
    refresh();
}

void SegmentedSwitch::mouseDoubleClick (const juce::MouseEvent&)
{
    // The second click of a double-click selects its segment like any click (mouseDown did);
    // reset to default is on the context menu.
}

void SegmentedSwitch::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    // As Slider: about 0.125 per notch, reversed scrolling honoured, fractions accumulate.
    const float d = (std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
    model.wheel (d * 8.0f, now());
    refresh();
}

bool SegmentedSwitch::keyPressed (const juce::KeyPress& k)
{
    bool known = false;
    const auto key = keyFor (k.getKeyCode(), known);
    if (! known)
        return false;
    model.key (key, now());
    refresh();
    return true;
}

std::unique_ptr<juce::AccessibilityHandler> SegmentedSwitch::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::group, juce::AccessibilityActions{},
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<ChoiceValue> (*this, handle) });
}

// ---- Toggle ----

Toggle::Toggle (UiContext& c, const juce::String& paramId, const juce::String& caption, int partIndex, Placement placement)
    : BoundComponent (c, paramId, caption, partIndex, "Toggle", placement), model (handle)
{
    refresh();
}

Toggle::~Toggle()
{
    model.cancel();
}

float Toggle::shownValue() const
{
    return model.isInGesture() ? handle.value() : BoundComponent::shownValue();
}

bool Toggle::isOn() const
{
    return model::stepIndex (drawnValue(), 2) == 1;
}

bool Toggle::showsMirroredValue (juce::String& why) const
{
    if (! BoundComponent::showsMirroredValue (why))
        return false;
    if (isOn() != (model::stepIndex (ctx.mirror.value (handle.getIndex()), 2) == 1))
    {
        why << getComponentID() << ": toggle state differs";
        return false;
    }
    return true;
}

void Toggle::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = isOn();
    g.setColour (on ? (inactive ? accent().withAlpha (0.4f) : accent()) : colour (Tok::BgWindow));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (colour (Tok::BgTrack));
    g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    g.setColour (on ? colour (Tok::TextOnAccent) : textColour());
    g.setFont (ctx.theme.font (metrics::kText, on));
    drawFittedText (g, label, r.reduced (4.0f, 0.0f), juce::Justification::centred);
    if (markerAlpha > 0.0f)
        drawExternalMarker (g, r.reduced (2.0f), {}, markerAlpha, ctx.theme, false);
    if (hasKeyboardFocus (false))
        drawFocusRing (g, r, 4.0f);
}

void Toggle::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showContextMenu();
        return;
    }
    model.press();
    refresh();
}

void Toggle::mouseUp (const juce::MouseEvent&)
{
    model.release();
    refresh();
}

void Toggle::valueChanged()
{
    repaint();
    if (auto* h = getAccessibilityHandler())
        h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
}

bool Toggle::keyPressed (const juce::KeyPress& k)
{
    const int code = k.getKeyCode();
    if (code == juce::KeyPress::spaceKey || code == juce::KeyPress::returnKey)
    {
        model.press();
        model.release();
        refresh();
        return true;
    }
    bool known = false;
    const auto key = keyFor (code, known);
    if (! known)
        return false;
    model.key (key, now());
    refresh();
    return true;
}

std::unique_ptr<juce::AccessibilityHandler> Toggle::createAccessibilityHandler()
{
    juce::AccessibilityActions actions;
    juce::Component::SafePointer<Toggle> self (this);
    actions.addAction (juce::AccessibilityActionType::toggle, [self] {
        if (self != nullptr)
        {
            self->model.press();
            self->model.release();
            self->refresh();
        }
    });
    return std::make_unique<CheckableHandler> (*this, juce::AccessibilityRole::toggleButton, std::move (actions), [this] { return isOn(); },
                                               juce::AccessibilityHandler::Interfaces { std::make_unique<ChoiceValue> (*this, handle) });
}
} // namespace vs::ui
