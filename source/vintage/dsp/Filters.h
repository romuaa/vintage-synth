#pragma once

#include "common/dsp/DspUtil.h"
#include "common/dsp/FastMath.h"

namespace vs::dsp
{
/** Prewarped TPT integrator gain for cutoff fc at sampleRate. */
inline float tptG (float fc, float sampleRate) noexcept { return std::tan (kPi * fc / sampleRate); }

/** One-pole TPT gain G = g / (1 + g) from the prewarped g. */
inline float tptOnePoleG (float g) noexcept { return g / (1.0f + g); }

/** 1-pole TPT high-pass (non-resonant), zero-delay feedback. */
class OnePoleHighPass
{
public:
    void reset() noexcept { s = 0.0f; }

    float process (float x, float g) noexcept { return processG (x, tptOnePoleG (g)); }

    /** @param G tptOnePoleG (g), precomputed (e.g. once per sample for all voices of a part) */
    float processG (float x, float G) noexcept
    {
        const float v = (x - s) * G;
        const float lp = v + s;
        s = lp + v;
        return x - lp;
    }

private:
    float s = 0.0f;
};

/**
    4-pole OTA cascade low-pass, zero-delay feedback.

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

    /**
        Per-sample coefficients. They depend only on cutoff and resonance, so a voice can
        compute a block of them in a vectorised loop before running the (serial) filter.
    */
    struct Coefs
    {
        float G;              // one-pole gain g / (1 + g)
        float c1, c2, c3, c4; // feedback-sum weights G^3 b, G^2 b, G b, b with b = 1 / (1 + g)
        float k;              // feedback gain
        float inv;            // 1 / (1 + k G^4)
        float comp;           // passband-loss compensation (linear DC gain is 1 / (1 + k))
    };

    /** @param g tptG(cutoff), @param k feedback 0..~4.2 */
    static Coefs makeCoefs (float g, float k) noexcept
    {
        const float b = 1.0f / (1.0f + g);
        const float G = g * b;
        const float G2 = G * G;
        return { G, G2 * G * b, G2 * b, G * b, b, k, 1.0f / (1.0f + k * G2 * G2), 1.0f + 0.5f * k };
    }

    /** @param g tptG(cutoff), @param k feedback 0..~4.2 */
    float process (float x, float g, float k, bool slope24) noexcept
    {
        const Coefs c = makeCoefs (g, k);
        return tick (x, c.G, c.c1, c.c2, c.c3, c.c4, c.k, c.inv, slope24) * c.comp;
    }

    /** One sample from precomputed coefficients (see Coefs), without the output compensation. */
    float tick (float x, float G, float c1, float c2, float c3, float c4, float k, float inv, bool slope24) noexcept
    {
        float y2, y4;
        cascade (x, G, c1, c2, c3, c4, k, inv, y2, y4);
        return slope24 ? y4 : y2;
    }

    /**
        tick() with the output between the two taps: stage 2 + (stage 4 - stage 2) * mix24. Used
        while a slope change crossfades on a sounding voice (the taps differ in phase and level,
        so switching between them at once steps the output).
    */
    float tickMix (float x, float G, float c1, float c2, float c3, float c4, float k, float inv, float mix24) noexcept
    {
        float y2, y4;
        cascade (x, G, c1, c2, c3, c4, k, inv, y2, y4);
        return y2 + (y4 - y2) * mix24;
    }

private:
    void cascade (float x, float G, float c1, float c2, float c3, float c4, float k, float inv, float& y2, float& y4) noexcept
    {
        // The global feedback is solved linearly, then saturated at the cascade input.
        const float sigma = c1 * s1 + c2 * s2 + c3 * s3 + c4 * s4;
        const float u = fast::tanh ((x - k * sigma) * inv);

        const float y1 = stage (u, s1, G);
        y2 = stage (y1, s2, G);
        const float y3 = stage (y2, s3, G);
        y4 = stage (y3, s4, G);
    }

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
