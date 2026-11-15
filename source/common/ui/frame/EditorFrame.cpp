#include "common/ui/frame/EditorFrame.h"

namespace vs::ui
{
namespace
{
using namespace metrics;
constexpr int kPopoverW = 300, kPopoverH = 196;
} // namespace

// ---- OutputPopover ----

OutputPopover::OutputPopover (UiContext& ctx)
    : fonts (ctx.theme),
      headerFont (ctx.theme.font (kTextHeader, true)),
      limiter (ctx, "master.limiter", "Limiter", -1),
      ceiling (ctx, "master.limiter.ceiling", "Ceiling", -1)
{
    setTitle ("Output settings");
    addAndMakeVisible (limiter);
    addAndMakeVisible (ceiling);
    setWantsKeyboardFocus (true);
}

void OutputPopover::setReduction (float gainMin)
{
    const float db = gainMin < 1.0f && gainMin > 0.0f ? -juce::Decibels::gainToDecibels (gainMin) : 0.0f;
    if (std::abs (db - reduction) >= 0.25f || (db <= 0.0f) != (reduction <= 0.0f))
    {
        reduction = db;
        repaint();
    }
}

void OutputPopover::resized()
{
    auto r = getLocalBounds().reduced (kPad);
    r.removeFromTop (24);
    limiter.setBounds (r.removeFromTop (kLabelRow + kSegment));
    r.removeFromTop (10);
    ceiling.setBounds (r.removeFromTop (42));
}

void OutputPopover::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (colour (Tok::BgPanel));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (colour (Tok::BgTrack));
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    auto inner = r.reduced (static_cast<float> (kPad));
    g.setColour (colour (Tok::TextPrimary));
    g.setFont (headerFont);
    g.drawText (outputText, inner.removeFromTop (20.0f), juce::Justification::centredLeft, false);
    // Reduction meter 0 ... 12 dB at the bottom.
    auto meterArea = inner.removeFromBottom (30.0f);
    g.setFont (fonts.small);
    g.setColour (colour (Tok::TextSecondary));
    g.drawText (reductionText, meterArea.removeFromTop (14.0f), juce::Justification::centredLeft, false);
    auto bar = meterArea.reduced (0.0f, 3.0f);
    g.setColour (colour (Tok::BgTrack));
    g.fillRoundedRectangle (bar, 2.0f);
    g.setColour (colour (Tok::Warn));
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, reduction / 12.0f)), 2.0f);
}

bool OutputPopover::keyPressed (const juce::KeyPress& k)
{
    if (k.getKeyCode() == juce::KeyPress::escapeKey && onClose)
    {
        onClose();
        return true;
    }
    return false;
}

std::unique_ptr<juce::AccessibilityHandler> OutputPopover::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::dialogWindow);
}

// ---- LIMIT chip: shows the limiter state and opens the popover; writes nothing ----

class EditorFrame::LimitChip final : public juce::Component
{
public:
    LimitChip (EditorFrame& f) : frame (f)
    {
        setTitle ("Limiter (opens the output settings)");
        setWantsKeyboardFocus (true);
        getProperties().set (helpIdProperty(), "ui.output");
        getProperties().set (helpLabelProperty(), "Output");
        setComponentID ("ui.limit");
    }
    void setOn (bool shouldBeOn)
    {
        if (on != shouldBeOn)
        {
            on = shouldBeOn;
            repaint();
        }
    }
    bool isOn() const noexcept { return on; }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const auto acc = colour (Tok::AccentGlobal);
        g.setColour (colour (Tok::BgWindow));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (acc);
        g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, 1.0f);
        auto dot = r.removeFromLeft (r.getHeight()).withSizeKeepingCentre (8.0f, 8.0f);
        g.setColour (on ? colour (Tok::AccentGlobal) : colour (Tok::TextDisabled));
        g.fillEllipse (dot);
        g.setColour (on ? colour (Tok::TextPrimary) : colour (Tok::TextDisabled));
        g.setFont (frame.fonts.smallBold);
        g.drawText (on ? limitText : limitOffText, r, juce::Justification::centredLeft, false);
        if (hasKeyboardFocus (false))
            drawFocusRing (g, r, r.getHeight() * 0.5f);
    }
    void mouseDown (const juce::MouseEvent&) override { frame.openOutputPopover(); }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k.getKeyCode() != juce::KeyPress::returnKey && k.getKeyCode() != juce::KeyPress::spaceKey)
            return false;
        frame.openOutputPopover();
        return true;
    }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        juce::AccessibilityActions a;
        a.addAction (juce::AccessibilityActionType::press, [this] { frame.openOutputPopover(); });
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::button, std::move (a));
    }

private:
    EditorFrame& frame;
    const juce::String limitText { "LIMIT" }, limitOffText { "LIMIT OFF" };
    bool on = true;
};

/** Covers the frame behind the popover: a click outside closes it. */
class EditorFrame::Scrim final : public juce::Component
{
public:
    explicit Scrim (EditorFrame& f) : frame (f) { setAccessible (false); }
    void mouseDown (const juce::MouseEvent&) override { frame.closeOutputPopover(); }

private:
    EditorFrame& frame;
};

// ---- EditorFrame ----

EditorFrame::EditorFrame (UiContext& c, std::vector<Page> p, juce::Component& preset)
    : ctx (c),
      fonts (c.theme),
      editingFont (c.theme.font (kTextEditing, true)),
      pages (std::move (p)),
      presetArea (preset),
      partSwitch (c.theme),
      rail (c.theme,
            [this] {
                std::vector<PageRail::Page> list;
                for (const auto& pg : pages)
                    list.push_back ({ pg.title, pg.icon });
                return list;
            }()),
      meter (c.theme),
      master (c, "master.volume", "Master", -1),
      helpLine (c.theme),
      bubble (c.theme),
      popover (c)
{
    setTitle ("Editor");
    limitChip = std::make_unique<LimitChip> (*this);
    scrim = std::make_unique<Scrim> (*this);

    for (auto* comp : std::initializer_list<juce::Component*> { &rail, &partSwitch, &presetArea, &meter, &master, limitChip.get(), &chevron,
                                                                 &sizeButton, &helpLine })
        addAndMakeVisible (*comp);
    addChildComponent (*scrim);
    addChildComponent (popover);
    addChildComponent (bubble);

    partSwitch.onPartChosen = [this] (int chosen) {
        if (onPartChosen)
            onPartChosen (chosen);
    };
    rail.onPageChosen = [this] (int i) {
        if (onPageChosen)
            onPageChosen (i);
    };
    meter.onClick = [this] { openOutputPopover(); };
    chevron.setTitle ("Output settings");
    chevron.onClick = [this] { isOutputPopoverOpen() ? closeOutputPopover() : openOutputPopover(); };
    chevron.getProperties().set (helpIdProperty(), "ui.output");
    chevron.getProperties().set (helpLabelProperty(), "Output");
    sizeButton.setTitle ("Window size");
    sizeButton.onClick = [this] {
        if (onSizeMenu)
            onSizeMenu();
    };
    sizeButton.getProperties().set (helpIdProperty(), "ui.size");
    sizeButton.getProperties().set (helpLabelProperty(), "Size");
    partSwitch.getProperties().set (helpIdProperty(), "ui.part");
    partSwitch.getProperties().set (helpLabelProperty(), "Edited part");
    presetArea.getProperties().set (helpIdProperty(), "ui.preset");
    presetArea.getProperties().set (helpLabelProperty(), "Program");
    popover.onClose = [this] { closeOutputPopover(); };

    limiterIndex = ctx.mirror.indexOf ("master.limiter");
    if (limiterIndex >= 0)
    {
        ctx.mirror.subscribe (limiterIndex, this);
        limitChip->setOn (model::stepIndex (ctx.mirror.value (limiterIndex), 2) == 1);
    }

    // Keyboard and screen-reader order (9.2.5): rail, top bar, page content, help, popover.
    int order = 1;
    for (auto* comp : std::initializer_list<juce::Component*> { &rail, &partSwitch, &presetArea, &meter, &master, limitChip.get(), &chevron,
                                                                 &sizeButton })
        comp->setExplicitFocusOrder (order++);
    firstPageFocusOrder = order;
    helpLine.setExplicitFocusOrder (order + 1);
    popover.setExplicitFocusOrder (order + 2);

    addMouseListener (this, true); // hover help for every child
    juce::Desktop::getInstance().addFocusChangeListener (this);
    setSize (kDesignWidth, kDesignHeight); // last: resized() places every child
}

EditorFrame::~EditorFrame()
{
    stopTimer();
    juce::Desktop::getInstance().removeFocusChangeListener (this);
    removeMouseListener (this);
    ctx.mirror.unsubscribe (this);
}

juce::Rectangle<int> EditorFrame::pageArea() const
{
    return { kRail, kTopBar, kDesignWidth - kRail, kDesignHeight - kTopBar - kHelpLine };
}

void EditorFrame::resized()
{
    // Fixed logical layout (9.2.4): the editor scales this whole component.
    rail.setBounds (0, kTopBar, kRail, kDesignHeight - kTopBar - kHelpLine);
    helpLine.setBounds (0, kDesignHeight - kHelpLine, kDesignWidth, kHelpLine);
    partSwitch.setBounds (kGutter, 8, 200, 32);
    presetArea.setBounds (396, 8, 510, 32);
    meter.setBounds (918, 6, 96, 36);
    master.setBounds (1024, 6, 150, 36);
    limitChip->setBounds (1184, 10, 70, 28);
    chevron.setBounds (1258, 10, 26, 28);
    sizeButton.setBounds (1290, 10, 58, 28);
    scrim->setBounds (getLocalBounds());
    popover.setBounds (kDesignWidth - kGutter - kPopoverW, kTopBar + 4, kPopoverW, kPopoverH);
    for (auto& e : pageComponents)
        e.component->setBounds (pageArea());
}

bool EditorFrame::isPartPage() const
{
    return page >= 0 && static_cast<size_t> (page) < pages.size() && pages[static_cast<size_t> (page)].partPage;
}

void EditorFrame::addPageComponent (int pageIndex, int partIndex, juce::Component& comp)
{
    pageComponents.push_back ({ pageIndex, partIndex, &comp });
    comp.setExplicitFocusOrder (firstPageFocusOrder);
    addChildComponent (comp);
    comp.setBounds (pageArea());
    // Overlays stay above the pages.
    scrim->toFront (false);
    popover.toFront (false);
    bubble.toFront (false);
}

juce::Component* EditorFrame::currentPageComponent() const
{
    const bool partPage = isPartPage();
    for (const auto& e : pageComponents)
        if (e.page == page && (! partPage || e.part == part))
            return e.component;
    return nullptr;
}

void EditorFrame::showPage (int pageIndex, int partIndex)
{
    page = juce::jlimit (0, static_cast<int> (pages.size()) - 1, pageIndex);
    part = partIndex == 1 ? 1 : 0;
    auto* shown = currentPageComponent();
    for (auto& e : pageComponents)
    {
        // A page that goes away ends its open gestures first (keyboard page switch mid-drag,
        // a session restore changing the page).
        if (e.component != shown && e.component->isVisible())
            if (auto* content = dynamic_cast<InteractiveContent*> (e.component))
                content->cancelInteraction();
        e.component->setVisible (e.component == shown);
    }
    partSwitch.setPart (part);
    partSwitch.setVisible (isPartPage());
    rail.setCurrent (page, isPartPage() ? part : -1);
    helpLine.setBanner (isPartPage() ? banner : juce::String());
    bubble.setVisible (false);
    showHint();
    if (auto* h = getAccessibilityHandler())
        h->notifyAccessibilityEvent (juce::AccessibilityEvent::structureChanged);
    repaint();
}

void EditorFrame::setBanner (const juce::String& text)
{
    banner = text;
    helpLine.setBanner (isPartPage() ? banner : juce::String());
}

void EditorFrame::updateMeters (float peakLeft, float peakRight, float limiterGainMin, double nowMs)
{
    meter.update (peakLeft, peakRight, nowMs);
    popover.setReduction (limiterGainMin);
}

void EditorFrame::openOutputPopover()
{
    scrim->setVisible (true);
    popover.setVisible (true);
    scrim->toFront (false);
    popover.toFront (true);
}

void EditorFrame::closeOutputPopover()
{
    popover.setVisible (false);
    scrim->setVisible (false);
}

void EditorFrame::mirrorChanged (int index, bool)
{
    if (index == limiterIndex)
        limitChip->setOn (model::stepIndex (ctx.mirror.value (limiterIndex), 2) == 1);
}

void EditorFrame::paint (juce::Graphics& g)
{
    g.fillAll (colour (Tok::BgWindow));
    const bool partPage = isPartPage();
    const int tintPart = partPage ? part : -1;

    // Top bar in the part tint on part pages.
    g.setColour (partPage ? tintFor (tintPart) : colour (Tok::BgPanel));
    g.fillRect (0, 0, kDesignWidth, kTopBar);

    auto text = juce::Rectangle<float> (224.0f, 8.0f, 164.0f, 32.0f);
    if (partPage)
    {
        g.setColour (accentFor (part));
        g.setFont (editingFont);
        g.drawText (part == 0 ? editingLower : editingUpper, text, juce::Justification::centredLeft, true);
    }
    else
    {
        g.setColour (colour (Tok::TextPrimary));
        g.setFont (fonts.textBold);
        g.drawText (showingWhole, juce::Rectangle<float> (kGutter, 8.0f, 200.0f, 32.0f), juce::Justification::centredLeft, true);
        for (int p = 0; p < 2; ++p)
        {
            auto chip = juce::Rectangle<float> (224.0f + 74.0f * static_cast<float> (p), 14.0f, 68.0f, 20.0f);
            g.setColour (accentFor (p));
            g.fillRoundedRectangle (chip, 10.0f);
            g.setColour (colour (Tok::TextOnAccent));
            g.setFont (fonts.smallBold);
            g.drawText (p == 0 ? lowerText : upperText, chip, juce::Justification::centred, false);
        }
    }
}

void EditorFrame::paintOverChildren (juce::Graphics& g)
{
    // Frame line in the edited part's colour (part pages), and the banner.
    const bool partPage = isPartPage();
    g.setColour (partPage ? accentFor (part) : colour (Tok::BgTrack));
    g.fillRect (0, kTopBar - 2, kDesignWidth, 2);
    g.drawRect (getLocalBounds().withTrimmedBottom (kHelpLine), partPage ? 2 : 1);
}

// ---- Help: hover and focus ----

juce::Component* EditorFrame::helpSourceAt (juce::Component* c) const
{
    for (auto* p = c; p != nullptr && p != this; p = p->getParentComponent())
        if (p->getProperties().contains (helpIdProperty()))
            return p;
    return nullptr;
}

void EditorFrame::hoverHelp (juce::Component* c)
{
    auto* source = helpSourceAt (c);
    // A control may change its label while it stays the hovered one (the category chips name the chip under the pointer).
    const auto label = source != nullptr ? source->getProperties()[helpLabelProperty()].toString() : juce::String();
    if (source == hovered.getComponent() && label == hoveredLabel)
        return;
    hovered = source;
    hoveredLabel = label;
    bubble.setVisible (false);
    stopTimer();
    if (source == nullptr)
    {
        showHint();
        return;
    }
    const auto id = source->getProperties()[helpIdProperty()].toString();
    const auto* text = ctx.help.find (id.toStdString());
    helpLine.show (label, text != nullptr ? juce::String::fromUTF8 (text->c_str()) : juce::String());
    startTimer (kBubbleDelayMs);
}

void EditorFrame::showHint()
{
    if (page >= 0 && static_cast<size_t> (page) < pages.size())
        helpLine.showHint (pages[static_cast<size_t> (page)].hint);
}

void EditorFrame::timerCallback()
{
    stopTimer();
    auto* source = hovered.getComponent();
    if (source == nullptr || ! source->isShowing() || juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        return;
    const auto id = source->getProperties()[helpIdProperty()].toString();
    if (const auto* text = ctx.help.find (id.toStdString()))
        bubble.showFor (juce::String::fromUTF8 (text->c_str()), getLocalArea (source, source->getLocalBounds()), getLocalBounds().reduced (4));
}

void EditorFrame::mouseMove (const juce::MouseEvent& e)
{
    hoverHelp (e.eventComponent);
}

void EditorFrame::mouseEnter (const juce::MouseEvent& e)
{
    hoverHelp (e.eventComponent);
}

void EditorFrame::mouseExit (const juce::MouseEvent& e)
{
    if (e.eventComponent == this && ! getLocalBounds().contains (e.getEventRelativeTo (this).getPosition()))
        hoverHelp (nullptr);
}

void EditorFrame::mouseDown (const juce::MouseEvent&)
{
    // Hidden while dragging.
    stopTimer();
    bubble.setVisible (false);
}

void EditorFrame::globalFocusChanged (juce::Component* focused)
{
    if (focused != nullptr && isParentOf (focused))
        hoverHelp (focused);
}
} // namespace vs::ui
