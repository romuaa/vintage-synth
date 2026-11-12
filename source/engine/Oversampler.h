#pragma once

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <memory>
#include <vector>

namespace vs
{
/**
    Oversampling factor of the voice path for a host rate (docs/architecture.md 4.4), so that the
    internal rate is at least 88.2 kHz wherever the path allows it:
    - real time: 1x from 88.2 kHz, 2x from 44.1 kHz, 4x below 44.1 kHz (32 kHz -> 128 kHz,
      22.05 kHz -> 88.2 kHz);
    - offline: 2x from 88.2 kHz, 4x below (4x is the largest factor the decimator has, so a host
      below 22.05 kHz stays under 88.2 kHz internally; offline and real time are then the same).
*/
inline int voiceOversamplingFactor (double hostRate, bool offline) noexcept
{
    constexpr double kMinInternal = 88200.0 - 0.5; // tolerate a host reporting 88199.99... or 44099.99...
    if (hostRate >= kMinInternal)
        return offline ? 2 : 1;
    if (hostRate * 2.0 >= kMinInternal)
        return offline ? 4 : 2;
    return 4;
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
/**
    Delay of the voice decimator alone, in host samples, at low frequencies (where a note's
    fundamental lives). Measured, not taken from juce::dsp::Oversampling::getLatencyInSamples():
    that is the latency of a full up + down round trip (the upsampling filter's delay included),
    but the voice path only decimates (the upsampler is fed silence), so it over-reported by
    2-3 samples. Method: a sine at fs / 512 is decimated and its phase is compared with the
    input's (coherent projection over whole periods); phase delay = group delay to well under
    0.01 sample at this frequency for these half-band filters. Allocates: prepare time only.
*/
inline double measureDecimatorDelay (int factor)
{
    if (factor <= 1)
        return 0.0;
    constexpr int period = 512, block = 512, warmup = 16 * period, measure = 64 * period;
    auto os = makeVoiceOversampler (factor);
    os->initProcessing (block);
    juce::AudioBuffer<float> silence (1, block), out (1, block);
    silence.clear();
    const double w = 2.0 * juce::MathConstants<double>::pi / static_cast<double> (period); // per host sample
    double s = 0.0, c = 0.0;
    for (int done = 0; done < warmup + measure; done += block)
    {
        auto up = os->processSamplesUp (juce::dsp::AudioBlock<float> (silence));
        float* x = up.getChannelPointer (0);
        for (int m = 0; m < block * factor; ++m)
            x[m] = static_cast<float> (std::sin (w * (static_cast<double> (done) + static_cast<double> (m) / factor)));
        juce::dsp::AudioBlock<float> ob (out);
        os->processSamplesDown (ob);
        if (done >= warmup)
            for (int n = 0; n < block; ++n)
            {
                const double t = w * static_cast<double> (done + n);
                s += static_cast<double> (out.getSample (0, n)) * std::sin (t);
                c += static_cast<double> (out.getSample (0, n)) * std::cos (t);
            }
    }
    // out = A sin (w n - phi): s ~ cos (phi), c ~ -sin (phi).
    const double phi = std::atan2 (-c, s);
    return phi / w;
}
} // namespace vs
