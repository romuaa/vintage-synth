#pragma once

// Theme tokens, metrics, fonts and the LookAndFeel of the custom editors (docs/architecture.md
// 9.2.3; dark only, U3). Message thread. No static mutable state: the embedded fonts live in an
// immutable holder shared through juce::SharedResourcePointer (one per font source).

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>

namespace vs::ui
{
enum class Tok
{
    BgWindow,
    BgPanel,
    BgTrack,
    TintLower,
    TintUpper,
    TextPrimary,
    TextSecondary,
    TextDisabled,
    AccentLower,
    AccentUpper,
    AccentGlobal,
    TextOnAccent,
    Focus,
    Warn,
    KeyWhite, // the white piano key's base (key tints mix a part accent into it)
    Marker // "changed by something other than this editor" (9.1.5); never the focus colour or shape
};

/** The token's colour (9.2.3). */
juce::Colour colour (Tok t) noexcept;
/** Accent of part 0 (Lower) / 1 (Upper); any other value: the global accent. */
juce::Colour accentFor (int part) noexcept;
/** Header / frame tint of part 0 / 1; any other value: the panel colour. */
juce::Colour tintFor (int part) noexcept;
/** Colour of the program category with this index (12 placeholder hues; the final set is an owner decision). */
juce::Colour categoryColour (int index) noexcept;

namespace metrics
{
// Logical pixels at 100 % (9.2.3, 9.2.4).
inline constexpr int kGrid = 4, kGutter = 12, kPad = 12, kRow = 32, kSegment = 28, kTrack = 6, kThumbW = 14, kThumbH = 20,
                     kRadius = 6, kHandleRadius = 10, kHeader = 28, kLabelRow = 18;
// Radii of controls (kRadius is the panel's).
inline constexpr float kRadiusControl = 4.0f, kRadiusSmall = 3.0f;
// Alphas and mixes of the pictures (the values drawn since M7a; one place for the grey scale of graphs).
inline constexpr float kInactiveAlpha = 0.4f;   // inactive accents and tracks (9.2.1)
inline constexpr float kMinorGridAlpha = 0.4f;  // minor grid lines of a picture
inline constexpr float kGridAlpha = 0.5f;       // grid lines under a picture
inline constexpr float kAxisAlpha = 0.9f;       // axes, beat lines, the zero line
inline constexpr float kCurveAlpha = 0.7f;      // a curve drawn in its accent
inline constexpr float kAreaAlpha = 0.22f;      // the area under such a curve
inline constexpr float kKeyWhiteMix = 0.6f, kKeyBlackMix = 0.62f; // how much of the part accent a white / black key takes
// Preset name button (the shared preset bar): name and chevron sizes.
inline constexpr float kTextPresetName = 16.0f, kTextChevron = 12.0f;
inline constexpr float kCategorySaturation = 0.55f, kCategoryValue = 0.85f; // placeholder category hues (owner decision pending)
inline constexpr float kTextSmall = 11.0f, kText = 13.0f, kTextHeader = 15.0f, kTextEditing = 18.0f, kTextLarge = 22.0f;
// Editor frame (9.2.4).
inline constexpr int kDesignWidth = 1360, kDesignHeight = 784, kTopBar = 48, kHelpLine = 24, kRail = 112;
} // namespace metrics

/** Embedded font files of one instrument (binary data). */
struct FontData
{
    const void* regular = nullptr;
    std::size_t regularSize = 0;
    const void* semibold = nullptr;
    std::size_t semiboldSize = 0;
};

/** Typefaces made once from a FontSource (a type with `static FontData fontData()`) and shared
    by every editor of the process (juce::SharedResourcePointer<EmbeddedFonts<Source>>). Immutable
    after construction. A null typeface means "the platform's default font" (the same tokens). */
template <class Source>
struct EmbeddedFonts
{
    EmbeddedFonts()
    {
        const FontData d = Source::fontData();
        if (d.regular != nullptr && d.regularSize > 0)
            regular = juce::Typeface::createSystemTypefaceFor (d.regular, d.regularSize);
        if (d.semibold != nullptr && d.semiboldSize > 0)
            semibold = juce::Typeface::createSystemTypefaceFor (d.semibold, d.semiboldSize);
    }
    juce::Typeface::Ptr regular, semibold;
};

/** The fonts a paint routine draws with, made once per theme: `Theme::font` builds a Font on every call, so
    paint() takes these instead (docs/architecture.md 9.5: nothing is made while painting). */
struct ThemeFonts
{
    juce::Font small, smallBold, text, textBold, badgeBold, headerBold, editingBold, largeBold;
};

/** The look of one editor: fonts and the LookAndFeel for JUCE-drawn parts (menus, text fields). */
class Theme final : public juce::LookAndFeel_V4
{
public:
    Theme (juce::Typeface::Ptr regular, juce::Typeface::Ptr semibold);

    /** Logical-pixel font; semibold for headers and lit text. */
    juce::Font font (float px, bool semibold = false) const;
    bool hasEmbeddedFont() const noexcept { return regularFace != nullptr; }
    /** The fonts for paint(), made when the theme was. */
    const ThemeFonts& fonts() const noexcept { return themeFonts; }

    // juce::LookAndFeel
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;
    juce::Font getPopupMenuFont() override { return font (metrics::kText); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return font (metrics::kText); }
    juce::Font getLabelFont (juce::Label&) override { return font (metrics::kText); }
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;

private:
    juce::Typeface::Ptr regularFace, semiboldFace;
    ThemeFonts themeFonts;
};

/** Draws text truncated with an ellipsis (layout never depends on text metrics, 9.2.4). */
void drawFittedText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Justification j);

/** The keyboard-focus ring (9.2.3): 2 px white, 1 px window-colour outside. */
void drawFocusRing (juce::Graphics& g, juce::Rectangle<float> area, float radius);
} // namespace vs::ui
