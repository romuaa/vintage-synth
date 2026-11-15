#include "common/ui/frame/FrameParts.h"

#include "common/ui/controls/BoundComponent.h"

namespace vs::ui
{
// ---- PartSwitch ----

PartSwitch::PartSwitch (const Theme& t) : headerFont (t.font (metrics::kTextHeader, true))
{
    setTitle ("Edited part");
    setWantsKeyboardFocus (true);
    setComponentID ("ui.part");
}

void PartSwitch::setPart (int p)
{
    if (p != part)
    {
        part = p;
        repaint();
    }
}

void PartSwitch::choose (int p)
{
    setPart (p);
    if (onPartChosen)
        onPartChosen (p);
}

void PartSwitch::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    for (int p = 0; p < 2; ++p)
    {
        auto seg = r.withWidth (r.getWidth() * 0.5f).withX (r.getX() + r.getWidth() * 0.5f * static_cast<float> (p)).reduced (1.0f);
        const auto acc = accentFor (p);
        const bool on = p == part;
        g.setColour (on ? acc : colour (Tok::BgWindow));
        g.fillRoundedRectangle (seg, 4.0f);
        g.setColour (acc);
        g.drawRoundedRectangle (seg.reduced (0.5f), 4.0f, on ? 1.0f : 1.5f);
        g.setColour (on ? colour (Tok::TextOnAccent) : acc);
        g.setFont (headerFont);
        g.drawText (p == 0 ? lowerText : upperText, seg, juce::Justification::centred, false);
    }
    if (hasKeyboardFocus (false))
        drawFocusRing (g, r, 4.0f);
}

void PartSwitch::mouseDown (const juce::MouseEvent& e)
{
    choose (e.position.x < static_cast<float> (getWidth()) * 0.5f ? 0 : 1);
}

bool PartSwitch::keyPressed (const juce::KeyPress& k)
{
    if (k.getKeyCode() == juce::KeyPress::leftKey)
        choose (0);
    else if (k.getKeyCode() == juce::KeyPress::rightKey)
        choose (1);
    else
        return false;
    return true;
}

std::unique_ptr<juce::AccessibilityHandler> PartSwitch::createAccessibilityHandler()
{
    struct Value final : juce::AccessibilityTextValueInterface
    {
        explicit Value (PartSwitch& s) : sw (s) {}
        bool isReadOnly() const override { return false; }
        juce::String getCurrentValueAsString() const override { return sw.getPart() == 0 ? "Lower" : "Upper"; }
        void setValueAsString (const juce::String& t) override { sw.choose (t.equalsIgnoreCase ("upper") ? 1 : 0); }
        PartSwitch& sw;
    };
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::group, juce::AccessibilityActions{},
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
}

// ---- PageRail ----

class PageRail::Button final : public juce::Component
{
public:
    Button (PageRail& r, int i) : rail (r), index (i), font (r.theme.font (metrics::kText, true))
    {
        setTitle (rail.pages[static_cast<size_t> (i)].title);
        setWantsKeyboardFocus (true);
        setComponentID ("page." + rail.pages[static_cast<size_t> (i)].title.toLowerCase().removeCharacters (" +"));
        getProperties().set (helpIdProperty(), "ui.page");
        getProperties().set (helpLabelProperty(), rail.pages[static_cast<size_t> (i)].title);
    }
    void paint (juce::Graphics& g) override
    {
        const bool on = rail.current == index;
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        const auto acc = accentFor (rail.accentPart);
        g.setColour (on ? acc : (isMouseOver() ? colour (Tok::BgTrack) : colour (Tok::BgPanel)));
        g.fillRoundedRectangle (r, 6.0f);
        const auto fg = on ? colour (Tok::TextOnAccent) : colour (Tok::TextPrimary);
        drawPageIcon (g, rail.pages[static_cast<size_t> (index)].icon, iconArea(), fg, iconPath);
        g.setColour (fg);
        g.setFont (font);
        g.drawText (getTitle(), r.withTrimmedTop (r.getHeight() * 0.6f), juce::Justification::centred, true);
        if (hasKeyboardFocus (false))
            drawFocusRing (g, r, 6.0f);
    }
    void resized() override { iconPath = pageIconPath (rail.pages[static_cast<size_t> (index)].icon, iconArea()); } // made on a size change, not in paint()
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override { choose(); }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k.getKeyCode() != juce::KeyPress::returnKey && k.getKeyCode() != juce::KeyPress::spaceKey)
            return false;
        choose();
        return true;
    }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        juce::AccessibilityActions a;
        a.addAction (juce::AccessibilityActionType::press, [this] { choose(); });
        return std::make_unique<CheckableHandler> (*this, juce::AccessibilityRole::radioButton, std::move (a), [this] { return rail.current == index; });
    }

private:
    void choose()
    {
        if (rail.onPageChosen)
            rail.onPageChosen (index);
    }
    juce::Rectangle<float> iconArea() const
    {
        const auto r = getLocalBounds().toFloat().reduced (2.0f);
        return r.withTrimmedBottom (r.getHeight() * 0.38f).withSizeKeepingCentre (30.0f, 26.0f);
    }
    PageRail& rail;
    int index;
    juce::Path iconPath;
    juce::Font font; // made once: paint() allocates nothing (9.5)
};

PageRail::PageRail (const Theme& t, std::vector<Page> p) : theme (t), pages (std::move (p))
{
    setTitle ("Pages");
    for (int i = 0; i < static_cast<int> (pages.size()); ++i)
    {
        buttons.push_back (std::make_unique<Button> (*this, i));
        buttons.back()->setExplicitFocusOrder (i + 1);
        addAndMakeVisible (*buttons.back());
    }
}

PageRail::~PageRail() = default;

void PageRail::setCurrent (int index, int part)
{
    current = index;
    accentPart = part;
    for (auto& b : buttons)
    {
        b->repaint();
        if (auto* h = b->getAccessibilityHandler())
            h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
    }
}

void PageRail::resized()
{
    auto r = getLocalBounds().reduced (metrics::kGutter, metrics::kGutter);
    const int h = 82;
    for (auto& b : buttons)
    {
        b->setBounds (r.removeFromTop (h));
        r.removeFromTop (8);
    }
}

void PageRail::paint (juce::Graphics& g)
{
    g.fillAll (colour (Tok::BgWindow));
}

juce::Path pageIconPath (PageIcon icon, juce::Rectangle<float> a)
{
    juce::Path p;
    const float x = a.getX(), y = a.getY(), w = a.getWidth(), h = a.getHeight();
    switch (icon)
    {
        case PageIcon::Sound:
            p.startNewSubPath (x, y + h * 0.5f);
            p.cubicTo (x + w * 0.25f, y - h * 0.2f, x + w * 0.25f, y - h * 0.2f, x + w * 0.5f, y + h * 0.5f);
            p.cubicTo (x + w * 0.75f, y + h * 1.2f, x + w * 0.75f, y + h * 1.2f, x + w, y + h * 0.5f);
            break;
        case PageIcon::Envelopes:
            p.startNewSubPath (x, y + h);
            p.lineTo (x + w * 0.25f, y);
            p.lineTo (x + w * 0.45f, y + h * 0.45f);
            p.lineTo (x + w * 0.75f, y + h * 0.45f);
            p.lineTo (x + w, y + h);
            break;
        case PageIcon::Lfo:
            p.startNewSubPath (x, y + h * 0.5f);
            p.lineTo (x + w * 0.3f, y + h * 0.5f);
            p.lineTo (x + w * 0.45f, y);
            p.lineTo (x + w * 0.6f, y + h);
            p.lineTo (x + w * 0.75f, y + h * 0.5f);
            p.lineTo (x + w, y + h * 0.5f);
            break;
        case PageIcon::Play:
        case PageIcon::ArpSeq:
        case PageIcon::Effects:
        case PageIcon::Browser:
            break; // drawn from shapes
    }
    return p;
}

void drawPageIcon (juce::Graphics& g, PageIcon icon, juce::Rectangle<float> a, juce::Colour c, const juce::Path& stroke)
{
    g.setColour (c);
    const float x = a.getX(), y = a.getY(), w = a.getWidth(), h = a.getHeight();
    switch (icon)
    {
        case PageIcon::Play:
            for (int i = 0; i < 4; ++i)
                g.fillRoundedRectangle (x + w * 0.25f * static_cast<float> (i) + 1.0f, y, w * 0.25f - 2.0f, h, 2.0f);
            return;
        case PageIcon::ArpSeq:
            for (int i = 0; i < 4; ++i)
                g.fillEllipse (x + w * 0.25f * static_cast<float> (i), y + h * (0.75f - 0.22f * static_cast<float> (i)), 5.0f, 5.0f);
            return;
        case PageIcon::Effects:
            g.drawEllipse (a.withSizeKeepingCentre (h, h), 2.0f);
            g.drawEllipse (a.withSizeKeepingCentre (h * 0.55f, h * 0.55f), 2.0f);
            g.fillEllipse (a.withSizeKeepingCentre (h * 0.2f, h * 0.2f));
            return;
        case PageIcon::Browser:
            for (int r = 0; r < 3; ++r)
                for (int col = 0; col < 3; ++col)
                    g.fillRoundedRectangle (x + w * 0.2f + static_cast<float> (col) * w * 0.22f, y + static_cast<float> (r) * h * 0.34f,
                                            w * 0.18f, h * 0.28f, 1.5f);
            return;
        case PageIcon::Sound:
        case PageIcon::Envelopes:
        case PageIcon::Lfo:
            break;
    }
    g.strokePath (stroke, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// ---- HelpLine ----

HelpLine::HelpLine (const Theme& t) : fonts (t)
{
    setTitle ("Help");
    setInterceptsMouseClicks (false, false);
}

void HelpLine::show (const juce::String& label, const juce::String& t)
{
    showHint (label.isEmpty() ? t : label + ": " + t);
}

void HelpLine::showHint (const juce::String& hint)
{
    if (hint == text)
        return;
    text = hint;
    setDescription (text);
    repaint();
    if (auto* h = getAccessibilityHandler())
        h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
}

void HelpLine::setBanner (const juce::String& warning)
{
    if (warning == banner)
        return;
    banner = warning;
    repaint();
    if (auto* h = getAccessibilityHandler())
        h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
}

void HelpLine::paint (juce::Graphics& g)
{
    g.fillAll (colour (Tok::BgWindow));
    auto r = getLocalBounds().toFloat().reduced (static_cast<float> (metrics::kGutter), 0.0f);
    if (banner.isNotEmpty())
    {
        auto pill = r.removeFromRight (560.0f).reduced (0.0f, 3.0f);
        g.setColour (colour (Tok::Warn));
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
        g.setColour (colour (Tok::TextOnAccent));
        g.setFont (fonts.textBold);
        g.drawText (banner, pill.reduced (10.0f, 0.0f), juce::Justification::centred, true);
        r.removeFromRight (12.0f);
    }
    g.setFont (fonts.smallBold);
    g.setColour (colour (Tok::AccentGlobal));
    g.drawText (helpWord, r.removeFromLeft (40.0f), juce::Justification::centredLeft, false);
    g.setColour (colour (Tok::TextPrimary));
    g.setFont (fonts.text);
    g.drawText (text, r, juce::Justification::centredLeft, true);
}

std::unique_ptr<juce::AccessibilityHandler> HelpLine::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::staticText);
}

// ---- HelpBubble ----

HelpBubble::HelpBubble (const Theme& t) : theme (t)
{
    setInterceptsMouseClicks (false, false);
    setWantsKeyboardFocus (false);
    setAccessible (false); // the same text is the control's accessibility description
}

void HelpBubble::showFor (const juce::String& t, juce::Rectangle<int> target, juce::Rectangle<int> limits)
{
    text = t;
    juce::AttributedString s;
    s.append (text, theme.font (metrics::kText), colour (Tok::BgWindow));
    s.setWordWrap (juce::AttributedString::byWord);
    constexpr float maxWidth = 320.0f, pad = 8.0f;
    layout.createLayout (s, maxWidth - 2.0f * pad);
    const int w = juce::roundToInt (juce::jmin (maxWidth, layout.getWidth() + 2.0f * pad + 2.0f));
    const int h = juce::roundToInt (layout.getHeight() + 2.0f * pad);
    // Below the control if there is room, else above; kept inside the limits.
    juce::Rectangle<int> b (target.getX(), target.getBottom() + 6, w, h);
    if (b.getBottom() > limits.getBottom())
        b.setY (target.getY() - 6 - h);
    b = b.constrainedWithin (limits);
    setBounds (b);
    setVisible (true);
    toFront (false);
    repaint();
}

void HelpBubble::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (colour (Tok::TextPrimary));
    g.fillRoundedRectangle (r, 6.0f);
    layout.draw (g, r.reduced (8.0f));
}

// ---- OutputMeter ----

OutputMeter::OutputMeter (const Theme& t) : fonts (t)
{
    setTitle ("Output level");
    getProperties().set (helpIdProperty(), "ui.output");
    getProperties().set (helpLabelProperty(), "Output");
    setComponentID ("ui.output");
}

void OutputMeter::update (float peakLeft, float peakRight, double nowMs)
{
    const double dt = lastMs > 0.0 ? juce::jlimit (0.0, 500.0, nowMs - lastMs) : 0.0;
    lastMs = nowMs;
    const float peaks[2] { peakLeft, peakRight };
    bool moved = false;
    for (int c = 0; c < 2; ++c)
    {
        const float db = peaks[c] > 0.0f ? juce::jmax (kFloorDb, juce::Decibels::gainToDecibels (peaks[c])) : kFloorDb;
        const float fallen = levelNow[c] - kFallDbPerSecond * static_cast<float> (dt / 1000.0);
        levelNow[c] = juce::jmax (db, juce::jmax (kFloorDb, fallen));
        if (levelNow[c] >= hold[c] || nowMs - holdAt[c] > kHoldMs)
        {
            hold[c] = levelNow[c];
            holdAt[c] = nowMs;
        }
        if (std::abs (levelNow[c] - shownDb[c]) >= 0.5f || std::abs (hold[c] - shownHold[c]) >= 0.5f)
        {
            shownDb[c] = levelNow[c];
            shownHold[c] = hold[c];
            moved = true;
        }
    }
    if (moved)
    {
        ++repaints;
        repaint();
    }
    // Screen readers: at most once per second, and only when the spoken value changed.
    const int spoken = juce::roundToInt (juce::jmax (shownDb[0], shownDb[1]));
    if (spoken != lastAnnounced && nowMs - lastAnnounceMs >= 1000.0)
    {
        lastAnnounced = spoken;
        lastAnnounceMs = nowMs;
        ++announcements;
        if (auto* h = getAccessibilityHandler())
            h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
    }
}

void OutputMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setFont (fonts.small);
    g.setColour (colour (Tok::TextSecondary));
    g.drawText (outText, r.removeFromTop (12.0f), juce::Justification::centredLeft, false);
    r.removeFromTop (2.0f);
    const float barH = (r.getHeight() - 3.0f) * 0.5f;
    for (int c = 0; c < 2; ++c)
    {
        auto bar = juce::Rectangle<float> (r.getX(), r.getY() + static_cast<float> (c) * (barH + 3.0f), r.getWidth(), barH);
        g.setColour (colour (Tok::BgTrack));
        g.fillRoundedRectangle (bar, 2.0f);
        const auto toX = [&] (float db) { return bar.getX() + bar.getWidth() * (db - kFloorDb) / -kFloorDb; };
        const float x = toX (juce::jmin (0.0f, shownDb[c]));
        g.setColour (shownDb[c] > kClipDb ? colour (Tok::Warn) : colour (Tok::AccentGlobal));
        g.fillRoundedRectangle (bar.withRight (x), 2.0f);
        if (shownHold[c] > kFloorDb)
        {
            g.setColour (shownHold[c] > kClipDb ? colour (Tok::Warn) : colour (Tok::TextPrimary));
            g.fillRect (juce::Rectangle<float> (toX (juce::jmin (0.0f, shownHold[c])) - 1.0f, bar.getY(), 2.0f, bar.getHeight()));
        }
    }
}

std::unique_ptr<juce::AccessibilityHandler> OutputMeter::createAccessibilityHandler()
{
    struct Value final : juce::AccessibilityTextValueInterface
    {
        explicit Value (OutputMeter& m) : meter (m) {}
        bool isReadOnly() const override { return true; }
        juce::String getCurrentValueAsString() const override
        {
            return juce::String (juce::roundToInt (juce::jmax (meter.levelDb (0), meter.levelDb (1)))) + " dB";
        }
        void setValueAsString (const juce::String&) override {}
        OutputMeter& meter;
    };
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::progressBar, juce::AccessibilityActions{},
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
}
} // namespace vs::ui
