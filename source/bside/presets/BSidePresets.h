#pragma once

// What is specific to Exitium B-Side in the preset system (the generic part is common/presets/):
// the file identity, the 8 x 8 bank, the extensions, the user folder, which parameters are left
// out of programs (master volume and the output limiter: session settings) and the embedded
// factory bank (presets/bside/factory/*.json). Like Vintage's, apart from the names.

#include "common/presets/FactoryBank.h"
#include "common/presets/PresetManager.h"

namespace bs
{
inline constexpr const char* kPresetProductFolder = "B-Side"; // <documents>/Exitium/B-Side/
inline constexpr const char* kPresetInstrumentId = "b-side";  // "instrument" in every preset file
inline constexpr int kPresetBankSize = 64;                    // 8 groups of 8: A1 ... H8

/** File identity, bank layout (8 x 8) and extensions (.bsprog, .bstone, .bsbank). */
vs::presets::Instrument bsideInstrument();

/** Tone prefixes lower. and upper. (empty until S2 adds the per-part parameters); excluded from
    programs: master.volume, master.limiter, master.limiter.ceiling. Build before any parameter is
    changed: the schema's defaults are the parameters' current values. */
vs::presets::Schema makeBSideSchema (const juce::AudioProcessor& processor);

/** The factory bank from the program files embedded from presets/bside/factory/ (JSON). */
vs::presets::Bank makeBSideFactoryBank (const vs::presets::Schema& schema, juce::StringArray* errors = nullptr);
} // namespace bs
