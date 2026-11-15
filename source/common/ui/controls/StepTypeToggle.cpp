#include "common/ui/controls/StepTypeToggle.h"

#include "common/params/FxArpParams.h"
#include "common/ui/models/StepGridModel.h"

namespace vs::ui
{
class StepTypeToggle::Button final : public juce::Component
{
public:
    Button (StepTypeToggle& o, int w) : owner (o), which (w)
    {
        setWantsKeyboardFocus (true);
        setTitle (juce::String ("Step ") + juce::String (o.stepNumber()) + (w == 0 ? " rest" : " tie"));
        setComponentID (o.getComponentID() + (w == 0 ? ".rest" : ".tie"));
        setHelp (*this, o.handle.param().paramID, "Step " + juce::String (o.stepNumber()) + " type", o.ctx.help);
    }

    bool lit() const { return which == 0 ? owner.isRest() : owner.isTie(); }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool on = lit();
        const float alpha = owner.dimmed ? 0.45f : 1.0f;
        g.setColour (on ? colour (Tok::AccentGlobal).withAlpha (alpha) : colour (Tok::BgWindow));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (colour (Tok::BgTrack));
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        g.setColour (on ? colour (Tok::TextOnAccent) : (owner.dimmed ? colour (Tok::TextDisabled) : colour (Tok::TextPrimary)));
        g.setFont (owner.ctx.theme.font (metrics::kTextSmall, on));
        g.drawText (caption, r, juce::Justification::centred, false);
        if (hasKeyboardFocus (false))
            drawFocusRing (g, r, 3.0f);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            owner.openContextMenu();
        else
            owner.press (which);
    }
    void mouseUp (const juce::MouseEvent&) override { owner.release(); }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k.getKeyCode() == juce::KeyPress::spaceKey || k.getKeyCode() == juce::KeyPress::returnKey)
        {
            owner.press (which);
            owner.release();
            return true;
        }
        return false;
    }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        juce::AccessibilityActions actions;
        juce::Component::SafePointer<Button> self (this);
        actions.addAction (juce::AccessibilityActionType::toggle, [self] {
            if (self != nullptr)
            {
                self->owner.press (self->which);
                self->owner.release();
            }
        });
        return std::make_unique<CheckableHandler> (*this, juce::AccessibilityRole::toggleButton, std::move (actions), [this] { return lit(); });
    }

private:
    StepTypeToggle& owner;
    int which;
    juce::String caption { which == 0 ? "R" : "T" };
};

StepTypeToggle::StepTypeToggle (UiContext& c, int stepNo, int partIndex)
    : BoundComponent (c, seqStepParamID (stepNo, SeqStepField::Type), "Step " + juce::String (stepNo) + " type", partIndex, "StepTypeToggle"),
      step (stepNo),
      model (handle, 3)
{
    setWantsKeyboardFocus (false); // its two buttons take the focus
    setTitle ("Step " + juce::String (step) + " type");
    rest = std::make_unique<Button> (*this, 0);
    tie = std::make_unique<Button> (*this, 1);
    addAndMakeVisible (*rest);
    addAndMakeVisible (*tie);
    refresh();
}

StepTypeToggle::~StepTypeToggle()
{
    model.cancel();
}

float StepTypeToggle::shownValue() const
{
    return model.isInGesture() ? handle.value() : BoundComponent::shownValue();
}

int StepTypeToggle::drawnType() const
{
    return model::stepIndex (drawnValue(), 3);
}

bool StepTypeToggle::showsMirroredValue (juce::String& why) const
{
    if (! BoundComponent::showsMirroredValue (why))
        return false;
    if (drawnType() != model::stepIndex (ctx.mirror.value (handle.getIndex()), 3))
    {
        why << getComponentID() << ": type differs";
        return false;
    }
    return true;
}

juce::Rectangle<int> StepTypeToggle::buttonBounds (int which) const
{
    const int w = getWidth() / 2;
    return which == 0 ? juce::Rectangle<int> (0, 0, w, getHeight()) : juce::Rectangle<int> (w, 0, getWidth() - w, getHeight());
}

void StepTypeToggle::resized()
{
    rest->setBounds (buttonBounds (0));
    tie->setBounds (buttonBounds (1));
}

void StepTypeToggle::press (int which)
{
    model.press (model::StepGridModel::typeAfterPress (model.index(), which));
    refresh();
}

void StepTypeToggle::release()
{
    model.release();
    refresh();
}

void StepTypeToggle::setDimmed (bool dim)
{
    if (dimmed == dim)
        return;
    dimmed = dim;
    rest->repaint();
    tie->repaint();
}

void StepTypeToggle::valueChanged()
{
    if (rest != nullptr)
    {
        rest->repaint();
        tie->repaint();
    }
}
} // namespace vs::ui
