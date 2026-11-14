#include "common/ui/Theme.h"

namespace vs::ui
{
juce::Colour colour (Tok t) noexcept
{
    switch (t)
    {
        case Tok::BgWindow: return juce::Colour (0xff16181cu);
        case Tok::BgPanel: return juce::Colour (0xff23262cu);
        case Tok::BgTrack: return juce::Colour (0xff3a3f48u);
        case Tok::TintLower: return juce::Colour (0xff3a3024u);
        case Tok::TintUpper: return juce::Colour (0xff1f3438u);
        case Tok::TextPrimary: return juce::Colour (0xffe8eaedu);
        case Tok::TextSecondary: return juce::Colour (0xffa9afb8u);
        case Tok::TextDisabled: return juce::Colour (0xff737985u);
        case Tok::AccentLower: return juce::Colour (0xfff2a23au);
        case Tok::AccentUpper: return juce::Colour (0xff3fb8c9u);
        case Tok::AccentGlobal: return juce::Colour (0xffc9d1dbu);
        case Tok::TextOnAccent: return juce::Colour (0xff16181cu);
        case Tok::Focus: return juce::Colour (0xffffffffu);
        case Tok::Warn: return juce::Colour (0xffff6b57u);
    }
    return juce::Colour (0xffff00ffu);
}

juce::Colour accentFor (int part) noexcept
{
    return part == 0 ? colour (Tok::AccentLower) : (part == 1 ? colour (Tok::AccentUpper) : colour (Tok::AccentGlobal));
}

juce::Colour tintFor (int part) noexcept
{
    return part == 0 ? colour (Tok::TintLower) : (part == 1 ? colour (Tok::TintUpper) : colour (Tok::BgPanel));
}

Theme::Theme (juce::Typeface::Ptr regular, juce::Typeface::Ptr semibold)
    : regularFace (std::move (regular)), semiboldFace (std::move (semibold))
{
    if (semiboldFace == nullptr)
        semiboldFace = regularFace;
    setColourScheme ({ colour (Tok::BgWindow), colour (Tok::BgPanel), colour (Tok::BgPanel), colour (Tok::BgTrack),
                       colour (Tok::TextPrimary), colour (Tok::AccentGlobal), colour (Tok::TextOnAccent), colour (Tok::BgTrack),
                       colour (Tok::TextPrimary) });
    setColour (juce::PopupMenu::backgroundColourId, colour (Tok::BgPanel));
    setColour (juce::PopupMenu::textColourId, colour (Tok::TextPrimary));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, colour (Tok::BgTrack));
    setColour (juce::PopupMenu::highlightedTextColourId, colour (Tok::TextPrimary));
    setColour (juce::TextEditor::backgroundColourId, colour (Tok::BgWindow));
    setColour (juce::TextEditor::textColourId, colour (Tok::TextPrimary));
    setColour (juce::TextEditor::outlineColourId, colour (Tok::BgTrack));
    setColour (juce::TextEditor::focusedOutlineColourId, colour (Tok::Focus));
    setColour (juce::TextButton::buttonColourId, colour (Tok::BgWindow));
    setColour (juce::TextButton::textColourOffId, colour (Tok::TextPrimary));
    setColour (juce::Label::textColourId, colour (Tok::TextPrimary));
    setColour (juce::ResizableWindow::backgroundColourId, colour (Tok::BgWindow));
}

juce::Font Theme::font (float px, bool semibold) const
{
    const auto& face = semibold ? semiboldFace : regularFace;
    if (face != nullptr)
        return juce::Font (juce::FontOptions (face).withHeight (px));
    return juce::Font (juce::FontOptions (px, semibold ? juce::Font::bold : juce::Font::plain));
}

juce::Typeface::Ptr Theme::getTypefaceForFont (const juce::Font& f)
{
    // Every font JUCE draws for us (menus, text fields, labels) uses the embedded face.
    if (regularFace != nullptr && f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
        return f.isBold() ? semiboldFace : regularFace;
    return juce::LookAndFeel_V4::getTypefaceForFont (f);
}

void Theme::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (down ? colour (Tok::BgTrack) : (highlighted ? colour (Tok::BgTrack).withAlpha (0.6f) : colour (Tok::BgWindow)));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (colour (Tok::BgTrack));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

void drawFittedText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Justification j)
{
    g.drawText (text, area, j, true); // useEllipsesIfTooBig
}

void drawFocusRing (juce::Graphics& g, juce::Rectangle<float> area, float radius)
{
    g.setColour (colour (Tok::BgWindow));
    g.drawRoundedRectangle (area.expanded (2.0f), radius + 2.0f, 1.0f);
    g.setColour (colour (Tok::Focus));
    g.drawRoundedRectangle (area.expanded (0.5f), radius, 2.0f);
}
} // namespace vs::ui
