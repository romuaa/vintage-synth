#pragma once

// The placeholder editor every instrument uses until it has a custom UI (Vintage: M7; B-Side: S6):
// the preset bar above JUCE's generic parameter editor, under one line that names the plugin and its version
// (docs/architecture.md 9.3.8). Message thread only.

#include "common/presets/PresetBar.h"
#include "common/ui/PreparedText.h"
#include "common/ui/frame/ProductName.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace vs::shell
{
/** One painted line of text, announced as static text: "Exitium B-Side  0.9.0" on screen,
    "Exitium B-Side, version 0.9.0" to a screen reader. No focus, no mouse. */
class NameLine final : public juce::Component
{
public:
    explicit NameLine (const ui::ProductName& name);
    void resized() override;
    void paint (juce::Graphics&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    /** The text as painted (full name, two spaces, version; just the name when there is no version). */
    const juce::String& getText() const noexcept { return text; }

private:
    juce::String text;
    juce::Font font { juce::FontOptions (15.0f, juce::Font::bold) }; // made once: paint() allocates nothing
    ui::PreparedText line;
};

class InstrumentEditor : public juce::AudioProcessorEditor
{
public:
    /** The name line reads `owner.getName()` and `version` (the build's project version, passed in by the plugin's own code). */
    InstrumentEditor (juce::AudioProcessor& owner, presets::PresetManager& presets, const juce::String& version);
    void resized() override;
    const presets::PresetBar& getBar() const noexcept { return bar; }
    /** The line above the preset bar. */
    const NameLine& getNameLine() const noexcept { return nameLine; }
    NameLine& getNameLine() noexcept { return nameLine; }
    const ui::ProductName& getProductName() const noexcept { return product; }
    static constexpr int kNameLineHeight = 24;

    static constexpr int kMinWidth = 420, kMinHeight = 300, kMaxWidth = 1200, kMaxHeight = 1600;
    static constexpr int kDefaultWidth = 560, kDefaultHeight = 680;

private:
    ui::ProductName product;
    NameLine nameLine;
    presets::PresetBar bar;
    juce::GenericAudioProcessorEditor parameters;
};

/** `new InstrumentEditor (owner, presets, version)`, for AudioProcessor::createEditor (keeps the GUI headers
    out of the processor's header). The two-argument form declared in ShellSupport.h (the skeleton's default
    createEditor) shows the name without a version. */
juce::AudioProcessorEditor* createInstrumentEditor (juce::AudioProcessor& owner, presets::PresetManager& presets,
                                                      const juce::String& version);
} // namespace vs::shell
