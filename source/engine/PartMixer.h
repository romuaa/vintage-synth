#pragma once

#include "params/Mappings.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace vs
{
/**
    Mono Lower/Upper part outputs -> stereo bus (docs/architecture.md 4.3). JUCE-free, no allocation.

    - Balance (-1 = Lower only, +1 = Upper only): constant power, both parts at unity at 0
      (map::balanceGains). In Whole mode Lower is held at unity whatever the balance.
    - Pan per part (-1 = left, +1 = right): constant power, unity in both channels at the centre
      (map::panGains).
    - The four resulting gains (part x channel) are smoothed per sample (one-pole, kSmoothingSeconds),
      as target + decaying deviation, so they converge exactly and do not depend on the block size.
*/
class PartMixer
{
public:
    static constexpr float kSmoothingSeconds = 0.01f;

    void prepare (double sampleRate) noexcept
    {
        const double a = 1.0 - std::exp (-1.0 / (static_cast<double> (kSmoothingSeconds) * sampleRate));
        keep = static_cast<float> (1.0 - a);
        reset();
    }

    /** Jumps to the current targets (no smoothing). */
    void reset() noexcept { dev.fill (0.0f); }

    /**
        New targets, per block. @param whole  Whole key mode: Lower at unity (the balance has no
        effect), Upper keeps its balance gain for a release tail.
    */
    void setTargets (float balance, float panLower, float panUpper, bool whole) noexcept
    {
        const auto bal = map::balanceGains (balance);
        const auto pl = map::panGains (panLower);
        const auto pu = map::panGains (panUpper);
        const float gLower = whole ? 1.0f : bal.lower;
        retarget (kLowerL, gLower * pl.left);
        retarget (kLowerR, gLower * pl.right);
        retarget (kUpperL, bal.upper * pu.left);
        retarget (kUpperR, bal.upper * pu.right);
    }

    /**
        Mixes n samples: left/right are overwritten. `upper` may be nullptr (Upper not rendered:
        silent); its gains still advance, so the state does not depend on whether it was rendered.
    */
    void process (const float* lower, const float* upper, float* left, float* right, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float lL = tgt[kLowerL] + dev[kLowerL];
            const float lR = tgt[kLowerR] + dev[kLowerR];
            const float uL = tgt[kUpperL] + dev[kUpperL];
            const float uR = tgt[kUpperR] + dev[kUpperR];
            for (auto& d : dev)
                d *= keep;
            const float lo = lower[i];
            const float up = upper != nullptr ? upper[i] : 0.0f;
            left[i] = lo * lL + up * uL;
            right[i] = lo * lR + up * uR;
        }
        for (auto& d : dev)
            if (std::abs (d) < 1.0e-12f)
                d = 0.0f; // settled: no denormals, exactly the target
    }

    /** Current (smoothed) gain of a part in a channel: part 0 = Lower, 1 = Upper; channel 0 = L. */
    float gain (int part, int channel) const noexcept
    {
        const auto k = static_cast<size_t> (part * 2 + channel);
        return tgt[k] + dev[k];
    }

private:
    enum : size_t { kLowerL, kLowerR, kUpperL, kUpperR };

    void retarget (size_t k, float newTarget) noexcept
    {
        if (std::bit_cast<std::uint32_t> (tgt[k]) == std::bit_cast<std::uint32_t> (newTarget))
            return;
        dev[k] = (tgt[k] + dev[k]) - newTarget;
        tgt[k] = newTarget;
    }

    float keep = 0.99f;
    std::array<float, 4> tgt { 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<float, 4> dev {};
};
} // namespace vs
