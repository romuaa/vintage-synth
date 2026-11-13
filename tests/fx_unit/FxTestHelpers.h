#pragma once

// Shared checks for the common effect requirements (docs/architecture.md 5.1), templated over
// the effect type so Chorus, Delay and Reverb run the same tests.

#include <catch2/catch_test_macros.hpp>

#include "../dsp_unit/TestHelpers.h"
#include "common/fx/FxCommon.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace vs::fxtest
{
inline const double kRates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
constexpr int kMaxBlock = 512;

struct Stereo
{
    std::vector<float> l, r;
    explicit Stereo (size_t n = 0) : l (n, 0.0f), r (n, 0.0f) {}
    size_t size() const { return l.size(); }
};

inline Stereo noise (size_t n, float amp, std::uint32_t seed)
{
    Stereo s (n);
    std::uint32_t st = seed;
    auto next = [&st] {
        st ^= st << 13;
        st ^= st >> 17;
        st ^= st << 5;
        return static_cast<float> (static_cast<std::int32_t> (st)) * (1.0f / 2147483648.0f);
    };
    for (size_t i = 0; i < n; ++i)
    {
        s.l[i] = amp * next();
        s.r[i] = amp * next();
    }
    return s;
}

inline Stereo sine (size_t n, double hz, double fs, float amp)
{
    Stereo s (n);
    for (size_t i = 0; i < n; ++i)
        s.l[i] = s.r[i] = amp * static_cast<float> (std::sin (2.0 * 3.14159265358979323846 * hz * static_cast<double> (i) / fs));
    return s;
}

/** Process `io` in place in blocks; sizeFn(blockIndex) gives each block size (clamped to 1..kMaxBlock).
    paramFn(fx, sampleIndex) is called before every block. */
template <typename Fx, typename SizeFn, typename ParamFn>
void run (Fx& fx, Stereo& io, SizeFn&& sizeFn, ParamFn&& paramFn)
{
    size_t pos = 0;
    int b = 0;
    while (pos < io.size())
    {
        const int want = std::clamp (sizeFn (b++), 1, kMaxBlock);
        const int len = static_cast<int> (std::min (static_cast<size_t> (want), io.size() - pos));
        paramFn (fx, pos);
        fx.process (io.l.data() + pos, io.r.data() + pos, len);
        pos += static_cast<size_t> (len);
    }
}

template <typename Fx, typename Params>
void runConst (Fx& fx, Stereo& io, const Params& p, int block = kMaxBlock)
{
    run (fx, io, [block] (int) { return block; }, [&p] (Fx& f, size_t) { f.setParams (p); });
}

inline bool allFinite (const Stereo& s, float bound, float* peakOut = nullptr)
{
    float peak = 0.0f;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (! std::isfinite (s.l[i]) || ! std::isfinite (s.r[i]))
            return false;
        peak = std::max (peak, std::max (std::abs (s.l[i]), std::abs (s.r[i])));
    }
    if (peakOut != nullptr)
        *peakOut = peak;
    return peak <= bound;
}

inline bool anySubnormal (const Stereo& s)
{
    for (size_t i = 0; i < s.size(); ++i)
        if (std::fpclassify (s.l[i]) == FP_SUBNORMAL || std::fpclassify (s.r[i]) == FP_SUBNORMAL)
            return true;
    return false;
}

inline float maxAbsDiff (const Stereo& a, const Stereo& b)
{
    float m = 0.0f;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max (m, std::max (std::abs (a.l[i] - b.l[i]), std::abs (a.r[i] - b.r[i])));
    return m;
}

inline float maxStep (const std::vector<float>& x, size_t from, size_t to)
{
    float m = 0.0f;
    for (size_t i = std::max<size_t> (from, 1); i < to; ++i)
        m = std::max (m, std::abs (x[i] - x[i - 1]));
    return m;
}

/** Power spectrum (|X|^2) with a 4-term Blackman-Harris window (sidelobes -92 dB). */
inline std::vector<double> powerSpectrumBH (const float* x, int order)
{
    const auto n = static_cast<size_t> (1) << order;
    std::vector<float> w (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double a = 2.0 * 3.14159265358979323846 * static_cast<double> (i) / static_cast<double> (n);
        const double win = 0.35875 - 0.48829 * std::cos (a) + 0.14128 * std::cos (2 * a) - 0.01168 * std::cos (3 * a);
        w[i] = static_cast<float> (win) * x[i];
    }
    const auto mag = vs::test::magnitudeSpectrum (w, order);
    std::vector<double> p (mag.size());
    for (size_t i = 0; i < mag.size(); ++i)
        p[i] = static_cast<double> (mag[i]) * static_cast<double> (mag[i]);
    return p;
}

inline int orderFor (size_t minSamples)
{
    int o = 1;
    while ((static_cast<size_t> (1) << o) < minSamples)
        ++o;
    return o;
}

// ---------------------------------------------------------------------------------------------
// Common requirement checks
// ---------------------------------------------------------------------------------------------

/** Same output for 512-sample, 1-sample and random-size blocks (params set before every block). */
template <typename Fx, typename Params>
void checkBlockSizeIndependence (const Params& p, double fs = 48000.0, double seconds = 1.5)
{
    const auto n = static_cast<size_t> (seconds * fs);
    const Stereo in = noise (n, 0.5f, 1234u);

    Stereo ref = in;
    Fx a;
    a.prepare (fs, kMaxBlock);
    runConst (a, ref, p, kMaxBlock);

    Stereo one = in;
    Fx b;
    b.prepare (fs, kMaxBlock);
    runConst (b, one, p, 1);

    Stereo rnd = in;
    Fx c;
    c.prepare (fs, kMaxBlock);
    std::uint32_t st = 99u;
    run (c, rnd, [&st] (int) { st = st * 1664525u + 1013904223u; return 1 + static_cast<int> ((st >> 8) % kMaxBlock); },
         [&p] (Fx& f, size_t) { f.setParams (p); });

    const float d1 = maxAbsDiff (ref, one), d2 = maxAbsDiff (ref, rnd);
    INFO ("max diff 512 vs 1: " << d1 << ", 512 vs random: " << d2);
    CHECK (d1 <= 1.0e-6f);
    CHECK (d2 <= 1.0e-6f);
}

/**
    Block-size independence with parameter changes (mode/level toggles, glides): the script gives
    the params per 512-sample segment, and setParams is called at the start of every block. Block
    patterns 512, 1 and random sizes that never cross a segment boundary, so every pattern sees
    each change at the same sample position (repeated identical setParams calls are no-ops).
*/
template <typename Fx, typename Script>
void checkBlockSizeIndependenceScripted (Script&& paramsForSegment, double fs = 48000.0, double seconds = 2.2)
{
    constexpr size_t seg = 512;
    const auto n = static_cast<size_t> (seconds * fs);
    const Stereo in = noise (n, 0.5f, 4321u);
    auto render = [&] (int pattern) {
        Stereo io = in;
        Fx fx;
        fx.prepare (fs, kMaxBlock);
        std::uint32_t st = 77u;
        size_t pos = 0;
        while (pos < n)
        {
            const size_t toBoundary = seg - pos % seg;
            size_t len = pattern == 0 ? seg : 1;
            if (pattern == 2)
            {
                st = st * 1664525u + 1013904223u;
                len = 1 + (st >> 8) % seg;
            }
            len = std::min ({ len, toBoundary, n - pos });
            fx.setParams (paramsForSegment (pos / seg));
            fx.process (io.l.data() + pos, io.r.data() + pos, static_cast<int> (len));
            pos += len;
        }
        return io;
    };
    const Stereo a = render (0), b = render (1), c = render (2);
    const float d1 = maxAbsDiff (a, b), d2 = maxAbsDiff (a, c);
    INFO ("max diff 512 vs 1: " << d1 << ", 512 vs random: " << d2);
    CHECK (d1 <= 1.0e-6f);
    CHECK (d2 <= 1.0e-6f);
}

/** Level/mix 0 (or Off) from the start, and after switching off: output == input bit for bit. */
template <typename Fx, typename Params>
void checkBitExactBypass (const Params& on, const Params& off, double fs = 48000.0)
{
    const auto n = static_cast<size_t> (fs);
    const Stereo in = noise (n, 0.7f, 77u);

    { // off from the start
        Stereo io = in;
        Fx fx;
        fx.prepare (fs, kMaxBlock);
        runConst (fx, io, off, 333);
        CHECK (std::memcmp (io.l.data(), in.l.data(), n * sizeof (float)) == 0);
        CHECK (std::memcmp (io.r.data(), in.r.data(), n * sizeof (float)) == 0);
    }
    { // on for 0.5 s, then off: after the 20 ms fade the signal passes untouched
        Stereo io = in;
        Fx fx;
        fx.prepare (fs, kMaxBlock);
        const size_t switchAt = n / 2;
        run (fx, io, [] (int) { return 256; }, [&] (Fx& f, size_t pos) { f.setParams (pos < switchAt ? on : off); });
        const size_t from = switchAt + static_cast<size_t> (0.03 * fs);
        CHECK (std::memcmp (io.l.data() + from, in.l.data() + from, (n - from) * sizeof (float)) == 0);
        CHECK (std::memcmp (io.r.data() + from, in.r.data() + from, (n - from) * sizeof (float)) == 0);
        CHECK (fx.isIdle());
    }
}

/**
    Switching on and off never clicks. Input: a 100 Hz sine (|step| <= 0.0065 per sample at 0.5 amp).
    A click is a jump far larger than the signal's own slope; we allow 1.5x the largest step seen
    in the steady "on" state (which includes the wet signal) and the dry signal.
*/
template <typename Fx, typename Params>
void checkClickFreeToggle (const Params& on, const Params& off, double fs = 48000.0)
{
    const auto n = static_cast<size_t> (2.0 * fs);
    Stereo io = sine (n, 100.0, fs, 0.5f);
    const Stereo dry = io;
    Fx fx;
    fx.prepare (fs, kMaxBlock);
    const size_t onAt = n / 4, offAt = 3 * n / 4;
    run (fx, io, [] (int) { return 64; }, [&] (Fx& f, size_t pos) { f.setParams (pos >= onAt && pos < offAt ? on : off); });

    const float steadyOn = std::max (maxStep (io.l, onAt + n / 8, offAt), maxStep (io.r, onAt + n / 8, offAt));
    const float dryStep = maxStep (dry.l, 0, n);
    const float limit = 1.5f * std::max (steadyOn, dryStep);
    const float aroundOn = std::max (maxStep (io.l, onAt - 64, onAt + n / 8), maxStep (io.r, onAt - 64, onAt + n / 8));
    const float aroundOff = std::max (maxStep (io.l, offAt - 64, n), maxStep (io.r, offAt - 64, n));
    INFO ("steady max step " << steadyOn << ", dry " << dryStep << ", around on " << aroundOn << ", around off " << aroundOff);
    CHECK (aroundOn <= limit);
    CHECK (aroundOff <= limit);
}

/**
    Click-free with content in the lines: a 50 Hz sine fills the effect (1.5 s), then it is
    switched off and back on 10 ms later (inside the 20 ms fade-out, lines still full), and later
    the level/mix steps between two non-zero values (on -> low -> on). Metric: the largest second
    difference |y[n] - 2 y[n-1] + y[n-2]|, which for a 50 Hz sine is only A w^2 (about 2e-5) while a
    jump of size J adds about J. The 20 ms gain ramps are linear, so where a ramp starts, ends or
    reverses the gain slope changes by at most 2/N per sample (N = ramp length); fade and level
    ramps can coincide, so the allowance for these slope kinks (not clicks) is 4 max|wet| / N.
    Limit per event: 1.5x the steady-state value (full, low, dry) + that allowance. Verified: with
    instant ramps all three effects fail it.
*/
template <typename Fx, typename Params>
void checkClickFreeWithContent (const Params& on, const Params& off, const Params& low, double fs = 48000.0)
{
    const auto n = static_cast<size_t> (4.0 * fs);
    Stereo io = sine (n, 50.0, fs, 0.5f);
    const Stereo dry = io;
    const auto offAt = static_cast<size_t> (1.5 * fs), backOn = offAt + 448; // 64-sample blocks
    const auto lowAt = static_cast<size_t> (2.5 * fs), highAt = static_cast<size_t> (3.2 * fs);
    Fx fx;
    fx.prepare (fs, kMaxBlock);
    run (fx, io, [] (int) { return 64; }, [&] (Fx& f, size_t pos) {
        if (pos >= offAt && pos < backOn)
            f.setParams (off);
        else if (pos >= lowAt && pos < highAt)
            f.setParams (low);
        else
            f.setParams (on);
    });
    auto d2 = [] (const std::vector<float>& x, size_t a, size_t b) {
        float m = 0.0f;
        for (size_t i = std::max<size_t> (a, 2); i < b; ++i)
            m = std::max (m, std::abs (x[i] - 2.0f * x[i - 1] + x[i - 2]));
        return m;
    };
    auto step = [&] (size_t a, size_t b) { return std::max (d2 (io.l, a, b), d2 (io.r, a, b)); };
    const float steady = std::max ({ step (offAt - static_cast<size_t> (0.4 * fs), offAt - 64),
                                     step (highAt - static_cast<size_t> (0.3 * fs), highAt - 64),
                                     step (n - static_cast<size_t> (0.3 * fs), n), d2 (dry.l, 0, n) });
    const auto win = static_cast<size_t> (0.1 * fs);
    const float atOff = step (offAt - 64, backOn), atOn = step (backOn - 64, backOn + win);
    const float atLow = step (lowAt - 64, lowAt + win), atHigh = step (highAt - 64, highAt + win);
    float maxWet = 0.0f;
    for (size_t i = 0; i < n; ++i)
        maxWet = std::max ({ maxWet, std::abs (io.l[i] - dry.l[i]), std::abs (io.r[i] - dry.r[i]) });
    const float limit = 1.5f * steady + 4.0f * maxWet / static_cast<float> (0.02 * fs);
    WARN ("max second difference: steady " << steady << ", switch off " << atOff << ", back on during fade " << atOn
                              << ", level down " << atLow << ", level up " << atHigh << "; limit " << limit
                              << " (max |wet| " << maxWet << ")");
    CHECK (atOff <= limit);
    CHECK (atOn <= limit);
    CHECK (atLow <= limit);
    CHECK (atHigh <= limit);
}

/** A burst, then silence, without flush-to-zero: no subnormal outputs, and the tail decays. */
template <typename Fx, typename Params>
void checkSilenceTail (const Params& p, double fs, double burstSeconds, double silenceSeconds, bool expectExactZero)
{
    const auto nb = static_cast<size_t> (burstSeconds * fs);
    const auto n = nb + static_cast<size_t> (silenceSeconds * fs);
    Stereo io (n);
    const Stereo burst = noise (nb, 0.5f, 5u);
    std::copy (burst.l.begin(), burst.l.end(), io.l.begin());
    std::copy (burst.r.begin(), burst.r.end(), io.r.begin());

    Fx fx;
    fx.prepare (fs, kMaxBlock);
    runConst (fx, io, p, 480);

    CHECK_FALSE (anySubnormal (io));
    const size_t q = (n - nb) / 4;
    const double early = std::max (vs::test::rms (io.l.data() + nb, q), vs::test::rms (io.r.data() + nb, q));
    const double late = std::max (vs::test::rms (io.l.data() + n - q, q), vs::test::rms (io.r.data() + n - q, q));
    INFO ("tail rms first quarter " << vs::test::toDb (early) << " dB, last quarter " << vs::test::toDb (late) << " dB");
    CHECK (late < early);
    if (expectExactZero)
        CHECK (late <= 0.0);
}

/** Random parameter jumps (including NaN, Inf and out-of-range values) with hot noise and impulses. */
template <typename Fx, typename RandomParams>
void checkExtremeParams (RandomParams&& randomParams, float bound, double fs, double seconds)
{
    const auto n = static_cast<size_t> (seconds * fs);
    Stereo io = noise (n, 1.0f, 4242u);
    for (size_t i = 0; i < n; i += 9973)
        io.l[i] = io.r[i] = 1.0f; // impulses
    Fx fx;
    fx.prepare (fs, kMaxBlock);
    std::uint32_t st = 2024u;
    auto rnd = [&st] { st = st * 1664525u + 1013904223u; return static_cast<float> (st >> 8) / 16777216.0f; };
    run (fx, io, [&rnd] (int) { return 1 + static_cast<int> (rnd() * kMaxBlock); },
         [&] (Fx& f, size_t) { f.setParams (randomParams (rnd)); });
    float peak = 0.0f;
    const bool ok = allFinite (io, bound, &peak);
    INFO ("peak " << peak << " (bound " << bound << ")");
    CHECK (ok);
}

/** After going idle (stale content is cleared incrementally while idle), waking up again, either
    right away or after a long idle period, must not replay old content: silent input stays silent. */
template <typename Fx, typename Params>
void checkNoStaleContentAfterWake (const Params& on, const Params& off, double fs = 48000.0)
{
    for (const int idleBlocks : { 1, 3, 2000 })
    {
        Fx fx;
        fx.prepare (fs, kMaxBlock);
        Stereo burst = noise (static_cast<size_t> (0.3 * fs), 0.8f, 9u);
        runConst (fx, burst, on, 256); // fill the lines
        Stereo quiet (static_cast<size_t> (0.05 * fs));
        runConst (fx, quiet, off, 256); // fade out -> idle
        CHECK (fx.isIdle());
        float buf[2 * 256] {};
        for (int b = 0; b < idleBlocks; ++b) // idle blocks
        {
            fx.setParams (off);
            fx.process (buf, buf + 256, 256);
        }
        Stereo after (static_cast<size_t> (2.5 * fs));
        runConst (fx, after, on, 256);
        float peak = 0.0f;
        allFinite (after, 1.0e9f, &peak);
        INFO ("idle blocks " << idleBlocks << ": peak after waking " << peak);
        CHECK (peak <= 0.0f);
    }
    { // reset() while active: nothing from before the reset may come back
        Fx fx;
        fx.prepare (fs, kMaxBlock);
        Stereo burst = noise (static_cast<size_t> (0.3 * fs), 0.8f, 10u);
        runConst (fx, burst, on, 256);
        fx.reset();
        Stereo after (static_cast<size_t> (2.5 * fs));
        runConst (fx, after, on, 256);
        float peak = 0.0f;
        allFinite (after, 1.0e9f, &peak);
        INFO ("peak after reset() " << peak);
        CHECK (peak <= 0.0f);
    }
}

/**
    Structural real-time cost: the only buffer writes are DelayLine::push calls inside process(),
    at most Fx::kLinesWritten per processed sample. setParams() (including waking up) and reset()
    write nothing, i.e. there is no bulk clearing anywhere on the audio thread. Exercised with
    random block sizes (1..512), random on/off toggles (including waking during the fade-out and
    right after going idle) and random resets.
*/
template <typename Fx, typename Params>
void checkBufferWritesBounded (const Params& on, const Params& off, double fs = 48000.0)
{
    Fx fx;
    fx.prepare (fs, kMaxBlock);
    std::uint32_t st = 555u;
    auto rnd = [&st] { st = st * 1664525u + 1013904223u; return static_cast<float> (st >> 8) / 16777216.0f; };
    std::vector<float> l (kMaxBlock), r (kMaxBlock);
    std::uint64_t setParamsWrites = 0, resetWrites = 0;
    double maxRatio = 0.0;
    for (int b = 0; b < 4000; ++b)
    {
        const int n = 1 + static_cast<int> (rnd() * (kMaxBlock - 1));
        for (int i = 0; i < n; ++i)
        {
            l[static_cast<size_t> (i)] = rnd() - 0.5f;
            r[static_cast<size_t> (i)] = rnd() - 0.5f;
        }
        auto w0 = fx.bufferWrites();
        fx.setParams (rnd() < 0.7f ? on : off);
        setParamsWrites += fx.bufferWrites() - w0;
        if (rnd() < 0.01f)
        {
            w0 = fx.bufferWrites();
            fx.reset();
            resetWrites += fx.bufferWrites() - w0;
        }
        w0 = fx.bufferWrites();
        fx.process (l.data(), r.data(), n);
        const auto d = fx.bufferWrites() - w0;
        maxRatio = std::max (maxRatio, static_cast<double> (d) / static_cast<double> (n));
    }
    INFO ("max buffer writes per processed sample " << maxRatio << " (limit " << Fx::kLinesWritten << "), in setParams "
                                                     << setParamsWrites << ", in reset " << resetWrites);
    CHECK (maxRatio <= static_cast<double> (Fx::kLinesWritten));
    CHECK (setParamsWrites == 0u);
    CHECK (resetWrites == 0u);
}

/**
    Non-finite input while active (NaN, +Inf, -Inf, 1e30 within one block, on both channels) must
    not poison the effect: the output stays finite and is bit-identical to the output for the same
    input with zeros in place of the bad samples, i.e. it recovers to the normal response without
    the effect being toggled.
*/
template <typename Fx, typename Params>
void checkNonFiniteInputRecovers (const Params& on, double fs = 48000.0)
{
    const auto n = static_cast<size_t> (3.0 * fs);
    Stereo clean = sine (n, 220.0, fs, 0.5f);
    for (size_t i = 0; i < n; ++i)
        clean.r[i] *= 0.8f;
    Stereo bad = clean;
    const size_t at = static_cast<size_t> (0.5 * fs) + 7;
    const float poison[] { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(), 1.0e30f };
    for (size_t k = 0; k < 4; ++k)
    {
        bad.l[at + 3 * k] = poison[k];
        bad.r[at + 3 * k + 1] = poison[(k + 1) % 4];
        clean.l[at + 3 * k] = 0.0f;
        clean.r[at + 3 * k + 1] = 0.0f;
    }
    Fx a, b;
    a.prepare (fs, kMaxBlock);
    b.prepare (fs, kMaxBlock);
    runConst (a, bad, on, 256);
    runConst (b, clean, on, 256);
    float peak = 0.0f;
    CHECK (allFinite (bad, 1.0e3f, &peak));
    CHECK (std::memcmp (bad.l.data(), clean.l.data(), n * sizeof (float)) == 0);
    CHECK (std::memcmp (bad.r.data(), clean.r.data(), n * sizeof (float)) == 0);
}

/** Pick a "wild" value: in range, slightly out of range, far out of range, NaN or +-Inf. */
template <typename Rnd>
float wild (Rnd& rnd, float lo, float hi)
{
    const float u = rnd();
    const float v = rnd();
    if (u < 0.6f) return lo + v * (hi - lo);
    if (u < 0.7f) return v < 0.5f ? lo : hi;
    if (u < 0.8f) return v < 0.5f ? -1.0e6f : 1.0e6f;
    if (u < 0.9f) return std::numeric_limits<float>::quiet_NaN();
    return v < 0.5f ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
}
} // namespace vs::fxtest
