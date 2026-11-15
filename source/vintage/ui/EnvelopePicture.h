#pragma once

// The envelope and LFO pictures of the Envelopes and LFO pages (docs/architecture.md 9.4),
// computed by the engine's own code with the parameters' own mappings. JUCE-free.
//
//  - Envelope shape: `dsp::Envelope` run per segment at a display rate of 256 samples per segment
//    (the rate that makes the segment's time 256 samples), sustain from the parameter. The shape of
//    each segment as a fraction of its time is the real RC curve; the page's x axis gives each
//    segment its width.
//  - LFO waveform: `dsp::Lfo` with a fixed preview seed, 1000 points over the window, `trigger()`
//    at time 0.

#include "common/params/Mappings.h"
#include "common/ui/models/EnvelopeModel.h"
#include "common/ui/models/LfoModel.h"
#include "vintage/dsp/Envelope.h"
#include "vintage/dsp/Lfo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vs::ui
{
/** The shape of an envelope with the given segment times (seconds) and sustain (0 ... 1). */
inline model::EnvelopeShape computeEnvelopeShape (float attackS, float decayS, float sustain, float releaseS)
{
    constexpr int n = model::EnvelopeShape::kSteps;
    model::EnvelopeShape out;
    out.sustain = sustain;
    dsp::Envelope env;
    env.reset();
    env.noteOn();

    // The display rate of a segment: its time is n samples. Coefficients follow the rate.
    const auto configure = [&] (float segmentSeconds) {
        env.setSampleRate (static_cast<float> (n) / segmentSeconds);
        env.setTimes (attackS, decayS, releaseS);
    };

    configure (attackS);
    out.attack[0] = 0.0f;
    for (int i = 1; i <= n; ++i)
        out.attack[static_cast<size_t> (i)] = std::min (env.process (sustain), 1.0f);
    for (int guard = 0; env.getStage() == dsp::Envelope::Stage::Attack && guard < 8; ++guard)
        env.process (sustain); // the attack ends when it reaches 1.0 (within a step of the n-th)
    out.attack[static_cast<size_t> (n)] = 1.0f;

    configure (decayS);
    out.decay[0] = 1.0f;
    for (int i = 1; i <= n; ++i)
        out.decay[static_cast<size_t> (i)] = env.process (sustain);

    env.noteOff();
    configure (releaseS);
    out.release[0] = out.decay[static_cast<size_t> (n)];
    for (int i = 1; i <= n; ++i)
        out.release[static_cast<size_t> (i)] = env.process (sustain);
    return out;
}

/** The shape for normalised parameter values: the times through the parameters' own mapping. */
inline model::EnvelopeShape computeEnvelopeShape (const model::EnvelopeValues& v)
{
    return computeEnvelopeShape (map::envTimeSec (v.attack), map::envTimeSec (v.decay), v.sustain, map::envTimeSec (v.release));
}

inline constexpr std::uint32_t kLfoPreviewSeed = 0x2545F491u;

/** The LFO over `windowSec` (1000 points). `wave` is the choice index (kLfoWaveLabels order). */
inline void computeLfoPreview (int wave, float rateHz, float delaySec, float windowSec, model::LfoPreview& out)
{
    constexpr int n = model::LfoPreview::kPoints;
    dsp::Lfo lfo;
    lfo.prepare (static_cast<float> (n) / windowSec, kLfoPreviewSeed);
    lfo.setWave (static_cast<dsp::LfoWave> (std::clamp (wave, 0, static_cast<int> (dsp::kNumLfoWaves) - 1)));
    lfo.setRate (rateHz);
    lfo.setDelay (delaySec);
    if (wave >= static_cast<int> (dsp::LfoWave::Random))
    {
        // The real LFO is free-running and keeps its last value across notes: Random and Noise would
        // start flat (a fresh Lfo holds 0 until its first cycle ends). Run two cycles first, at the
        // preview's own deterministic seed, so the picture shows real values from its first point.
        const int preroll = static_cast<int> (std::ceil (2.0 * static_cast<double> (n) / static_cast<double> (windowSec) / static_cast<double> (rateHz))) + 2;
        for (int i = 0; i < preroll; ++i)
            lfo.process();
    }
    lfo.trigger();
    out.windowSec = windowSec;
    for (int i = 0; i < n; ++i)
        out.y[static_cast<size_t> (i)] = lfo.process();
}
} // namespace vs::ui
