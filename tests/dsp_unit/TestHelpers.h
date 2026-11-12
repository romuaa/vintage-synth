#pragma once

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <vector>

namespace vs::test
{
inline double rms (const float* x, size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i)
        acc += static_cast<double> (x[i]) * static_cast<double> (x[i]);
    return std::sqrt (acc / static_cast<double> (n));
}

inline double toDb (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-12)); }

/** Magnitude spectrum (rectangular window; use with coherent, bin-centred signals). */
inline std::vector<float> magnitudeSpectrum (const std::vector<float>& x, int order)
{
    const auto n = static_cast<size_t> (1) << order;
    std::vector<float> buf (2 * n, 0.0f);
    std::copy (x.begin(), x.begin() + static_cast<std::ptrdiff_t> (n), buf.begin());
    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (buf.data(), true);
    buf.resize (n / 2);
    return buf;
}

/** Average frequency from rising zero crossings (linear interpolation). */
inline double measureFrequency (const std::vector<float>& x, double sampleRate)
{
    double first = -1.0, last = -1.0;
    int count = 0;
    for (size_t i = 1; i < x.size(); ++i)
    {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = static_cast<double> (i - 1) + static_cast<double> (-x[i - 1] / (x[i] - x[i - 1]));
            if (first < 0.0)
                first = t;
            else
                ++count;
            last = t;
        }
    }
    return count > 0 ? sampleRate * count / (last - first) : 0.0;
}
} // namespace vs::test
