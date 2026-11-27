#include "common/shell/InstrumentEditor.h"

#include "common/shell/ShellSupport.h"

namespace vs::shell
{
NameLine::NameLine (const ui::ProductName& name)
    : text (name.version.isEmpty() ? name.fullName() : name.fullName() + "  " + name.version)
{
    setTitle (name.announcement());
    setComponentID ("ui.name");
    setInterceptsMouseClicks (false, false);
    setWantsKeyboardFocus (false);
}

void NameLine::resized()
{
    line.setLine (text, font, getLocalBounds().toFloat().reduced (8.0f, 0.0f), juce::Justification::centredLeft);
}

void NameLine::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
    g.setColour (getLookAndFeel().findColour (juce::Label::textColourId));
    line.draw (g);
}

std::unique_ptr<juce::AccessibilityHandler> NameLine::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::staticText);
}

InstrumentEditor::InstrumentEditor (juce::AudioProcessor& owner, presets::PresetManager& presets, const juce::String& version)
    : juce::AudioProcessorEditor (owner),
      product (ui::ProductName::make (owner.getName(), version)),
      nameLine (product),
      bar (presets),
      parameters (owner)
{
    addAndMakeVisible (nameLine);
    addAndMakeVisible (bar);
    addAndMakeVisible (parameters);
    setResizable (true, true);
    setResizeLimits (kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
    setSize (kDefaultWidth, kDefaultHeight);
}

void InstrumentEditor::resized()
{
    auto r = getLocalBounds();
    nameLine.setBounds (r.removeFromTop (kNameLineHeight));
    bar.setBounds (r.removeFromTop (presets::PresetBar::kHeight));
    parameters.setBounds (r);
}

juce::AudioProcessorEditor* createInstrumentEditor (juce::AudioProcessor& owner, presets::PresetManager& presets,
                                                      const juce::String& version)
{
    return new InstrumentEditor (owner, presets, version);
}

juce::AudioProcessorEditor* createInstrumentEditor (juce::AudioProcessor& owner, presets::PresetManager& presets)
{
    return new InstrumentEditor (owner, presets, {});
}
} // namespace vs::shell
