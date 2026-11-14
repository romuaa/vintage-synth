#include "vintage/PluginProcessor.h"

#include "vintage/dsp/Drift.h"
#include "vintage/presets/VintagePresets.h"
#include "vintage/ui/PageList.h"

#include <cmath>

namespace vs
{
VintageSynthProcessor::VintageSynthProcessor() : Base (createParameterLayout())
{
    lowerBinding.bind (apvts, kLowerPrefix);
    upperBinding.bind (apvts, kUpperPrefix);
    vintage.bind (apvts, ParamID::vintage);
    setUiPageNames (ui::kVintagePageNames); // the session saves the editor's UI state (9.1.3)

    // Presets (after the parameters exist: the schema's defaults are their values right now).
    auto schema = makeVintageSchema (*this);
    auto factory = makeVintageFactoryBank (schema);
    initPresets (std::move (schema), std::move (factory), kPresetProductFolder);
}

void VintageSynthProcessor::readInstrumentParams (VintageParams& out) const noexcept
{
    lowerBinding.read (out.lower);
    upperBinding.read (out.upper);
    out.vintage = vintage.get();
}

void VintageSynthProcessor::applyInstrumentParams (const VintageParams& p) noexcept
{
    const float vintageAmount = p.vintage;
    lowerParams = p.lower;
    upperParams = p.upper;
    lowerPart.setParams (lowerParams);
    upperPart.setParams (upperParams);
    lowerPart.setVintage (vintageAmount);
    upperPart.setVintage (vintageAmount);
}

void VintageSynthProcessor::prepareParts (double hostRate, int maxBlock, int osFactor, int realtimeOsFactor)
{
    lowerParams = getBlockParams().lower;
    upperParams = getBlockParams().upper;
    lowerPart.setParams (lowerParams);
    upperPart.setParams (upperParams);
    lowerPart.prepare (hostRate, maxBlock, osFactor, kLowerSeed, realtimeOsFactor);
    upperPart.prepare (hostRate, maxBlock, osFactor, kUpperSeed, realtimeOsFactor);
}

double VintageSynthProcessor::getSynthTailSeconds() const noexcept
{
    // A released voice rings until ENV-2 falls below 1e-5 (-100 dB, Envelope.h): the mapped release
    // time covers 99 % (-40 dB), so that takes 2.5 release times. Worst cases that lengthen it:
    // ENV-2 key follow (times double every two octaves below C4: x 2^(60/24) at MIDI note 0) and
    // Vintage (envelope times up to +8 % per voice, Drift::kEnvTimeSpread).
    const auto mode = currentKeyMode();
    const float vintageAmount = vintage.get();
    double longest = 0.0;
    for (int p = 0; p < 2; ++p)
    {
        if (p == 1 && mode == KeyMode::Whole)
            continue; // Upper cannot sound in Whole mode
        PartParams params;
        (p == 0 ? lowerBinding : upperBinding).read (params);
        double t = static_cast<double> (map::envTimeSec (params.env2Release)) * 2.5;
        if (params.env2KeyFollow == 1)
            t *= std::exp2 (60.0 / 24.0);
        t *= 1.0 + static_cast<double> (dsp::Drift::kEnvTimeSpread) * static_cast<double> (vintageAmount);
        longest = std::max (longest, t);
    }
    return longest;
}

void VintageSynthProcessor::migrateState (juce::ValueTree& state)
{
    // Before v1.0 the version stays 1 and nothing needs migrating; from v1.0 on, each change adds a
    // step here with a test that loads a state saved by the previous version
    // (tests/fixtures/state_m5.bin is the first).
    [[maybe_unused]] const int version = state.getProperty (shell::kStateVersionAttr, 1);
    // if (version < 2) { ... }
}
} // namespace vs

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return vs::shell::createPluginProcessor<vs::VintageSynthProcessor>();
}
