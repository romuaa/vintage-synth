#pragma once

// A placeholder page (docs/architecture.md 9.7, M7a): until its real page is built (M7b-M7e) it
// lists its parameters with plain sliders, so every parameter is reachable from day one and the
// registry is complete. The parameter lists per page are the contract's (9.3.2-9.3.6).

#include "common/ui/controls/Slider.h"

#include <memory>
#include <vector>

namespace vs::ui
{
class PlaceholderPage final : public juce::Component
{
public:
    struct Item
    {
        juce::String paramId, label;
        int part; // accent: 0 Lower, 1 Upper, -1 global
    };

    PlaceholderPage (UiContext& ctx, const juce::String& title, const juce::String& note, std::vector<Item> items);
    int numControls() const noexcept { return static_cast<int> (sliders.size()); }
    Slider& getSlider (int i) noexcept { return *sliders[static_cast<size_t> (i)]; }
    void cancelInteraction();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    const Theme& theme;
    juce::String title, note;
    std::vector<std::unique_ptr<Slider>> sliders;
};

/** The parameter suffixes of each per-part page and the global lists (docs/architecture.md 9.3). */
std::vector<juce::String> envelopeSuffixes();
std::vector<juce::String> lfoSuffixes();
std::vector<juce::String> playPartSuffixes();
} // namespace vs::ui
