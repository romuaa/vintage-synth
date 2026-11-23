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

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <map>
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

/** The phase constellations of the gate (8.19.4 after the second review): voice k of a multi-voice row starts at
    phase k x step (radians of its fundamental). 0: every voice at phase 0, a common reset; 0.3: the first closing
    round's (the only one then); 1.1, 2.0, -0.7: the second review's, among them the worst it found (2.0: three voices
    at 2 kHz at 88.2 and 96 kHz; 0: three voices at 3.85 kHz and at 128 kHz). One-voice rows have one (phase does not
    matter). */
constexpr double kPhaseSteps[] { 0.0, 0.3, 1.1, 2.0, -0.7 };

/** Real chips: voice k starts (its oscillator set, its envelope gated) d[k] samples after voice 1, so that its
    fundamental lags by f_k d_k, close to the constellation's k x step (to the sample: about 1/48 period at 2 kHz and
    96 kHz). Step 0: every voice starts at once from the power-on accumulator (a common reset). */
std::array<int, 3> startDelays (double rate, const Signal& s, double step)
{
    std::array<int, 3> d {};
    for (int k = 1; k < s.voices; ++k)
    {
        const double f = s.reg[static_cast<std::size_t> (k)] * kClock / 16777216.0;
        double frac = std::fmod (-step * k / (2.0 * kPi), 1.0);
        if (frac < 0.0)
            frac += 1.0;
        d[static_cast<std::size_t> (k)] = static_cast<int> (std::lround (frac * rate / f));
    }
    return d;
}

/** Starts the signal's voices at their delays (startDelays), rendering in between with `renderFor (n)`. Returns the
    samples rendered so far. */
template <class SetOsc, class RenderFor>
int startVoices (double rate, const Signal& s, double step, SetOsc&& setOsc, RenderFor&& renderFor)
{
    const auto d = startDelays (rate, s, step);
    std::array<int, 3> order { 0, 1, 2 };
    std::sort (order.begin(), order.begin() + s.voices, [&] (int a, int b) { return d[static_cast<std::size_t> (a)] < d[static_cast<std::size_t> (b)]; });
    int at = 0;
    for (int i = 0; i < s.voices; ++i)
    {
        const int k = order[static_cast<std::size_t> (i)];
        if (d[static_cast<std::size_t> (k)] > at)
        {
            renderFor (at, d[static_cast<std::size_t> (k)] - at);
            at = d[static_cast<std::size_t> (k)];
        }
        setOsc (k);
    }
    return at;
}

/** The chain's output before the board filters, kAliasN samples after a settling time; the voices started in the
    constellation `step` (startDelays). */
std::vector<double> renderSignal (Model m, double rate, const Signal& s, const Setting& st, double step = 0.0)
{
    Voice p (m, rate, false);
    ChipVoice& v = *p.v;
    v.boardFiltersEnabled (false);
    v.write (0x15, static_cast<std::uint8_t> (st.cutoff & 7));
    v.write (0x16, static_cast<std::uint8_t> (st.cutoff >> 3));
    const int routed = st.routing == 0 ? 0 : (st.routing == 1 ? 1 : 7);
    v.write (0x17, static_cast<std::uint8_t> ((st.resonance << 4) | routed));
    v.write (0x18, static_cast<std::uint8_t> (st.mode | 0x0f));
    std::vector<double> pre;
    startVoices (
        rate, s, step,
        [&] (int k) {
            voiceOsc (v, k, s.reg[static_cast<std::size_t> (k)], 2048.0f, static_cast<std::uint8_t> (s.wave[static_cast<std::size_t> (k)] | ctrl::gate));
        },
        [&] (int, int n) {
            pre.assign (static_cast<std::size_t> (n), 0.0);
            v.render (pre.data(), n);
        });
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

/** The six bus streams a chip renders (Chip::renderBuses) or test code builds from ideal voices. */
struct BusStreams
{
    std::vector<double> fa, da, fe, de, fv, dv;
};

/** The buses of ideal voices, as renderSignal sets the chip up: the buses of the routing (the oscillators routed,
    sounding or not, count on the filter bus), envelope sums of the sounding voices. */
BusStreams idealBuses (const Signal& s, const Setting& st, const std::array<std::vector<double>, 3>& voices)
{
    const int routed = st.routing == 0 ? 0 : (st.routing == 1 ? 1 : 7);
    const std::size_t total = voices[0].size();
    BusStreams b { std::vector<double> (total, 0.0), std::vector<double> (total, 0.0), std::vector<double> (total, 0.0),
                   std::vector<double> (total, 0.0), std::vector<double> (total, 0.0), std::vector<double> (total, 0.0) };
    double nf = 0.0;
    for (int k = 0; k < 3; ++k)
        nf += ((routed >> k) & 1) != 0 ? 1.0 : 0.0;
    for (std::size_t i = 0; i < total; ++i)
    {
        for (int k = 0; k < s.voices; ++k)
        {
            const bool toFilter = ((routed >> k) & 1) != 0;
            (toFilter ? b.fa : b.da)[i] += voices[static_cast<std::size_t> (k)][i];
            (toFilter ? b.fe : b.de)[i] += 1.0;
        }
        b.fv[i] = nf;
        b.dv[i] = 3.0 - nf;
    }
    return b;
}

/** The chain run on ideal voices (ChipVoice::processBuses) with the buses of idealBuses. */
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
    BusStreams b = idealBuses (s, st, voices);
    const std::size_t total = b.fa.size();
    std::vector<double> y (total, 0.0);
    v.processBuses ({ b.fa.data(), b.da.data(), b.fe.data(), b.de.data(), b.fv.data(), b.dv.data() }, static_cast<int> (total), y.data());
    const int warm = static_cast<int> (total) - kAliasN;
    y.erase (y.begin(), y.begin() + warm);
    return y;
}
} // namespace

namespace
{
/** The reference of statement (c) (docs/architecture.md 8.19.4, after the review): the same chain with its curves
    evaluated at four times the internal rate through filters of at least 120 dB, test code only. A real chip (Model A
    bent as in the chain) renders the buses; the filter and the stage run as in ChipVoice (Model A: the same bus
    pre-filter); the mixer input (Model A: the filter's linear output and low-pass value) is interpolated by two
    half-band stages of 120 dB, the curves run at 4x, two half-band stages of 120 dB decimate. What remains above the
    chain's own figure is the curves' aliasing. With `buses`, those buses replace the chip's (statement (a): ideal
    voices); with `range`, the mixer input's lowest and highest value at 4x over the measured samples (the pinned
    rows' check that the input leaves the range the curve was fitted on). */
std::vector<double> renderReference4x (Model m, double rate, const Signal& s, const Setting& st, const BusStreams* buses = nullptr,
                                       std::array<double, 2>* range = nullptr, double step = 0.0)
{
    Chip c = makeChip (m, rate);
    c.renderBendVoices (true);
    const int routed = st.routing == 0 ? 0 : (st.routing == 1 ? 1 : 7);
    c.write (0x17, static_cast<std::uint8_t> ((st.resonance << 4) | routed));
    c.write (0x18, static_cast<std::uint8_t> (st.mode | 0x0f));
    Filter f;
    f.prepare (m, rate, 0.5f);
    f.setCutoff (static_cast<float> (st.cutoff));
    f.setResonance (st.resonance);
    f.setMode (static_cast<std::uint8_t> (st.mode | 0x0f));
    const auto sh = f.outputShapeCoefficients();
    OutputStage o;
    o.prepare (m, rate, false);
    o.setRouting (static_cast<std::uint8_t> (routed));
    o.setModeVolume (static_cast<std::uint8_t> (st.mode | 0x0f));
    const auto pre = chip::designHalfBand (rate, ChipVoice::kAntiAliasPassHz, ChipVoice::kAntiAliasDeviation);
    const auto d2 = chip::designHalfBand (2.0 * rate, 20000.0, 1.0e-6), d4 = chip::designHalfBand (4.0 * rate, 20000.0, 1.0e-6);
    chip::HalfBandFir preAc, preDc, preDi;
    preAc.prepare (pre);
    preDc.prepare (pre);
    preDi.prepare (pre);
    chip::HalfBandUp upX1, upX2, upL1, upL2; // stage 1 at 2x, stage 2 at 4x (one object per stream: they run in order)
    chip::HalfBandDown dn2, dn1;
    upX1.prepare (d2);
    upL1.prepare (d2);
    upX2.prepare (d4);
    upL2.prepare (d4);
    dn2.prepare (d4);
    dn1.prepare (d2);
    // the chip's voices started as renderSignal starts them (startDelays), the samples before the last start rendered
    // and run through the chain too; ideal buses (statement (a)) come whole
    const auto delays = startDelays (rate, s, step);
    const int preRoll = buses != nullptr ? 0 : *std::max_element (delays.begin(), delays.begin() + s.voices);
    const int warm = std::max (16384, static_cast<int> (rate * 0.03)) + preRoll;
    const int total = warm + kAliasN;
    BusStreams made;
    if (buses == nullptr)
    {
        const auto z = static_cast<std::size_t> (total);
        made = { std::vector<double> (z), std::vector<double> (z), std::vector<double> (z), std::vector<double> (z), std::vector<double> (z), std::vector<double> (z) };
        const Chip::BusBlock all { made.fa.data(), made.da.data(), made.fe.data(), made.de.data(), made.fv.data(), made.dv.data() };
        const int at = startVoices (
            rate, s, step,
            [&] (int k) {
                OscInput in;
                in.frequency = s.reg[static_cast<std::size_t> (k)];
                in.pulseWidth = 2048.0f;
                in.control = static_cast<std::uint8_t> (s.wave[static_cast<std::size_t> (k)] | ctrl::gate);
                in.attackDecay = 0x00;
                in.sustainRelease = 0xf0;
                c.setOscillator (k, in);
            },
            [&] (int from, int n) { c.renderBuses (all.from (from), n); });
        REQUIRE (at == preRoll);
        c.renderBuses (all.from (at), total - at);
    }
    const BusStreams& bs = buses != nullptr ? *buses : made;
    REQUIRE (bs.fa.size() == static_cast<std::size_t> (total));
    const auto &fa = bs.fa, &da = bs.da, &fe = bs.fe, &de = bs.de, &fv = bs.fv, &dv = bs.dv;
    std::vector<double> y (static_cast<std::size_t> (kAliasN));
    double lowest = 1e300, highest = -1e300;
    const double g = o.gainFactor();
    auto shape = [&] (double lp) {
        const double v = lp < sh.lpMin ? sh.lpMin : (lp > sh.lpMax ? sh.lpMax : lp);
        return g * (((((sh.p5 * v + sh.p4) * v + sh.p3) * v + sh.p2) * v + sh.p1) * v);
    };
    for (int i = 0; i < total; ++i)
    {
        const auto j = static_cast<std::size_t> (i);
        double x = 0.0, lp = 0.0;
        if (m == Model::B)
        {
            const double lin = f.processLinearDouble (o.filterInput (fa[j], fe[j]), lp);
            x = g * (lin + o.directSumWith (da[j], de[j], dv[j]));
        }
        else
        {
            const double in = preAc.process (o.filterInputAc (fa[j], fe[j])) + preDc.process (OutputStage::voiceDc (Model::A) * fv[j]);
            const double di = preDi.process (o.directSumWith (da[j], de[j], dv[j]));
            x = g * (f.processLinearDouble (in, lp) + di);
        }
        // 4x: two interpolating stages, the curves, two decimating stages
        double xa = 0.0, xb = 0.0, la = 0.0, lb = 0.0;
        upX1.process (x, xa, xb);
        upL1.process (lp, la, lb);
        double x4[4], l4[4];
        upX2.process (xa, x4[0], x4[1]);
        upX2.process (xb, x4[2], x4[3]);
        upL2.process (la, l4[0], l4[1]);
        upL2.process (lb, l4[2], l4[3]);
        double m4[4];
        for (int k = 0; k < 4; ++k)
        {
            const double in4 = x4[k] + (m == Model::A ? shape (l4[k]) : 0.0);
            if (i >= warm)
            {
                lowest = std::min (lowest, in4);
                highest = std::max (highest, in4);
            }
            m4[k] = o.mixerCurve (in4);
        }
        const double ma = dn2.process (m4[0], m4[1]);
        const double mb = dn2.process (m4[2], m4[3]);
        const double out = dn1.process (ma, mb);
        if (i >= warm)
            y[static_cast<std::size_t> (i - warm)] = out;
    }
    if (range != nullptr)
        *range = { lowest, highest };
    return y;
}
} // namespace

namespace
{
/** Which statement of the restated gate a row belongs to (8.19.4, after the review and the fix rounds). */
enum class Statement
{
    a, // ideal band-limited voices into the chain's buses: the absolute limit
    b, // real chips, all direct and low-pass at resonance 0: the absolute limit
    c  // real chips, every other row: the absolute limit or within 1 dB of the 4x reference
};

/** The real chip's rows under (b): all direct, and the low-pass rows at resonance 0 (both cutoffs). */
bool chipAbsolute (const Signal&, const Setting& st) { return st.routing == 0 || (st.mode == 0x10 && st.resonance == 0); }

/** The pinned rows (docs/architecture.md 8.19.4 after the second review; owner decision O6): Model A rows of the
    overdriven class (its mixer input leaves the range the curve was fitted on) that miss (a) or (c) in some phase
    constellation, with their WORST figure over the constellations; each is gated at figure + 1 dB and two-sided: a
    listed row more than 2 dB better than its figure, or inside the limits in every constellation, fails as stale. In
    every constellation where a listed row is over its limit, its mixer input must leave the fitted range
    (OutputStage::mixerLimits, measured at 4x on that constellation's buses), so the list holds nothing else; every row
    not listed meets the full limits in every constellation. Model B has no pinned row. Figures: macOS arm64,
    2026-11-23; the full set's raw output: ~/Library/Caches/bside-oracle/s3b2/gate-final2.txt (outside the repo). */
struct Pin
{
    Statement which;
    double rate;
    const char* signal;
    int routing; // 1 voice 1 filtered, 2 all filtered
    std::uint8_t mode;
    int cutoff, resonance;
    double figure;
};
const std::vector<Pin> kPins {
    // all filtered, low-pass, resonance 15; the figure is the worst over the phase constellations
    { Statement::a, 88200.0, "saw + saw 3:2 at 2 kHz", 2, 0x10, 2047, 15, -98.2 },
    { Statement::a, 88200.0, "saw + square 5:4 at 2 kHz", 2, 0x10, 2047, 15, -94.3 },
    { Statement::a, 88200.0, "three voices at 3.85 kHz", 2, 0x10, 2047, 15, -79.0 },
    { Statement::a, 88200.0, "three voices at 2 kHz", 2, 0x10, 2047, 15, -88.9 },
    { Statement::a, 88200.0, "three voices at 2 kHz", 2, 0x10, 1024, 15, -98.7 },
    { Statement::c, 88200.0, "saw + saw 3:2 at 2 kHz", 2, 0x10, 2047, 15, -98.8 },
    { Statement::c, 88200.0, "saw + square 5:4 at 2 kHz", 2, 0x10, 2047, 15, -90.6 },
    { Statement::c, 88200.0, "three voices at 3.85 kHz", 2, 0x10, 2047, 15, -81.3 },
    { Statement::c, 88200.0, "three voices at 2 kHz", 2, 0x10, 2047, 15, -86.6 },
    { Statement::a, 96000.0, "saw + square 5:4 at 2 kHz", 2, 0x10, 2047, 15, -95.2 },
    { Statement::a, 96000.0, "three voices at 3.85 kHz", 2, 0x10, 2047, 15, -80.4 },
    { Statement::a, 96000.0, "three voices at 2 kHz", 2, 0x10, 2047, 15, -90.4 },
    { Statement::c, 96000.0, "saw + square 5:4 at 2 kHz", 2, 0x10, 2047, 15, -93.8 },
    { Statement::c, 96000.0, "three voices at 3.85 kHz", 2, 0x10, 2047, 15, -81.5 },
    { Statement::c, 96000.0, "three voices at 2 kHz", 2, 0x10, 2047, 15, -87.9 },
    { Statement::a, 128000.0, "three voices at 2 kHz", 2, 0x10, 2047, 15, -98.8 },
};

const Pin* pinFor (Model m, double rate, Statement which, const Signal& s, const Setting& st)
{
    if (m != Model::A)
        return nullptr;
    for (const auto& p : kPins)
        if (p.which == which && std::abs (p.rate - rate) < 0.5 && std::string (p.signal) == s.name && p.routing == st.routing && p.mode == st.mode
            && p.cutoff == st.cutoff && p.resonance == st.resonance)
            return &p;
    return nullptr;
}

struct GateTotals
{
    int rows[3] {}, failures[3] {}, measured = 0, pinsSeen = 0;
    double closest[3] { -1e9, -1e9, -1e9 };   // the largest margin to the limit among rows not pinned
    std::string closestWhat[3];
    std::vector<std::string> over;            // every failing row with the reason
    std::vector<std::string> pinned;          // every pinned row measured
    std::vector<std::string> outsideClass;    // rows over a limit in a constellation that does not overdrive the mixer
    std::map<double, double> classExcess;     // per rate: the largest excess over the full limit among pinned rows
    std::map<double, std::string> classExcessWhat;
};

/** The rows of one model and rate that `select` keeps, under the statement given, each over the phase constellations
    (kPhaseSteps; one for one-voice rows); printed if asked. */
template <class Select>
void runStatement (Model m, double rate, Statement which, Select&& select, GateTotals& t, bool print)
{
    const int w = static_cast<int> (which);
    const int warm = std::max (16384, static_cast<int> (rate * 0.03));
    OutputStage fittedStage;
    fittedStage.prepare (m, rate, false);
    const auto fitted = fittedStage.mixerLimits();
    for (const auto& s : kSignals)
    {
        const int constellations = s.voices == 1 ? 1 : static_cast<int> (sizeof (kPhaseSteps) / sizeof (kPhaseSteps[0]));
        std::vector<std::array<std::vector<double>, 3>> voices (static_cast<std::size_t> (constellations));
        for (const auto& st : settingsFor (s))
        {
            const bool absoluteRow = chipAbsolute (s, st);
            if ((which == Statement::b && ! absoluteRow) || (which == Statement::c && absoluteRow) || ! select (s, st))
                continue;
            const double f1 = s.reg[0] * kClock / 16777216.0;
            const double absolute = limitFor (m, s);
            const Pin* pin = pinFor (m, rate, which, s, st);
            double worstDb = -300.0, worstHz = 0.0, worstStep = 0.0, worstLimit = absolute, maxExcess = -1e9, excessStep = 0.0;
            std::string notOverdriven;
            std::array<double, 2> worstRange { 0.0, 0.0 };
            for (int ci = 0; ci < constellations; ++ci)
            {
                const double step = kPhaseSteps[static_cast<std::size_t> (ci)];
                auto& vs = voices[static_cast<std::size_t> (ci)];
                double limit = absolute;
                bool rangeKnown = false;
                std::array<double, 2> range { 0.0, 0.0 };
                Alias a;
                if (which == Statement::a)
                {
                    if (vs[0].empty())
                        for (int k = 0; k < s.voices; ++k)
                            vs[static_cast<std::size_t> (k)] = idealVoice (m, rate, s.reg[static_cast<std::size_t> (k)] * kClock / 16777216.0,
                                                                           s.wave[static_cast<std::size_t> (k)] == kSquare, step * k, warm + kAliasN);
                    a = aliasOfSpectrum (bhMagnitudes (renderIdeal (m, rate, s, st, vs)), rate, f1 / s.common, f1);
                }
                else
                {
                    a = aliasOfSpectrum (bhMagnitudes (renderSignal (m, rate, s, st, step)), rate, f1 / s.common, f1);
                    // (c): inside the absolute limit, or within 1 dB of the 4x reference
                    if (which == Statement::c && a.db > absolute)
                    {
                        const double ref = aliasOfSpectrum (bhMagnitudes (renderReference4x (m, rate, s, st, nullptr, &range, step)), rate, f1 / s.common, f1).db;
                        limit = std::max (absolute, ref + 1.0);
                        rangeKnown = true;
                    }
                }
                ++t.measured;
                const double excess = a.db - limit;
                if (excess > 0.0)
                {
                    // over the full limit in this constellation: allowed only in the overdriven class (and only if listed)
                    if (! rangeKnown)
                    {
                        if (which == Statement::a)
                        {
                            const BusStreams b = idealBuses (s, st, vs);
                            renderReference4x (m, rate, s, st, &b, &range);
                        }
                        else
                            renderReference4x (m, rate, s, st, nullptr, &range, step);
                    }
                    const bool overdriven = m == Model::A && (range[0] < fitted[0] || range[1] > fitted[1]);
                    if (! overdriven)
                    {
                        char nb[120];
                        std::snprintf (nb, sizeof (nb), "; NOT OVERDRIVEN at phase step %+.1f (mixer input %.2f .. %.2f)", step, range[0], range[1]);
                        notOverdriven += nb;
                    }
                    if (excess > maxExcess)
                        worstRange = range;
                }
                if (excess > maxExcess)
                {
                    maxExcess = excess;
                    excessStep = step;
                }
                if (a.db > worstDb)
                {
                    worstDb = a.db;
                    worstHz = a.hz;
                    worstStep = step;
                    worstLimit = limit;
                }
            }
            ++t.rows[w];
            bool fail = maxExcess > 0.0;
            char pinNote[260] = "";
            if (pin != nullptr && maxExcess <= 0.0)
            {
                // a listed row inside the limits everywhere (stale anyway): its class still checked, at its worst constellation
                auto& vs = voices[static_cast<std::size_t> (std::find (std::begin (kPhaseSteps), std::end (kPhaseSteps), worstStep) - std::begin (kPhaseSteps))];
                if (which == Statement::a)
                {
                    const BusStreams b = idealBuses (s, st, vs);
                    renderReference4x (m, rate, s, st, &b, &worstRange);
                }
                else
                    renderReference4x (m, rate, s, st, nullptr, &worstRange, worstStep);
                if (! (worstRange[0] < fitted[0] || worstRange[1] > fitted[1]))
                {
                    char nb[120];
                    std::snprintf (nb, sizeof (nb), "; NOT OVERDRIVEN at phase step %+.1f (mixer input %.2f .. %.2f)", worstStep, worstRange[0], worstRange[1]);
                    notOverdriven += nb;
                }
            }
            if (pin != nullptr)
            {
                ++t.pinsSeen;
                const bool stale = maxExcess <= 0.0 || worstDb < pin->figure - 2.0;
                std::snprintf (pinNote, sizeof (pinNote), "; PINNED (O6) at %.1f (+1 / -2 dB); excess over the full limit %+.1f dB at phase step %+.1f, mixer input %.2f .. %.2f, fitted %.2f .. %.2f%s",
                               pin->figure, maxExcess, excessStep, worstRange[0], worstRange[1], fitted[0], fitted[1],
                               maxExcess <= 0.0 ? "; STALE: inside the full limits" : (worstDb < pin->figure - 2.0 ? "; STALE: more than 2 dB better than its figure" : ""));
                fail = worstDb > pin->figure + 1.0 || stale || ! notOverdriven.empty();
                if (maxExcess > 0.0 && (t.classExcess.count (rate) == 0 || maxExcess > t.classExcess[rate]))
                {
                    t.classExcess[rate] = maxExcess;
                    t.classExcessWhat[rate] = std::string (s.name);
                }
            }
            char buf[640];
            std::snprintf (buf, sizeof (buf), "(%c) Model %s %6.0f Hz %-30s %-16s %-9s cutoff %4d res %2d: worst %7.1f dB at %7.1f Hz (phase step %+.1f; limit %.1f), largest excess %+.1f dB (phase step %+.1f)%s%s",
                           "abc"[w], m == Model::A ? "A" : "B", rate, s.name, st.routing == 0 ? "all direct" : (st.routing == 1 ? "voice 1 filtered" : "all filtered"),
                           st.routing == 0 ? "" : modeName (st.mode), st.cutoff, st.resonance, worstDb, worstHz, worstStep, worstLimit, maxExcess, excessStep, pinNote,
                           notOverdriven.c_str());
            if (print)
                std::printf ("[alias-gate] %s\n", buf);
            if (pin != nullptr)
                t.pinned.push_back (buf);
            else if (maxExcess > 0.0 && ! notOverdriven.empty())
                t.outsideClass.push_back (buf);
            if (fail)
            {
                ++t.failures[w];
                t.over.push_back (buf);
            }
            if (pin == nullptr && maxExcess > t.closest[w])
            {
                t.closest[w] = maxExcess;
                t.closestWhat[w] = buf;
            }
        }
    }
}

void report (const GateTotals& t)
{
    for (int w = 0; w < 3; ++w)
        std::printf ("[alias-gate] (%c) %d rows, %d failing; closest unpinned row to its limit: %s\n", "abc"[w], t.rows[w], t.failures[w], t.closestWhat[w].c_str());
    std::printf ("[alias-gate] %d measurements (rows x phase constellations); pinned rows measured: %d of %d listed\n", t.measured, t.pinsSeen, static_cast<int> (kPins.size()));
    for (const auto& [rate, e] : t.classExcess)
        std::printf ("[alias-gate] the overdriven class at %6.0f Hz: largest excess over the full limit %+.1f dB (%s)\n", rate, e, t.classExcessWhat.at (rate).c_str());
    for (const auto& r : t.pinned)
        std::printf ("[alias-gate] PINNED: %s\n", r.c_str());
    for (const auto& r : t.outsideClass)
        std::printf ("[alias-gate] OUTSIDE THE CLASS: %s\n", r.c_str());
    for (const auto& r : t.over)
        std::printf ("[alias-gate] NOT MET: %s\n", r.c_str());
}
} // namespace

TEST_CASE ("alias gate: the static curves through the chain, the full set of 8.19.4 at every internal rate (hidden; several minutes)", "[.chain-alias-full]")
{
    // 8.19.4 (after the review and the fix rounds): the signal set (one voice: saw and square at 3.85 kHz, saw at 2 and
    // 1 kHz; two voices: saw + saw 3:2, saw + square 5:4 at 3.85 and 2 kHz; three voices at 3.85 and 2 kHz; full envelope;
    // all direct, voice 1 through the filter, all through it: low-, band-, high-pass, cutoff 2047 and 1024, resonance 0
    // and 15), every multi-voice row in each phase constellation of kPhaseSteps (the worst taken), and the measurement
    // (2^18 samples, 7-term Blackman-Harris, 20 Hz - 20 kHz, the common fundamental's lines excluded +-10 bins, relative
    // to voice 1's fundamental) under three statements, all asserted: (a) ideal band-limited voices in the chain's
    // buses, every row, Model B at most -103 dB, Model A at most -99 dB to 2 kHz and -83 dB to 3.85 kHz; (b) real chips,
    // all direct and low-pass at resonance 0, the same limits; (c) real chips, every other row, the same limits or within
    // 1 dB of the chain with its curves at four times the rate (renderReference4x). The pinned rows (kPins) at their
    // figure + 1 / - 2 dB. Every row is printed.
    const auto t0 = std::chrono::steady_clock::now();
    GateTotals t;
    for (const Model m : { Model::B, Model::A })
        for (const auto& rc : rateConfigs())
            for (const Statement w : { Statement::a, Statement::b, Statement::c })
                runStatement (m, rc.internalRate, w, [] (const Signal&, const Setting&) { return true; }, t, true);
    report (t);
    std::printf ("[alias-gate] full set: %.0f s\n", std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
    CHECK (t.rows[0] == 2 * 7 * 202);
    CHECK (t.rows[1] + t.rows[2] == 2 * 7 * 202);
    CHECK (t.pinsSeen == static_cast<int> (kPins.size()));
    CHECK (t.outsideClass.empty());
    for (int w = 0; w < 3; ++w)
    {
        INFO (t.closestWhat[w]);
        CHECK (t.failures[w] == 0);
    }
}

TEST_CASE ("alias gate: the static curves through the chain, a reduced set in the normal run (88.2 and 96 kHz)", "[chain][alias][gate]")
{
    // The full set (hidden, "[.chain-alias-full]") at the two lowest internal rates (the narrowest transition bands, the
    // worst rows of the full set) under the same rules, the phase constellations and the pinned rows of these rates
    // included: (a) every row; (b) every row; (c) the rows at resonance 15 and the three-voice rows.
    const auto t0 = std::chrono::steady_clock::now();
    GateTotals t;
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 88200.0, 96000.0 })
        {
            runStatement (m, rate, Statement::a, [] (const Signal&, const Setting&) { return true; }, t, false);
            runStatement (m, rate, Statement::b, [] (const Signal&, const Setting&) { return true; }, t, false);
            runStatement (m, rate, Statement::c, [] (const Signal& s, const Setting& st) { return st.resonance == 15 || s.voices == 3; }, t, false);
        }
    report (t);
    std::printf ("[alias-gate] reduced set: %.0f s\n", std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
    int listedHere = 0;
    for (const auto& p : kPins)
        if (p.rate < 100000.0 && (p.which != Statement::c || p.resonance == 15))
            ++listedHere;
    CHECK (t.pinsSeen == listedHere);
    CHECK (t.outsideClass.empty());
    for (int w = 0; w < 3; ++w)
    {
        INFO (t.closestWhat[w]);
        CHECK (t.failures[w] == 0);
        CHECK (t.rows[w] > 50);
    }
}

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
            const HalfBandDesign d = chip::designHalfBand (r, 20000.0, r > rate ? ChipVoice::kTwiceDeviation : ChipVoice::kAntiAliasDeviation);
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
        const HalfBandDesign pre = chip::designHalfBand (rate, 20000.0, ChipVoice::kAntiAliasDeviation);
        const HalfBandDesign twice = chip::designHalfBand (2.0 * rate, 20000.0, ChipVoice::kTwiceDeviation);
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
        const HalfBandDesign pre = chip::designHalfBand (rc.internalRate, 20000.0, ChipVoice::kAntiAliasDeviation);
        const HalfBandDesign twice = chip::designHalfBand (2.0 * rc.internalRate, 20000.0, ChipVoice::kAntiAliasDeviation);
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
            const HalfBandDesign pre = chip::designHalfBand (rate, 20000.0, ChipVoice::kAntiAliasDeviation);
            const HalfBandDesign up = chip::designHalfBand (2.0 * rate, 20000.0, ChipVoice::kTwiceDeviation);
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
                std::array<Filter::Shape, ChipVoice::kBlock> sh {};
                for (int i = 0; i < got; ++i)
                {
                    const double dc = v[0]->staticFilterBusDc (i);
                    double in = dc;
                    for (auto& x : v)
                        in += x->filterInputAc (i);
                    so[static_cast<std::size_t> (i)] = sf.processLinearDouble (in, sl[static_cast<std::size_t> (i)]);
                    ss[static_cast<std::size_t> (i)] = sf.staticOutput (dc);
                    sh[static_cast<std::size_t> (i)] = sf.outputShapeCoefficients();
                }
                for (auto& x : v)
                    x->finishShared (sh.data(), so.data(), sl.data(), ss.data(), 2, out.data(), got);
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

// ---- after the review of S3b-2: write timing per type, Shared mode writes, the chain with a moving cutoff ----

namespace
{
/** One write case of the timing test: a held voice (oscillator 1 frozen by the test bit: a constant), the registers
    before, the write. */
struct TimingCase
{
    const char* name;
    std::uint8_t route0, mode0, reg, value;
    int cutoff;
    bool volumeDc;
};

const TimingCase kTimingCases[] { { "volume", 0x00, 0x1f, 0x18, 0x18, 2047, true },
                                  { "volume, volume DC off", 0x00, 0x1f, 0x18, 0x18, 2047, false },
                                  { "routing (oscillators 1 and 2 swap buses, no mode selected)", 0x02, 0x0f, 0x17, 0x01, 2047, true },
                                  { "routing with the counts (oscillator 1 to the filter, no mode selected)", 0x00, 0x0f, 0x17, 0x01, 2047, true },
                                  { "mode (low-pass to high-pass)", 0x01, 0x1f, 0x18, 0x4f, 1000, true },
                                  { "resonance (Model A: the output function's gain)", 0x01, 0x1f, 0x17, 0xf1, 2047, true } };

void timingSetup (ChipVoice& v, const TimingCase& c)
{
    OscInput in;
    in.frequency = 0.0;
    in.pulseWidth = 2048.0f;
    in.control = ctrl::saw | ctrl::test | ctrl::gate;
    in.attackDecay = 0x00;
    in.sustainRelease = 0xf0;
    v.setOscillator (0, in);
    v.write (0x15, static_cast<std::uint8_t> (c.cutoff & 7));
    v.write (0x16, static_cast<std::uint8_t> (c.cutoff >> 3));
    v.write (0x17, c.route0);
    v.write (0x18, c.mode0);
}

/** The centre of a step in y: the centroid of its first difference within +-200 samples of `around`, minus 1/2. */
double stepCentre (const std::vector<double>& y, double around)
{
    double s = 0.0, sw = 0.0;
    for (int n = static_cast<int> (around) - 200; n <= static_cast<int> (around) + 200; ++n)
    {
        const double d = y[static_cast<std::size_t> (n)] - y[static_cast<std::size_t> (n - 1)];
        s += d * n;
        sw += d;
    }
    return s / sw - 0.5;
}

/** The mixer curve's input recovered from the output (board filters off, volume DC on: output = n (Y0 + level N (x))),
    by Newton on OutputStage::mixerCurve (monotonic): a step's centre measured there is not bent by the curve (a step
    through a curve whose slope changes along it has its centre of gravity moved; Model A's gain moves with a routing
    write by 13 %). The output is the curve's output decimated, so this is exact for a held input and close to it within
    the step. */
std::vector<double> curveInput (Model m, double rate, const TimingCase& c, int chips, std::vector<double> y)
{
    OutputStage o;
    o.prepare (m, rate, true);
    o.setModeVolume (c.mode0);
    const double a = o.postScale(), b = o.postOffset();
    for (double& v : y)
    {
        const double target = (v / chips - b) / a;
        double x = target;
        for (int k = 0; k < 60; ++k)
        {
            const double f = o.mixerCurve (x) - target;
            const double d = (o.mixerCurve (x + 1e-6) - o.mixerCurve (x - 1e-6)) / 2e-6;
            if (! (d > 1e-9))
                break;
            const double dx = f / d;
            x -= dx;
            if (std::abs (dx) < 1e-15)
                break;
        }
        v = x;
    }
    return y;
}

/** The owner's switch at bus sample switchAt (ChipVoice::sharedSwitchSample, for a write at the chip's current cycle)
    against Per Note's blend of the two configurations: the long kernel's unit step at the write's place as weights on the
    filter's signal samples q (ChipVoice::queue), the centre of their first difference; the switch acts at the filter's
    signal sample switchAt - filterLagSamples (). In samples, positive: the switch later. */
double switchRuleOffset (ChipVoice& v, std::int64_t switchAt)
{
    const Chip::EventTime t = v.chip().renderEventTime();
    std::array<double, chip::render::kKernelTaps> taps {};
    int first = 0;
    v.chip().unitStepTaps (t.base, taps, first);
    const auto s0 = static_cast<std::int64_t> (t.sample);
    auto weight = [&] (std::int64_t q) {
        const std::int64_t k = q - (s0 + first);
        const double inside = k >= 0 && k < chip::render::kKernelTaps ? taps[static_cast<std::size_t> (k)] : 0.0;
        return (q >= s0 ? 1.0 : 0.0) + inside;
    };
    double sq = 0.0, sd = 0.0;
    for (std::int64_t q = s0 + first - 2; q <= s0 + first + chip::render::kKernelTaps + 2; ++q)
    {
        const double d = weight (q) - weight (q - 1);
        sq += d * static_cast<double> (q);
        sd += d;
    }
    return static_cast<double> (switchAt - v.filterLagSamples()) - sq / sd;
}

/** n chips, Per Note (each its own filter; only n = 1 here) or Shared (one filter run by this owner as ChipVoice.h
    "TOPOLOGY" says), the case's write at `w` to every chip (the owner switches its filter's resonance and mode at the bus
    sample ChipVoice::sharedSwitchSample () names; with switchOffset, that switch against Per Note's blend); board filters
    off; the summed output. */
std::vector<double> timingRun (Model m, double rate, const TimingCase& c, int chips, bool shared, std::uint64_t w, int total, int& latency,
                               double* switchOffset = nullptr)
{
    std::vector<std::unique_ptr<ChipVoice>> v;
    Filter sf;
    sf.prepare (m, rate, 0.5f);
    for (int k = 0; k < chips; ++k)
    {
        chip::Config cfg;
        cfg.model = m;
        cfg.internalRate = rate;
        cfg.volumeDc = c.volumeDc;
        v.push_back (std::make_unique<ChipVoice>());
        v.back()->prepare (cfg);
        v.back()->boardFiltersEnabled (false);
        if (! shared)
            v.back()->setFilter (&sf);
        timingSetup (*v.back(), c);
    }
    latency = v[0]->latencySamples();
    if (shared)
    {
        sf.setCutoff (static_cast<float> (c.cutoff));
        sf.setResonance (c.route0 >> 4);
        sf.setMode (c.mode0);
    }
    std::vector<double> y;
    bool written = false;
    std::int64_t switchAt = -1;
    while (static_cast<int> (y.size()) < total)
    {
        const int want = std::min (ChipVoice::kBlock, total - static_cast<int> (y.size()));
        std::array<double, ChipVoice::kBlock> out {}, so {}, sl {}, ss {};
        std::array<Filter::Shape, ChipVoice::kBlock> sh {};
        if (! shared)
        {
            const int got = v[0]->renderUntil (out.data(), want, written ? ~std::uint64_t { 0 } : w);
            y.insert (y.end(), out.begin(), out.begin() + got);
        }
        else
        {
            int got = 0;
            for (auto& x : v)
                got = x->renderBusesUntil (want, written ? ~std::uint64_t { 0 } : w);
            for (int i = 0; i < got; ++i)
            {
                // the owner's filter takes the write at the bus sample ChipVoice::sharedSwitchSample () names
                const auto b = static_cast<std::int64_t> (y.size()) + i;
                if (switchAt >= 0 && b == switchAt)
                {
                    if (c.reg == 0x18)
                        sf.setMode (c.value);
                    if (c.reg == 0x17)
                        sf.setResonance (c.value >> 4);
                }
                const double dc = v[0]->staticFilterBusDc (i);
                double in = dc;
                for (auto& x : v)
                    in += x->filterInputAc (i);
                so[static_cast<std::size_t> (i)] = sf.processLinearDouble (in, sl[static_cast<std::size_t> (i)]);
                ss[static_cast<std::size_t> (i)] = sf.staticOutput (dc);
                sh[static_cast<std::size_t> (i)] = sf.outputShapeCoefficients();
            }
            for (auto& x : v)
                x->finishShared (sh.data(), so.data(), sl.data(), ss.data(), chips, out.data(), got);
            y.insert (y.end(), out.begin(), out.begin() + got);
        }
        if (! written && v[0]->chip().cycle() == w)
        {
            switchAt = v[0]->sharedSwitchSample();
            if (switchOffset != nullptr)
                *switchOffset = switchRuleOffset (*v[0], switchAt);
            for (auto& x : v)
                x->write (c.reg, c.value);
            written = true;
        }
    }
    return y;
}
} // namespace

TEST_CASE ("alias: every write lands where latencySamples () says: volume, routing, mode, resonance, both models, Per Note and Shared (0.02 samples)", "[chain][alias][latency][writes]")
{
    // 8.19.4 (after the review): a write's step, the centre of gravity of its first difference, lies within 0.02 internal
    // samples of (cycle - 1) / cycles per sample + latencySamples () (the analog registers' writes lead an oscillator
    // write by one cycle, Chip::kAnalogWriteLeadCycles). A held voice (a constant through the chain: the step is the
    // write's alone, no filter transient: the filter's state does not move for a held input), every internal rate.
    // Volume (volume DC on and off), routing (oscillator 1, the held voice, to the filter with no mode selected, and the
    // silent oscillator 2 to the direct bus in the same write: the voice leaves the output while the voice counts, and
    // with them the mixer's gain, stay: a gain changing with the step would add a product of two steps whose centre is
    // not the write's), mode (low- to high-pass) and, on Model A, resonance (it scales the output
    // function; Model B's linear filter keeps a held input's level at every resonance, so it has no step: its timing is
    // the filter's, pinned in ChipVoiceTests.cpp "the cutoff runs per sample ..."). Per Note, and Shared with two
    // identical chips for the volume and routing writes (the stage's); Shared mode writes: the next test. The review
    // found Model A's mode steps 0.31 samples early (the output function's coefficients half a 2x sample early) and
    // routing steps on Model A up to 31 samples off with the voice counts not delayed; both are caught here.
    int checked = 0;
    double worst = 0.0;
    for (const Model m : { Model::B, Model::A })
        for (const auto& rc : rateConfigs())
            for (const auto& c : kTimingCases)
                for (const int chips : { 1, 2 })
                {
                    const bool resonance = c.reg == 0x17 && c.value == 0xf1;
                    if ((resonance && m == Model::B) || (chips == 2 && (c.reg == 0x18 && c.value == 0x4f)) || (chips == 2 && resonance))
                        continue;
                    const double rate = rc.internalRate;
                    const double cps = 985248.0 / rate;
                    const auto w = static_cast<std::uint64_t> (0.05 * 985248.0) + 3; // not on a sample boundary
                    int latency = 0;
                    auto y = timingRun (m, rate, c, chips, chips > 1, w, static_cast<int> (rate * 0.08), latency);
                    if (c.reg != 0x18 || c.value != 0x18)
                        y = curveInput (m, rate, c, chips, y);
                    const double expect = static_cast<double> (w - 1) / cps + latency;
                    const double off = stepCentre (y, expect) - expect;
                    INFO ((m == Model::A ? "Model A " : "Model B ") << rate << " Hz, " << c.name << (chips == 1 ? ", Per Note" : ", Shared")
                                                                   << ": the step's centre lies " << off << " samples from latencySamples ()'s place");
                    // the routing write that changes the voice counts moves Model A's mixer gain by 13 %: the gain's step (the
                    // counts are delayed with the signal, not band-limited by the pre-filter) times the smoothed signal's step
                    // has its centre 0.06 to 0.07 samples late with the timing exact (8.19.4 after the second review; the
                    // review measured 0.0000 at the buses), so on Model A that case is held to 0.1 sample: it is there to show
                    // that the counts move with the delayed signal (the first review's mutation moved it by up to 31 samples).
                    // Model B's gain moves 0.4 %: held to 0.02 like every other case.
                    const bool looser = c.reg == 0x17 && c.route0 == 0x00 && m == Model::A;
                    CHECK (std::abs (off) < (looser ? 0.1 : 0.02));
                    if (! looser)
                        worst = std::max (worst, std::abs (off));
                    ++checked;
                }
    std::printf ("[timing] %d write steps, the largest offset from latencySamples ()'s place %.4f samples\n", checked, worst);
    CHECK (checked > 100);
}

namespace
{
} // namespace

TEST_CASE ("alias: Shared mode and resonance writes against Per Note: the owner's switch at the nearest sample (0.5 samples), the same outside the window", "[chain][alias][shared][writes]")
{
    // ChipVoice.h "TOPOLOGY" (8.19.4 after the second review): the owner of a shared filter switches its mode or
    // resonance at ChipVoice::sharedSwitchSample (), the bus sample nearest the write's place; Per Note blends the two
    // configurations over the write's window (the long kernel). One chip in Shared against Per Note, every internal rate,
    // six write positions across a sample, both models, mode (low- to high-pass) and resonance (0 -> 15), the held voice
    // of the timing test:
    // - the rule: the owner's switch against the centre of Per Note's blend weights (Chip::unitStepTaps at the write's
    //   place), within 0.5 samples: every case, Model B's resonance included (a held voice shows no resonance step on
    //   Model B's linear filter, and on a moving voice a hard switch and a blend leave different filter states, so no
    //   output measure times it);
    // - the output: the step's centre (on the mixer curve's input) against Per Note's, where there is a step: Model B's
    //   mode and Model A's resonance within 0.5 samples; Model A's mode within 0.6: its output step comes out about 1.2
    //   times the switch's offset (measured over the positions; the shared filter's output function switches with it
    //   while the voice's 2x part interpolates the coefficients over the step);
    // - outside the write's window widened by the stages after the filter: the same to 1e-12.
    double worst = 0.0, worstRule = 0.0;
    int checked = 0;
    for (const Model m : { Model::B, Model::A })
        for (const auto& rc : rateConfigs())
            for (const bool resonance : { false, true })
            {
                const double rate = rc.internalRate;
                const double cps = 985248.0 / rate;
                const TimingCase* found = nullptr;
                for (const auto& k : kTimingCases)
                    if (resonance ? (k.reg == 0x17 && k.value == 0xf1) : (k.reg == 0x18 && k.value == 0x4f))
                        found = &k;
                REQUIRE (found != nullptr);
                const TimingCase& c = *found;
                double worstHere = 0.0, ruleHere = 0.0;
                if (! resonance)
                {
                    // the rule alone at every cycle across three samples (no write: the switch for a write made there)
                    Voice p (m, rate, true);
                    timingSetup (*p.v, c);
                    std::vector<double> sink (8192, 0.0);
                    const auto w0 = static_cast<std::uint64_t> (0.05 * 985248.0);
                    while (p.v->chip().cycle() < w0)
                        p.v->renderUntil (sink.data(), 8192, w0);
                    for (std::uint64_t w = w0; w < w0 + static_cast<std::uint64_t> (3.0 * cps); ++w)
                    {
                        std::fill (sink.begin(), sink.end(), 0.0);
                        p.v->renderUntil (sink.data(), 8192, w);
                        REQUIRE (p.v->chip().cycle() == w);
                        const double r = switchRuleOffset (*p.v, p.v->sharedSwitchSample());
                        INFO ((m == Model::A ? "Model A " : "Model B ") << rate << " Hz, cycle " << w << ": the owner's switch lies " << r << " samples from Per Note's blend");
                        CHECK (std::abs (r) <= 0.5);
                        ruleHere = std::max (ruleHere, std::abs (r));
                    }
                }
                for (int j = 0; j < 6; ++j)
                {
                    // six positions across one sample (cycles from a sample boundary)
                    const auto w = static_cast<std::uint64_t> (0.05 * 985248.0) + static_cast<std::uint64_t> (std::floor (cps * (j + 0.5) / 6.0));
                    int latency = 0;
                    const int total = static_cast<int> (rate * 0.08);
                    double rule = 0.0;
                    const auto pn = timingRun (m, rate, c, 1, false, w, total, latency);
                    const auto sh = timingRun (m, rate, c, 1, true, w, total, latency, &rule);
                    const double expect = static_cast<double> (w - 1) / cps + latency;
                    INFO ((m == Model::A ? "Model A " : "Model B ") << rate << " Hz, " << (resonance ? "resonance" : "mode") << ", position " << j
                                                                   << ": the owner's switch lies " << rule << " samples from Per Note's blend");
                    CHECK (std::abs (rule) <= 0.5);
                    ruleHere = std::max (ruleHere, std::abs (rule));
                    ++checked;
                    if (resonance && m == Model::B)
                        continue; // no step to measure (above)
                    const double off = stepCentre (curveInput (m, rate, c, 1, sh), expect) - stepCentre (curveInput (m, rate, c, 1, pn), expect);
                    if (! resonance)
                    {
                        // outside the window: identical
                        const int post = latency - Voice (m, rate, true).v->filterLagSamples() + 1;
                        double outside = 0.0, size = 0.0;
                        for (std::size_t i = 100; i < pn.size(); ++i)
                        {
                            const double di = static_cast<double> (i);
                            size = std::max (size, std::abs (pn[i] - pn[100]));
                            if (di < expect - 10.0 - post || di > expect + 10.0 + post)
                                outside = std::max (outside, std::abs (sh[i] - pn[i]));
                        }
                        CHECK (size > 1.0e-3);
                        CHECK (outside <= 1.0e-12 * std::max (1.0, size));
                    }
                    INFO ("Shared's output step lies " << off << " samples from Per Note's");
                    CHECK (std::abs (off) <= (m == Model::A && ! resonance ? 0.6 : 0.5));
                    worstHere = std::max (worstHere, std::abs (off));
                }
                std::printf ("[shared-timing] Model %s %6.0f Hz %-9s switch against Per Note's blend: worst %.3f; output step: %s%.3f samples\n", m == Model::A ? "A" : "B",
                             rate, resonance ? "resonance" : "mode", ruleHere, resonance && m == Model::B ? "(none) " : "worst ", worstHere);
                worst = std::max (worst, worstHere);
                worstRule = std::max (worstRule, ruleHere);
            }
    std::printf ("[shared-timing] %d writes: the switch within %.3f samples of Per Note's blend, the output step within %.3f of Per Note's\n", checked, worstRule, worst);
    CHECK (checked == 2 * 7 * 2 * 6);
}

TEST_CASE ("alias: the chain's output with a moving cutoff equals a twin built from the public pieces", "[chain][alias][cutoff]")
{
    // The S3b-1 test of the cutoff's rule compares the filter's output (ChipVoiceTests.cpp); this one compares the whole
    // chain's output after the filter, through the anti-alias stages, with a twin made of the public pieces: a Chip (its
    // voices bent, as ChipVoice bends them) rendering the buses, a Filter driven by the rule written out (a cutoff write
    // takes effect at the filter's signal time of its sample, filterLagSamples () after it in the output; processWith-
    // Cutoff while the cutoff moved within 256 samples), an OutputStage, and the HalfBand filters of the designs (Model
    // A: the bus pre-filter, the interpolators, the decimator, the output function's coefficients; Model B: the mixer
    // FIR), each delay explicit. A saw through the low-pass at resonance 8, one isolated cutoff change, a sweep (a change
    // every 32 samples), a last change; volume DC on, board filters off. To 1e-9 (compilers may contract differently),
    // both models, 96 and 88.2 kHz.
    for (const Model m : { Model::B, Model::A })
        for (const double rate : { 96000.0, 88200.0 })
        {
            Voice p (m, rate, true);
            ChipVoice& v = *p.v;
            v.boardFiltersEnabled (false);
            Chip twin = makeChip (m, rate);
            twin.renderBendVoices (true);
            OscInput in;
            in.frequency = 220.0 * 16777216.0 / 985248.0;
            in.control = ctrl::saw | ctrl::gate;
            in.sustainRelease = 0xf0;
            v.setOscillator (0, in);
            twin.setOscillator (0, in);
            auto both = [&] (std::uint8_t reg, std::uint8_t value) {
                v.write (reg, value);
                twin.write (reg, value);
            };
            both (0x15, 0);
            both (0x16, 0x40);
            both (0x17, 0x81);
            both (0x18, 0x1f);
            const double cps = static_cast<double> (twin.sampleClock().increment()) / 4294967296.0;
            struct W
            {
                std::uint64_t cycle;
                std::uint8_t value;
                std::int64_t sample = 0;
            };
            std::vector<W> w;
            auto at = [cps] (double sample) { return static_cast<std::uint64_t> (sample * cps + 0.37 * cps); };
            w.push_back ({ at (2000.0), 0x60 });
            for (int k = 0; k < 60; ++k)
                w.push_back ({ at (4000.0 + 32.0 * k), static_cast<std::uint8_t> (0x30 + (k * 23) % 160) });
            w.push_back ({ at (11000.0), 0x90 });
            const int total = 14000;
            std::vector<double> y (static_cast<std::size_t> (total), 0.0);
            std::vector<double> fa (y.size()), da (y.size()), fe (y.size()), de (y.size()), fv (y.size()), dv (y.size());
            auto busAt = [&] (int k) { return Chip::BusBlock { fa.data() + k, da.data() + k, fe.data() + k, de.data() + k, fv.data() + k, dv.data() + k }; };
            int done = 0, doneTwin = 0;
            for (auto& x : w)
            {
                done += v.renderUntil (y.data() + done, total - done, x.cycle);
                doneTwin += twin.renderBusesUntil (busAt (doneTwin), total - doneTwin, x.cycle);
                REQUIRE (v.chip().cycle() == x.cycle);
                x.sample = static_cast<std::int64_t> (v.chip().renderSampleIndex());
                both (0x16, x.value);
            }
            v.renderUntil (y.data() + done, total - done, ~std::uint64_t { 0 });
            twin.renderBusesUntil (busAt (doneTwin), total - doneTwin, ~std::uint64_t { 0 });
            // the twin
            Filter rf;
            rf.prepare (m, rate, 0.5f);
            rf.setCutoff (512.0f);
            rf.setResonance (8);
            rf.setMode (0x1f);
            OutputStage st;
            st.prepare (m, rate, true);
            st.setRouting (0x01);
            st.setModeVolume (0x1f);
            const auto pre = chip::designHalfBand (rate, ChipVoice::kAntiAliasPassHz, ChipVoice::kAntiAliasDeviation);
            const auto twice = chip::designHalfBand (2.0 * rate, ChipVoice::kAntiAliasPassHz, ChipVoice::kTwiceDeviation);
            chip::HalfBandFir mix, preAc, preDc, preDi;
            for (auto* f : { &mix, &preAc, &preDc, &preDi })
                f->prepare (pre);
            chip::HalfBandUp upX, upL;
            chip::HalfBandDown dn;
            upX.prepare (twice);
            upL.prepare (twice);
            dn.prepare (twice);
            const int lag = v.filterLagSamples(), shapeDelay = upX.pairCount() - 1;
            REQUIRE (lag == Chip::kLatencySamples + 1 + (m == Model::A ? pre.delay() : 0));
            float cutoff = 512.0f;
            bool changed = false, moving = false;
            std::int64_t last = 0;
            std::size_t next = 0;
            std::vector<std::array<double, 5>> coef;
            double worst = 0.0;
            int movingSamples = 0;
            for (std::size_t b = 0; b < y.size(); ++b)
            {
                const std::int64_t t = static_cast<std::int64_t> (b) - lag;
                while (next < w.size() && w[next].sample <= t)
                {
                    const auto c = static_cast<float> (w[next++].value << 3);
                    if (! (c < cutoff || c > cutoff))
                        continue;
                    cutoff = c;
                    if (changed && t - last < 256)
                        moving = true;
                    last = t;
                    changed = true;
                    if (! moving)
                        rf.setCutoff (cutoff);
                }
                if (moving && t - last >= 256)
                {
                    moving = false;
                    rf.setCutoff (cutoff);
                }
                movingSamples += moving ? 1 : 0;
                st.setVoices (fv[b], dv[b]); // the counts do not move here
                const double g = st.gainFactor();
                double out = 0.0, lp = 0.0;
                if (m == Model::B)
                {
                    const double fin = st.filterInput (fa[b], fe[b]);
                    const double lin = moving ? rf.processWithCutoffLinearDouble (fin, cutoff, lp) : rf.processLinearDouble (fin, lp);
                    const double x = g * (lin + st.directSumWith (da[b], de[b], dv[b]));
                    if (b == 0)
                        mix.prime (x);
                    out = st.postScale() * st.mixerCurve (mix.process (x)) + st.postOffset();
                }
                else
                {
                    const double ac = st.filterInputAc (fa[b], fe[b]), dc = OutputStage::voiceDc (m) * fv[b], di = st.directSumWith (da[b], de[b], dv[b]);
                    if (b == 0)
                    {
                        preAc.prime (ac);
                        preDc.prime (dc);
                        preDi.prime (di);
                    }
                    const double pa = preAc.process (ac), pd = preDc.process (dc), pdi = preDi.process (di);
                    st.setFilterStaticDc (rf.staticOutput (pd));
                    const double lin = moving ? rf.processWithCutoffLinearDouble (pa + pd, cutoff, lp) : rf.processLinearDouble (pa + pd, lp);
                    const double x = g * (lin + pdi);
                    const auto sh = rf.outputShapeCoefficients();
                    coef.push_back ({ g * sh.p1, g * sh.p2, g * sh.p3, g * sh.p4, g * sh.p5 });
                    auto shapeAt = [&] (std::size_t back, double l) {
                        const auto& c = coef[coef.size() > back ? coef.size() - 1 - back : 0];
                        const double vv = l < sh.lpMin ? sh.lpMin : (l > sh.lpMax ? sh.lpMax : l);
                        return ((((c[4] * vv + c[3]) * vv + c[2]) * vv + c[1]) * vv + c[0]) * vv;
                    };
                    if (b == 0)
                    {
                        upX.prime (x);
                        upL.prime (lp);
                        dn.prime (st.mixerCurve (x + shapeAt (0, lp)));
                    }
                    double l0 = 0.0, l1 = 0.0, p0 = 0.0, p1 = 0.0;
                    upX.process (x, l0, l1);
                    upL.process (lp, p0, p1);
                    // the interpolated low-pass value p0 lies half a sample before p1, which is shapeDelay samples late
                    const auto d = static_cast<std::size_t> (shapeDelay);
                    const double s0 = 0.5 * (shapeAt (d, p0) + shapeAt (d + 1, p0)), s1 = shapeAt (d, p1);
                    const double mm = dn.process (st.mixerCurve (l0 + s0), st.mixerCurve (l1 + s1));
                    out = st.postScale() * mm + st.postOffset();
                }
                worst = std::max (worst, std::abs (out - y[b]));
            }
            INFO ((m == Model::A ? "Model A " : "Model B ") << rate << " Hz: " << movingSamples << " samples moving; largest difference from the twin " << worst);
            CHECK (movingSamples > 2000);
            CHECK (worst <= 1.0e-9);
        }
}

namespace
{
/** A Model A chip with its voices bent: oscillator 1 a saw synced by oscillator 3 (sync) or a triangle ring-modulated
    by it (ring), oscillator 3 a bent saw sounding too; or (glide) oscillator 1 a saw whose frequency moves every 32
    samples up and down a ramp that crosses converter switches, the pattern repeating. Returns the single sum (bent),
    n samples after a settling time, and the line spacing of the periodic output. */
std::vector<double> bentPatch (double rate, int which, int n, double& lineHz)
{
    Chip c = makeChip (Model::A, rate);
    c.renderBendVoices (true);
    const double src = 17026.0; // about 1 kHz, an exact register value
    OscInput in;
    in.pulseWidth = 2048.0f;
    in.attackDecay = 0x00;
    in.sustainRelease = 0xf0;
    in.frequency = src;
    in.control = ctrl::saw | ctrl::gate;
    if (which != 2)
        c.setOscillator (2, in); // the source, a bent saw
    in.frequency = which == 2 ? src : (which == 0 ? src * 3.137 : src * 2.5); // ring: an exact 5 : 2 (its lines at half the source)
    in.control = static_cast<std::uint8_t> (which == 0 ? (ctrl::saw | ctrl::sync | ctrl::gate) : (which == 1 ? (ctrl::triangle | ctrl::ring | ctrl::gate) : (ctrl::saw | ctrl::gate)));
    c.setOscillator (0, in);
    lineHz = src * 985248.0 / 16777216.0 / (which == 1 ? 2.0 : 1.0); // the source's frequency (ring: half of it)
    const int warm = 8192;
    std::vector<double> y;
    y.reserve (static_cast<std::size_t> (warm + n));
    std::vector<float> buf (32);
    int k = 0;
    while (static_cast<int> (y.size()) < warm + n)
    {
        if (which == 2)
        {
            // the glide: 64 steps up then 64 down, a step every 32 samples (1 to 3.85 kHz: the converter terms switch)
            const int phase = k % 128;
            const double pos = phase < 64 ? phase / 63.0 : (127 - phase) / 63.0;
            in.frequency = src * (1.0 + 2.85 * pos);
            c.setOscillator (0, in);
            ++k;
        }
        std::fill (buf.begin(), buf.end(), 0.0f);
        c.render (buf.data(), 32);
        for (const float v : buf)
            y.push_back (static_cast<double> (v));
    }
    y.erase (y.begin(), y.begin() + warm);
    if (which == 2)
        lineHz = rate / (128.0 * 32.0); // the pattern repeats every 128 x 32 samples (its frequencies are not exact: approximate)
    return y;
}
} // namespace

TEST_CASE ("alias: the bend with sync, ring and a glide across converter switches (Model A)", "[chain][alias][bend]")
{
    // Chip.h "BEND": events that the renderer takes away from the line's own sweep (a sync reset, a ring flip, a
    // frequency write; Model A's converter terms switching with the frequency) carry the bend's slope term too. A bent
    // saw synced by a bent saw (3.137 : 1), a bent triangle ring-modulated by it (5 : 2), 88.2 / 96 / 192 kHz: aliasing off the
    // source's lines at most -96 dB re the strongest line (8.4.5, Model A, with sync and ring). The glide is printed (its
    // output is not periodic: no gate).
    for (const double rate : { 88200.0, 96000.0, 192000.0 })
        for (const int which : { 0, 1, 2 })
        {
            double lineHz = 0.0;
            const int n = 1 << 17;
            const auto y = bentPatch (rate, which, n, lineHz);
            std::vector<float> yf (y.begin(), y.end());
            const auto a = offGridAlias (windowedMagnitudes (yf), rate, 20000.0, lineHz, 9.0);
            const char* name = which == 0 ? "sync" : (which == 1 ? "ring" : "glide");
            std::printf ("[bend] %s at %.0f Hz: %.1f dB at %.1f Hz\n", name, rate, a.worstDb, a.worstHz);
            INFO (name << " at " << rate << " Hz: " << a.worstDb << " dB at " << a.worstHz << " Hz");
            if (which != 2)
                CHECK (a.worstDb <= -96.0);
        }
}
