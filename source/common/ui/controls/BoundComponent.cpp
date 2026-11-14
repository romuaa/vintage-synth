#include "common/ui/controls/BoundComponent.h"

namespace vs::ui
{
void setHelp (juce::Component& c, const juce::String& helpId, const juce::String& label, const HelpTexts& help)
{
    c.getProperties().set (helpIdProperty(), helpId);
    c.getProperties().set (helpLabelProperty(), label);
    if (const auto* text = help.find (helpId.toStdString()))
        c.setDescription (juce::String::fromUTF8 (text->c_str()));
}

juce::String partPrefix (int part)
{
    return part == 0 ? "Lower " : (part == 1 ? "Upper " : "");
}

BoundComponent::BoundComponent (UiContext& c, const juce::String& paramId, const juce::String& labelText, int partIndex,
                                const juce::String& controlType, Placement placement, int captions)
    : ctx (c), handle (c.mirror, c.mirror.indexOf (paramId)), label (labelText), part (partIndex), accentColour (accentFor (partIndex))
{
    jassert (handle.getIndex() >= 0); // unknown parameter ID
    ctx.mirror.subscribe (handle.getIndex(), this);
    ctx.mirror.addIdleClient (this);

    int choices = 0;
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&handle.param()))
        choices = choice->choices.size();
    ctx.registry.add ({ paramId, ctx.registry.page(), ctx.registry.part(), placement, controlType, captions, choices, this });

    setHelp (*this, paramId, label, ctx.help);
    setTitle (partPrefix (part) + label);
    setWantsKeyboardFocus (true);
    setComponentID (paramId);
}

BoundComponent::~BoundComponent()
{
    ctx.mirror.unsubscribe (this);
    ctx.mirror.removeIdleClient (this);
}

void BoundComponent::setInactive (bool shouldBeInactive)
{
    if (inactive == shouldBeInactive)
        return;
    inactive = shouldBeInactive;
    auto description = getDescription().upToFirstOccurrenceOf (" (no effect in this mode)", false, false);
    setDescription (inactive ? description + " (no effect in this mode)" : description);
    refresh();
}

void BoundComponent::setAccent (juce::Colour c)
{
    accentColour = c;
    repaint();
}

float BoundComponent::shownValue() const
{
    return ctx.mirror.value (handle.getIndex());
}

void BoundComponent::refresh()
{
    drawn = shownValue();
    drawnString = handle.textFor (drawn);
    valueChanged();
}

bool BoundComponent::showsMirroredValue (juce::String& why) const
{
    // What is drawn (prepared at the last notification), not a fresh read of the mirror.
    const float mirrored = ctx.mirror.value (handle.getIndex());
    if (! model::sameBits (drawn, mirrored))
    {
        why << getComponentID() << ": draws " << drawn << ", parameter " << mirrored;
        return false;
    }
    const auto text = handle.textFor (mirrored);
    if (drawnString != text)
    {
        why << getComponentID() << ": text " << drawnString << " vs " << text;
        return false;
    }
    return true;
}

void BoundComponent::mirrorChanged (int, bool)
{
    markerAlpha = ctx.mirror.markerAlpha (handle.getIndex(), ctx.mirror.lastTickTime());
    refresh();
}

void BoundComponent::mirrorIdle (double nowMs)
{
    idle (nowMs);
}

void BoundComponent::typed (float normalised)
{
    model::writeOnce (handle, normalised);
    refresh();
}

void BoundComponent::showContextMenu()
{
    juce::PopupMenu menu;
    std::unique_ptr<juce::HostProvidedContextMenu> hostMenu;
    if (auto* editor = findParentComponentOfClass<juce::AudioProcessorEditor>())
        if (auto* host = editor->getHostContext())
            hostMenu = host->getContextMenuForParameter (&handle.param());
    menu.addItem (1, "Reset to default");
    menu.addItem (2, "Type a value...");
    if (hostMenu != nullptr)
    {
        menu.addSeparator();
        menu.addSectionHeader ("Host");
        auto hostItems = hostMenu->getEquivalentPopupMenu();
        menu.addSubMenu ("Host", hostItems);
    }
    juce::Component::SafePointer<BoundComponent> self (this);
    std::shared_ptr<juce::HostProvidedContextMenu> keepHost (std::move (hostMenu));
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [self, keepHost] (int result) {
        if (self == nullptr || result == 0)
            return;
        if (result == 1)
            self->typed (self->handle.defaultValue());
        else if (result == 2)
            self->showTypedEntry (self->getLocalBounds());
    });
}

void BoundComponent::showTypedEntry (juce::Rectangle<int> where)
{
    entry = std::make_unique<juce::TextEditor>();
    entry->setFont (ctx.theme.font (metrics::kText));
    entry->setText (shownText(), false);
    entry->selectAll();
    entry->setBounds (where.withSizeKeepingCentre (juce::jmax (where.getWidth(), 80), juce::jmin (where.getHeight(), 24)));
    entry->setTitle ("Type a value for " + getTitle());
    juce::Component::SafePointer<BoundComponent> self (this);
    entry->onReturnKey = [self] {
        if (self == nullptr || self->entry == nullptr)
            return;
        float v = 0.0f;
        if (self->handle.parse (self->entry->getText(), v))
            self->typed (v); // unreadable text changes nothing
        juce::MessageManager::callAsync ([self] { if (self != nullptr) self->entry.reset(); });
    };
    entry->onEscapeKey = [self] {
        juce::MessageManager::callAsync ([self] { if (self != nullptr) self->entry.reset(); });
    };
    entry->onFocusLost = entry->onEscapeKey;
    addAndMakeVisible (*entry);
    entry->grabKeyboardFocus();
}

void drawExternalMarker (juce::Graphics& g, juce::Rectangle<float> thumb, juce::Rectangle<float> badge, float alpha, const Theme& theme,
                         bool round)
{
    if (alpha <= 0.0f)
        return;
    // A dashed outline in the marker colour: never the focus ring's solid white.
    juce::Path outline, dashed;
    if (round)
        outline.addEllipse (thumb.expanded (1.5f));
    else
        outline.addRoundedRectangle (thumb, 3.0f);
    const float dashes[] { 3.0f, 2.5f };
    juce::PathStrokeType (1.5f).createDashedStroke (dashed, outline, dashes, 2);
    g.setColour (colour (Tok::Marker).withAlpha (alpha));
    g.fillPath (dashed);
    if (! badge.isEmpty())
    {
        g.setColour (colour (Tok::Marker).withAlpha (alpha));
        g.fillRoundedRectangle (badge, badge.getHeight() * 0.5f);
        g.setColour (colour (Tok::TextOnAccent).withAlpha (alpha));
        g.setFont (theme.font (metrics::kTextSmall - 1.0f, true));
        g.drawText ("AUTO", badge, juce::Justification::centred, false);
    }
}
} // namespace vs::ui
