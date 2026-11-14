#include "common/presets/PresetActions.h"

namespace vs::presets
{
namespace
{
juce::String capitalised (const juce::String& s) { return s.substring (0, 1).toUpperCase() + s.substring (1); }
} // namespace

PresetActions::PresetActions (PresetManager& manager) : presets (manager) {}

PresetActions::~PresetActions() = default;

void PresetActions::report (const juce::String& error)
{
    if (error.isEmpty())
        return;
    if (onError)
        onError (error);
    else
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Presets", error);
}

void PresetActions::request (const FileRequest& r, std::function<void (const juce::File&)> onChosen)
{
    if (fileSource)
    {
        fileSource (r, std::move (onChosen));
        return;
    }
    const auto flags = r.save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting
                              : juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    juce::File start;
    if (r.save)
        start = (r.folder.isDirectory() ? r.folder : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory))
                    .getChildFile (juce::File::createLegalFileName (r.suggestedName) + r.extension);
    else if (r.folder.isDirectory())
        start = r.folder;
    chooser = std::make_unique<juce::FileChooser> (r.title, start, r.save ? "*" + r.extension : r.pattern);
    chooser->launchAsync (flags, [weak = juce::WeakReference<PresetActions> (this), chosen = std::move (onChosen)] (const juce::FileChooser& fc) {
        const auto f = fc.getResult();
        if (weak == nullptr || f == juce::File())
            return; // the owner (and the editor) closed while the dialog was open, or cancelled
        chosen (f);
    });
}

void PresetActions::chooseAndLoad (const juce::String& title, const juce::File& folder, const juce::String& pattern,
                                   std::function<juce::String (const juce::File&)> action)
{
    FileRequest r;
    r.title = title;
    r.folder = folder;
    r.pattern = pattern;
    request (r, [weak = juce::WeakReference<PresetActions> (this), run = std::move (action)] (const juce::File& f) {
        if (weak != nullptr)
            weak->report (run (f));
    });
}

void PresetActions::chooseAndSave (const juce::String& title, const juce::File& folder, const juce::String& extension,
                                   std::function<juce::String (const juce::File&)> action)
{
    FileRequest r;
    r.save = true;
    r.title = title;
    r.folder = folder;
    r.extension = extension;
    r.suggestedName = presets.currentName();
    request (r, [weak = juce::WeakReference<PresetActions> (this), extension, run = std::move (action)] (const juce::File& chosen) {
        if (weak == nullptr)
            return;
        auto f = chosen;
        if (! f.hasFileExtension (extension))
            f = f.withFileExtension (extension);
        weak->report (run (f));
    });
}

// The file actions run later, from a callback that has checked this object still exists; they reach
// the manager through a plain reference: the processor (which owns it) outlives every editor.
void PresetActions::loadProgramFile()
{
    auto& pm = presets;
    chooseAndLoad ("Load program", pm.getLocations().programs(), "*" + pm.getSchema().instrument().programExtension,
                   [&pm] (const juce::File& f) { return pm.loadProgramFile (f); });
}

void PresetActions::loadBankFile()
{
    auto& pm = presets;
    chooseAndLoad ("Load bank", pm.getLocations().banks(), "*" + pm.getSchema().instrument().bankExtension,
                   [&pm] (const juce::File& f) { return pm.loadBankFile (f); });
}

void PresetActions::loadToneFile (int part)
{
    auto& pm = presets;
    chooseAndLoad ("Load tone", pm.getLocations().tones(), "*" + pm.getSchema().instrument().toneExtension,
                   [&pm, part] (const juce::File& f) { return pm.loadToneFile (f, part); });
}

void PresetActions::saveProgramAs()
{
    auto& pm = presets;
    chooseAndSave ("Save program", pm.getLocations().programs(), pm.getSchema().instrument().programExtension,
                   [&pm] (const juce::File& f) { return pm.saveProgramFile (f, f.getFileNameWithoutExtension()); });
}

void PresetActions::saveBankAs()
{
    auto& pm = presets;
    chooseAndSave ("Save bank", pm.getLocations().banks(), pm.getSchema().instrument().bankExtension,
                   [&pm] (const juce::File& f) { return pm.saveBankFile (f, f.getFileNameWithoutExtension()); });
}

void PresetActions::saveToneAs (int part)
{
    auto& pm = presets;
    chooseAndSave ("Save tone", pm.getLocations().tones(), pm.getSchema().instrument().toneExtension,
                   [&pm, part] (const juce::File& f) { return pm.saveToneFile (f, part, f.getFileNameWithoutExtension()); });
}

bool PresetActions::savesInPlace() const
{
    return presets.bankStatus() == BankStatus::User && ! presets.isDetached();
}

void PresetActions::save (juce::Component& anchor)
{
    if (savesInPlace())
        report (presets.storeInBank (presets.currentIndex()));
    else
        showSaveAsMenu (anchor);
}

void PresetActions::showLoadMenu (juce::Component& target)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Program file...");
    menu.addItem (2, "Bank file...");
    menu.addSeparator();
    const auto& schema = presets.getSchema();
    for (int t = 0; t < schema.numTones(); ++t)
        menu.addItem (10 + t, "Tone into " + capitalised (schema.toneKey (t)) + "...");

    runMenu (std::move (menu), target, [weak = juce::WeakReference<PresetActions> (this)] (int choice) { handleLoadMenuResult (weak, choice); });
}

void PresetActions::showSaveMenu (juce::Component& target)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Program as...");
    menu.addItem (2, "Bank as...");
    menu.addItem (3, "Store in this slot (user banks)", presets.bankStatus() == BankStatus::User && ! presets.isDetached(), false);
    menu.addSeparator();
    const auto& schema = presets.getSchema();
    for (int t = 0; t < schema.numTones(); ++t)
        menu.addItem (10 + t, capitalised (schema.toneKey (t)) + " tone as...");

    runMenu (std::move (menu), target,
             [weak = juce::WeakReference<PresetActions> (this), gen = presets.bankGeneration()] (int choice) { handleSaveMenuResult (weak, choice, gen); });
}

void PresetActions::showSaveAsMenu (juce::Component& target)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Program file...");
    menu.addItem (2, "Bank file (a copy of this bank)...");
    if (presets.bankStatus() == BankStatus::User)
    {
        juce::PopupMenu slots;
        for (int s = 0; s < presets.bankSize(); ++s)
            slots.addItem (100 + s, presets.positionName (s) + "  " + presets.programName (s));
        menu.addSeparator();
        menu.addSubMenu ("Store in slot of this user bank", slots);
    }
    runMenu (std::move (menu), target,
             [weak = juce::WeakReference<PresetActions> (this), gen = presets.bankGeneration()] (int choice) { handleSaveAsMenuResult (weak, choice, gen); });
}

void PresetActions::runMenu (juce::PopupMenu menu, juce::Component& target, std::function<void (int)> done, int minimumWidth)
{
    if (menuRunner)
    {
        menuRunner (std::move (menu), target, std::move (done));
        return;
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMinimumWidth (minimumWidth), std::move (done));
}

bool PresetActions::bankChangedSince (std::optional<std::uint32_t> generation, const juce::String& message)
{
    if (! generation.has_value() || *generation == presets.bankGeneration())
        return false;
    report (message);
    return true;
}

void PresetActions::handleLoadMenuResult (juce::WeakReference<PresetActions> actions, int choice)
{
    // JUCE calls a menu's callback with 0 when it is dismissed, also when the owner (with its editor) was
    // deleted while the menu was open: nothing of it may be touched then.
    if (choice == 0 || actions == nullptr)
        return;
    actions->handleLoadChoice (choice);
}

void PresetActions::handleSaveMenuResult (juce::WeakReference<PresetActions> actions, int choice, std::optional<std::uint32_t> generation)
{
    if (choice == 0 || actions == nullptr)
        return;
    if (choice == 3 && actions->bankChangedSince (generation, "The bank changed; nothing was saved."))
        return;
    actions->handleSaveChoice (choice);
}

void PresetActions::handleSaveAsMenuResult (juce::WeakReference<PresetActions> actions, int choice, std::optional<std::uint32_t> generation)
{
    if (choice == 0 || actions == nullptr)
        return;
    if (choice >= 100 && actions->bankChangedSince (generation, "The bank changed; nothing was saved."))
        return;
    actions->handleSaveAsChoice (choice);
}

void PresetActions::handleLoadChoice (int choice)
{
    if (choice == 1)
        loadProgramFile();
    else if (choice == 2)
        loadBankFile();
    else if (choice >= 10 && choice - 10 < presets.getSchema().numTones())
        loadToneFile (choice - 10);
}

void PresetActions::handleSaveChoice (int choice)
{
    if (choice == 1)
        saveProgramAs();
    else if (choice == 2)
        saveBankAs();
    else if (choice == 3)
        report (presets.storeInBank (presets.currentIndex()));
    else if (choice >= 10 && choice - 10 < presets.getSchema().numTones())
        saveToneAs (choice - 10);
}

void PresetActions::handleSaveAsChoice (int choice)
{
    if (choice == 1)
        saveProgramAs();
    else if (choice == 2)
        saveBankAs();
    else if (choice >= 100 && choice - 100 < presets.bankSize())
        storeInSlot (choice - 100);
}

std::vector<ui::model::SlotInfo> slotInfos (const PresetManager& presets)
{
    std::vector<ui::model::SlotInfo> list;
    for (int s = 0; s < presets.bankSize(); ++s)
    {
        const auto m = presets.slotMeta (s);
        list.push_back ({ m.name.toStdString(), m.category.toStdString() });
    }
    return list;
}

std::vector<std::string> positionNames (const PresetManager& presets)
{
    std::vector<std::string> list;
    for (int s = 0; s < presets.bankSize(); ++s)
        list.push_back (presets.positionName (s).toStdString());
    return list;
}

std::string bankUnavailableText (const PresetManager& presets)
{
    switch (presets.bankStatus())
    {
        case BankStatus::Missing: return ("bank not found: " + presets.bankName()).toStdString();
        case BankStatus::Pending: return (presets.bankName() + " (loading)").toStdString();
        case BankStatus::Factory:
        case BankStatus::User: break;
    }
    return {};
}
} // namespace vs::presets
