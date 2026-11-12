#include "FxTestHelpers.h"
#include "fx/Delay.h"

using namespace vs::fx;
using namespace vs::fxtest;

namespace
{
DelayParams dp (float time, float fb, float damping, bool pp, float level)
{
    DelayParams p;
    p.timeSeconds = time;
    p.feedback = fb;
    p.damping = damping;
    p.pingPong = pp;
    p.level = level;
    return p;
}

/** Index of the largest |x| in [centre - radius, centre + radius]. */
size_t peakNear (const std::vector<float>& x, double centre, int radius)
{
    const auto c = static_cast<std::int64_t> (std::llround (centre));
    size_t best = 0;
    float bestV = -1.0f;
    for (std::int64_t i = std::max<std::int64_t> (0, c - radius); i <= c + radius && i < static_cast<std::int64_t> (x.size()); ++i)
        if (std::abs (x[static_cast<size_t> (i)]) > bestV)
        {
            bestV = std::abs (x[static_cast<size_t> (i)]);
            best = static_cast<size_t> (i);
        }
    return best;
}
} // namespace

TEST_CASE ("Delay: output does not depend on the block size", "[fx][delay]")
{
    checkBlockSizeIndependence<Delay> (dp (0.0137f, 0.8f, 0.4f, false, 0.7f));
    checkBlockSizeIndependence<Delay> (dp (0.0291f, 0.6f, 0.0f, true, 1.0f), 96000.0, 0.8);
}

TEST_CASE ("Delay: level 0 is a bit-exact bypass", "[fx][delay]")
{
    checkBitExactBypass<Delay> (dp (0.05f, 0.7f, 0.3f, false, 0.8f), dp (0.05f, 0.7f, 0.3f, false, 0.0f));
}

TEST_CASE ("Delay: switching on and off is click-free", "[fx][delay]")
{
    checkClickFreeToggle<Delay> (dp (0.1f, 0.5f, 0.3f, false, 0.8f), dp (0.1f, 0.5f, 0.3f, false, 0.0f));
    checkClickFreeToggle<Delay> (dp (0.0573f, 0.7f, 0.0f, true, 1.0f), dp (0.0573f, 0.7f, 0.0f, true, 0.0f));
}

TEST_CASE ("Delay: ping-pong toggle is click-free", "[fx][delay]")
{
    const double fs = 48000.0;
    const auto n = static_cast<size_t> (4.0 * fs);
    Stereo io = sine (n, 100.0, fs, 0.5f);
    io.r = std::vector<float> (n, 0.0f); // left-only input makes the routing change audible
    Delay d;
    d.prepare (fs, kMaxBlock);
    const auto period = static_cast<size_t> (0.8 * fs);
    run (d, io, [] (int) { return 64; }, [&] (Delay& f, size_t pos) { f.setParams (dp (0.07f, 0.6f, 0.2f, (pos / period) % 2 == 1, 0.8f)); });

    const float steady = std::max (maxStep (io.l, period / 2, period), maxStep (io.r, period + period / 2, 2 * period));
    float worst = 0.0f;
    for (size_t k = 1; k < 5; ++k)
    {
        const size_t s = k * period;
        if (s + 9600 < n)
            worst = std::max ({ worst, maxStep (io.l, s - 64, s + 9600), maxStep (io.r, s - 64, s + 9600) });
    }
    WARN ("delay ping-pong toggle: max step " << worst << ", steady " << steady);
    CHECK (worst <= 1.5f * steady);
}

TEST_CASE ("Delay: echo spacing exact to +-1 sample", "[fx][delay]")
{
    for (const double fs : kRates)
        for (const float t : { 0.001f, 0.0104f, 0.25f, 0.6667f, 2.0f })
            for (const bool pp : { false, true })
            {
                const double T = static_cast<double> (t) * fs;
                const auto n = static_cast<size_t> (3.5 * T) + 64;
                Stereo io (n);
                io.l[0] = io.r[0] = 1.0f;
                Delay d;
                d.prepare (fs, kMaxBlock);
                runConst (d, io, dp (t, 0.5f, 0.0f, pp, 1.0f));
                io.l[0] = io.r[0] = 0.0f; // remove the dry impulse

                int worst = 0;
                for (int k = 1; k <= 3; ++k)
                {
                    const auto& ch = (pp && k % 2 == 0) ? io.r : io.l;
                    const auto pos = static_cast<std::int64_t> (peakNear (ch, k * T, 8));
                    worst = std::max (worst, static_cast<int> (std::abs (static_cast<double> (pos) - k * T) + 0.5));
                }
                INFO ("fs " << fs << " time " << t << " pp " << pp << " worst offset " << worst);
                CHECK (worst <= 1);
            }
}

TEST_CASE ("Delay: feedback decay per repeat matches the setting +-0.5 dB", "[fx][delay]")
{
    const double fs = 48000.0;
    SECTION ("impulse, damping 0")
    {
        for (const float fb : { 0.1f, 0.35f, 0.7f, 0.95f })
        {
            const int T = 4800;
            const auto n = static_cast<size_t> (6 * T + 10);
            Stereo io (n);
            io.l[0] = 1.0f;
            Delay d;
            d.prepare (fs, kMaxBlock);
            runConst (d, io, dp (static_cast<float> (T / fs), fb, 0.0f, false, 1.0f));
            double worst = 0.0;
            for (int k = 1; k < 5; ++k)
            {
                const double a = std::abs (io.l[static_cast<size_t> (k * T)]);
                const double b = std::abs (io.l[static_cast<size_t> ((k + 1) * T)]);
                worst = std::max (worst, std::abs (vs::test::toDb (b / a) - vs::test::toDb (fb)));
            }
            WARN ("delay fb " << fb << " (impulse): worst per-repeat error " << worst << " dB");
            CHECK (worst <= 0.5);
        }
    }
    SECTION ("200 Hz tone burst, damping 0.6")
    {
        for (const float fb : { 0.35f, 0.8f })
        {
            const auto T = static_cast<size_t> (0.25 * fs);
            const auto burst = static_cast<size_t> (0.02 * fs);
            const auto n = 7 * T;
            Stereo io (n);
            for (size_t i = 0; i < burst; ++i)
            {
                const double w = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * static_cast<double> (i) / static_cast<double> (burst));
                io.l[i] = static_cast<float> (0.5 * w * std::sin (2.0 * 3.14159265358979 * 200.0 * static_cast<double> (i) / fs));
            }
            Delay d;
            d.prepare (fs, kMaxBlock);
            runConst (d, io, dp (0.25f, fb, 0.6f, false, 1.0f));
            double worst = 0.0;
            for (size_t k = 1; k < 5; ++k)
            {
                const double a = vs::test::rms (io.l.data() + k * T, burst + 200);
                const double b = vs::test::rms (io.l.data() + (k + 1) * T, burst + 200);
                worst = std::max (worst, std::abs (vs::test::toDb (b / a) - vs::test::toDb (fb)));
            }
            WARN ("delay fb " << fb << " (200 Hz burst, damping 0.6): worst per-repeat error " << worst << " dB");
            CHECK (worst <= 0.5);
        }
    }
}

TEST_CASE ("Delay: stable at maximum feedback for 60 s of noise", "[fx][delay]")
{
    // fb 0.95 with a flat loop is the worst case: steady-state gain up to 1 / (1 - 0.95) = 20.
    const double fs = 48000.0;
    for (const bool pp : { false, true })
    {
        const auto n = static_cast<size_t> (60.0 * fs);
        Stereo io = noise (n, 0.5f, 11u);
        Delay d;
        d.prepare (fs, kMaxBlock);
        runConst (d, io, dp (0.0731f, 0.95f, 0.0f, pp, 1.0f));
        float peak = 0.0f;
        const bool ok = allFinite (io, 0.5f * 21.0f, &peak);
        const auto ten = static_cast<size_t> (10.0 * fs);
        const double rmsA = vs::test::rms (io.l.data() + ten, ten);
        const double rmsB = vs::test::rms (io.l.data() + n - ten, ten);
        WARN ("delay 60 s noise at fb 0.95 pp " << pp << ": peak " << peak << ", rms 10-20 s " << vs::test::toDb (rmsA)
                                                << " dB, last 10 s " << vs::test::toDb (rmsB) << " dB");
        CHECK (ok);
        CHECK (std::abs (vs::test::toDb (rmsB / rmsA)) < 0.5);
    }
}

TEST_CASE ("Delay: time sweeps and jumps glide without discontinuities", "[fx][delay]")
{
    // For a 440 Hz sine of amplitude A the second difference is A w^2. The glide bends the pitch of
    // the read head by at most 1.75x, so dry + one echo (feedback 0) stays below
    // A w^2 (1 + 1.75^2). A discontinuity of e in the read head adds about e (an unsmoothed
    // 64-sample time step here would add about 0.3). With feedback every echo passes the moving head
    // again (pitch 1.75^k, as on tape), so the continuity bound is checked at feedback 0 and the
    // feedback run is checked for boundedness only. The glide reaches the 1.75x limit exactly, so the
    // measured value sits right at the ideal bound; 5 % is allowed for interpolation ripple (still
    // 40x below a real discontinuity).
    const double fs = 48000.0;
    const double w = 2.0 * 3.14159265358979 * 440.0 / fs;
    const double bound = 1.05 * 0.5 * w * w * (1.0 + 1.75 * 1.75);
    const auto n = static_cast<size_t> (10.0 * fs);
    for (const float fb : { 0.0f, 0.4f })
    {
    Stereo io = sine (n, 440.0, fs, 0.5f);
    Delay d;
    d.prepare (fs, kMaxBlock);
    run (d, io, [] (int) { return 64; },
         [&] (Delay& f, size_t pos) {
             const double t = static_cast<double> (pos) / fs;
             float time = 0.05f;
             if (t < 3.0)
                 time = static_cast<float> (0.05 + 0.55 * t / 3.0); // sweep up in 64-sample steps
             else if (t < 5.0)
                 time = 0.1f;                                       // jump down
             else
                 time = 1.5f;                                       // big jump up
             f.setParams (dp (time, fb, 0.2f, false, 1.0f));
         });
    double worst = 0.0;
    for (size_t i = 2; i < n; ++i)
        worst = std::max (worst, static_cast<double> (std::abs (io.l[i] - 2.0f * io.l[i - 1] + io.l[i - 2])));
    float peak = 0.0f;
    const bool finite = allFinite (io, 0.5f * (1.0f + 1.0f / (1.0f - fb)), &peak);
    WARN ("delay sweep fb " << fb << ": max second difference " << worst << " (continuity bound " << bound << "), peak " << peak);
    CHECK (finite);
    if (fb <= 0.0f)
        CHECK (worst <= bound);
    CHECK (std::abs (d.currentDelaySamples() - 1.5 * fs) < 1.0e-9); // arrived exactly
    }
}

TEST_CASE ("Delay: all sample rates, bounded and finite", "[fx][delay]")
{
    for (const double fs : kRates)
    {
        Stereo io = noise (static_cast<size_t> (1.0 * fs), 1.0f, 3u);
        Delay d;
        d.prepare (fs, kMaxBlock);
        runConst (d, io, dp (0.0123f, 0.9f, 0.5f, true, 1.0f), 300);
        INFO ("fs " << fs);
        CHECK (allFinite (io, 21.0f));
    }
}

TEST_CASE ("Delay: silence after a burst decays, no subnormals", "[fx][delay]")
{
    checkSilenceTail<Delay> (dp (0.001f, 0.95f, 0.0f, false, 1.0f), 48000.0, 0.1, 2.0, true);
    checkSilenceTail<Delay> (dp (0.3f, 0.9f, 0.7f, true, 1.0f), 48000.0, 0.1, 3.0, false);
}

TEST_CASE ("Delay: tailSeconds covers the decay to -120 dBFS", "[fx][delay]")
{
    const double fs = 48000.0;
    const auto p = dp (0.05f, 0.8f, 0.0f, false, 1.0f);
    Delay d;
    d.prepare (fs, kMaxBlock);
    d.setParams (p);
    const double tail = d.tailSeconds();
    const auto nb = static_cast<size_t> (0.5 * fs);
    const auto n = nb + static_cast<size_t> (tail * fs);
    Stereo io (n);
    const Stereo burst = noise (nb, 1.0f, 8u);
    std::copy (burst.l.begin(), burst.l.end(), io.l.begin());
    std::copy (burst.r.begin(), burst.r.end(), io.r.begin());
    runConst (d, io, p);
    const auto last = static_cast<size_t> (0.05 * fs);
    float peak = 0.0f;
    for (size_t i = n - last; i < n; ++i)
        peak = std::max ({ peak, std::abs (io.l[i]), std::abs (io.r[i]) });
    WARN ("delay tailSeconds " << tail << " s; peak in the last 50 ms " << vs::test::toDb (peak) << " dBFS");
    CHECK (peak < 1.0e-6f);
}

TEST_CASE ("Delay: extreme and fast-changing parameters stay finite", "[fx][delay]")
{
    checkExtremeParams<Delay> (
        [] (auto& rnd) {
            DelayParams p;
            p.timeSeconds = wild (rnd, 0.001f, 2.0f);
            p.feedback = wild (rnd, 0.0f, 0.95f);
            p.damping = wild (rnd, 0.0f, 1.0f);
            p.pingPong = rnd() < 0.5f;
            p.level = wild (rnd, 0.0f, 1.0f);
            return p;
        },
        25.0f, 48000.0, 4.0);
}

TEST_CASE ("Delay: waking up after idle never replays stale content", "[fx][delay]")
{
    checkNoStaleContentAfterWake<Delay> (dp (0.4f, 0.8f, 0.2f, true, 1.0f), dp (0.4f, 0.8f, 0.2f, true, 0.0f));
}

TEST_CASE ("Delay: no bulk buffer work on the audio thread (writes only in process, bounded per sample)", "[fx][delay]")
{
    checkBufferWritesBounded<Delay> (dp (0.4f, 0.8f, 0.2f, true, 1.0f), dp (0.4f, 0.8f, 0.2f, true, 0.0f));
}
