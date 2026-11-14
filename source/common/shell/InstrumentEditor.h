#pragma once

// The placeholder editor every instrument uses until it has a custom UI (Vintage: M7; B-Side: S6):
// the preset bar above JUCE's generic parameter editor. Message thread only.

#include "common/presets/PresetBar.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace vs::shell
{
class InstrumentEditor : public juce::AudioProcessorEditor
{
public:
    InstrumentEditor (juce::AudioProcessor& owner, presets::PresetManager& presets);
    void resized() override;
    const presets::PresetBar& getBar() const noexcept { return bar; }

    static constexpr int kMinWidth = 420, kMinHeight = 300, kMaxWidth = 1200, kMaxHeight = 1600;
    static constexpr int kDefaultWidth = 560, kDefaultHeight = 680;

private:
    presets::PresetBar bar;
    juce::GenericAudioProcessorEditor parameters;
};

/** `new InstrumentEditor (owner, presets)`, for AudioProcessor::createEditor (keeps the GUI headers
    out of the processor's header). */
juce::AudioProcessorEditor* createInstrumentEditor (juce::AudioProcessor& owner, presets::PresetManager& presets);
} // namespace vs::shell
