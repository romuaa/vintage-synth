#pragma once

// Generic parameter plumbing shared by every instrument: the host-facing parameter factories
// (float with display text, choice, stepped integer), the display kinds and their text conversions,
// the bound-parameter helper that reads a raw host value safely, and note-name text conversion.

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <stdexcept>

#include "common/params/Mappings.h"

namespace vs
{
// Version hint for every parameter. JUCE/VST3 uses it to keep parameter order stable.
inline constexpr int kParamVersionHint = 1;

/** A setup mistake in an instrument, found while it is being built: a parameter the shared code needs
    is missing from the layout (BoundParam::bind), or the instrument never called initPresets
    (shell::BiTimbralProcessor). A programming error that every build type reports at construction,
    by name; see shell::createPluginProcessor for what the plugin entry point does with it. */
class ConfigError : public std::logic_error
{
public:
    using std::logic_error::logic_error;
};

/** How a float parameter is shown and parsed. The kind names are used by the instruments' X-macro
    lists (e.g. vintage/params/PartParamList.h). */
enum class Display
{
    Panel10,
    CutoffHz,
    HpfHz,
    LfoRate,
    LfoDelay,
    EnvTime,
    Cents,
    PortaTime,
    DetuneCents,
    Percent,
    Pan,     // -1..1: "L 50", "C", "R 50"
    Balance, // -1..1: "Lower 50", "Centre", "Upper 50"
    // Wave 3
    DelayTime,    // 0..1 -> 1 ms .. 2 s
    Feedback,     // 0..1 -> 0 .. 95 %
    ReverbDecay,  // 0..1 -> 0.3 .. 20 s
    PreDelay,     // 0..1 -> 0 .. 200 ms
    PercentValue, // the value is a percentage already (swing, gate)
    Bpm           // the value is in bpm
};

juce::String toText (Display d, float v);
float fromText (Display d, const juce::String& t);

std::unique_ptr<juce::AudioParameterFloat> makeFloat (const juce::String& id, const juce::String& name,
                                                      float min, float max, float def, Display d);

template <size_t N>
std::unique_ptr<juce::AudioParameterChoice> makeChoice (const juce::String& id, const juce::String& name,
                                                        const std::array<const char*, N>& labels, int def)
{
    juce::StringArray choices;
    for (const auto* l : labels)
        choices.add (l);

    return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, kParamVersionHint }, name,
                                                         choices, def);
}

/** An integer parameter that hosts treat as stepped. juce::AudioParameterInt reports itself as
    continuous, so a VST3 host would offer (and store) values between the integers. */
class SteppedIntParameter final : public juce::AudioParameterInt
{
public:
    using juce::AudioParameterInt::AudioParameterInt;
    bool isDiscrete() const override { return true; }
};

/** MIDI note -> name, middle C (60) = "C4". */
juce::String noteName (int note);
/** "C4", "c#4", "Db-1", "60" -> MIDI note (clamped to 0..127); -1 if unreadable. */
int noteFromName (const juce::String& text);

/**
    One parameter's raw-value atomic plus its range and default, captured at bind time. Every read
    is sanitised (Mappings.h): a non-finite value a host may have written becomes the default and
    anything else is clamped to the range, so a bad host value never reaches the engine.
*/
struct BoundParam
{
    std::atomic<float>* value = nullptr;
    float lo = 0.0f, hi = 1.0f, def = 0.0f;

    void bind (juce::AudioProcessorValueTreeState& state, const juce::String& id);
    /** Plain value in the parameter's range (lock-free, any thread). */
    float get() const noexcept { return map::sanitiseParam (value->load (std::memory_order_relaxed), lo, hi, def); }
    /** Choice index or integer value. */
    int index() const noexcept
    {
        return map::sanitiseIndex (value->load (std::memory_order_relaxed), static_cast<int> (lo), static_cast<int> (hi),
                                   static_cast<int> (def));
    }
};

} // namespace vs
