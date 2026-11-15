#pragma once

// Small frame components (docs/architecture.md 9.2.2): PartSwitch, PageRail, HelpLine, HelpBubble
// and OutputMeter. None of them writes a parameter: the part and the page are UI state
// (EditorState), the meter reads the UI feed.

#include "common/ui/HelpTexts.h"
#include "common/ui/PaintFonts.h"
#include "common/ui/Theme.h"

#include <functional>
#include <memory>
#include <vector>

namespace vs::ui
{
/** LOWER / UPPER: which part the part pages edit. */
class PartSwitch final : public juce::Component
{
public:
    explicit PartSwitch (const Theme& t);
    void setPart (int p);
    int getPart() const noexcept { return part; }
    std::function<void (int)> onPartChosen;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void choose (int p);
    juce::Font headerFont; // made once: paint() allocates nothing (9.5)
    const juce::String lowerText { "LOWER" }, upperText { "UPPER" };
    int part = 0;
};

enum class PageIcon
{
    Sound,
    Envelopes,
    Lfo,
    Play,
    ArpSeq,
    Effects,
    Browser
};

/** The page rail: icon + text buttons, the current page lit. */
class PageRail final : public juce::Component
{
public:
    struct Page
    {
        juce::String title;
        PageIcon icon;
    };
    PageRail (const Theme& t, std::vector<Page> pages);
    ~PageRail() override;
    void setCurrent (int index, int part);
    int getCurrent() const noexcept { return current; }
    std::function<void (int)> onPageChosen;
    void resized() override;
    void paint (juce::Graphics&) override;

    class Button;

private:
    const Theme& theme;
    std::vector<Page> pages;
    std::vector<std::unique_ptr<Button>> buttons;
    int current = 0, accentPart = 0;
};

/** Bottom row: "Label: text" of the hovered or focused control, else the page's hint. */
class HelpLine final : public juce::Component
{
public:
    explicit HelpLine (const Theme& t);
    void show (const juce::String& label, const juce::String& text);
    void showHint (const juce::String& hint);
    juce::String getText() const { return text; }
    /** A warning kept at the right of the row (empty: none), e.g. "Upper is not heard ...". */
    void setBanner (const juce::String& warning);
    juce::String getBanner() const { return banner; }
    void paint (juce::Graphics&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    PaintFonts fonts;
    const juce::String helpWord { "HELP" };
    juce::String text, banner;
};

/** The hover bubble (600 ms after the pointer rests on a control), max 320 px wide. */
class HelpBubble final : public juce::Component
{
public:
    explicit HelpBubble (const Theme& t);
    /** Shows `text` beside `target` (in the parent's coordinates), inside `limits`. */
    void showFor (const juce::String& text, juce::Rectangle<int> target, juce::Rectangle<int> limits);
    void paint (juce::Graphics&) override;
    juce::String getText() const { return text; }

private:
    const Theme& theme;
    juce::String text;
    juce::TextLayout layout;
};

/** Two peak bars, -48 ... 0 dBFS, peak hold 1 s, fall 20 dB/s, red above -0.1 dBFS. */
class OutputMeter final : public juce::Component
{
public:
    explicit OutputMeter (const Theme& t);
    /** New peaks (linear) at `nowMs`; repaints only when a bar moved by 0.5 dB or more. */
    void update (float peakLeft, float peakRight, double nowMs);
    float levelDb (int channel) const noexcept { return shownDb[channel & 1]; }
    int repaintCount() const noexcept { return repaints; }
    int announcementCount() const noexcept { return announcements; }
    std::function<void()> onClick;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override
    {
        if (onClick)
            onClick();
    }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    static constexpr float kFloorDb = -48.0f, kFallDbPerSecond = 20.0f, kHoldMs = 1000.0f, kClipDb = -0.1f;

private:
    PaintFonts fonts;
    const juce::String outText { "OUT" };
    float levelNow[2] { kFloorDb, kFloorDb }, hold[2] { kFloorDb, kFloorDb };
    double holdAt[2] { 0.0, 0.0 };
    float shownDb[2] { kFloorDb, kFloorDb }, shownHold[2] { kFloorDb, kFloorDb };
    double lastMs = 0.0;
    int repaints = 0;
    int lastAnnounced = -1000, announcements = 0;
    double lastAnnounceMs = -1.0e12;
};

/** The outline of the stroked icons (Sound, Envelopes, LFO; empty for the others), made when the size changes. */
juce::Path pageIconPath (PageIcon icon, juce::Rectangle<float> area);
void drawPageIcon (juce::Graphics& g, PageIcon icon, juce::Rectangle<float> area, juce::Colour c, const juce::Path& stroke);
} // namespace vs::ui
