#include "common/presets/PresetBar.h"

namespace vs::presets
{
namespace
{
juce::String capitalised (const juce::String& s) { return s.substring (0, 1).toUpperCase() + s.substring (1); }
} // namespace

PresetBar::PresetBar (PresetManager& manager) : presets (manager)
{
    for (auto* b : { &previous, &next, &load, &save, &init })
        addAndMakeVisible (*b);
    for (auto* l : { &bankLabel, &positionLabel, &nameLabel })
    {
        l->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (*l);
    }
    positionLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));

    // Button callbacks run synchronously on a click, while the bar exists: `this` is safe there.
    // Everything asynchronous (menus, file choosers) holds a SafePointer and checks it first.
    previous.onClick = [this] { presets.selectPrevious(); };
    next.onClick = [this] { presets.selectNext(); };
    init.onClick = [this] { presets.initialise(); };
    load.onClick = [this] { showLoadMenu(); };
    save.onClick = [this] { showSaveMenu(); };
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
    nameLabel.setText (presets.currentName() + (presets.isModified() ? " *" : ""), juce::dontSendNotification);
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
    nameLabel.setBounds (r);
}

void PresetBar::report (const juce::String& error)
{
    if (error.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Presets", error);
}

void PresetBar::chooseAndLoad (const juce::String& title, const juce::File& folder, const juce::String& pattern,
                               std::function<juce::String (const juce::File&)> action)
{
    chooser = std::make_unique<juce::FileChooser> (title, folder.isDirectory() ? folder : juce::File(), pattern);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<PresetBar> (this), run = std::move (action)] (const juce::FileChooser& fc) {
                              const auto f = fc.getResult();
                              if (safe == nullptr || f == juce::File())
                                  return; // the bar (and the editor) closed while the dialog was open, or cancelled
                              safe->report (run (f));
                          });
}

void PresetBar::chooseAndSave (const juce::String& title, const juce::File& folder, const juce::String& extension,
                               std::function<juce::String (const juce::File&)> action)
{
    const auto suggestion = (folder.isDirectory() ? folder : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory))
                                .getChildFile (juce::File::createLegalFileName (presets.currentName()) + extension);
    chooser = std::make_unique<juce::FileChooser> (title, suggestion, "*" + extension);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer<PresetBar> (this), extension, run = std::move (action)] (const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (safe == nullptr || f == juce::File())
                                  return;
                              if (! f.hasFileExtension (extension))
                                  f = f.withFileExtension (extension);
                              safe->report (run (f));
                          });
}

void PresetBar::showLoadMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Program file...");
    menu.addItem (2, "Bank file...");
    menu.addSeparator();
    const auto& schema = presets.getSchema();
    for (int t = 0; t < schema.numTones(); ++t)
        menu.addItem (10 + t, "Tone into " + capitalised (schema.toneKey (t)) + "...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (load),
                        [safe = juce::Component::SafePointer<PresetBar> (this)] (int choice) { handleLoadMenuResult (safe, choice); });
}

void PresetBar::showSaveMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Program as...");
    menu.addItem (2, "Bank as...");
    menu.addItem (3, "Store in this slot (user banks)", presets.bankStatus() == BankStatus::User && ! presets.isDetached(), false);
    menu.addSeparator();
    const auto& schema = presets.getSchema();
    for (int t = 0; t < schema.numTones(); ++t)
        menu.addItem (10 + t, capitalised (schema.toneKey (t)) + " tone as...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (save),
                        [safe = juce::Component::SafePointer<PresetBar> (this)] (int choice) { handleSaveMenuResult (safe, choice); });
}

void PresetBar::handleLoadMenuResult (juce::Component::SafePointer<PresetBar> bar, int choice)
{
    // JUCE calls a menu's callback with 0 when it is dismissed, also when the bar (with its
    // editor) was deleted while the menu was open: nothing of the bar may be touched then.
    if (choice == 0 || bar == nullptr)
        return;
    bar->loadChosen (choice);
}

void PresetBar::handleSaveMenuResult (juce::Component::SafePointer<PresetBar> bar, int choice)
{
    if (choice == 0 || bar == nullptr)
        return;
    bar->saveChosen (choice);
}

// The file actions below run later, from a file chooser's callback that has checked the bar still
// exists; they reach the manager through a plain reference: the processor (which owns it) outlives
// every editor.
void PresetBar::loadChosen (int choice)
{
    const auto where = presets.getLocations();
    const auto& inst = presets.getSchema().instrument();
    auto& pm = presets;
    if (choice == 1)
        chooseAndLoad ("Load program", where.programs(), "*" + inst.programExtension, [&pm] (const juce::File& f) { return pm.loadProgramFile (f); });
    else if (choice == 2)
        chooseAndLoad ("Load bank", where.banks(), "*" + inst.bankExtension, [&pm] (const juce::File& f) { return pm.loadBankFile (f); });
    else if (choice >= 10)
        chooseAndLoad ("Load tone", where.tones(), "*" + inst.toneExtension,
                       [&pm, part = choice - 10] (const juce::File& f) { return pm.loadToneFile (f, part); });
}

void PresetBar::saveChosen (int choice)
{
    const auto where = presets.getLocations();
    const auto& inst = presets.getSchema().instrument();
    auto& pm = presets;
    if (choice == 1)
        chooseAndSave ("Save program", where.programs(), inst.programExtension,
                       [&pm] (const juce::File& f) { return pm.saveProgramFile (f, f.getFileNameWithoutExtension()); });
    else if (choice == 2)
        chooseAndSave ("Save bank", where.banks(), inst.bankExtension,
                       [&pm] (const juce::File& f) { return pm.saveBankFile (f, f.getFileNameWithoutExtension()); });
    else if (choice == 3)
        report (presets.storeInBank (presets.currentIndex()));
    else if (choice >= 10)
        chooseAndSave ("Save tone", where.tones(), inst.toneExtension,
                       [&pm, part = choice - 10] (const juce::File& f) { return pm.saveToneFile (f, part, f.getFileNameWithoutExtension()); });
}
} // namespace vs::presets
