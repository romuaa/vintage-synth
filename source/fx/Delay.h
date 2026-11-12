#pragma once

// Stereo feedback delay with optional ping-pong.
//
//   normal:     lineL <- inL + fb * damp(outL)          lineR <- inR + fb * damp(outR)
//   ping-pong:  lineL <- (inL + inR)/2 + fb * damp(outR) lineR <- fb * damp(outL)
//   out = dry + level * (outL, outR)
//
// - Time glides like a tape delay: two cascaded one-poles (75 ms each) on the delay in samples,
//   so the read speed itself changes smoothly (no kinks), slew-limited to 0.75 samples per
//   sample, so the read head never stops or reverses (pitch stays within 0.25x..1.75x during a
//   glide). No zipper steps, no clicks.
// - Reads use 4-point Lagrange interpolation. The time target is rounded to a whole number of
//   samples (echo spacing within +-0.5 sample of the setting), so once a glide has settled the read
//   sits exactly on a sample, where Lagrange is exact (weights 0, 1, 0, 0). A static delay is
//   therefore exactly flat and repeats never darken depending on time x sample rate. Only while
//   gliding are reads fractional (a half-sample read loses about 0.5 dB at 10 kHz at 48 kHz per pass).
// - Damping is a 1-pole low-pass in the feedback path, 20 kHz (damping 0, blended out so the
//   loop is exactly flat once the time has settled) down to 1.5 kHz (damping 1).
// - Normal <-> ping-pong is a 20 ms crossfade of the loop routing; the routing matrix
//   [[1-w, w], [w, 1-w]] * fb has eigenvalues fb and fb (1 - 2w), so the loop stays stable.
// - Level 0 fades out over 20 ms, then the delay goes idle (bit-exact bypass). Going idle and
//   reset() are O(1): the lines only forget their content (DelayLine::clear gates reads of
//   anything older), so no buffer is ever wiped on the audio thread. Waking up fades the line
//   input in as well, so the first echo has no hard onset.

#include "fx/FxCommon.h"

namespace vs::fx
{
struct DelayParams
{
    float timeSeconds = 0.375f; ///< 0.001..2 s
    float feedback = 0.35f;     ///< 0..0.95 (gain per repeat)
    float damping = 0.3f;       ///< 0..1: feedback low-pass 20 kHz (0, flat) .. 1.5 kHz (1)
    bool pingPong = false;
    float level = 0.0f;         ///< 0..1 wet gain next to the unchanged dry signal; 0 = bypass
};

class Delay
{
public:
    static constexpr float kMinTime = 0.001f, kMaxTime = 2.0f, kMaxFeedback = 0.95f;
    static constexpr float kMaxSlew = 0.75f; ///< max change of the delay, samples per sample

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        fs = sampleRate;
        lineL.resize (static_cast<std::size_t> (std::ceil (static_cast<double> (kMaxTime) * sampleRate)) + 8);
        lineR.resize (lineL.capacity());
        timeCoef = 1.0 - std::exp (-1.0 / (0.075 * sampleRate));
        paramCoef = smoothCoef (0.02, sampleRate);
        const int rampLen = static_cast<int> (std::lround (0.02 * sampleRate));
        fade.setLength (rampLen);
        level.setLength (rampLen);
        pingPong.setLength (rampLen);
        reset();
    }

    void reset() noexcept
    {
        lineL.clear();
        lineR.clear();
        dampL.reset();
        dampR.reset();
        fade.snap (0.0f);
        level.snap (0.0f);
        idle = true;
        snapNext = true;
    }

    void setParams (const DelayParams& p) noexcept
    {
        const float lvl = sanitize (p.level, 0.0f, 1.0f);
        const bool active = lvl > 0.0f;

        // Delay in samples measured from the current output sample (>= 2 so a read-before-write
        // Lagrange read has all four taps in the past), rounded to a whole sample (see above).
        const double t = static_cast<double> (sanitize (p.timeSeconds, kMinTime, kMaxTime));
        timeTarget = std::max (2.0, std::round (t * fs));
        fbTarget = sanitize (p.feedback, 0.0f, kMaxFeedback);
        const float damp = sanitize (p.damping, 0.0f, 1.0f);
        dampGTarget = tptCoef (20000.0 * std::pow (1500.0 / 20000.0, static_cast<double> (damp)), fs);
        dampMixTarget = std::min (1.0f, 20.0f * damp);

        if (active && idle)
        {
            idle = false;
            snapNext = true;
        }
        if (snapNext)
        {
            time = time1 = timeTarget;
            fb = fbTarget;
            dampG = dampGTarget;
            dampMix = dampMixTarget;
            pingPong.snap (p.pingPong ? 1.0f : 0.0f);
            snapNext = false;
        }
        pingPong.setTarget (p.pingPong ? 1.0f : 0.0f);
        fade.setTarget (active ? 1.0f : 0.0f);
        level.setTarget (lvl);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        if (idle)
            return; // bit-exact bypass: the audio is not touched

        for (int i = 0; i < numSamples; ++i)
        {
            const float f = fade.next();
            const float g = level.next() * f;
            const float w = pingPong.next();

            // Double precision: in float a 2 s delay (384000 samples at 192 kHz) has a 0.03-sample
            // ulp and a one-pole step would stall hundreds of samples short of the target.
            if (std::abs (timeTarget - time) < 1.0e-3 && std::abs (timeTarget - time1) < 1.0e-3)
            {
                time = time1 = timeTarget;
            }
            else
            {
                time1 += (timeTarget - time1) * timeCoef;
                time += std::clamp ((time1 - time) * timeCoef, -static_cast<double> (kMaxSlew), static_cast<double> (kMaxSlew));
            }
            fb += (fbTarget - fb) * paramCoef;
            dampG += (dampGTarget - dampG) * paramCoef;
            dampMix += (dampMixTarget - dampMix) * paramCoef;

            // Read before write: the sample written `time` samples ago is read (time - 1).
            const float yL = lineL.read (time - 1.0);
            const float yR = lineR.read (time - 1.0);

            const float lpL = dampL.lowPass (yL, dampG);
            const float lpR = dampR.lowPass (yR, dampG);
            const float fbL = fb * (yL + dampMix * (lpL - yL));
            const float fbR = fb * (yR + dampMix * (lpR - yR));

            const float dl = safeInput (left[i]), dr = safeInput (right[i]);
            const float inL = dl * f, inR = dr * f;
            const float mono = 0.5f * (inL + inR);
            const float w1 = 1.0f - w;
            lineL.push (flushTiny (w1 * (inL + fbL) + w * (mono + fbR)));
            lineR.push (flushTiny (w1 * (inR + fbR) + w * fbL));

            left[i] = dl + g * yL;
            right[i] = dr + g * yR;

            if (f <= 0.0f && fade.isSettled())
            {
                idle = true;
                lineL.clear(); // O(1)
                lineR.clear();
                dampL.reset();
                dampR.reset();
                break;
            }
        }
    }

    /** Time for the echoes to fall 140 dB below a sustained full-scale input (feedback build-up included). */
    double tailSeconds() const noexcept
    {
        const double t = timeTarget / fs;
        const double perRepeatDb = -20.0 * std::log10 (std::max (1.0e-6, static_cast<double> (fbTarget)));
        return t * (1.0 + 140.0 / std::max (perRepeatDb, 1.0e-3)) + 0.05;
    }

    // ---- test hooks ----
    double currentDelaySamples() const noexcept { return time; }
    bool isIdle() const noexcept { return idle; }
    std::uint64_t bufferWrites() const noexcept { return lineL.writes() + lineR.writes(); }
    static constexpr int kLinesWritten = 2; ///< buffer writes per processed sample, at most

private:
    double fs = 48000.0;
    DelayLine lineL, lineR;
    OnePole dampL, dampR;
    double timeCoef = 1.0, time = 2.0, time1 = 2.0, timeTarget = 2.0;
    float paramCoef = 1.0f, fb = 0.0f, fbTarget = 0.0f;
    float dampG = 0.0f, dampGTarget = 0.0f, dampMix = 0.0f, dampMixTarget = 0.0f;
    LinearRamp fade, level, pingPong;
    bool idle = true, snapNext = true;
};
} // namespace vs::fx
