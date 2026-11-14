#pragma once

// A plain preset bar until the custom UI exists (M7): bank and position, program name with a
// modified marker, previous / next, Load, Save and Init menus. All logic lives in PresetManager;
// the bar only calls it, so presets work without any editor open. File dialogs are asynchronous
// (never modal), start in the user preset folders without creating them, and are never shown by tests.
// The dialogs and menus of Load and Save are PresetActions (shared with the Vintage Browser).
// The program name is a button: it opens the program menu (the bank's programs grouped by category,
// docs/architecture.md 9.3.8); Enter or Space on it does the same.
// Every asynchronous callback (menus, file choosers) holds a weak or safe pointer and does
// nothing if the bar is gone (the editor closed while a menu or dialog was open) or if it was
// dismissed (result 0). The bar's own timer and listener are stopped in its destructor.

#include "common/presets/PresetActions.h"
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

    /** The Load and Save menus' results are PresetActions::handleLoadMenuResult / handleSaveMenuResult. */
    /** The program menu's choice: a slot + 1, or kOpenBrowserChoice; nothing for 0 or when the bar is gone. */
    /** `generation`: the bank's generation when the menu opened; if the bank changed since, nothing is loaded. */
    static void handleProgramMenuResult (juce::Component::SafePointer<PresetBar> bar, int choice, std::optional<std::uint32_t> generation = {});
    static constexpr int kOpenBrowserChoice = 10000;

    /** Set by the Vintage editor: adds "Open Browser" to the program menu. */
    std::function<void()> onOpenBrowser;
    /** The menu the program name opens, from the manager's state now (tests). */
    ui::model::ProgramMenu programMenuModel() const;
    void showProgramMenu();
    juce::Button& getNameButton() noexcept;
    juce::Button& getSaveButton() noexcept { return save; }
    juce::Button& getLoadButton() noexcept { return load; }
    juce::Button& getInitButton() noexcept { return init; }
    juce::Component& getBankLabel() noexcept { return bankLabel; }

    /** Save stores in place (a user bank, not detached) and otherwise opens the Save as menu, instead of the
        Save menu. Set by the Vintage editor; the second plugin's bar keeps its menu. */
    bool smartSave = false;
    /** The Vintage top bar's layout: the program name takes the room (no Init button, a short bank label). */
    void setCompactLayout (bool compact);
    PresetActions& getActions() noexcept { return actions; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override { refresh(); } // picks up the modified marker (set from any thread)
    void refresh();
    juce::String bankText() const;
    juce::String positionText() const;

    class NameButton;

    PresetManager& presets;
    PresetActions actions;
    bool compactLayout = false;
    juce::TextButton previous { "<" }, next { ">" }, load { "Load..." }, save { "Save..." }, init { "Init" };
    juce::Label bankLabel, positionLabel;
    std::unique_ptr<NameButton> nameButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBar)
};
} // namespace vs::presets
