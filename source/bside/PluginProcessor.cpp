#include "bside/PluginProcessor.h"

#include "bside/presets/BSidePresets.h"

namespace bs
{
BSideProcessor::BSideProcessor() : Base (createParameterLayout())
{
    // Presets (after the parameters exist: the schema's defaults are their values right now).
    auto schema = makeBSideSchema (*this);
    auto factory = makeBSideFactoryBank (schema);
    initPresets (std::move (schema), std::move (factory), kPresetProductFolder);
}

void BSideProcessor::prepareParts (double hostRate, int /*maxBlock*/, int /*osFactor*/, int /*realtimeOsFactor*/)
{
    // Test tones run at the host rate: no oversampling, no latency.
    for (auto& p : parts)
        p.prepare (hostRate);
}

void BSideProcessor::migrateState (juce::ValueTree& state)
{
    // Before v1.0 the version stays 1 and nothing needs migrating; from v1.0 on, each change adds a
    // step here with a test that loads a state saved by the previous version.
    [[maybe_unused]] const int version = state.getProperty (vs::shell::kStateVersionAttr, 1);
    // if (version < 2) { ... }
}
} // namespace bs

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return vs::shell::createPluginProcessor<bs::BSideProcessor>();
}
