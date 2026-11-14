#pragma once

// Oscillator pictures for the editor's Sound page (docs/architecture.md 9.4). JUCE-free.
// The picture runs the voice's own code offline on the message thread: the configuration comes from
// oscConfigFor (what Voice::setBlockParams applies), the pulse width from the voice's mapping at
// rest (LFO at its centre, ENV-1 at 0), and every sample from oscPairSample (what
// Voice::renderSample runs: VCO-2 first, VCO-1 with cross mod and synced to VCO-2's wrap), after a
// warm-up of kLatency samples (the BLEP look-back). Without sync and cross mod each scope shows two
// cycles of its own oscillator; with either, the window is two cycles of VCO-2 and VCO-1 runs at its
// frequency ratio to it (capped at four points per cycle).

#include "vintage/engine/OscPath.h"
#include "vintage/params/PartMappings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace vs
{
struct OscScopeSettings
{
    OscConfig config;
    float fineSemis = 0.0f;    // VCO-2 fine tune (semitones)
    float crossModOct = 0.0f;  // map::crossModOct of the Cross mod slider
    float width = 0.5f;        // pulse width at rest

    /** Bitwise equality (no float ==). */
    bool operator== (const OscScopeSettings& o) const noexcept
    {
        return config == o.config && std::bit_cast<std::uint32_t> (fineSemis) == std::bit_cast<std::uint32_t> (o.fineSemis)
               && std::bit_cast<std::uint32_t> (crossModOct) == std::bit_cast<std::uint32_t> (o.crossModOct)
               && std::bit_cast<std::uint32_t> (width) == std::bit_cast<std::uint32_t> (o.width);
    }
};

/** The picture's settings for a part's parameters (only the oscillator-related members are read). */
inline OscScopeSettings oscScopeSettingsFor (const PartParams& p) noexcept
{
    OscScopeSettings s;
    s.config = oscConfigFor (p);
    s.fineSemis = map::fineTuneSemis (p.vco2FineTune);
    s.crossModOct = map::crossModOct (p.vco1CrossMod);
    s.width = restingPulseWidth (s.config, p.pwmDepth);
    return s;
}

struct OscScopes
{
    static constexpr int kPoints = 256;
    std::array<float, kPoints> osc1 {}, osc2 {};
};

/** VCO-1 frequency / VCO-2 frequency at middle C. */
inline double oscScopeRatio (const OscScopeSettings& s) noexcept
{
    constexpr double middleCHz = 261.6255653;
    const double f1 = middleCHz * std::exp2 (static_cast<double> (s.config.octave1));
    const double f2 = s.config.loFreq2 ? 2.0 * std::exp2 (static_cast<double> (s.config.octave2))
                                       : middleCHz * std::exp2 (static_cast<double> (s.config.octave2) + static_cast<double> (s.fineSemis) / 12.0);
    return f1 / f2;
}

inline void renderOscScopes (const OscScopeSettings& s, OscScopes& out) noexcept
{
    constexpr int n = OscScopes::kPoints;
    constexpr float twoCycles = 2.0f / static_cast<float> (n);
    dsp::Oscillator vco1, vco2;
    vco1.setWave (static_cast<dsp::Wave> (std::clamp (s.config.wave1, 0, static_cast<int> (dsp::kNumWaves) - 1)));
    vco2.setWave (static_cast<dsp::Wave> (std::clamp (s.config.wave2, 0, static_cast<int> (dsp::kNumWaves) - 1)));
    vco1.setSeed (0x51u);
    vco2.setSeed (0x52u);
    vco1.reset();
    vco2.reset();
    const bool crossOn = s.crossModOct > 0.0f;
    const bool coupled = s.config.sync || crossOn;
    const float dt2 = twoCycles;
    const float dt1 = coupled ? static_cast<float> (std::min (static_cast<double> (dt2) * oscScopeRatio (s), 0.25)) : twoCycles;
    for (int i = -dsp::Oscillator::kLatency; i < n; ++i)
    {
        float o1, o2;
        oscPairSample (vco1, vco2, dt1, dt2, s.width, crossOn, s.crossModOct, s.config.sync, o1, o2);
        if (i >= 0)
        {
            out.osc1[static_cast<size_t> (i)] = o1;
            out.osc2[static_cast<size_t> (i)] = o2;
        }
    }
}
} // namespace vs
