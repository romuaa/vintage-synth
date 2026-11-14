#pragma once

// A titled panel (docs/architecture.md 9.2.3): header in the part tint (caps, 15 px), body in the
// panel colour, 6 px corners. Its children are placed by the page.

#include "common/ui/Theme.h"

namespace vs::ui
{
class Panel final : public juce::Component
{
public:
    Panel (const Theme& t, const juce::String& titleText, int partIndex) : theme (t), part (partIndex)
    {
        setTitle (titleText);
        setText (titleText);
        setInterceptsMouseClicks (false, true);
    }
    /** Changes the header text; only the header strip repaints (the body's controls are untouched). */
    void setText (const juce::String& t)
    {
        const auto upper = t.toUpperCase();
        if (upper == header)
            return;
        header = upper;
        setTitle (t); // the accessibility name follows the header
        repaint (getLocalBounds().removeFromTop (metrics::kHeader));
    }
    /** The header text as drawn (upper case). */
    const juce::String& getHeaderText() const noexcept { return header; }
    /** Body area below the header, in this panel's coordinates. */
    juce::Rectangle<int> body() const { return getLocalBounds().withTrimmedTop (metrics::kHeader).reduced (metrics::kPad, 8); }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour (colour (Tok::BgPanel));
        g.fillRoundedRectangle (r, static_cast<float> (metrics::kRadius));
        juce::Path head;
        head.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), static_cast<float> (metrics::kHeader), static_cast<float> (metrics::kRadius),
                                  static_cast<float> (metrics::kRadius), true, true, false, false);
        g.setColour (tintFor (part));
        g.fillPath (head);
        g.setColour (part >= 0 ? accentFor (part) : colour (Tok::TextPrimary));
        g.setFont (theme.font (metrics::kTextHeader, true).withExtraKerningFactor (0.04f));
        g.drawText (header, r.withHeight (static_cast<float> (metrics::kHeader)).reduced (static_cast<float> (metrics::kPad), 0.0f),
                    juce::Justification::centredLeft, true);
    }

private:
    const Theme& theme;
    int part;
    juce::String header;
};
} // namespace vs::ui
