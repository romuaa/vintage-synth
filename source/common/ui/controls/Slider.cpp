#include "common/ui/controls/Slider.h"

namespace vs::ui
{
namespace
{
bool fineModifier (const juce::ModifierKeys& m) { return m.isShiftDown() || m.isCommandDown(); }
double now() { return juce::Time::getMillisecondCounterHiRes(); }

/** Value interface for screen readers: text from the parameter, set through getValueForText. */
class SliderValue final : public juce::AccessibilityTextValueInterface
{
public:
    SliderValue (Slider& s, ParamHandle& h) : slider (s), handle (h) {}
    bool isReadOnly() const override { return false; }
    juce::String getCurrentValueAsString() const override { return slider.shownText(); }
    void setValueAsString (const juce::String& t) override
    {
        float v = 0.0f;
        if (handle.parse (t, v))
            slider.getModel().typed (v);
    }

private:
    Slider& slider;
    ParamHandle& handle;
};
} // namespace

Slider::Slider (UiContext& c, const juce::String& paramId, const juce::String& labelText, int partIndex, Style s, bool isBipolar,
                Placement placement)
    : BoundComponent (c, paramId, labelText, partIndex, "Slider", placement), style (s), bipolar (isBipolar), model (handle)
{
    valueChanged();
}

Slider::~Slider()
{
    model.cancel();
}

float Slider::shownValue() const
{
    return model.isDragging() ? model.dragValue() : BoundComponent::shownValue();
}

void Slider::valueChanged()
{
    valueText = shownText();
    repaint();
}

void Slider::resized()
{
    auto r = getLocalBounds().toFloat();
    using namespace metrics;
    if (style == Style::Vertical)
    {
        valueArea = r.removeFromTop (static_cast<float> (kLabelRow));
        labelArea = r.removeFromBottom (static_cast<float> (kLabelRow));
        r.reduce (0.0f, static_cast<float> (kThumbW) * 0.5f + 2.0f);
        track = juce::Rectangle<float> (r.getCentreX() - kTrack * 0.5f, r.getY(), static_cast<float> (kTrack), r.getHeight());
        model.setTrack (0.0f, track.getHeight());
    }
    else if (style == Style::Compact)
    {
        labelArea = {};
        valueArea = r.removeFromRight (64.0f);
        r.reduce (static_cast<float> (kThumbW) * 0.5f + 2.0f, 0.0f);
        track = juce::Rectangle<float> (r.getX(), r.getCentreY() - kTrack * 0.5f, r.getWidth(), static_cast<float> (kTrack));
        model.setTrack (track.getX(), track.getWidth());
    }
    else
    {
        auto top = r.removeFromTop (static_cast<float> (kLabelRow));
        labelArea = top;
        valueArea = top;
        r.reduce (static_cast<float> (kThumbW) * 0.5f + 2.0f, 0.0f);
        track = juce::Rectangle<float> (r.getX(), r.getCentreY() - kTrack * 0.5f, r.getWidth(), static_cast<float> (kTrack));
        model.setTrack (track.getX(), track.getWidth());
    }
}

float Slider::pxOf (juce::Point<float> p) const noexcept
{
    // Vertical: increasing value upwards, 0 at the bottom of the track.
    return style == Style::Vertical ? track.getBottom() - p.y : p.x;
}

juce::Rectangle<float> Slider::thumbBounds() const
{
    using namespace metrics;
    const float v = shownValue();
    if (style == Style::Vertical)
    {
        const float y = track.getBottom() - v * track.getHeight();
        return { track.getCentreX() - kThumbH * 0.5f, y - kThumbW * 0.5f, static_cast<float> (kThumbH), static_cast<float> (kThumbW) };
    }
    const float x = model.positionOf (v);
    return { x - kThumbW * 0.5f, track.getCentreY() - kThumbH * 0.5f, static_cast<float> (kThumbW), static_cast<float> (kThumbH) };
}

void Slider::paint (juce::Graphics& g)
{
    using namespace metrics;
    const auto& theme = ctx.theme;
    const float v = shownValue();
    const auto acc = inactive ? accent().withAlpha (0.4f) : accent();

    // Label and value text.
    g.setFont (theme.font (kText));
    if (! labelArea.isEmpty())
    {
        g.setColour (inactive ? colour (Tok::TextDisabled) : colour (Tok::TextSecondary));
        drawFittedText (g, label, labelArea, style == Style::Vertical ? juce::Justification::centred : juce::Justification::centredLeft);
    }
    g.setColour (textColour());
    g.setFont (theme.font (kText, true));
    drawFittedText (g, valueText, valueArea, style == Style::Vertical ? juce::Justification::centred : juce::Justification::centredRight);

    // Track and fill (bipolar: from the centre).
    g.setColour (colour (Tok::BgTrack).withAlpha (inactive ? 0.4f : 1.0f));
    g.fillRoundedRectangle (track, kTrack * 0.5f);
    g.setColour (acc);
    if (style == Style::Vertical)
    {
        const float y = track.getBottom() - v * track.getHeight();
        const float y0 = bipolar ? track.getCentreY() : track.getBottom();
        g.fillRoundedRectangle (juce::Rectangle<float>::leftTopRightBottom (track.getX(), juce::jmin (y, y0), track.getRight(), juce::jmax (y, y0)),
                                kTrack * 0.5f);
    }
    else
    {
        const float x = model.positionOf (v);
        const float x0 = bipolar ? track.getCentreX() : track.getX();
        g.fillRoundedRectangle (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (x, x0), track.getY(), juce::jmax (x, x0), track.getBottom()),
                                kTrack * 0.5f);
    }

    const auto thumb = thumbBounds();
    g.setColour (inactive ? colour (Tok::TextDisabled) : colour (Tok::TextPrimary));
    g.fillRoundedRectangle (thumb, 3.0f);
    g.setColour (acc);
    g.drawRoundedRectangle (thumb.reduced (0.5f), 3.0f, 1.0f);

    if (markerAlpha > 0.0f)
    {
        // The badge sits in the label row, left of the value text (fixed slot: never measured).
        const juce::Rectangle<float> badge = style == Style::Horizontal && valueArea.getWidth() > 160.0f
                                                 ? juce::Rectangle<float> (valueArea.getRight() - 128.0f, valueArea.getY() + 2.0f, 40.0f, 14.0f)
                                                 : juce::Rectangle<float>();
        drawExternalMarker (g, thumb, badge, markerAlpha, theme);
    }
    if (hasKeyboardFocus (false))
        drawFocusRing (g, thumb, 3.0f);
}

void Slider::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showContextMenu();
        return;
    }
    if (valueArea.contains (e.position) && ! track.expanded (0.0f, 6.0f).contains (e.position) && style != Style::Vertical)
    {
        showTypedEntry (valueArea.toNearestInt());
        return;
    }
    const bool onThumb = thumbBounds().expanded (static_cast<float> (metrics::kHandleRadius) - 6.0f).contains (e.position);
    model.mouseDown (pxOf (e.position), onThumb, fineModifier (e.mods));
    valueChanged();
}

void Slider::mouseDrag (const juce::MouseEvent& e)
{
    if (! model.isDragging())
        return;
    model.mouseDrag (pxOf (e.position), fineModifier (e.mods));
    valueChanged();
}

void Slider::mouseUp (const juce::MouseEvent&)
{
    if (! model.isDragging())
        return;
    model.mouseUp();
    // A contested drag writes nothing here: the next tick shows whatever the parameter holds.
    valueChanged();
}

void Slider::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    model.cancel();
    model.doubleClick();
    valueChanged();
}

void Slider::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // Over the control only (never scrolls a page). About 0.125 per notch on most mice.
    const float delta = (std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
    model.wheel (delta * 8.0f, fineModifier (e.mods), now());
    valueChanged();
}

bool Slider::keyPressed (const juce::KeyPress& k)
{
    const bool fine = fineModifier (k.getModifiers());
    const int code = k.getKeyCode();
    model::Key key;
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey)
        key = model::Key::Increase;
    else if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey)
        key = model::Key::Decrease;
    else if (code == juce::KeyPress::pageUpKey)
        key = model::Key::PageUp;
    else if (code == juce::KeyPress::pageDownKey)
        key = model::Key::PageDown;
    else if (code == juce::KeyPress::homeKey)
        key = model::Key::Home;
    else if (code == juce::KeyPress::endKey)
        key = model::Key::End;
    else if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
        key = model::Key::Reset;
    else if (code == juce::KeyPress::returnKey)
    {
        showTypedEntry (style == Style::Vertical ? getLocalBounds() : valueArea.toNearestInt());
        return true;
    }
    else
        return false;
    model.key (key, fine, now());
    valueChanged();
    return true;
}

std::unique_ptr<juce::AccessibilityHandler> Slider::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::slider, juce::AccessibilityActions{},
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<SliderValue> (*this, handle) });
}
} // namespace vs::ui
