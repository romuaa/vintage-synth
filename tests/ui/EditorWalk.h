#pragma once

// Walks the whole component tree of an editor (every page, both parts). Test only.

#include "EditorRig.h"

#include <cctype>
#include <functional>
#include <typeinfo>
#include <vector>

namespace vs::itest
{
/** Calls `f` for `root` and for every visible component below it (parents before children). */
inline void forEachVisible (juce::Component& root, const std::function<void (juce::Component&)>& f)
{
    f (root);
    for (auto* child : root.getChildren())
        if (child->isVisible())
            forEachVisible (*child, f);
}

/** The compiler's name of the dynamic type. Mangled on GCC and Clang, readable on MSVC: tests only
    search it for a class name (`contains ("ChipStrip")`), which both spellings keep. */
inline juce::String typeNameOf (const juce::Component& c)
{
    return juce::String (typeid (c).name());
}

/** The class's own name without its namespaces and enclosing classes ("ToneSourceSwitch" for
    vs::ui::BrowserPage::ToneSourceSwitch), from the Itanium-mangled name (GCC, Clang) or MSVC's readable one. */
inline juce::String classNameOf (const juce::Component& c)
{
    const auto name = typeNameOf (c);
    if (name.contains ("::")) // MSVC: "class vs::ui::BrowserPage::ToneSourceSwitch"
        return name.fromLastOccurrenceOf ("::", false, false).trim();
    const auto s = name.toStdString(); // "N2vs2ui11BrowserPage16ToneSourceSwitchE" or "6Slider" for an unnested one
    size_t i = s.rfind ('E') == s.size() - 1 && s.front() == 'N' ? 1 : 0;
    std::string last;
    while (i < s.size() && std::isdigit (static_cast<unsigned char> (s[i])) != 0)
    {
        size_t n = 0;
        while (i < s.size() && std::isdigit (static_cast<unsigned char> (s[i])) != 0)
            n = n * 10 + static_cast<size_t> (s[i++] - '0');
        last = s.substr (i, n);
        i += n;
    }
    return last.empty() ? name : juce::String (last);
}

/** Every page and part the editor shows, in rail order: { page index, edited part }. */
inline std::vector<std::pair<int, int>> allPages()
{
    return { { 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 2, 0 }, { 2, 1 }, { 3, 0 }, { 4, 0 }, { 5, 0 }, { 6, 0 } };
}

/** Selects a page the way the rail does and lets the editor catch up. */
inline void showPage (EditorRig& rig, int page, int part)
{
    rig.editor->selectPage (page, part);
    rig.tick();
    rig.tick();
}
} // namespace vs::itest
