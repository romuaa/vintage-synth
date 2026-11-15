#pragma once

// The Browser page (docs/architecture.md 9.3.7): bank chooser, search, category chips, the 8 x 8
// grid of the bank's programs and the panel of the selected program (load, load one tone into a
// part, save, init, import and export). It has no parameters; everything it does is a
// PresetManager or PresetActions call, the same flows as the top bar. It reads the bank through
// slotMeta and refreshes when bankGeneration changes (or the manager broadcasts a change); like
// every page it is driven by the editor's tick (the mirror's idle hook), never by a timer of its own.
//
// The logic that does not need JUCE (category groups, search, arrow movement) is in
// common/ui/models/ProgramModels.h and tested there.

#include "common/presets/PresetActions.h"
#include "common/ui/PaintFonts.h"
#include "common/ui/ParamMirror.h"
#include "common/ui/PreparedText.h"
#include "common/ui/controls/BoundComponent.h"
#include "common/ui/controls/Panel.h"
#include "common/ui/models/ProgramModels.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <vector>

namespace vs::ui
{
/** The grid of programs: one component, 64 cells laid out when the bank changes. paint() allocates nothing
    (prepared glyph runs), and a state change repaints only the cells involved. */
class ProgramGrid final : public juce::Component
{
public:
    explicit ProgramGrid (const Theme& theme);

    static constexpr int kCellW = 100, kCellH = 66, kGap = 4, kLabelW = 28, kHeaderH = 22;
    static constexpr int kWidth = kLabelW + 8 * kCellW + 7 * kGap, kHeight = kHeaderH + 8 * kCellH + 7 * kGap;

    /** Rebuilds the cells (names, positions, stripes). Not a per-frame call: only when the bank changed. */
    void setSlots (const std::vector<model::SlotInfo>& infos, const std::vector<std::string>& positions, const std::vector<juce::Colour>& stripes);
    /** `lit[i]` false: the cell is dimmed to 30 %. Repaints the cells whose state changed. */
    void setLit (const std::vector<bool>& lit);
    /** Repaints the cells that stop or start being selected / current, and the current one when the dot changes. */
    void setState (int selectedSlot, int currentSlot, bool modified);

    int numSlots() const noexcept { return static_cast<int> (cells.size()); }
    int selected() const noexcept { return selectedSlot; }
    int current() const noexcept { return currentSlot; }
    bool isLit (int slot) const { return slot >= 0 && slot < numSlots() && cells[static_cast<size_t> (slot)].lit; }
    juce::Rectangle<int> cellBounds (int slot) const;
    /** The area repainted for a cell: the cell and its outline (inside the gap to the next one). */
    juce::Rectangle<int> cellRepaintArea (int slot) const { return cellBounds (slot).expanded (3); }
    int slotAt (juce::Point<int> p) const;
    /** How often the cells were rebuilt (tests: only when the bank changed). */
    int rebuilds() const noexcept { return rebuildCount; }
    /** Heap bytes of the prepared text (the grid's whole cache; there is no image). */
    size_t cacheBytes() const noexcept;

    std::function<void (int)> onSelect, onLoad;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override { repaintSelected(); }
    void focusLost (FocusChangeType) override { repaintSelected(); }
    /** Role list; the title names the selected program, the press action loads it. */
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    /** Names the grid for screen readers ("Programs, selected A1 Init") and announces the change. */
    void setSelectionDescription (const juce::String& selected);

private:
    struct Cell
    {
        PreparedText name, position;
        juce::Colour stripe;
        bool lit = true;
    };
    void select (int slot);
    void repaintCell (int slot);
    void repaintSelected() { repaintCell (selectedSlot); }

    const Theme& theme;
    std::vector<Cell> cells;
    std::vector<PreparedText> columnLabels, rowLabels;
    int selectedSlot = 0, currentSlot = 0, rebuildCount = 0;
    bool modifiedDot = false;
};

/** "All" plus one chip per category. Chips are as wide as their text (minimum and maximum, long names cut
    in the middle), on two rows; categories that do not fit go to a "More..." menu. Hovering a chip names
    its full category in the help line. */
class ChipStrip final : public juce::Component
{
public:
    explicit ChipStrip (const Theme& theme);
    struct Chip
    {
        juce::String label; // the full category name
        juce::Colour colour;
    };
    /** `chips`: the categories (without "All"); `active`: 0 = All, i = chips[i - 1]. */
    void setChips (std::vector<Chip> chips, int active);
    int active() const noexcept { return activeChip; }
    void choose (int index); // as a click does
    int numChips() const noexcept { return static_cast<int> (chips.size()); }
    /** Bounds of the chip `index` (0 = All, i = chips[i-1]) or an empty rectangle if it is in "More". */
    juce::Rectangle<int> chipBounds (int index) const;
    juce::Rectangle<int> moreBounds() const;
    /** The text drawn on chip `index` (cut in the middle when long). */
    juce::String shownLabel (int index) const;
    /** The chip under a point: 0 = All, i = a category, -1 = "More...", -2 = none. */
    int chipAt (juce::Point<int> p) const;
    std::function<void (int)> onChoose;

    void paint (juce::Graphics&) override;
    void resized() override { rebuildLayout(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    /** Role group whose value is the chosen category ("All" or its name); typing a category name chooses it. */
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    juce::String activeLabel() const;

    static constexpr int kChipH = 26, kMinW = 64, kMaxW = 150, kAllW = 56, kMoreW = 80, kGap = 4, kDot = 14, kPad = 10;

private:
    struct Slot
    {
        juce::Rectangle<int> bounds;
        int chip; // 0 = All, i = a category, -1 = "More"
        PreparedText text;
        juce::String shown;
    };
    void rebuildLayout();
    void showMore();
    void setHoverLabel (const juce::String& label);

    const Theme& theme;
    std::vector<Chip> chips;
    std::vector<Slot> layout;
    int activeChip = 0, focusSlot = 0;
};

class BrowserPage final : public juce::Component,
                          public InteractiveContent,
                          private ParamMirror::IdleClient,
                          private juce::ChangeListener
{
public:
    BrowserPage (UiContext& ctx, presets::PresetManager& manager, std::function<int()> editedPart);
    ~BrowserPage() override;

    /** Chip and stripe colour of the category with this index (final colours come with M7f). */
    static juce::Colour categoryColour (int index);

    // ---- State and introspection for tests ----
    int selectedSlot() const noexcept { return selected; }
    void selectSlot (int slot);                  // as a click does
    void loadSelected();                         // as double-click, Enter or "Load program" do
    int toneSource() const noexcept { return source; }
    void setToneSource (int tone);
    void loadToneInto (int part);                // "Load tone > Lower / Upper"
    std::string activeCategory() const;          // "" for All
    void setSearch (const juce::String& text);
    juce::String getSearch() const { return search.getText(); }
    bool upperToneHeard() const noexcept { return upperHeard; }
    juce::String bankButtonText() const { return bank.getButtonText(); }
    void chooseBank (int choice);                // the bank menu's result
    const std::vector<model::CategoryGroup>& categoryGroupsShown() const noexcept { return groups; }
    ProgramGrid& getGrid() noexcept { return grid; }
    ChipStrip& getChips() noexcept { return chips; }
    juce::TextButton& getLoadButton() noexcept { return loadProgram; }
    juce::TextButton& getToneButton (int part) noexcept { return part == 0 ? toLower : toUpper; }
    juce::TextButton& getSaveButton() noexcept { return saveButton; }
    juce::TextButton& getSaveAsButton() noexcept { return saveAs; }
    juce::TextButton& getInitButton() noexcept { return initButton; }
    juce::TextButton& getImportButton() noexcept { return importBank; }
    juce::TextButton& getExportButton() noexcept { return exportBank; }
    juce::TextButton& getSaveToneButton() noexcept { return saveTone; }
    juce::TextButton& getLoadToneFileButton() noexcept { return loadToneFile; }
    juce::TextButton& getBankButton() noexcept { return bank; }
    juce::TextEditor& getSearchBox() noexcept { return search; }
    presets::PresetActions& getActions() noexcept { return actions; }
    int bankRefreshes() const noexcept { return bankRefreshCount; }
    void cancelInteraction() override {}

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class ToneSourceSwitch;
    class SelectedInfo;

    void mirrorIdle (double nowMs) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { dirty.store (true, std::memory_order_relaxed); }
    void refreshBank();
    void refreshState();
    void refreshLit();
    bool infosStale() const;
    void showBankMenu();
    juce::String bankText() const;

    UiContext& ctx;
    presets::PresetManager& presets;
    std::function<int()> editedPart;
    presets::PresetActions actions;

    ProgramGrid grid;
    ChipStrip chips;
    juce::TextButton bank;
    juce::TextEditor search;
    Panel panel;
    std::unique_ptr<SelectedInfo> info;
    std::unique_ptr<ToneSourceSwitch> toneSwitch;
    juce::Label takeLabel, programsLabel, toneFilesLabel, banksLabel, upperNote, readOnlyNote;
    juce::TextButton loadProgram { "Load program" }, toLower { "Load tone > Lower" }, toUpper { "Load tone > Upper" }, saveButton { "Save" },
        saveAs { "Save as..." }, initButton { "Init (blank)" }, saveTone { "Save tone..." }, loadToneFile { "Load tone file..." },
        importBank { "Import..." }, exportBank { "Export bank..." };

    std::vector<model::SlotInfo> infos;
    std::vector<model::CategoryGroup> groups;
    int selected = 0, source = 0;
    int keyModeIndex = -1;
    bool upperHeard = true;
    std::uint32_t seenGeneration = 0;
    int seenCurrent = -1, bankRefreshCount = 0;
    struct StateKey
    {
        int current = -1, status = -1;
        bool modified = false, detached = false;
        bool operator== (const StateKey& o) const noexcept
        {
            return current == o.current && status == o.status && modified == o.modified && detached == o.detached;
        }
    } seenState;
    std::atomic<bool> dirty { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrowserPage)
};
} // namespace vs::ui
