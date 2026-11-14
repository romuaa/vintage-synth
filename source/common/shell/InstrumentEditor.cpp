#include "common/shell/InstrumentEditor.h"

namespace vs::shell
{
InstrumentEditor::InstrumentEditor (juce::AudioProcessor& owner, presets::PresetManager& presets)
    : juce::AudioProcessorEditor (owner), bar (presets), parameters (owner)
{
    addAndMakeVisible (bar);
    addAndMakeVisible (parameters);
    setResizable (true, true);
    setResizeLimits (kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
    setSize (kDefaultWidth, kDefaultHeight);
}

void InstrumentEditor::resized()
{
    auto r = getLocalBounds();
    bar.setBounds (r.removeFromTop (presets::PresetBar::kHeight));
    parameters.setBounds (r);
}

juce::AudioProcessorEditor* createInstrumentEditor (juce::AudioProcessor& owner, presets::PresetManager& presets)
{
    return new InstrumentEditor (owner, presets);
}
} // namespace vs::shell
