#pragma once

// The file dialogs, menus and save/load flows of the preset UI (docs/architecture.md 9.1.1, 9.3.7),
// moved out of PresetBar so the plain bar (B-Side) and the Vintage Browser run the same code. All
// logic lives in PresetManager; this class only asks for a file or a menu choice and then calls it.
//
// Dialogs and menus are asynchronous (never modal), start in the user preset folders without
// creating them, and are never shown by tests (tests install a FileSource and call the handle...
// functions). An owner (the bar, the Browser page) holds one PresetActions as a member; every
// asynchronous callback holds a weak reference to it and does nothing once it is gone (the editor
// closed while a menu or dialog was open) or when the result is "dismissed" (0 / no file).
// Files chosen here are read by the manager's strict readers only; nothing is parsed in this class.

#include "common/presets/PresetManager.h"
#include "common/ui/models/ProgramModels.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace vs::presets
{
class PresetActions
{
public:
    explicit PresetActions (PresetManager& manager);
    ~PresetActions();

    /** What a file dialog is asked for (a test's FileSource answers it without showing anything). */
    struct FileRequest
    {
        bool save = false;
        juce::String title;
        juce::File folder;      // the user folder (may not exist: then the dialog's default place is used)
        juce::String pattern;   // load: "*.vsprog"
        juce::String extension; // save: ".vsprog"
        juce::String suggestedName; // save: the file name without extension
    };
    /** Delivers the chosen file, or never calls back (cancelled). Default: an asynchronous juce::FileChooser. */
    using FileSource = std::function<void (const FileRequest&, std::function<void (const juce::File&)>)>;
    void setFileSource (FileSource source) { fileSource = std::move (source); }
    /** Where an error message goes. Default: an asynchronous alert box. */
    void setErrorHandler (std::function<void (const juce::String&)> handler) { onError = std::move (handler); }
    /** Shows `error` unless it is empty. */
    void report (const juce::String& error);

    // ---- File flows (each asks for a file, then calls the manager and reports its message) ----
    void loadProgramFile();
    void loadBankFile();
    void loadToneFile (int part);
    void saveProgramAs();
    void saveBankAs();
    void saveToneAs (int part);

    /** Store in this slot (a user bank, not detached), else the "Save as" menu below. Same flow in the
        top bar and the Browser. `anchor` is where the menu opens. */
    void save (juce::Component& anchor);
    /** True if save() stores in the current slot without asking. */
    bool savesInPlace() const;
    void storeInSlot (int slot) { report (presets.storeInBank (slot)); }

    // ---- Menus (the plain bar's Load and Save menus; ids are stable) ----
    // Load: 1 program file, 2 bank file, 10 + tone tone file. Save: 1 program as, 2 bank as, 3 store in this
    // slot (a user bank), 10 + tone tone as. Save as (Browser): 1 program file, 2 bank file (a copy of the
    // bank shown: the way to a user bank), 100 + slot store in that slot (user banks).
    void showLoadMenu (juce::Component& target);
    void showSaveMenu (juce::Component& target);
    void showSaveAsMenu (juce::Component& target);
    /** 0 (dismissed) does nothing. Public for tests. */
    void handleLoadChoice (int choice);
    void handleSaveChoice (int choice);
    void handleSaveAsChoice (int choice);

private:
    void chooseAndLoad (const juce::String& title, const juce::File& folder, const juce::String& pattern,
                        std::function<juce::String (const juce::File&)> action);
    void chooseAndSave (const juce::String& title, const juce::File& folder, const juce::String& extension,
                        std::function<juce::String (const juce::File&)> action);
    void request (const FileRequest& r, std::function<void (const juce::File&)> done);

    PresetManager& presets;
    FileSource fileSource;
    std::function<void (const juce::String&)> onError;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_WEAK_REFERENCEABLE (PresetActions)
    JUCE_DECLARE_NON_COPYABLE (PresetActions)
};

// ---- What the program models need from the manager (message thread or any: copies) ----
/** name and category of every slot of the bank that is shown. */
std::vector<ui::model::SlotInfo> slotInfos (const PresetManager& presets);
/** "A1" ... of every slot. */
std::vector<std::string> positionNames (const PresetManager& presets);
/** For a Pending or Missing session bank: the status text shown as the one disabled menu entry; else empty. */
std::string bankUnavailableText (const PresetManager& presets);
} // namespace vs::presets
