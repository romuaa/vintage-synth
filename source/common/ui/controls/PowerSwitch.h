#pragma once

// PowerSwitch (docs/architecture.md 9.3.6, U5): the on/off switch in the header of a delay or
// reverb panel. It is a secondary binding of the effect's level: lit while the level is above 0;
// switching off remembers the level (EditorState::fxLastLevel) and sets 0, switching on brings the
// remembered level back (0.35 if none). No parameter of its own.

#include "common/ui/controls/BoundComponent.h"
#include "common/ui/models/FxPictureModels.h"

#include <atomic>

namespace vs::ui
{
class PowerSwitch final : public BoundComponent
{
public:
    /** `levelId`: the effect's level parameter; `lastLevel`: where the level is remembered while off. */
    PowerSwitch (UiContext& ctx, const juce::String& levelId, const juce::String& effectName, std::atomic<float>& lastLevel);
    ~PowerSwitch() override;

    bool isOn() const noexcept { return model::PowerSwitchModel::isOn (drawnValue()); }
    /** Presses the switch as a click does: one gesture, one value. */
    void press();
    bool showsMirroredValue (juce::String& why) const override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    void valueChanged() override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    std::atomic<float>& last;
    juce::String effect;
};
} // namespace vs::ui
