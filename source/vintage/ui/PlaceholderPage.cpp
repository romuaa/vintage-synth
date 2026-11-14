#include "vintage/ui/PlaceholderPage.h"

namespace vs::ui
{
std::vector<juce::String> envelopeSuffixes()
{
    return { "env1.attack", "env1.decay", "env1.sustain", "env1.release", "env1.key_follow", "env1.polarity", "vcf.env_mod", "vcf.env_source",
             "vcomod.env_mod", "vel.vcf", "env2.attack", "env2.decay", "env2.sustain", "env2.release", "env2.key_follow", "vel.vca" };
}

std::vector<juce::String> lfoSuffixes()
{
    return { "lfo.waveform", "lfo.rate", "lfo.delay", "vcomod.lfo_mod", "vcomod.target", "vcomod.pwm", "vcomod.pwm_source", "vcf.lfo_mod", "vca.lfo_mod" };
}

std::vector<juce::String> playPartSuffixes()
{
    return { "assign.mode", "assign.unison_voices", "unison.detune", "porta.time", "porta.mode", "bend.range", "mod.wheel_vco", "mix.pan" };
}

PlaceholderPage::PlaceholderPage (UiContext& ctx, const juce::String& titleText, const juce::String& noteText, std::vector<Item> items)
    : theme (ctx.theme), title (titleText), note (noteText)
{
    setTitle (titleText);
    for (const auto& item : items)
    {
        sliders.push_back (std::make_unique<Slider> (ctx, item.paramId, item.label, item.part));
        addAndMakeVisible (*sliders.back());
    }
    setSize (metrics::kDesignWidth - metrics::kRail, metrics::kDesignHeight - metrics::kTopBar - metrics::kHelpLine);
}

void PlaceholderPage::cancelInteraction()
{
    for (auto& s : sliders)
        s->cancelInteraction();
}

void PlaceholderPage::resized()
{
    using namespace metrics;
    // Grid: as many columns as the list needs to fit (rows of 46 px), never measured from text.
    const auto area = getLocalBounds().reduced (kGutter + kPad, kGutter).withTrimmedTop (56);
    constexpr int rowH = 46;
    const int maxRows = juce::jmax (1, area.getHeight() / rowH);
    const int n = static_cast<int> (sliders.size());
    const int cols = juce::jmax (4, (n + maxRows - 1) / maxRows);
    const int colW = (area.getWidth() - (cols - 1) * 24) / cols;
    for (int i = 0; i < n; ++i)
    {
        const int col = i % cols, row = i / cols;
        sliders[static_cast<size_t> (i)]->setBounds (area.getX() + col * (colW + 24), area.getY() + row * rowH, colW, 40);
    }
}

void PlaceholderPage::paint (juce::Graphics& g)
{
    using namespace metrics;
    g.fillAll (colour (Tok::BgWindow));
    auto r = getLocalBounds().reduced (kGutter).toFloat();
    g.setColour (colour (Tok::BgPanel));
    g.fillRoundedRectangle (r, static_cast<float> (kRadius));
    auto head = r.removeFromTop (48.0f).reduced (static_cast<float> (kPad), 4.0f);
    g.setColour (colour (Tok::TextPrimary));
    g.setFont (theme.font (kTextHeader, true));
    g.drawText (title.toUpperCase(), head.removeFromTop (20.0f), juce::Justification::centredLeft, true);
    g.setColour (colour (Tok::TextSecondary));
    g.setFont (theme.font (kTextSmall));
    g.drawText (note, head, juce::Justification::centredLeft, true);
}
} // namespace vs::ui
