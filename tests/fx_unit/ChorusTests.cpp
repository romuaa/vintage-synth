#include "FxTestHelpers.h"
#include "fx/Chorus.h"

using namespace vs::fx;
using namespace vs::fxtest;

namespace
{
ChorusParams cp (ChorusMode m, float mix = 1.0f) { return { m, mix }; }

/** Wet-path artefact level for a 1 kHz sine: power outside 1 kHz +-100 Hz (the Doppler sidebands
    of the modulation are within +-20 Hz) relative to the power inside, on (out - dry). */
double chorusArtefactDb (ChorusMode mode, double fs, bool leftChannel)
{
    const int order = orderFor (static_cast<size_t> (2.5 * fs));
    const auto nfft = static_cast<size_t> (1) << order;
    const size_t skip = static_cast<size_t> (0.2 * fs);
    Stereo io = sine (skip + nfft, 1000.0, fs, 0.5f);
    const Stereo dry = io;
    Chorus c;
    c.prepare (fs, kMaxBlock);
    runConst (c, io, cp (mode));

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
} // namespace

TEST_CASE ("Chorus: output does not depend on the block size", "[fx][chorus]")
{
    checkBlockSizeIndependence<Chorus> (cp (ChorusMode::I));
    checkBlockSizeIndependence<Chorus> (cp (ChorusMode::II, 0.6f), 96000.0, 0.8);
}

TEST_CASE ("Chorus: Off and mix 0 are bit-exact bypasses", "[fx][chorus]")
{
    checkBitExactBypass<Chorus> (cp (ChorusMode::I), cp (ChorusMode::Off));
    checkBitExactBypass<Chorus> (cp (ChorusMode::II), cp (ChorusMode::II, 0.0f));
}

TEST_CASE ("Chorus: switching on and off is click-free", "[fx][chorus]")
{
    checkClickFreeToggle<Chorus> (cp (ChorusMode::I), cp (ChorusMode::Off));
    checkClickFreeToggle<Chorus> (cp (ChorusMode::II), cp (ChorusMode::II, 0.0f));
}

TEST_CASE ("Chorus: mode switch I <-> II is click-free", "[fx][chorus]")
{
    const double fs = 48000.0;
    const auto n = static_cast<size_t> (3.0 * fs);
    Stereo io = sine (n, 100.0, fs, 0.5f);
    Chorus c;
    c.prepare (fs, kMaxBlock);
    const size_t a = n / 3, b = 2 * n / 3;
    run (c, io, [] (int) { return 128; }, [&] (Chorus& f, size_t pos) { f.setParams (cp (pos >= a && pos < b ? ChorusMode::II : ChorusMode::I)); });

    const float steady = std::max (maxStep (io.l, n / 10, a - 1000), maxStep (io.l, a + n / 10, b - 1000));
    const float atSwitch = std::max ({ maxStep (io.l, a - 64, a + 4800), maxStep (io.r, a - 64, a + 4800),
                                       maxStep (io.l, b - 64, b + 4800), maxStep (io.r, b - 64, b + 4800) });
    WARN ("chorus mode switch: max step at switch " << atSwitch << ", steady " << steady);
    CHECK (atSwitch <= 1.5f * steady);
}

TEST_CASE ("Chorus: mono-compatible (L+R has no deep notches at 1 kHz)", "[fx][chorus]")
{
    // A "dry + wet on both sides" chorus notches L+R to -inf at 1 kHz whenever the taps pass
    // half a period of delay. Gate: the mono sum never dips more than 6 dB below the dry level.
    // Exactly 1 kHz is a lucky case for this design (the 4 ms centre is a whole number of periods,
    // so the tap difference is in quadrature with the dry signal); 937.5 and 1062.5 Hz put it in
    // anti-phase, which is the worst case. All of them are checked.
    const double fs = 48000.0;
    for (const double hz : { 1000.0, 937.5, 1062.5, 970.0, 300.0, 2500.0 })
    for (const auto mode : { ChorusMode::I, ChorusMode::II })
    {
        const auto n = static_cast<size_t> (5.0 * fs); // > 2 LFO periods of mode I
        Stereo io = sine (n, hz, fs, 0.5f);
        Chorus c;
        c.prepare (fs, kMaxBlock);
        runConst (c, io, cp (mode));

        const auto win = static_cast<size_t> (std::lround (std::ceil (0.01 * hz) * fs / hz)); // >= 10 ms, whole periods
        const double dryRms = 0.5 / std::sqrt (2.0);
        double minDb = 1.0e9, maxDb = -1.0e9;
        std::vector<float> mono (win);
        for (size_t s = static_cast<size_t> (0.1 * fs); s + win <= n; s += win / 2)
        {
            for (size_t i = 0; i < win; ++i)
                mono[i] = 0.5f * (io.l[s + i] + io.r[s + i]);
            const double db = vs::test::toDb (vs::test::rms (mono.data(), win) / dryRms);
            minDb = std::min (minDb, db);
            maxDb = std::max (maxDb, db);
        }
        WARN ("chorus mode " << static_cast<int> (mode) << " mono sum at " << hz << " Hz: min " << minDb << " dB, max " << maxDb << " dB re dry");
        CHECK (minDb >= -6.0);
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
            c.setParams (cp ((i / static_cast<size_t> (0.7 * fs)) % 2 == 0 ? ChorusMode::I : ChorusMode::II));
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
    for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const auto mode : { ChorusMode::I, ChorusMode::II })
        {
            const double l = chorusArtefactDb (mode, fs, true);
            const double r = chorusArtefactDb (mode, fs, false);
            WARN ("chorus fs " << fs << " mode " << static_cast<int> (mode) << ": artefacts L " << l << " dB, R " << r << " dB");
            CHECK (l <= -80.0);
            CHECK (r <= -80.0);
        }
}

TEST_CASE ("Chorus: all sample rates, bounded and finite", "[fx][chorus]")
{
    for (const double fs : kRates)
    {
        Stereo io = noise (static_cast<size_t> (0.5 * fs), 1.0f, 3u);
        Chorus c;
        c.prepare (fs, kMaxBlock);
        runConst (c, io, cp (ChorusMode::I), 300);
        float peak = 0.0f;
        INFO ("fs " << fs);
        CHECK (allFinite (io, 2.5f, &peak));
    }
}

TEST_CASE ("Chorus: silence after a burst decays to exactly zero, no subnormals", "[fx][chorus]")
{
    checkSilenceTail<Chorus> (cp (ChorusMode::I), 48000.0, 0.2, 1.0, true);
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
    checkNoStaleContentAfterWake<Chorus> (cp (ChorusMode::I), cp (ChorusMode::Off));
}
