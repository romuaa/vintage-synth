// The instrument the bside_render executable renders: Exitium B-Side, through the real processor and its real preset
// manager (the counterpart of tests/render_regression/VintageInstance.cpp). Includes nothing of Vintage.
#include "RenderHarness.h"

#include "bside/PluginProcessor.h"

namespace vs::rr
{
Instance makeDefaultInstance()
{
    auto processor = std::make_unique<bs::BSideProcessor>();
    auto* presetManager = &processor->getPresets();
    return { std::move (processor), presetManager };
}
} // namespace vs::rr
