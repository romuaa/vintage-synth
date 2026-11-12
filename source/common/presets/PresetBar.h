#pragma once

// A plain preset bar until the custom UI exists (M7): bank and position, program name with a
// modified marker, previous / next, Load, Save and Init menus. All logic lives in PresetManager;
// the bar only calls it, so presets work without any editor open. File dialogs are asynchronous
// (never modal), start in the user preset folders without creating them, and are never shown by tests.

#include "common/presets/PresetManager.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vs::presets
{
class PresetBar final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit PresetBar (PresetManager& manager);
    ~PresetBar() override;

    static constexpr int kHeight = 32;

    void resized() override;
    void paint (juce::Graphics&) override;

    /** The text shown (for tests and accessibility): "Factory  A1  Init *". */
    juce::String describe() const;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override { refresh(); } // picks up the modified marker (set from any thread)
    void refresh();
    void showLoadMenu();
    void showSaveMenu();
    void chooseAndLoad (const juce::String& title, const juce::File& folder, const juce::String& pattern, std::function<juce::String (const juce::File&)> action);
    void chooseAndSave (const juce::String& title, const juce::File& folder, const juce::String& extension, std::function<juce::String (const juce::File&)> action);
    void report (const juce::String& error);

    PresetManager& presets;
    juce::TextButton previous { "<" }, next { ">" }, load { "Load..." }, save { "Save..." }, init { "Init" };
    juce::Label bankLabel, positionLabel, nameLabel;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBar)
};
} // namespace vs::presets
