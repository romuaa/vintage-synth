#pragma once

// Logic of the program Browser and of the program menu on the program name (docs/architecture.md
// 9.3.7, 9.3.8). JUCE-free: plain strings and numbers, so it is tested on every target.
//
//   categoryGroups   the distinct categories of a bank in order of first appearance (empty text:
//                    "Uncategorised"), each with its slots. Chips and menu groups are both this.
//   programMenu      the menu of a bank: groups (a flat list when there is one group), entries
//                    "A3  Name", the current program ticked; a bank that is not usable (a session's
//                    user bank that is pending or missing) is one disabled entry with its status.
//   matchesSearch    the search box: case-insensitive substring of name or category.
//   moveSelection    arrow-key movement in the grid of groups x slots per group (rows x columns).
//
// Text is UTF-8. Case folding is ASCII only (other characters must match exactly).

#include <cstddef>
#include <string>
#include <vector>

namespace vs::ui::model
{
inline constexpr const char* kUncategorised = "Uncategorised";

/** What the models need to know about one slot of a bank. */
struct SlotInfo
{
    std::string name, category;
};

struct CategoryGroup
{
    std::string label;      // the category as shown ("Uncategorised" for none)
    std::vector<int> slots; // ascending
};

namespace detail
{
inline std::string trimmed (const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
        --b;
    return s.substr (a, b - a);
}

inline std::string lowered (const std::string& s)
{
    std::string r = s;
    for (auto& c : r)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char> (c - 'A' + 'a');
    return r;
}
} // namespace detail

/** The label a category is shown under: surrounding blanks removed, empty -> "Uncategorised". */
inline std::string categoryLabel (const std::string& category)
{
    auto t = detail::trimmed (category);
    return t.empty() ? std::string (kUncategorised) : t;
}

inline std::vector<CategoryGroup> categoryGroups (const std::vector<SlotInfo>& slots)
{
    std::vector<CategoryGroup> groups;
    for (size_t i = 0; i < slots.size(); ++i)
    {
        const auto label = categoryLabel (slots[i].category);
        CategoryGroup* g = nullptr;
        for (auto& existing : groups)
            if (existing.label == label)
            {
                g = &existing;
                break;
            }
        if (g == nullptr)
        {
            groups.push_back ({ label, {} });
            g = &groups.back();
        }
        g->slots.push_back (static_cast<int> (i));
    }
    return groups;
}

/** Index of the group (chip) a slot belongs to, or -1. */
inline int groupOfSlot (const std::vector<CategoryGroup>& groups, int slot)
{
    for (size_t g = 0; g < groups.size(); ++g)
        for (int s : groups[g].slots)
            if (s == slot)
                return static_cast<int>(g);
    return -1;
}

struct MenuEntry
{
    std::string text; // "A3  Name"
    int slot = -1;    // -1: a note (disabled)
    bool ticked = false, enabled = true;
};

struct MenuGroup
{
    std::string label; // empty for a flat menu
    std::vector<MenuEntry> entries;
};

struct ProgramMenu
{
    /** One group with an empty label when the bank has a single category (no submenus). */
    std::vector<MenuGroup> groups;
    bool flat() const noexcept { return groups.size() == 1 && groups.front().label.empty(); }
    size_t entryCount() const noexcept
    {
        size_t n = 0;
        for (const auto& g : groups)
            n += g.entries.size();
        return n;
    }
};

/** `positions[i]` is the slot's position name ("A1"); `current` the slot to tick (-1: none, e.g. a
    detached sound); `unavailable` non-empty: the bank cannot be used now and that text (its status)
    is shown as one disabled entry instead of the programs. */
inline ProgramMenu programMenu (const std::vector<SlotInfo>& slots, const std::vector<std::string>& positions, int current,
                                const std::string& unavailable = {})
{
    ProgramMenu menu;
    if (! unavailable.empty())
    {
        menu.groups.push_back ({ {}, { { unavailable, -1, false, false } } });
        return menu;
    }
    const auto groups = categoryGroups (slots);
    for (const auto& g : groups)
    {
        MenuGroup mg;
        mg.label = groups.size() > 1 ? g.label : std::string();
        for (int s : g.slots)
        {
            const auto i = static_cast<size_t> (s);
            const std::string pos = i < positions.size() ? positions[i] : std::string();
            mg.entries.push_back ({ pos + "  " + slots[i].name, s, s == current, true });
        }
        menu.groups.push_back (std::move (mg));
    }
    return menu;
}

/** An empty query matches everything. */
inline bool matchesSearch (const std::string& name, const std::string& category, const std::string& query)
{
    const auto q = detail::lowered (detail::trimmed (query));
    if (q.empty())
        return true;
    return detail::lowered (name).find (q) != std::string::npos || detail::lowered (category).find (q) != std::string::npos;
}

enum class Move
{
    Left,
    Right,
    Up,
    Down,
    RowStart,
    RowEnd,
    First,
    Last
};

/** The slot reached from `slot` in a grid of `rows` x `columns` (slot = row * columns + column).
    Movement stops at the edges (no wrap); an out-of-range slot is first clamped. */
inline int moveSelection (int slot, Move move, int rows, int columns)
{
    const int count = rows * columns;
    if (count <= 0)
        return 0;
    slot = slot < 0 ? 0 : (slot >= count ? count - 1 : slot);
    const int row = slot / columns, col = slot % columns;
    switch (move)
    {
        case Move::Left: return col > 0 ? slot - 1 : slot;
        case Move::Right: return col < columns - 1 ? slot + 1 : slot;
        case Move::Up: return row > 0 ? slot - columns : slot;
        case Move::Down: return row < rows - 1 ? slot + columns : slot;
        case Move::RowStart: return row * columns;
        case Move::RowEnd: return row * columns + columns - 1;
        case Move::First: return 0;
        case Move::Last: return count - 1;
    }
    return slot;
}
} // namespace vs::ui::model
