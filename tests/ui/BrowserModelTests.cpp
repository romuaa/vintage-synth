// The JUCE-free logic of the Browser and the program menu (docs/architecture.md 9.3.7, 9.3.8):
// category groups, the menu model, search and grid movement; against the factory bank and against
// banks with empty and duplicate categories.

#include <catch2/catch_test_macros.hpp>

#include "../preset_unit/PresetTestHelpers.h"
#include "common/ui/models/ProgramModels.h"
#include "vintage/presets/VintagePresets.h"

#include <set>

using namespace vs;
using namespace vs::ui::model;

namespace
{
std::vector<SlotInfo> infosOf (const presets::Bank& bank)
{
    std::vector<SlotInfo> v;
    for (const auto& p : bank.programs)
        v.push_back ({ p.meta.name.toStdString(), p.meta.category.toStdString() });
    return v;
}

std::vector<std::string> positions (int n)
{
    std::vector<std::string> v;
    for (int i = 0; i < n; ++i)
        v.push_back (std::string (1, static_cast<char> ('A' + i / 8)) + std::to_string (i % 8 + 1));
    return v;
}
} // namespace

TEST_CASE ("Categories: distinct values in order of first appearance; empty is Uncategorised; duplicates merge", "[ui][browser][model]")
{
    const std::vector<SlotInfo> slots {
        { "a", "Lead" }, { "b", "" },     { "c", "Pad" },  { "d", "Lead" }, { "e", "  " },
        { "f", "Pad " }, { "g", "Bass" }, { "h", "lead" }, { "i", "Uncategorised" },
    };
    const auto groups = categoryGroups (slots);
    REQUIRE (groups.size() == 5);
    CHECK (groups[0].label == "Lead");
    CHECK (groups[0].slots == std::vector<int> { 0, 3 });
    CHECK (groups[1].label == "Uncategorised"); // "", blanks and the literal word share one group
    CHECK (groups[1].slots == std::vector<int> { 1, 4, 8 });
    CHECK (groups[2].label == "Pad");
    CHECK (groups[2].slots == std::vector<int> { 2, 5 }); // surrounding blanks are not a difference
    CHECK (groups[3].label == "Bass");
    CHECK (groups[4].label == "lead"); // case is a difference: the category text is the user's
    // Every slot is in exactly one group.
    std::multiset<int> all;
    for (const auto& g : groups)
        all.insert (g.slots.begin(), g.slots.end());
    CHECK (all.size() == slots.size());
    CHECK (groupOfSlot (groups, 5) == 2);
    CHECK (groupOfSlot (groups, 99) == -1);
    CHECK (categoryGroups ({}).empty());
}

TEST_CASE ("Program menu: groups, entry text, tick, flat menu for one category, one disabled entry for an unusable bank", "[ui][browser][model]")
{
    const std::vector<SlotInfo> slots { { "Init", "" }, { "Brass", "Brass" }, { "Pad", "Pad" }, { "Horns", "Brass" } };
    const auto pos = positions (4);

    const auto menu = programMenu (slots, pos, 3);
    REQUIRE (menu.groups.size() == 3);
    CHECK_FALSE (menu.flat());
    CHECK (menu.entryCount() == 4);
    CHECK (menu.groups[0].label == "Uncategorised");
    CHECK (menu.groups[1].label == "Brass");
    REQUIRE (menu.groups[1].entries.size() == 2);
    CHECK (menu.groups[1].entries[0].text == "A2  Brass");
    CHECK (menu.groups[1].entries[1].text == "A4  Horns");
    CHECK (menu.groups[1].entries[1].slot == 3);
    int ticked = 0;
    for (const auto& g : menu.groups)
        for (const auto& e : g.entries)
        {
            ticked += e.ticked ? 1 : 0;
            CHECK (e.enabled);
            CHECK (e.ticked == (e.slot == 3));
        }
    CHECK (ticked == 1);

    // No current program (a sound that is in no slot): nothing is ticked.
    for (const auto& g : programMenu (slots, pos, -1).groups)
        for (const auto& e : g.entries)
            CHECK_FALSE (e.ticked);

    // One category: a flat list, no submenu.
    const auto flat = programMenu ({ { "x", "Only" }, { "y", "Only" } }, positions (2), 0);
    CHECK (flat.flat());
    CHECK (flat.groups.front().entries.size() == 2);
    CHECK (flat.groups.front().entries[0].text == "A1  x");
    CHECK (flat.groups.front().entries[0].ticked);

    // Only uncategorised programs: one group too.
    CHECK (programMenu ({ { "x", "" }, { "y", "" } }, positions (2), -1).flat());

    // A bank that is not usable: one disabled entry with its status, no programs.
    const auto missing = programMenu (slots, pos, 0, "bank not found: Mine");
    REQUIRE (missing.groups.size() == 1);
    CHECK (missing.flat());
    REQUIRE (missing.groups[0].entries.size() == 1);
    CHECK (missing.groups[0].entries[0].text == "bank not found: Mine");
    CHECK_FALSE (missing.groups[0].entries[0].enabled);
    CHECK (missing.groups[0].entries[0].slot == -1);

    // Duplicate names are listed as they are; positions tell them apart.
    const auto dup = programMenu ({ { "Same", "A" }, { "Same", "A" } }, positions (2), 1);
    CHECK (dup.groups.front().entries[0].text == "A1  Same");
    CHECK (dup.groups.front().entries[1].text == "A2  Same");
}

TEST_CASE ("Program menu of the factory bank: 64 entries, every slot once, groups as the bank's categories", "[ui][browser][model]")
{
    vs::ptest::PresetRig rig;
    auto& pm = rig.presets();
    const auto bank = makeVintageFactoryBank (rig.schema());
    const auto infos = infosOf (bank);
    REQUIRE (infos.size() == 64);
    std::vector<std::string> pos;
    for (int s = 0; s < 64; ++s)
        pos.push_back (pm.positionName (s).toStdString());

    const int current = vs::ptest::kSlotDual;
    const auto menu = programMenu (infos, pos, current);
    CHECK (menu.entryCount() == 64);
    std::set<int> slots;
    std::vector<std::string> labels;
    for (const auto& g : menu.groups)
    {
        labels.push_back (g.label);
        for (const auto& e : g.entries)
        {
            slots.insert (e.slot);
            CHECK (e.text == pos[static_cast<size_t> (e.slot)] + "  " + infos[static_cast<size_t> (e.slot)].name);
            CHECK (e.ticked == (e.slot == current));
        }
    }
    CHECK (slots.size() == 64);
    // The groups are the distinct categories in order of first appearance in the bank.
    std::vector<std::string> expected;
    for (const auto& i : infos)
    {
        const auto label = categoryLabel (i.category);
        if (std::find (expected.begin(), expected.end(), label) == expected.end())
            expected.push_back (label);
    }
    CHECK (labels == expected);
    CHECK (labels.size() > 1);
    // Entries inside a group keep the bank order.
    for (const auto& g : menu.groups)
        for (size_t i = 1; i < g.entries.size(); ++i)
            CHECK (g.entries[i - 1].slot < g.entries[i].slot);
}

TEST_CASE ("Search: case-insensitive substring of name or category; empty matches all", "[ui][browser][model]")
{
    CHECK (matchesSearch ("Silk Lead", "Lead", ""));
    CHECK (matchesSearch ("Silk Lead", "Lead", "   "));
    CHECK (matchesSearch ("Silk Lead", "Pad", "silk"));
    CHECK (matchesSearch ("Silk Lead", "Pad", "K LE"));
    CHECK (matchesSearch ("Warm Strings", "Strings + Pads", "pads"));
    CHECK_FALSE (matchesSearch ("Silk Lead", "Pad", "brass"));
    CHECK_FALSE (matchesSearch ("Silk", "Pad", "silkk"));
    CHECK (matchesSearch ("Caf\xc3\xa9 Pad", "", "caf\xc3\xa9")); // non-ASCII must match exactly, which it does
}

TEST_CASE ("Grid movement: arrows stop at the edges, home/end per row, ctrl for the corners; bad slots are clamped", "[ui][browser][model]")
{
    const auto mv = [] (int s, Move m) { return moveSelection (s, m, 8, 8); };
    CHECK (mv (0, Move::Left) == 0);
    CHECK (mv (0, Move::Up) == 0);
    CHECK (mv (0, Move::Right) == 1);
    CHECK (mv (0, Move::Down) == 8);
    CHECK (mv (7, Move::Right) == 7);
    CHECK (mv (8, Move::Left) == 8); // no wrap to the previous row
    CHECK (mv (63, Move::Right) == 63);
    CHECK (mv (63, Move::Down) == 63);
    CHECK (mv (63, Move::Up) == 55);
    CHECK (mv (27, Move::RowStart) == 24);
    CHECK (mv (27, Move::RowEnd) == 31);
    CHECK (mv (27, Move::First) == 0);
    CHECK (mv (27, Move::Last) == 63);
    CHECK (mv (-5, Move::Right) == 1);
    CHECK (mv (500, Move::Left) == 62);
    CHECK (moveSelection (3, Move::Down, 0, 0) == 0);
    // Every slot is reachable from slot 0 with arrows alone.
    std::set<int> seen { 0 };
    int s = 0;
    for (int r = 0; r < 8; ++r)
    {
        for (int c = 0; c < 7; ++c)
        {
            s = mv (s, r % 2 == 0 ? Move::Right : Move::Left);
            seen.insert (s);
        }
        s = mv (s, Move::Down);
        seen.insert (s);
    }
    CHECK (seen.size() == 64);
}
