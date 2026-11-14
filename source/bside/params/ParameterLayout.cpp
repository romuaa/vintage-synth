#include "bside/params/ParameterLayout.h"

#include "common/params/FxArpParams.h"
#include "common/params/GlobalParams.h"

namespace bs
{
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    vs::addMasterVolume (layout);
    vs::addKeyMixParameters (layout);
    vs::addFxParameters (layout);
    vs::addArpSeqParameters (layout);
    vs::addLimiterParameters (layout);
    // S2: the per-part parameters (lower.*, upper.*) and chip.clock are appended here (8.7).
    return layout;
}
} // namespace bs
