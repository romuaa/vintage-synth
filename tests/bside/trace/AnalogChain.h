#pragma once

// Test support (S3a, docs/architecture.md 8.19.1): a trace rendered through our whole analog chain
// (Chip::render -> Filter -> OutputStage, board filters included) at an internal rate, decimated to the 48 kHz
// signal the reference's .bsspec describes (8.9.4: 192 kHz internal, one linear-phase decimator).
//
// Until the renderer has two buses (S3b) Chip::render gives one sum of the three voices. Per internal sample the
// sum goes to the bus of the voices that sound (envelope counter above 0): the filter when all of them are
// routed to it, the direct path when none is, nowhere when the only one is oscillator 3 with "oscillator 3
// off" and not routed. Voices that sound on different paths make the sample *ambiguous*: its sum goes to the
// direct path and it is counted, and a gate leaves out every segment that holds such a sample. The voices'
// DC (OutputStage::filterBusLevel, kEnvDc) is exact per voice: it uses each voice's own envelope counter.
// Register writes reach the filter and the output stage at the sample of their cycle (the renderer's latency
// is removed first, so audio, envelopes and registers line up in chip time).
//
// JUCE-free; allocates (offline test code).

#include "bside/chip/Chip.h"
#include "bside/chip/Filter.h"
#include "bside/chip/OutputStage.h"
#include "bside/chip/RenderTables.h"
#include "bside/trace/SpecAnalysis.h"
#include "bside/trace/TraceFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vs::bside::test
{
struct ChainOptions
{
    double rate = 192000.0;   // internal rate; a multiple of 48 kHz for the decimator
    bool volumeDc = true;     // the reference has it
    float variation = 0.5f;   // Model A: the reference's curve
};

struct ChainRender
{
    std::vector<float> out;              // 48 kHz, trace::specSampleCount (t) samples (sample i at cycle i x clock / 48000)
    std::vector<std::uint8_t> ambiguous; // per 48 kHz sample: 1 where voices sounding on both paths met
    std::vector<float> internal;         // the chain's output at the internal rate, chip time (tests)
    std::size_t ambiguousSamples = 0;    // internal samples
};

/** A linear-phase low-pass FIR (Kaiser-windowed sinc, beta 12): pass to 20 kHz, about -120 dB from 24 kHz. */
inline std::vector<double> decimatorTaps (int factor)
{
    const int taps = 192 * factor - 1;
    constexpr double pi = 3.14159265358979323846;
    const double fc = 22000.0 / (48000.0 * factor);
    constexpr double beta = 12.0;
    auto i0 = [] (double x) {
        double s = 1.0, t = 1.0;
        for (int k = 1; k < 60; ++k)
        {
            t *= (x / (2.0 * k)) * (x / (2.0 * k));
            s += t;
        }
        return s;
    };
    std::vector<double> h (static_cast<std::size_t> (taps));
    double sum = 0.0;
    for (int n = 0; n < taps; ++n)
    {
        const double m = n - (taps - 1) / 2.0;
        const double sinc = std::abs (m) < 1e-12 ? 2.0 * fc : std::sin (2.0 * pi * fc * m) / (pi * m);
        const double r = 2.0 * n / (taps - 1.0) - 1.0;
        h[static_cast<std::size_t> (n)] = sinc * i0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0 (beta);
        sum += h[static_cast<std::size_t> (n)];
    }
    for (double& v : h)
        v /= sum;
    return h;
}

inline ChainRender renderChain (const trace::Trace& t, const ChainOptions& o = {})
{
    using namespace vs::bside::chip;
    const Model model = t.model == trace::Model::A ? Model::A : Model::B;
    Chip c;
    Config cfg;
    cfg.model = model;
    cfg.clockHz = t.clockHz;
    cfg.internalRate = o.rate;
    c.prepare (cfg);
    const auto& env = render::tablesFor (model, cfg.shapes).envelope;
    const int factor = static_cast<int> (std::lround (o.rate / 48000.0));
    const auto h = decimatorTaps (factor);
    const auto n48 = static_cast<std::size_t> (trace::specSampleCount (t));
    const std::size_t half = (h.size() - 1) / 2;
    const std::size_t lat = static_cast<std::size_t> (Chip::kLatencySamples);
    const std::size_t total = static_cast<std::size_t> (factor) * n48 + half + lat + 8;

    // 1. render sample by sample, recording the registers and envelope counters at each sample's end
    std::vector<float> x (total, 0.0f);
    struct Rec
    {
        std::array<std::uint8_t, 4> regs; // 0x15 .. 0x18
        std::array<std::uint8_t, 3> env;
    };
    std::vector<Rec> rec (total);
    std::size_t done = 0, next = 0;
    while (done < total)
    {
        const std::uint64_t target = next < t.writes.size() ? t.writes[next].cycle : ~std::uint64_t { 0 };
        const int n = c.renderUntil (x.data() + done, 1, target);
        while (next < t.writes.size() && c.cycle() == t.writes[next].cycle)
        {
            c.write (t.writes[next].reg, t.writes[next].value);
            ++next;
        }
        if (n == 1)
        {
            const auto s = c.state();
            Rec& r = rec[done];
            for (std::size_t i = 0; i < 4; ++i)
                r.regs[i] = s.registers[0x15 + i];
            for (std::size_t i = 0; i < 3; ++i)
                r.env[i] = s.env[i].counter;
            ++done;
        }
    }

    // 2. the chain in chip time: sample j uses audio x[j + lat] and the registers and envelopes of sample j
    Filter filter;
    filter.prepare (model, o.rate, o.variation);
    OutputStage stage;
    stage.prepare (model, o.rate, o.volumeDc);
    ChainRender r;
    const std::size_t m = total - lat;
    r.internal.assign (m, 0.0f);
    std::vector<std::uint8_t> amb (m, 0);
    for (std::size_t j = 0; j < m; ++j)
    {
        const Rec& q = rec[j];
        const std::uint8_t res = q.regs[2];
        const std::uint8_t mv = q.regs[3];
        filter.setCutoff (static_cast<float> ((q.regs[0] & 7) | (q.regs[1] << 3)));
        filter.setResonance (res >> 4);
        filter.setMode (mv);
        stage.setRouting (res);
        stage.setModeVolume (mv);
        stage.setFilterStaticDc (filter.staticOutput (stage.staticFilterBusDc()));
        float envF = 0.0f, envD = 0.0f;
        int bus = -1; // -1 none sounding, 0 direct, 1 filter, 2 off, 3 ambiguous
        for (int v = 0; v < 3; ++v)
        {
            const bool routed = ((res >> v) & 1) != 0;
            const bool off = v == 2 && ! routed && (mv & 0x80) != 0;
            const float e = env[q.env[static_cast<std::size_t> (v)]];
            if (routed)
                envF += e;
            else if (! off)
                envD += e;
            if (q.env[static_cast<std::size_t> (v)] != 0)
            {
                const int b = routed ? 1 : (off ? 2 : 0);
                bus = bus < 0 ? b : (bus == b ? bus : 3);
            }
        }
        const float a = x[j + lat];
        float aF = 0.0f, aD = 0.0f;
        if (bus == 1)
            aF = a;
        else if (bus == 0 || bus < 0 || bus == 3)
            aD = a;
        if (bus == 3)
        {
            amb[j] = 1;
            ++r.ambiguousSamples;
        }
        const float fOut = filter.process (stage.filterBusLevel (aF, envF));
        r.internal[j] = stage.process (fOut, aD, envD);
    }

    // 3. decimate: output i at chip time i / 48000 = internal sample factor x i
    r.out.assign (n48, 0.0f);
    r.ambiguous.assign (n48, 0);
    for (std::size_t i = 0; i < n48; ++i)
    {
        const std::size_t centre = static_cast<std::size_t> (factor) * i;
        double acc = 0.0;
        for (std::size_t k = 0; k < h.size(); ++k)
        {
            const std::ptrdiff_t idx = static_cast<std::ptrdiff_t> (centre) + static_cast<std::ptrdiff_t> (half) - static_cast<std::ptrdiff_t> (k);
            // positions before 0 take the first sample's value (the reference's convention, 8.15.7)
            const std::size_t ii = idx < 0 ? 0 : static_cast<std::size_t> (idx);
            if (ii < m)
                acc += h[k] * static_cast<double> (r.internal[ii]);
        }
        r.out[i] = static_cast<float> (acc);
        for (std::size_t k = 0; k < static_cast<std::size_t> (factor); ++k)
            if (centre + k < m && amb[centre + k] != 0)
                r.ambiguous[i] = 1;
    }
    return r;
}
} // namespace vs::bside::test
