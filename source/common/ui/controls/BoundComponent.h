#pragma once

// What every parameter-bound control shares (docs/architecture.md 9.2.1): the mirror subscription,
// the parameter handle, the registry entry, help and accessibility text, the inactive state and the
// external-change marker. A control prepares everything it draws when its value changes
// (mirrorChanged); paint() only draws.

#include "common/ui/ControlRegistry.h"
#include "common/ui/HelpTexts.h"
#include "common/ui/ParamHandle.h"
#include "common/ui/ParamMirror.h"
#include "common/ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vs::ui
{
/** Everything a control needs from its editor. */
struct UiContext
{
    ParamMirror& mirror;
    const Theme& theme;
    ControlRegistry& registry;
    const HelpTexts& help;
};

/** Component property names the editor's help system reads (hover and focus). */
inline juce::Identifier helpIdProperty() { return "vsHelpId"; }
inline juce::Identifier helpLabelProperty() { return "vsHelpLabel"; }

/** Marks `c` as having help text `helpId` (a parameter or `ui.*` id) shown as "label: text". */
void setHelp (juce::Component& c, const juce::String& helpId, const juce::String& label, const HelpTexts& help);

/** "Lower ", "Upper " or "" (part pages name their part in every accessibility name). */
juce::String partPrefix (int part);

/** A page (or other content) whose open gestures must end when it is hidden (9.1.2 rule 5). */
class InteractiveContent
{
public:
    virtual ~InteractiveContent() = default;
    virtual void cancelInteraction() = 0;
};

/** Accessibility handler that reports a checkable state (toggles, rail buttons). */
class CheckableHandler final : public juce::AccessibilityHandler
{
public:
    CheckableHandler (juce::Component& c, juce::AccessibilityRole role, juce::AccessibilityActions actions, std::function<bool()> isChecked,
                      Interfaces interfaces = {})
        : juce::AccessibilityHandler (c, role, std::move (actions), std::move (interfaces)), checked (std::move (isChecked))
    {
    }
    juce::AccessibleState getCurrentState() const override
    {
        auto s = juce::AccessibilityHandler::getCurrentState().withCheckable();
        return checked && checked() ? s.withChecked() : s;
    }

private:
    std::function<bool()> checked;
};

class BoundComponent : public juce::Component,
                       public ParamMirror::Listener,
                       public ParamMirror::IdleClient,
                       public BoundControl
{
public:
    BoundComponent (UiContext& ctx, const juce::String& paramId, const juce::String& label, int part, const juce::String& controlType,
                    Placement placement = Placement::Primary, int captions = 0);
    ~BoundComponent() override;

    int getParamIndex() const noexcept { return handle.getIndex(); }
    const juce::String& getLabel() const noexcept { return label; }
    int getPart() const noexcept { return part; }
    /** Display only (9.3.9): dimmed, still editable; accessibility says "(no effect in this mode)". */
    void setInactive (bool shouldBeInactive);
    bool isInactive() const noexcept { return inactive; }
    void setAccent (juce::Colour c);

    /** The value this control shows: the user's own during a gesture, else the mirror's. */
    virtual float shownValue() const;
    juce::String shownText() const { return handle.textFor (shownValue()); }
    /** What paint() draws: prepared by refresh() when the value changed (tests check these). */
    float drawnValue() const noexcept { return drawn; }
    const juce::String& drawnText() const noexcept { return drawnString; }
    bool showsMirroredValue (juce::String& why) const override;
    float getMarkerAlpha() const noexcept { return markerAlpha; }

    // ParamMirror
    void mirrorChanged (int index, bool valueChanged) override;
    void mirrorIdle (double nowMs) override;

    /** Ends any open gesture (called before the editor goes away, and by the destructor). */
    virtual void cancelInteraction() {}

protected:
    /** Prepares the drawn value and text from shownValue(), then valueChanged(). Call after every
        change (mirror notification or own interaction). */
    void refresh();
    /** Called by refresh(): prepare further drawing data and repaint. */
    virtual void valueChanged() { repaint(); }
    virtual void idle (double) {}
    virtual bool isInGesture() const { return false; }
    void showContextMenu();
    void showTypedEntry (juce::Rectangle<int> where);
    void typed (float normalised);
    juce::Colour accent() const noexcept { return accentColour; }
    juce::Colour textColour() const noexcept { return inactive ? colour (Tok::TextDisabled) : colour (Tok::TextPrimary); }

    UiContext& ctx;
    ParamHandle handle;
    juce::String label;
    int part;
    bool inactive = false;
    float markerAlpha = 0.0f;
    float drawn = 0.0f;
    juce::String drawnString;
    juce::Colour accentColour;
    std::unique_ptr<juce::TextEditor> entry;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BoundComponent)
};

/** Draws the external-change marker: a ring around `thumb` (round for a thumb or handle, else a
    rounded rectangle) and, if `badge` is not empty, an "AUTO" badge there (alpha fades). */
void drawExternalMarker (juce::Graphics& g, juce::Rectangle<float> thumb, juce::Rectangle<float> badge, float alpha, const Theme& theme,
                         bool round = true);
} // namespace vs::ui
