#pragma once

// The fonts a component paints with, made once (docs/architecture.md 9.5: our paint code allocates
// nothing). Theme::font builds a juce::Font on every call, which allocates; a component keeps the
// few sizes it draws with as members and passes them to Graphics::setFont (a ref-counted copy).

#include "common/ui/Theme.h"

namespace vs::ui
{
struct PaintFonts
{
    explicit PaintFonts (const Theme& t)
        : small (t.font (metrics::kTextSmall)),
          smallBold (t.font (metrics::kTextSmall, true)),
          text (t.font (metrics::kText)),
          textBold (t.font (metrics::kText, true))
    {
    }
    juce::Font small, smallBold, text, textBold;
};
} // namespace vs::ui
