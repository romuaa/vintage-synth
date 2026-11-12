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

    - Balance (-1 = Lower only, +1 = Upper only) and pan per part (-1 = left, +1 = right): never
      a boost; unity at the centre, the near side stays at 1 and the far side falls to exactly 0
      (map::balanceGains, map::panGains). In Whole mode Lower is held at unity whatever the balance.
    - The four resulting gains (part x channel) are smoothed per sample (one-pole, kSmoothingSeconds),
      as target + decaying deviation, so they converge exactly and do not depend on the block size.
    - Latch (owner decision, M3 review): after a key-mode change a part that is still sounding keeps
      its gains (new targets are held back) until it starts its first new note or goes silent, so
      release tails fade at the level they were heard at (a part the balance had silenced in Dual
      does not reappear in Whole). The caller unlatches at the note-on sample (smoothed from there)
      or, once the part is silent, with a jump (inaudible, and the result is then the same whenever
      the silence was detected).
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

    /** Jumps to the current targets (no smoothing); clears any latch (its held-back targets apply). */
    void reset() noexcept
    {
        for (int p = 0; p < 2; ++p)
            unlatch (p, true);
        dev.fill (0.0f);
    }

    /** Holds part p (0 = Lower, 1 = Upper) at its current targets; later targets wait for unlatch(). */
    void latch (int p) noexcept
    {
        const auto k = static_cast<size_t> (2 * p);
        if (! latched[static_cast<size_t> (p)])
        {
            pending[k] = tgt[k];
            pending[k + 1] = tgt[k + 1];
        }
        latched[static_cast<size_t> (p)] = true;
    }

    /** Applies part p's held-back targets: smoothed from now, or at once if jump. */
    void unlatch (int p, bool jump) noexcept
    {
        const auto q = static_cast<size_t> (p);
        if (! latched[q])
            return;
        latched[q] = false;
        const auto k = 2 * q;
        retarget (k, pending[k]);
        retarget (k + 1, pending[k + 1]);
        if (jump)
            dev[k] = dev[k + 1] = 0.0f;
    }

    bool isLatched (int p) const noexcept { return latched[static_cast<size_t> (p)]; }

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
        setPart (0, gLower * pl.left, gLower * pl.right);
        setPart (1, bal.upper * pu.left, bal.upper * pu.right);
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
            // Settled deviations snap to 0 per sample (no denormals, exactly the target, and the
            // same sample whatever the block size).
            for (auto& d : dev)
                d = std::abs (d) < 1.0e-12f ? 0.0f : d * keep;
            const float lo = lower[i];
            const float up = upper != nullptr ? upper[i] : 0.0f;
            left[i] = lo * lL + up * uL;
            right[i] = lo * lR + up * uR;
        }
    }

    /** Current (smoothed) gain of a part in a channel: part 0 = Lower, 1 = Upper; channel 0 = L. */
    float gain (int part, int channel) const noexcept
    {
        const auto k = static_cast<size_t> (part * 2 + channel);
        return tgt[k] + dev[k];
    }

private:
    enum : size_t { kLowerL, kLowerR, kUpperL, kUpperR };

    void setPart (size_t p, float l, float r) noexcept
    {
        const auto k = 2 * p;
        if (latched[p])
        {
            pending[k] = l;
            pending[k + 1] = r;
            return;
        }
        retarget (k, l);
        retarget (k + 1, r);
    }

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
    std::array<float, 4> pending { 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<bool, 2> latched {};
};
} // namespace vs
