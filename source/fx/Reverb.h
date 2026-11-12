#pragma once

// Algorithmic hall reverb: a 16-line feedback delay network (FDN), built for long, smooth pad tails.
//
//   in L/R ─► pre-delay ─► 4 allpass diffusers per side ─► inject (L: even lines, R: odd lines)
//                                                                 │
//      ┌──────────────── 16 delay lines (31..84 ms, primes) ◄─────┘
//      │      all slowly modulated (0.11..0.89 Hz, +-0.19..0.42 ms)
//      ▼
//   per-line absorption shelf (sets RT60 at 1 kHz exactly, damping shortens the highs)
//      ▼
//   16x16 Hadamard mix (orthogonal, lossless) ─► back into the lines
//   outputs: two orthogonal +-1 tap vectors over the 16 lines -> L and R (uncorrelated tails)
//
// Published techniques this follows (own implementation):
// - Feedback delay networks with an orthogonal (here Hadamard) feedback matrix and per-line
//   absorption proportional to the line length, so every mode decays at the same RT60
//   (J.-M. Jot and A. Chaigne, "Digital delay networks for designing artificial reverberators",
//   AES 90th Convention, 1991).
// - Schroeder allpass diffusers in front of the network (M. R. Schroeder, "Natural sounding
//   artificial reverberation", JAES 10(3), 1962).
// - Slowly modulated delay lengths to break up modal ringing (J. Dattorro, "Effect Design
//   Part 1: Reverberator and Other Filters", JAES 45(9), 1997).
//
// Absorption filter per line i: H = Glo * LP + Ghi * HP, with a complementary 1-pole TPT pair at
// 5 kHz. The LP and HP outputs are in quadrature, so |H(f)|^2 = Glo^2 |LP|^2 + Ghi^2 |HP|^2 exactly;
// Glo is solved so that |H(1 kHz)| equals the gain for the requested RT60 at 1 kHz (exact at every
// sample rate) and Ghi gives the high-frequency RT60 = decay * (1 - 0.85 * damping), limited for
// short, heavily damped settings so the high-frequency decay stays uniform (see computeAbsorption).
// Low frequencies then decay slightly slower than 1 kHz (at most about 1.3x RT60 at full damping),
// like a real hall.

#include "fx/FxCommon.h"

#include <array>

namespace vs::fx
{
struct ReverbParams
{
    float decaySeconds = 2.5f;    ///< RT60 at 1 kHz, 0.3..20 s
    float damping = 0.5f;         ///< 0..1: high-frequency RT60 = decay * (1 - 0.85 * damping)
    float preDelaySeconds = 0.02f; ///< 0..0.2 s
    float level = 0.0f;           ///< 0..1 wet gain next to the unchanged dry signal; 0 = bypass
};

class Reverb
{
public:
    static constexpr int kLines = 16;
    static constexpr float kMinDecay = 0.3f, kMaxDecay = 20.0f, kMaxPreDelay = 0.2f;
    static constexpr double kShelfHz = 5000.0;
    /// Wet output gain: at level 1 and decay 2.5 s a sustained input gives wet about 3 dB below dry.
    /// Wet loudness grows with the decay (energy is proportional to RT60): -13 dB at 0.5 s, +5 dB at 20 s.
    static constexpr float kOutGain = 1.75f;

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        fs = sampleRate;
        // Line lengths: roughly geometric 31..84 ms, rounded up to the next prime at this rate.
        static constexpr double lineMs[kLines] { 31.3, 33.9, 36.7, 39.1, 42.4, 45.0, 48.6, 51.7,
                                                 55.3, 58.1, 62.9, 66.4, 70.7, 74.6, 79.3, 83.9 };
        // Modulation of every line: depth (ms) and rate (Hz), all different. Every line is
        // modulated so the small high-frequency loss of the fractional (Lagrange) read is the same
        // in all lines; with only some lines modulated, high-frequency modes living in the
        // unmodulated lines outlast the rest and show up as narrow peaks in the tail.
        static constexpr double modMs[kLines] { 0.42, 0.19, 0.31, 0.36, 0.37, 0.24, 0.22, 0.33,
                                                0.35, 0.28, 0.27, 0.21, 0.40, 0.30, 0.25, 0.38 };
        static constexpr float modHz[kLines] { 0.11f, 0.83f, 0.53f, 0.23f, 0.29f, 0.67f, 0.73f, 0.13f,
                                               0.17f, 0.41f, 0.61f, 0.89f, 0.37f, 0.19f, 0.47f, 0.31f };

        for (int i = 0; i < kLines; ++i)
        {
            const auto idx = static_cast<std::size_t> (i);
            length[idx] = nextPrime (static_cast<int> (std::lround (lineMs[idx] * sampleRate / 1000.0)));
            modDepth[idx] = static_cast<float> (modMs[idx] * sampleRate / 1000.0);
            modInc[idx] = static_cast<double> (modHz[idx]) / sampleRate;
            lines[idx].resize (static_cast<std::size_t> (length[idx]) + static_cast<std::size_t> (std::ceil (modDepth[idx])) + 4);
        }

        static constexpr double apMsL[4] { 3.1, 4.7, 7.3, 11.3 };
        static constexpr double apMsR[4] { 3.4, 5.1, 7.9, 10.7 };
        for (std::size_t k = 0; k < 4; ++k)
        {
            apLenL[k] = static_cast<std::size_t> (std::lround (apMsL[k] * sampleRate / 1000.0));
            apLenR[k] = static_cast<std::size_t> (std::lround (apMsR[k] * sampleRate / 1000.0));
            apL[k].resize (apLenL[k]);
            apR[k].resize (apLenR[k]);
        }

        preL.resize (static_cast<std::size_t> (std::ceil (static_cast<double> (kMaxPreDelay) * sampleRate)) + 4);
        preR.resize (preL.capacity());

        shelfG = tptCoef (kShelfHz, sampleRate);
        const double x = std::tan (kPiD * 1000.0 / sampleRate) / std::tan (kPiD * kShelfHz / sampleRate);
        shelfX2 = x * x;
        // |LP(16 kHz)| of the shelf's low-pass (quadrature form), used to keep the HF decay uniform.
        const double x16 = std::tan (kPiD * std::min (16000.0, 0.45 * sampleRate) / sampleRate) / std::tan (kPiD * kShelfHz / sampleRate);
        lp16Db = -10.0 * std::log10 (1.0 + x16 * x16);
        maxLength = *std::max_element (length.begin(), length.end());

        coefSmooth = smoothCoef (0.05, sampleRate);
        preCoef = 1.0 - std::exp (-1.0 / (0.05 * sampleRate));
        const int rampLen = static_cast<int> (std::lround (0.02 * sampleRate));
        fade.setLength (rampLen);
        level.setLength (rampLen);
        lastDecay = lastDamping = -1.0f; // line lengths changed: recompute the absorption
        computeAbsorption (ReverbParams {}.decaySeconds, ReverbParams {}.damping);
        reset();
    }

    void reset() noexcept
    {
        clearState();
        fade.snap (0.0f);
        level.snap (0.0f);
        idle = true;
        snapNext = true;
    }

    void setParams (const ReverbParams& p) noexcept
    {
        const float lvl = sanitize (p.level, 0.0f, 1.0f);
        const bool active = lvl > 0.0f;
        const float decay = sanitize (p.decaySeconds, kMinDecay, kMaxDecay);
        const float damp = sanitize (p.damping, 0.0f, 1.0f);
        preTarget = std::max (1.0, static_cast<double> (sanitize (p.preDelaySeconds, 0.0f, kMaxPreDelay)) * fs);

        if (! sameValue (decay, lastDecay) || ! sameValue (damp, lastDamping))
        {
            lastDecay = decay;
            lastDamping = damp;
            computeAbsorption (decay, damp);
        }

        if (active && idle)
        {
            idle = false;
            snapNext = true;
        }
        if (snapNext)
        {
            glo = gloTarget;
            ghi = ghiTarget;
            pre = preTarget;
            snapNext = false;
        }
        fade.setTarget (active ? 1.0f : 0.0f);
        level.setTarget (lvl);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        if (idle)
            return; // bit-exact bypass: the audio is not touched

        constexpr float apG = 0.62f;
        constexpr float inScale = 0.35355339f; // 1 / sqrt(8): each side feeds 8 lines
        constexpr float hadScale = 0.25f;      // 1 / sqrt(16): orthonormal Hadamard
        // Output taps: two orthogonal +-1 vectors (dot product 0), so L and R tails are uncorrelated.
        static constexpr float tapL[kLines] { 1, 1, 1, 1, -1, -1, -1, -1, 1, 1, 1, 1, -1, -1, -1, -1 };
        static constexpr float tapR[kLines] { 1, -1, 1, -1, 1, -1, 1, -1, -1, 1, -1, 1, -1, 1, -1, 1 };
        static constexpr float inSign[kLines] { 1, 1, -1, 1, 1, -1, -1, -1, 1, -1, 1, 1, -1, -1, 1, -1 };

        for (int n = 0; n < numSamples; ++n)
        {
            const float f = fade.next();
            const float g = level.next() * f * kOutGain * hadScale;

            // Pre-delay (double-precision glide, slew-limited so the read head never reverses).
            const double diff = preTarget - pre;
            if (std::abs (diff) < 1.0e-3)
                pre = preTarget;
            else
                pre += std::clamp (diff * preCoef, -0.5, 0.5);

            const float dl = safeInput (left[n]), dr = safeInput (right[n]);
            preL.push (dl * f);
            preR.push (dr * f);
            float xl = preL.read (pre), xr = preR.read (pre);

            for (std::size_t k = 0; k < 4; ++k)
            {
                xl = allpass (apL[k], apLenL[k], xl, apG);
                xr = allpass (apR[k], apLenR[k], xr, apG);
            }
            xl *= inScale;
            xr *= inScale;

            // Read the network.
            std::array<float, kLines> y;
            for (std::size_t i = 0; i < static_cast<std::size_t> (kLines); ++i)
            {
                modPhase[i] += modInc[i]; // double: at 192 kHz a 0.11 Hz increment is ~10 float ulp
                if (modPhase[i] >= 1.0)
                    modPhase[i] -= 1.0;
                const float d = static_cast<float> (length[i] - 1) + modDepth[i] * fastSin01 (static_cast<float> (modPhase[i]));
                y[i] = lines[i].read (d);
            }

            // Absorption shelves (coefficients smoothed per sample).
            for (std::size_t i = 0; i < static_cast<std::size_t> (kLines); ++i)
            {
                glo[i] += (gloTarget[i] - glo[i]) * coefSmooth;
                ghi[i] += (ghiTarget[i] - ghi[i]) * coefSmooth;
                const float v = (y[i] - shelfState[i]) * shelfG;
                const float lp = v + shelfState[i];
                shelfState[i] = flushTiny (lp + v);
                y[i] = glo[i] * lp + ghi[i] * (y[i] - lp);
            }

            float outL = 0.0f, outR = 0.0f;
            for (std::size_t i = 0; i < static_cast<std::size_t> (kLines); ++i)
            {
                outL += tapL[i] * y[i];
                outR += tapR[i] * y[i];
            }

            hadamard16 (y);

            for (std::size_t i = 0; i < static_cast<std::size_t> (kLines); ++i)
            {
                const float in = (i & 1u) == 0 ? xl : xr;
                lines[i].push (flushTiny (y[i] * hadScale + inSign[i] * in));
            }

            left[n] = dl + g * outL;
            right[n] = dr + g * outR;

            if (f <= 0.0f && fade.isSettled())
            {
                idle = true;
                clearState(); // O(1) per line
                break;
            }
        }
    }

    /**
        Upper bound of (longest RT60 over frequency) / (RT60 at 1 kHz). The longest RT60 is at low
        frequencies: with Ghi set for the HF ratio q, |H(DC)| = Glo and per pass
        Glo^2 = g1^2 (1 + x^2) - Ghi^2 x^2, x = tan(pi 1 kHz/fs) / tan(pi 5 kHz/fs) <= 0.2 at any
        rate, which gives a ratio of about 1 / (1 - x^2 (1/q - 1)). The requested q is used (the
        HF limit only raises q, which lowers the ratio). Checked against the exact per-line value.
    */
    static double lowFrequencyRatioBound (const ReverbParams& p) noexcept
    {
        const double q = 1.0 - 0.85 * static_cast<double> (sanitize (p.damping, 0.0f, 1.0f));
        return 1.0 / (1.0 - 0.04 * (1.0 / q - 1.0));
    }

    /** Time for the tail to fall about 150 dB (2.5 x the longest RT60, which is at low frequencies).
        Pure function of the params (no object state, any thread). */
    static double tailSecondsUncapped (const ReverbParams& p) noexcept
    {
        const double decay = static_cast<double> (sanitize (p.decaySeconds, kMinDecay, kMaxDecay));
        const double pre = static_cast<double> (sanitize (p.preDelaySeconds, 0.0f, kMaxPreDelay));
        return pre + 2.5 * decay * lowFrequencyRatioBound (p) + 0.1;
    }

    /** Tail length reported to the host: the above, capped at kMaxReportedTailSeconds (30 s). */
    static double tailSeconds (const ReverbParams& p) noexcept { return std::min (kMaxReportedTailSeconds, tailSecondsUncapped (p)); }

    // ---- test hooks ----
    bool isIdle() const noexcept { return idle; }
    /** HF/1 kHz RT60 ratio actually used (the requested one, limited for uniform HF decay). */
    double hfRatio() const noexcept { return effectiveQ; }
    /** Exact (longest RT60) / (RT60 at 1 kHz) over the lines for the current settings. */
    double lowFrequencyRatio() const noexcept { return lowFreqRatio; }
    std::uint64_t bufferWrites() const noexcept
    {
        std::uint64_t n = 0;
        for (const auto& l : lines)
            n += l.writes();
        for (std::size_t k = 0; k < 4; ++k)
            n += apL[k].writes() + apR[k].writes();
        return n + preL.writes() + preR.writes();
    }
    static constexpr int kLinesWritten = kLines + 8 + 2; ///< buffer writes per processed sample, at most
    int lineLength (int i) const noexcept { return length[static_cast<std::size_t> (i)]; }

private:
    static int nextPrime (int n) noexcept
    {
        auto isPrime = [] (int v) {
            if (v < 2) return false;
            for (int d = 2; d * d <= v; ++d)
                if (v % d == 0) return false;
            return true;
        };
        while (! isPrime (n))
            ++n;
        return n;
    }

    static float allpass (DelayLine& line, std::size_t len, float x, float g) noexcept
    {
        // w[n] = x[n] + g w[n-D];  y[n] = w[n-D] - g w[n]
        const float wd = line.readInt (len - 1);
        const float w = x + g * wd;
        line.push (flushTiny (w));
        return wd - g * w;
    }

    static void hadamard16 (std::array<float, kLines>& v) noexcept
    {
        for (std::size_t h = 1; h < static_cast<std::size_t> (kLines); h <<= 1)
            for (std::size_t i = 0; i < static_cast<std::size_t> (kLines); i += h << 1)
                for (std::size_t j = i; j < i + h; ++j)
                {
                    const float a = v[j], b = v[j + h];
                    v[j] = a + b;
                    v[j + h] = a - b;
                }
    }

    void computeAbsorption (float decay, float damp) noexcept
    {
        // Requested HF/1 kHz RT60 ratio. Above the 5 kHz corner the shelf's low-pass branch only
        // rolls off at 6 dB/oct, so Glo |LP(f)| leaks past the intended high-band gain Ghi. That leak
        // is not proportional to the line length: if it dominates, the long lines outlast the short
        // ones at high frequencies and the late HF tail turns sparse and peaky (measured +7.5 dB at
        // 15.5 kHz for decay 1 s, damping 1). So q is limited such that the proportional Ghi term
        // stays at least 6 dB above the leak at 16 kHz for the longest line:
        //   A1/q >= A1 + lp16Db + 6  <=>  q >= 1 / (1 + (lp16Db + 6) / A1).
        // This only bites for short decays with heavy damping (e.g. q >= 0.37 at 1 s); from about
        // 4 s on the full range (q down to 0.15) is available.
        double q = 1.0 - 0.85 * static_cast<double> (damp);
        const double a1Max = -60.0 * static_cast<double> (maxLength) / (fs * static_cast<double> (decay));
        if (lp16Db + 6.0 < 0.0)
            q = std::max (q, 1.0 / (1.0 + (lp16Db + 6.0) / a1Max));
        effectiveQ = q;
        double ratio = 1.0;
        for (std::size_t i = 0; i < static_cast<std::size_t> (kLines); ++i)
        {
            const double a1 = -60.0 * static_cast<double> (length[i]) / (fs * static_cast<double> (decay)); // dB per pass at 1 kHz
            const double g1 = std::pow (10.0, a1 / 20.0);
            const double gh = std::pow (10.0, a1 / (20.0 * q));
            const double gl2 = g1 * g1 * (1.0 + shelfX2) - gh * gh * shelfX2;
            const double gl = std::min (0.99999, std::sqrt (std::max (gl2, 0.0)));
            gloTarget[i] = static_cast<float> (gl);
            ghiTarget[i] = static_cast<float> (gh);
            ratio = std::max (ratio, a1 / std::min (-1.0e-9, 20.0 * std::log10 (gl)));
        }
        lowFreqRatio = ratio;
    }

    template <typename Fn>
    void forEachLine (Fn&& fn) noexcept
    {
        for (auto& l : lines)
            fn (l);
        for (std::size_t k = 0; k < 4; ++k)
        {
            fn (apL[k]);
            fn (apR[k]);
        }
        fn (preL);
        fn (preR);
    }

    void resetSmallState() noexcept
    {
        shelfState.fill (0.0f);
        for (std::size_t i = 0; i < static_cast<std::size_t> (kLines); ++i)
        {
            modPhase[i] = static_cast<double> (i) * 0.137; // spread start phases (deterministic)
            modPhase[i] -= std::floor (modPhase[i]);
        }
    }

    /** O(1) per line: the lines forget their content (reads of older samples return 0). */
    void clearState() noexcept
    {
        forEachLine ([] (DelayLine& l) { l.clear(); });
        resetSmallState();
    }

    double fs = 48000.0;
    std::array<DelayLine, kLines> lines;
    std::array<int, kLines> length {};
    std::array<float, kLines> modDepth {};
    std::array<double, kLines> modInc {}, modPhase {};
    std::array<float, kLines> glo {}, ghi {}, gloTarget {}, ghiTarget {}, shelfState {};
    std::array<DelayLine, 4> apL, apR;
    std::array<std::size_t, 4> apLenL {}, apLenR {};
    DelayLine preL, preR;
    float shelfG = 0.0f, coefSmooth = 1.0f;
    double shelfX2 = 0.0, lp16Db = 0.0, effectiveQ = 1.0, preCoef = 1.0, pre = 1.0, preTarget = 1.0;
    int maxLength = 1;
    double lowFreqRatio = 1.0;
    float lastDecay = -1.0f, lastDamping = -1.0f;
    LinearRamp fade, level;
    bool idle = true, snapNext = true;
};
} // namespace vs::fx
