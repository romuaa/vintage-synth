#pragma once

// The custom editor of Exitium Vintage Synth (docs/architecture.md 9). Message thread only.
//
// It reads the processor through three doors only: the parameters (polled by the ParamMirror on a
// vblank-driven tick, written with host gestures), the PresetManager (preset bar), and the
// processor's EditorState and UiFeed. It registers no parameter or processor listener and never
// touches an engine object; the processor never calls it. Pages are built once (both parts' part
// pages exist; the edited part's is shown), so every binding is registered at construction.
//
// Size: a fixed logical canvas of 1360 x 784 px scaled as a whole, 75-200 % at a fixed aspect
// ratio (corner resizer and a Size menu); the size is UI state.

#include "common/presets/PresetBar.h"
#include "common/ui/ControlRegistry.h"
#include "common/ui/HelpTexts.h"
#include "common/ui/ParamMirror.h"
#include "common/ui/Theme.h"
#include "common/ui/controls/BoundComponent.h"
#include "common/ui/frame/EditorFrame.h"
#include "vintage/PluginProcessor.h"
#include "vintage/ui/PageList.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace vs::ui
{
class SoundPage;
class PlayPage;
class EffectsPage;
class PlaceholderPage;

/** The embedded Inter faces (resources/fonts/inter, SIL OFL 1.1). */
struct VintageFontSource
{
    static FontData fontData();
};

class VintageEditor final : public juce::AudioProcessorEditor, private juce::Timer, private ParamMirror::Listener
{
public:
    explicit VintageEditor (VintageSynthProcessor& processor);
    ~VintageEditor() override;

    /** One mirror pass and everything that follows from it (tests call this directly). */
    void tick (double nowMs);
    void tick() { tick (juce::Time::getMillisecondCounterHiRes()); }

    // ---- Introspection for tests ----
    const presets::PresetBar& getBar() const noexcept { return bar; }
    const ControlRegistry& getRegistry() const noexcept { return registry; }
    ParamMirror& getMirror() noexcept { return mirror; }
    EditorFrame& getFrame() noexcept { return frame; }
    SoundPage& getSoundPage (int part) noexcept { return *sound[part & 1]; }
    PlayPage& getPlayPage() noexcept { return *playPage; }
    EffectsPage& getEffectsPage() noexcept { return *effectsPage; }
    PlaceholderPage& getPlaceholder (VintagePage page, int part = 0);
    const HelpTexts& getHelpTexts() const noexcept { return help; }
    const Theme& getTheme() const noexcept { return theme; }
    /** Selects a page and the edited part as the rail and the part switch do (UI state). */
    void selectPage (int page, int part);
    float getScale() const noexcept { return scale; }
    /** Sets the size as the Size menu does (percent of 1360 x 784). */
    void setSizePercent (int percent);
    static int initialSizePercent (int saved, juce::Rectangle<int> userArea);

    static constexpr int kMenuSizes[] { 75, 90, 100, 125, 150, 175, 200 };
    static constexpr double kMinTickMs = 15.0, kVBlankTimeoutMs = 100.0, kMeterMs = 33.0;

    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void onVBlank();
    void timerCallback() override;
    void mirrorChanged (int index, bool valueChanged) override;
    void syncFromEditorState();
    void updateBanner();
    void showSizeMenu();
    void buildPages();

    shell::EditorState& editorState;
    UiFeed& feed;
    juce::SharedResourcePointer<EmbeddedFonts<VintageFontSource>> fonts;
    Theme theme;
    HelpTexts help;
    ParamMirror mirror;
    ControlRegistry registry;
    UiContext ctx;
    presets::PresetBar bar;
    EditorFrame frame;
    std::unique_ptr<SoundPage> sound[2];
    std::unique_ptr<PlayPage> playPage;
    std::unique_ptr<EffectsPage> effectsPage;
    std::vector<std::unique_ptr<PlaceholderPage>> placeholders;
    struct PlaceholderRef
    {
        VintagePage page;
        int part;
        PlaceholderPage* component;
    };
    std::vector<PlaceholderRef> placeholderRefs;

    float scale = 1.0f;
    juce::Rectangle<int> lastSetSize;
    bool constructed = false;
    int appliedSize = 0; // the EditorState size this editor has applied or stored
    int keyModeIndex = -1;
    double lastTickMs = -1.0e9, lastVBlankMs = -1.0e9, lastMeterMs = -1.0e9;
    float lastRate = 0.0f;

    juce::VBlankAttachment vblank; // last: detached first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VintageEditor)
};
} // namespace vs::ui
