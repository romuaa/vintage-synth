#include "vintage/PluginEditor.h"

namespace vs
{
VintageSynthEditor::VintageSynthEditor (VintageSynthProcessor& owner)
    : juce::AudioProcessorEditor (owner), bar (owner.getPresets()), parameters (owner)
{
    addAndMakeVisible (bar);
    addAndMakeVisible (parameters);
    setResizable (true, true);
    setResizeLimits (420, 300, 1200, 1600);
    setSize (560, 680);
}

void VintageSynthEditor::resized()
{
    auto r = getLocalBounds();
    bar.setBounds (r.removeFromTop (presets::PresetBar::kHeight));
    parameters.setBounds (r);
}
} // namespace vs
