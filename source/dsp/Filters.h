#pragma once

#include "dsp/DspUtil.h"

namespace vs::dsp
{
/** Prewarped TPT integrator gain for cutoff fc at sampleRate. */
inline float tptG (float fc, float sampleRate) noexcept { return std::tan (kPi * fc / sampleRate); }

/** 1-pole TPT high-pass (non-resonant), zero-delay feedback. */
class OnePoleHighPass
{
public:
    void reset() noexcept { s = 0.0f; }

    float process (float x, float g) noexcept
    {
        const float v = (x - s) * (g / (1.0f + g));
        const float lp = v + s;
        s = lp + v;
        return x - lp;
    }

private:
    float s = 0.0f;
};

/**
    4-pole OTA cascade low-pass (IR3109 style), zero-delay feedback.

    The global feedback is solved linearly, then saturated with tanh at the
    cascade input. That bounds self-oscillation and gives the resonance its
    character without needing an iterative solver.

    Slope -24 dB taps stage 4. Slope -12 dB taps stage 2 while the feedback still
    comes from stage 4, so the resonance (and self-oscillation) behaves the same
    in both modes.
*/
class OtaLadder
{
public:
    void reset() noexcept { s1 = s2 = s3 = s4 = 0.0f; }

    /** @param g tptG(cutoff), @param k feedback 0..~4.2 */
    float process (float x, float g, float k, bool slope24) noexcept
    {
        const float G = g / (1.0f + g);
        const float b = 1.0f / (1.0f + g);

        const float G2 = G * G;
        const float sigma = G2 * G * b * s1 + G2 * b * s2 + G * b * s3 + b * s4;
        const float uLin = (x - k * sigma) / (1.0f + k * G2 * G2);
        const float u = std::tanh (uLin);

        const float y1 = stage (u, s1, G);
        const float y2 = stage (y1, s2, G);
        const float y3 = stage (y2, s3, G);
        const float y4 = stage (y3, s4, G);

        // Partial passband-loss compensation (linear DC gain is 1 / (1 + k)).
        const float comp = 1.0f + 0.5f * k;
        return (slope24 ? y4 : y2) * comp;
    }

private:
    static float stage (float in, float& s, float G) noexcept
    {
        const float v = (in - s) * G;
        const float y = v + s;
        s = y + v;
        return y;
    }

    float s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f;
};
} // namespace vs::dsp
