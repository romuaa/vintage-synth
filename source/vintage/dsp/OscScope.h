#pragma once

// Oscillator pictures for the editor's Sound page (docs/architecture.md 9.4). JUCE-free.
// The voice's own dsp::Oscillator objects run offline on the message thread: VCO-2 first, VCO-1
// synced to VCO-2's wrap exactly as Voice::renderSample does, after a warm-up of kLatency samples
// (the BLEP look-back). Without sync each scope shows two cycles of its own oscillator; with sync
// the window is two cycles of VCO-2 (the restarts are what the picture is about) and VCO-1 runs at
// its octave ratio to it (capped so a cycle keeps at least four points).

#include "vintage/dsp/Oscillator.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace vs::dsp
{
struct OscScopeSettings
{
    int wave1 = 2, wave2 = 2;   // kVcoWaveLabels order
    int octave1 = 0, octave2 = 0; // octaves relative to 8'
    bool loFreq2 = false;       // VCO-2 "Lo Freq" mode (about 2 Hz at 8')
    float fineSemis = 0.0f;     // VCO-2 fine tune
    bool sync = false;
    float width = 0.5f;         // pulse width (Manual: 0.5 - 0.45 x pwm; else the resting 0.5)

    /** Bitwise equality (no float ==). */
    bool operator== (const OscScopeSettings& o) const noexcept
    {
        return wave1 == o.wave1 && wave2 == o.wave2 && octave1 == o.octave1 && octave2 == o.octave2 && loFreq2 == o.loFreq2
               && std::bit_cast<std::uint32_t> (fineSemis) == std::bit_cast<std::uint32_t> (o.fineSemis) && sync == o.sync
               && std::bit_cast<std::uint32_t> (width) == std::bit_cast<std::uint32_t> (o.width);
    }
};

struct OscScopes
{
    static constexpr int kPoints = 256;
    std::array<float, kPoints> osc1 {}, osc2 {};
};

/** VCO-1 frequency / VCO-2 frequency at middle C. */
inline double oscScopeRatio (const OscScopeSettings& s) noexcept
{
    constexpr double middleCHz = 261.6255653;
    const double f1 = middleCHz * std::exp2 (static_cast<double> (s.octave1));
    const double f2 = s.loFreq2 ? 2.0 * std::exp2 (static_cast<double> (s.octave2))
                                : middleCHz * std::exp2 (static_cast<double> (s.octave2) + static_cast<double> (s.fineSemis) / 12.0);
    return f1 / f2;
}

inline void renderOscScopes (const OscScopeSettings& s, OscScopes& out) noexcept
{
    constexpr int n = OscScopes::kPoints;
    constexpr float twoCycles = 2.0f / static_cast<float> (n);
    Oscillator vco1, vco2;
    vco1.setWave (static_cast<Wave> (std::clamp (s.wave1, 0, static_cast<int> (kNumWaves) - 1)));
    vco2.setWave (static_cast<Wave> (std::clamp (s.wave2, 0, static_cast<int> (kNumWaves) - 1)));
    vco1.setSeed (0x51u);
    vco2.setSeed (0x52u);
    vco1.reset();
    vco2.reset();
    const float dt2 = twoCycles;
    const float dt1 = s.sync ? static_cast<float> (std::min (static_cast<double> (dt2) * oscScopeRatio (s), 0.25)) : twoCycles;
    const float width = std::clamp (s.width, 0.05f, 0.95f);
    for (int i = -Oscillator::kLatency; i < n; ++i)
    {
        float wrap2 = -1.0f, unused = -1.0f;
        const float o2 = vco2.process (dt2, width, -1.0f, wrap2);
        const float o1 = vco1.process (dt1, width, s.sync ? wrap2 : -1.0f, unused);
        if (i >= 0)
        {
            out.osc1[static_cast<size_t> (i)] = o1;
            out.osc2[static_cast<size_t> (i)] = o2;
        }
    }
}
} // namespace vs::dsp
