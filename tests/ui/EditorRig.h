#pragma once

// Shared helpers for the editor tests (docs/architecture.md 9.6): a processor with an editor
// that is not on screen, driven by calling tick() directly.

#include "../integration/IntegrationRig.h"
#include "../seq_unit/AllocationCounter.h" // VS_SEQ_TEST_ASAN

#include "vintage/PluginEditor.h"
#include "vintage/ui/SoundPage.h"

#include <map>
#include <memory>

namespace vs::itest
{
/** Constructing JUCE components makes macOS's font and graphics libraries parse system files, where
    AddressSanitizer reports inside Apple's CoreGraphics (not our code): the editor tests are
    skipped only under macOS ASan (9.6); they run under Linux ASan/UBSan (xvfb). */
#if defined(VS_SEQ_TEST_ASAN) && JUCE_MAC
 #define VS_SKIP_EDITOR_UNDER_MAC_ASAN() SKIP ("not run under macOS AddressSanitizer: system graphics library reports")
#else
 #define VS_SKIP_EDITOR_UNDER_MAC_ASAN() ((void) 0)
#endif

struct EditorRig : Rig
{
    std::unique_ptr<ui::VintageEditor> editor;
    double clockMs = 1000.0;

    explicit EditorRig (double rate = 48000.0, int maxBlockSize = 512) : Rig (rate, maxBlockSize)
    {
        prepare();
        open();
    }

    void open()
    {
        editor.reset (dynamic_cast<ui::VintageEditor*> (proc.createEditor()));
        REQUIRE (editor != nullptr);
    }

    /** One editor tick, 20 ms after the last (a vblank-driven tick). */
    void tick (double stepMs = 20.0)
    {
        clockMs += stepMs;
        editor->tick (clockMs);
    }

    juce::RangedAudioParameter& param (const juce::String& id)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
        REQUIRE (p != nullptr);
        return *p;
    }
};

/** Counts the host gestures the processor reports (what a host records automation between). */
struct GestureCounter final : juce::AudioProcessorListener
{
    explicit GestureCounter (juce::AudioProcessor& p) : proc (p) { proc.addListener (this); }
    ~GestureCounter() override { proc.removeListener (this); }
    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float) override
    {
        ++values;
        if (open[index] <= 0)
            ++valuesOutsideGesture;
    }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override
    {
        ++begins;
        if (open[index]++ > 0)
            ++nestedBegins;
    }
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index) override
    {
        ++ends;
        --open[index];
    }
    int openCount() const
    {
        int n = 0;
        for (const auto& [i, c] : open)
            n += c;
        return n;
    }

    juce::AudioProcessor& proc;
    std::map<int, int> open;
    int begins = 0, ends = 0, values = 0, valuesOutsideGesture = 0, nestedBegins = 0;
};
} // namespace vs::itest
