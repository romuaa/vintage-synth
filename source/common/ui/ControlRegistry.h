#pragma once

// Which control places which parameter (docs/architecture.md 9.3.10, 9.6): every bound control
// registers here when it is built. The registry test checks that the primary placements are the
// parameter list exactly once each, that every secondary binding's parameter has its primary on the
// same page or in the frame, and that segment captions match the choice counts; the automation-
// follow test asks every binding whether it shows its parameter's current value.

#include <juce_core/juce_core.h>

#include <vector>

namespace vs::ui
{
/** A control (or graph handle) bound to one parameter, as the tests see it. */
class BoundControl
{
public:
    virtual ~BoundControl() = default;
    /** True if the control shows the parameter's value the mirror holds now (bitwise, its text,
        or for a graph handle the position the page's mapping gives); else `why` says what differs. */
    virtual bool showsMirroredValue (juce::String& why) const = 0;
};

enum class Placement
{
    Primary,  // the one control that places the parameter
    Secondary // a graph handle or shortcut whose primary is on the same page or in the frame
};

struct ControlEntry
{
    juce::String paramId;
    juce::String page;   // page name, or "frame" (top bar, output popover)
    int part = -1;       // 0 Lower, 1 Upper (part pages), -1 global
    Placement placement = Placement::Primary;
    juce::String control; // "Slider", "SegmentedSwitch", "Toggle", "GraphHandle"
    int captions = 0;     // segment captions shown (0: not a segmented control)
    int choices = 0;      // the parameter's choice count (0: not a choice)
    const BoundControl* view = nullptr;
};

class ControlRegistry
{
public:
    void add (ControlEntry e) { list.push_back (std::move (e)); }
    const std::vector<ControlEntry>& entries() const noexcept { return list; }
    /** Records how many segment captions a registered control shows. */
    void setCaptions (const BoundControl* view, int captions)
    {
        for (auto& e : list)
            if (e.view == view)
                e.captions = captions;
    }

    /** Context for the controls built next (pages register through it). */
    void setContext (const juce::String& page, int part)
    {
        currentPage = page;
        currentPart = part;
    }
    const juce::String& page() const noexcept { return currentPage; }
    int part() const noexcept { return currentPart; }

private:
    std::vector<ControlEntry> list;
    juce::String currentPage { "frame" };
    int currentPart = -1;
};
} // namespace vs::ui
