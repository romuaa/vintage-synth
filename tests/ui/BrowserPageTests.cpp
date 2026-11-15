// The Browser page and the program menu on the program name (docs/architecture.md 9.3.7, 9.3.8, 9.6):
// the grid follows the bank and the current program, selection and loading by mouse, keys and
// buttons, tone loading from a chosen slot, search and category dimming, refresh only when the bank
// changed, save / init / import / export flows (through a test dialog), the top bar's program menu
// model and its keyboard operation, layout snapshot, paint allocations, lifetime.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EditorRig.h"
#include "EditorWalk.h"
#include "NullGraphicsContext.h"
#include "../preset_unit/PresetTestHelpers.h"
#include "vintage/presets/VintagePresets.h"
#include "vintage/ui/BrowserPage.h"

#include <thread>

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


// =================================================================================================
// Review round 1
// =================================================================================================

namespace
{
/** A menu captured instead of shown: what it held and the callback JUCE would call with the choice. */
struct MenuCapture
{
    std::vector<juce::PopupMenu> menus;
    std::vector<std::function<void (int)>> callbacks;
    void install (presets::PresetActions& a)
    {
        a.setMenuRunner ([this] (juce::PopupMenu m, juce::Component&, std::function<void (int)> done) {
            menus.push_back (std::move (m));
            callbacks.push_back (std::move (done));
        });
    }
    void choose (int choice) { callbacks.back() (choice); }
};

/** A bank of 64 programs with 40-character names and 30 categories (some empty). */
presets::Bank stressBank (const presets::Schema& schema)
{
    auto bank = presets::makeInitBank (schema);
    bank.name = "Stress Bank With A Long Name Indeed";
    for (int i = 0; i < 64; ++i)
    {
        auto& m = bank.programs[static_cast<size_t> (i)].meta;
        m.name = (juce::String ("Program number ") + juce::String (i).paddedLeft ('0', 2) + " with a very long name x").substring (0, 40);
        m.category = i % 9 == 0 ? juce::String() : (i % 30 == 5 ? juce::String ("Category Number Twelve Hundred ") + juce::String (i) : "Category " + juce::String (i % 30 + 1));
        m.author = "Somebody With A Long Name";
    }
    return bank;
}

/** Records every area the editor is asked to repaint (installed as its cached image). */
struct RepaintRecorder final : juce::CachedComponentImage
{
    void paint (juce::Graphics&) override {}
    bool invalidateAll() override
    {
        ++all;
        return false;
    }
    bool invalidate (const juce::Rectangle<int>& area) override
    {
        areas.push_back (area);
        return false;
    }
    void releaseResources() override {}
    std::vector<juce::Rectangle<int>> areas;
    int all = 0;
};

juce::Rectangle<int> inEditor (BrowserRig& rig, juce::Component& c, juce::Rectangle<int> r) { return rig.editor->getLocalArea (&c, r); }
} // namespace

TEST_CASE ("Top bar: Save is the contract's Save in the Vintage bar (in place in a user bank, else Save as) and the menu in the second plugin's bar", "[ui][browser][bar]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& bar = rig.editor->getBar();
    auto& pm = rig.pm();
    REQUIRE (bar.smartSave);
    MenuCapture menus;
    menus.install (bar.getActions());
    Dialogs dialogs;
    dialogs.install (bar.getActions());

    // Factory bank: nothing to store into; the "Save as" menu opens (program file, bank copy; no slot list).
    rig.set ("lower.vcf.cutoff", 0.3f);
    bar.getSaveButton().onClick();
    REQUIRE (menus.menus.size() == 1);
    CHECK (dialogs.errors.isEmpty());
    CHECK_FALSE ((rig.temp / "Banks").exists());
    {
        juce::PopupMenu::MenuItemIterator it (menus.menus.back());
        int items = 0;
        while (it.next())
            if (it.getItem().itemID != 0 || it.getItem().subMenu != nullptr)
                ++items;
        CHECK (items == 2);
    }

    // A user bank and a slot: Save writes the bank file at once and opens no menu.
    const auto file = rig.makeUserBank (rig.temp.dir);
    rig.set ("lower.vcf.cutoff", 0.77f);
    REQUIRE (pm.renameProgram (0, "Saved From Bar").isEmpty());
    bar.getSaveButton().onClick();
    CHECK (menus.menus.size() == 1);
    CHECK_FALSE (pm.isModified());
    PresetRig other;
    REQUIRE (other.presets().loadBankFile (file).isEmpty());
    CHECK (other.presets().programName (0) == "Saved From Bar");

    // The second plugin's bar does not set the flag: Save opens its menu, whatever the bank.
    presets::PresetBar plain (pm);
    CHECK_FALSE (plain.smartSave);
    MenuCapture plainMenus;
    plainMenus.install (plain.getActions());
    rig.set ("lower.vcf.cutoff", 0.2f);
    plain.getSaveButton().onClick();
    CHECK (plainMenus.menus.size() == 1);
    CHECK (pm.isModified());
}

TEST_CASE ("Menus: a choice that names a slot is refused when the bank changed while the menu was open", "[ui][browser][menu]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& pm = rig.pm();
    auto& bar = rig.editor->getBar();
    const auto file = rig.makeUserBank (rig.temp.dir);
    MenuCapture menus;
    menus.install (bar.getActions());
    Dialogs dialogs;
    dialogs.install (bar.getActions());
    juce::Component anchor;

    SECTION ("Save as > Store in slot")
    {
        const auto before = readText (file);
        rig.set ("lower.vcf.cutoff", 0.9f);
        bar.getActions().showSaveAsMenu (anchor);
        REQUIRE (menus.callbacks.size() == 1);
        pm.setBank ({}, juce::File()); // the bank changes while the menu is open
        menus.choose (100 + 5);
        CHECK (dialogs.errors.size() == 1);
        CHECK (dialogs.errors[0].contains ("bank changed"));
        CHECK (readText (file) == before);

        // Without a change the same choice stores.
        REQUIRE (pm.loadBankFile (file).isEmpty());
        dialogs.errors.clear();
        rig.set ("lower.vcf.cutoff", 0.9f);
        bar.getActions().showSaveAsMenu (anchor);
        menus.choose (100 + 5);
        CHECK (dialogs.errors.isEmpty());
        CHECK (readText (file) != before);
    }
    SECTION ("Save menu > Store in this slot")
    {
        REQUIRE (pm.loadBankFile (file).isEmpty());
        const auto before = readText (file);
        rig.set ("lower.vcf.cutoff", 0.9f);
        bar.getActions().showSaveMenu (anchor);
        pm.setBank ({}, juce::File());
        menus.choose (3);
        CHECK (dialogs.errors.size() == 1);
        CHECK (readText (file) == before);
        // The other items of that menu do not depend on the slot: "Program as..." still opens its dialog.
        menus.choose (1);
        CHECK (dialogs.requests.size() == 1);
    }
    SECTION ("the program menu")
    {
        pm.selectProgram (3);
        bar.showProgramMenu();
        REQUIRE (menus.callbacks.size() == 1);
        pm.setBank ({}, juce::File());
        pm.selectProgram (10);
        menus.choose (20 + 1);
        CHECK (pm.currentIndex() == 10); // not loaded: it would have been another program
        CHECK (dialogs.errors.size() == 1);
        CHECK (dialogs.errors[0].contains ("nothing was loaded"));
        // Open Browser is not about a slot: it works whatever changed.
        rig.editor->selectPage (0, 0);
        menus.choose (presets::PresetBar::kOpenBrowserChoice);
        CHECK (rig.editor->getFrame().getPage() == kBrowserPage);
        // The same menu opened again and not disturbed selects.
        bar.showProgramMenu();
        menus.choose (20 + 1);
        CHECK (pm.currentIndex() == 20);
    }
}

TEST_CASE ("Grid paint allocates nothing; nothing repaints at rest; a selection or program change repaints only the cells involved", "[ui][browser][paint]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    rig.editor->setSizePercent (100);
    rig.editor->setVisible (true);
    auto& page = rig.page();
    auto& grid = page.getGrid();
    rig.tick();

#if VS_SEQ_COUNT_ALLOCATIONS
    {
        // The harness sees an allocation (so a zero below means something).
        vs::seq::test::AllocationCounter probe;
        juce::String s ("an allocation the counter must see");
        s += juce::String (probe.count());
        CHECK (probe.count() > 0);
    }
    const auto allocations = [] (juce::Component& c, bool focused) {
        (void) focused;
        NullGraphicsContext context ({ 0, 0, 2000, 1000 }, true);
        juce::Graphics g (context);
        c.paint (g); // warm
        vs::seq::test::AllocationCounter counter;
        c.paint (g);
        c.paint (g);
        CHECK (context.glyphRuns > 0);
        return counter.count();
    };
    CHECK (allocations (grid, false) == 0);
    CHECK (allocations (page.getChips(), false) == 0);
    grid.grabKeyboardFocus(); // the focus ring too
    CHECK (allocations (grid, true) == 0);
#endif

    auto* recorder = new RepaintRecorder();
    rig.editor->setCachedComponentImage (recorder);
    const auto gridArea = inEditor (rig, grid, grid.getLocalBounds());
    const auto inGrid = [&] {
        std::vector<juce::Rectangle<int>> v;
        for (const auto& a : recorder->areas)
            if (a.intersects (gridArea))
                v.push_back (a);
        return v;
    };
    const auto cellArea = [&] (int slot) { return inEditor (rig, grid, grid.cellRepaintArea (slot)); };
    const auto expectOnly = [&] (std::initializer_list<int> slots) {
        const auto areas = inGrid();
        CHECK_FALSE (areas.empty()); // the recorder does see repaints of a visible editor
        for (const auto& a : areas)
        {
            bool inside = false;
            for (int s : slots)
                inside |= cellArea (s).contains (a);
            INFO ("area " << a.toString());
            CHECK (inside);
        }
        for (int s : slots)
        {
            bool covered = false;
            for (const auto& a : areas)
                covered |= a.intersects (cellArea (s));
            CHECK (covered);
        }
    };

    // At rest nothing is repainted.
    recorder->areas.clear();
    for (int i = 0; i < 5; ++i)
        rig.tick();
    CHECK (inGrid().empty());

    page.selectSlot (12); // selected 5 -> 12: two cells
    CHECK (rig.pm().currentIndex() == 0);
    const int from = grid.selected() == 12 ? 0 : -1; // (selection started at the current program, slot 0)
    (void) from;
    expectOnly ({ 0, 12 });
    CHECK (inGrid().size() == 2);

    recorder->areas.clear();
    grid.onLoad (20); // double-click on 20: selected 12 -> 20, current 0 -> 20 (after the next tick)
    rig.tick();
    expectOnly ({ 0, 12, 20 });

    recorder->areas.clear();
    rig.set ("lower.vcf.cutoff", 0.33f); // the program becomes modified: the dot on the current cell only
    rig.tick();
    expectOnly ({ 20 });

    recorder->areas.clear();
    page.setSearch ("zzzz"); // everything but nothing-matching dims: only the cells that were lit change, no whole-grid repaint
    for (const auto& a : inGrid())
        CHECK (a.getWidth() * a.getHeight() < gridArea.getWidth() * gridArea.getHeight() / 8);
    rig.editor->setCachedComponentImage (nullptr);
}

TEST_CASE ("Grid cache memory: the prepared text of a bank with 40-character names stays under 1 MB; there is no image", "[ui][browser][paint]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    rig.pm().setBank (stressBank (rig.schema()), rig.temp.dir.getChildFile ("Banks").getChildFile ("stress.vsbank"));
    rig.pm().selectProgram (3);
    rig.tick();
    auto& grid = rig.page().getGrid();
    CHECK (grid.getCachedComponentImage() == nullptr);
    const auto bytes = grid.cacheBytes();
    WARN ("worst-case prepared text of the grid: " << bytes << " bytes");
    CHECK (bytes > 0);
    CHECK (bytes < 1024u * 1024u);
}

TEST_CASE ("Chips are as wide as their text; long names are cut in the middle; the overflow is in More; hover names the full category", "[ui][browser][chips]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    rig.pm().setBank (stressBank (rig.schema()), rig.temp.dir.getChildFile ("Banks").getChildFile ("stress.vsbank"));
    rig.pm().selectProgram (3);
    rig.tick();
    auto& page = rig.page();
    auto& chips = page.getChips();
    const auto& groups = page.categoryGroupsShown();
    REQUIRE (groups.size() > 20);

    int widest = 0, narrowest = 1000, cut = 0, visible = 0;
    for (int i = 1; i <= chips.numChips(); ++i)
    {
        const auto r = chips.chipBounds (i);
        if (r.isEmpty())
            continue;
        ++visible;
        widest = std::max (widest, r.getWidth());
        narrowest = std::min (narrowest, r.getWidth());
        CHECK (r.getWidth() >= ui::ChipStrip::kMinW);
        CHECK (r.getWidth() <= ui::ChipStrip::kMaxW);
        const auto shown = chips.shownLabel (i);
        const auto full = juce::String::fromUTF8 (groups[static_cast<size_t> (i - 1)].label.c_str());
        if (shown != full)
        {
            ++cut;
            CHECK (shown.contains (juce::String::fromUTF8 ("\xE2\x80\xA6")));
            CHECK (shown.startsWith (full.substring (0, 4)));
            CHECK (shown.endsWith (full.getLastCharacters (2)));
            CHECK (shown.length() < full.length());
        }
    }
    CHECK (narrowest < widest); // not all alike
    CHECK (cut >= 1);
    CHECK_FALSE (chips.moreBounds().isEmpty());
    CHECK (visible < chips.numChips());
    // No two chips overlap, all lie inside the strip.
    std::vector<juce::Rectangle<int>> rects;
    for (int i = -1; i <= chips.numChips(); ++i)
    {
        const auto r = i < 0 ? chips.moreBounds() : chips.chipBounds (i);
        if (r.isEmpty())
            continue;
        CHECK (chips.getLocalBounds().contains (r));
        for (const auto& o : rects)
            CHECK_FALSE (r.intersects (o));
        rects.push_back (r);
    }

    // Hover: the help line names the full category, also when the same strip stays hovered across chips.
    auto& frame = rig.editor->getFrame();
    int first = 0, second = 0;
    for (int i = 1; i <= chips.numChips() && second == 0; ++i)
        if (! chips.chipBounds (i).isEmpty())
            (first == 0 ? first : second) = i;
    for (int idx : { first, second })
    {
        chips.mouseMove (mouseEvent (chips, chips.chipBounds (idx).getCentre(), 0));
        frame.hoverHelp (&chips);
        const auto full = juce::String::fromUTF8 (groups[static_cast<size_t> (idx - 1)].label.c_str());
        INFO (full);
        CHECK (frame.getHelpLine().getText().startsWith ("Category " + full + ": "));
    }
    chips.mouseExit (mouseEvent (chips, { 0, 0 }, 0));
    frame.hoverHelp (&chips);
    CHECK (frame.getHelpLine().getText().startsWith ("Category: "));
}

TEST_CASE ("Top bar: a 20-character program name fits at 100 %; the bank label is short; Init moved to the Browser", "[ui][browser][bar]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    rig.editor->setSizePercent (100);
    auto& bar = rig.editor->getBar();
    const juce::String name ("Twin Horizon Lead XL"); // 20 characters
    REQUIRE (name.length() == 20);
    REQUIRE (rig.pm().renameProgram (rig.pm().currentIndex(), name).isEmpty());
    rig.tick();
    bar.setSize (bar.getWidth(), bar.getHeight());
    auto& button = bar.getNameButton();
    CHECK (button.getButtonText() == name);
    // The button paints its text between 8 px of padding and the 16 px of its chevron.
    const float room = static_cast<float> (button.getWidth() - 2 * 8 - 16);
    CHECK (ui::PreparedText::widthOf (name, rig.editor->getTheme().font (16.0f, true)) < room);
    CHECK (bar.describe().contains ("Factory  A1  " + name));
    CHECK (static_cast<juce::Label&> (bar.getBankLabel()).getText() == "Factory");
    CHECK_FALSE (bar.getInitButton().isVisible());
    // The second plugin's bar keeps its Init button.
    presets::PresetBar plain (rig.pm());
    CHECK (plain.getInitButton().isVisible());
}

TEST_CASE ("Bank chooser: Factory shows the bank without loading anything while the sound is modified", "[ui][browser][files]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& page = rig.page();
    auto& pm = rig.pm();
    rig.makeUserBank (rig.temp.dir);
    pm.selectProgram (6);
    rig.set ("lower.vcf.cutoff", 0.123f); // an edit not saved
    REQUIRE (pm.isModified());
    const auto before = rig.raw();
    page.chooseBank (1);
    CHECK (pm.isFactoryBank());
    CHECK (rig.raw() == before);        // the sound is kept
    CHECK (pm.isModified());
    CHECK (pm.currentIndex() == 6);

    // Nothing modified: the first program of the bank is loaded, as Load bank file does.
    rig.makeUserBank (rig.temp.dir);
    pm.selectProgram (6);
    REQUIRE_FALSE (pm.isModified());
    page.chooseBank (1);
    CHECK (pm.isFactoryBank());
    CHECK (pm.currentIndex() == 0);
}

TEST_CASE ("Browser keyboard order is the reading order; the search box reacts to typing, not to a poll", "[ui][browser][page]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& page = rig.page();
    juce::KeyboardFocusTraverser traverser;
    CHECK (traverser.getNextComponent (&page.getBankButton()) == &page.getSearchBox());
    CHECK (traverser.getNextComponent (&page.getSearchBox()) == &page.getChips());
    CHECK (traverser.getNextComponent (&page.getChips()) == &page.getGrid());
    auto* afterGrid = traverser.getNextComponent (&page.getGrid());
    REQUIRE (afterGrid != nullptr);
    CHECK (page.isParentOf (afterGrid));
    CHECK_FALSE (page.getGrid().isParentOf (afterGrid));
    CHECK (afterGrid != &page.getSearchBox());
    CHECK (traverser.getPreviousComponent (&page.getBankButton()) != &page.getChips()); // not the old y-order

    // Typing: the editor's change message calls onTextChange; a tick does not look at the text.
    page.getSearchBox().setText ("twin", false);
    page.getSearchBox().onTextChange(); // what the editor's (asynchronous) text-change message calls
    CHECK_FALSE (page.getGrid().isLit (0));
    CHECK_FALSE (page.getGrid().isLit (0));
    CHECK (page.getGrid().isLit (kSlotDual));
    page.getSearchBox().setText ("", false); // no notification: nothing polls the text
    rig.tick();
    rig.tick();
    CHECK_FALSE (page.getGrid().isLit (0));
}

TEST_CASE ("Chip middle ellipsis keeps the start and the end of a name", "[ui][browser][model]")
{
    using ui::model::middleEllipsis;
    const std::string e = "\xE2\x80\xA6";
    CHECK (middleEllipsis ("short", 10) == "short");
    CHECK (middleEllipsis ("exactlyten", 10) == "exactlyten");
    CHECK (middleEllipsis ("Category Number 12", 10) == "Categ" + e + "r 12");
    CHECK (middleEllipsis ("Category Number 12", 9) == "Cate" + e + "r 12");
    CHECK (middleEllipsis ("abcdefghij", 2) == "a" + e);
    CHECK (middleEllipsis ("abcdefghij", 1) == e);
    CHECK (middleEllipsis ("abcdefghij", 0) == e);
    CHECK (middleEllipsis ("", 3).empty());
    // Characters are code points: a multi-byte letter is never split.
    const std::string accents = "\xC3\xA4\xC3\xB6\xC3\xBC\xC3\xA4\xC3\xB6\xC3\xBC\xC3\xA4\xC3\xB6"; // 8 letters
    const auto cut = middleEllipsis (accents, 5);
    CHECK (juce::String::fromUTF8 (cut.c_str()).length() == 5);
    CHECK (juce::CharPointer_UTF8::isValidString (cut.c_str(), static_cast<int> (cut.size())));
}

TEST_CASE ("UI shots (hidden): the Browser with the factory bank and a stress bank at 75, 100 and 200 %", "[.uishots]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VS_UISHOT_DIR", {});
    if (dir.isEmpty())
        SKIP ("VS_UISHOT_DIR not set");
    const juce::File out (dir);
    REQUIRE (out.createDirectory());
    BrowserRig rig;
    const auto shoot = [&] (const juce::String& prefix) {
        for (int scale : { 75, 100, 200 })
        {
            rig.editor->setSizePercent (scale);
            rig.tick();
            const auto image = rig.editor->createComponentSnapshot (rig.editor->getLocalBounds(), true, 1.0f);
            const auto file = out.getChildFile (prefix + "_" + juce::String (scale) + ".png");
            file.deleteFile();
            juce::FileOutputStream stream (file);
            REQUIRE (stream.openedOk());
            REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
        }
    };
    rig.pm().selectProgram (kSlotDual);
    rig.editor->getBrowserPage().selectSlot (kSlotDual);
    rig.tick();
    shoot ("browser_factory");
    rig.pm().setBank (stressBank (rig.schema()), rig.temp.dir.getChildFile ("Banks").getChildFile ("stress.vsbank"));
    rig.pm().selectProgram (5);
    rig.tick();
    rig.editor->getBrowserPage().selectSlot (17);
    rig.editor->getBrowserPage().getChips().choose (2);
    rig.tick();
    shoot ("browser_stress");
}

TEST_CASE ("Browser accessibility: the grid names its selection and loads it on press; the chips and the tone switch take their value as text", "[ui][browser][a11y]")
{
    VS_SKIP_EDITOR_UNDER_MAC_ASAN();
    BrowserRig rig;
    auto& page = rig.page();
    auto& pm = rig.pm();

    SECTION ("program grid")
    {
        auto& grid = page.getGrid();
        const auto handler = grid.createAccessibilityHandler();
        REQUIRE (handler != nullptr);
        CHECK (handler->getRole() == juce::AccessibilityRole::list);
        page.selectSlot (5);
        rig.tick();
        CHECK (grid.getTitle() == "Programs, selected " + pm.positionName (5) + " " + pm.slotMeta (5).name);
        CHECK (pm.currentIndex() != 5); // selecting loads nothing
        REQUIRE (handler->getActions().invoke (juce::AccessibilityActionType::press));
        CHECK (pm.currentIndex() == 5); // press loads the selected slot
        rig.tick();                     // (the page follows the program change first)
        page.selectSlot (23);
        rig.tick();
        CHECK (grid.getTitle() == "Programs, selected " + pm.positionName (23) + " " + pm.slotMeta (23).name);
        REQUIRE (handler->getActions().invoke (juce::AccessibilityActionType::press));
        CHECK (pm.currentIndex() == 23);
    }
    SECTION ("category chips")
    {
        auto& chips = page.getChips();
        const auto handler = chips.createAccessibilityHandler();
        REQUIRE (handler != nullptr);
        auto* value = handler->getValueInterface();
        REQUIRE (value != nullptr);
        CHECK (value->getCurrentValueAsString() == "All");
        REQUIRE (page.categoryGroupsShown().size() > 1);
        const auto category = juce::String (page.categoryGroupsShown()[1].label);
        value->setValueAsString (category.toUpperCase()); // case does not matter
        CHECK (chips.active() == 2);
        CHECK (value->getCurrentValueAsString() == category);
        CHECK (page.activeCategory() == category.toStdString());
        value->setValueAsString ("No such category");
        CHECK (chips.active() == 2); // an unknown value changes nothing
        value->setValueAsString ("all");
        CHECK (chips.active() == 0);
        CHECK (page.activeCategory().empty());
    }
    SECTION ("tone source switch")
    {
        juce::Component* sw = nullptr;
        forEachVisible (page, [&] (juce::Component& c) { sw = classNameOf (c) == "ToneSourceSwitch" ? &c : sw; });
        REQUIRE (sw != nullptr);
        const auto handler = sw->createAccessibilityHandler();
        auto* value = handler->getValueInterface();
        REQUIRE (value != nullptr);
        CHECK (page.toneSource() == 0);
        CHECK (value->getCurrentValueAsString() == "Lower tone");
        value->setValueAsString ("Upper tone");
        CHECK (page.toneSource() == 1);
        CHECK (value->getCurrentValueAsString() == "Upper tone");
        value->setValueAsString ("Lower tone");
        CHECK (page.toneSource() == 0);
    }
}
