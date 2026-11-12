#pragma once

// A plain preset bar until the custom UI exists (M7): bank and position, program name with a
// modified marker, previous / next, Load, Save and Init menus. All logic lives in PresetManager;
// the bar only calls it, so presets work without any editor open. File dialogs are asynchronous
// (never modal), start in the user preset folders without creating them, and are never shown by tests.
// Every asynchronous callback (menus, file choosers) holds a Component::SafePointer and does
// nothing if the bar is gone (the editor closed while a menu or dialog was open) or if it was
// dismissed (result 0). The bar's own timer and listener are stopped in its destructor.

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

    /** The text shown (for tests and accessibility): "Factory  A1  Init *", or for a session whose
        user bank is gone "bank not found: Mine  --  Lead *". */
    juce::String describe() const;

    /** What the Load / Save menus do with their result (public for tests): nothing for 0 (dismissed)
        or when the bar no longer exists. */
    static void handleLoadMenuResult (juce::Component::SafePointer<PresetBar> bar, int choice);
    static void handleSaveMenuResult (juce::Component::SafePointer<PresetBar> bar, int choice);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override { refresh(); } // picks up the modified marker (set from any thread)
    void refresh();
    void showLoadMenu();
    void showSaveMenu();
    void loadChosen (int choice);
    void saveChosen (int choice);
    juce::String bankText() const;
    juce::String positionText() const;
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
