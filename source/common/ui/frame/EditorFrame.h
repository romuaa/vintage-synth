#pragma once

// The editor frame (docs/architecture.md 9.2.2, 9.3.8, 9.2.4): a fixed logical canvas of
// 1360 x 784 px (scaled as a whole by the editor), with the top bar (part switch, "Editing: ..."
// or "Showing: whole instrument", the preset area, output meter, Master slider, LIMIT chip and
// output chevron, Size menu), the page rail, the page area, the help line and the hover bubble,
// and the output popover. The frame writes exactly three parameters: master.volume (top bar) and
// master.limiter / master.limiter.ceiling (popover). Everything is laid out from the grid, never
// from measured text.

#include "common/ui/controls/SegmentedSwitch.h"
#include "common/ui/controls/Slider.h"
#include "common/ui/frame/FrameParts.h"

namespace vs::ui
{
/** The output popover: limiter Off/On, ceiling, gain-reduction meter 0-12 dB. */
class OutputPopover final : public juce::Component
{
public:
    explicit OutputPopover (UiContext& ctx);
    /** Smallest limiter gain since the last update (linear). */
    void setReduction (float gainMin);
    float reductionDb() const noexcept { return reduction; }
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    std::function<void()> onClose;
    SegmentedSwitch& limiterSwitch() noexcept { return limiter; }
    Slider& ceilingSlider() noexcept { return ceiling; }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    const Theme& theme;
    SegmentedSwitch limiter;
    Slider ceiling;
    float reduction = 0.0f;
};

class EditorFrame final : public juce::Component,
                          private juce::Timer,
                          private juce::FocusChangeListener,
                          private ParamMirror::Listener
{
public:
    struct Page
    {
        juce::String name;  // saved in the session ("sound")
        juce::String title; // shown ("Sound")
        PageIcon icon;
        bool partPage;      // shows the edited part's parameters
        juce::String hint;  // the help line's text while nothing is hovered
    };

    /** `presetArea`: the preset component shown in the top bar (owned by the caller). */
    EditorFrame (UiContext& ctx, std::vector<Page> pages, juce::Component& presetArea);
    ~EditorFrame() override;

    /** Pages are added in the order of `pages` (owned by the caller), shown one at a time. */
    void addPageComponent (int pageIndex, int part, juce::Component& page);
    void showPage (int pageIndex, int part);
    int getPage() const noexcept { return page; }
    int getPart() const noexcept { return part; }
    /** The page component shown now. */
    juce::Component* currentPageComponent() const;

    std::function<void (int)> onPageChosen, onPartChosen;
    std::function<void()> onSizeMenu;

    /** A warning shown at the right of the help row while a part page is shown (empty: none). */
    void setBanner (const juce::String& text);
    juce::String getBanner() const { return banner; }

    /** Called by the editor's tick (at most 30 times per second): meters from the UI feed. */
    void updateMeters (float peakLeft, float peakRight, float limiterGainMin, double nowMs);

    /** Ends every open gesture of the frame's controls (editor closing). */
    void cancelInteraction()
    {
        master.cancelInteraction();
        popover.limiterSwitch().cancelInteraction();
        popover.ceilingSlider().cancelInteraction();
    }

    void openOutputPopover();
    void closeOutputPopover();
    bool isOutputPopoverOpen() const { return popover.isVisible(); }

    HelpLine& getHelpLine() noexcept { return helpLine; }
    HelpBubble& getHelpBubble() noexcept { return bubble; }
    OutputMeter& getMeter() noexcept { return meter; }
    Slider& getMasterSlider() noexcept { return master; }
    OutputPopover& getPopover() noexcept { return popover; }
    PartSwitch& getPartSwitch() noexcept { return partSwitch; }
    PageRail& getRail() noexcept { return rail; }
    juce::Rectangle<int> pageArea() const;

    /** Help for a component (hover or focus): the help line now, the bubble after 600 ms. */
    void hoverHelp (juce::Component* c);

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

    static constexpr int kBubbleDelayMs = 600;

private:
    class LimitChip;
    class Scrim;

    void timerCallback() override;
    void globalFocusChanged (juce::Component* focused) override;
    void mirrorChanged (int index, bool valueChanged) override;
    juce::Component* helpSourceAt (juce::Component* c) const;
    void showHint();
    bool isPartPage() const;

    UiContext& ctx;
    std::vector<Page> pages;
    juce::Component& presetArea;
    PartSwitch partSwitch;
    PageRail rail;
    OutputMeter meter;
    Slider master;
    std::unique_ptr<LimitChip> limitChip;
    juce::TextButton chevron { "v" }, sizeButton { "Size" };
    HelpLine helpLine;
    HelpBubble bubble;
    std::unique_ptr<Scrim> scrim;
    OutputPopover popover;
    struct PageEntry
    {
        int page, part; // part -1: a global page (one component for both parts)
        juce::Component* component;
    };
    std::vector<PageEntry> pageComponents;
    int page = 0, part = 0;
    juce::String banner;
    juce::Component::SafePointer<juce::Component> hovered;
    juce::String hoveredLabel;
    int limiterIndex = -1;
    int firstPageFocusOrder = 9;
};
} // namespace vs::ui
