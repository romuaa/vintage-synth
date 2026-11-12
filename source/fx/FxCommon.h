#pragma once

// Shared building blocks for the effect blocks (chorus, delay, reverb).
// Header-only, JUCE-free. Everything here is allocation-free after prepare/resize.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vs::fx
{
inline constexpr double kPiD = 3.14159265358979323846;
inline constexpr float kTwoPi = 6.28318530717958647692f;

/** Values with a smaller magnitude are flushed to zero in feedback paths, so decaying tails never
    reach the subnormal range (no denormal stalls on CPUs without flush-to-zero). -300 dBFS. */
inline constexpr float kFlushThreshold = 1.0e-15f;

/** Exact equality without -Wfloat-equal (false if either is NaN). */
inline bool sameValue (float a, float b) noexcept { return ! (a < b) && ! (a > b) && a == a && b == b; }

inline float flushTiny (float v) noexcept { return std::abs (v) < kFlushThreshold ? 0.0f : v; }

/** NaN-safe clamp: NaN (and anything not inside [lo, hi]) ends up at a bound. */
inline float sanitize (float v, float lo, float hi) noexcept
{
    if (! (v >= lo)) return lo; // also catches NaN
    if (! (v <= hi)) return hi;
    return v;
}

inline std::size_t nextPow2 (std::size_t n) noexcept
{
    std::size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

/** One-pole smoothing coefficient for time constant tau (seconds); computed in double for high rates. */
inline float smoothCoef (double tauSeconds, double sampleRate) noexcept
{
    return tauSeconds <= 0.0 ? 1.0f : static_cast<float> (1.0 - std::exp (-1.0 / (tauSeconds * sampleRate)));
}

/**
    Linear ramp that reaches its target exactly after a fixed number of samples.
    Used for gains that must arrive at exactly 0 (bit-exact bypass) or 1.
    Setting the same target again does not restart the ramp, so calling setTarget once per
    block gives the same result for any block size.
*/
class LinearRamp
{
public:
    void setLength (int samples) noexcept { length = samples < 1 ? 1 : samples; }

    void snap (float v) noexcept
    {
        current = target = v;
        steps = 0;
    }

    void setTarget (float v) noexcept
    {
        if (sameValue (v, target))
            return;
        target = v;
        steps = length;
        inc = (target - current) / static_cast<float> (length);
    }

    float next() noexcept
    {
        if (steps > 0)
        {
            current += inc;
            if (--steps == 0)
                current = target;
        }
        return current;
    }

    float value() const noexcept { return current; }
    float targetValue() const noexcept { return target; }
    bool isSettled() const noexcept { return steps == 0; }

private:
    float current = 0.0f, target = 0.0f, inc = 0.0f;
    int steps = 0, length = 1;
};

/** 4-point (3rd-order) Lagrange interpolation between y1 (t = 0) and y2 (t = 1); nodes at -1, 0, 1, 2.
    Exact for cubics: the error for a 1 kHz sine at 44.1 kHz is about -100 dB, and |H| <= 1. */
inline float lagrange4 (float y0, float y1, float y2, float y3, float t) noexcept
{
    const float tm1 = t - 1.0f, tm2 = t - 2.0f, tp1 = t + 1.0f;
    const float c0 = -t * tm1 * tm2 * (1.0f / 6.0f);
    const float c1 = tp1 * tm1 * tm2 * 0.5f;
    const float c2 = -tp1 * t * tm2 * 0.5f;
    const float c3 = tp1 * t * tm1 * (1.0f / 6.0f);
    return c0 * y0 + c1 * y1 + c2 * y2 + c3 * y3;
}

/**
    Circular delay line with a power-of-two buffer.

    Convention: push() writes the current sample; read(d) returns the sample written d pushes ago
    (read(0) = the sample just pushed). Fractional reads use 4-point Lagrange and need
    1 <= d <= capacity() - 3 when called after push(). Callers that read *before* pushing the
    current sample (feedback loops) pass d - 1.

    clear() is O(1): it does not touch the buffer. The line counts the samples pushed since the
    last clear (saturating at the capacity) and every read of an older position returns 0, so
    stale content is never heard and nothing is ever wiped in bulk on the audio thread.
*/
class DelayLine
{
public:
    /** Allocates. Call from prepare only. */
    void resize (std::size_t minSamples)
    {
        buffer.assign (nextPow2 (minSamples + 4), 0.0f);
        mask = buffer.size() - 1;
        writeIndex = 0;
        written = 0;
    }

    /** O(1): forget the content (reads of anything pushed before now return 0). */
    void clear() noexcept { written = 0; }

    std::size_t capacity() const noexcept { return buffer.size(); }

    void push (float x) noexcept
    {
        writeIndex = (writeIndex + 1) & mask;
        buffer[writeIndex] = x;
        if (written < buffer.size())
            ++written;
        ++pushCount;
    }

    float readInt (std::size_t d) const noexcept { return d < written ? buffer[(writeIndex - d) & mask] : 0.0f; }

    /** Double-precision position for long lines (float has a 0.03-sample ulp at 384000). */
    float read (double d) const noexcept
    {
        const auto n = static_cast<std::size_t> (d);
        return readAt (n, static_cast<float> (d - static_cast<double> (n)));
    }

    float read (float d) const noexcept
    {
        const auto n = static_cast<std::size_t> (d);
        return readAt (n, d - static_cast<float> (n));
    }

    /** Test hook: total number of samples ever written into the buffer (push is the only write). */
    std::uint64_t writes() const noexcept { return pushCount; }

private:
    float readAt (std::size_t n, float t) const noexcept
    {
        // Taps are n-1, n, n+1, n+2 pushes old.
        const std::size_t base = writeIndex - n;
        if (n + 2 < written) // fast path: all four taps written since the last clear
            return lagrange4 (buffer[(base + 1) & mask], buffer[base & mask], buffer[(base - 1) & mask], buffer[(base - 2) & mask], t);
        return lagrange4 (readInt (n - 1), readInt (n), readInt (n + 1), readInt (n + 2), t);
    }

    std::vector<float> buffer;
    std::size_t mask = 0, writeIndex = 0, written = 0;
    std::uint64_t pushCount = 0;
};

/** Prewarped TPT integrator coefficient G = g / (1 + g), g = tan(pi fc / fs). Clamped below Nyquist. */
inline float tptCoef (double fc, double sampleRate) noexcept
{
    const double f = std::min (fc, 0.49 * sampleRate);
    const double g = std::tan (kPiD * f / sampleRate);
    return static_cast<float> (g / (1.0 + g));
}

/** 1-pole TPT low-pass / high-pass pair (shared state), coefficient G from tptCoef. */
class OnePole
{
public:
    void reset() noexcept { s = 0.0f; }

    float lowPass (float x, float G) noexcept
    {
        const float v = (x - s) * G;
        const float lp = v + s;
        s = flushTiny (lp + v);
        return lp;
    }

    float highPass (float x, float G) noexcept { return x - lowPass (x, G); }

private:
    float s = 0.0f;
};

/** 2-pole TPT state-variable low-pass (Butterworth at k = sqrt 2), fixed coefficients. */
class SvfLowPass
{
public:
    void setup (double fc, double sampleRate, double q) noexcept
    {
        const double f = std::min (fc, 0.45 * sampleRate);
        const double g = std::tan (kPiD * f / sampleRate);
        const double k = 1.0 / q;
        a1 = static_cast<float> (1.0 / (1.0 + g * (g + k)));
        a2 = static_cast<float> (g) * a1;
        a3 = static_cast<float> (g) * a2;
    }

    void reset() noexcept { ic1 = ic2 = 0.0f; }

    float process (float x) noexcept
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = flushTiny (2.0f * v1 - ic1);
        ic2 = flushTiny (2.0f * v2 - ic2);
        return v2;
    }

private:
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, ic1 = 0.0f, ic2 = 0.0f;
};

/** Cheap sine for LFOs, phase in [0, 1). Max error about 0.001 (parabolic approximation with
    one refinement step); smooth (continuous first derivative). */
inline float fastSin01 (float phase) noexcept
{
    // Map to x in [-pi, pi).
    const float x = kTwoPi * (phase < 0.5f ? phase : phase - 1.0f);
    constexpr float B = 4.0f / 3.14159265358979f;
    constexpr float C = -4.0f / (3.14159265358979f * 3.14159265358979f);
    const float y = B * x + C * x * std::abs (x);
    return 0.225f * (y * std::abs (y) - y) + y;
}
} // namespace vs::fx
