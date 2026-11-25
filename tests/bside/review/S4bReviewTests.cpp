// Independent review of S4b (docs/architecture.md 8.21.2): the factory bank, mix.level, the silent program apply and Clean's
// clear at every note start. Written from the contract and the code under review, not from the authors' tests.
//
//   [s4b-review]     default tests that pass and add coverage (apply edge cases, Clean's host-block independence)
//   [.s4b-measure]   hidden: the reviewer's own measurements of the bank, from the AUDIO where the authors read registers
//                    (pitch tracks of A2, A3, A6, A7, A8, B1; B6's sweep; B7's pan; the DC tail of the drums). Prints only.
//   [.s4b-defect]    hidden: a contract statement the build does not meet (fails on purpose)
#include <catch2/catch_test_macros.hpp>

#include "../integration/BankSound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace bsbank;

namespace
{
struct ApplyRun
{
    Stereo out;
    int lowerFaded = 0, upperFaded = 0, lowerActive = 0;
};

std::vector<double> mono (const Stereo& s)
{
    std::vector<double> m (s.l.size());
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = 0.5 * (static_cast<double> (s.l[i]) + static_cast<double> (s.r[i]));
    return m;
}

/** YIN pitch (Hz) of x over [start, start + w), lags for fmax .. fmin; 0 when the window does not fit. */
double yinHz (const std::vector<double>& x, size_t start, int w, double fmin, double fmax)
{
    const int tauMin = std::max (2, static_cast<int> (std::floor (kFs / fmax)));
    const int tauMax = static_cast<int> (std::ceil (kFs / fmin));
    if (start + static_cast<size_t> (w + tauMax + 2) > x.size())
        return 0.0;
    std::vector<double> d (static_cast<size_t> (tauMax + 2), 0.0), c (d.size(), 1.0);
    for (int tau = 1; tau <= tauMax + 1; ++tau)
    {
        double acc = 0.0;
        for (int j = 0; j < w; ++j)
        {
            const double e = x[start + static_cast<size_t> (j)] - x[start + static_cast<size_t> (j + tau)];
            acc += e * e;
        }
        d[static_cast<size_t> (tau)] = acc;
    }
    double run = 0.0;
    for (int tau = 1; tau <= tauMax + 1; ++tau)
    {
        run += d[static_cast<size_t> (tau)];
        c[static_cast<size_t> (tau)] = run > 0.0 ? d[static_cast<size_t> (tau)] * tau / run : 1.0;
    }
    int best = -1;
    for (int tau = tauMin; tau <= tauMax; ++tau)
        if (c[static_cast<size_t> (tau)] < 0.15)
        {
            while (tau + 1 <= tauMax && c[static_cast<size_t> (tau + 1)] < c[static_cast<size_t> (tau)])
                ++tau;
            best = tau;
            break;
        }
    if (best < 0)
    {
        best = tauMin;
        for (int tau = tauMin; tau <= tauMax; ++tau)
            if (c[static_cast<size_t> (tau)] < c[static_cast<size_t> (best)])
                best = tau;
    }
    const double a = c[static_cast<size_t> (best - 1)], b = c[static_cast<size_t> (best)], e = c[static_cast<size_t> (best + 1)];
    const double den = a - 2.0 * b + e;
    const double shift = std::abs (den) > 1e-12 ? 0.5 * (a - e) / den : 0.0;
    return kFs / (best + std::clamp (shift, -0.5, 0.5));
}

double cents (double f, double ref) { return 1200.0 * std::log2 (f / ref); }
double hzOf (double note) { return 440.0 * std::exp2 ((note - 69.0) / 12.0); }

/** Spectral centroid (Hz) of x over [start, start + 2048), Hann window. */
double centroidAt (const std::vector<double>& x, size_t start)
{
    constexpr int order = 11, size = 1 << order;
    if (start + size > x.size())
        return 0.0;
    juce::dsp::FFT fft (order);
    std::vector<float> f (2 * size, 0.0f);
    for (int k = 0; k < size; ++k)
        f[static_cast<size_t> (k)] = static_cast<float> (x[start + static_cast<size_t> (k)] * (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * k / (size - 1))));
    fft.performFrequencyOnlyForwardTransform (f.data());
    double num = 0.0, den = 0.0;
    for (int k = 1; k <= size / 2; ++k)
    {
        num += static_cast<double> (f[static_cast<size_t> (k)]) * k * kFs / size;
        den += f[static_cast<size_t> (k)];
    }
    return den > 0.0 ? num / den : 0.0;
}

double meanOf (const std::vector<float>& x, size_t a, size_t b)
{
    double s = 0.0;
    for (size_t i = a; i < b; ++i)
        s += x[i];
    return s / static_cast<double> (std::max<size_t> (1, b - a));
}

double rmsOf (const std::vector<float>& x, size_t a, size_t b)
{
    double s = 0.0;
    for (size_t i = a; i < b; ++i)
        s += static_cast<double> (x[i]) * x[i];
    return std::sqrt (s / static_cast<double> (std::max<size_t> (1, b - a)));
}

Stereo held (Rig& rig, const std::vector<int>& notes, double on, double off, double total, int block = kBlock)
{
    std::vector<Midi> midi;
    for (int n : notes)
    {
        midi.push_back (noteOn (at (on), n));
        midi.push_back (noteOff (at (off), n));
    }
    return rig.render (at (total), midi, block);
}

bool stepRuleHolds (double before, double after) { return after <= 3.0 * before + 0.002; }

/** Init's smooth sound (Vintage's apply test: triangles, low-pass at 300) on Lower. */
void smooth (Rig& rig)
{
    rig.set ("lower.osc1.wave", 1.0f);
    rig.set ("lower.filter.cutoff", 300.0f);
}

/** The first differing sample and the largest difference of two renders (for the messages). */
std::string diffText (const std::vector<float>& a, const std::vector<float>& b)
{
    size_t first = a.size();
    double worst = 0.0;
    for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
    {
        const double d = std::abs (static_cast<double> (a[i]) - static_cast<double> (b[i]));
        if (d > 0.0 && first == a.size())
            first = i;
        worst = std::max (worst, d);
    }
    return "first difference at sample " + std::to_string (first) + " (" + juce::String (static_cast<double> (first) / kFs, 4).toStdString() + " s), largest "
           + juce::String (worst, 9).toStdString() + " (" + juce::String (dbOf (worst), 1).toStdString() + " dB)";
}

bool sameBits (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
}
} // namespace

// =====================================================================================================================
// Measurements (hidden; prints)
// =====================================================================================================================

TEST_CASE ("S4b review: the bank measured from its audio", "[.s4b-measure]")
{
    // ---- A2: the vibrato's onset and rate from the audio pitch (C4 held 1.6 s)
    {
        Rig rig (1, false);
        const auto x = mono (held (rig, { 60 }, 0.05, 1.65, 1.8));
        const double ref = hzOf (60);
        double onset = -1.0;
        std::vector<double> upCross;
        double prev = 0.0;
        std::string first;
        for (double t = 0.06; t < 1.6; t += 0.002)
        {
            const double f = yinHz (x, static_cast<size_t> (at (t)), 480, 150.0, 400.0);
            const double c = cents (f, ref);
            if (onset < 0.0 && std::abs (c) > 10.0)
                onset = t - 0.05;
            if (onset >= 0.0 && prev < 0.0 && c >= 0.0)
                upCross.push_back (t);
            prev = c;
            if (t < 0.5 && std::fmod (t + 1e-9, 0.02) < 0.002)
                first += juce::String (c, 1).toStdString() + " ";
        }
        double period = 0.0;
        if (upCross.size() > 2)
            period = (upCross.back() - upCross.front()) / static_cast<double> (upCross.size() - 1);
        std::printf ("A2 audio: pitch every 20 ms (cents vs C4) %s\nA2 audio: vibrato onset %.1f ms after note-on, period %.1f ms (%.2f Hz) over %zu cycles\n",
                     first.c_str(), 1000.0 * onset, 1000.0 * period, period > 0 ? 1.0 / period : 0.0, upCross.size());
    }
    // ---- A2: legato C4 -> G4 (doc: 251 ms of glide blocks, 0.4 to 0.9 s, ends 7.08 semitones above C4)
    {
        Rig rig (1, false);
        const std::vector<Midi> midi { noteOn (at (0.05), 60), noteOn (at (0.55), 67), noteOff (at (0.56), 60), noteOff (at (2.0), 67) };
        const auto x = mono (rig.render (at (2.1), midi, kBlock));
        double t50 = -1.0;
        std::string row;
        for (double t = 0.55; t < 1.6; t += 0.005)
        {
            const double f = yinHz (x, static_cast<size_t> (at (t)), 480, 150.0, 500.0);
            const double st = cents (f, hzOf (60)) / 100.0;
            if (t50 < 0.0 && std::abs (st - 7.0) < 0.5)
                t50 = t - 0.55;
            if (std::fmod (t - 0.55 + 1e-9, 0.05) < 0.005)
                row += juce::String (st, 2).toStdString() + " ";
        }
        std::printf ("A2 audio legato C4->G4: semitones above C4 every 50 ms from the second note-on: %s\n   within 50 cents of G4 after %.1f ms\n", row.c_str(),
                     1000.0 * t50);
    }
    // ---- A3: the fundamental stays at the key (oscillator 1, the sync source) while the synced oscillator climbs: the centroid moves
    {
        Rig rig (2, false);
        const auto x = mono (held (rig, { 60 }, 0.05, 1.55, 1.6));
        double lo = 1e9, hi = -1e9, cmin = 1e9, cmax = 0, tmax = 0;
        std::string cRow;
        for (double t = 0.07; t < 1.5; t += 0.01)
        {
            const double c = cents (yinHz (x, static_cast<size_t> (at (t)), 960, 100.0, 2000.0), hzOf (60));
            lo = std::min (lo, c);
            hi = std::max (hi, c);
        }
        for (double t = 0.06; t < 1.5; t += 0.04)
        {
            const double c = centroidAt (x, static_cast<size_t> (at (t)));
            if (c > cmax)
            {
                cmax = c;
                tmax = t - 0.05;
            }
            cmin = std::min (cmin, c);
            cRow += juce::String (c, 0).toStdString() + " ";
        }
        // harmonicity: the share of the spectrum's energy within +-1 bin of the harmonics of C4 (8192 points, 0.2 .. 1.0 s), with the
        // table as it is and with "+sync" removed from it
        const auto harmonicShare = [] (const std::vector<double>& y) {
            constexpr int order = 13, size = 1 << order;
            juce::dsp::FFT fft (order);
            std::vector<double> pw (size / 2 + 1, 0.0);
            for (size_t st = static_cast<size_t> (at (0.25)); st + size <= static_cast<size_t> (at (1.05)); st += size / 2)
            {
                std::vector<float> fr (2 * size, 0.0f);
                for (int k = 0; k < size; ++k)
                    fr[static_cast<size_t> (k)] = static_cast<float> (y[st + static_cast<size_t> (k)] * (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * k / (size - 1))));
                fft.performFrequencyOnlyForwardTransform (fr.data());
                for (int k = 0; k <= size / 2; ++k)
                    pw[static_cast<size_t> (k)] += static_cast<double> (fr[static_cast<size_t> (k)]) * fr[static_cast<size_t> (k)];
            }
            double near = 0.0, all = 0.0;
            const double binHz = kFs / size, f0 = hzOf (60);
            for (int k = 1; k <= size / 2; ++k)
            {
                const double hz = k * binHz;
                const double h = std::round (hz / f0);
                all += pw[static_cast<size_t> (k)];
                if (h >= 1.0 && std::abs (hz - h * f0) <= 1.5 * binHz)
                    near += pw[static_cast<size_t> (k)];
            }
            return all > 0.0 ? near / all : 0.0;
        };
        const double withSync = harmonicShare (x);
        auto lines = factoryBank().programs[2].data[0];
        for (auto& l : lines)
            for (size_t p0 = l.find ("+sync"); p0 != std::string::npos; p0 = l.find ("+sync"))
                l.erase (p0, 5);
        Rig noSync (2, false);
        REQUIRE (noSync.presets().setTable (0, lines).isEmpty());
        const double withoutSync = harmonicShare (mono (held (noSync, { 60 }, 0.05, 1.55, 1.6)));
        std::printf ("A3 audio: share of the energy on the harmonics of C4 (0.2 .. 1.0 s): %.3f with the table's sync, %.3f with +sync removed\n", withSync,
                     withoutSync);
        std::printf ("A3 audio: fundamental from %.1f to %.1f cents of C4 over 0.07 .. 1.5 s (sync: the period is oscillator 1's)\n"
                     "A3 audio: centroid every 40 ms %s\n   min %.0f Hz, max %.0f Hz at %.0f ms\n",
                     lo, hi, cRow.c_str(), cmin, cmax, 1000.0 * tmax);
    }
    // ---- A5: velocity to cutoff from the audio: centroid of 0.06 .. 0.25 s at velocities 127, 100, 64, 20 (key C2)
    {
        std::string row;
        for (const int vel : { 127, 100, 64, 20 })
        {
            Rig rig (4, false);
            const auto x = mono (rig.render (at (0.6), { noteOn (at (0.05), 36, vel), noteOff (at (0.5), 36) }, kBlock));
            row += std::to_string (vel) + ":" + juce::String (centroidAt (x, static_cast<size_t> (at (0.08))), 0).toStdString() + "Hz/"
                   + juce::String (centroidAt (x, static_cast<size_t> (at (0.45))), 0).toStdString() + "Hz ";
        }
        std::printf ("A5 audio: centroid at 30 ms / 400 ms after the note-on, per velocity %s\n", row.c_str());
    }
    // ---- A6: the blip's length from the audio (C4)
    {
        Rig rig (5, false);
        const auto x = mono (held (rig, { 60 }, 0.05, 0.35, 0.4));
        double drop = -1.0;
        std::string row;
        for (double t = 0.05; t < 0.15; t += 0.001)
        {
            const double f = yinHz (x, static_cast<size_t> (at (t)), 240, 150.0, 1000.0);
            if (drop < 0.0 && t > 0.055 && f < std::sqrt (2.0) * hzOf (60) && f > 0.0)
                drop = t - 0.05;
            if (std::fmod (t - 0.05 + 1e-9, 0.005) < 0.001)
                row += juce::String (f, 0).toStdString() + " ";
        }
        std::printf ("A6 audio: Hz every 5 ms (5 ms windows starting there) %s\n   octave-up blip ends (window start below 370 Hz) at %.1f ms\n", row.c_str(), 1000.0 * drop);
    }
    // ---- A7, A8: the arpeggio's steps and their length from the audio (C4 held)
    for (const int slot : { 6, 7 })
    {
        Rig rig (slot, false);
        const auto x = mono (held (rig, { 60 }, 0.05, 1.25, 1.3));
        std::vector<std::pair<int, double>> runs; // semitone, length
        int cur = 999;
        double since = 0.0;
        for (double t = 0.05; t < 1.2; t += 0.0005)
        {
            const double f = yinHz (x, static_cast<size_t> (at (t)), 240, 150.0, 1200.0);
            const int st = static_cast<int> (std::lround (cents (f, hzOf (60)) / 100.0));
            if (st != cur)
            {
                if (cur != 999)
                    runs.push_back ({ cur, t - since });
                cur = st;
                since = t;
            }
        }
        std::string row;
        double sum = 0.0;
        int n = 0;
        for (size_t i = 0; i < runs.size() && i < 30; ++i)
        {
            row += std::to_string (runs[i].first) + "(" + juce::String (1000.0 * runs[i].second, 1).toStdString() + ") ";
            if (i > 0 && i + 1 < runs.size())
            {
                sum += runs[i].second;
                ++n;
            }
        }
        std::printf ("%s audio: semitone(ms) runs %s\n   mean step %.2f ms over %d inner runs\n", slot == 6 ? "A7" : "A8", row.c_str(), n > 0 ? 1000.0 * sum / n : 0.0, n);
    }
    // ---- B1: the kick's pitch from the audio's zero crossings (key 48 and key 72: the same drum?)
    for (const int key : { 48, 72 })
    {
        Rig rig (8, false);
        const auto x = mono (held (rig, { key }, 0.1, 0.4, 0.5));
        std::string row;
        double last = -1.0;
        int shown = 0;
        for (size_t i = static_cast<size_t> (at (0.112)) + 1; i < static_cast<size_t> (at (0.4)) && shown < 40; ++i)
            if (x[i - 1] < 0.0 && x[i] >= 0.0)
            {
                const double t = static_cast<double> (i - 1) + x[i - 1] / (x[i - 1] - x[i]);
                if (last >= 0.0)
                {
                    row += juce::String (1000.0 * (t / kFs - 0.1), 1).toStdString() + "ms:" + juce::String (kFs / (t - last), 1).toStdString() + "Hz ";
                    ++shown;
                }
                last = t;
            }
        std::printf ("B1 audio, key %d: upward zero crossings (time after note-on: frequency of the last period) %s\n", key, row.c_str());
    }
    // ---- B6: the band-pass sweep from the audio's centroid
    {
        Rig rig (13, false);
        const auto x = mono (held (rig, { 60 }, 0.05, 2.75, 5.0));
        std::string row;
        for (double t = 0.1; t < 4.5; t += 0.25)
            row += juce::String (centroidAt (x, static_cast<size_t> (at (t))), 0).toStdString() + " ";
        const auto rmsD = [&x] (size_t a, size_t b) {
            double acc = 0.0;
            for (size_t i = a; i < b; ++i)
                acc += x[i] * x[i];
            return std::sqrt (acc / static_cast<double> (b - a));
        };
        const double rmsHeld = rmsD (static_cast<size_t> (at (2.5)), static_cast<size_t> (at (2.75)));
        double t20 = -1.0;
        for (size_t i = static_cast<size_t> (at (2.75)); i + 2400 < x.size(); i += 480)
            if (rmsD (i, i + 2400) < rmsHeld * 0.1)
            {
                t20 = static_cast<double> (i) / kFs - 2.75;
                break;
            }
        std::printf ("B6 audio: centroid every 250 ms from 0.1 s %s\n   release: 20 dB below the held level %.2f s after the note-off\n", row.c_str(), t20);
    }
    // ---- B7: the pan of each layer from the audio (the other layer at -24 dB relative)
    for (const char* quiet : { "upper", "lower" })
    {
        Rig rig (14, false);
        rig.set (juce::String (quiet) + ".mix.level", -24.0f);
        const auto s = held (rig, { 60, 64 }, 0.05, 1.5, 1.6);
        const double l = rmsOf (s.l, static_cast<size_t> (at (0.8)), static_cast<size_t> (at (1.4)));
        const double r = rmsOf (s.r, static_cast<size_t> (at (0.8)), static_cast<size_t> (at (1.4)));
        std::printf ("B7 audio with %s at -24 dB: left - right %.2f dB\n", quiet, 20.0 * std::log10 (l / r));
    }
}

TEST_CASE ("S4b review: the drums' DC tail through the board high-pass: its size, whether it builds up over a fast roll, and the step when a voice goes idle",
           "[.s4b-measure]")
{
    for (const int slot : { 8, 9, 10 })
    {
        // one hit, held 0.5 s: the window the B3 test measures (350 .. 500 ms after the note-on)
        {
            Rig rig (slot, false);
            const auto s = held (rig, { 48 }, 0.1, 0.6, 0.7);
            const size_t a = static_cast<size_t> (at (0.45)), b = static_cast<size_t> (at (0.6));
            std::printf ("%s one hit: window 350-500 ms mean %.1f dBFS, RMS %.1f dB; hit peak %.1f dBFS\n", slot == 8 ? "B1" : slot == 9 ? "B2" : "B3",
                         dbOf (std::abs (meanOf (s.l, a, b))), dbOf (rmsOf (s.l, a, b)), dbOf (peakAbs (s.l, 0, s.l.size())));
        }
        // a fast roll: 64 hits 0.125 s apart (8 s), each held 0.1 s; the mean per hit period, and the largest
        {
            Rig rig (slot, false);
            std::vector<Midi> midi;
            for (int i = 0; i < 64; ++i)
            {
                midi.push_back (noteOn (at (0.05 + 0.125 * i), 48));
                midi.push_back (noteOff (at (0.05 + 0.125 * i + 0.1), 48));
            }
            const auto s = rig.render (at (8.05 + 3.0), midi, kBlock);
            double worst = 0.0, lastPeriod = 0.0;
            for (int i = 0; i < 64; ++i)
            {
                const auto a = static_cast<size_t> (at (0.05 + 0.125 * i)), b = static_cast<size_t> (at (0.05 + 0.125 * (i + 1)));
                const double m = meanOf (s.l, a, b);
                worst = std::max (worst, std::abs (m));
                if (i == 63)
                    lastPeriod = m;
            }
            // the largest |value| from 1.5 s after the last hit, and the largest sample step there (a voice leaving the sum)
            const size_t from = static_cast<size_t> (at (8.05 + 1.5));
            std::printf ("%s roll of 64 hits at 8 per second: largest |mean| over a hit period %.1f dBFS (last %.1f dBFS); from 1.5 s after the last hit: "
                         "peak %.1f dBFS, largest step %.1f dB; active voices at the end %d\n",
                         slot == 8 ? "B1" : slot == 9 ? "B2" : "B3", dbOf (worst), dbOf (std::abs (lastPeriod)), dbOf (peakAbs (s.l, from, s.l.size())),
                         dbOf (maxStep (s.l, from, s.l.size())), rig.proc.getLowerPart().numActiveVoices());
        }
        // the step when the voice leaves the sum: one hit, rendered in 1 ms blocks, the output around the block where the voice goes inactive
        {
            Rig rig (slot, false);
            std::vector<Midi> midi { noteOn (at (0.05), 48), noteOff (at (0.25), 48) };
            int idleBlock = -1, blockIndex = 0;
            const auto s = rig.render (at (4.0), midi, 48, {}, [&] (int) {
                if (idleBlock < 0 && blockIndex > 60 && rig.proc.getLowerPart().numActiveVoices() == 0)
                    idleBlock = blockIndex;
                ++blockIndex;
            });
            if (idleBlock >= 0)
            {
                const size_t c = static_cast<size_t> (idleBlock * 48);
                std::printf ("%s voice leaves the sum %.3f s after the note-on; |value| before it %.1f dBFS; largest step within +-2 ms %.1f dB; "
                             "largest step in the 100 ms before %.1f dB\n",
                             slot == 8 ? "B1" : slot == 9 ? "B2" : "B3", static_cast<double> (c) / kFs - 0.05, dbOf (std::abs (s.l[c - 48])),
                             dbOf (maxStep (s.l, c - 96, c + 96)), dbOf (maxStep (s.l, c - 4800, c - 96)));
            }
            else
                std::printf ("voice never left the sum in 4 s\n");
        }
    }
}

TEST_CASE ("S4b review: drum hit-peak spread at other spacings than the gate's phrase", "[.s4b-measure]")
{
    for (const int slot : { 8, 9, 10 })
        for (const double spacing : { 0.125, 0.15, 0.2, 0.25, 0.3, 0.4, 0.5, 0.7, 1.0 })
            for (const bool shortHold : { false, true }) // held 0.8 of the spacing, or 0.2 s (at most 0.8 of the spacing)
        {
            const double hold = shortHold ? std::min (0.2, 0.8 * spacing) : 0.8 * spacing;
            if (shortHold && spacing < 0.26)
                continue;
            Performance perf;
            for (int i = 0; i < 8; ++i)
                addNote (perf, 0.05 + spacing * i, 0.05 + spacing * i + hold, 48, kVelocity);
            finish (perf);
            Rig rig (slot, false);
            const auto hits = hitTimings (rig.play (perf, 0.5), perf);
            double lo = 1e9, hi = -1e9, dlo = 1e9, dhi = -1e9;
            for (const auto& h : hits)
            {
                lo = std::min (lo, h.peakDb);
                hi = std::max (hi, h.peakDb);
                dlo = std::min (dlo, h.onsetMs);
                dhi = std::max (dhi, h.onsetMs);
            }
            std::printf ("%s spacing %.3f s held %.3f s: peak spread %.2f dB, onset spread %.2f ms\n", slot == 8 ? "B1" : slot == 9 ? "B2" : "B3", spacing, hold,
                         hi - lo, dhi - dlo);
        }
}

// =====================================================================================================================
// Apply edge cases (default)
// =====================================================================================================================

TEST_CASE ("S4b review: the same program applied again over held keys does not fade: the shell releases the notes, which sound on in their release without a step",
           "[bside][s4b-review]")
{
    // A program whose parameters equal the ones in force is no "program applied" for the part (Part::applyBlockParams): no fade. The
    // shell still releases every voice on a preset change (BiTimbralProcessor::releaseForPresetChange), so the notes go into their
    // release (A4: release 5) instead of the 3 ms fade.
    const auto render = [] (bool again) {
        Rig rig (3, false); // A4: a table and a held tone
        std::map<long long, std::function<void()>> changes;
        if (again)
            changes[at (0.3)] = [&rig] { REQUIRE (rig.presets().selectProgram (3)); };
        std::vector<Midi> midi { noteOn (at (0.05), 48), noteOn (at (0.05), 55), noteOn (at (0.05), 64) };
        ApplyRun r;
        r.out = rig.render (at (0.32), midi, kBlock, changes);
        r.lowerFaded = rig.proc.getLowerPart().appliesFaded();
        r.lowerActive = rig.proc.getLowerPart().numActiveVoices();
        return r;
    };
    const auto a = render (false), b = render (true);
    INFO ("left: " << diffText (a.out.l, b.out.l));
    CHECK (b.lowerFaded == 0);
    CHECK (b.lowerActive == 3); // still sounding 20 ms later: released, not faded
    const size_t s0 = static_cast<size_t> (at (0.3));
    CHECK (std::equal (a.out.l.begin(), a.out.l.begin() + static_cast<std::ptrdiff_t> (s0), b.out.l.begin()));
    CHECK (stepRuleHolds (maxStep (b.out.l, s0 - 9600, s0), maxStep (b.out.l, s0, b.out.l.size())));
}

TEST_CASE ("S4b review: a program applied 1 ms into a structural switch (chip.model by hand) still ends silent, without a step, with nothing held back",
           "[bside][s4b-review]")
{
    Rig rig (0, false);
    smooth (rig);
    std::vector<Midi> midi { noteOn (at (0.05), 48), noteOn (at (0.05), 55), noteOn (at (0.05), 64) };
    const int sw = at (0.3);
    const auto out = rig.render (at (0.45), midi, 48,
                                 { { sw, [&] { rig.set ("lower.chip.model", 0.0f); } }, { sw + 48, [&] { REQUIRE (rig.presets().selectProgram (3)); } } });
    const auto before = maxStep (out.l, static_cast<size_t> (sw) - 9600, static_cast<size_t> (sw));
    const auto after = maxStep (out.l, static_cast<size_t> (sw), static_cast<size_t> (sw) + 4800);
    INFO ("step before " << before << ", after " << after);
    CHECK (stepRuleHolds (before, after));
    CHECK (rig.proc.getLowerPart().appliesFaded() == 1);
    CHECK (rig.proc.getLowerPart().numActiveVoices() == 0);
    CHECK (rig.proc.getBlockParams().lowerParams.chipModel == 1); // A4 is Model B: the program, not the hand's Model A, is in force
    CHECK (peakAbs (out.l, static_cast<size_t> (sw + at (0.01)), out.l.size()) < 1.0e-4);
}

TEST_CASE ("S4b review: a program applied while a steal fade runs: the stealing note starts once, after the apply's fade, with the new program",
           "[bside][s4b-review]")
{
    Rig rig (0, false);
    smooth (rig);
    std::vector<Midi> midi;
    for (int n = 0; n < 16; ++n)
        midi.push_back (noteOn (at (0.05), 40 + n)); // all 16 voices
    const int sw = at (0.3);
    midi.push_back (noteOn (sw, 80)); // the 17th steals (3 ms fade) in the block the program arrives
    const auto out = rig.render (at (0.4), midi, 48, { { sw, [&] { REQUIRE (rig.presets().selectProgram (3)); } } });
    const auto& lower = rig.proc.getLowerPart();
    CHECK (lower.appliesFaded() == 1);
    REQUIRE (lower.numActiveVoices() == 1);
    for (int i = 0; i < bs::Part::kVoices; ++i)
        if (lower.getVoice (i).active)
        {
            CHECK (lower.getVoice (i).note == 80);
            CHECK ((lower.getVoice (i).chain.chip().registerValue (4) & 0xf0) == 0x10); // A4's triangle (the new program)
        }
    const auto before = maxStep (out.l, static_cast<size_t> (sw) - 9600, static_cast<size_t> (sw));
    const auto after = maxStep (out.l, static_cast<size_t> (sw), static_cast<size_t> (sw + at (0.0035)));
    INFO ("step before " << before << ", in the fade " << after);
    CHECK (stepRuleHolds (before, after));
}

TEST_CASE ("S4b review: in Dual, a program that changes only Upper fades only Upper; Lower's output is the same bit for bit as when the unchanged program is applied",
           "[bside][s4b-review]")
{
    const auto& bank = factoryBank();
    auto changed = bank.programs[14]; // B7 Pulse Over Pad, Dual
    {
        Rig probeRig (-1, false);
        const int i = probeRig.schema().indexOf ("upper.filter.cutoff");
        REQUIRE (i >= 0);
        changed.values[static_cast<size_t> (i)] = 1400.0f;
    }
    const auto render = [&] (const vs::presets::Program& applied) {
        Rig rig (14, false);
        rig.set ("upper.mix.pan", 1.0f);
        rig.set ("lower.mix.pan", -1.0f);
        std::map<long long, std::function<void()>> changes;
        changes[at (0.3)] = [&] {
            rig.presets().applyProgram (applied, 14, false);
            // keep the pans of this test (the program carries its own): Lower alone on the left, Upper alone on the right
            rig.set ("upper.mix.pan", 1.0f);
            rig.set ("lower.mix.pan", -1.0f);
        };
        ApplyRun r;
        r.out = rig.render (at (0.5), { noteOn (at (0.05), 60), noteOn (at (0.05), 64) }, kBlock, changes);
        r.lowerFaded = rig.proc.getLowerPart().appliesFaded();
        r.upperFaded = rig.proc.getUpperPart().appliesFaded();
        return r;
    };
    const auto a = render (bank.programs[14]), b = render (changed);
    INFO ("left: " << diffText (a.out.l, b.out.l) << "; right: " << diffText (a.out.r, b.out.r));
    CHECK (a.lowerFaded == 0);
    CHECK (a.upperFaded == 0);
    CHECK (b.lowerFaded == 0);
    CHECK (b.upperFaded == 1);
    CHECK (sameBits (a.out.l, b.out.l));
    CHECK_FALSE (sameBits (a.out.r, b.out.r));
}

TEST_CASE ("S4b review: every program, held over three keys, with Init applied over it: Vintage's step rule, and nothing sounds after the fade",
           "[bside][s4b-review]")
{
    // 8.21.2 "in both directions": the authors' gate applies every program from Init and from the program before it (so X -> Init only for
    // B8); this is X -> Init for A2 .. B8, on one rig (Init over Init is the same program: no fade, the shell's release; see above).
    Rig rig (0, false);
    for (int slot = 1; slot < kBankSize; ++slot)
    {
        INFO ("slot " << slot);
        REQUIRE (rig.presets().selectProgram (slot));
        rig.render (at (0.05), {}, kBlock); // the new program's smoothed values settle
        const auto t0 = static_cast<int> (rig.now);
        std::vector<Midi> midi;
        for (int n : { 48, 55, 64 })
            midi.push_back (noteOn (t0 + at (0.05), n));
        const auto out = rig.render (at (0.42), midi, kBlock, { { t0 + at (0.3), [&] { REQUIRE (rig.presets().selectProgram (0)); } } });
        const size_t s0 = static_cast<size_t> (at (0.3));
        for (const auto* ch : { &out.l, &out.r })
        {
            const double before = maxStep (*ch, s0 - 9600, s0), after = maxStep (*ch, s0, s0 + 4800);
            INFO ("step before " << before << ", after " << after);
            CHECK (stepRuleHolds (before, after));
            CHECK (peakAbs (*ch, s0 + static_cast<size_t> (at (0.01)), ch->size()) < 1.0e-4);
        }
        rig.render (at (0.2), {}, kBlock);
    }
}

namespace
{
struct FixedPlayHead final : juce::AudioPlayHead
{
    double bpm = 90.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (true);
        p.setBpm (bpm);
        p.setPpqPosition (0.0);
        return p;
    }
};
} // namespace

TEST_CASE ("S4b review: A7's tick follows the host's tempo while the host plays (tick.rate Tempo, 12 per beat)", "[bside][s4b-review]")
{
    for (const double bpm : { 90.0, 150.0 })
    {
        Rig rig (6, false);
        FixedPlayHead head;
        head.bpm = bpm;
        rig.proc.setPlayHead (&head);
        std::uint32_t period = 0;
        rig.render (at (0.3), { noteOn (at (0.05), 60) }, kBlock, {}, [&] (int) {
            if (const auto* v = newestVoice (rig.proc, 0))
                period = v->engine.tickPeriod();
        });
        rig.proc.setPlayHead (nullptr);
        const double want = 985248.0 * 60.0 / (bpm * 12.0);
        INFO (bpm << " bpm: tick period " << period << " cycles, want " << want);
        CHECK (std::abs (static_cast<double> (period) - want) <= 1.0);
    }
}

// =====================================================================================================================
// Clean's clear at every note start: host-block independence (default)
// =====================================================================================================================

TEST_CASE ("S4b review: Clean's clear at every note start does not depend on the host's blocks: B2's and B3's phrases and B5's Shared retriggers at 1 and 256",
           "[bside][s4b-review]")
{
    struct Case
    {
        int slot;
        Performance perf;
    };
    Performance retrig; // B5 (Shared, Clean): one key struck three times while two others are held
    addNote (retrig, 0.05, 1.5, 60, kVelocity);
    addNote (retrig, 0.05, 1.5, 67, kVelocity);
    for (int i = 0; i < 3; ++i)
        addNote (retrig, 0.1 + 0.37 * i, 0.1 + 0.37 * i + 0.3, 64, kVelocity);
    finish (retrig);
    Performance shortPerc; // four hits, 0.25 s apart, as the phrase
    for (int i = 0; i < 4; ++i)
        addNote (shortPerc, 0.05 + 0.25 * i, 0.05 + 0.25 * i + 0.2, 48, kVelocity);
    finish (shortPerc);
    for (const auto& c : { Case { 9, shortPerc }, Case { 10, shortPerc }, Case { 12, retrig } })
    {
        Rig big (c.slot, false), one (c.slot, false);
        const auto total = c.perf.lastOff + at (0.1);
        const auto a = big.render (total, c.perf.midi, 256);
        const auto b = one.render (total, c.perf.midi, 1);
        double worst = 0.0;
        for (size_t i = 0; i < a.l.size(); ++i)
            worst = std::max ({ worst, std::abs (static_cast<double> (a.l[i]) - b.l[i]), std::abs (static_cast<double> (a.r[i]) - b.r[i]) });
        INFO ("slot " << c.slot << ": largest difference between host blocks of 256 and 1: " << worst);
        CHECK (worst < 1.0e-6);
        const auto ha = hitTimings (a, c.perf), hb = hitTimings (b, c.perf);
        for (size_t h = 0; h < ha.size(); ++h)
            CHECK (std::abs (ha[h].onsetMs - hb[h].onsetMs) < 0.05);
        for (const auto* p : { &big, &one })
            for (int i = 0; i < bs::Part::kVoices; ++i)
                CHECK (p->proc.getLowerPart().getVoice (i).chain.chip().cycle() == big.proc.getLowerPart().getVoice (i).chain.chip().cycle());
    }
}

// =====================================================================================================================
// An apply that changes only the table (8.21.2, review finding D1; default)
// =====================================================================================================================

TEST_CASE ("S4b review: a program that differs from the sounding one only in its table is not faded: the held note is released into the old table's release, the next note plays the new table",
           "[bside][s4b-review]")
{
    // 8.21.2 "An apply that changes only the table": nothing is faded (Part::applyBlockParams fades only when the parameters differ);
    // the shell's preset-change release (BiTimbralProcessor::releaseForPresetChange) ends the held note in the OLD table's release,
    // and the new table plays from the next note. Vintage's step rule holds for it as well.
    const auto& bank = factoryBank();
    auto variant = bank.programs[6]; // A7's parameters ...
    variant.data = bank.programs[7].data; // ... with A8's table (minor chord, pulse sweep)
    variant.meta.name = "A7 variant";

    // The semitone steps (relative to key 60) the newest voice's pitch register passes through from `from` to the end of the render.
    const auto semitoneSteps = [] (Rig& rig, std::vector<int>& semis, double from) {
        return [&rig, &semis, from] (int) {
            if (rig.now > at (from))
                if (const auto* v = newestVoice (rig.proc, 0))
                {
                    const auto& c = v->chain.chip();
                    const int f = c.registerValue (0) | (c.registerValue (1) << 8);
                    const int st = static_cast<int> (std::lround (12.0 * std::log2 (f * 985248.0 / 16777216.0 / hzOf (60))));
                    if (semis.empty() || semis.back() != st)
                        semis.push_back (st);
                }
        };
    };
    const auto text = [] (const std::vector<int>& v) {
        std::string row;
        for (int s : v)
            row += std::to_string (s) + " ";
        return row;
    };

    Rig rig (6, false);
    const auto& lower = rig.proc.getLowerPart();
    int activeAt305 = -1;
    const auto first = rig.render (at (1.0), { noteOn (at (0.05), 60) }, 48, { { at (0.3), [&] { rig.presets().applyProgram (variant, 6, false); } } },
                                   [&] (int) {
                                       if (rig.now > at (0.305) && activeAt305 < 0)
                                           activeAt305 = lower.numActiveVoices();
                                   });
    CHECK (activeAt305 == 1); // released, not faded and idle (a fade is over after 3 ms)
    const size_t s0 = static_cast<size_t> (at (0.3));
    CHECK (lower.appliesFaded() == 0);
    CHECK (lower.numActiveVoices() == 0); // the note ended in its release
    CHECK (peakAbs (first.l, static_cast<size_t> (at (0.9)), first.l.size()) < 1.0e-3); // -60 dBFS: the board high-pass's DC tail (1.7e-4 here) is not the note
    INFO ("largest step before " << maxStep (first.l, s0 - 9600, s0) << ", after " << maxStep (first.l, s0, first.l.size()));
    CHECK (stepRuleHolds (maxStep (first.l, s0 - 9600, s0), maxStep (first.l, s0, first.l.size())));

    // The next note plays the new table: A8's minor chord (0 3 7 10 12 ...), not A7's (0 4 7 12 ...); the same as a fresh instance
    // that was given the variant before any note.
    std::vector<int> next;
    const auto t0 = static_cast<int> (rig.now);
    rig.render (at (0.8), { noteOn (t0 + at (0.05), 60) }, 48, {}, semitoneSteps (rig, next, 0.0));
    Rig fresh (6, false);
    fresh.presets().applyProgram (variant, 6, false);
    std::vector<int> expected;
    fresh.render (at (0.8), { noteOn (at (0.05), 60) }, 48, {}, semitoneSteps (fresh, expected, 0.0));
    Rig plain (6, false);
    std::vector<int> major;
    plain.render (at (0.8), { noteOn (at (0.05), 60) }, 48, {}, semitoneSteps (plain, major, 0.0));
    INFO ("after the apply the new note steps through " << text (next) << "; the variant on a fresh instance " << text (expected) << "; A7 " << text (major));
    CHECK (lower.appliesFaded() == 0);
    CHECK (next == expected);
    CHECK (next != major);
    CHECK (std::find (next.begin(), next.end(), 3) != next.end());
    CHECK (std::find (next.begin(), next.end(), 4) == next.end());
}
