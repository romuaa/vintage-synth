#pragma once

// Envelope picture maths (docs/architecture.md 9.3.2). JUCE-free.
//
// The x axis is the sliders' own scale (logarithmic time), not seconds: three slots of
// S = 0.8 W / 3; attack, decay and release each span S x (0.1 + 0.9 v) (v = the normalised
// parameter); the sustain plateau is a fixed 0.2 W; y is the level 0 ... 1 (linear, as sustain).

#include "common/ui/models/ParamTarget.h"

#include <algorithm>
#include <array>

namespace vs::ui::model
{
/** The four normalised parameter values of one envelope. */
struct EnvelopeValues
{
    float attack = 0.0f, decay = 0.0f, sustain = 0.0f, release = 0.0f;
};

/**
    The envelope's shape as a function of the fraction of each segment: levels at i / 256 of the
    attack (0 -> 1), the decay (1 -> near the sustain) and the release (-> near 0). Computed by the
    instrument from its own envelope code (the page); the view only draws it.
*/
struct EnvelopeShape
{
    static constexpr int kSteps = 256;
    std::array<float, kSteps + 1> attack {}, decay {}, release {};
    float sustain = 0.0f;
};

/** Horizontal and vertical geometry of one envelope picture, in the plot's px. */
struct EnvelopeLayout
{
    float x0 = 0.0f, width = 100.0f;      // plot left and width (W)
    float yTop = 0.0f, yBase = 100.0f;    // y of level 1 and of level 0

    float slot() const noexcept { return 0.8f * width / 3.0f; }
    float segmentWidth (float v) const noexcept { return slot() * (0.1f + 0.9f * clamp01 (v)); }
    float plateauWidth() const noexcept { return 0.2f * width; }
    float levelY (float level) const noexcept { return yBase + (yTop - yBase) * level; }
    float levelAt (float y) const noexcept { return (y - yBase) / (yTop - yBase); }

    struct Points
    {
        float peakX = 0.0f, decayEndX = 0.0f, plateauEndX = 0.0f, releaseEndX = 0.0f;
        float topY = 0.0f, sustainY = 0.0f, baseY = 0.0f;
    };
    Points points (const EnvelopeValues& v) const noexcept
    {
        Points p;
        p.peakX = x0 + segmentWidth (v.attack);
        p.decayEndX = p.peakX + segmentWidth (v.decay);
        p.plateauEndX = p.decayEndX + plateauWidth();
        p.releaseEndX = p.plateauEndX + segmentWidth (v.release);
        p.topY = levelY (1.0f);
        p.sustainY = levelY (clamp01 (v.sustain));
        p.baseY = levelY (0.0f);
        return p;
    }

    /** Normalised time parameter for a segment that ends at `x` and starts at `startX`. */
    float timeFromX (float x, float startX) const noexcept { return clamp01 (((x - startX) / slot() - 0.1f) / 0.9f); }
    float attackFromX (float x) const noexcept { return timeFromX (x, x0); }
    float decayFromX (float x, float peakX) const noexcept { return timeFromX (x, peakX); }
    float releaseFromX (float x, float plateauEndX) const noexcept { return timeFromX (x, plateauEndX); }
};

/** Places `n` labels of `width` px, wanted at the centres `desired` (left to right), inside
    [lo, hi] and never overlapping: the picture's time labels under the handles. */
inline void spreadLabels (const float* desired, float* out, int n, float width, float lo, float hi) noexcept
{
    const float half = 0.5f * width;
    for (int i = 0; i < n; ++i)
    {
        out[i] = std::clamp (desired[i], lo + half, hi - half);
        if (i > 0 && out[i] < out[i - 1] + width)
            out[i] = out[i - 1] + width;
    }
    if (n > 0 && out[n - 1] > hi - half)
        out[n - 1] = hi - half;
    for (int i = n - 2; i >= 0; --i)
        if (out[i] > out[i + 1] - width)
            out[i] = out[i + 1] - width;
}
} // namespace vs::ui::model
