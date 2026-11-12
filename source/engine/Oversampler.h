#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

namespace vs
{
/**
    Oversampling factor of the voice path for a host rate (docs/architecture.md 4.4):
    below 88.2 kHz 2x in real time and 4x offline; at 88.2 kHz and above 1x and 2x.
    The internal rate is then at least 88.2 kHz at every host rate.
*/
inline int voiceOversamplingFactor (double hostRate, bool offline) noexcept
{
    const bool high = hostRate >= 88200.0 - 0.5; // tolerate a host reporting 88199.99...
    if (offline)
        return high ? 2 : 4;
    return high ? 1 : 2;
}

/**
    Mono decimator for the oversampled voice path (polyphase IIR half-band).
    It has very low latency, and the stop-band is tuned for oscillator/filter
    content, which is far stronger than anything a 2x upsampler needs to reject.
    @param factor 1, 2 or 4
*/
inline std::unique_ptr<juce::dsp::Oversampling<float>> makeVoiceOversampler (int factor)
{
    auto os = std::make_unique<juce::dsp::Oversampling<float>> (1);
    using FT = juce::dsp::Oversampling<float>::FilterType;

    if (factor <= 1)
    {
        os->addDummyOversamplingStage();
        return os;
    }

    // Outer stage (closest to the base rate) carries the tight transition band.
    os->addOversamplingStage (FT::filterHalfBandPolyphaseIIR, 0.06f, -60.0f, 0.05f, -100.0f);
    if (factor >= 4)
        os->addOversamplingStage (FT::filterHalfBandPolyphaseIIR, 0.12f, -60.0f, 0.10f, -90.0f);

    return os;
}
} // namespace vs
