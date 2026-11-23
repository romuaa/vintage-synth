// The aliasing of the static curves through the whole chain (docs/architecture.md 8.19.4, S3b-2; ChipVoice.h "ALIAS",
// Chip.h "BEND", HalfBand.h): the alias gate on the chain's output before the board filters, the wanted signal (pass
// band, products of sines, harmonics of band-limited voices against the exact curves), the anti-alias filters' own
// responses, the latency, silence, and the configurations blended across a write.

#include "FuzzSupport.h"
#include "RenderTestSupport.h"

#include "bside/chip/ChipVoice.h"
#include "bside/chip/HalfBand.h"
#include "bside/chip/RenderTables.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace bs_render_test;
using vs::bside::chip::ChipVoice;
using vs::bside::chip::Filter;
using vs::bside::chip::HalfBandDesign;
using vs::bside::chip::OutputStage;
namespace ctrl = vs::bside::chip::ctrl;
namespace chip = vs::bside::chip;

namespace
{
constexpr double kClock = 985248.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kAliasN = 1 << 18;

/** A voice with its own filter (Per Note), as ChipVoiceTests.cpp. */
struct Voice
{
    std::unique_ptr<ChipVoice> v = std::make_unique<ChipVoice>();
    std::unique_ptr<Filter> f = std::make_unique<Filter>();
    Voice (Model m, double rate, bool volumeDc)
    {
        chip::Config cfg;
        cfg.model = m;
        cfg.internalRate = rate;
        cfg.volumeDc = volumeDc;
        v->prepare (cfg);
        f->prepare (m, rate, 0.5f);
        v->setFilter (f.get());
    }
};

void voiceOsc (ChipVoice& v, int n, double reg, float pw, std::uint8_t control)
{
    OscInput in;
    in.frequency = reg;
    in.pulseWidth = pw;
    in.control = control;
    in.attackDecay = 0x00;
    in.sustainRelease = 0xf0; // full envelope (sustain 15)
    v.setOscillator (n, in);
}

/** 7-term Blackman-Harris magnitudes of x (its mean removed), double precision throughout. */
std::vector<double> bhMagnitudes (const std::vector<double>& x)
{
    constexpr double a[7] { 0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803, 0.01081174209837, 0.00077658482522, 0.00001388721735 };
    double mean = 0.0;
    for (const double s : x)
        mean += s;
    mean /= static_cast<double> (x.size());
    const double n = static_cast<double> (x.size());
    std::vector<std::complex<double>> c (x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        double w = 0.0;
        for (int k = 0; k < 7; ++k)
            w += (k % 2 == 0 ? 1.0 : -1.0) * a[k] * std::cos (2.0 * kPi * k * static_cast<double> (i) / n);
        c[i] = (x[i] - mean) * w;
    }
    fft (c);
    std::vector<double> m (c.size() / 2);
    for (std::size_t i = 0; i < m.size(); ++i)
        m[i] = std::abs (c[i]);
    return m;
}

/** 8.19.4's measurement: the worst bin in 20 Hz .. 20 kHz more than 10 bins from every multiple of the common
    fundamental, relative to the peak of the first voice's fundamental (+-12 bins), not bin-centred. */
struct Alias
{
    double db = -300.0, hz = 0.0;
};
Alias aliasOfSpectrum (const std::vector<double>& mag, double rate, double commonHz, double firstHz)
{
    const double binHz = rate / (2.0 * static_cast<double> (mag.size()));
    const auto c = static_cast<std::ptrdiff_t> (std::lround (firstHz / binHz));
    double ref = 0.0;
    for (std::ptrdiff_t b = c - 12; b <= c + 12; ++b)
        if (b > 0 && b < static_cast<std::ptrdiff_t> (mag.size()))
            ref = std::max (ref, mag[static_cast<std::size_t> (b)]);
    Alias r;
    for (std::size_t b = static_cast<std::size_t> (std::ceil (20.0 / binHz)); b < mag.size(); ++b)
    {
        const double hz = static_cast<double> (b) * binHz;
        if (hz > 20000.0)
            break;
        const double nearest = std::round (hz / commonHz) * commonHz;
        if (std::abs (hz - nearest) <= 10.0 * binHz)
            continue;
        const double db = 20.0 * std::log10 (mag[b] / ref + 1e-300);
        if (db > r.db)
            r = { db, hz };
    }
    return r;
}

// ---- the gate's signal set (8.19.4) ----

struct Signal
{
    const char* name;
    int voices;
    std::array<double, 3> reg;          // register frequencies (exact ratios: multiples of 15 at the top)
    std::array<std::uint8_t, 3> wave;   // ctrl bits
    double common;                      // the common fundamental's divisor of voice 1's frequency
};

// 65535 is the register's top (3848.6 Hz at 985 248 Hz) and a multiple of 15; 34050 = 15 x 2270 is 2000.0 Hz
constexpr std::uint8_t kSaw = ctrl::saw, kSquare = ctrl::pulse;
const Signal kSignals[] { { "saw 3.85 kHz", 1, { 65535.0, 0, 0 }, { kSaw, 0, 0 }, 1.0 },
                         { "square 3.85 kHz", 1, { 65535.0, 0, 0 }, { kSquare, 0, 0 }, 1.0 },
                         { "saw 2 kHz", 1, { 34050.0, 0, 0 }, { kSaw, 0, 0 }, 1.0 },
                         { "saw 1 kHz", 1, { 17025.0, 0, 0 }, { kSaw, 0, 0 }, 1.0 },
                         { "saw + saw 3:2 at 3.85 kHz", 2, { 65535.0, 43690.0, 0 }, { kSaw, kSaw, 0 }, 3.0 },
                         { "saw + saw 3:2 at 2 kHz", 2, { 34050.0, 22700.0, 0 }, { kSaw, kSaw, 0 }, 3.0 },
                         { "saw + square 5:4 at 3.85 kHz", 2, { 65535.0, 52428.0, 0 }, { kSaw, kSquare, 0 }, 5.0 },
                         { "saw + square 5:4 at 2 kHz", 2, { 34050.0, 27240.0, 0 }, { kSaw, kSquare, 0 }, 5.0 },
                         { "three voices at 3.85 kHz", 3, { 65535.0, 43690.0, 52428.0 }, { kSaw, kSaw, kSquare }, 15.0 },
                         { "three voices at 2 kHz", 3, { 34050.0, 22700.0, 27240.0 }, { kSaw, kSaw, kSquare }, 15.0 } };

struct Setting
{
    int routing;          // 0 all direct, 1 voice 1 through the filter, 2 all through the filter
    std::uint8_t mode;    // 0x10 low, 0x20 band, 0x40 high
    int cutoff, resonance;
};

std::vector<Setting> settingsFor (const Signal& s)
{
    std::vector<Setting> v { { 0, 0x10, 2047, 0 } };
    for (const int routing : { 1, 2 })
    {
        if (routing == 1 && s.voices == 1)
            continue; // voice 1 through the filter is all through the filter
        for (const std::uint8_t mode : { std::uint8_t { 0x10 }, std::uint8_t { 0x20 }, std::uint8_t { 0x40 } })
            for (const int cutoff : { 2047, 1024 })
                for (const int res : { 0, 15 })
                    v.push_back ({ routing, mode, cutoff, res });
    }
    return v;
}

const char* modeName (std::uint8_t m) { return m == 0x10 ? "low-pass" : (m == 0x20 ? "band-pass" : "high-pass"); }

/** The limit of 8.19.4 for a signal: Model B -103 dB; Model A -99 dB up to 2 kHz, -83 dB up to 3.85 kHz. */
double limitFor (Model m, const Signal& s) { return m == Model::B ? -103.0 : (s.reg[0] > 40000.0 ? -83.0 : -99.0); }

/** The chain's output before the board filters, kAliasN samples after a settling time. */
std::vector<double> renderSignal (Model m, double rate, const Signal& s, const Setting& st)
{
    Voice p (m, rate, false);
    ChipVoice& v = *p.v;
    v.boardFiltersEnabled (false);
    for (int k = 0; k < s.voices; ++k)
        voiceOsc (v, k, s.reg[static_cast<std::size_t> (k)], 2048.0f, static_cast<std::uint8_t> (s.wave[static_cast<std::size_t> (k)] | ctrl::gate));
    v.write (0x15, static_cast<std::uint8_t> (st.cutoff & 7));
    v.write (0x16, static_cast<std::uint8_t> (st.cutoff >> 3));
    const int routed = st.routing == 0 ? 0 : (st.routing == 1 ? 1 : 7);
    v.write (0x17, static_cast<std::uint8_t> ((st.resonance << 4) | routed));
    v.write (0x18, static_cast<std::uint8_t> (st.mode | 0x0f));
    const int warm = std::max (16384, static_cast<int> (rate * 0.03));
    std::vector<double> y (static_cast<std::size_t> (warm + kAliasN), 0.0);
    v.render (y.data(), static_cast<int> (y.size()));
    y.erase (y.begin(), y.begin() + warm);
    return y;
}


// ---- ideal band-limited voices (the static curves alone) ----

/** One voice's waveform after its converter at full envelope, band-limited exactly: every harmonic below rate / 2 of the
    ideal saw (w = u - 1/2) or square (-1/2 below half the period, +1/2 above), Model A bent (render::bend: the waveform
    the renderer's folded bend renders), synthesised as sines (phasors re-anchored every 4096 samples). */
std::vector<double> idealVoice (Model m, double rate, double hz, bool square, double phase, int n)
{
    constexpr int kM = 1 << 16;
    std::vector<std::complex<double>> c (kM);
    for (int j = 0; j < kM; ++j)
    {
        const double u = (j + 0.5) / kM;
        const double w = square ? (u < 0.5 ? -0.5 : 0.5) : u - 0.5;
        c[static_cast<std::size_t> (j)] = m == Model::A ? chip::render::bend (w) : w;
    }
    fft (c); // c[k] / M: the Fourier coefficient of harmonic k (midpoint rule)
    const int harmonics = static_cast<int> (std::floor ((rate / 2.0 - 1.0) / hz));
    std::vector<double> x (static_cast<std::size_t> (n), c[0].real() / kM);
    for (int k = 1; k <= harmonics && k < kM / 2; ++k)
    {
        const std::complex<double> a = 2.0 * c[static_cast<std::size_t> (k)] / static_cast<double> (kM);
        const double w = 2.0 * kPi * k * hz / rate;
        const std::complex<double> step (std::cos (w), std::sin (w));
        std::complex<double> z;
        for (int i = 0; i < n; ++i)
        {
            if ((i & 4095) == 0)
            {
                const double ph = w * i + k * phase;
                z = a * std::complex<double> (std::cos (ph), std::sin (ph));
            }
            x[static_cast<std::size_t> (i)] += z.real();
            z *= step;
        }
    }
    return x;
}

/** The chain run on ideal voices (ChipVoice::processBuses), as renderSignal sets it up: the buses of the routing (the
    oscillators routed, sounding or not, count on the filter bus), envelope sums of the sounding voices. */
std::vector<double> renderIdeal (Model m, double rate, const Signal& s, const Setting& st, const std::array<std::vector<double>, 3>& voices)
{
    Voice p (m, rate, false);
    ChipVoice& v = *p.v;
    v.boardFiltersEnabled (false);
    v.write (0x15, static_cast<std::uint8_t> (st.cutoff & 7));
    v.write (0x16, static_cast<std::uint8_t> (st.cutoff >> 3));
    const int routed = st.routing == 0 ? 0 : (st.routing == 1 ? 1 : 7);
    v.write (0x17, static_cast<std::uint8_t> ((st.resonance << 4) | routed));
    v.write (0x18, static_cast<std::uint8_t> (st.mode | 0x0f));
    const std::size_t total = voices[0].size();
    std::vector<double> fa (total, 0.0), da (total, 0.0), fe (total, 0.0), de (total, 0.0), fv (total, 0.0), dv (total, 0.0);
    double nf = 0.0;
    for (int k = 0; k < 3; ++k)
        nf += ((routed >> k) & 1) != 0 ? 1.0 : 0.0;
    for (std::size_t i = 0; i < total; ++i)
    {
        for (int k = 0; k < s.voices; ++k)
        {
            const bool toFilter = ((routed >> k) & 1) != 0;
            (toFilter ? fa : da)[i] += voices[static_cast<std::size_t> (k)][i];
            (toFilter ? fe : de)[i] += 1.0;
        }
        fv[i] = nf;
        dv[i] = 3.0 - nf;
    }
    std::vector<double> y (total, 0.0);
    v.processBuses ({ fa.data(), da.data(), fe.data(), de.data(), fv.data(), dv.data() }, static_cast<int> (total), y.data());
    const int warm = static_cast<int> (total) - kAliasN;
    y.erase (y.begin(), y.begin() + warm);
    return y;
}
} // namespace

namespace
{
/** Diagnosis: the same signal and setting through the chain with every static curve linear (the renderer without the
    bend, the filter's linear part plus the output function's linear term, the mixer without its curve; no anti-alias
    stage): the renderer's own aliasing seen through the linear filter, relative to voice 1's fundamental. */
std::vector<double> renderLinear (Model m, double rate, const Signal& s, const Setting& st)
{
    Chip c = makeChip (m, rate);
    for (int k = 0; k < s.voices; ++k)
    {
        OscInput in;
        in.frequency = s.reg[static_cast<std::size_t> (k)];
        in.pulseWidth = 2048.0f;
        in.control = static_cast<std::uint8_t> (s.wave[static_cast<std::size_t> (k)] | ctrl::gate);
        in.attackDecay = 0x00;
        in.sustainRelease = 0xf0;
        c.setOscillator (k, in);
    }
    const int routed = st.routing == 0 ? 0 : (st.routing == 1 ? 1 : 7);
    c.write (0x17, static_cast<std::uint8_t> ((st.resonance << 4) | routed));
    c.write (0x18, static_cast<std::uint8_t> (st.mode | 0x0f));
    Filter f;
    f.prepare (m, rate, 0.5f);
    f.setCutoff (static_cast<float> (st.cutoff));
    f.setResonance (st.resonance);
    f.setMode (static_cast<std::uint8_t> (st.mode | 0x0f));
    const double p1 = f.outputShapeCoefficients().p1;
    OutputStage o;
    o.prepare (m, rate, false);
    o.setRouting (static_cast<std::uint8_t> (routed));
    o.setModeVolume (static_cast<std::uint8_t> (st.mode | 0x0f));
    const int warm = std::max (16384, static_cast<int> (rate * 0.03));
    const int total = warm + kAliasN;
    std::vector<double> fa (static_cast<std::size_t> (total)), da (fa.size()), fe (fa.size()), de (fa.size()), fv (fa.size()), dv (fa.size());
    c.renderBuses ({ fa.data(), da.data(), fe.data(), de.data(), fv.data(), dv.data() }, total);
    std::vector<double> y (static_cast<std::size_t> (kAliasN));
    for (int i = 0; i < total; ++i)
    {
        const auto j = static_cast<std::size_t> (i);
        o.setVoices (fv[j], dv[j]);
        double lp = 0.0;
        const double lin = f.processLinearDouble (o.filterInput (fa[j], fe[j]), lp) + p1 * lp;
        if (i >= warm)
            y[static_cast<std::size_t> (i - warm)] = lin + o.directSumWith (da[j], de[j], dv[j]);
    }
    return y;
}
} // namespace

namespace
{
struct GateRow
{
    double db = -300.0, hz = 0.0, limit = 0.0;
    std::string what;
};

/** Whether the row drives a static curve past its limit (Model A's mixer curve below where its quintic turns, or the
    output function's low-pass value outside its range; Model B's never), simulated without anti-alias stages on the
    ideal voices: the Filter and OutputStage per internal sample, the voices' sum on their buses. */
bool reachesLimit (Model m, double rate, const Signal& s, const Setting& st, const std::array<std::vector<double>, 3>& voices)
{
    const int routed = st.routing == 0 ? 0 : (st.routing == 1 ? 1 : 7);
    Filter f;
    f.prepare (m, rate, 0.5f);
    f.setCutoff (static_cast<float> (st.cutoff));
    f.setResonance (st.resonance);
    f.setMode (static_cast<std::uint8_t> (st.mode | 0x0f));
    OutputStage o;
    o.prepare (m, rate, false);
    o.setRouting (static_cast<std::uint8_t> (routed));
    o.setModeVolume (static_cast<std::uint8_t> (st.mode | 0x0f));
    const auto lim = o.mixerLimits();
    const auto sh = f.outputShapeCoefficients();
    const double nd = 3.0 - static_cast<double> (o.filterVoices());
    const std::size_t n = std::min<std::size_t> (voices[0].size(), 40000);
    bool reached = false;
    for (std::size_t i = 0; i < n; ++i)
    {
        double fa = 0.0, da = 0.0, fe = 0.0, de = 0.0;
        for (int k = 0; k < s.voices; ++k)
        {
            const bool toFilter = ((routed >> k) & 1) != 0;
            (toFilter ? fa : da) += voices[static_cast<std::size_t> (k)][i];
            (toFilter ? fe : de) += 1.0;
        }
        double lp = 0.0;
        const double lin = f.processLinearDouble (o.filterInput (fa, fe), lp);
        const double x = o.gainFactor() * (lin + f.outputShape (lp) + o.directSumWith (da, de, nd));
        if (i > 8000 && (x < lim[0] || x > lim[1] || (m == Model::A && (lp < sh.lpMin || lp > sh.lpMax))))
            reached = true;
    }
    return reached;
}

enum class Stimulus
{
    chip,  // the real chain: Chip (its voices bent), filter, stage, anti-alias stages
    ideal  // ideal band-limited voices into the chain's buses: the static curves and the anti-alias stages alone
};

/** The gate's rows for one model and rate: the stimulus, the rows selected, printed if asked; the worst row against
    its limit (the largest db - limit), the rows and failures counted. */
struct GateTotals
{
    int rows = 0, failures = 0;           // every row; over its limit
    int limitRows = 0, limitFailures = 0; // rows driving a curve past its limit (reachesLimit); over its limit
    std::vector<std::string> limitFailed; // those rows
};

template <class Select>
GateRow runGate (Model m, double rate, Stimulus stim, Select&& select, GateTotals& t, bool print)
{
    GateRow worst;
    double worstMargin = -1e9;
    const int warm = std::max (16384, static_cast<int> (rate * 0.03));
    for (const auto& s : kSignals)
    {
        std::array<std::vector<double>, 3> voices;
        bool made = false;
        for (const auto& st : settingsFor (s))
        {
            if (! select (s, st))
                continue;
            if (! made)
            {
                for (int k = 0; k < s.voices; ++k)
                    voices[static_cast<std::size_t> (k)] = idealVoice (m, rate, s.reg[static_cast<std::size_t> (k)] * kClock / 16777216.0,
                                                                       s.wave[static_cast<std::size_t> (k)] == kSquare, 0.3 * k, warm + kAliasN);
                made = true;
            }
            const auto y = stim == Stimulus::chip ? renderSignal (m, rate, s, st) : renderIdeal (m, rate, s, st, voices);
            const double f1 = s.reg[0] * kClock / 16777216.0;
            const Alias a = aliasOfSpectrum (bhMagnitudes (y), rate, f1 / s.common, f1);
            const double limit = limitFor (m, s);
            const bool reached = reachesLimit (m, rate, s, st, voices);
            ++t.rows;
            t.limitRows += reached ? 1 : 0;
            char buf[256];
            std::snprintf (buf, sizeof (buf), "Model %s %6.0f Hz %s %-30s %-16s %-9s cutoff %4d res %2d: %7.1f dB at %7.1f Hz (limit %.0f)%s", m == Model::A ? "A" : "B", rate,
                           stim == Stimulus::chip ? "chip " : "ideal", s.name, st.routing == 0 ? "all direct" : (st.routing == 1 ? "voice 1 filtered" : "all filtered"),
                           st.routing == 0 ? "" : modeName (st.mode), st.cutoff, st.resonance, a.db, a.hz, limit, reached ? " [the mixer's limit reached]" : "");
            if (a.db > limit)
            {
                ++t.failures;
                if (reached)
                {
                    ++t.limitFailures;
                    t.limitFailed.push_back (buf);
                }
            }
            if (print)
                std::printf ("[alias-gate] %s\n", buf);
            if (a.db - limit > worstMargin)
            {
                worstMargin = a.db - limit;
                worst = { a.db, a.hz, limit, buf };
            }
        }
    }
    return worst;
}

/** The rows a real chip is gated on: all direct, and through the low-pass at cutoff 2047, resonance 0 (where the filter
    passes voice 1's fundamental and the renderer's own components alike: decisions "B-Side S3b-2" A2). */
bool chipGated (const Signal&, const Setting& st) { return st.routing == 0 || (st.mode == 0x10 && st.cutoff == 2047 && st.resonance == 0); }

void gateAt (Model m, double rate, bool allIdealRows, bool print, GateTotals& t)
{
    const auto chipRow = runGate (m, rate, Stimulus::chip, chipGated, t, print);
    const auto idealRow = runGate (m, rate, Stimulus::ideal, [allIdealRows] (const Signal& s, const Setting& st) {
        return allIdealRows || st.routing == 0 || st.resonance == 15 || (s.voices == 3 && st.cutoff == 2047);
    }, t, print);
    for (const auto* w : { &chipRow, &idealRow })
        std::printf ("[alias-gate] worst %s\n", w->what.c_str());
}
} // namespace

TEST_CASE ("alias gate: the static curves through the chain, the full set of 8.19.4 at every internal rate (hidden; about three minutes)", "[.chain-alias-full]")
{
    // 8.19.4: one voice (saw and square at 3.85 kHz, saw at 2 and 1 kHz), two voices (saw + saw 3:2, saw + square 5:4 at
    // 3.85 and 2 kHz), three voices at 3.85 and 2 kHz, full envelope; all direct, voice 1 through the filter, all through
    // it (low-, band-, high-pass, cutoff 2047 and 1024, resonance 0 and 15); 2^18 samples, 7-term Blackman-Harris,
    // 20 Hz - 20 kHz, the common fundamental's lines excluded +-10 bins, relative to voice 1's fundamental. Limits:
    // Model B -103 dB; Model A -99 dB to 2 kHz, -83 dB to 3.85 kHz. Every row is printed ([alias-gate]).
    // Gated (decisions "B-Side S3b-2" A2): every row with ideal band-limited voices in the chain's buses (Model A's bent
    // as the renderer's folded bend renders them), which isolates the static curves and their anti-alias stages; and
    // with real chips (the renderer, its bend, the chain) every row whose filter passes voice 1's fundamental and the
    // renderer's own components alike (all direct; low-pass at 2047, resonance 0). Reported, not gated: the real chips
    // through the other filter settings, beside the same signal through the chain with every curve linear (the
    // renderer's own components weighed by the filter: a high- or band-pass lowers voice 1's fundamental, the resonance
    // lifts the renderer's components near the corner).
    const auto t0 = std::chrono::steady_clock::now();
    GateTotals t;
    for (const Model m : { Model::B, Model::A })
        for (const auto& rc : rateConfigs())
            gateAt (m, rc.internalRate, true, true, t);
    // reported: real chips through every other setting, with the linear chain's figure
    int reported = 0, overLimit = 0, overLinear = 0;
    double worstExcess = -1e9;
    std::string worstExcessRow;
    for (const Model m : { Model::B, Model::A })
        for (const auto& rc : rateConfigs())
            for (const auto& s : kSignals)
                for (const auto& st : settingsFor (s))
                {
                    if (chipGated (s, st))
                        continue;
                    const double f1 = s.reg[0] * kClock / 16777216.0;
                    const Alias a = aliasOfSpectrum (bhMagnitudes (renderSignal (m, rc.internalRate, s, st)), rc.internalRate, f1 / s.common, f1);
                    const Alias l = aliasOfSpectrum (bhMagnitudes (renderLinear (m, rc.internalRate, s, st)), rc.internalRate, f1 / s.common, f1);
                    const double limit = limitFor (m, s);
                    ++reported;
                    overLimit += a.db > limit ? 1 : 0;
                    overLinear += a.db > l.db + 1.0 ? 1 : 0;
                    char buf[256];
                    std::snprintf (buf, sizeof (buf), "Model %s %6.0f Hz chip %-30s %-16s %-9s cutoff %4d res %2d: %7.1f dB at %7.1f Hz, every curve linear %7.1f dB at %7.1f Hz (limit %.0f)",
                                   m == Model::A ? "A" : "B", rc.internalRate, s.name, st.routing == 1 ? "voice 1 filtered" : "all filtered", modeName (st.mode), st.cutoff,
                                   st.resonance, a.db, a.hz, l.db, l.hz, limit);
                    std::printf ("[alias-reported] %s\n", buf);
                    if (a.db - std::max (limit, l.db) > worstExcess)
                    {
                        worstExcess = a.db - std::max (limit, l.db);
                        worstExcessRow = buf;
                    }
                }
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("[alias-gate] %d gated rows, %d over their limit; %d reported rows (real chips, other filter settings): %d over the limit, %d more than 1 dB above "
                 "the linear chain; the largest excess over max (limit, linear): %+.1f dB (%s); %.0f s\n",
                 t.rows, t.failures, reported, overLimit, overLinear, worstExcess, worstExcessRow.c_str(), secs);
    std::printf ("[alias-gate] rows driving the mixer past its limit: %d, of them over the limit: %d\n", t.limitRows, t.limitFailures);
    for (const auto& r : t.limitFailed)
        std::printf ("[alias-gate] NOT MET (the mixer's limit reached): %s\n", r.c_str());
    CHECK (t.rows == 2 * 7 * (26 + 202));
    CHECK (t.failures == 0); // the gate as 8.19.4 states it, every row: not met where the mixer's limit is reached (decisions "B-Side S3b-2" A3)
}

TEST_CASE ("alias gate: the static curves through the chain, a reduced set in the normal run (88.2 and 96 kHz)", "[chain][alias][gate]")
{
    // The hidden full set ("[.chain-alias-full]") reduced to the two lowest internal rates (the narrowest transition bands,
    // the worst rows of the full set): real chips on their gated rows; ideal voices all direct, every resonance-15 row,
    // and three voices at cutoff 2047. Rows that drive Model A's mixer past the limit of its curve (three or two voices at
    // full level through the low-pass at resonance 15) miss the limit (decisions "B-Side S3b-2" A3): reported here, and
    // asserted (failing) in the full set; every other row is asserted.
    GateTotals t;
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 88200.0, 96000.0 })
            gateAt (m, rate, false, false, t);
    std::printf ("[alias-gate] reduced set: %d rows, %d over their limit, all of them driving the mixer past its limit (%d such rows)\n", t.rows, t.failures,
                 t.limitRows);
    for (const auto& r : t.limitFailed)
        WARN ("not met, the mixer's limit reached (decisions \"B-Side S3b-2\" A3): " << r);
    CHECK (t.rows > 300);
    // every row that does not drive a curve past its limit meets 8.19.4's limit; the ones that do and miss it are the
    // known shortfall of decisions A3 (reported above; the hidden full set asserts them and fails on them)
    CHECK (t.failures == t.limitFailures);
    CHECK (t.limitRows > 0);
}

// ---- the anti-alias filters themselves ----

TEST_CASE ("alias: the half-band designs: pass band within 0.01 dB to 20 kHz, stop band at least 40 dB from 8.19.4's edge, the filters realise them", "[chain][alias][halfband]")
{
    // 8.19.4: a linear-phase half-band FIR at the internal rate, pass band to 20 kHz within 0.01 dB, stop band from
    // max (rate / 2 - pass edge, (rate - 20 kHz) / 3) with at least 40 dB; the 2x part's two half-bands at twice the
    // rate likewise (pass band to 20 kHz). Every internal rate of 4.4, on a fine grid; then the filter classes' impulse
    // responses must be the designs' taps (HalfBandFir; HalfBandUp followed by HalfBandDown: the two designs convolved),
    // and their delays the designs'.
    for (const auto& rc : rateConfigs())
    {
        const double rate = rc.internalRate;
        for (const double r : { rate, 2.0 * rate })
        {
            const HalfBandDesign d = chip::designHalfBand (r, 20000.0, 1.0e-3);
            double pass = 0.0, stop = -300.0;
            for (int i = 0; i <= 2000; ++i)
            {
                const double f = 20000.0 * i / 2000.0;
                pass = std::max (pass, std::abs (20.0 * std::log10 (std::abs (d.response (f)))));
            }
            const double edge = std::max (r / 2.0 - 20000.0, (r - 20000.0) / 3.0);
            for (int i = 0; i <= 4000; ++i)
            {
                const double f = edge + (r / 2.0 - edge) * i / 4000.0;
                stop = std::max (stop, 20.0 * std::log10 (std::abs (d.response (f)) + 1e-300));
            }
            const double dc = d.response (0.0);
            std::printf ("[halfband] at %8.0f Hz (%s): %2d pairs, delay %2d, pass band %.4f dB, stop band from %.0f Hz %.1f dB\n", r,
                         r > rate ? "2x part" : "internal rate", d.pairs, d.delay(), pass, edge, stop);
            INFO (r << " Hz: pass band " << pass << " dB, stop band " << stop << " dB, DC " << dc);
            CHECK (pass <= 0.01);
            CHECK (stop <= -40.0);
            CHECK (std::abs (dc - 1.0) < 1.0e-12);
        }
        // the classes realise the designs
        const HalfBandDesign pre = chip::designHalfBand (rate, 20000.0, 1.0e-3);
        const HalfBandDesign twice = chip::designHalfBand (2.0 * rate, 20000.0, 1.0e-3);
        chip::HalfBandFir fir;
        fir.prepare (pre);
        std::vector<double> h (static_cast<std::size_t> (4 * pre.pairs + 8), 0.0);
        for (std::size_t i = 0; i < h.size(); ++i)
            h[i] = fir.process (i == 0 ? 1.0 : 0.0);
        double worst = 0.0;
        for (std::size_t i = 0; i < h.size(); ++i)
        {
            const int k = static_cast<int> (i) - pre.delay();
            double want = 0.0;
            if (k == 0)
                want = 0.5;
            else if (std::abs (k) % 2 == 1 && std::abs (k) <= pre.delay())
                want = pre.h[static_cast<std::size_t> ((std::abs (k) - 1) / 2)];
            worst = std::max (worst, std::abs (h[i] - want));
        }
        INFO (rate << " Hz: the FIR's impulse response against the design: " << worst);
        CHECK (worst < 1.0e-15);
        CHECK (fir.delay() == pre.delay());
        // up then down: the impulse response at the internal rate is the two designs convolved at 2x, every second tap
        chip::HalfBandUp up;
        chip::HalfBandDown dn;
        up.prepare (twice);
        dn.prepare (twice);
        const int d2 = 2 * twice.delay(); // up + down at 2x
        std::vector<double> full (static_cast<std::size_t> (2 * d2 + 1), 0.0);
        auto tap = [&] (int k) {
            const int a = std::abs (k);
            return a == 0 ? 0.5 : (a % 2 == 1 && a <= twice.delay() ? twice.h[static_cast<std::size_t> ((a - 1) / 2)] : 0.0);
        };
        for (int i = 0; i <= 2 * d2; ++i)
            for (int k = -twice.delay(); k <= twice.delay(); ++k)
            {
                const int other = i - d2 / 2 - k - d2 / 2; // (i - d2) - k centred
                if (std::abs (other) <= twice.delay())
                    full[static_cast<std::size_t> (i)] += 2.0 * tap (k) * tap (other); // x2: the interpolator's gain
            }
        double worstUd = 0.0;
        for (int m = 0; m < d2 + 4; ++m)
        {
            double a = 0.0, b = 0.0;
            up.process (m == 0 ? 1.0 : 0.0, a, b);
            const double y = dn.process (a, b);
            // the output at base sample m sits at 2x time 2 m - (delay_up + delay_down); the impulse at 2x time 0
            const int at = 2 * m;
            const double want = at <= 2 * d2 ? full[static_cast<std::size_t> (at)] : 0.0;
            worstUd = std::max (worstUd, std::abs (y - want));
        }
        INFO (rate << " Hz: up + down against the convolved designs: " << worstUd);
        CHECK (worstUd < 1.0e-15);
        CHECK (up.delay2x() + dn.delay2x() == d2);
    }
}

TEST_CASE ("alias: silence in gives exact zeros out after the filters' length; a constant gives that constant", "[chain][alias][halfband]")
{
    // HalfBand.h: FIR filters, each pair taken as its difference from the centre: after a loud random input, zero input
    // gives exactly 0 once the filter's length has passed, and a constant gives exactly that constant (also through
    // the interpolator and the decimator). Then the chain (both models, volume DC off, board filters off): silent buses
    // after a loud direct-path signal give an output of exactly 0 once the anti-alias stages' length has passed (the
    // static DC and the curve's operating point cancel exactly).
    for (const auto& rc : rateConfigs())
    {
        const HalfBandDesign pre = chip::designHalfBand (rc.internalRate, 20000.0, 1.0e-3);
        const HalfBandDesign twice = chip::designHalfBand (2.0 * rc.internalRate, 20000.0, 1.0e-3);
        chip::HalfBandFir fir;
        chip::HalfBandUp up;
        chip::HalfBandDown dn;
        fir.prepare (pre);
        up.prepare (twice);
        dn.prepare (twice);
        bst::Rng r (7);
        for (int i = 0; i < 500; ++i)
        {
            const double x = (static_cast<double> (r.below (2000001)) - 1000000.0) * 1.0e-6;
            double a = 0.0, b = 0.0;
            fir.process (x);
            up.process (x, a, b);
            dn.process (a, b);
        }
        for (const double c : { 0.0, -0.2734567891 })
        {
            bool exact = true;
            for (int i = 0; i < 300; ++i)
            {
                double a = 0.0, b = 0.0;
                const double y = fir.process (c);
                up.process (c, a, b);
                const double z = dn.process (a, b);
                if (i >= 4 * pre.pairs && std::bit_cast<std::uint64_t> (y) != std::bit_cast<std::uint64_t> (c))
                    exact = false;
                if (i >= 4 * twice.pairs && (std::bit_cast<std::uint64_t> (a) != std::bit_cast<std::uint64_t> (c) || std::bit_cast<std::uint64_t> (b) != std::bit_cast<std::uint64_t> (c)
                                              || std::bit_cast<std::uint64_t> (z) != std::bit_cast<std::uint64_t> (c)))
                    exact = false;
            }
            INFO (rc.internalRate << " Hz, constant " << c);
            CHECK (exact);
        }
    }
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 88200.0, 192000.0 })
        {
            Voice p (m, rate, false);
            ChipVoice& v = *p.v;
            v.boardFiltersEnabled (false);
            v.write (0x18, 0x1f);
            const int n = 4000, loud = 1000;
            std::vector<double> fa (n, 0.0), da (n, 0.0), fe (n, 0.0), de (n, 0.0), fv (n, 0.0), dv (n, 3.0);
            for (int i = 0; i < loud; ++i)
            {
                da[static_cast<std::size_t> (i)] = 0.5 * std::sin (0.37 * i) + 0.3 * std::sin (1.91 * i);
                de[static_cast<std::size_t> (i)] = 1.0;
            }
            std::vector<double> y (n, 0.0);
            v.processBuses ({ fa.data(), da.data(), fe.data(), de.data(), fv.data(), dv.data() }, n, y.data());
            const int settled = loud + 2 * (v.latencySamples() - Chip::kLatencySamples) + 2; // the filters' whole length: twice their delay
            double worst = 0.0, peak = 0.0;
            for (int i = 0; i < n; ++i)
                (i < loud ? peak : worst) = std::max (i < loud ? peak : worst, i >= settled || i < loud ? std::abs (y[static_cast<std::size_t> (i)]) : 0.0);
            INFO ((m == Model::A ? "Model A" : "Model B") << " at " << rate << " Hz: peak " << peak << ", after the filters' length " << worst);
            CHECK (peak > 0.01);
            CHECK (! (worst > 0.0)); // exactly 0 (no float == under -Wfloat-equal)
        }
}



// ---- the wanted signal (8.19.4) ----

namespace
{
/** The complex amplitude of the component at hz in y (7-term Blackman-Harris window, its DTFT at that exact frequency,
    scaled to the sine's amplitude): no scalloping, the other lines' leakage below -150 dB beyond 7 bins. */
std::complex<double> lineAt (const std::vector<double>& y, double rate, double hz)
{
    constexpr double a[7] { 0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803, 0.01081174209837, 0.00077658482522, 0.00001388721735 };
    const double n = static_cast<double> (y.size());
    const double w = 2.0 * kPi * hz / rate;
    std::complex<double> acc, z;
    double wsum = 0.0;
    const std::complex<double> step (std::cos (w), -std::sin (w));
    for (std::size_t i = 0; i < y.size(); ++i)
    {
        if ((i & 4095) == 0)
            z = std::complex<double> (std::cos (w * static_cast<double> (i)), -std::sin (w * static_cast<double> (i)));
        double win = 0.0;
        for (int k = 0; k < 7; ++k)
            win += (k % 2 == 0 ? 1.0 : -1.0) * a[k] * std::cos (2.0 * kPi * k * static_cast<double> (i) / n);
        acc += y[i] * win * z;
        wsum += win;
        z *= step;
    }
    return 2.0 * acc / wsum;
}

double db (double v) { return 20.0 * std::log10 (v + 1e-300); }

/** The buses of one test voice on the direct bus (oscillators 2 and 3 silent there too) or on the filter bus (filterBus:
    oscillator 1 routed, the others direct): the signal, its envelope at full level. */
struct Buses
{
    std::vector<double> fa, da, fe, de, fv, dv;
    Buses (const std::vector<double>& x, bool filterBus)
        : fa (x.size(), 0.0), da (x.size(), 0.0), fe (x.size(), 0.0), de (x.size(), 0.0), fv (x.size(), filterBus ? 1.0 : 0.0), dv (x.size(), filterBus ? 2.0 : 3.0)
    {
        (filterBus ? fa : da) = x;
        (filterBus ? fe : de).assign (x.size(), 1.0);
    }
    Chip::BusBlock block() { return { fa.data(), da.data(), fe.data(), de.data(), fv.data(), dv.data() }; }
};

/** The chain (board filters off) over the buses; the first `skip` samples dropped. */
std::vector<double> chainOver (Model m, double rate, Buses& b, int skip, std::uint8_t modeVolume = 0x1f, int cutoff = 2047)
{
    Voice p (m, rate, false);
    ChipVoice& v = *p.v;
    v.boardFiltersEnabled (false);
    v.write (0x15, static_cast<std::uint8_t> (cutoff & 7));
    v.write (0x16, static_cast<std::uint8_t> (cutoff >> 3));
    v.write (0x18, modeVolume);
    std::vector<double> y (b.fa.size(), 0.0);
    v.processBuses (b.block(), static_cast<int> (y.size()), y.data());
    y.erase (y.begin(), y.begin() + skip);
    return y;
}

/** One period (of commonHz) of the exact curve over continuous sinusoids: M samples of y (t) = f (t), its FFT; the
    complex amplitude of harmonic k of commonHz is 2 Y[k] / M. */
template <class F>
std::vector<std::complex<double>> exactPeriod (F&& f, int m)
{
    std::vector<std::complex<double>> c (static_cast<std::size_t> (m));
    for (int j = 0; j < m; ++j)
        c[static_cast<std::size_t> (j)] = f (static_cast<double> (j) / m);
    fft (c);
    return c;
}
} // namespace

TEST_CASE ("alias: the wanted signal's pass band through the chain: within +-0.05 dB from 20 Hz to 20 kHz, as the designs say", "[chain][alias][wanted]")
{
    // A sine of 1e-3 (the curves linear around their operating point) on the direct bus, through the whole chain (board
    // filters off), at 20 Hz .. 20 kHz: its gain against the gain at 1 kHz within +-0.05 dB (8.19.4), and within 0.002 dB
    // of the anti-alias filters' designs (Model B the mixer FIR; Model A the bus FIR, the interpolator, the decimator).
    // Every internal rate, both models.
    const double freqs[] { 20.0, 50.0, 200.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0, 19000.0, 19700.0, 20000.0 };
    const int n = 1 << 19, skip = 4096;
    for (const Model m : { Model::B, Model::A })
        for (const auto& rc : rateConfigs())
        {
            const double rate = rc.internalRate;
            const HalfBandDesign pre = chip::designHalfBand (rate, 20000.0, 1.0e-3);
            const HalfBandDesign up = chip::designHalfBand (2.0 * rate, 20000.0, 1.0e-3);
            const HalfBandDesign down = up; // the decimator's design is the interpolator's
            auto design = [&] (double hz) { return m == Model::B ? pre.response (hz) : pre.response (hz) * up.response (hz) * down.response (hz); };
            std::vector<double> gain;
            for (const double hz : freqs)
            {
                std::vector<double> x (static_cast<std::size_t> (n + skip));
                for (std::size_t i = 0; i < x.size(); ++i)
                    x[i] = 1.0e-3 * std::sin (2.0 * kPi * hz * static_cast<double> (i) / rate);
                Buses b (x, false);
                gain.push_back (std::abs (lineAt (chainOver (m, rate, b, skip), rate, hz)));
            }
            double worst = 0.0, worstDesign = 0.0;
            for (std::size_t k = 0; k < gain.size(); ++k)
            {
                const double rel = db (gain[k] / gain[3]);
                worst = std::max (worst, std::abs (rel));
                worstDesign = std::max (worstDesign, std::abs (rel - db (std::abs (design (freqs[k]) / design (1000.0)))));
            }
            INFO ((m == Model::A ? "Model A" : "Model B") << " at " << rate << " Hz: the pass band's largest deviation " << worst << " dB, from the designs "
                                                          << worstDesign << " dB");
            CHECK (worst <= 0.05);
            CHECK (worstDesign <= 0.002);
        }
}

TEST_CASE ("alias: the wanted signal: a 6.6 kHz sine and a sine pair, the curves' products within 0.5 dB of the exact curves", "[chain][alias][wanted]")
{
    // 8.19.4: the products of a sine (6.6 kHz) and a sine pair (6.6 + 8.1 kHz) through the curves, against the exact
    // curves applied to the continuous sinusoids (one common period sampled finely, its FFT): every product in 20 Hz ..
    // 20 kHz whose exact level is above -100 dB re the first sine, within 0.5 dB. Model B and Model A on the direct bus
    // (the mixer curve); Model A also through the filter (low-pass, cutoff 2047, resonance 0: the output function and
    // the mixer curve), the filter's linear part and low-pass value measured from the same Filter at the internal rate.
    // Every internal rate.
    struct Case
    {
        const char* name;
        std::vector<std::pair<double, double>> tones; // hz, amplitude
        double common;
    };
    const Case cases[] { { "6.6 kHz", { { 6600.0, 0.45 } }, 6600.0 }, { "6.6 + 8.1 kHz", { { 6600.0, 0.3 }, { 8100.0, 0.2 } }, 300.0 } };
    const int n = 1 << 18, skip = 8192;
    int compared = 0;
    double worstAll = 0.0;
    for (const Model m : { Model::B, Model::A })
        for (const bool viaFilter : { false, true })
        {
            if (viaFilter && m == Model::B)
                continue;
            for (const auto& rc : rateConfigs())
                for (const auto& cs : cases)
                {
                    const double rate = rc.internalRate;
                    std::vector<double> x (static_cast<std::size_t> (n + skip), 0.0);
                    for (std::size_t i = 0; i < x.size(); ++i)
                        for (const auto& [hz, amp] : cs.tones)
                            x[i] += amp * std::sin (2.0 * kPi * hz * static_cast<double> (i) / rate);
                    Buses b (x, viaFilter);
                    const auto y = chainOver (m, rate, b, skip);
                    // the exact curves: the stage's gain and curve, the filter's linear output and low-pass value as sinusoids
                    OutputStage o;
                    o.prepare (m, rate, false);
                    o.setRouting (viaFilter ? 0x01 : 0x00);
                    o.setModeVolume (0x1f);
                    Filter f;
                    f.prepare (m, rate, 0.5f);
                    f.setCutoff (2047.0f);
                    f.setMode (0x1f);
                    std::vector<double> lin (x.size()), lp (x.size());
                    for (std::size_t i = 0; i < x.size(); ++i)
                        lin[i] = f.processLinearDouble (o.filterInput (b.fa[i], b.fe[i]), lp[i]);
                    lin.erase (lin.begin(), lin.begin() + skip);
                    lp.erase (lp.begin(), lp.begin() + skip);
                    double lin0 = 0.0, lp0 = 0.0;
                    for (std::size_t i = 0; i < lin.size(); ++i)
                    {
                        lin0 += lin[i];
                        lp0 += lp[i];
                    }
                    lin0 /= static_cast<double> (lin.size());
                    lp0 /= static_cast<double> (lp.size());
                    std::vector<std::complex<double>> linA, lpA;
                    for (const auto& tone : cs.tones)
                    {
                        linA.push_back (lineAt (lin, rate, tone.first));
                        lpA.push_back (lineAt (lp, rate, tone.first));
                    }
                    const double g = o.gainFactor();
                    const double direct0 = o.directSumWith (0.0, viaFilter ? 0.0 : 1.0, viaFilter ? 2.0 : 3.0);
                    const int periods = static_cast<int> (std::lround (cs.tones[0].first / cs.common));
                    const auto ex = exactPeriod ([&] (double u) {
                        // u: the time in common periods; the sines in phase with the chain's input (sin at sample 0)
                        double dir = direct0, l = viaFilter ? lin0 : 0.0, q = viaFilter ? lp0 : 0.0;
                        for (std::size_t k = 0; k < cs.tones.size(); ++k)
                        {
                            const double ph = 2.0 * kPi * cs.tones[k].first / cs.common * u;
                            const std::complex<double> e (std::cos (ph), std::sin (ph));
                            if (viaFilter)
                            {
                                l += (linA[k] * e).real(); // the projections' phase is the signal's at sample 0
                                q += (lpA[k] * e).real();
                            }
                            else
                                dir += cs.tones[k].second * std::sin (ph);
                        }
                        return o.mixerCurve (g * (l + f.outputShape (q) + dir));
                    }, 1 << 14);
                    (void) periods;
                    const double ref = std::abs (ex[static_cast<std::size_t> (std::lround (cs.tones[0].first / cs.common))]) * 2.0 / (1 << 14);
                    const double refChain = std::abs (lineAt (y, rate, cs.tones[0].first));
                    double worst = 0.0;
                    for (int k = 1; static_cast<double> (k) * cs.common <= 20000.0; ++k)
                    {
                        const double hz = k * cs.common;
                        bool tone = false;
                        for (const auto& t : cs.tones)
                            tone = tone || std::abs (t.first - hz) < 1.0;
                        if (tone)
                            continue;
                        const double exactDb = db (std::abs (ex[static_cast<std::size_t> (k)]) * 2.0 / (1 << 14) / ref);
                        if (exactDb < -100.0)
                            continue;
                        const double chainDb = db (std::abs (lineAt (y, rate, hz)) / refChain);
                        ++compared;
                        worst = std::max (worst, std::abs (chainDb - exactDb));
                    }
                    worstAll = std::max (worstAll, worst);
                    INFO ((m == Model::A ? "Model A" : "Model B") << (viaFilter ? " through the filter" : " direct") << " at " << rate << " Hz, " << cs.name
                                                                  << ": products' largest deviation from the exact curves " << worst << " dB");
                    CHECK (worst <= 0.5);
                }
        }
    std::printf ("[wanted] sines: %d products compared, largest deviation %.3f dB\n", compared, worstAll);
    CHECK (compared > 50);
}

TEST_CASE ("alias: the wanted signal: sawtooth and square harmonics above -40 dB within 0.5 dB of the exact curve on band-limited voices", "[chain][alias][wanted]")
{
    // 8.19.4: a band-limited sawtooth and square (every harmonic below half the internal rate; Model A's bent as the
    // renderer bends them) on the direct bus through the chain, against the exact mixer curve applied to the same
    // band-limited voice (continuous; one period sampled finely, its FFT): the harmonics in 20 Hz .. 20 kHz whose exact
    // level is above -40 dB re the fundamental within 0.5 dB. Weaker ones may move (decisions B2): printed. 1 kHz and
    // 3.85 kHz, every internal rate, both models.
    int compared = 0;
    double worstAll = 0.0, worstWeak = 0.0;
    const int n = 1 << 18;
    for (const Model m : { Model::B, Model::A })
        for (const auto& rc : rateConfigs())
            for (const bool square : { false, true })
                for (const double hz : { 17025.0 * kClock / 16777216.0, 65535.0 * kClock / 16777216.0 })
                {
                    const double rate = rc.internalRate;
                    const int skip = 8192;
                    const auto x = idealVoice (m, rate, hz, square, 0.0, n + skip);
                    Buses b (x, false);
                    const auto y = chainOver (m, rate, b, skip);
                    OutputStage o;
                    o.prepare (m, rate, false);
                    o.setRouting (0x00);
                    o.setModeVolume (0x1f);
                    const double g = o.gainFactor(), direct0 = o.directSumWith (0.0, 1.0, 3.0);
                    // the same band-limited voice, continuous: its harmonics (idealVoice's own coefficients)
                    const auto one = idealVoice (m, 64.0 * 4096.0, 64.0, square, 0.0, 4096); // one period, 4096 points, every harmonic below 2048
                    std::vector<std::complex<double>> c (one.begin(), one.end());
                    fft (c);
                    const int harmonics = static_cast<int> (std::floor ((rate / 2.0 - 1.0) / hz));
                    const auto ex = exactPeriod ([&] (double u) {
                        double v = c[0].real() / 4096.0;
                        for (int k = 1; k <= harmonics; ++k)
                        {
                            const std::complex<double> a = 2.0 * c[static_cast<std::size_t> (k)] / 4096.0;
                            v += (a * std::complex<double> (std::cos (2.0 * kPi * k * u), std::sin (2.0 * kPi * k * u))).real();
                        }
                        return o.mixerCurve (g * (v + direct0));
                    }, 8192);
                    const double ref = std::abs (ex[1]);
                    const double refChain = std::abs (lineAt (y, rate, hz));
                    double worst = 0.0;
                    for (int k = 2; k * hz <= 20000.0; ++k)
                    {
                        const double exactDb = db (std::abs (ex[static_cast<std::size_t> (k)]) / ref);
                        const double chainDb = db (std::abs (lineAt (y, rate, k * hz)) / refChain);
                        if (exactDb > -40.0)
                        {
                            ++compared;
                            worst = std::max (worst, std::abs (chainDb - exactDb));
                        }
                        else if (exactDb > -100.0)
                            worstWeak = std::max (worstWeak, std::abs (chainDb - exactDb));
                    }
                    worstAll = std::max (worstAll, worst);
                    INFO ((m == Model::A ? "Model A " : "Model B ") << (square ? "square " : "saw ") << hz << " Hz at " << rate << " Hz: harmonics' largest deviation " << worst << " dB");
                    CHECK (worst <= 0.5);
                }
    std::printf ("[wanted] harmonics: %d above -40 dB compared, largest deviation %.3f dB; weaker ones (-40 .. -100 dB) move by up to %.2f dB\n", compared, worstAll,
                 worstWeak);
    CHECK (compared > 100);
}

// ---- latency, and the configurations blended across a write ----

TEST_CASE ("alias: the chain's latency is where the signal lands: an impulse through the anti-alias stages, a step through the whole chain (Per Note and Shared)", "[chain][alias][latency]")
{
    // latencySamples () = the renderer's 8 + the anti-alias stages' delay (ChipVoice.h "LATENCY"). (1) An impulse of 1e-3
    // on the direct bus (ChipVoice::processBuses, every stage linear-phase and the curves smooth): the centre of gravity
    // of the output's response lies latencySamples () - 8 samples after the impulse, to 1e-6 samples. (2) A step of a
    // frozen oscillator (the test bit raising a pulse, sustain 1) through the whole chain, Per Note and Shared (two
    // identical chips on one filter): the step's centre lies latencySamples () - 8 samples after the renderer's own
    // (Chip::renderBuses of a twin chip, its voices bent like the chain's), to 0.02 samples (one cycle is 0.1 sample at
    // 96 kHz); one cycle earlier or later is outside. Both models; impulses at every internal rate, steps at three.
    for (const Model m : { Model::B, Model::A })
        for (const auto& rc : rateConfigs())
        {
            const double rate = rc.internalRate;
            const int n = 2048, at = 700;
            std::vector<double> x (static_cast<std::size_t> (n), 0.0);
            x[static_cast<std::size_t> (at)] = 1.0e-3;
            Buses b (x, false);
            Buses still (std::vector<double> (static_cast<std::size_t> (n), 0.0), false);
            const auto y = chainOver (m, rate, b, 0), y0 = chainOver (m, rate, still, 0);
            double s0 = 0.0, s1 = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double d = y[static_cast<std::size_t> (i)] - y0[static_cast<std::size_t> (i)];
                s0 += d;
                s1 += d * i;
            }
            const int latency = Voice (m, rate, false).v->latencySamples();
            const double centre = s1 / s0 - at;
            INFO ((m == Model::A ? "Model A" : "Model B") << " at " << rate << " Hz: impulse centre " << centre << ", latency " << latency);
            CHECK (std::abs (centre - (latency - Chip::kLatencySamples)) < 1.0e-6);
        }
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 88200.0, 96000.0, 192000.0 })
        {
            const auto w = static_cast<std::uint64_t> (0.05 * kClock) + 3;
            auto setup = [] (auto& target) {
                OscInput in;
                in.frequency = 0.0;
                in.pulseWidth = 2048.0f;
                in.control = ctrl::pulse | ctrl::gate;
                in.attackDecay = 0x00;
                in.sustainRelease = 0x10; // sustain 1
                target.setOscillator (0, in);
                target.write (0x18, 0x0f); // volume 15, no filter mode
            };
            const int total = static_cast<int> (rate * 0.08);
            // the renderer's own step (its buses' sum, voices bent as in the chain)
            Chip twin = makeChip (m, rate);
            twin.renderBendVoices (true);
            setup (twin);
            std::vector<double> fa (static_cast<std::size_t> (total)), da (fa.size()), fe (fa.size()), de (fa.size()), fv (fa.size()), dv (fa.size());
            int done = twin.renderBusesUntil ({ fa.data(), da.data(), fe.data(), de.data(), fv.data(), dv.data() }, total, w);
            twin.write (0x04, ctrl::pulse | ctrl::test | ctrl::gate);
            twin.renderBusesUntil ({ fa.data() + done, da.data() + done, fe.data() + done, de.data() + done, fv.data() + done, dv.data() + done }, total - done, ~std::uint64_t { 0 });
            std::vector<double> r (fa.size());
            for (std::size_t i = 0; i < r.size(); ++i)
                r[i] = fa[i] + da[i];
            auto centreOf = [] (const std::vector<double>& y, int from, int to) {
                double s0 = 0.0, s1 = 0.0;
                for (int i = from; i < to; ++i)
                {
                    const double d = y[static_cast<std::size_t> (i)] - y[static_cast<std::size_t> (i - 1)];
                    s0 += d;
                    s1 += d * i;
                }
                return s1 / s0;
            };
            const double cps = static_cast<double> (twin.sampleClock().increment()) / 4294967296.0;
            const int around = static_cast<int> (static_cast<double> (w) / cps);
            const double rendered = centreOf (r, around - 60, around + 60);
            // Per Note
            Voice p (m, rate, false);
            p.v->boardFiltersEnabled (false);
            setup (*p.v);
            std::vector<double> y (static_cast<std::size_t> (total), 0.0);
            done = p.v->renderUntil (y.data(), total, w);
            p.v->write (0x04, ctrl::pulse | ctrl::test | ctrl::gate);
            p.v->renderUntil (y.data() + done, total - done, ~std::uint64_t { 0 });
            const int extra = p.v->latencySamples() - Chip::kLatencySamples;
            const double perNote = centreOf (y, around - 60 + extra, around + 60 + 2 * extra) - rendered;
            // Shared: two identical chips on one filter (ChipVoice.h "TOPOLOGY")
            std::vector<std::unique_ptr<ChipVoice>> v;
            for (int k = 0; k < 2; ++k)
            {
                chip::Config cfg;
                cfg.model = m;
                cfg.internalRate = rate;
                v.push_back (std::make_unique<ChipVoice>());
                v.back()->prepare (cfg);
                v.back()->boardFiltersEnabled (false);
                setup (*v.back());
            }
            Filter sf;
            sf.prepare (m, rate, 0.5f);
            std::vector<double> ys;
            bool written = false;
            while (static_cast<int> (ys.size()) < total)
            {
                const int want = std::min (ChipVoice::kBlock, total - static_cast<int> (ys.size()));
                int got = 0;
                for (auto& x : v)
                    got = x->renderBusesUntil (want, written ? ~std::uint64_t { 0 } : w);
                std::array<double, ChipVoice::kBlock> so {}, sl {}, ss {}, out {};
                for (int i = 0; i < got; ++i)
                {
                    const double dc = v[0]->staticFilterBusDc (i);
                    double in = dc;
                    for (auto& x : v)
                        in += x->filterInputAc (i);
                    so[static_cast<std::size_t> (i)] = sf.processLinearDouble (in, sl[static_cast<std::size_t> (i)]);
                    ss[static_cast<std::size_t> (i)] = sf.staticOutput (dc);
                }
                for (auto& x : v)
                    x->finishShared (sf, so.data(), sl.data(), ss.data(), 2, out.data(), got);
                ys.insert (ys.end(), out.begin(), out.begin() + got);
                if (! written && v[0]->chip().cycle() == w)
                {
                    for (auto& x : v)
                        x->write (0x04, ctrl::pulse | ctrl::test | ctrl::gate);
                    written = true;
                }
            }
            const double shared = centreOf (ys, around - 60 + extra, around + 60 + 2 * extra) - rendered;
            const double cycle = 1.0 / cps;
            INFO ((m == Model::A ? "Model A" : "Model B") << " at " << rate << " Hz: the step lands " << perNote << " (Per Note) and " << shared
                                                          << " (Shared) samples after the renderer's; latency - 8 = " << extra << ", one cycle " << cycle);
            CHECK (std::abs (perNote - extra) < 0.02);
            CHECK (std::abs (shared - extra) < 0.02);
            CHECK (cycle > 0.04);
        }
}

TEST_CASE ("alias: configurations blended across a volume write share the anti-alias stages: the ideal band-limited switch, the post-curve coefficients delayed with the signal", "[chain][alias][blend]")
{
    // ChipVoice.h "ALIAS": the configurations of a volume or mode write are blended before the anti-alias stages, what
    // follows the curve as coefficients delayed with the signal; for a volume write (only post-curve coefficients
    // differ) that is exact. A saw through the filter (low-pass at 1024) and one direct, a volume write 15 -> 6 (volume
    // DC on: the mixer's volume; off: the post gain): the output (board filters off) equals the outputs held at the old
    // and the new volume switched by the long kernel's band-limited step at the write's place (one cycle before its
    // cycle, latencySamples () late) to 1e-6 of the volume's change; a switch placed one sample off is far worse.
    // Both models, 88.2, 96 and 192 kHz.
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 88200.0, 96000.0, 192000.0 })
            for (const bool vdc : { true, false })
            {
                const std::uint64_t w = 30011;
                auto run = [&] (std::uint8_t from, std::uint8_t to) {
                    Voice p (m, rate, vdc);
                    ChipVoice& v = *p.v;
                    v.boardFiltersEnabled (false);
                    voiceOsc (v, 0, 7480.0, 2048.0f, ctrl::saw | ctrl::gate);
                    voiceOsc (v, 1, 5001.0, 2048.0f, ctrl::saw | ctrl::gate);
                    v.write (0x15, 0);
                    v.write (0x16, 0x80);
                    v.write (0x17, 0x81);
                    v.write (0x18, from);
                    const int total = static_cast<int> (rate * 0.06);
                    std::vector<double> y (static_cast<std::size_t> (total), 0.0);
                    int done = v.renderUntil (y.data(), total, w);
                    v.write (0x18, to);
                    v.renderUntil (y.data() + done, total - done, ~std::uint64_t { 0 });
                    return std::pair { y, v.latencySamples() };
                };
                const auto [before, latency] = run (0x1f, 0x1f);
                const auto after = run (0x16, 0x16).first, step = run (0x1f, 0x16).first;
                const double cps = static_cast<double> (makeChip (m, rate).sampleClock().increment()) / 4294967296.0;
                auto blepCum = [] (double t) {
                    constexpr double z = Chip::LongBlep::kZeroCrossings;
                    if (t <= -z)
                        return 0.0;
                    if (t > z)
                        return 1.0;
                    const int j = static_cast<int> (std::ceil (t)) - 1;
                    return Chip::LongBlep::get().step (j, t - j) + (j >= 0 ? 1.0 : 0.0);
                };
                auto deviation = [&] (double shiftSamples) {
                    double worst = 0.0;
                    const double pos = (static_cast<double> (w) - 1.0) / cps + latency + shiftSamples;
                    for (std::size_t s = 200; s < step.size(); ++s)
                        worst = std::max (worst, std::abs (step[s] - (before[s] + (after[s] - before[s]) * blepCum (static_cast<double> (s) - pos))));
                    return worst;
                };
                double size = 0.0;
                for (std::size_t s = 200; s < step.size(); ++s)
                    size = std::max (size, std::abs (after[s] - before[s]));
                const double exact = deviation (0.0), off = std::min (deviation (-1.0), deviation (1.0));
                INFO ((m == Model::A ? "Model A" : "Model B") << " at " << rate << " Hz, volume DC " << (vdc ? "on" : "off") << ": the volume's change " << size
                                                              << ", deviation from the ideal switch " << exact << " (one sample off " << off << ")");
                CHECK (size > 1.0e-3);
                CHECK (exact <= 1.0e-6 * size); // the kernel's step read two ways (the table's own step here, the renderer's Hermite read there): as "a volume write with volume DC on ..." in ChipVoiceTests.cpp
                CHECK (off > 100.0 * exact);
            }
}


TEST_CASE ("alias: the voice bend folded into the renderer keeps the harmonics of the exact bend of the band-limited voice", "[chain][alias][wanted][bend]")
{
    // Chip.h "BEND", 8.19.4: Model A, a saw and a square (sustain 15: envelope value 1) at about 1 kHz and 3 kHz, the
    // buses with the bend folded in (renderBendVoices) against the same chip's unbent buses bent sample by sample
    // (render::bend: the exact curve on the band-limited voice; its aliases lie off the harmonics): every harmonic in
    // 20 Hz .. 20 kHz above -40 dB re the fundamental within 0.5 dB; and the folded voice carries the bend: its mean moves
    // from the unbent voice's by the bend's DC (a2 <w^2> + a3 <w^3>: about -0.014 for the saw, -0.044 for the square),
    // within 2e-3 of the per-sample bend's (which differs by the band-limited voice's missing power, the folded one
    // keeps the naive waveform's).
    const int n = 1 << 17, skip = 2048;
    int compared = 0;
    double worstAll = 0.0;
    for (const double rate : { 88200.0, 96000.0, 192000.0 })
        for (const std::uint8_t wave : { ctrl::saw, ctrl::pulse })
            for (const double reg : { 17100.0, 51100.0 })
            {
                auto render = [&] (bool bend) {
                    Chip c = makeChip (Model::A, rate);
                    c.renderBendVoices (bend);
                    OscInput in;
                    in.frequency = reg;
                    in.pulseWidth = 2048.0f;
                    in.control = static_cast<std::uint8_t> (wave | ctrl::gate);
                    in.attackDecay = 0x00;
                    in.sustainRelease = 0xf0;
                    c.setOscillator (0, in);
                    std::vector<double> fa (static_cast<std::size_t> (n + skip)), da (fa.size()), fe (fa.size()), de (fa.size()), fv (fa.size()), dv (fa.size());
                    c.renderBuses ({ fa.data(), da.data(), fe.data(), de.data(), fv.data(), dv.data() }, n + skip);
                    std::vector<double> y (static_cast<std::size_t> (n));
                    for (std::size_t i = 0; i < y.size(); ++i)
                        y[i] = da[i + static_cast<std::size_t> (skip)];
                    return y;
                };
                const auto folded = render (true), plain = render (false);
                std::vector<double> exact (plain.size());
                for (std::size_t i = 0; i < plain.size(); ++i)
                    exact[i] = chip::render::bend (plain[i]); // envelope value 1
                const double hz = reg * kClock / 16777216.0;
                const double refF = std::abs (lineAt (folded, rate, hz)), refE = std::abs (lineAt (exact, rate, hz)), refP = std::abs (lineAt (plain, rate, hz));
                double worst = 0.0;
                for (int k = 2; k * hz <= 20000.0; ++k)
                {
                    const double e = db (std::abs (lineAt (exact, rate, k * hz)) / refE);
                    if (e < -40.0)
                        continue;
                    ++compared;
                    worst = std::max (worst, std::abs (db (std::abs (lineAt (folded, rate, k * hz)) / refF) - e));
                }
                double mf = 0.0, me = 0.0, mp = 0.0;
                for (std::size_t i = 0; i < plain.size(); ++i)
                {
                    mf += folded[i];
                    me += exact[i];
                    mp += plain[i];
                }
                mf /= static_cast<double> (plain.size());
                me /= static_cast<double> (plain.size());
                mp /= static_cast<double> (plain.size());
                (void) refP;
                worstAll = std::max (worstAll, worst);
                INFO (rate << " Hz, " << (wave == ctrl::saw ? "saw " : "square ") << hz << " Hz: harmonics' largest deviation " << worst << " dB; mean folded " << mf
                            << ", exact bend " << me << ", unbent " << mp);
                CHECK (worst <= 0.5);
                CHECK (std::abs (mf - me) < 2.0e-3);
                CHECK (std::abs (mf - mp) > 0.01);
            }
    std::printf ("[bend] %d harmonics above -40 dB compared, largest deviation %.3f dB\n", compared, worstAll);
    CHECK (compared > 20);
}
