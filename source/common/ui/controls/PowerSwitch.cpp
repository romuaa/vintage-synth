#include "common/ui/controls/PowerSwitch.h"

namespace vs::ui
{
PowerSwitch::PowerSwitch (UiContext& c, const juce::String& levelId, const juce::String& effectName, std::atomic<float>& lastLevel)
    : BoundComponent (c, levelId, effectName + " power", -1, "PowerSwitch", Placement::Secondary), last (lastLevel), effect (effectName)
{
    setHelp (*this, "ui.fx_power", effectName + " power", ctx.help);
    setAccent (colour (Tok::AccentGlobal));
    refresh();
}

PowerSwitch::~PowerSwitch() = default;

bool PowerSwitch::showsMirroredValue (juce::String& why) const
{
    if (isOn() != model::PowerSwitchModel::isOn (ctx.mirror.value (handle.getIndex())))
    {
        why << getComponentID() << ": power state differs";
        return false;
    }
    return true;
}

void PowerSwitch::press()
{
    float stored = last.load (std::memory_order_relaxed);
    const float level = handle.value(); // the live level: a press acts on what the parameter holds
    const float next = model::PowerSwitchModel::press (level, stored);
    if (model::PowerSwitchModel::isOn (level))
        last.store (stored, std::memory_order_relaxed);
    model::writeOnce (handle, next);
    refresh();
}

void PowerSwitch::valueChanged()
{
    repaint();
    if (auto* h = getAccessibilityHandler())
        h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
}

void PowerSwitch::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = isOn();
    g.setColour (on ? accent() : colour (Tok::BgWindow));
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    g.setColour (on ? accent() : colour (Tok::TextDisabled));
    g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, 1.0f);
    g.setColour (on ? colour (Tok::TextOnAccent) : colour (Tok::TextSecondary));
    g.setFont (ctx.theme.font (metrics::kTextSmall, true));
    g.drawText (on ? "ON" : "OFF", r, juce::Justification::centred, false);
    if (markerAlpha > 0.0f)
        drawExternalMarker (g, r.reduced (2.0f), {}, markerAlpha, ctx.theme, false);
    if (hasKeyboardFocus (false))
        drawFocusRing (g, r, r.getHeight() * 0.5f);
}

void PowerSwitch::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return; // the level's own menu belongs to its slider
    press();
}

bool PowerSwitch::keyPressed (const juce::KeyPress& k)
{
    const int code = k.getKeyCode();
    if (code == juce::KeyPress::spaceKey || code == juce::KeyPress::returnKey)
    {
        press();
        return true;
    }
    return false;
}

std::unique_ptr<juce::AccessibilityHandler> PowerSwitch::createAccessibilityHandler()
{
    juce::AccessibilityActions actions;
    juce::Component::SafePointer<PowerSwitch> self (this);
    actions.addAction (juce::AccessibilityActionType::toggle, [self] {
        if (self != nullptr)
            self->press();
    });
    return std::make_unique<CheckableHandler> (*this, juce::AccessibilityRole::toggleButton, std::move (actions), [this] { return isOn(); });
}
} // namespace vs::ui
