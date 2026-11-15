#include "vintage/ui/BrowserPage.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace vs::ui
{
namespace
{
using namespace metrics;

juce::String utf8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

constexpr int kStripX = 12, kStripY = 8, kStripW = 1224, kStripH = 68;
constexpr int kGridX = 12, kGridY = 88;
constexpr int kPanelX = 888, kPanelY = 88, kPanelW = 348, kPanelH = 578;
} // namespace

// =================================================================================================
// ProgramGrid
// =================================================================================================

ProgramGrid::ProgramGrid (const Theme& t) : theme (t)
{
    setSize (kWidth, kHeight);
    setWantsKeyboardFocus (true);
    setTitle ("Programs");
    setComponentID ("browser.grid");
}

std::unique_ptr<juce::AccessibilityHandler> ProgramGrid::createAccessibilityHandler()
{
    juce::AccessibilityActions actions;
    juce::Component::SafePointer<ProgramGrid> self (this);
    actions.addAction (juce::AccessibilityActionType::press, [self] {
        if (self != nullptr && self->onLoad)
            self->onLoad (self->selectedSlot);
    });
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::list, std::move (actions));
}

void ProgramGrid::setSelectionDescription (const juce::String& selected)
{
    const auto title = "Programs, selected " + selected;
    if (title == getTitle())
        return;
    setTitle (title);
    if (auto* h = getAccessibilityHandler())
        h->notifyAccessibilityEvent (juce::AccessibilityEvent::titleChanged);
}

juce::Rectangle<int> ProgramGrid::cellBounds (int slot) const
{
    const int row = slot / 8, col = slot % 8;
    return { kLabelW + col * (kCellW + kGap), kHeaderH + row * (kCellH + kGap), kCellW, kCellH };
}

int ProgramGrid::slotAt (juce::Point<int> p) const
{
    for (int s = 0; s < numSlots(); ++s)
        if (cellBounds (s).contains (p))
            return s;
    return -1;
}

size_t ProgramGrid::cacheBytes() const noexcept
{
    size_t n = 0;
    for (const auto& c : cells)
        n += c.name.memoryBytes() + c.position.memoryBytes();
    for (const auto& l : columnLabels)
        n += l.memoryBytes();
    for (const auto& l : rowLabels)
        n += l.memoryBytes();
    return n;
}

void ProgramGrid::repaintCell (int slot)
{
    if (slot >= 0 && slot < numSlots())
        repaint (cellRepaintArea (slot));
}

void ProgramGrid::setSlots (const std::vector<model::SlotInfo>& infos, const std::vector<std::string>& positions,
                            const std::vector<juce::Colour>& stripes)
{
    ++rebuildCount;
    const bool same = cells.size() == infos.size();
    cells.resize (infos.size());
    const auto nameFont = theme.font (kText);
    const auto posFont = theme.font (kTextSmall);
    for (size_t i = 0; i < infos.size(); ++i)
    {
        auto& c = cells[i];
        if (! same)
            c.lit = true;
        const auto r = cellBounds (static_cast<int> (i)).toFloat();
        c.name.set (utf8 (infos[i].name), nameFont, { r.getX() + 10.0f, r.getY() + 7.0f, r.getWidth() - 16.0f, 36.0f }, juce::Justification::topLeft, 2);
        c.position.set (i < positions.size() ? utf8 (positions[i]) : juce::String(), posFont,
                        { r.getX() + 10.0f, r.getBottom() - 20.0f, r.getWidth() - 16.0f, 14.0f }, juce::Justification::centredRight, 1);
        c.stripe = i < stripes.size() ? stripes[i] : colour (Tok::TextSecondary);
    }
    // Row letters and column numbers come from the position names ("A3": letter, then number).
    columnLabels.assign (8, {});
    rowLabels.assign (8, {});
    const auto labelFont = theme.font (kText, true);
    for (int k = 0; k < 8; ++k)
    {
        const auto col = static_cast<size_t> (k);
        const auto row = static_cast<size_t> (k * 8);
        const juce::String colName = col < positions.size() ? utf8 (positions[col]).substring (1) : juce::String (k + 1);
        const juce::String rowName = row < positions.size() ? utf8 (positions[row]).substring (0, 1) : juce::String();
        const auto cb = cellBounds (k);
        columnLabels[col].set (colName, labelFont, { static_cast<float> (cb.getX()), 0.0f, static_cast<float> (kCellW), static_cast<float> (kHeaderH) },
                               juce::Justification::centred);
        const auto rb = cellBounds (k * 8);
        rowLabels[col].set (rowName, labelFont, { 0.0f, static_cast<float> (rb.getY()), static_cast<float> (kLabelW), static_cast<float> (kCellH) },
                            juce::Justification::centred);
    }
    repaint(); // the whole bank changed
}

void ProgramGrid::setLit (const std::vector<bool>& lit)
{
    for (size_t i = 0; i < cells.size(); ++i)
    {
        const bool l = i < lit.size() ? lit[i] : true;
        if (cells[i].lit != l)
        {
            cells[i].lit = l;
            repaintCell (static_cast<int> (i));
        }
    }
}

void ProgramGrid::setState (int selectedSlot_, int currentSlot_, bool modified)
{
    const int oldSelected = selectedSlot, oldCurrent = currentSlot;
    const bool oldModified = modifiedDot;
    selectedSlot = selectedSlot_;
    currentSlot = currentSlot_;
    modifiedDot = modified;
    if (oldSelected != selectedSlot)
    {
        repaintCell (oldSelected);
        repaintCell (selectedSlot);
    }
    if (oldCurrent != currentSlot)
    {
        repaintCell (oldCurrent);
        repaintCell (currentSlot);
    }
    else if (oldModified != modified)
        repaintCell (currentSlot);
}

void ProgramGrid::select (int slot)
{
    slot = juce::jlimit (0, juce::jmax (0, numSlots() - 1), slot);
    if (slot != selectedSlot && onSelect)
        onSelect (slot);
}

void ProgramGrid::mouseDown (const juce::MouseEvent& e)
{
    const int s = slotAt (e.getPosition());
    if (s >= 0)
        select (s);
    grabKeyboardFocus();
}

void ProgramGrid::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int s = slotAt (e.getPosition());
    if (s < 0)
        return;
    select (s);
    if (onLoad)
        onLoad (s);
}

bool ProgramGrid::keyPressed (const juce::KeyPress& k)
{
    using M = model::Move;
    const int code = k.getKeyCode();
    std::optional<M> move;
    if (code == juce::KeyPress::leftKey) move = M::Left;
    else if (code == juce::KeyPress::rightKey) move = M::Right;
    else if (code == juce::KeyPress::upKey) move = M::Up;
    else if (code == juce::KeyPress::downKey) move = M::Down;
    else if (code == juce::KeyPress::homeKey) move = k.getModifiers().isCommandDown() ? M::First : M::RowStart;
    else if (code == juce::KeyPress::endKey) move = k.getModifiers().isCommandDown() ? M::Last : M::RowEnd;
    if (move)
    {
        select (model::moveSelection (selectedSlot, *move, 8, 8));
        return true;
    }
    if (code == juce::KeyPress::returnKey)
    {
        if (onLoad)
            onLoad (selectedSlot);
        return true;
    }
    return false;
}

void ProgramGrid::paint (juce::Graphics& g)
{
    // Nothing is allocated here: the text is prepared, the shapes are drawn by the context.
    const auto clip = g.getClipBounds();
    g.setColour (colour (Tok::TextSecondary));
    for (const auto& a : columnLabels)
        a.draw (g);
    for (const auto& a : rowLabels)
        a.draw (g);
    for (size_t i = 0; i < cells.size(); ++i)
    {
        const int slot = static_cast<int> (i);
        if (! cellRepaintArea (slot).intersects (clip))
            continue;
        const auto& c = cells[i];
        const auto r = cellBounds (slot).toFloat();
        const float alpha = c.lit ? 1.0f : 0.3f;
        const bool isSelected = slot == selectedSlot, isCurrent = slot == currentSlot;
        g.setColour ((isSelected ? colour (Tok::BgTrack) : colour (Tok::BgPanel)).withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (r, metrics::kRadiusControl);
        g.setColour (c.stripe.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (r.withWidth (5.0f), 2.0f);
        g.setColour (colour (Tok::TextPrimary).withMultipliedAlpha (alpha));
        c.name.draw (g);
        g.setColour (colour (Tok::TextSecondary).withMultipliedAlpha (alpha));
        c.position.draw (g);
        if (isCurrent)
        {
            g.setColour (colour (Tok::TextPrimary).withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (r.reduced (1.0f), metrics::kRadiusControl, 2.0f);
            if (modifiedDot)
            {
                g.setColour (colour (Tok::Warn));
                g.fillEllipse (r.getRight() - 16.0f, r.getY() + 6.0f, 8.0f, 8.0f);
            }
        }
    }
    if (hasKeyboardFocus (false) && selectedSlot >= 0 && selectedSlot < numSlots())
        drawFocusRing (g, cellBounds (selectedSlot).toFloat(), 4.0f);
}

// =================================================================================================
// ChipStrip
// =================================================================================================

ChipStrip::ChipStrip (const Theme& t) : theme (t)
{
    setWantsKeyboardFocus (true);
    setTitle ("Categories");
    setComponentID ("browser.chips");
    setSize (766, 2 * kChipH + kGap);
}

void ChipStrip::setChips (std::vector<Chip> list, int activeIndex)
{
    chips = std::move (list);
    activeChip = juce::jlimit (0, numChips(), activeIndex);
    rebuildLayout();
    focusSlot = juce::jlimit (0, juce::jmax (0, static_cast<int> (layout.size()) - 1), focusSlot);
    repaint();
}

void ChipStrip::rebuildLayout()
{
    layout.clear();
    const int width = getWidth();
    const auto font = theme.font (kText, false);
    const int total = 1 + numChips();

    // Width of each chip: its text between a minimum and a maximum; text above the maximum is cut in the middle.
    struct Measured
    {
        int w;
        juce::String shown;
    };
    std::vector<Measured> measured;
    for (int i = 0; i < total; ++i)
    {
        if (i == 0)
        {
            measured.push_back ({ kAllW, "All" });
            continue;
        }
        const auto full = chips[static_cast<size_t> (i - 1)].label;
        const float room = static_cast<float> (kMaxW - kDot - 2 * kPad);
        juce::String shown = full;
        if (PreparedText::widthOf (full, font) > room)
        {
            const auto utf = full.toStdString();
            for (size_t n = static_cast<size_t> (full.length()); n > 1; --n)
            {
                shown = utf8 (model::middleEllipsis (utf, n));
                if (PreparedText::widthOf (shown, font) <= room)
                    break;
            }
        }
        const int w = juce::jlimit (kMinW, kMaxW, static_cast<int> (std::ceil (PreparedText::widthOf (shown, font))) + kDot + 2 * kPad);
        measured.push_back ({ w, shown });
    }

    struct Cursor
    {
        int x = 0, row = 0;
    };
    const auto tryPlace = [width] (Cursor& cur, int w, juce::Rectangle<int>& out) {
        if (cur.x + w > width)
        {
            if (cur.row == 1 || w > width)
                return false;
            cur.row = 1;
            cur.x = 0;
        }
        out = { cur.x, cur.row * (kChipH + kGap), w, kChipH };
        cur.x += w + kGap;
        return true;
    };

    Cursor all;
    juce::Rectangle<int> r;
    bool everythingFits = true;
    for (int i = 0; i < total && everythingFits; ++i)
        everythingFits = tryPlace (all, measured[static_cast<size_t> (i)].w, r);

    Cursor cur;
    for (int i = 0; i < total; ++i)
    {
        Cursor probe = cur;
        if (! tryPlace (probe, measured[static_cast<size_t> (i)].w, r))
            break;
        if (! everythingFits)
        {
            Cursor after = probe;
            juce::Rectangle<int> more;
            if (! tryPlace (after, kMoreW, more)) // keep room for "More..." after this chip
                break;
        }
        cur = probe;
        layout.push_back ({ r, i, {}, measured[static_cast<size_t> (i)].shown });
    }
    if (! everythingFits && tryPlace (cur, kMoreW, r))
        layout.push_back ({ r, -1, {}, "More..." });

    for (auto& s : layout)
    {
        auto text = s.bounds.toFloat().reduced (static_cast<float> (kPad), 0.0f);
        if (s.chip > 0)
            text.removeFromLeft (static_cast<float> (kDot));
        s.text.set (s.shown, font, text, s.chip == 0 ? juce::Justification::centred : juce::Justification::centredLeft, 1);
    }
}

juce::Rectangle<int> ChipStrip::chipBounds (int index) const
{
    for (const auto& s : layout)
        if (s.chip == index)
            return s.bounds;
    return {};
}

juce::Rectangle<int> ChipStrip::moreBounds() const
{
    return chipBounds (-1);
}

juce::String ChipStrip::shownLabel (int index) const
{
    for (const auto& s : layout)
        if (s.chip == index)
            return s.shown;
    return {};
}

int ChipStrip::chipAt (juce::Point<int> p) const
{
    for (const auto& s : layout)
        if (s.bounds.contains (p))
            return s.chip;
    return -2;
}

juce::String ChipStrip::activeLabel() const
{
    return activeChip == 0 ? juce::String ("All") : chips[static_cast<size_t> (activeChip - 1)].label;
}

std::unique_ptr<juce::AccessibilityHandler> ChipStrip::createAccessibilityHandler()
{
    struct Value final : juce::AccessibilityTextValueInterface
    {
        explicit Value (ChipStrip& s) : strip (s) {}
        bool isReadOnly() const override { return false; }
        juce::String getCurrentValueAsString() const override { return strip.activeLabel(); }
        void setValueAsString (const juce::String& text) override
        {
            if (text.equalsIgnoreCase ("All"))
                strip.choose (0);
            for (int i = 0; i < strip.numChips(); ++i)
                if (strip.chips[static_cast<size_t> (i)].label.equalsIgnoreCase (text.trim()))
                    strip.choose (i + 1);
        }
        ChipStrip& strip;
    };
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::group, juce::AccessibilityActions{},
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
}

void ChipStrip::choose (int index)
{
    index = juce::jlimit (0, numChips(), index);
    if (index == activeChip)
        return;
    activeChip = index;
    if (auto* h = getAccessibilityHandler())
        h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
    repaint();
    if (onChoose)
        onChoose (index);
}

void ChipStrip::showMore()
{
    juce::PopupMenu menu;
    int lastShown = 0;
    for (const auto& s : layout)
        lastShown = juce::jmax (lastShown, s.chip);
    for (int i = lastShown + 1; i <= numChips(); ++i)
        menu.addItem (i + 1, chips[static_cast<size_t> (i - 1)].label, true, i == activeChip);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [safe = juce::Component::SafePointer<ChipStrip> (this)] (int result) {
                            if (safe != nullptr && result > 0)
                                safe->choose (result - 1);
                        });
}

void ChipStrip::setHoverLabel (const juce::String& label)
{
    getProperties().set (helpLabelProperty(), label);
}

void ChipStrip::mouseMove (const juce::MouseEvent& e)
{
    const int chip = chipAt (e.getPosition());
    if (chip > 0)
        setHoverLabel ("Category " + chips[static_cast<size_t> (chip - 1)].label); // the full name, also when the chip shows a cut one
    else if (chip == -1)
        setHoverLabel ("More categories");
    else
        setHoverLabel ("Category");
}

void ChipStrip::mouseExit (const juce::MouseEvent&)
{
    setHoverLabel ("Category");
}

void ChipStrip::mouseDown (const juce::MouseEvent& e)
{
    const int chip = chipAt (e.getPosition());
    if (chip == -2)
        return;
    for (size_t i = 0; i < layout.size(); ++i)
        if (layout[i].chip == chip)
            focusSlot = static_cast<int> (i);
    grabKeyboardFocus();
    if (chip < 0)
        showMore();
    else
        choose (chip);
}

bool ChipStrip::keyPressed (const juce::KeyPress& k)
{
    const int n = static_cast<int> (layout.size());
    if (n == 0)
        return false;
    if (k.isKeyCode (juce::KeyPress::leftKey) || k.isKeyCode (juce::KeyPress::rightKey))
    {
        focusSlot = juce::jlimit (0, n - 1, focusSlot + (k.isKeyCode (juce::KeyPress::rightKey) ? 1 : -1));
        repaint();
        return true;
    }
    if (k.isKeyCode (juce::KeyPress::returnKey) || k.isKeyCode (juce::KeyPress::spaceKey))
    {
        const int chip = layout[static_cast<size_t> (juce::jlimit (0, n - 1, focusSlot))].chip;
        if (chip < 0)
            showMore();
        else
            choose (chip);
        return true;
    }
    return false;
}

void ChipStrip::paint (juce::Graphics& g)
{
    // Nothing is allocated here. The active chip may be one of those in "More": then "More..." is lit.
    bool activeVisible = false;
    for (const auto& s : layout)
        activeVisible |= s.chip == activeChip;
    for (size_t i = 0; i < layout.size(); ++i)
    {
        const auto& s = layout[i];
        const auto r = s.bounds.toFloat();
        const bool more = s.chip < 0;
        const bool active = s.chip == activeChip || (more && ! activeVisible);
        g.setColour (active ? colour (Tok::BgTrack) : colour (Tok::BgWindow));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (active ? colour (Tok::TextPrimary) : colour (Tok::BgTrack));
        g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, 1.0f);
        if (s.chip > 0)
        {
            g.setColour (chips[static_cast<size_t> (s.chip - 1)].colour);
            g.fillEllipse (r.getX() + static_cast<float> (kPad), r.getCentreY() - 4.0f, 8.0f, 8.0f);
        }
        g.setColour (colour (Tok::TextPrimary));
        s.text.draw (g);
        if (hasKeyboardFocus (false) && static_cast<int> (i) == focusSlot)
            drawFocusRing (g, r, r.getHeight() * 0.5f);
    }
}

// =================================================================================================
// Selected program info and the tone source switch
// =================================================================================================

class BrowserPage::SelectedInfo final : public juce::Component
{
public:
    explicit SelectedInfo (const Theme& t) : nameFont (t.font (kTextLarge, true)), fonts (t)
    {
        setInterceptsMouseClicks (false, false);
        setAccessible (false);
    }
    void setProgram (const juce::String& n, const juce::String& m, const juce::String& cat, juce::Colour c)
    {
        if (n == name && m == meta && cat == category && c == dot)
            return;
        name = n;
        meta = m;
        category = cat;
        dot = c;
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (colour (Tok::TextPrimary));
        g.setFont (nameFont);
        drawFittedText (g, name, r.removeFromTop (32.0f), juce::Justification::centredLeft);
        g.setColour (colour (Tok::TextSecondary));
        g.setFont (fonts.text);
        drawFittedText (g, meta, r.removeFromTop (20.0f), juce::Justification::centredLeft);
        auto row = r.removeFromTop (22.0f);
        g.setColour (dot);
        g.fillEllipse (row.getX(), row.getCentreY() - 5.0f, 10.0f, 10.0f);
        g.setColour (colour (Tok::TextPrimary));
        drawFittedText (g, category, row.withTrimmedLeft (18.0f), juce::Justification::centredLeft);
    }
    const juce::String& shownName() const noexcept { return name; }

private:
    juce::Font nameFont; // fonts made once: paint() allocates nothing (9.5)
    PaintFonts fonts;
    juce::String name, meta, category;
    juce::Colour dot;
};

class BrowserPage::ToneSourceSwitch final : public juce::Component
{
public:
    explicit ToneSourceSwitch (const Theme& t) : fonts (t)
    {
        setWantsKeyboardFocus (true);
        setTitle ("Take its tone");
    }
    int selected() const noexcept { return index; }
    void set (int i, bool notify)
    {
        i = i == 1 ? 1 : 0;
        if (i == index)
            return;
        index = i;
        repaint();
        if (auto* h = getAccessibilityHandler())
            h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
        if (notify && onChange)
            onChange (index);
    }
    std::function<void (int)> onChange;
    void paint (juce::Graphics& g) override
    {
        const float w = static_cast<float> (getWidth()) * 0.5f;
        for (int i = 0; i < 2; ++i)
        {
            const auto r = juce::Rectangle<float> (static_cast<float> (i) * w, 0.0f, w, static_cast<float> (getHeight())).reduced (1.0f);
            const bool on = i == index;
            g.setColour (on ? accentFor (i) : colour (Tok::BgWindow));
            g.fillRoundedRectangle (r, metrics::kRadiusControl);
            g.setColour (colour (Tok::BgTrack));
            g.drawRoundedRectangle (r, metrics::kRadiusControl, 1.0f);
            g.setColour (on ? colour (Tok::TextOnAccent) : colour (Tok::TextPrimary));
            g.setFont (on ? fonts.textBold : fonts.text);
            drawFittedText (g, names[i], r, juce::Justification::centred);
        }
        if (hasKeyboardFocus (false))
            drawFocusRing (g, getLocalBounds().toFloat().reduced (1.0f), 4.0f);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        set (e.x * 2 >= getWidth() ? 1 : 0, true);
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k.isKeyCode (juce::KeyPress::leftKey)) { set (0, true); return true; }
        if (k.isKeyCode (juce::KeyPress::rightKey)) { set (1, true); return true; }
        if (k.isKeyCode (juce::KeyPress::spaceKey)) { set (1 - index, true); return true; }
        return false;
    }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        struct Value final : juce::AccessibilityTextValueInterface
        {
            explicit Value (ToneSourceSwitch& s) : sw (s) {}
            bool isReadOnly() const override { return false; }
            juce::String getCurrentValueAsString() const override { return sw.names[sw.index]; }
            void setValueAsString (const juce::String& text) override { sw.set (text.containsIgnoreCase ("upper") ? 1 : 0, true); }
            ToneSourceSwitch& sw;
        };
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::group, juce::AccessibilityActions{},
                                                             juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
    }

private:
    PaintFonts fonts;
    const juce::String names[2] { "Lower tone", "Upper tone" }; // made once: paint() allocates nothing (9.5)
    int index = 0;
};

// =================================================================================================
// BrowserPage
// =================================================================================================

juce::Colour BrowserPage::categoryColour (int index)
{
    return vs::ui::categoryColour (index);
}

BrowserPage::BrowserPage (UiContext& c, presets::PresetManager& manager, std::function<int()> part)
    : ctx (c),
      presets (manager),
      editedPart (std::move (part)),
      actions (manager),
      grid (c.theme),
      chips (c.theme),
      panel (c.theme, "Selected program", -1)
{
    setTitle ("Browser");
    setComponentID ("browser");
    keyModeIndex = presets.getSchema().indexOf ("key.mode");

    info = std::make_unique<SelectedInfo> (c.theme);
    toneSwitch = std::make_unique<ToneSourceSwitch> (c.theme);

    addAndMakeVisible (grid);
    addAndMakeVisible (chips);
    addAndMakeVisible (bank);
    addAndMakeVisible (search);
    addAndMakeVisible (panel);
    panel.addAndMakeVisible (*info);
    panel.addAndMakeVisible (*toneSwitch);
    for (auto* l : { &takeLabel, &programsLabel, &toneFilesLabel, &banksLabel, &upperNote, &readOnlyNote })
    {
        l->setInterceptsMouseClicks (false, false);
        l->setColour (juce::Label::textColourId, colour (Tok::TextSecondary));
        l->setFont (c.theme.font (kTextSmall + 1.0f));
        panel.addAndMakeVisible (*l);
    }
    takeLabel.setText ("Take only the sound of one part:", juce::dontSendNotification);
    programsLabel.setText ("Your programs", juce::dontSendNotification);
    toneFilesLabel.setText ("Tone files (the part you are editing)", juce::dontSendNotification);
    banksLabel.setText ("Banks", juce::dontSendNotification);
    readOnlyNote.setText ("Factory programs are read-only: Save as... stores a copy in your user area.", juce::dontSendNotification);
    readOnlyNote.setMinimumHorizontalScale (1.0f);
    for (auto* b : { &loadProgram, &toLower, &toUpper, &saveButton, &saveAs, &initButton, &saveTone, &loadToneFile, &importBank, &exportBank })
        panel.addAndMakeVisible (*b);

    search.setTextToShowWhenEmpty ("Search name...", colour (Tok::TextSecondary));
    search.setColour (juce::TextEditor::backgroundColourId, colour (Tok::BgWindow));
    search.setColour (juce::TextEditor::textColourId, colour (Tok::TextPrimary));
    search.setColour (juce::TextEditor::outlineColourId, colour (Tok::BgTrack));
    search.setColour (juce::TextEditor::focusedOutlineColourId, colour (Tok::Focus));
    search.setFont (c.theme.font (kText));
    search.setTitle ("Search programs");
    search.setEscapeAndReturnKeysConsumed (false);
    search.onTextChange = [this] { refreshLit(); };
    search.onEscapeKey = [this] { search.clear(); };

    // Callbacks run on the message thread while this page exists; the page owns every source of them.
    grid.onSelect = [this] (int slot) { selectSlot (slot); };
    grid.onLoad = [this] (int slot) {
        selectSlot (slot);
        loadSelected();
    };
    chips.onChoose = [this] (int) { refreshLit(); };
    toneSwitch->onChange = [this] (int tone) { setToneSource (tone); };
    bank.onClick = [this] { showBankMenu(); };
    loadProgram.onClick = [this] { loadSelected(); };
    toLower.onClick = [this] { loadToneInto (0); };
    toUpper.onClick = [this] { loadToneInto (1); };
    saveButton.onClick = [this] { actions.save (saveButton); };
    saveAs.onClick = [this] { actions.showSaveAsMenu (saveAs); };
    initButton.onClick = [this] { presets.initialise(); };
    saveTone.onClick = [this] { actions.saveToneAs (editedPart ? editedPart() : 0); };
    loadToneFile.onClick = [this] { actions.loadToneFile (editedPart ? editedPart() : 0); };
    importBank.onClick = [this] { actions.loadBankFile(); };
    exportBank.onClick = [this] { actions.saveBankAs(); };

    setHelp (grid, "ui.program_cell", "Program", c.help);
    setHelp (chips, "ui.category", "Category", c.help);
    setHelp (bank, "ui.bank", "Bank", c.help);
    setHelp (search, "ui.search", "Search", c.help);
    setHelp (loadProgram, "ui.load_program", "Load program", c.help);
    setHelp (*toneSwitch, "ui.tone_source", "Take its", c.help);
    setHelp (toLower, "ui.load_tone_lower", "Load tone > Lower", c.help);
    setHelp (toUpper, "ui.load_tone_upper", "Load tone > Upper", c.help);
    setHelp (saveButton, "ui.save", "Save", c.help);
    setHelp (saveAs, "ui.save_as", "Save as", c.help);
    setHelp (initButton, "ui.init", "Init", c.help);
    setHelp (importBank, "ui.import_bank", "Import", c.help);
    setHelp (exportBank, "ui.export_bank", "Export bank", c.help);
    setHelp (saveTone, "ui.save_tone", "Save tone", c.help);
    setHelp (loadToneFile, "ui.load_tone_file", "Load tone file", c.help);
    bank.setTitle ("Bank");

    // Keyboard order is the reading order: bank, search, chips, grid, the panel on the right (its own
    // children follow by position). The page itself sits at the frame's page order.
    bank.setExplicitFocusOrder (1);
    search.setExplicitFocusOrder (2);
    chips.setExplicitFocusOrder (3);
    grid.setExplicitFocusOrder (4);
    panel.setExplicitFocusOrder (5);

    selected = presets.isDetached() ? 0 : presets.currentIndex();
    presets.changes().addChangeListener (this);
    ctx.mirror.addIdleClient (this);
    setSize (1248, 712);
    mirrorIdle (0.0);
}

BrowserPage::~BrowserPage()
{
    presets.changes().removeChangeListener (this);
    ctx.mirror.removeIdleClient (this);
}

std::string BrowserPage::activeCategory() const
{
    const int a = chips.active();
    return a <= 0 || a > static_cast<int> (groups.size()) ? std::string() : groups[static_cast<size_t> (a - 1)].label;
}

void BrowserPage::setSearch (const juce::String& text)
{
    search.setText (text, false);
    refreshLit();
}

juce::String BrowserPage::bankText() const
{
    switch (presets.bankStatus())
    {
        case presets::BankStatus::Factory: return "Factory (read-only)";
        case presets::BankStatus::User: return presets.bankName();
        case presets::BankStatus::Pending: return presets.bankName() + " (loading)";
        case presets::BankStatus::Missing: return "bank not found: " + presets.bankName();
    }
    return {};
}

void BrowserPage::refreshBank()
{
    ++bankRefreshCount;
    // The active category survives a refresh by name; if it is gone the chips show All.
    const auto wanted = activeCategory();
    seenGeneration = presets.bankGeneration();
    infos = presets::slotInfos (presets);
    groups = model::categoryGroups (infos);
    std::vector<juce::Colour> stripes (infos.size());
    for (size_t g = 0; g < groups.size(); ++g)
        for (int s : groups[g].slots)
            stripes[static_cast<size_t> (s)] = categoryColour (static_cast<int> (g));
    grid.setSlots (infos, presets::positionNames (presets), stripes);

    int active = 0;
    std::vector<ChipStrip::Chip> list;
    for (size_t g = 0; g < groups.size(); ++g)
    {
        list.push_back ({ utf8 (groups[g].label), categoryColour (static_cast<int> (g)) });
        if (! wanted.empty() && groups[g].label == wanted)
            active = static_cast<int> (g) + 1;
    }
    chips.setChips (std::move (list), active);
    bank.setButtonText (bankText());
    refreshLit();
}

void BrowserPage::refreshLit()
{
    std::vector<bool> lit (infos.size(), true);
    const int a = chips.active();
    const auto q = search.getText().toStdString();
    for (size_t i = 0; i < infos.size(); ++i)
    {
        bool ok = model::matchesSearch (infos[i].name, infos[i].category, q);
        if (ok && a > 0 && a <= static_cast<int> (groups.size()))
        {
            const auto& slots = groups[static_cast<size_t> (a - 1)].slots;
            ok = std::find (slots.begin(), slots.end(), static_cast<int> (i)) != slots.end();
        }
        lit[i] = ok;
    }
    grid.setLit (lit);
}

void BrowserPage::refreshState()
{
    const bool detached = presets.isDetached();
    grid.setState (selected, detached ? -1 : presets.currentIndex(), presets.isModified());
    const auto meta = presets.slotMeta (selected);
    const auto group = model::groupOfSlot (groups, selected);
    const auto categoryName = meta.category.trim().isEmpty() ? juce::String (model::kUncategorised) : meta.category.trim();
    juce::String line = presets.positionName (selected);
    if (meta.author.isNotEmpty())
        line += "   by " + meta.author;
    info->setProgram (meta.name, line, categoryName, categoryColour (group < 0 ? 0 : group));
    grid.setSelectionDescription (presets.positionName (selected) + " " + meta.name);

    // The selected program's Upper tone is not heard when its key mode is Whole.
    bool heard = true;
    if (keyModeIndex >= 0)
    {
        const float v = presets.slotValue (selected, keyModeIndex);
        heard = std::isnan (v) || v >= 0.5f; // Whole is value 0; no value counts as heard
    }
    upperHeard = heard;
    upperNote.setText (heard ? juce::String() : juce::String ("Upper tone: not heard in this program (Whole mode)."), juce::dontSendNotification);
    const bool factory = presets.bankStatus() != presets::BankStatus::User;
    readOnlyNote.setVisible (factory);
}

bool BrowserPage::infosStale() const
{
    for (size_t s = 0; s < infos.size(); ++s)
    {
        const auto m = presets.slotMeta (static_cast<int> (s));
        if (m.name.toStdString() != infos[s].name || m.category.toStdString() != infos[s].category)
            return true;
    }
    return false;
}

void BrowserPage::mirrorIdle (double)
{
    // The cells are rebuilt only when the bank shown changed (its generation) or a slot's name or
    // category did (a rename of the current program does not change the generation).
    const bool changed = dirty.exchange (false, std::memory_order_relaxed);
    if (presets.bankGeneration() != seenGeneration || infos.empty() || (changed && infosStale()))
        refreshBank();
    const bool detached = presets.isDetached();
    const int current = presets.currentIndex();
    StateKey key;
    key.current = current;
    key.detached = detached;
    key.modified = presets.isModified();
    key.status = static_cast<int> (presets.bankStatus());
    const bool first = seenCurrent == -1;
    if (current != seenCurrent || key.detached != seenState.detached)
    {
        // The current program changed (another program, a host change): the selection follows it.
        if (! detached)
            selected = current;
        seenCurrent = current;
    }
    if (changed || first || ! (key == seenState))
    {
        seenState = key;
        bank.setButtonText (bankText());
        refreshState();
    }
}

void BrowserPage::selectSlot (int slot)
{
    slot = juce::jlimit (0, juce::jmax (0, presets.bankSize() - 1), slot);
    if (slot == selected)
        return;
    selected = slot;
    refreshState();
}

void BrowserPage::loadSelected()
{
    presets.selectProgram (selected);
}

void BrowserPage::setToneSource (int tone)
{
    source = tone == 1 ? 1 : 0;
    toneSwitch->set (source, false);
}

void BrowserPage::loadToneInto (int part)
{
    actions.report (presets.applyToneFromSlot (selected, source, part));
}

void BrowserPage::showBankMenu()
{
    juce::PopupMenu menu;
    const auto status = presets.bankStatus();
    menu.addItem (1, "Factory (read-only)", true, status == presets::BankStatus::Factory);
    if (status == presets::BankStatus::User)
        menu.addItem (2, presets.bankName(), false, true);
    menu.addSeparator();
    menu.addItem (3, "Load bank file...");
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (bank),
                        [safe = juce::Component::SafePointer<BrowserPage> (this)] (int result) {
                            if (safe != nullptr && result > 0)
                                safe->chooseBank (result);
                        });
}

void BrowserPage::chooseBank (int choice)
{
    if (choice == 1 && presets.bankStatus() != presets::BankStatus::Factory)
    {
        // Show the factory bank; load its first program only when nothing would be lost (a modified sound is kept).
        const bool keepSound = presets.isModified();
        presets.setBank ({}, juce::File());
        if (! keepSound)
            presets.selectProgram (0);
    }
    else if (choice == 3)
        actions.loadBankFile();
}

void BrowserPage::resized()
{
    chips.setBounds (kStripX + 12 + 190 + 12 + 220 + 12, kStripY + 6, 766, 2 * ChipStrip::kChipH + ChipStrip::kGap);
    bank.setBounds (kStripX + 12, kStripY + 18, 190, 32);
    search.setBounds (kStripX + 12 + 190 + 12, kStripY + 18, 220, 32);
    grid.setBounds (kGridX, kGridY, ProgramGrid::kWidth, ProgramGrid::kHeight);
    panel.setBounds (kPanelX, kPanelY, kPanelW, kPanelH);

    const auto b = panel.body();
    const int x = b.getX(), w = b.getWidth(), half = (w - 12) / 2;
    int y = b.getY();
    info->setBounds (x, y, w, 74);
    y += 82;
    loadProgram.setBounds (x, y, w, 40);
    y += 52;
    takeLabel.setBounds (x, y, w, 18);
    y += 20;
    toneSwitch->setBounds (x, y, w, 30);
    y += 32;
    upperNote.setBounds (x, y, w, 16);
    y += 20;
    toLower.setBounds (x, y, half, 32);
    toUpper.setBounds (x + half + 12, y, half, 32);
    y += 48;
    programsLabel.setBounds (x, y, w, 18);
    y += 20;
    saveButton.setBounds (x, y, half, 32);
    saveAs.setBounds (x + half + 12, y, half, 32);
    y += 40;
    initButton.setBounds (x, y, half, 32);
    y += 48;
    toneFilesLabel.setBounds (x, y, w, 18);
    y += 20;
    saveTone.setBounds (x, y, half, 32);
    loadToneFile.setBounds (x + half + 12, y, half, 32);
    y += 48;
    banksLabel.setBounds (x, y, w, 18);
    y += 20;
    importBank.setBounds (x, y, half, 32);
    exportBank.setBounds (x + half + 12, y, half, 32);
    y += 44;
    readOnlyNote.setBounds (x, y, w, 32);
}

void BrowserPage::paint (juce::Graphics& g)
{
    g.setColour (colour (Tok::BgPanel));
    g.fillRoundedRectangle (juce::Rectangle<float> (static_cast<float> (kStripX), static_cast<float> (kStripY), static_cast<float> (kStripW),
                                                   static_cast<float> (kStripH)),
                            static_cast<float> (kRadius));
}
} // namespace vs::ui
