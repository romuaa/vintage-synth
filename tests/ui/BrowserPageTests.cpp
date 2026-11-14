// The Browser page and the program menu on the program name (docs/architecture.md 9.3.7, 9.3.8, 9.6):
// the grid follows the bank and the current program, selection and loading by mouse, keys and
// buttons, tone loading from a chosen slot, search and category dimming, refresh only when the bank
// changed, save / init / import / export flows (through a test dialog), the top bar's program menu
// model and its keyboard operation, layout snapshot, paint allocations, lifetime.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EditorRig.h"
#include "../preset_unit/PresetTestHelpers.h"
#include "vintage/presets/VintagePresets.h"
#include "vintage/ui/BrowserPage.h"

using namespace vs;
using namespace vs::itest;
using namespace vs::ptest;
using Catch::Approx;

namespace
{
constexpr int kBrowserPage = 6;

juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<int> pos, int clicks)
{
    const auto now = juce::Time::getCurrentTime();
    const auto p = pos.toFloat();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now,
                             p, now, clicks, false);
}

juce::Point<int> centreOf (ui::ProgramGrid& g, int slot) { return g.cellBounds (slot).getCentre(); }

struct Dialogs
{
    juce::File answer;
    std::vector<presets::PresetActions::FileRequest> requests;
    juce::StringArray errors;
    void install (presets::PresetActions& a)
    {
        a.setFileSource ([this] (const presets::PresetActions::FileRequest& r, std::function<void (const juce::File&)> done) {
            requests.push_back (r);
            if (answer != juce::File())
                done (answer);
        });
        a.setErrorHandler ([this] (const juce::String& e) { errors.add (e); });
    }
};

struct BrowserRig : EditorRig
{
    TempDir temp;
    BrowserRig()
    {
        proc.getPresets().setLocations ({ temp.dir });
        proc.getPresets().getGate().setTimeouts (0, 0);
        editor->selectPage (kBrowserPage, 0);
        tick();
    }
    ui::BrowserPage& page() { return editor->getBrowserPage(); }
    presets::PresetManager& pm() { return proc.getPresets(); }
    const presets::Schema& schema() { return proc.getPresets().getSchema(); }
    std::vector<float> raw()
    {
        std::vector<float> v;
        for (int i = 0; i < schema().numParams(); ++i)
            v.push_back (proc.getState().getRawParameterValue (schema().spec (i).id)->load());
        return v;
    }
    /** A user bank file made through the manager (the factory bank copied under a new name). */
    juce::File makeUserBank (const juce::File& dir, const juce::String& name = "My Bank")
    {
        const auto file = dir.getChildFile ("Banks").getChildFile ("mine.vsbank");
        REQUIRE (pm().saveBankFile (file, name).isEmpty());
        return file;
    }
};

std::vector<bool> litOf (ui::ProgramGrid& g)
{
    std::vector<bool> v;
    for (int s = 0; s < g.numSlots(); ++s)
        v.push_back (g.isLit (s));
    return v;
}

void dumpTree (const juce::Component& c, int depth, juce::String& out)
{
    if (! c.isVisible())
        return;
    auto name = c.getComponentID();
    if (name.isEmpty())
        name = c.getTitle();
    if (name.isEmpty())
        name = c.getName();
    if (name.isEmpty())
        name = "-";
    out << juce::String::repeatedString ("  ", depth) << name << " " << c.getBounds().toString() << "\n";
    for (auto* child : c.getChildren())
        dumpTree (*child, depth + 1, out);
}
} // namespace

TEST_CASE ("Browser: the grid shows the 64 programs of the bank; the page has no parameters; selection starts at the current program", "[ui][browser][page]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& page = rig.page();
    auto& grid = page.getGrid();
    CHECK (grid.numSlots() == 64);
    CHECK (page.selectedSlot() == rig.pm().currentIndex());
    CHECK (grid.current() == rig.pm().currentIndex());
    CHECK (page.bankButtonText() == "Factory (read-only)");
    for (int s = 0; s < 64; ++s)
        CHECK (grid.isLit (s));
    CHECK (page.categoryGroupsShown().size() > 1);
    CHECK (page.getChips().numChips() == static_cast<int> (page.categoryGroupsShown().size()));
    CHECK (rig.editor->getFrame().currentPageComponent() == &page);

    // No parameters are bound on this page.
    int onBrowser = 0;
    for (const auto& e : rig.editor->getRegistry().entries())
        onBrowser += e.page == "browser" ? 1 : 0;
    CHECK (onBrowser == 0);
}

TEST_CASE ("Browser: selection follows program changes; click selects, double-click, Enter and Load program load; arrows move", "[ui][browser][page]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& page = rig.page();
    auto& grid = page.getGrid();
    const auto factory = makeVintageFactoryBank (rig.schema());

    rig.pm().selectProgram (5); // a host program change, `>`, the program menu
    rig.tick();
    CHECK (page.selectedSlot() == 5);
    CHECK (grid.current() == 5);

    grid.mouseDown (mouseEvent (grid, centreOf (grid, 12), 1));
    CHECK (page.selectedSlot() == 12);
    CHECK (rig.pm().currentIndex() == 5); // selecting loads nothing
    grid.mouseDown (mouseEvent (grid, { 2, 2 }, 1)); // the header: not a cell
    grid.mouseDown (mouseEvent (grid, { ui::ProgramGrid::kLabelW + ui::ProgramGrid::kCellW + 1, ui::ProgramGrid::kHeaderH + 1 }, 1)); // a gap between cells
    CHECK (page.selectedSlot() == 12);

    grid.mouseDoubleClick (mouseEvent (grid, centreOf (grid, 20), 2));
    CHECK (rig.pm().currentIndex() == 20);
    CHECK (rig.pm().currentName() == factory.programs[20].meta.name);
    CHECK_FALSE (rig.pm().isModified());
    rig.tick();
    CHECK (page.selectedSlot() == 20);

    CHECK (grid.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    CHECK (page.selectedSlot() == 21);
    CHECK (grid.keyPressed (juce::KeyPress (juce::KeyPress::downKey)));
    CHECK (page.selectedSlot() == 29);
    CHECK (grid.keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));
    CHECK (grid.keyPressed (juce::KeyPress (juce::KeyPress::upKey)));
    CHECK (page.selectedSlot() == 20);
    CHECK (rig.pm().currentIndex() == 20);
    CHECK (grid.keyPressed (juce::KeyPress (juce::KeyPress::endKey)));
    CHECK (page.selectedSlot() == 23);
    CHECK_FALSE (grid.keyPressed (juce::KeyPress ('x')));
    CHECK (grid.keyPressed (juce::KeyPress (juce::KeyPress::returnKey)));
    CHECK (rig.pm().currentIndex() == 23);

    page.selectSlot (41);
    page.getLoadButton().onClick();
    CHECK (rig.pm().currentIndex() == 41);
    rig.tick();
    CHECK (page.selectedSlot() == 41);

    // Everything the editor shows follows (9.1.5): the mirror holds every parameter after the load.
    auto& mirror = rig.editor->getMirror();
    for (int i = 0; i < mirror.size(); ++i)
        REQUIRE (sameBitsF (mirror.value (i), mirror.parameter (i).getValue()));
}

TEST_CASE ("Browser: the modified marker follows an edit and a save; a missing selection never leaves the grid", "[ui][browser][page]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& page = rig.page();
    page.selectSlot (-4);
    CHECK (page.selectedSlot() == 0);
    page.selectSlot (99);
    CHECK (page.selectedSlot() == 63);
    page.getInitButton().onClick();
    CHECK (rig.pm().currentName() == "Init");
    CHECK (rig.pm().isModified());
    CHECK (rig.pm().currentIndex() == 0); // the slot keeps its place (the program of slot A1 was current)
    rig.tick();
    CHECK (page.getGrid().current() == rig.pm().currentIndex());
}

TEST_CASE ("Browser: Load tone takes one tone of the selected program into a part and changes nothing else", "[ui][browser][tone]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& page = rig.page();
    const auto& schema = rig.schema();
    const auto factory = makeVintageFactoryBank (schema);
    const auto& dual = factory.programs[kSlotDual];

    rig.pm().selectProgram (3);
    rig.tick();
    const auto before = rig.raw();
    CHECK (page.toneSource() == 0); // default: Lower tone

    page.selectSlot (kSlotDual);
    page.setToneSource (1); // its Upper tone
    page.getToneButton (0).onClick(); // -> Lower
    auto now = rig.raw();
    for (int k = 0; k < schema.toneSize(); ++k)
    {
        const int lower = schema.toneParam (0, k), upper = schema.toneParam (1, k);
        CHECK (std::abs (now[static_cast<size_t> (lower)] - dual.values[static_cast<size_t> (upper)]) <= 1.0e-4f * (schema.spec (lower).hi - schema.spec (lower).lo));
        CHECK (sameBitsF (now[static_cast<size_t> (upper)], before[static_cast<size_t> (upper)])); // the other part stays
    }
    for (int g : schema.globalParams())
        CHECK (sameBitsF (now[static_cast<size_t> (g)], before[static_cast<size_t> (g)]));
    CHECK (rig.pm().currentIndex() == 3);
    CHECK (rig.pm().isModified());

    page.setToneSource (0); // its Lower tone
    page.getToneButton (1).onClick(); // -> Upper
    now = rig.raw();
    for (int k = 0; k < schema.toneSize(); ++k)
    {
        const int lower = schema.toneParam (0, k), upper = schema.toneParam (1, k);
        CHECK (std::abs (now[static_cast<size_t> (upper)] - dual.values[static_cast<size_t> (lower)]) <= 1.0e-4f * (schema.spec (upper).hi - schema.spec (upper).lo));
    }

    // Everything the editor shows follows.
    rig.tick();
    auto& mirror = rig.editor->getMirror();
    for (int i = 0; i < mirror.size(); ++i)
        REQUIRE (sameBitsF (mirror.value (i), mirror.parameter (i).getValue()));
}

TEST_CASE ("Browser: the Upper tone is marked as not heard for a program in Whole mode", "[ui][browser][tone]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& page = rig.page();
    page.selectSlot (0); // Init: Whole
    CHECK_FALSE (page.upperToneHeard());
    page.selectSlot (kSlotDual);
    CHECK (page.upperToneHeard());
    page.selectSlot (0);
    CHECK_FALSE (page.upperToneHeard());
}

TEST_CASE ("Browser: search and category chips dim programs (30 %), never move them; both apply together", "[ui][browser][search]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& page = rig.page();
    auto& grid = page.getGrid();
    auto& chips = page.getChips();
    const auto factory = makeVintageFactoryBank (rig.schema());
    std::vector<juce::Rectangle<int>> bounds;
    for (int s = 0; s < 64; ++s)
        bounds.push_back (grid.cellBounds (s));

    page.setSearch ("twin");
    std::vector<bool> expect;
    for (const auto& p : factory.programs)
        expect.push_back (ui::model::matchesSearch (p.meta.name.toStdString(), p.meta.category.toStdString(), "twin"));
    CHECK (litOf (grid) == expect);
    CHECK (grid.isLit (kSlotDual));
    CHECK (std::count (expect.begin(), expect.end(), true) < 64);
    CHECK (std::count (expect.begin(), expect.end(), true) >= 1);

    page.setSearch ("");
    for (int s = 0; s < 64; ++s)
        CHECK (grid.isLit (s));
    page.setSearch ("zzzzzz");
    for (int s = 0; s < 64; ++s)
        CHECK_FALSE (grid.isLit (s));
    page.setSearch ("");

    const auto& groups = page.categoryGroupsShown();
    REQUIRE (groups.size() > 1);
    chips.choose (2); // the second category
    CHECK (page.activeCategory() == groups[1].label);
    for (int s = 0; s < 64; ++s)
        CHECK (grid.isLit (s) == (std::find (groups[1].slots.begin(), groups[1].slots.end(), s) != groups[1].slots.end()));
    // Search inside the category: both must match.
    const auto nameInGroup = factory.programs[static_cast<size_t> (groups[1].slots.front())].meta.name;
    page.setSearch (nameInGroup);
    CHECK (grid.isLit (groups[1].slots.front()));
    int lit = 0;
    for (int s = 0; s < 64; ++s)
        lit += grid.isLit (s) ? 1 : 0;
    CHECK (lit >= 1);
    CHECK (lit <= static_cast<int> (groups[1].slots.size()));
    page.setSearch ("");
    chips.choose (0); // All
    CHECK (page.activeCategory().empty());
    for (int s = 0; s < 64; ++s)
        CHECK (grid.isLit (s));

    for (int s = 0; s < 64; ++s)
        CHECK (grid.cellBounds (s) == bounds[static_cast<size_t> (s)]);
}

TEST_CASE ("Browser: a bank with many, empty and duplicate categories gets one chip per distinct category; the overflow is reachable", "[ui][browser][search]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& pm = rig.pm();
    auto bank = presets::makeInitBank (rig.schema());
    for (int i = 0; i < 64; ++i)
    {
        bank.programs[static_cast<size_t> (i)].meta.name = "P" + juce::String (i);
        bank.programs[static_cast<size_t> (i)].meta.category = i < 4 ? juce::String() : "Cat" + juce::String (i % 30 + 1);
    }
    pm.setBank (bank, rig.temp.dir.getChildFile ("Banks").getChildFile ("many.vsbank"));
    pm.selectProgram (10); // the current slot shows the loaded sound's own metadata: load one of this bank
    rig.tick();
    auto& page = rig.page();
    auto& chips = page.getChips();
    // 30 distinct categories + Uncategorised (slots 0-3 and, from the modulus, nothing else empty).
    CHECK (page.categoryGroupsShown().size() == 31);
    CHECK (page.categoryGroupsShown().front().label == "Uncategorised");
    CHECK (chips.numChips() == 31);
    CHECK_FALSE (chips.moreBounds().isEmpty());
    int visible = 0;
    std::vector<juce::Rectangle<int>> rects;
    for (int i = 0; i <= chips.numChips(); ++i)
    {
        const auto r = chips.chipBounds (i);
        if (! r.isEmpty())
        {
            ++visible;
            CHECK (chips.getLocalBounds().contains (r));
            for (const auto& other : rects)
                CHECK_FALSE (r.intersects (other));
            rects.push_back (r);
        }
    }
    CHECK_FALSE (chips.getLocalBounds().isEmpty());
    CHECK (visible < chips.numChips() + 1);
    CHECK_FALSE (chips.moreBounds().intersects (chips.chipBounds (0)));
    // A category that is only in "More" can still be chosen, and then dims everything else.
    chips.choose (chips.numChips());
    CHECK (page.activeCategory() == page.categoryGroupsShown().back().label);
    int lit = 0;
    for (int s = 0; s < 64; ++s)
        lit += page.getGrid().isLit (s) ? 1 : 0;
    CHECK (lit == static_cast<int> (page.categoryGroupsShown().back().slots.size()));
    // Back to the factory bank: the chosen category is gone, All is shown.
    page.chooseBank (1);
    rig.tick();
    CHECK (page.activeCategory().empty());
    CHECK (page.getGrid().isLit (0));
}

TEST_CASE ("Browser: the cells are rebuilt only when the bank shown, or a slot's name or category, changed", "[ui][browser][page]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& page = rig.page();
    auto& pm = rig.pm();
    const int base = page.bankRefreshes();
    for (int i = 0; i < 5; ++i)
        rig.tick();
    pm.selectProgram (9);
    pm.applyTone (pm.captureTone (0), 1);
    rig.set ("lower.vcf.cutoff", 0.4f);
    rig.tick();
    CHECK (page.bankRefreshes() == base);
    CHECK (page.getGrid().rebuilds() == base);
    // Init renames the current program: its cell shows "Init" now.
    pm.initialise();
    rig.tick();
    CHECK (page.bankRefreshes() == base + 1);

    // The name of the current program changes with no new bank generation (a factory bank rename).
    REQUIRE (pm.renameProgram (pm.currentIndex(), "Renamed Now").isEmpty());
    rig.tick();
    CHECK (page.bankRefreshes() == base + 2);

    const auto file = rig.makeUserBank (rig.temp.dir);
    rig.tick();
    CHECK (page.bankRefreshes() == base + 3);
    CHECK (page.bankButtonText() == "My Bank");
    REQUIRE (pm.renameProgram (4, "Four").isEmpty());
    rig.tick();
    CHECK (page.bankRefreshes() == base + 4);
    REQUIRE (pm.storeInBank (7).isEmpty());
    rig.tick();
    CHECK (page.bankRefreshes() == base + 5);
    pm.setBank ({}, juce::File());
    rig.tick();
    CHECK (page.bankRefreshes() == base + 6);
    CHECK (page.bankButtonText() == "Factory (read-only)");
    (void) file;
}

TEST_CASE ("Browser: Save stores in place in a user bank; Init, Import and Export run the shared flows; errors are reported", "[ui][browser][files]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& page = rig.page();
    auto& pm = rig.pm();
    rig.pm().setLocations ({ rig.temp.dir });
    Dialogs dialogs;
    dialogs.install (page.getActions());

    const auto bankFile = rig.makeUserBank (rig.temp.dir);
    rig.tick();
    CHECK (page.getActions().savesInPlace());
    rig.set ("lower.vcf.cutoff", 0.77f);
    REQUIRE (pm.renameProgram (0, "From Browser").isEmpty());
    CHECK (pm.isModified());
    page.getSaveButton().onClick(); // user bank + slot: the file is written, no dialog
    CHECK (dialogs.requests.empty());
    CHECK (dialogs.errors.isEmpty());
    CHECK_FALSE (pm.isModified());
    {
        PresetRig other;
        REQUIRE (other.presets().loadBankFile (bankFile).isEmpty());
        CHECK (other.presets().programName (0) == "From Browser");
    }

    // Export: a dialog for a bank file; Import: a dialog for a bank file, then the bank is shown.
    dialogs.answer = rig.temp / "exported";
    page.getExportButton().onClick();
    REQUIRE (dialogs.requests.size() == 1);
    CHECK (dialogs.requests.back().extension == ".vsbank");
    CHECK ((rig.temp / "exported").withFileExtension (".vsbank").existsAsFile());
    pm.setBank ({}, juce::File());
    rig.tick();
    dialogs.answer = bankFile;
    page.getImportButton().onClick();
    REQUIRE (dialogs.requests.size() == 2);
    CHECK (dialogs.requests.back().pattern == "*.vsbank");
    rig.tick();
    CHECK (pm.bankStatus() == presets::BankStatus::User);
    CHECK (page.bankButtonText() == "My Bank");
    CHECK (page.getGrid().current() == 0);

    // A file that is not a bank: refused by the strict reader, reported, nothing changes.
    const auto junk = rig.temp / "junk.vsbank";
    REQUIRE (junk.replaceWithText ("[1, 2, 3]"));
    dialogs.answer = junk;
    const auto before = rig.raw();
    page.getImportButton().onClick();
    CHECK (dialogs.errors.size() == 1);
    CHECK (rig.raw() == before);
    CHECK (page.bankButtonText() == "My Bank");

    // Tone files act on the part being edited.
    rig.editor->selectPage (kBrowserPage, 1);
    rig.tick();
    dialogs.answer = rig.temp / "tone";
    page.getSaveToneButton().onClick();
    CHECK (dialogs.requests.back().extension == ".vstone");
    rig.set ("lower.vcf.cutoff", 0.5f);
    rig.set ("upper.vcf.cutoff", 0.6f);
    dialogs.answer = (rig.temp / "tone").withFileExtension (".vstone");
    REQUIRE (dialogs.answer.existsAsFile());
    page.getLoadToneFileButton().onClick(); // the Upper tone file into the Upper part (the edited one)
    CHECK (dialogs.errors.size() == 1);
    CHECK (rig.get ("lower.vcf.cutoff") == Approx (0.5f).margin (0.01));
}

TEST_CASE ("Browser: the bank chooser switches to the factory bank (loading its first program) and loads a bank file", "[ui][browser][files]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& page = rig.page();
    auto& pm = rig.pm();
    Dialogs dialogs;
    dialogs.install (page.getActions());
    const auto file = rig.makeUserBank (rig.temp.dir);
    pm.selectProgram (6);
    page.chooseBank (1);
    CHECK (pm.isFactoryBank());
    CHECK (pm.currentIndex() == 0);
    page.chooseBank (1); // already the factory bank: nothing
    CHECK (pm.currentIndex() == 0);
    dialogs.answer = file;
    page.chooseBank (3);
    CHECK (dialogs.requests.size() == 1);
    CHECK (pm.bankStatus() == presets::BankStatus::User);
    page.chooseBank (0);
    page.chooseBank (99);
    CHECK (pm.bankStatus() == presets::BankStatus::User);
}

TEST_CASE ("Program menu: the top bar's model, choosing, Open Browser, keyboard operation and help", "[ui][browser][menu]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    auto& bar = rig.editor->getBar();
    auto& pm = rig.proc.getPresets();
    const auto factory = makeVintageFactoryBank (pm.getSchema());

    auto menu = bar.programMenuModel();
    CHECK (menu.entryCount() == 64);
    CHECK_FALSE (menu.flat());
    pm.selectProgram (kSlotDual);
    menu = bar.programMenuModel();
    int ticked = -1;
    for (const auto& g : menu.groups)
        for (const auto& e : g.entries)
            if (e.ticked)
                ticked = e.slot;
    CHECK (ticked == kSlotDual);
    bool found = false;
    for (const auto& g : menu.groups)
        for (const auto& e : g.entries)
            if (e.slot == kSlotDual)
            {
                found = true;
                CHECK (e.text == pm.positionName (kSlotDual).toStdString() + "  " + factory.programs[kSlotDual].meta.name.toStdString());
            }
    CHECK (found);

    // Choosing an entry is selectProgram (the same flow as < and >).
    juce::Component::SafePointer<presets::PresetBar> safe (&bar);
    presets::PresetBar::handleProgramMenuResult (safe, 0);
    CHECK (pm.currentIndex() == kSlotDual);
    presets::PresetBar::handleProgramMenuResult (safe, 10 + 1);
    CHECK (pm.currentIndex() == 10);
    presets::PresetBar::handleProgramMenuResult (safe, 65); // not a program
    CHECK (pm.currentIndex() == 10);
    // Open Browser switches the page (and keeps the edited part).
    rig.editor->selectPage (0, 1);
    presets::PresetBar::handleProgramMenuResult (safe, presets::PresetBar::kOpenBrowserChoice);
    CHECK (rig.editor->getFrame().getPage() == kBrowserPage);
    CHECK (rig.editor->getFrame().getPart() == 1);

    // Keyboard: Enter or Space on the focused name opens the menu (here: counted instead of shown).
    auto& name = bar.getNameButton();
    auto& nameKeys = static_cast<juce::Component&> (name); // Button keeps keyPressed protected
    CHECK (name.getWantsKeyboardFocus());
    int opened = 0;
    name.onClick = [&opened] { ++opened; };
    CHECK (nameKeys.keyPressed (juce::KeyPress (juce::KeyPress::returnKey)));
    CHECK (nameKeys.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)));
    CHECK (opened == 2);
    CHECK_FALSE (nameKeys.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
    CHECK_FALSE (nameKeys.keyPressed (juce::KeyPress ('a')));
    CHECK (opened == 2);
    CHECK (name.getDescription().isNotEmpty());

    // Help on the program name (the bar's help text is found from its child).
    auto& frame = rig.editor->getFrame();
    frame.hoverHelp (&name);
    CHECK (frame.getHelpLine().getText().startsWith ("Program: Click the program name"));

    // The bar is gone (the editor closed with a menu open): the result does nothing.
    rig.editor.reset();
    CHECK (safe == nullptr);
    presets::PresetBar::handleProgramMenuResult (safe, 3);
    presets::PresetBar::handleProgramMenuResult (safe, presets::PresetBar::kOpenBrowserChoice);
    CHECK (pm.currentIndex() == 10);
}

TEST_CASE ("Program menu: a bank that is not found shows one disabled entry with its status", "[ui][browser][menu]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    PresetRig a;
    REQUIRE (a.presets().saveBankFile ((a.temp / "Banks").getChildFile ("gone.vsbank"), "Gone Bank").isEmpty());
    juce::MemoryBlock state;
    a.proc.getStateInformation (state);

    EditorRig rig;
    rig.proc.getPresets().setLocations ({ juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("vs_nowhere_" + juce::Uuid().toString()) });
    rig.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    auto menu = rig.editor->getBar().programMenuModel();
    REQUIRE (menu.groups.size() == 1);
    REQUIRE (menu.groups[0].entries.size() == 1);
    CHECK_FALSE (menu.groups[0].entries[0].enabled);
    CHECK (menu.groups[0].entries[0].text == "Gone Bank (loading)");
    rig.proc.getPresets().pollRequests();
    menu = rig.editor->getBar().programMenuModel();
    REQUIRE (menu.groups[0].entries.size() == 1);
    CHECK (menu.groups[0].entries[0].text == "bank not found: Gone Bank");
    CHECK_FALSE (menu.groups[0].entries[0].enabled);
    rig.tick();
    CHECK (rig.editor->getBrowserPage().bankButtonText() == "bank not found: Gone Bank");
}

TEST_CASE ("Browser: a dialog still open when the editor closes does nothing afterwards", "[ui][browser][lifetime]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    std::function<void (const juce::File&)> pending;
    rig.page().getActions().setFileSource ([&pending] (const presets::PresetActions::FileRequest&, std::function<void (const juce::File&)> done) {
        pending = std::move (done);
    });
    rig.page().getImportButton().onClick();
    REQUIRE (pending != nullptr);
    const auto file = rig.temp.dir.getChildFile ("Banks").getChildFile ("late.vsbank");
    REQUIRE (rig.pm().saveBankFile (file, "Late").isEmpty());
    rig.pm().setBank ({}, juce::File());
    rig.editor.reset();
    pending (file);
    CHECK (rig.pm().isFactoryBank());
}

TEST_CASE ("Layout snapshot: the Browser page's component bounds equal the golden files", "[ui][editor][layout][browser]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    EditorRig rig;
    const bool update = juce::SystemStats::getEnvironmentVariable ("VS_UPDATE_UI_GOLDENS", {}) == "1";
    for (int scale : { 100, 150 })
    {
        rig.editor->setSizePercent (scale);
        rig.editor->selectPage (kBrowserPage, 0);
        rig.tick();
        juce::String dump;
        dump << "editor " << rig.editor->getBounds().toString() << " scale " << juce::String (rig.editor->getScale(), 4) << "\n";
        dumpTree (rig.editor->getFrame(), 0, dump);
        const auto file = juce::File (VS_UI_GOLDEN_DIR).getChildFile ("browser_" + juce::String (scale) + ".txt");
        if (update)
        {
            REQUIRE (file.replaceWithText (dump, false, false, "\n"));
            continue;
        }
        INFO (file.getFileName() << " (VS_UPDATE_UI_GOLDENS=1 rewrites it after an intended layout change)");
        REQUIRE (file.existsAsFile());
        CHECK (file.loadFileAsString().removeCharacters ("\r") == dump);
    }
}

#if VS_SEQ_COUNT_ALLOCATIONS
TEST_CASE ("Browser: repainting the grid and the chips with unchanged values allocates the same small number of times (cached image)", "[ui][browser][paint]")
{
    BrowserRig rig;
    auto& grid = rig.page().getGrid();
    auto& chips = rig.page().getChips();
    // What a repaint with nothing changed does: the component's cached image is drawn (it is
    // re-rendered only after repaint() invalidated it, i.e. when the bank or the state changed).
    const auto paintCount = [] (juce::Component& c) {
        auto* cache = c.getCachedComponentImage();
        REQUIRE (cache != nullptr);
        juce::Image image (juce::Image::ARGB, c.getWidth(), c.getHeight(), true);
        juce::Graphics g (image);
        cache->paint (g); // renders the cache
        cache->paint (g);
        vs::seq::test::AllocationCounter counter;
        cache->paint (g);
        const int first = counter.count();
        vs::seq::test::AllocationCounter counter2;
        cache->paint (g);
        return std::pair<int, int> { first, counter2.count() };
    };
    const auto g = paintCount (grid);
    CHECK (g.first == g.second);
    CHECK (g.first <= 8);
    const auto ch = paintCount (chips);
    CHECK (ch.first == ch.second);
    CHECK (ch.first <= 8);

    // The uncached drawing of 64 cells is what the cache avoids: it allocates in the renderer, far more.
    juce::Image image (juce::Image::ARGB, grid.getWidth(), grid.getHeight(), true);
    juce::Graphics g2 (image);
    grid.paintEntireComponent (g2, false);
    vs::seq::test::AllocationCounter counter;
    grid.paintEntireComponent (g2, false);
    CHECK (counter.count() > 8);
}
#endif
