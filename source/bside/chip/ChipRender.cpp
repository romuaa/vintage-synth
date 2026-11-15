// Band-limited rendering of the chip's waveforms (docs/architecture.md 8.4.5). Design: "RENDERING" at the
// end of Chip.h. Everything here is reachable from the audio thread: no allocation, no locks, no
// exceptions, bounded loops. The digital state is advanced only through advanceCycles / stepCycle, so it
// is exactly what stepping gives (tested).

#include "bside/chip/Chip.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vs::bside::chip
{
namespace
{
constexpr double kTwo32 = 4294967296.0;
constexpr double kTwo56 = 72057594037927936.0;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr int kLatency = Chip::kLatencySamples; // the long kernel's: every step is delayed to it
constexpr std::uint64_t kPieceCycles = 32; // digital cycles advanced per piece (bounds the event lists)
constexpr int kEventCap = 80;             // per oscillator and piece (<= 32 resets + 32 pull-downs, ...)
constexpr int kSweepGuard = 4096;         // events per oscillator and sweep (a backstop)

double frac01 (double x) noexcept { return x - std::floor (x); }

/** sigma = 2^(imin - 12): the spacing of the boundaries a line event stops at, in periods. */
constexpr double kSigma[12] { 1.0 / 4096, 1.0 / 2048, 1.0 / 1024, 1.0 / 512, 1.0 / 256, 1.0 / 128, 1.0 / 64, 1.0 / 32, 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2 };
double sigmaOf (int imin) noexcept { return kSigma[static_cast<std::size_t> (imin)]; }

/** The lowest index bit whose square wave stays below half the internal rate (Model A converter terms):
    bit i toggles 2^(12 - i) times per period. */
int iminFor (double periodsPerSample) noexcept
{
    for (int i = 0; i < 11; ++i)
        if (periodsPerSample * std::ldexp (1.0, 12 - i) < 1.0)
            return i;
    return 11;
}

/** iminFor with hysteresis (S2b-2 reviews): a bit is dropped as soon as it would toggle once per sample, but
    taken back only once it toggles at most 0.5 times per sample (its square wave then lies below a quarter
    of the internal rate; between, its content lies above 22 kHz at every internal rate, so leaving it out
    changes nothing audible). A vibrato would have to span an octave to switch it back and forth. */
int iminWithHysteresis (double periodsPerSample, int current) noexcept
{
    const int must = iminFor (periodsPerSample);      // bits below this one alias
    const int may = iminFor (periodsPerSample / 0.5); // bits from this one on are clearly below the limit
    if (must > current)
        return must;
    if (may < current)
        return may;
    return current;
}
} // namespace

// ---- events of one piece (at most kPieceCycles digital cycles inside one sample) ----

struct Chip::PieceEvents
{
    enum : std::uint8_t { kJumpSync, kJumpSyncPending, kJumpConfirm };
    struct Raw // a digital sync reset
    {
        double t;   // the source's rendered crossing
        double off; // the target's rendered minus digital phase after the reset
        bool used;
    };
    struct Jump
    {
        double t, off;
        std::uint8_t kind;
    };
    struct Held
    {
        double t;
        std::uint16_t value;
    };
    struct Flip
    {
        double t;
        bool top;
    };
    std::array<std::array<Raw, kEventCap>, 3> raw;
    std::array<std::array<Jump, kEventCap>, 3> jumps;
    std::array<std::array<Held, kEventCap>, 3> held;
    std::array<std::array<Flip, kEventCap>, 3> flips; // top-bit changes of oscillator n (a ring or sync source)
    std::array<int, 3> rawCount {}, jumpCount {}, heldCount {}, flipCount {};

    void addRaw (int n, double t, double off) noexcept
    {
        auto& c = rawCount[static_cast<std::size_t> (n)];
        if (c < kEventCap)
            raw[static_cast<std::size_t> (n)][static_cast<std::size_t> (c++)] = { t, off, false };
    }
    void addJump (int n, double t, double off, std::uint8_t kind) noexcept
    {
        auto& c = jumpCount[static_cast<std::size_t> (n)];
        if (c < kEventCap)
            jumps[static_cast<std::size_t> (n)][static_cast<std::size_t> (c++)] = { t, off, kind };
    }
    void addHeld (int n, double t, std::uint16_t v) noexcept
    {
        auto& c = heldCount[static_cast<std::size_t> (n)];
        if (c < kEventCap)
            held[static_cast<std::size_t> (n)][static_cast<std::size_t> (c++)] = { t, v };
    }
    void addFlip (int n, double t, bool top) noexcept
    {
        auto& c = flipCount[static_cast<std::size_t> (n)];
        if (c < kEventCap)
            flips[static_cast<std::size_t> (n)][static_cast<std::size_t> (c++)] = { t, top };
    }
};

namespace
{
/** Collects a piece's digital events. It reads only the renderer's own state, never the chip's digital
    state (Chip.h "EVENTS"). */
template <bool kStep>
struct RenderSinkT
{
    static constexpr bool kWantsNoiseEvents = true;
    static constexpr bool kWantsStepEvents = kStep; // per-cycle mode: which oscillator was pulled down
    void* self = nullptr;
    std::array<bool, 3> pulled {};
    void (*onSync) (void*, const SyncResetEvent&) noexcept = nullptr;
    void (*onNoise) (void*, int, std::uint64_t, std::uint16_t) noexcept = nullptr;
    // Per-cycle mode: a step's sync resets are passed on after its pull-downs (part (c) comes before (d)).
    std::array<SyncResetEvent, 3> pending {};
    int pendingCount = 0;
    void syncReset (const SyncResetEvent& e) noexcept
    {
        if constexpr (kStep)
        {
            if (pendingCount < 3)
                pending[static_cast<std::size_t> (pendingCount++)] = e;
        }
        else
            onSync (self, e);
    }
    void noiseClock (const NoiseClockEvent& e) noexcept { onNoise (self, e.osc, e.cycle, e.output); }
    void noiseWriteBack (const NoiseWriteBackEvent&) noexcept {} // write-back runs in per-cycle mode
    void oscStep (const OscStepEvent& e) noexcept
    {
        if (e.pulledDown)
            pulled[static_cast<std::size_t> (e.osc)] = true;
    }
};
} // namespace

// ---- small helpers ----

double Chip::renderSlopeU (int n) const noexcept
{
    return (ctl (n) & ctrl::test) != 0 ? 0.0 : static_cast<double> (osc[idx (n)].inc) / kTwo56;
}

double Chip::renderDigitalU (int n, double t) const noexcept
{
    const double base = static_cast<double> (osc[idx (n)].phase) / kTwo56;
    return base + renderSlopeU (n) * (t - static_cast<double> (cycleCount - rs.start));
}

std::uint8_t Chip::renderMode (int n) const noexcept
{
    const std::uint8_t c = ctl (n);
    const std::uint8_t b = wave::bits (c);
    if (b == 0)
        return rmOff;
    if (wave::writesBack (c))
        return rmCycle;
    if (b == wave::kNoise)
        return rmNoise;
    if (wave::usesTable (b))
        return rmTable;
    if (b == wave::kTri)
        return rmTri;
    if (b == wave::kSaw)
        return rmSaw;
    return rmPulse;
}

namespace
{
bool gateAt (double u, double slope, float pw, bool test) noexcept
{
    if (test || ! (pw > 0.0f))
        return true;
    return frac01 (u - slope) * 4096.0 >= static_cast<double> (pw);
}
} // namespace

double Chip::renderValue (int n, double u) const noexcept
{
    const RenderOsc& v = rs.osc[idx (n)];
    const render::RenderTables& T = *rtab;
    switch (v.mode)
    {
        case rmOff:
        case rmNoise:
        case rmCycle: return static_cast<double> (T.wave[v.held & 0xfffu]);
        case rmPulse: return static_cast<double> (v.gate ? T.wave[0xfff] : T.wave[0]);
        case rmTri:
        case rmSaw:
        {
            const render::Converter& d = T.dac;
            const double sigma = sigmaOf (v.imin);
            const auto ixBase = static_cast<std::uint32_t> ((static_cast<std::uint64_t> (v.k) << v.imin) & 0xfffu);
            const double ixc = static_cast<double> (ixBase) + 4096.0 * (u - static_cast<double> (v.k) * sigma);
            double val = 0.0;
            if (v.mode == rmSaw)
            {
                val = d.g * ixc;
                if (! d.ideal)
                {
                    val += d.excludedSaw[static_cast<std::size_t> (v.imin)];
                    for (int i = v.imin; i < 12; ++i)
                        if (((ixBase >> i) & 1u) != 0)
                            val += d.e[static_cast<std::size_t> (i)];
                }
            }
            else
            {
                const std::uint32_t top = (ixBase >> 11) & 1u;
                const std::uint32_t b = top ^ (v.ringFlip ? 1u : 0u);
                const double x = ixc - 2048.0 * static_cast<double> (top);
                const double c = b != 0 ? 2048.0 - x : x;
                val = d.g * 2.0 * c;
                if (! d.ideal)
                {
                    val += d.excludedTri[static_cast<std::size_t> (v.imin)];
                    for (int i = v.imin; i < 11; ++i)
                        if ((((ixBase >> i) & 1u) ^ b) != 0)
                            val += d.e[static_cast<std::size_t> (i + 1)];
                }
            }
            return val - d.center;
        }
        case rmTable:
        {
            const std::uint8_t c = ctl (n);
            if ((c & ctrl::pulse) != 0 && ! v.gate)
                return static_cast<double> (T.wave[0]);
            const int slot = render::tableSlot (static_cast<std::uint8_t> (wave::bits (c) & 7u));
            if (slot < 0)
                return 0.0;
            const auto cell = static_cast<std::size_t> ((static_cast<std::uint64_t> (v.k) + (v.ringFlip ? 2048u : 0u)) & 0xfffu);
            return static_cast<double> (T.steps[static_cast<std::size_t> (slot)].value[cell]);
        }
        default: return 0.0;
    }
}

double Chip::renderValueAt (int n, double u) noexcept
{
    RenderOsc& v = rs.osc[idx (n)];
    if (v.mode != rmTri && v.mode != rmSaw && v.mode != rmTable)
        return renderValue (n, u);
    const std::int64_t keep = v.k;
    const double sigma = sigmaOf (v.imin);
    const double w = frac01 (u);
    v.k = static_cast<std::int64_t> (std::floor (w / sigma));
    const double r = renderValue (n, w);
    v.k = keep;
    return r;
}

double Chip::renderDerivativeAt (int n, double u) noexcept
{
    RenderOsc& v = rs.osc[idx (n)];
    if (v.mode != rmTri)
        return renderDerivative (n, u);
    const std::int64_t keep = v.k;
    const double w = frac01 (u);
    v.k = static_cast<std::int64_t> (std::floor (w / sigmaOf (v.imin)));
    const double r = renderDerivative (n, w);
    v.k = keep;
    return r;
}

double Chip::renderDerivative (int n, double) const noexcept
{
    const RenderOsc& v = rs.osc[idx (n)];
    const render::RenderTables& T = *rtab;
    switch (v.mode)
    {
        case rmSaw: return T.dac.g * 4096.0;
        case rmTri:
        {
            const auto ixBase = static_cast<std::uint32_t> ((static_cast<std::uint64_t> (v.k) << v.imin) & 0xfffu);
            const std::uint32_t b = ((ixBase >> 11) & 1u) ^ (v.ringFlip ? 1u : 0u);
            return T.dac.g * (b != 0 ? -8192.0 : 8192.0);
        }
        default: return 0.0;
    }
}

double Chip::renderNextLineEvent (int n, double t, double slope, double& ue, int& kind) const noexcept
{
    const RenderOsc& v = rs.osc[idx (n)];
    if (! (slope > 0.0))
        return kInf;
    switch (v.mode)
    {
        case rmTri:
        case rmSaw:
        {
            ue = static_cast<double> (v.k + 1) * sigmaOf (v.imin);
            kind = 1;
            return t + (ue - v.u) / slope;
        }
        case rmPulse:
        case rmTable:
        {
            double best = kInf;
            if (v.mode == rmTable)
            {
                // the next cell whose value differs (exact staircase)
                const int slot = render::tableSlot (static_cast<std::uint8_t> (wave::bits (ctl (n)) & 7u));
                if (slot >= 0)
                {
                    const auto cell = static_cast<std::size_t> ((static_cast<std::uint64_t> (v.k) + (v.ringFlip ? 2048u : 0u)) & 0xfffu);
                    const std::uint16_t d = rtab->steps[static_cast<std::size_t> (slot)].nextChange[cell];
                    if (d != 0)
                    {
                        ue = static_cast<double> (v.k + d) / 4096.0;
                        kind = 1;
                        best = t + (ue - v.u) / slope;
                    }
                }
                if (cfg.model == Model::A && (ctl (n) & ctrl::saw) != 0 && (static_cast<std::uint64_t> (v.k) & 0xfffu) < 2048u)
                {
                    // the top bit rises: a possible pull-down even where the value does not change
                    const double uh = static_cast<double> ((v.k & ~std::int64_t { 0xfff }) + 2048) / 4096.0;
                    const double th = t + (uh - v.u) / slope;
                    if (th < best)
                    {
                        ue = uh;
                        kind = 1;
                        best = th;
                    }
                }
                if (v.landPull)
                {
                    const double tl = static_cast<double> (v.landStart) - static_cast<double> (rs.start) + v.landT;
                    if (tl < best)
                    {
                        ue = v.u + slope * (tl - t);
                        kind = 4;
                        best = tl;
                    }
                }
                if ((ctl (n) & ctrl::pulse) == 0)
                    return best;
            }
            const double pw = static_cast<double> (osc[idx (n)].pwSmooth) / 4096.0;
            if (! (pw > 0.0))
                return best;
            const double vph = v.u - slope; // the comparison is one cycle late (8.15.2 (c))
            const double fl = std::floor (vph);
            const double fr = vph - fl;
            double up = 0.0;
            if (v.gate)
                up = (fr < 1.0e-9 ? fl : fl + 1.0) + slope;
            else
                up = (fr < pw + 1.0e-9 ? fl + pw : fl + 1.0 + pw) + slope;
            const double tp = t + (up - v.u) / slope;
            if (tp < best)
            {
                ue = up;
                kind = 2;
                best = tp;
            }
            if (v.gateCheckU >= 0.0)
            {
                const double tq = t + (v.gateCheckU - v.u) / slope;
                if (tq < best)
                {
                    ue = v.gateCheckU;
                    kind = 3;
                    best = tq;
                }
            }
            return best;
        }
        default: return kInf;
    }
}

// ---- Model A pull-down, predicted from the line ----

bool Chip::renderGateAtRise (int n, double tCross) const noexcept
{
    // The pulse comparison the chip uses in the first cycle of the upper half was made in the cycle before,
    // on the whole-cycle accumulator (8.15.2 (c)): find that cycle on the digital line (the rendered line
    // minus offU) and compare its index with the integer width, as the chip does.
    if ((ctl (n) & ctrl::pulse) == 0 || (ctl (n) & ctrl::test) != 0)
        return true;
    const double slope = renderSlopeU (n);
    if (! (slope > 0.0))
        return rs.osc[idx (n)].gate;
    const double tD = tCross + rs.osc[idx (n)].offU / slope; // the digital line crosses its top bit here
    const double e = std::ceil (tD - 1.0e-9);                // the first whole cycle in the upper half
    const RenderOsc& v = rs.osc[idx (n)];
    if (std::abs (static_cast<double> (rs.start) + e - 1.0 - static_cast<double> (v.cmpCycle)) < 0.5)
        return v.cmpHigh; // made in the step a write followed: as the write found it
    const double uPrev = 0.5 - slope * (tD - (e - 1.0));     // the digital phase one cycle before
    const auto ix = static_cast<std::uint32_t> (std::floor (frac01 (uPrev) * 4096.0)) & 0xfffu;
    return ix >= osc[idx (n)].pw;
}

int Chip::renderPullCell (int n, int cell, double tCross) const noexcept
{
    // Where the line of a Model A saw combination, entering `cell` (or the upper half) from below, is pulled
    // down: the first cell from there whose output has bit 11 clear, the pulse gate (one cycle late)
    // included. 4096: never (a combination whose upper half keeps bit 11 everywhere).
    const int slot = render::tableSlot (static_cast<std::uint8_t> (wave::bits (ctl (n)) & 7u));
    if (slot < 0)
        return 4096;
    const int c = std::max (cell, 2048);
    if ((ctl (n) & ctrl::pulse) != 0)
    {
        // entering the upper half: the gate as the chip decides it (renderGateAtRise); already inside it: the
        // rendered gate (the gate only closes at a wrap, so inside the upper half it can only have opened)
        const bool open = cell <= 2048 ? renderGateAtRise (n, tCross) : rs.osc[idx (n)].gate;
        if (! open)
            return c;
    }
    return rtab->steps[static_cast<std::size_t> (slot)].firstClear[static_cast<std::size_t> (c)];
}

bool Chip::renderPullDown (int n, double t, bool atWrite) noexcept
{
    // 8.15.2 (c) step 4: Model A, a combination with the saw: in a cell of the upper half whose output has
    // bit 11 clear, the accumulator's top bit is cleared. The renderer does it where the line enters such a
    // cell (or where the pulse gate closes in one); the digital core does it in the cycle it lands there,
    // up to one cycle later (re-anchoring waits three cycles, as for a predicted sync reset). After a write
    // inside the upper half the chip's own landings decide instead (renderLanding).
    RenderOsc& v = rs.osc[idx (n)];
    if (cfg.model != Model::A || v.mode != rmTable || (ctl (n) & ctrl::saw) == 0)
        return false;
    const auto cell = static_cast<std::size_t> (static_cast<std::uint64_t> (v.k) & 0xfffu);
    if (cell < 2048)
    {
        v.landHold = false; // the line has left the upper half: the ordinary rule again
        return false;
    }
    if (v.landHold)
        return false; // a write left the line inside the upper half: the chip's own landings decide (renderLanding)
    const int slot = render::tableSlot (static_cast<std::uint8_t> (wave::bits (ctl (n)) & 7u));
    if (slot < 0)
        return false;
    const bool pulse = (ctl (n) & ctrl::pulse) != 0;
    // the pulse gate: at a write, the chip's own comparison for its next step; entering the upper half, as
    // the chip decides it at whole cycles; inside it, the rendered gate (it only opens there)
    const bool open = ! pulse || (ctl (n) & ctrl::test) != 0 || (atWrite ? osc[idx (n)].pulseHigh : cell == 2048 ? renderGateAtRise (n, t) : v.gate);
    if (pulse && cell == 2048 && open && ! atWrite)
        v.gate = true; // the chip's gate is open in this first upper-half cycle; the rendered one follows it
    const bool high = (rtab->steps[static_cast<std::size_t> (slot)].shape[cell] & 0x800u) != 0 && open;
    if (high)
        return false;
    v.u -= 0.5;
    v.k -= 2048;
    v.pending = true;
    v.pendingPull = true;
    v.pendingStart = rs.start;
    v.pendingT = t;
    return true;
}

// ---- a write inside the upper half: the chip's own landings ----

void Chip::renderLanding (int n, double tw) noexcept
{
    // After a write the line of a Model A saw combination may sit inside the upper half, where the chip pulls
    // it down in the first step whose landing cell (whole cycles: up to 16 cells apart) has output bit 11
    // clear, or whose pulse comparison (made one step earlier, on the previous landing) is low. The continuous
    // line would see cells the chip skips, so the step is found here from the accumulator itself, exactly, and
    // the pull-down scheduled for it. Bounded: 4096 steps (half a period at one cell per step or more; a
    // slower line visits every cell, and the line's own rule takes over where the search ends).
    RenderOsc& v = rs.osc[idx (n)];
    v.landPull = false;
    v.landHold = false;
    if (cfg.model != Model::A || v.mode != rmTable || (ctl (n) & ctrl::saw) == 0 || (ctl (n) & ctrl::test) != 0)
        return;
    const Osc& o = osc[idx (n)];
    if (o.inc == 0)
        return;
    if ((o.phase & kPhaseMsb) == 0)
        return; // the lower half: the next rise is the line's own rule
    if (frac01 (v.u) < 0.5 && std::abs (frac01 (v.u - renderDigitalU (n, rs.sweepT) - v.offU + 0.5) - 0.5) > 0.25)
        return; // the rendered line is already down (a pull-down the chip makes in its next step)
    // (otherwise the line is still in step with the chip, inside its upper half or, less than a cycle ahead,
    // already past the wrap: a pull-down the chip makes before its wrap moves it by half a period too)
    const int slot = render::tableSlot (static_cast<std::uint8_t> (wave::bits (ctl (n)) & 7u));
    if (slot < 0)
        return;
    const auto& shape = rtab->steps[static_cast<std::size_t> (slot)].shape;
    const bool pulse = (ctl (n) & ctrl::pulse) != 0;
    v.landHold = true; // from here until the line leaves the upper half, only these landings decide
    bool gate = o.pulseHigh; // the comparison the next step uses
    std::uint64_t phase = o.phase;
    for (int k = 1; k <= 4096; ++k)
    {
        phase = (phase + o.inc) & kPhaseMask;
        if ((phase & kPhaseMsb) == 0)
            return; // wrapped first: no pull-down
        const auto cell = static_cast<std::size_t> ((phase >> 44) & 0xfffu);
        const bool high = (shape[cell] & 0x800u) != 0 && (! pulse || gate);
        if (! high)
        {
            v.landPull = true;
            v.landStart = rs.start;
            v.landT = tw + static_cast<double> (k - 1); // in the cycle that ends with the step that pulls
            return;
        }
        gate = cell >= o.pw;
    }
    v.landHold = false; // not decided within 4096 steps (a slow line, below one cell per step): the line's own rule
}

// ---- emitting a discontinuity ----

namespace
{
/** Adds a band-limited step dv and slope change ds at `base` samples before the grid point of sample
    `sample` into a ring, with the kernel `tab` (its window +-Z around the sample; the output is read
    kLatency + 1 samples behind, so a short kernel's step comes out exactly as before, only later). */
template <bool kSecondOrder, class Table, std::size_t N>
void addStep (const Table& tab, std::array<double, N>& buf, std::uint64_t sample, double base, double dv, double ds) noexcept
{
    constexpr int z = Table::kZeroCrossings;
    static_assert (2 * z + 2 <= static_cast<int> (N), "the output ring must hold the BLEP window and one sample of look-back");
    for (int m = -(z + 1); m <= z; ++m)
    {
        const double tt = base + static_cast<double> (m);
        if (tt <= -static_cast<double> (z))
            continue;
        const bool included = m >= 0; // samples from the current one on are evaluated after the event
        double add = 0.0;
        if (tt > static_cast<double> (z))
        {
            if (included)
                break;
            add = dv + ds * tt; // (only reachable with a clamped base; kept for completeness)
        }
        else
        {
            const int j = static_cast<int> (std::ceil (tt)) - 1;
            using S = typename Table::SampleType;
            const auto d = static_cast<S> (tt - static_cast<double> (j));
            double stepCum = static_cast<double> (tab.step (j, d)) + (j >= 0 ? 1.0 : 0.0);
            const double pos = tt > 0.0 ? tt : 0.0;
            double rampCum = static_cast<double> (tab.ramp (j, d)) + pos;
            if constexpr (kSecondOrder)
            {
                // The tables interpolate linearly between kResolution points per sample, an error of about
                // 1e-6 of a step; under the long kernel's deeper stop band that is what remains of a nearly
                // silent combined shape. Remove it with the second derivative, which the kernel gives in
                // closed form (step'' = kernel', ramp'' = kernel).
                constexpr double res = Table::kResolution;
                const double p = static_cast<double> (d) * res;
                const double cellFrac = p - std::fmin (std::floor (p), res - 1.0);
                const double w = 0.5 * cellFrac * (1.0 - cellFrac) / (res * res);
                const auto tf = static_cast<S> (tt);
                constexpr S e = S (1) / S (2048);
                const double k1 = (static_cast<double> (tab.kernelAt (tf + e)) - static_cast<double> (tab.kernelAt (tf - e))) / (2.0 * static_cast<double> (e));
                stepCum -= w * k1;
                rampCum -= w * static_cast<double> (tab.kernelAt (tf));
            }
            add = dv * (stepCum - (included ? 1.0 : 0.0)) + ds * (rampCum - (included ? pos : 0.0));
        }
        const auto slot = static_cast<std::size_t> ((sample + static_cast<std::uint64_t> (N) + static_cast<std::uint64_t> (static_cast<std::int64_t> (m)))
                                                    % static_cast<std::uint64_t> (N));
        buf[slot] += add;
    }
}
} // namespace

void Chip::renderEmit (int n, double t, double dv, double ds) noexcept
{
    renderEmitWith (n, t, dv, ds, rs.osc[idx (n)].longKernel);
}

void Chip::renderEmitWith (int n, double t, double dv, double ds, bool longKernel) noexcept
{
    if (! (std::abs (dv) > 1.0e-15) && ! (std::abs (ds) > 1.0e-18))
        return;
    RenderOsc& v = rs.osc[idx (n)];
    // time from the event to the grid point of the current sample (its end), in samples; an event lies at
    // most one sample before the current sample's start
    const double base = std::fmin (std::fmax ((rs.tEnd - t) / rs.cps, 0.0), 2.0);
    if (longKernel)
        addStep<true> (*blepLong, v.buf, rs.sample, base, dv, ds);
    else
        addStep<false> (*blep, v.buf, rs.sample, base, dv, ds);
}

// ---- state from the digital core ----

void Chip::renderSyncOsc (int n) noexcept
{
    RenderOsc& v = rs.osc[idx (n)];
    const std::uint8_t c = ctl (n);
    const std::uint8_t previousMode = rs.active && v.mode != rmOff ? v.mode : std::uint8_t { rmOff };
    v.mode = renderMode (n);
    v.landPull = false;
    v.landHold = false;
    const double slope = renderSlopeU (n);
    if (! v.pending) // a predicted, unconfirmed sync reset: the digital phase has not been reset yet
        v.u = frac01 (renderDigitalU (n, rs.sweepT) + v.offU);
    const double rate = slope * rs.cps;
    // the hysteresis continues only from a triangle or saw; any other previous mode starts it afresh
    const bool converterTerms = ! rtab->dac.ideal && (v.mode == rmTri || v.mode == rmSaw);
    const bool continues = converterTerms && (previousMode == rmTri || previousMode == rmSaw);
    const int imin = v.mode == rmTable ? 0 : converterTerms ? (continues ? iminWithHysteresis (rate, v.imin) : iminFor (rate)) : 11;
    if (continues && imin != v.imin)
        ++rs.converterSwitches;
    v.imin = imin;
    v.k = static_cast<std::int64_t> (std::floor (v.u / sigmaOf (v.imin)));
    v.ring = (c & ctrl::ring) != 0 && (c & ctrl::saw) == 0 && (v.mode == rmTri || v.mode == rmTable);
    v.longKernel = v.mode == rmTable || v.mode == rmPulse; // combined waveforms and pulse edges (docs/decisions.md S2b-2)
    v.gate = gateAt (v.u, slope, osc[idx (n)].pwSmooth, (c & ctrl::test) != 0);
    v.gateCheckU = -1.0;
    v.shownTop = (osc[idx (n)].phase & kPhaseMsb) != 0;
    v.held = v.mode == rmNoise ? osc[idx (n)].noiseOut : osc[idx (n)].out;
}

void Chip::renderActivate() noexcept
{
    rs = RenderState {};
    rs.active = true;
    rs.cps = static_cast<double> (clock.increment()) / kTwo32;
    renderStartSample();
    for (int n = 0; n < 3; ++n)
        renderSyncOsc (n);
    for (int n = 0; n < 3; ++n)
    {
        RenderOsc& v = rs.osc[idx (n)];
        v.ringFlip = v.ring && frac01 (rs.osc[idx (detail::syncSource (n))].u) < 0.5;
        // The samples before the first one (the output's first kLatencySamples + 1 samples come from them)
        // hold the state at activation, so the output starts on its value instead of stepping to it from 0.
        const double value = renderValue (n, v.u);
        const float level = rtab->envelope[env[idx (n)].counter];
        for (int k = 1; k <= kLatency + 1; ++k)
        {
            const auto slot = static_cast<std::size_t> ((static_cast<std::uint64_t> (kRing) - static_cast<std::uint64_t> (k)) % static_cast<std::uint64_t> (kRing));
            v.buf[slot] = value;
            v.env[slot] = level;
        }
    }
}

void Chip::renderStartSample() noexcept
{
    rs.start = cycleCount;
    const std::uint32_t f = clock.fraction();
    rs.cycles = takeSampleCycles();
    rs.tStart = static_cast<double> (f) / kTwo32;
    rs.tEnd = rs.tStart + rs.cps;
    rs.sweepT = rs.tStart; // the previous sample's end, in this sample's time
    rs.inProgress = true;
}

void Chip::renderBeforeChange() noexcept
{
    if (! rs.inProgress)
        renderStartSample();
    const double tw = static_cast<double> (cycleCount - rs.start);
    for (int n = 0; n < 3; ++n)
    {
        const double slope = renderSlopeU (n);
        const double u = rs.osc[idx (n)].u - slope * (rs.sweepT - tw); // the line at the change's cycle
        rs.leftValue[idx (n)] = renderValueAt (n, u);
        rs.leftSlope[idx (n)] = renderDerivativeAt (n, u) * slope * rs.cps;
        rs.leftSlopeU[idx (n)] = slope;
    }
}

void Chip::renderAfterChange (int testSetMask) noexcept
{
    std::array<bool, 3> longBefore {};
    for (int n = 0; n < 3; ++n)
        longBefore[idx (n)] = rs.osc[idx (n)].longKernel;
    // The change happened at the current cycle: after the last advanced cycle of the sample in progress, or
    // (right after a completed sample) up to one cycle before the sweep position; values are taken at the
    // sweep position, the discontinuity is placed at the change's own cycle.
    const double tw = static_cast<double> (cycleCount - rs.start);
    for (int n = 0; n < 3; ++n)
    {
        RenderOsc& v = rs.osc[idx (n)];
        v.cmpCycle = cycleCount; // the pulse comparison the chip's next step uses, as the write found it
        v.cmpHigh = osc[idx (n)].pulseHigh;
        if (((testSetMask >> n) & 1) != 0)
        {
            v.offU = 0.0; // the accumulator was set: rendered and digital phase agree again
            v.pending = false;
        }
        // offU is a sub-cycle time (where a sync reset fell between two of the chip's steps) held as a phase:
        // a new frequency rescales it, so rendered and digital line stay less than one cycle apart in time
        const double s0 = rs.leftSlopeU[idx (n)];
        if (s0 > 0.0)
        {
            v.offU *= renderSlopeU (n) / s0;
            // likewise a predicted sync reset not yet confirmed: the line since the reset is that time
            if (v.pending && ! v.pendingPull)
                v.u *= renderSlopeU (n) / s0;
        }
        // A write ends the wait for a predicted pull-down: the line goes back to the digital phase
        // (renderSyncOsc), pulled if the chip has made it, and if not, the chip decides it with the new
        // controls (renderLanding, renderPullDown below). A predicted sync reset whose sync bit the write
        // cleared will not come: dropped likewise.
        if (v.pending && (v.pendingPull || (ctl (n) & ctrl::sync) == 0))
            v.pending = false;
        renderSyncOsc (n);
    }
    for (int n = 0; n < 3; ++n)
    {
        RenderOsc& v = rs.osc[idx (n)];
        renderLanding (n, tw);
        if (! v.landPull && renderPullDown (n, tw, true))
            v.gateCheckU = v.u + renderSlopeU (n);
    }
    for (int n = 0; n < 3; ++n) // after the sources' pull-downs at the write
    {
        RenderOsc& v = rs.osc[idx (n)];
        v.ringFlip = v.ring && frac01 (rs.osc[idx (detail::syncSource (n))].u) < 0.5;
    }
    for (int n = 0; n < 3; ++n)
    {
        RenderOsc& v = rs.osc[idx (n)];
        const double slope = renderSlopeU (n);
        const double u = v.u - slope * (rs.sweepT - tw);
        const double wr = renderValueAt (n, u);
        const double sr = renderDerivativeAt (n, u) * slope * rs.cps;
        renderEmitWith (n, tw, wr - rs.leftValue[idx (n)], sr - rs.leftSlope[idx (n)], v.longKernel || longBefore[idx (n)]);
    }
}

// ---- one oscillator's line over a stretch of time ----

void Chip::renderSweep (int n, double tEnd, const PieceEvents* ev) noexcept
{
    RenderOsc& v = rs.osc[idx (n)];
    const double slope = renderSlopeU (n);
    const double rate = slope * rs.cps;
    const auto ni = idx (n);
    const auto src = idx (detail::syncSource (n));
    const int nd = ev != nullptr ? ev->jumpCount[ni] : 0;
    const int nh = ev != nullptr ? ev->heldCount[ni] : 0;
    const int nr = ev != nullptr && v.ring ? ev->flipCount[src] : 0;
    int id = 0, ih = 0, ir = 0;
    double t = rs.sweepT;
    const bool test = (ctl (n) & ctrl::test) != 0;
    for (int guard = 0; guard < kSweepGuard; ++guard)
    {
        double ue = 0.0;
        int kind = 0;
        const double tl = renderNextLineEvent (n, t, slope, ue, kind);
        const double td = id < nd ? ev->jumps[ni][static_cast<std::size_t> (id)].t : kInf;
        const double th = ih < nh ? ev->held[ni][static_cast<std::size_t> (ih)].t : kInf;
        const double tr = ir < nr ? ev->flips[src][static_cast<std::size_t> (ir)].t : kInf;
        const double tn = std::fmin (std::fmin (tl, td), std::fmin (th, tr));
        if (! (tn <= tEnd))
            break;
        if (tl <= td && tl <= th && tl <= tr)
        {
            // a boundary of the line itself (index bits, fold, wrap) or a pulse edge
            if (tl > t)
            {
                v.u = ue;
                t = tl;
            }
            const double wl = renderValue (n, v.u);
            const double sl = renderDerivative (n, v.u);
            if (kind == 1)
            {
                v.k = static_cast<std::int64_t> (std::llround (ue * (1.0 / sigmaOf (v.imin))));
                const std::int64_t period = std::int64_t { 1 } << (12 - v.imin);
                if (v.k >= period)
                {
                    v.k -= period;
                    v.u -= 1.0;
                    if (v.gateCheckU >= 0.0)
                        v.gateCheckU -= 1.0;
                }
            }
            else if (kind == 2)
                v.gate = ! v.gate;
            else if (kind == 3)
            {
                // one cycle after a phase jump the comparison reads the new line
                v.gate = gateAt (v.u, slope, osc[ni].pwSmooth, test);
                v.gateCheckU = -1.0;
            }
            if (kind == 4)
            {
                // the pull-down the chip makes at the landing renderLanding found
                v.landPull = false;
                v.landHold = false;
                v.u -= 0.5;
                v.k = static_cast<std::int64_t> (std::floor (frac01 (v.u) / sigmaOf (v.imin)));
                v.u = frac01 (v.u);
                v.pending = true;
                v.pendingPull = true;
                v.pendingStart = rs.start;
                v.pendingT = t;
                v.gateCheckU = v.u + slope;
            }
            else if (renderPullDown (n, t, false))
                v.gateCheckU = v.u + slope;
            const double wr = renderValue (n, v.u);
            const double sr = renderDerivative (n, v.u);
            renderEmit (n, t, wr - wl, (sr - sl) * rate);
            continue;
        }
        const double te = std::fmax (tn, t);
        v.u += slope * (te - t);
        t = te;
        // values at the event's own time (an event can lie up to two cycles before the sweep position)
        const double back = slope * (t - tn);
        const double wl = renderValueAt (n, v.u - back);
        const double sl = renderDerivativeAt (n, v.u - back);
        double uAfter = v.u - back;
        double lateEdge = -1.0; // a falling pulse edge one cycle after a reset that this sweep reaches late
        if (td <= th && td <= tr)
        {
            const auto& d = ev->jumps[ni][static_cast<std::size_t> (id++)];
            switch (d.kind)
            {
                case PieceEvents::kJumpSync:
                case PieceEvents::kJumpSyncPending:
                    v.landPull = false;
                    v.landHold = false;
                    v.u = test ? 0.0 : slope * (t - d.t);
                    uAfter = 0.0;
                    v.pending = d.kind == PieceEvents::kJumpSyncPending;
                    v.pendingPull = false;
                    if (v.pending)
                    {
                        v.pendingStart = rs.start;
                        v.pendingT = d.t;
                    }
                    else
                        v.offU = test ? 0.0 : d.off;
                    if (! test && v.gate && (v.mode == rmPulse || (v.mode == rmTable && (ctl (n) & ctrl::pulse) != 0)) && t - d.t >= 1.0 - 1.0e-9)
                        lateEdge = d.t + 1.0;
                    break;
                default: // kJumpConfirm
                    v.offU = test ? 0.0 : d.off;
                    v.pending = false;
                    break;
            }
            v.u = frac01 (v.u);
            v.k = static_cast<std::int64_t> (std::floor (v.u / sigmaOf (v.imin)));
            // the pulse comparison is one cycle late (8.15.2 (c)): for one more cycle it still reads the old
            // line, so the gate keeps its state; after a reset to 0 its next edge, computed from the new line,
            // already lies one cycle on (a falling edge at the next wrap of the delayed line, u = slope)
        }
        else if (th <= tr)
        {
            v.held = ev->held[ni][static_cast<std::size_t> (ih++)].value;
            if (v.mode == rmOff)
            {
                // a fade step of a deselected output: on the stronger kernel too (a step of up to half the
                // output, which may follow a pulse); the step is emitted here and the generic emit skipped
                const double wr = renderValueAt (n, uAfter);
                renderEmitWith (n, tn, wr - wl, 0.0, true);
                continue;
            }
        }
        else
        {
            v.ringFlip = v.ring && ! ev->flips[src][static_cast<std::size_t> (ir++)].top;
            // a combined shape tracks only the cells where its value changes; with the other half of the
            // table in use, the current cell must be known exactly
            if (v.mode == rmTable)
            {
                v.k = static_cast<std::int64_t> (std::floor (v.u * 4096.0));
                if (v.k >= 4096)
                {
                    v.k -= 4096;
                    v.u -= 1.0;
                }
            }
        }
        const double wr = renderValueAt (n, uAfter);
        const double sr = renderDerivativeAt (n, uAfter);
        renderEmit (n, tn, wr - wl, (sr - sl) * rate);
        if (lateEdge >= 0.0)
        {
            // the gate's falling edge (one cycle after the reset) lies already behind the sweep position:
            // emitted at its own time, as the next line event would have
            const double uf = v.u - slope * (t - lateEdge);
            const double hl = renderValueAt (n, uf);
            const double hs = renderDerivativeAt (n, uf);
            v.gate = false;
            renderEmit (n, lateEdge, renderValueAt (n, uf) - hl, (renderDerivativeAt (n, uf) - hs) * rate);
        }
    }
    if (tEnd > t)
        v.u += slope * (tEnd - t);
}

// ---- the three lines together: predicted sync resets, ring and sync sources' top-bit changes ----
//
// A sync reset is known to the digital core only at the cycle after the source's top bit rose, while the
// rendered line has to place it at the source's crossing, which can lie up to two cycles earlier, possibly
// in a stretch already rendered. So the renderer predicts every reset from the source's rendered line (the
// same line the digital accumulator lies on) and takes the digital reset, when it arrives, as the
// confirmation that sets the target's phase offset. A digital reset without a prediction is applied where
// it lies (looking back); a prediction the digital core never confirms (the sync-chain exception of
// 8.15.2 (d)) times out after three cycles and the line is re-anchored with a band-limited correction.

void Chip::renderLines (PieceEvents& ev, double tEnd) noexcept
{
    constexpr double kMatch = 1.0e-3; // cycles between a predicted and a digital reset of the same crossing
    std::array<double, 3> u {}, slope {}, pullU {};
    std::array<bool, 3> top {}, wanted {}, synced {}, pulls {}, digital {};
    for (int n = 0; n < 3; ++n)
    {
        const auto ni = idx (n);
        u[ni] = rs.osc[ni].u;
        slope[ni] = renderSlopeU (n);
        top[ni] = frac01 (u[ni]) >= 0.5;
        synced[ni] = (ctl (n) & ctrl::sync) != 0 && (ctl (n) & ctrl::test) == 0;
        // a Model A saw combination is pulled down in the upper half (8.15.2 (c) step 4): its top bit falls
        // where renderSweep's renderPullDown will pull it, and a rise it cannot keep syncs nothing
        pulls[ni] = cfg.model == Model::A && rs.osc[ni].mode == rmTable && (ctl (n) & ctrl::saw) != 0 && slope[ni] > 0.0;
        // a source in per-cycle mode (noise with another waveform) is not predicted: its output, and on
        // Model A its pull-down (which depends on the noise output), are known only cycle by cycle, so its
        // ring flips come from the digital accumulator (renderPiece) and its sync resets from the digital core
        digital[ni] = rs.osc[ni].mode == rmCycle;
        pullU[ni] = -1.0;
        if (pulls[ni] && rs.osc[ni].landPull)
        {
            const double tl = static_cast<double> (rs.osc[ni].landStart) - static_cast<double> (rs.start) + rs.osc[ni].landT;
            pullU[ni] = u[ni] + slope[ni] * std::fmax (tl - rs.sweepT, 0.0);
        }
        else if (pulls[ni] && top[ni] && ! rs.osc[ni].landHold)
        {
            const double base = std::floor (u[ni]);
            const int c = renderPullCell (n, static_cast<int> ((u[ni] - base) * 4096.0), rs.sweepT);
            if (c < 4096)
                pullU[ni] = base + static_cast<double> (c) / 4096.0;
        }
    }
    for (int n = 0; n < 3; ++n)
    {
        const auto tg = idx (detail::syncTarget (n));
        wanted[idx (n)] = rs.osc[tg].ring || synced[tg];
    }
    // A ring flip of oscillator 1's source (oscillator 3) that falls after the stretch rendered now: the end
    // of a one-cycle glitch (see below), carried over
    for (int n = 0; n < 3; ++n)
    {
        RenderOsc& v = rs.osc[idx (n)];
        if (! v.glitchPending)
            continue;
        const double when = static_cast<double> (v.glitchStart) - static_cast<double> (rs.start) + v.glitchT;
        if (when <= tEnd)
        {
            if (wanted[idx (n)] && ! digital[idx (n)])
                ev.addFlip (n, std::fmax (when, rs.sweepT), false);
            v.glitchPending = false;
        }
    }
    if (! wanted[0] && ! wanted[1] && ! wanted[2] && ev.rawCount[0] == 0 && ev.rawCount[1] == 0 && ev.rawCount[2] == 0)
        return; // no sync, no ring, no digital jump: nothing couples the lines
    // Oscillator 1 reads oscillator 3's top bit before oscillator 3's own output step (8.15.2 (c) step 1):
    // where oscillator 3 is pulled down, oscillator 1 still sees the top bit of that cycle, one cycle longer
    // than oscillators 2 and 3 see their sources'. The flip back of that source is placed one cycle later.
    auto flipBack = [&] (int n, double when) noexcept {
        const auto ni = idx (n);
        if (! wanted[ni])
            return;
        if (detail::syncTarget (n) != 0)
        {
            ev.addFlip (n, when, false);
            return;
        }
        const double late = when + 1.0;
        if (late <= tEnd)
            ev.addFlip (n, late, false);
        else
        {
            RenderOsc& v = rs.osc[ni];
            v.glitchPending = true;
            v.glitchStart = rs.start;
            v.glitchT = late;
        }
    };
    auto settle = [&] (int n, double when) noexcept {
        const auto ni = idx (n);
        const bool now = frac01 (u[ni]) >= 0.5;
        if (now != top[ni])
        {
            top[ni] = now;
            if (wanted[ni] && ! digital[ni])
                ev.addFlip (n, when, now);
        }
    };
    std::array<int, 3> next {};
    double t = rs.sweepT;
    for (int guard = 0; guard < kSweepGuard; ++guard)
    {
        double tc = kInf, uc = 0.0;
        int cs = -1;
        bool pull = false;
        for (int n = 0; n < 3; ++n)
        {
            const auto ni = idx (n);
            if (! (slope[ni] > 0.0) || digital[ni])
                continue;
            // the next half-period boundary; one reached exactly at another line's crossing (lines in step: the
            // same frequency and phase) is still to be taken, now
            const bool unsettled = (frac01 (u[ni]) >= 0.5) != top[ni];
            const double uh = unsettled ? u[ni] : (std::floor (2.0 * u[ni]) + 1.0) * 0.5;
            const double th = unsettled ? t : t + (uh - u[ni]) / slope[ni];
            if (th < tc)
            {
                tc = th;
                uc = uh;
                cs = n;
                pull = false;
            }
            if (pullU[ni] >= 0.0)
            {
                const double tp = t + (pullU[ni] - u[ni]) / slope[ni];
                if (tp <= tc)
                {
                    tc = std::fmax (tp, t);
                    uc = pullU[ni];
                    cs = n;
                    pull = true;
                }
            }
        }
        double tr = kInf;
        int rn = -1;
        for (int n = 0; n < 3; ++n)
        {
            const auto ni = idx (n);
            while (next[ni] < ev.rawCount[ni] && ev.raw[ni][static_cast<std::size_t> (next[ni])].used)
                ++next[ni];
            if (next[ni] < ev.rawCount[ni] && ev.raw[ni][static_cast<std::size_t> (next[ni])].t < tr)
            {
                tr = ev.raw[ni][static_cast<std::size_t> (next[ni])].t;
                rn = n;
            }
        }
        const bool crossing = cs >= 0 && tc <= tr + 1.0e-6;
        const double tn = crossing ? tc : tr;
        if (! (tn <= tEnd))
            break;
        const double te = std::fmax (tn, t);
        for (std::size_t i = 0; i < 3; ++i)
            u[i] += slope[i] * (te - t);
        t = te;
        if (crossing && pull)
        {
            const auto ci = idx (cs);
            u[ci] = uc - 0.5 + slope[ci] * (t - tc);
            pullU[ci] = -1.0;
            if (frac01 (u[ci]) < 0.5)
            {
                top[ci] = false;
                flipBack (cs, tc);
            }
            else if (! top[ci])
            {
                // the line had run past its wrap (renderLanding): half a period on, it is up again
                top[ci] = true;
                if (wanted[ci] && ! digital[ci])
                    ev.addFlip (cs, tc, true);
            }
            continue;
        }
        if (crossing)
        {
            const auto ci = idx (cs);
            u[ci] = uc + slope[ci] * (t - tc);
            if (pulls[ci] && frac01 (uc) >= 0.25 && frac01 (uc) < 0.75)
            {
                // the top bit rises: pulled down at once (no rise, no sync), later, or never
                const double base = std::floor (uc);
                const int c = renderPullCell (cs, 2048, tc);
                if (c == 2048)
                {
                    u[ci] -= 0.5;
                    // no rise for the sync and for oscillators 2 and 3; oscillator 1 sees it for one cycle
                    if (wanted[ci] && detail::syncTarget (cs) == 0)
                    {
                        ev.addFlip (cs, tc, true);
                        flipBack (cs, tc);
                    }
                    continue;
                }
                pullU[ci] = c < 4096 ? base + static_cast<double> (c) / 4096.0 : -1.0;
            }
            settle (cs, tc);
            const int tg = detail::syncTarget (cs);
            const auto gi = idx (tg);
            if (top[idx (cs)] && synced[gi])
            {
                int found = -1;
                for (int i = 0; i < ev.rawCount[gi]; ++i)
                {
                    const auto& r = ev.raw[gi][static_cast<std::size_t> (i)];
                    if (! r.used && std::abs (r.t - tc) < kMatch)
                    {
                        found = i;
                        break;
                    }
                }
                if (found >= 0)
                {
                    auto& r = ev.raw[gi][static_cast<std::size_t> (found)];
                    r.used = true;
                    ev.addJump (tg, tc, r.off, PieceEvents::kJumpSync);
                }
                else
                    ev.addJump (tg, tc, 0.0, PieceEvents::kJumpSyncPending);
                u[gi] = slope[gi] * (t - tc);
                pullU[gi] = -1.0;
                settle (tg, tc);
            }
            continue;
        }
        auto& r = ev.raw[idx (rn)][static_cast<std::size_t> (next[idx (rn)])];
        r.used = true;
        const auto ri = idx (rn);
        const RenderOsc& v = rs.osc[ri];
        const double age = static_cast<double> (rs.start - v.pendingStart) + r.t - v.pendingT;
        if (v.pending && std::abs (age) < kMatch)
            ev.addJump (rn, r.t, r.off, PieceEvents::kJumpConfirm);
        else
        {
            ev.addJump (rn, r.t, r.off, PieceEvents::kJumpSync);
            u[ri] = slope[ri] * (t - r.t);
            pullU[ri] = -1.0;
            settle (rn, r.t);
        }
    }
}

// ---- one piece of digital time ----

void Chip::renderPiece (std::uint64_t k, bool toSampleEnd) noexcept
{
    PieceEvents ev;
    const double c0 = static_cast<double> (cycleCount - rs.start);

    bool stepEach = false;
    for (int n = 0; n < 3; ++n)
    {
        const RenderOsc& v = rs.osc[idx (n)];
        if (v.mode == rmCycle)
            stepEach = true; // the output changes cycle by cycle
    }

    std::array<std::uint32_t, 3> fadeAt {};
    std::array<std::uint16_t, 3> outBefore {};
    for (int n = 0; n < 3; ++n)
    {
        fadeAt[idx (n)] = osc[idx (n)].fadeTimer;
        outBefore[idx (n)] = osc[idx (n)].out;
    }

    struct Ctx
    {
        Chip* chip;
        PieceEvents* ev;
    } ctx { this, &ev };
    auto onSync = [] (void* p, const SyncResetEvent& e) noexcept {
        auto& c = *static_cast<Ctx*> (p);
        Chip& ch = *c.chip;
        const RenderOsc& sv = ch.rs.osc[idx (e.source)];
        const double srcSlope = static_cast<double> (e.sourceIncrement) / kTwo56;
        // the source's RENDERED line crossed its top bit (offset from a sync of its own included)
        const double before = std::fmin (std::fmax ((static_cast<double> (e.sourcePhase) / kTwo56 + sv.offU - 0.5) / srcSlope, -1.0), 2.0);
        const double eRel = static_cast<double> (e.cycle - ch.rs.start);
        const double tStar = eRel - before;
        // the target's rendered phase runs from 0 at tStar; its digital phase from 0 at E (this ignores a
        // target held by the test bit, whose phase stays 0: its offset is cleared when the bit is set)
        c.ev->addRaw (e.target, tStar, ch.renderSlopeU (e.target) * before); // registers are constant within a piece
    };
    auto onNoise = [] (void* p, int n, std::uint64_t cycle, std::uint16_t out) noexcept {
        auto& c = *static_cast<Ctx*> (p);
        if (c.chip->rs.osc[idx (n)].mode == rmNoise)
            c.ev->addHeld (n, static_cast<double> (cycle - c.chip->rs.start), out);
    };

    if (stepEach)
    {
        RenderSinkT<true> sink;
        sink.self = &ctx;
        sink.onSync = onSync;
        sink.onNoise = onNoise;
        std::array<std::uint16_t, 3> last {};
        // the top bit each per-cycle source shows to its ring target: the accumulator after the step, or, for
        // oscillator 1 reading oscillator 3, before oscillator 3's pull-down of that step
        std::array<bool, 3> ringFeed {}, view {};
        for (int n = 0; n < 3; ++n)
        {
            last[idx (n)] = rs.osc[idx (n)].held;
            ringFeed[idx (n)] = rs.osc[idx (n)].mode == rmCycle && rs.osc[idx (detail::syncTarget (n))].ring;
            view[idx (n)] = rs.osc[idx (n)].shownTop; // carried across pieces: a glitch can end in the next one
        }
        for (std::uint64_t i = 0; i < k; ++i)
        {
            sink.pendingCount = 0;
            sink.pulled = {};
            stepCycle (sink);
            const double eRel = static_cast<double> (cycleCount - rs.start);
            for (int n = 0; n < 3; ++n)
            {
                const auto ni = idx (n);
                if (! ringFeed[ni])
                    continue;
                const bool after = (osc[ni].phase & kPhaseMsb) != 0;
                const bool seen = after || (detail::syncTarget (n) == 0 && sink.pulled[ni]);
                if (seen != view[ni])
                {
                    view[ni] = seen;
                    ev.addFlip (n, eRel, seen);
                }
            }
            for (int n = 0; n < 3; ++n)
                if (ringFeed[idx (n)])
                    rs.osc[idx (n)].shownTop = view[idx (n)];
            for (int i2 = 0; i2 < sink.pendingCount; ++i2)
                onSync (&ctx, sink.pending[static_cast<std::size_t> (i2)]);
            for (int n = 0; n < 3; ++n)
            {
                const auto ni = idx (n);
                if (rs.osc[ni].mode == rmCycle && osc[ni].out != last[ni])
                {
                    last[ni] = osc[ni].out;
                    ev.addHeld (n, eRel, last[ni]);
                }
            }
        }
    }
    else
    {
        RenderSinkT<false> sink;
        sink.self = &ctx;
        sink.onSync = onSync;
        sink.onNoise = onNoise;
        advanceCycles (k, sink);
    }

    // a deselected output fades (8.15.2 (c)); at most one step per piece (the intervals are >= 1 400 cycles)
    for (int n = 0; n < 3; ++n)
    {
        const auto ni = idx (n);
        if (rs.osc[ni].mode == rmOff && fadeAt[ni] != 0 && fadeAt[ni] <= k && osc[ni].out != outBefore[ni])
            ev.addHeld (n, c0 + static_cast<double> (fadeAt[ni]), osc[ni].out);
    }

    // the last piece of a sample that is finished right away sweeps on to the sample's end (no digital
    // events lie between its last cycle and the end)
    const double tb = toSampleEnd ? rs.tEnd : static_cast<double> (cycleCount - rs.start);

    renderLines (ev, tb);
    for (int n = 0; n < 3; ++n)
        renderSweep (n, tb, &ev);
    rs.sweepT = tb;
}

// ---- the end of a sample ----

float Chip::renderFinishSample() noexcept
{
    if (rs.sweepT < rs.tEnd)
    {
        PieceEvents ev; // no digital events after the sample's last whole cycle; ring flips still occur
        renderLines (ev, rs.tEnd);
        for (int n = 0; n < 3; ++n)
            renderSweep (n, rs.tEnd, &ev);
    }
    rs.sweepT = rs.tEnd;

    const auto slot = static_cast<std::size_t> (rs.sample % static_cast<std::uint64_t> (kRing));
    for (int n = 0; n < 3; ++n)
    {
        RenderOsc& v = rs.osc[idx (n)];
        const double slope = renderSlopeU (n);
        const double rate = slope * rs.cps;
        // the converter terms follow the frequency, at sample boundaries
        const int imin = v.mode == rmTable ? 0 : (! rtab->dac.ideal && (v.mode == rmTri || v.mode == rmSaw)) ? iminWithHysteresis (rate, v.imin) : 11;
        if (imin != v.imin)
        {
            ++rs.converterSwitches;
            const double wl = renderValue (n, v.u);
            v.imin = imin;
            v.k = static_cast<std::int64_t> (std::floor (v.u / sigmaOf (v.imin)));
            renderEmit (n, rs.tEnd, renderValue (n, v.u) - wl, 0.0);
        }
        v.buf[slot] += renderValue (n, v.u);
        v.env[slot] = rtab->envelope[env[idx (n)].counter];

        // Re-anchor the line on the digital phase (no drift). They agree up to rounding unless a predicted
        // sync reset is still unconfirmed (then wait, at most three cycles) or a prediction was wrong (then
        // the line moves, band-limited). A per-cycle oscillator renders the chip's own output and its ring and
        // sync come from the chip too: its line only follows the accumulator (on Model A pulled down by the
        // noise-dependent rule, which the line does not predict), silently.
        if (v.mode == rmCycle)
        {
            v.pending = false;
            v.u = frac01 (renderDigitalU (n, rs.tEnd) + v.offU);
            continue;
        }
        if (v.pending)
        {
            if (static_cast<double> (cycleCount - v.pendingStart) - v.pendingT < 3.0)
                continue;
            v.pending = false;
        }
        double ua = renderDigitalU (n, rs.tEnd) + v.offU;
        ua += std::round (v.u - ua);
        if (std::abs (ua - v.u) > 1.0e-9)
        {
            ++rs.corrections;
            if (std::abs (ua - v.u) > 1.0e-4)
                ++rs.largeCorrections; // a prediction error, not the rounding of a sub-cycle sync time
            const double wl = renderValue (n, v.u);
            const double sl = renderDerivative (n, v.u);
            v.u = frac01 (ua);
            v.k = static_cast<std::int64_t> (std::floor (v.u / sigmaOf (v.imin)));
            renderEmit (n, rs.tEnd, renderValue (n, v.u) - wl, (renderDerivative (n, v.u) - sl) * rate);
        }
        else
            v.u = ua;
        if (v.mode != rmTri && v.mode != rmSaw && v.mode != rmTable)
            v.u = frac01 (v.u);
    }

    const auto outSlot = static_cast<std::size_t> ((rs.sample + static_cast<std::uint64_t> (kRing - kLatency - 1)) % static_cast<std::uint64_t> (kRing));
    double y = 0.0;
    for (auto& v : rs.osc)
    {
        y += static_cast<double> (v.env[outSlot]) * v.buf[outSlot];
        v.buf[outSlot] = 0.0;
        v.env[outSlot] = 0.0f;
    }
    ++rs.sample;
    rs.inProgress = false;
    return static_cast<float> (y);
}

// ---- the public entry points ----

int Chip::renderUntil (float* out, int maxSamples, std::uint64_t untilCycle) noexcept
{
    if (! rs.active)
        renderActivate();
    int done = 0;
    while (done < maxSamples)
    {
        if (! rs.inProgress)
        {
            if (untilCycle <= cycleCount)
                break;
            renderStartSample();
        }
        const std::uint64_t end = rs.start + rs.cycles;
        const std::uint64_t target = std::min (end, untilCycle);
        const bool finishing = untilCycle > end;
        while (cycleCount < target)
        {
            const std::uint64_t k = std::min (target - cycleCount, kPieceCycles);
            renderPiece (k, finishing && cycleCount + k == end);
        }
        if (untilCycle <= end)
            break; // stop inside (or at the last cycle of) the sample: it stays in progress
        out[done++] += renderFinishSample();
    }
    return done;
}

void Chip::render (float* out, int numInternalSamples) noexcept
{
    renderUntil (out, numInternalSamples, std::numeric_limits<std::uint64_t>::max());
}
} // namespace vs::bside::chip
