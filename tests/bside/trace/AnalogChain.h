#pragma once

// Test support (S3a, docs/architecture.md 8.19.1; S3b-1, 8.19.4): a trace rendered through our whole analog chain
// at an internal rate, decimated to the 48 kHz signal the reference's .bsspec describes (8.9.4: 192 kHz internal,
// one linear-phase decimator).
//
// renderChain (S3b-1, the gates): the real chain, chip::ChipVoice (the renderer's two buses -> Filter -> OutputStage,
// every write at its cycle), at the bottom of this file.
//
// renderChainTestSide (S3a's harness, kept for the diagnostics): Chip::render gave one sum of the three voices
// before S3b. Two ways around it:
// - PER VOICE (recordChip, when no sync or ring bit is ever written, so the voices do not interact): voice k is
//   rendered by a chip given only voice k's register writes (and the filter's), minus two thirds of a chip given
//   none (the two other voices' identical power-on output). The buses are then exact, every routing included,
//   and each voice gets its own bend (OutputStage::shapeVoice) before it joins its bus, as the renderer of S3b will.
// - SINGLE SUM (otherwise): per internal sample the sum goes to the bus of the voices that sound (envelope
//   counter above 0); voices sounding on different paths make the sample *ambiguous*: its sum goes to the direct
//   path, it is counted, and a gate leaves out every segment or frame that holds such a sample.
// The voices' DC uses each voice's own envelope counter. Register writes reach the filter and the output stage
// at the sample of their cycle (the renderer's latency is removed first, so audio, envelopes and registers line
// up in chip time).
//
// JUCE-free; allocates (offline test code).

#include "bside/chip/Chip.h"
#include "bside/chip/ChipVoice.h"
#include "bside/chip/Filter.h"
#include "bside/chip/OutputStage.h"
#include "bside/chip/RenderTables.h"
#include "bside/trace/SpecAnalysis.h"
#include "bside/trace/TraceFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
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

struct ChipRecord
{
    vs::bside::chip::Model model = vs::bside::chip::Model::B;
    double rate = 192000.0;
    std::size_t n48 = 0, samples = 0; // 48 kHz outputs; internal samples in chip time
    bool perVoice = false;            // voice[k] holds voice k alone; otherwise voice[0] holds the sum
    std::array<std::vector<float>, 3> voice;
    std::vector<std::array<std::uint8_t, 3>> counters; // envelope counters per sample
    std::vector<std::array<std::uint8_t, 4>> regs;     // registers 0x15 .. 0x18 per sample
};

/** Whether the voices of a trace are independent (no sync or ring bit is ever written): then each voice can be
    rendered alone (recordChip). */
inline bool voicesIndependent (const trace::Trace& t)
{
    for (const auto& w : t.writes)
        if (w.reg < 0x15 && w.reg % 7 == 4 && (w.value & 0x06) != 0)
            return false;
    return true;
}

namespace detail
{
/** One chip over the trace's writes that `keep` accepts; audio in chip time (latency removed). */
template <class Keep>
std::vector<float> renderWrites (const trace::Trace& t, double rate, std::size_t total, Keep&& keep,
                                 std::vector<std::array<std::uint8_t, 3>>* counters = nullptr,
                                 std::vector<std::array<std::uint8_t, 4>>* regs = nullptr)
{
    using namespace vs::bside::chip;
    Chip c;
    Config cfg;
    cfg.model = t.model == trace::Model::A ? Model::A : Model::B;
    cfg.clockHz = t.clockHz;
    cfg.internalRate = rate;
    c.prepare (cfg);
    const std::size_t lat = static_cast<std::size_t> (Chip::kLatencySamples);
    std::vector<float> x (total + lat, 0.0f);
    if (counters != nullptr)
        counters->assign (total + lat, {});
    if (regs != nullptr)
        regs->assign (total + lat, {});
    std::size_t done = 0, next = 0;
    auto skip = [&] () {
        while (next < t.writes.size() && ! keep (t.writes[next].reg))
            ++next;
    };
    skip();
    while (done < total + lat)
    {
        const std::uint64_t target = next < t.writes.size() ? t.writes[next].cycle : ~std::uint64_t { 0 };
        const int n = c.renderUntil (x.data() + done, 1, target);
        while (next < t.writes.size() && c.cycle() == t.writes[next].cycle)
        {
            c.write (t.writes[next].reg, t.writes[next].value);
            ++next;
            skip();
        }
        if (n == 1)
        {
            if (counters != nullptr || regs != nullptr)
            {
                const auto s = c.state();
                if (regs != nullptr)
                    for (std::size_t i = 0; i < 4; ++i)
                        (*regs)[done][i] = s.registers[0x15 + i];
                if (counters != nullptr)
                    for (std::size_t i = 0; i < 3; ++i)
                        (*counters)[done][i] = s.env[i].counter;
            }
            ++done;
        }
    }
    x.erase (x.begin(), x.begin() + static_cast<std::ptrdiff_t> (lat));
    if (counters != nullptr)
        counters->resize (total);
    if (regs != nullptr)
        regs->resize (total);
    return x;
}
} // namespace detail

/** The chip side of a trace at an internal rate (see the header): per-voice audio when the voices are
    independent and `perVoice`, the sum otherwise; envelope counters and filter registers per sample. */
inline ChipRecord recordChip (const trace::Trace& t, double rate = 192000.0, bool perVoice = true)
{
    ChipRecord r;
    r.model = t.model == trace::Model::A ? vs::bside::chip::Model::A : vs::bside::chip::Model::B;
    r.rate = rate;
    const int factor = static_cast<int> (std::lround (rate / 48000.0));
    r.n48 = static_cast<std::size_t> (trace::specSampleCount (t));
    const std::size_t taps = static_cast<std::size_t> (192 * factor - 1);
    r.samples = static_cast<std::size_t> (factor) * r.n48 + (taps - 1) / 2 + 8;
    auto all = [] (std::uint8_t) { return true; };
    r.voice[0] = detail::renderWrites (t, rate, r.samples, all, &r.counters, &r.regs);
    r.perVoice = perVoice && voicesIndependent (t);
    if (r.perVoice)
    {
        auto none = [] (std::uint8_t reg) { return reg >= 0x15; };
        const auto base = detail::renderWrites (t, rate, r.samples, none);
        for (int k = 0; k < 3; ++k)
        {
            auto only = [k] (std::uint8_t reg) { return reg >= 0x15 || reg / 7 == k; };
            auto v = detail::renderWrites (t, rate, r.samples, only);
            for (std::size_t i = 0; i < v.size(); ++i)
                v[i] -= (2.0f / 3.0f) * base[i];
            r.voice[static_cast<std::size_t> (k)] = std::move (v);
        }
    }
    return r;
}

/** Our chain over a chip record (see the header). */
inline ChainRender runChain (const ChipRecord& rec, const ChainOptions& o = {})
{
    using namespace vs::bside::chip;
    const auto& envTable = render::tablesFor (rec.model, ShapeSet::fitted).envelope;
    const int factor = static_cast<int> (std::lround (rec.rate / 48000.0));
    const auto h = decimatorTaps (factor);
    const std::size_t half = (h.size() - 1) / 2;
    Filter filter;
    filter.prepare (rec.model, rec.rate, o.variation);
    OutputStage stage;
    stage.prepare (rec.model, rec.rate, o.volumeDc);
    ChainRender r;
    const std::size_t m = rec.samples;
    r.internal.assign (m, 0.0f);
    std::vector<std::uint8_t> amb (m, 0);
    for (std::size_t j = 0; j < m; ++j)
    {
        const auto& q = rec.regs[j];
        const std::uint8_t res = q[2];
        const std::uint8_t mv = q[3];
        filter.setCutoff (static_cast<float> ((q[0] & 7) | (q[1] << 3)));
        filter.setResonance (res >> 4);
        filter.setMode (mv);
        stage.setRouting (res);
        stage.setModeVolume (mv);
        stage.setFilterStaticDc (filter.staticOutput (stage.staticFilterBusDc()));
        float envF = 0.0f, envD = 0.0f, aF = 0.0f, aD = 0.0f;
        int bus = -1; // -1 none sounding, 0 direct, 1 filter, 2 off, 3 ambiguous
        for (std::size_t v = 0; v < 3; ++v)
        {
            const bool routed = ((res >> v) & 1) != 0;
            const bool off = v == 2 && ! routed && (mv & 0x80) != 0;
            const float e = envTable[rec.counters[j][v]];
            if (routed)
                envF += e;
            else if (! off)
                envD += e;
            if (rec.perVoice)
            {
                const float a = OutputStage::shapeVoice (rec.model, rec.voice[v][j], e);
                if (routed)
                    aF += a;
                else if (! off)
                    aD += a;
            }
            else if (rec.counters[j][v] != 0)
            {
                const int b = routed ? 1 : (off ? 2 : 0);
                bus = bus < 0 ? b : (bus == b ? bus : 3);
            }
        }
        if (! rec.perVoice)
        {
            // one sum: the voice bend is exact only while one voice sounds (its own envelope value)
            float eOne = 0.0f;
            int sounding = 0;
            for (std::size_t v = 0; v < 3; ++v)
                if (rec.counters[j][v] != 0)
                {
                    ++sounding;
                    eOne = envTable[rec.counters[j][v]];
                }
            const float a = sounding == 1 ? OutputStage::shapeVoice (rec.model, rec.voice[0][j], eOne) : rec.voice[0][j];
            if (bus == 1)
                aF = a;
            else if (bus != 2)
                aD = a;
            if (bus == 3)
            {
                amb[j] = 1;
                ++r.ambiguousSamples;
            }
        }
        const float fOut = filter.process (stage.filterBusLevel (aF, envF));
        r.internal[j] = stage.process (fOut, aD, envD);
    }

    // decimate: output i at chip time i / 48000 = internal sample factor x i
    r.out.assign (rec.n48, 0.0f);
    r.ambiguous.assign (rec.n48, 0);
    for (std::size_t i = 0; i < rec.n48; ++i)
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

/** The test-side chain of S3a (per voice or single sum, see the header): kept for the diagnostics that compare
    against it (FilterDiagnosticTests.cpp) and for the record. */
inline ChainRender renderChainTestSide (const trace::Trace& t, const ChainOptions& o = {})
{
    return runChain (recordChip (t, o.rate), o);
}

namespace detail
{
/** Decimates the chain's internal-rate output (chip time: sample j at the start of internal sample j) to the 48 kHz
    signal the reference's .bsspec describes (as runChain). */
inline std::vector<float> decimate (const std::vector<double>& internal, double rate, std::size_t n48)
{
    const int factor = static_cast<int> (std::lround (rate / 48000.0));
    const auto h = decimatorTaps (factor);
    const std::size_t half = (h.size() - 1) / 2;
    std::vector<float> out (n48, 0.0f);
    for (std::size_t i = 0; i < n48; ++i)
    {
        const std::size_t centre = static_cast<std::size_t> (factor) * i;
        double acc = 0.0;
        for (std::size_t k = 0; k < h.size(); ++k)
        {
            const std::ptrdiff_t idx = static_cast<std::ptrdiff_t> (centre) + static_cast<std::ptrdiff_t> (half) - static_cast<std::ptrdiff_t> (k);
            const std::size_t ii = idx < 0 ? 0 : static_cast<std::size_t> (idx); // before 0: the first sample (8.15.7)
            if (ii < internal.size())
                acc += h[k] * internal[ii];
        }
        out[i] = static_cast<float> (acc);
    }
    return out;
}
} // namespace detail

/** A trace through the real chain (docs/architecture.md 8.19.4: chip::ChipVoice, Per Note, its own Filter): every
    write at its exact cycle through ChipVoice::write, the output in double at the internal rate, its latency
    (ChipVoice::latencySamples) removed, decimated to 48 kHz. Nothing is ambiguous: the renderer has both buses (the
    result's ambiguous flags are all 0 and ambiguousSamples is 0 by construction). */
inline ChainRender renderChain (const trace::Trace& t, const ChainOptions& o = {})
{
    using namespace vs::bside::chip;
    Config cfg;
    cfg.model = t.model == trace::Model::A ? Model::A : Model::B;
    cfg.clockHz = t.clockHz;
    cfg.internalRate = o.rate;
    cfg.variation = o.variation;
    cfg.volumeDc = o.volumeDc;
    auto voice = std::make_unique<ChipVoice>();
    voice->prepare (cfg);
    Filter filter;
    filter.prepare (cfg.model, o.rate, o.variation);
    voice->setFilter (&filter);
    const int factor = static_cast<int> (std::lround (o.rate / 48000.0));
    const std::size_t n48 = static_cast<std::size_t> (trace::specSampleCount (t));
    const std::size_t taps = static_cast<std::size_t> (192 * factor - 1);
    const std::size_t lat = static_cast<std::size_t> (voice->latencySamples());
    const std::size_t total = static_cast<std::size_t> (factor) * n48 + (taps - 1) / 2 + 8 + lat;
    std::vector<double> y (total, 0.0);
    std::size_t done = 0, next = 0;
    while (done < total)
    {
        const std::uint64_t target = next < t.writes.size() ? t.writes[next].cycle : ~std::uint64_t { 0 };
        done += static_cast<std::size_t> (voice->renderUntil (y.data() + done, static_cast<int> (std::min<std::size_t> (total - done, 4096)), target));
        while (next < t.writes.size() && voice->chip().cycle() == t.writes[next].cycle)
        {
            voice->write (t.writes[next].reg, t.writes[next].value);
            ++next;
        }
    }
    y.erase (y.begin(), y.begin() + static_cast<std::ptrdiff_t> (lat));
    ChainRender r;
    r.internal.assign (y.size(), 0.0f);
    for (std::size_t i = 0; i < y.size(); ++i)
        r.internal[i] = static_cast<float> (y[i]);
    r.out = detail::decimate (y, o.rate, n48);
    r.ambiguous.assign (n48, 0);
    return r;
}
} // namespace vs::bside::test
