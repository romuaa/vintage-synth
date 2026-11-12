#include "FxTestHelpers.h"
#include "fx/Chorus.h"

using namespace vs::fx;
using namespace vs::fxtest;

namespace
{
const float kDefaultMix = ChorusParams {}.mix; // reproduces the original maximum (wet 0.45)
const float kMixes[] { kDefaultMix, 1.0f };     // every behavioural test runs at both

ChorusParams cp (ChorusMode m, float mix = kDefaultMix) { return { m, mix }; }

/** Wet-path artefact level for a 1 kHz sine: power outside 1 kHz +-100 Hz (the Doppler sidebands
    of the modulation are within +-20 Hz) relative to the power inside, on (out - dry). */
double chorusArtefactDb (ChorusMode mode, float mix, double fs, bool leftChannel)
{
    const int order = orderFor (static_cast<size_t> (2.5 * fs));
    const auto nfft = static_cast<size_t> (1) << order;
    const size_t skip = static_cast<size_t> (0.2 * fs);
    Stereo io = sine (skip + nfft, 1000.0, fs, 0.5f);
    const Stereo dry = io;
    Chorus c;
    c.prepare (fs, kMaxBlock);
    runConst (c, io, cp (mode, mix));

    std::vector<float> wet (nfft);
    for (size_t i = 0; i < nfft; ++i)
        wet[i] = leftChannel ? io.l[skip + i] - dry.l[skip + i] : io.r[skip + i] - dry.r[skip + i];
    const auto p = powerSpectrumBH (wet.data(), order);
    const double binHz = fs / static_cast<double> (nfft);
    double in = 0.0, out = 0.0;
    for (size_t k = 1; k < p.size(); ++k)
    {
        const double hz = static_cast<double> (k) * binHz;
        (std::abs (hz - 1000.0) <= 100.0 ? in : out) += p[k];
    }
    return 10.0 * std::log10 (std::max (out, 1.0e-30) / in);
}

/** Worst (lowest) short-term level of the mono sum (L+R)/2 relative to the dry sine, in dB,
    over 900..1100 Hz (937.5 and 1062.5 Hz are this design's worst case: there the 4 ms centre
    puts the tap difference in anti-phase with the dry signal; exactly 1 kHz is a lucky case). */
double worstMonoDipDb (ChorusMode mode, float mix, double& worstHz)
{
    const double fs = 48000.0;
    double worst = 1.0e9;
    for (const double hz : { 900.0, 937.5, 970.0, 1000.0, 1030.0, 1062.5, 1100.0 })
    {
        const auto n = static_cast<size_t> (5.0 * fs); // > 2 LFO periods of mode I
        Stereo io = sine (n, hz, fs, 0.5f);
        Chorus c;
        c.prepare (fs, kMaxBlock);
        runConst (c, io, cp (mode, mix));

        const auto win = static_cast<size_t> (std::lround (std::ceil (0.01 * hz) * fs / hz)); // >= 10 ms, whole periods
        const double dryRms = 0.5 / std::sqrt (2.0);
        std::vector<float> mono (win);
        for (size_t s = static_cast<size_t> (0.1 * fs); s + win <= n; s += win / 2)
        {
            for (size_t i = 0; i < win; ++i)
                mono[i] = 0.5f * (io.l[s + i] + io.r[s + i]);
            const double db = vs::test::toDb (vs::test::rms (mono.data(), win) / dryRms);
            if (db < worst)
            {
                worst = db;
                worstHz = hz;
            }
        }
    }
    return worst;
}
} // namespace

TEST_CASE ("Chorus: mix curve is monotonic, default reproduces the original maximum", "[fx][chorus]")
{
    // The documented curve g = mix^p (public constant): 0.45 at the default mix, 1 at mix 1.
    CHECK (std::abs (std::pow (kDefaultMix, Chorus::kMixExponent) - 0.45f) < 1.0e-4f);
    CHECK (std::abs (std::pow (1.0f, Chorus::kMixExponent) - 1.0f) < 1.0e-6f);
    float prev = -1.0f;
    for (int i = 0; i <= 100; ++i)
    {
        const float g = std::pow (static_cast<float> (i) / 100.0f, Chorus::kMixExponent);
        CHECK (g > prev);
        prev = g;
    }
}

TEST_CASE ("Chorus: output does not depend on the block size", "[fx][chorus]")
{
    for (const float mix : kMixes)
    {
        checkBlockSizeIndependence<Chorus> (cp (ChorusMode::I, mix));
        checkBlockSizeIndependence<Chorus> (cp (ChorusMode::II, mix), 96000.0, 0.8);
    }
}

TEST_CASE ("Chorus: Off and mix 0 are bit-exact bypasses", "[fx][chorus]")
{
    for (const float mix : kMixes)
    {
        checkBitExactBypass<Chorus> (cp (ChorusMode::I, mix), cp (ChorusMode::Off, mix));
        checkBitExactBypass<Chorus> (cp (ChorusMode::II, mix), cp (ChorusMode::II, 0.0f));
    }
}

TEST_CASE ("Chorus: switching on and off is click-free", "[fx][chorus]")
{
    for (const float mix : kMixes)
    {
        checkClickFreeToggle<Chorus> (cp (ChorusMode::I, mix), cp (ChorusMode::Off, mix));
        checkClickFreeToggle<Chorus> (cp (ChorusMode::II, mix), cp (ChorusMode::II, 0.0f));
    }
}

TEST_CASE ("Chorus: mode switch I <-> II is click-free", "[fx][chorus]")
{
    for (const float mix : kMixes)
    {
        const double fs = 48000.0;
        const auto n = static_cast<size_t> (3.0 * fs);
        Stereo io = sine (n, 100.0, fs, 0.5f);
        Chorus c;
        c.prepare (fs, kMaxBlock);
        const size_t a = n / 3, b = 2 * n / 3;
        run (c, io, [] (int) { return 128; }, [&] (Chorus& f, size_t pos) { f.setParams (cp (pos >= a && pos < b ? ChorusMode::II : ChorusMode::I, mix)); });

        const float steady = std::max ({ maxStep (io.l, n / 10, a - 1000), maxStep (io.r, n / 10, a - 1000),
                                         maxStep (io.l, a + n / 10, b - 1000), maxStep (io.r, a + n / 10, b - 1000) });
        const float atSwitch = std::max ({ maxStep (io.l, a - 64, a + 4800), maxStep (io.r, a - 64, a + 4800),
                                           maxStep (io.l, b - 64, b + 4800), maxStep (io.r, b - 64, b + 4800) });
        WARN ("chorus mix " << mix << " mode switch: max step at switch " << atSwitch << ", steady " << steady);
        CHECK (atSwitch <= 1.5f * steady);
    }
}

TEST_CASE ("Chorus: mono-compatible at the default mix (no dip deeper than 6 dB, 900-1100 Hz)", "[fx][chorus]")
{
    // A "dry + wet on both sides" chorus notches L+R to -inf around 1 kHz whenever the taps pass
    // half a period of delay. Gate at the default mix: never more than 6 dB below the dry level.
    for (const auto mode : { ChorusMode::I, ChorusMode::II })
    {
        double hz = 0.0;
        const double dip = worstMonoDipDb (mode, kDefaultMix, hz);
        WARN ("chorus mode " << static_cast<int> (mode) << " default mix: worst mono dip " << dip << " dB at " << hz << " Hz");
        CHECK (dip >= -6.0);
    }
}

TEST_CASE ("Chorus: mono sum at mix 1 dips but never cancels (bound -15 dB, 900-1100 Hz)", "[fx][chorus]")
{
    // At mix 1 the wet equals the dry level. The L/R-differing part of the wet is capped at 0.7 and
    // the output is scaled by k = 0.775, so the worst mono level is k (1 - 0.7 |H|) = about -12.5 dB
    // (|H| = wet filter gain at 1 kHz, about 0.99). Without the cap it would be a full null (-inf).
    // The -15 dB bound leaves 2.5 dB for the 10 ms measurement window and filter phase; a deep comb
    // notch (< -20 dB) still fails it.
    for (const auto mode : { ChorusMode::I, ChorusMode::II })
    {
        double hz = 0.0;
        const double dip = worstMonoDipDb (mode, 1.0f, hz);
        WARN ("chorus mode " << static_cast<int> (mode) << " mix 1: worst mono dip " << dip << " dB at " << hz << " Hz");
        CHECK (dip >= -15.0);
    }
}

TEST_CASE ("Chorus: peak gain for a full-scale sine sweep", "[fx][chorus]")
{
    // Log sweep 20 Hz..20 kHz at 0 dBFS (L = R), 20 s. The original maximum (wet 0.45) can peak at
    // 1 + 0.45 = +3.2 dB; mix 1 is loudness-held by k, so its theoretical peak is
    // 0.775 * (1 + 1) = +3.8 dB. Gate: never above +4.5 dB at any mix.
    const double fs = 48000.0;
    const auto n = static_cast<size_t> (20.0 * fs);
    Stereo sweep (n);
    const double f0 = 20.0, f1 = 20000.0, T = 20.0, kk = std::log (f1 / f0);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) / fs;
        sweep.l[i] = sweep.r[i] = static_cast<float> (std::sin (2.0 * 3.14159265358979323846 * f0 * T / kk * (std::exp (t * kk / T) - 1.0)));
    }
    for (const auto mode : { ChorusMode::I, ChorusMode::II })
        for (const float mix : { kDefaultMix, 0.75f, 1.0f })
        {
            Stereo io = sweep;
            Chorus c;
            c.prepare (fs, kMaxBlock);
            runConst (c, io, cp (mode, mix));
            float peak = 0.0f;
            allFinite (io, 1.0e9f, &peak);
            WARN ("chorus mode " << static_cast<int> (mode) << " mix " << mix << ": peak gain for a 0 dBFS sweep "
                                 << vs::test::toDb (peak) << " dB");
            CHECK (vs::test::toDb (peak) <= 4.5);
        }
}

TEST_CASE ("Chorus: delay modulation stays inside the delay line", "[fx][chorus]")
{
    for (const double fs : kRates)
    {
        Chorus c;
        c.prepare (fs, kMaxBlock);
        const auto cap = static_cast<float> (c.lineCapacity());
        float lo = 1.0e9f, hi = -1.0e9f;
        float buf[2] { 0.1f, 0.1f };
        const auto n = static_cast<size_t> (4.5 * fs);
        for (size_t i = 0; i < n; ++i)
        {
            // Modes alternate every 0.7 s to include the glides between them.
            c.setParams (cp ((i / static_cast<size_t> (0.7 * fs)) % 2 == 0 ? ChorusMode::I : ChorusMode::II, 1.0f));
            for (const float d : { c.delaySamplesA(), c.delaySamplesB() })
            {
                lo = std::min (lo, d);
                hi = std::max (hi, d);
            }
            c.process (buf, buf + 1, 1);
        }
        INFO ("fs " << fs << ": delay range " << lo << " .. " << hi << " samples, capacity " << cap);
        CHECK (lo >= 1.0f);
        CHECK (hi <= cap - 3.0f);
        CHECK (lo >= static_cast<float> (2.0e-3 * fs)); // never below 2 ms (a chorus, not a flanger)
    }
}

TEST_CASE ("Chorus: interpolation artefacts <= -80 dB for a 1 kHz sine", "[fx][chorus]")
{
    for (const float mix : kMixes)
        for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
            for (const auto mode : { ChorusMode::I, ChorusMode::II })
            {
                const double l = chorusArtefactDb (mode, mix, fs, true);
                const double r = chorusArtefactDb (mode, mix, fs, false);
                WARN ("chorus mix " << mix << " fs " << fs << " mode " << static_cast<int> (mode) << ": artefacts L " << l << " dB, R " << r << " dB");
                CHECK (l <= -80.0);
                CHECK (r <= -80.0);
            }
}

TEST_CASE ("Chorus: all sample rates, bounded and finite", "[fx][chorus]")
{
    for (const float mix : kMixes)
        for (const double fs : kRates)
        {
            Stereo io = noise (static_cast<size_t> (0.5 * fs), 1.0f, 3u);
            Chorus c;
            c.prepare (fs, kMaxBlock);
            runConst (c, io, cp (ChorusMode::I, mix), 300);
            float peak = 0.0f;
            INFO ("fs " << fs << " mix " << mix);
            CHECK (allFinite (io, 2.5f, &peak));
        }
}

TEST_CASE ("Chorus: silence after a burst decays to exactly zero, no subnormals", "[fx][chorus]")
{
    for (const float mix : kMixes)
        checkSilenceTail<Chorus> (cp (ChorusMode::I, mix), 48000.0, 0.2, 1.0, true);
}

TEST_CASE ("Chorus: extreme and fast-changing parameters stay finite", "[fx][chorus]")
{
    checkExtremeParams<Chorus> (
        [] (auto& rnd) {
            const float u = rnd();
            const auto mode = u < 0.33f ? ChorusMode::Off : (u < 0.66f ? ChorusMode::I : ChorusMode::II);
            return ChorusParams { mode, wild (rnd, 0.0f, 1.0f) };
        },
        3.0f, 48000.0, 3.0);
}

TEST_CASE ("Chorus: waking up after idle never replays stale content", "[fx][chorus]")
{
    for (const float mix : kMixes)
        checkNoStaleContentAfterWake<Chorus> (cp (ChorusMode::I, mix), cp (ChorusMode::Off, mix));
}

TEST_CASE ("Chorus: no bulk buffer work on the audio thread (writes only in process, bounded per sample)", "[fx][chorus]")
{
    checkBufferWritesBounded<Chorus> (cp (ChorusMode::I, 1.0f), cp (ChorusMode::Off, 1.0f));
}
