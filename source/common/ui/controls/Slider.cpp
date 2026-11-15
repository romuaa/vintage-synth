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
    refresh();
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
    prepareText();
    repaint();
}

void Slider::prepareText()
{
    const auto just = [&] (bool isLabel) {
        return style == Style::Vertical ? juce::Justification::centred : (isLabel ? juce::Justification::centredLeft : juce::Justification::centredRight);
    };
    // The label gives up the badge slot while the marker shows, so its area is part of what it depends on.
    const auto labelRect = labelBounds();
    if (labelRect != preparedLabelArea || ! labelPrepared.isPrepared())
    {
        preparedLabelArea = labelRect;
        if (! labelArea.isEmpty())
            labelPrepared.setLine (label, ctx.theme.fonts().text, labelRect, just (true));
        else
            labelPrepared.clear();
    }
    if (valueArea != preparedValueArea || drawnString != preparedValue || ! valuePrepared.isPrepared())
    {
        preparedValueArea = valueArea;
        preparedValue = drawnString;
        valuePrepared.setLine (drawnString, ctx.theme.fonts().textBold, valueArea, just (false));
    }
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
        // Fixed slots, never measured: value text right (84 px), the AUTO badge left of it (48 px,
        // only while the marker shows), the label in what is left (truncated with an ellipsis).
        valueArea = top.removeFromRight (juce::jmin (valueSlot, top.getWidth() * 0.5f));
        badgeArea = top.getWidth() >= 100.0f ? top.withLeft (top.getRight() - 48.0f).withSizeKeepingCentre (44.0f, 14.0f) : juce::Rectangle<float>();
        labelArea = top;
        r.reduce (static_cast<float> (kThumbW) * 0.5f + 2.0f, 0.0f);
        track = juce::Rectangle<float> (r.getX(), r.getCentreY() - kTrack * 0.5f, r.getWidth(), static_cast<float> (kTrack));
        model.setTrack (track.getX(), track.getWidth());
    }
    preparedLabelArea = {}; // the areas moved: lay the text out again
    preparedValueArea = {};
    prepareText();
}

float Slider::pxOf (juce::Point<float> p) const noexcept
{
    // Vertical: increasing value upwards, 0 at the bottom of the track.
    return style == Style::Vertical ? track.getBottom() - p.y : p.x;
}

juce::Rectangle<float> Slider::labelBounds() const noexcept
{
    // While the badge shows, the label gives up the badge slot (its slot is right of the label).
    if (markerAlpha > 0.0f && ! badgeArea.isEmpty() && style == Style::Horizontal)
        return labelArea.withRight (juce::jmin (labelArea.getRight(), badgeArea.getX() - 2.0f));
    return labelArea;
}

juce::Rectangle<float> Slider::badgeBounds() const noexcept
{
    return markerAlpha > 0.0f && style == Style::Horizontal ? badgeArea : juce::Rectangle<float>();
}

juce::Rectangle<float> Slider::thumbBounds() const
{
    using namespace metrics;
    const float v = drawnValue();
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
    const float v = drawnValue();
    const auto acc = inactive ? accent().withAlpha (0.4f) : accent();

    // Label and value text (laid out when the value, the size or the marker changed).
    if (! labelArea.isEmpty())
    {
        g.setColour (inactive ? colour (Tok::TextDisabled) : colour (Tok::TextSecondary));
        labelPrepared.draw (g);
    }
    g.setColour (textColour());
    valuePrepared.draw (g);

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
        drawExternalMarker (g, thumb, badgeBounds(), markerAlpha, theme);
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
    refresh();
}

void Slider::mouseDrag (const juce::MouseEvent& e)
{
    if (! model.isDragging())
        return;
    model.mouseDrag (pxOf (e.position), fineModifier (e.mods));
    refresh();
}

void Slider::mouseUp (const juce::MouseEvent&)
{
    if (! model.isDragging())
        return;
    model.mouseUp();
    // A contested drag writes nothing here: the next tick shows whatever the parameter holds.
    refresh();
}

void Slider::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    model.cancel();
    model.doubleClick();
    refresh();
}

void Slider::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // Over the control only (never scrolls a page). About 0.125 per notch on most mice.
    const float delta = (std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
    model.wheel (delta * 8.0f, fineModifier (e.mods), now());
    refresh();
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
    else if (code == juce::KeyPress::returnKey)
    {
        showTypedEntry (style == Style::Vertical ? getLocalBounds() : valueArea.toNearestInt());
        return true;
    }
    else
        return false;
    model.key (key, fine, now());
    refresh();
    return true;
}

std::unique_ptr<juce::AccessibilityHandler> Slider::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::slider, juce::AccessibilityActions{},
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<SliderValue> (*this, handle) });
}
} // namespace vs::ui
