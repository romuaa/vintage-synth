#pragma once

// A graphics context that draws nothing (docs/architecture.md 9.5, 9.6 "Paint allocations"). Test only.
//
// A real renderer allocates inside its own drawing (edge tables, glyph caches, the default
// implementations of rounded rectangles, ellipses and strokes build a Path each), so counting the
// allocations of a paint through it cannot tell our code from the renderer's. Through this context
// every drawing call is a no-op, including the ones with a default implementation that allocates
// (they are overridden), so what an AllocationCounter sees is the editor's own paint code: strings,
// paths, fonts and arrays it builds while painting.
//
//   NullGraphicsContext context;
//   juce::Graphics g (context);
//   { AllocationCounter counter; component.paint (g); ... counter.count() }
//
// `clipRegionIntersects` answers false by default, so Graphics::drawText returns before JUCE's own
// glyph arrangement (that is JUCE's cache, not our code); the clip is otherwise non-empty so that
// fillPath and strokePath reach the context. With `layoutText` it answers true: text is then laid
// out and reaches drawGlyphs (counted in `glyphRuns`), so a test can also see the allocations of
// the text layout (JUCE's per drawText call) and check that prepared text reaches the context.

#include <juce_graphics/juce_graphics.h>

namespace vs::itest
{
class NullGraphicsContext final : public juce::LowLevelGraphicsContext
{
public:
    explicit NullGraphicsContext (juce::Rectangle<int> bounds = { 0, 0, 4000, 2000 }, bool layoutText = false)
        : clip (bounds), textIsLaidOut (layoutText)
    {
    }

    bool isVectorDevice() const override { return false; }
    void setOrigin (juce::Point<int>) override {}
    void addTransform (const juce::AffineTransform&) override {}
    float getPhysicalPixelScaleFactor() const override { return 1.0f; }
    bool clipToRectangle (const juce::Rectangle<int>&) override { return true; }
    bool clipToRectangleList (const juce::RectangleList<int>&) override { return true; }
    void excludeClipRectangle (const juce::Rectangle<int>&) override {}
    void clipToPath (const juce::Path&, const juce::AffineTransform&) override {}
    void clipToImageAlpha (const juce::Image&, const juce::AffineTransform&) override {}
    bool clipRegionIntersects (const juce::Rectangle<int>&) override { return textIsLaidOut; }
    juce::Rectangle<int> getClipBounds() const override { return clip; }
    bool isClipEmpty() const override { return false; }
    void saveState() override {}
    void restoreState() override {}
    void beginTransparencyLayer (float) override {}
    void endTransparencyLayer() override {}
    void setFill (const juce::FillType&) override {}
    void setOpacity (float) override {}
    void setImageBlendMode (juce::BlendMode) override {}
    void setInterpolationQuality (juce::Graphics::ResamplingQuality) override {}
    void fillAll() override { ++calls; }
    void fillRect (const juce::Rectangle<int>&, bool) override { ++calls; }
    void fillRect (const juce::Rectangle<float>&) override { ++calls; }
    void fillRectList (const juce::RectangleList<float>&) override { ++calls; }
    void fillPath (const juce::Path&, const juce::AffineTransform&) override { ++calls; }
    void drawRect (const juce::Rectangle<float>&, float) override { ++calls; }
    void strokePath (const juce::Path&, const juce::PathStrokeType&, const juce::AffineTransform&) override { ++calls; }
    void drawImage (const juce::Image&, const juce::AffineTransform&) override { ++calls; }
    void drawLine (const juce::Line<float>&) override { ++calls; }
    void drawLineWithThickness (const juce::Line<float>&, float) override { ++calls; }
    void setFont (const juce::Font& f) override { font = f; }
    const juce::Font& getFont() override { return font; }
    void drawGlyphs (juce::Span<const uint16_t>, juce::Span<const juce::Point<float>>, const juce::AffineTransform&) override { ++calls; ++glyphRuns; }
    std::unique_ptr<juce::ImageType> getPreferredImageTypeForTemporaryImages() const override { return std::make_unique<juce::SoftwareImageType>(); }
    void drawRoundedRectangle (const juce::Rectangle<float>&, float, float) override { ++calls; }
    void fillRoundedRectangle (const juce::Rectangle<float>&, float) override { ++calls; }
    void drawEllipse (const juce::Rectangle<float>&, float) override { ++calls; }
    void fillEllipse (const juce::Rectangle<float>&) override { ++calls; }
    uint64_t getFrameId() const override { return 0; }

    /** Number of drawing calls received (a test checks that painting reached the context at all). */
    int calls = 0;
    /** Number of glyph runs (drawn text) received. */
    int glyphRuns = 0;

private:
    juce::Rectangle<int> clip;
    bool textIsLaidOut;
    juce::Font font { juce::FontOptions() };
};
} // namespace vs::itest
