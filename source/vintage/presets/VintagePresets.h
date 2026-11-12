#pragma once

// What is specific to this instrument in the preset system (the generic part is common/presets/):
// which parameters form a tone (the lower.* and upper.* sets), which are left out of programs
// (master volume), the folder name for user files and the embedded factory bank.

#include "common/presets/FactoryBank.h"
#include "common/presets/PresetManager.h"

namespace vs
{
inline constexpr const char* kPresetProductFolder = "Vintage Synth"; // <documents>/Exitium/Vintage Synth/

/** Build before any parameter is changed: the schema's defaults are the parameters' current values. */
presets::Schema makeVintageSchema (const juce::AudioProcessor& processor);

/** The factory bank from the program files embedded from presets/factory/ (JSON). */
presets::Bank makeVintageFactoryBank (const presets::Schema& schema, juce::StringArray* errors = nullptr);
} // namespace vs
