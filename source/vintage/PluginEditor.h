#pragma once

#include "common/presets/PresetBar.h"
#include "vintage/PluginProcessor.h"

namespace vs
{
/** Placeholder editor until the custom UI (M7): the preset bar above JUCE's generic parameter editor. */
class VintageSynthEditor final : public juce::AudioProcessorEditor
{
public:
    explicit VintageSynthEditor (VintageSynthProcessor& owner);
    void resized() override;
    const presets::PresetBar& getBar() const noexcept { return bar; }

private:
    presets::PresetBar bar;
    juce::GenericAudioProcessorEditor parameters;
};
} // namespace vs
