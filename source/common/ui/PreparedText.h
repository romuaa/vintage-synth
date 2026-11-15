#pragma once

// Text laid out once and drawn many times without allocating (docs/architecture.md 9.5: nothing of
// ours allocates in paint()). juce::Graphics::drawText and GlyphArrangement::draw build temporary
// arrays on every call; this keeps the glyph numbers and positions of a laid-out line and hands them
// straight to the graphics context. Layout (set) allocates and belongs in a value-change handler.

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace vs::ui
{
class PreparedText
{
public:
    /** Lays `text` out in `area` (at most `maxLines` lines, cut with an ellipsis, never squeezed). */
    void set (const juce::String& text, const juce::Font& f, juce::Rectangle<float> area, juce::Justification justification, int maxLines = 1)
    {
        juce::GlyphArrangement arrangement;
        arrangement.addFittedText (f, text, area.getX(), area.getY(), area.getWidth(), area.getHeight(), justification, maxLines, 1.0f);
        prepared = true;
        font = f;
        origin = {};
        glyphs.clear();
        positions.clear();
        for (int i = 0; i < arrangement.getNumGlyphs(); ++i)
        {
            const auto& pg = arrangement.getGlyph (i);
            glyphs.push_back (static_cast<std::uint16_t> (pg.getGlyphIndex()));
            positions.push_back ({ pg.getLeft(), pg.getBaselineY() });
        }
        bytes = glyphs.capacity() * sizeof (std::uint16_t) + positions.capacity() * sizeof (juce::Point<float>);
    }
    /** One line exactly as Graphics::drawText (text, area, justification, useEllipsesIfTooBig) would draw it:
        the same arrangement, drawn at the same offset, so the picture does not change. */
    void setLine (const juce::String& text, const juce::Font& f, juce::Rectangle<float> area, juce::Justification justification)
    {
        glyphs.clear();
        positions.clear();
        prepared = true;
        font = f;
        origin = area.getPosition();
        if (text.isEmpty())
            return;
        juce::GlyphArrangement arrangement;
        arrangement.addCurtailedLineOfText (f, text, 0.0f, 0.0f, area.getWidth(), true);
        arrangement.justifyGlyphs (0, arrangement.getNumGlyphs(), 0.0f, 0.0f, area.getWidth(), area.getHeight(), justification);
        for (int i = 0; i < arrangement.getNumGlyphs(); ++i)
        {
            const auto& pg = arrangement.getGlyph (i);
            glyphs.push_back (static_cast<std::uint16_t> (pg.getGlyphIndex()));
            positions.push_back ({ pg.getLeft(), pg.getBaselineY() });
        }
        bytes = glyphs.capacity() * sizeof (std::uint16_t) + positions.capacity() * sizeof (juce::Point<float>);
    }
    void clear()
    {
        glyphs.clear();
        positions.clear();
        prepared = true;
    }
    bool isEmpty() const noexcept { return glyphs.empty(); }
    /** True once set() or setLine() ran (also for text with no glyphs). */
    bool isPrepared() const noexcept { return prepared; }
    /** Heap bytes held (for the memory budget test). */
    size_t memoryBytes() const noexcept { return bytes; }

    /** In the current colour. Allocates nothing. */
    void draw (juce::Graphics& g) const
    {
        if (glyphs.empty())
            return;
        auto& context = g.getInternalContext();
        context.saveState();
        context.setFont (font);
        context.drawGlyphs (glyphs, positions, juce::AffineTransform::translation (origin.x, origin.y));
        context.restoreState();
    }

    /** Width of `text` set in one line in font f. */
    static float widthOf (const juce::String& text, const juce::Font& f)
    {
        juce::GlyphArrangement arrangement;
        arrangement.addLineOfText (f, text, 0.0f, 0.0f);
        return arrangement.getBoundingBox (0, -1, true).getWidth();
    }

private:
    juce::Font font { juce::FontOptions {} };
    juce::Point<float> origin;
    bool prepared = false;
    std::vector<std::uint16_t> glyphs;
    std::vector<juce::Point<float>> positions;
    size_t bytes = 0;
};
} // namespace vs::ui
