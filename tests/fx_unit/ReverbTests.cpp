#include "FxTestHelpers.h"
#include "fx/Reverb.h"

using namespace vs::fx;
using namespace vs::fxtest;

namespace
{
ReverbParams rp (float decay, float damping, float preDelay, float level)
{
    ReverbParams p;
    p.decaySeconds = decay;
    p.damping = damping;
    p.preDelaySeconds = preDelay;
    p.level = level;
    return p;
}

/** Impulse response (wet only: the dry impulse at n = 0 is removed). */
Stereo impulseResponse (const ReverbParams& p, double fs, double seconds, bool stereoInput = true)
{
    Stereo io (static_cast<size_t> (seconds * fs));
    io.l[0] = 1.0f;
    io.r[0] = stereoInput ? 1.0f : 0.0f;
    Reverb r;
    r.prepare (fs, kMaxBlock);
    runConst (r, io, p);
    io.l[0] -= 1.0f;
    io.r[0] -= stereoInput ? 1.0f : 0.0f;
    return io;
}

/** RBJ band-pass (constant 0 dB peak gain), run in double. */
std::vector<double> bandPass (const std::vector<float>& x, double fc, double q, double fs)
{
    const double w0 = 2.0 * 3.14159265358979323846 * fc / fs;
    const double alpha = std::sin (w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    const double b0 = alpha / a0, b2 = -alpha / a0, a1 = -2.0 * std::cos (w0) / a0, a2 = (1.0 - alpha) / a0;
    std::vector<double> y (x.size());
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double in = x[i];
        const double out = b0 * in + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = in;
        y2 = y1;
        y1 = out;
        y[i] = out;
    }
    return y;
}

/** RT60 at 1 kHz: octave band (two cascaded band-passes), Schroeder backward integration, T30 fit. */
double measureRt60At1k (const Stereo& ir, double fs)
{
    std::vector<double> energy (ir.size(), 0.0);
    for (const auto* ch : { &ir.l, &ir.r })
    {
        auto b1 = bandPass (*ch, 1000.0, 1.414, fs);
        std::vector<float> tmp (b1.begin(), b1.end());
        const auto b2 = bandPass (tmp, 1000.0, 1.414, fs);
        for (size_t i = 0; i < b2.size(); ++i)
            energy[i] += b2[i] * b2[i];
    }
    std::vector<double> edc (energy.size());
    double acc = 0.0;
    for (size_t i = energy.size(); i-- > 0;)
    {
        acc += energy[i];
        edc[i] = acc;
    }
    const double total = edc[0];
    // Linear regression of the EDC (dB) between -5 and -35 dB.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int count = 0;
    for (size_t i = 0; i < edc.size(); i += 16)
    {
        const double db = 10.0 * std::log10 (edc[i] / total);
        if (db > -5.0 || db < -35.0)
            continue;
        const double t = static_cast<double> (i) / fs;
        sx += t;
        sy += db;
        sxx += t * t;
        sxy += t * db;
        ++count;
    }
    const double slope = (count * sxy - sx * sy) / (count * sxx - sx * sx);
    return -60.0 / slope;
}

double correlation (const float* a, const float* b, size_t n)
{
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < n; ++i)
    {
        ab += static_cast<double> (a[i]) * b[i];
        aa += static_cast<double> (a[i]) * a[i];
        bb += static_cast<double> (b[i]) * b[i];
    }
    return ab / std::sqrt (aa * bb);
}
} // namespace

TEST_CASE ("Reverb: output does not depend on the block size", "[fx][reverb]")
{
    checkBlockSizeIndependence<Reverb> (rp (2.0f, 0.5f, 0.023f, 0.8f));
    checkBlockSizeIndependence<Reverb> (rp (8.0f, 0.1f, 0.0f, 1.0f), 96000.0, 0.8);
}

TEST_CASE ("Reverb: level 0 is a bit-exact bypass", "[fx][reverb]")
{
    checkBitExactBypass<Reverb> (rp (3.0f, 0.5f, 0.02f, 0.7f), rp (3.0f, 0.5f, 0.02f, 0.0f));
}

TEST_CASE ("Reverb: switching on and off is click-free", "[fx][reverb]")
{
    checkClickFreeToggle<Reverb> (rp (2.5f, 0.5f, 0.0f, 1.0f), rp (2.5f, 0.5f, 0.0f, 0.0f));
}

TEST_CASE ("Reverb: RT60 at 1 kHz within 15 % of the setting at every sample rate", "[fx][reverb]")
{
    struct Case { double fs; float decay; float damping; };
    std::vector<Case> cases;
    for (const double fs : kRates)
        cases.push_back ({ fs, 2.5f, 0.5f });
    for (const float d : { 0.3f, 1.0f, 8.0f, 20.0f })
        cases.push_back ({ 48000.0, d, 0.5f });
    cases.push_back ({ 48000.0, 2.5f, 0.0f });
    cases.push_back ({ 48000.0, 2.5f, 1.0f });
    cases.push_back ({ 192000.0, 8.0f, 1.0f });

    for (const auto& c : cases)
    {
        const auto ir = impulseResponse (rp (c.decay, c.damping, 0.0f, 1.0f), c.fs, 0.9 * c.decay + 0.4);
        const double rt = measureRt60At1k (ir, c.fs);
        const double err = 100.0 * (rt / c.decay - 1.0);
        WARN ("reverb fs " << c.fs << " decay " << c.decay << " damping " << c.damping << ": RT60(1 kHz) " << rt << " s (" << err << " %)");
        CHECK (std::abs (err) <= 15.0);
    }
}

TEST_CASE ("Reverb: bounded energy for any parameter combination", "[fx][reverb]")
{
    const double fs = 48000.0;
    for (const float decay : { 0.3f, 20.0f })
        for (const float damping : { 0.0f, 1.0f })
            for (const float pre : { 0.0f, 0.2f })
            {
                const auto nb = static_cast<size_t> (1.0 * fs);
                const auto n = nb + static_cast<size_t> (3.0 * fs);
                Stereo io (n);
                const Stereo burst = noise (nb, 1.0f, 21u);
                std::copy (burst.l.begin(), burst.l.end(), io.l.begin());
                std::copy (burst.r.begin(), burst.r.end(), io.r.begin());
                Reverb r;
                r.prepare (fs, kMaxBlock);
                runConst (r, io, rp (decay, damping, pre, 1.0f));
                float peak = 0.0f;
                const bool ok = allFinite (io, 16.0f, &peak);
                // After the input stops, the energy of each 0.5 s window never grows.
                const auto win = static_cast<size_t> (0.5 * fs);
                double prev = 1.0e300;
                bool monotonic = true;
                for (size_t s = nb + static_cast<size_t> (pre * fs) + win / 2; s + win <= n; s += win)
                {
                    const double e = vs::test::rms (io.l.data() + s, win) + vs::test::rms (io.r.data() + s, win);
                    monotonic = monotonic && e <= prev;
                    prev = e;
                }
                INFO ("decay " << decay << " damping " << damping << " pre " << pre << " peak " << peak);
                CHECK (ok);
                CHECK (monotonic);
            }

    checkExtremeParams<Reverb> (
        [] (auto& rnd) {
            ReverbParams p;
            p.decaySeconds = wild (rnd, 0.3f, 20.0f);
            p.damping = wild (rnd, 0.0f, 1.0f);
            p.preDelaySeconds = wild (rnd, 0.0f, 0.2f);
            p.level = wild (rnd, 0.0f, 1.0f);
            return p;
        },
        16.0f, 48000.0, 4.0);
}

TEST_CASE ("Reverb: L/R tail correlation < 0.5", "[fx][reverb]")
{
    const double fs = 48000.0;
    for (const bool stereoIn : { true, false })
        for (const float damping : { 0.0f, 0.5f })
        {
            const auto ir = impulseResponse (rp (3.0f, damping, 0.0f, 1.0f), fs, 2.5, stereoIn);
            const auto from = static_cast<size_t> (0.1 * fs);
            const double c = correlation (ir.l.data() + from, ir.r.data() + from, ir.size() - from);
            WARN ("reverb tail correlation (input " << (stereoIn ? "L=R" : "L only") << ", damping " << damping << "): " << c);
            CHECK (std::abs (c) < 0.5);
        }
}

TEST_CASE ("Reverb: tail falls below -120 dBFS within tailSeconds", "[fx][reverb]")
{
    const double fs = 48000.0;
    for (const float decay : { 2.5f, 20.0f })
    {
        const auto p = rp (decay, 0.5f, 0.1f, 1.0f);
        Reverb r;
        r.prepare (fs, kMaxBlock);
        r.setParams (p);
        const double tail = r.tailSeconds();
        const auto nb = static_cast<size_t> (1.0 * fs);
        const auto n = nb + static_cast<size_t> (tail * fs);
        Stereo io (n);
        const Stereo burst = noise (nb, 1.0f, 31u);
        std::copy (burst.l.begin(), burst.l.end(), io.l.begin());
        std::copy (burst.r.begin(), burst.r.end(), io.r.begin());
        runConst (r, io, p);
        float peakIn = 0.0f, peakEnd = 0.0f;
        for (size_t i = 0; i < nb; ++i)
            peakIn = std::max ({ peakIn, std::abs (io.l[i]), std::abs (io.r[i]) });
        for (size_t i = n - static_cast<size_t> (0.05 * fs); i < n; ++i)
            peakEnd = std::max ({ peakEnd, std::abs (io.l[i]), std::abs (io.r[i]) });
        WARN ("reverb decay " << decay << ": tailSeconds " << tail << " s, output peak during the burst "
                              << vs::test::toDb (peakIn) << " dBFS, last 50 ms " << vs::test::toDb (peakEnd) << " dBFS");
        CHECK (peakEnd < 1.0e-6f);
        CHECK_FALSE (anySubnormal (io));
    }
}

TEST_CASE ("Reverb: no metallic ringing (tail spectrum peaks <= 6 dB above the 1/3-octave mean)", "[fx][reverb]")
{
    // Welch average of energy-normalised frames over the tail, compared with its own 1/3-octave
    // smoothed version. A ringing mode shows up in every frame of every excitation and survives the
    // averaging; estimator noise does not. The excitation must be white, so it is an impulse; it is
    // repeated 8 times at different start offsets (different modulation phases, so the fine
    // structure decorrelates) to give even the short (1 s) tail about 100 averages. A plain
    // 6-frame average spreads about +-5 dB over 1000 bins, which would hide or fake a 6 dB peak.
    // (Noise bursts are not usable: a short burst's own random spectrum passes straight through.)
    const double fs = 48000.0;
    for (const float damping : { 0.0f, 0.5f })
        for (const float decay : { 1.0f, 4.0f })
        {
            constexpr int order = 13;
            constexpr size_t frame = 1u << order, hop = frame / 2;
            std::vector<double> avg (frame / 2, 0.0);
            int frames = 0;
            for (int rep = 0; rep < 8; ++rep)
            {
            const auto t0 = static_cast<size_t> (0.173 * rep * fs);
            Stereo ir (t0 + static_cast<size_t> ((0.3 + 0.6 * decay) * fs));
            ir.l[t0] = ir.r[t0] = 1.0f;
            Reverb r;
            r.prepare (fs, kMaxBlock);
            runConst (r, ir, rp (decay, damping, 0.0f, 1.0f));
            for (size_t s = t0 + static_cast<size_t> (0.25 * fs); s + frame <= ir.size(); s += hop)
            {
                for (const auto* ch : { &ir.l, &ir.r })
                {
                    const auto p = powerSpectrumBH (ch->data() + s, order);
                    double tot = 0.0;
                    for (const double v : p)
                        tot += v;
                    for (size_t k = 0; k < avg.size(); ++k)
                        avg[k] += p[k] / tot;
                }
                ++frames;
            }
            }
            const double binHz = fs / static_cast<double> (frame);
            double worst = 0.0, worstHz = 0.0;
            const double third = std::pow (2.0, 1.0 / 6.0);
            for (size_t k = static_cast<size_t> (150.0 / binHz); k < static_cast<size_t> (12000.0 / binHz); ++k)
            {
                const auto lo = static_cast<size_t> (static_cast<double> (k) / third);
                const auto hi = static_cast<size_t> (static_cast<double> (k) * third) + 1;
                double mean = 0.0;
                for (size_t j = lo; j <= hi; ++j)
                    mean += avg[j];
                mean /= static_cast<double> (hi - lo + 1);
                const double db = 10.0 * std::log10 (avg[k] / mean);
                if (db > worst)
                {
                    worst = db;
                    worstHz = static_cast<double> (k) * binHz;
                }
            }
            WARN ("reverb decay " << decay << " damping " << damping << ": " << frames << " frames, highest tail peak "
                                  << worst << " dB above the 1/3-octave mean at " << worstHz << " Hz");
            CHECK (worst <= 6.0);
        }
}

TEST_CASE ("Reverb: all sample rates, bounded and finite", "[fx][reverb]")
{
    for (const double fs : kRates)
    {
        Stereo io = noise (static_cast<size_t> (1.0 * fs), 1.0f, 3u);
        Reverb r;
        r.prepare (fs, kMaxBlock);
        runConst (r, io, rp (5.0f, 0.3f, 0.05f, 1.0f), 300);
        INFO ("fs " << fs);
        CHECK (allFinite (io, 16.0f));
    }
}

TEST_CASE ("Reverb: silence after a burst decays to exactly zero, no subnormals", "[fx][reverb]")
{
    checkSilenceTail<Reverb> (rp (0.3f, 0.5f, 0.0f, 1.0f), 48000.0, 0.2, 3.0, true);
    checkSilenceTail<Reverb> (rp (20.0f, 0.0f, 0.2f, 1.0f), 48000.0, 0.2, 2.0, false);
}

TEST_CASE ("Reverb diag: wet level for sustained input", "[.diag]")
{
    // Steady-state wet/dry RMS ratio at level 1 (sustained noise, stereo and mono), to calibrate kOutGain.
    const double fs = 48000.0;
    for (const bool mono : { false, true })
        for (const float decay : { 0.5f, 1.0f, 2.5f, 8.0f, 20.0f })
        {
            const auto n = static_cast<size_t> ((2.0 + 1.5 * decay) * fs);
            Stereo in = noise (n, 0.5f, 17u);
            if (mono)
                in.r = in.l;
            Stereo io = in;
            Reverb r;
            r.prepare (fs, kMaxBlock);
            runConst (r, io, rp (decay, 0.5f, 0.0f, 1.0f));
            const auto win = static_cast<size_t> (1.0 * fs);
            std::vector<float> wet (win);
            for (size_t i = 0; i < win; ++i)
                wet[i] = io.l[n - win + i] - in.l[n - win + i];
            const double ratio = vs::test::rms (wet.data(), win) / vs::test::rms (in.l.data() + n - win, win);
            WARN ((mono ? "mono" : "stereo") << " decay " << decay << ": wet/dry " << vs::test::toDb (ratio) << " dB");
        }
}

TEST_CASE ("Reverb diag: tail spectrum peaks (long render, many frames)", "[.diag]")
{
    // Prints the strongest peaks of the energy-normalised Welch tail spectrum relative to the
    // 1/3-octave mean, to tell a real ringing mode from estimator noise.
    for (const double fs : { 48000.0, 96000.0 })
    for (const float damping : { 0.0f, 0.5f })
    {
        const auto ir = impulseResponse (rp (2.0f, damping, 0.0f, 1.0f), fs, 2.0);
        const int order = fs > 50000.0 ? 14 : 13;
        const size_t frame = static_cast<size_t> (1) << order, hop = frame / 8;
        std::vector<double> avg (frame / 2, 0.0);
        int frames = 0;
        for (size_t s = static_cast<size_t> (0.25 * fs); s + frame <= ir.size(); s += hop, ++frames)
            for (const auto* ch : { &ir.l, &ir.r })
            {
                const auto p = powerSpectrumBH (ch->data() + s, order);
                double tot = 0.0;
                for (const double v : p)
                    tot += v;
                for (size_t k = 0; k < avg.size(); ++k)
                    avg[k] += p[k] / tot;
            }
        const double binHz = fs / static_cast<double> (frame);
        const double third = std::pow (2.0, 1.0 / 6.0);
        std::vector<std::pair<double, double>> peaks;
        for (size_t k = static_cast<size_t> (150.0 / binHz); k < static_cast<size_t> (16000.0 / binHz); ++k)
        {
            const auto lo = static_cast<size_t> (static_cast<double> (k) / third);
            const auto hi = static_cast<size_t> (static_cast<double> (k) * third) + 1;
            double mean = 0.0;
            for (size_t j = lo; j <= hi; ++j)
                mean += avg[j];
            mean /= static_cast<double> (hi - lo + 1);
            peaks.emplace_back (10.0 * std::log10 (avg[k] / mean), static_cast<double> (k) * binHz);
        }
        std::sort (peaks.rbegin(), peaks.rend());
        for (size_t i = 0; i < 8; ++i)
            WARN ("fs " << fs << " damping " << damping << " frames " << frames << ": " << peaks[i].first << " dB at " << peaks[i].second << " Hz");
        for (size_t k = static_cast<size_t> (8300.0 / binHz); k < static_cast<size_t> (8700.0 / binHz); k += 2)
            WARN ("fs " << fs << " damping " << damping << " " << static_cast<double> (k) * binHz << " Hz: " << 10.0 * std::log10 (avg[k] * 1.0e6) << " dB");
    }
}

TEST_CASE ("Reverb: waking up after idle never replays stale content", "[fx][reverb]")
{
    checkNoStaleContentAfterWake<Reverb> (rp (4.0f, 0.3f, 0.1f, 1.0f), rp (4.0f, 0.3f, 0.1f, 0.0f));
}

TEST_CASE ("Reverb: no bulk buffer work on the audio thread (writes only in process, bounded per sample)", "[fx][reverb]")
{
    checkBufferWritesBounded<Reverb> (rp (4.0f, 0.3f, 0.1f, 1.0f), rp (4.0f, 0.3f, 0.1f, 0.0f));
}
