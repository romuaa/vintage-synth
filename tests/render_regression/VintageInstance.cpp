// The instrument the render-regression executable renders by default: Vintage.
#include "RenderHarness.h"

#include "vintage/PluginProcessor.h"

namespace vs::rr
{
Instance makeDefaultInstance()
{
    auto processor = std::make_unique<VintageSynthProcessor>();
    auto* presetManager = &processor->getPresets();
    return { std::move (processor), presetManager };
}
} // namespace vs::rr
