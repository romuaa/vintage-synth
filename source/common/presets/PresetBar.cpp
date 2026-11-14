#include "common/presets/PresetBar.h"

namespace vs::presets
{
/** The program name as a button: opens the program menu on a click, Enter or Space. */
class PresetBar::NameButton final : public juce::Button
{
public:
    // No name or title: a screen reader then speaks the button text (the program name), and the
    // editor's layout snapshots keep their text-free form.
    NameButton() : juce::Button ({})
    {
        setDescription ("Opens a list of the programs of this bank, grouped by category");
        setWantsKeyboardFocus (true);
        setMouseClickGrabsKeyboardFocus (true);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (isEnabled() && (k.isKeyCode (juce::KeyPress::returnKey) || k.isKeyCode (juce::KeyPress::spaceKey)))
        {
            if (onClick)
                onClick(); // at once (triggerClick would post it)
            return true;
        }
        return false;
    }

    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto r = getLocalBounds().toFloat();
        const auto base = getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
        g.setColour (down ? base.brighter (0.25f) : (highlighted ? base.brighter (0.15f) : base.brighter (0.07f)));
        g.fillRoundedRectangle (r, 4.0f);
        if (hasKeyboardFocus (false))
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 2.0f);
        }
        auto textArea = r.reduced (8.0f, 0.0f);
        g.setColour (getLookAndFeel().findColour (juce::Label::textColourId));
        g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        g.drawText (getButtonText(), textArea.withTrimmedRight (16.0f), juce::Justification::centredLeft, true);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("v", textArea.removeFromRight (14.0f), juce::Justification::centred, false);
    }
};

PresetBar::PresetBar (PresetManager& manager) : presets (manager), actions (manager)
{
    nameButton = std::make_unique<NameButton>();
    for (auto* b : { &previous, &next, &load, &save, &init })
        addAndMakeVisible (*b);
    for (auto* l : { &bankLabel, &positionLabel })
    {
        l->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (*l);
    }
    addAndMakeVisible (*nameButton); // last: the child order is part of the layout snapshots
    positionLabel.setJustificationType (juce::Justification::centred);

    // Button callbacks run synchronously on a click, while the bar exists: `this` is safe there.
    // Everything asynchronous (menus, file choosers) holds a SafePointer and checks it first.
    previous.onClick = [this] { presets.selectPrevious(); };
    next.onClick = [this] { presets.selectNext(); };
    init.onClick = [this] { presets.initialise(); };
    load.onClick = [this] { actions.showLoadMenu (load); };
    save.onClick = [this] { actions.showSaveMenu (save); };
    nameButton->onClick = [this] { showProgramMenu(); };
    previous.setTooltip ("Previous program");
    next.setTooltip ("Next program");
    init.setTooltip ("Initialise: every parameter at its default");

    presets.changes().addChangeListener (this);
    refresh();
    startTimerHz (8);
}

PresetBar::~PresetBar()
{
    stopTimer();
    presets.changes().removeChangeListener (this);
}

juce::String PresetBar::bankText() const
{
    switch (presets.bankStatus())
    {
        case BankStatus::Missing: return "bank not found: " + presets.bankName();
        case BankStatus::Pending: return presets.bankName() + " (loading)";
        case BankStatus::Factory:
        case BankStatus::User: break;
    }
    return presets.bankName();
}

juce::String PresetBar::positionText() const
{
    return presets.isDetached() ? juce::String ("--") : presets.positionName (presets.currentIndex());
}

juce::String PresetBar::describe() const
{
    return bankText() + "  " + positionText() + "  " + presets.currentName() + (presets.isModified() ? " *" : "");
}

void PresetBar::refresh()
{
    // Label::setText only repaints when the text changed, so calling this often is cheap.
    bankLabel.setText (bankText() + (presets.isFactoryBank() ? " (factory)" : ""), juce::dontSendNotification);
    positionLabel.setText (positionText(), juce::dontSendNotification);
    nameButton->setButtonText (presets.currentName() + (presets.isModified() ? " *" : ""));
}

void PresetBar::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void PresetBar::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId).darker (0.3f));
}

void PresetBar::resized()
{
    auto r = getLocalBounds().reduced (4, 3);
    const int button = 28;
    previous.setBounds (r.removeFromLeft (button));
    r.removeFromLeft (2);
    next.setBounds (r.removeFromLeft (button));
    r.removeFromLeft (6);
    init.setBounds (r.removeFromRight (48));
    r.removeFromRight (4);
    save.setBounds (r.removeFromRight (64));
    r.removeFromRight (4);
    load.setBounds (r.removeFromRight (64));
    r.removeFromRight (6);
    bankLabel.setBounds (r.removeFromLeft (juce::jmin (150, r.getWidth() / 3)));
    positionLabel.setBounds (r.removeFromLeft (36));
    nameButton->setBounds (r);
}

juce::Button& PresetBar::getNameButton() noexcept
{
    return *nameButton;
}

ui::model::ProgramMenu PresetBar::programMenuModel() const
{
    return ui::model::programMenu (slotInfos (presets), positionNames (presets), presets.isDetached() ? -1 : presets.currentIndex(),
                                   bankUnavailableText (presets));
}

void PresetBar::showProgramMenu()
{
    const auto model = programMenuModel();
    const auto addEntries = [] (juce::PopupMenu& menu, const ui::model::MenuGroup& group) {
        for (const auto& e : group.entries)
            menu.addItem (e.slot + 1, juce::String::fromUTF8 (e.text.c_str()), e.enabled, e.ticked);
    };
    juce::PopupMenu menu;
    if (model.flat())
        addEntries (menu, model.groups.front());
    else
        for (const auto& group : model.groups)
        {
            juce::PopupMenu sub;
            addEntries (sub, group);
            menu.addSubMenu (juce::String::fromUTF8 (group.label.c_str()), sub);
        }
    if (onOpenBrowser)
    {
        menu.addSeparator();
        menu.addItem (kOpenBrowserChoice, "Open Browser");
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (*nameButton).withMinimumWidth (nameButton->getWidth()),
                        [safe = juce::Component::SafePointer<PresetBar> (this)] (int choice) { handleProgramMenuResult (safe, choice); });
}

void PresetBar::handleLoadMenuResult (juce::Component::SafePointer<PresetBar> bar, int choice)
{
    // JUCE calls a menu's callback with 0 when it is dismissed, also when the bar (with its
    // editor) was deleted while the menu was open: nothing of the bar may be touched then.
    if (choice == 0 || bar == nullptr)
        return;
    bar->actions.handleLoadChoice (choice);
}

void PresetBar::handleSaveMenuResult (juce::Component::SafePointer<PresetBar> bar, int choice)
{
    if (choice == 0 || bar == nullptr)
        return;
    bar->actions.handleSaveChoice (choice);
}

void PresetBar::handleProgramMenuResult (juce::Component::SafePointer<PresetBar> bar, int choice)
{
    if (choice == 0 || bar == nullptr)
        return;
    if (choice == kOpenBrowserChoice)
    {
        if (bar->onOpenBrowser)
            bar->onOpenBrowser();
    }
    else if (choice >= 1 && choice <= bar->presets.bankSize())
        bar->presets.selectProgram (choice - 1);
}
} // namespace vs::presets
