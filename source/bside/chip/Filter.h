#pragma once

// The chip's filter (docs/architecture.md 8.4.4, 8.5, 8.19.1; docs/decisions.md "B-Side S3a"). JUCE-free,
// allocates nothing (not even in prepare), no locks, no exceptions; every loop is bounded.
//
// WHAT IT IS. A 2-pole state-variable filter run sample by sample at the internal rate (4.4), written in the
// zero-delay-feedback (trapezoidal) form: g = tan (pi fc / rate), damping k = 1 / Q. Its input is the level
// of the voices routed to it, in the units Chip::render produces (one voice at full envelope swings +-0.5),
// with the voices' own DC added by OutputStage::filterBusLevel. Its output is the sum of the outputs the mode
// bits select (register 0x18 bits 4..6), in the same units, with the reference's signs: low-pass and
// high-pass come out inverted against the direct path, band-pass not (measured, decisions row 6), so low +
// high is a notch. The filter is clock-independent (8.15.7): the cutoff in Hz never depends on the chip
// clock. Nothing in it reads a chip, so a Part can own one and lend it to several chips (8.5, Shared).
//
// MODEL B is close to linear in the register: fc = a cubic in FC (fitted, 220 Hz at 32 to 14.6 kHz at 2047),
// Q = 0.71 at resonance 0 and a fitted 16-step law up to 2.6, and three linear outputs.
// MODEL A (all fitted to generated reference data; decisions "B-Side S3a"):
// - the cutoff register goes through its own converter, an 11-bit terminated resistor ladder of ratio 2.18
//   (RenderTables.h ladderWeights; it makes the steps at the bit boundaries, e.g. 1023 -> 1024 falls by 8 %),
//   and the converter value d (0..1) through a fitted curve (ln fc piecewise linear over 24 points of d, about
//   260 Hz at 0 to 21 kHz at 2047), fitted so that the -3 dB point the shared estimator (AnalogMeasure.h)
//   finds lands on the reference's; `variation` shifts d by +-kVariationShift (0.5: the reference's curve);
// - Q at resonance 0 depends on the cutoff (0.79 at 250 Hz, 0.55 around 1 to 2 kHz, 0.77 at 21 kHz); resonance
//   multiplies it by a fitted 16-step law (2.9 x at 15) and lowers the cutoff (to 0.87 x at 15);
// - the outputs are not the ideal ones: the low-pass output is a static quintic of the low-pass state (the
//   Model A distortion: compressive, saturating near three voices at full level; its gain grows with resonance
//   to 1.2 x at 15); the band-pass output is k0 x bp (k0 = 1 / Q at resonance 0) plus a static cubic of the
//   low-pass state (a leak); the high-pass output carries some of the band-pass state and a little of the
//   low-pass state.
// MODEL A, SIGNAL-DEPENDENT (processSignalDependent instead of process; the caller chooses; all fitted to
// generated reference data; decisions "B-Side S3a round 3"). The reference's Model A moves its corner with the level
// and the DC of what is routed into it. In this form the two integrators do not share one cutoff: each has its own
// rate, and the rates follow the signal, sample by sample. It follows the reference's level, DC and resonance
// behaviour far better than the fixed form above, but its corner at rest is not within 5 % at every cutoff, so the
// fixed form stays what process () does (the gate figures of both: decisions "B-Side S3a round 3"). Both forms share
// the two states, so a stream is run through one of them.
// - The loop: hp = rho (x - lp) - k bp, bp' = 2 pi r1 hp, lp' = 2 pi r2 bp; k falls with the resonance (16 steps)
//   and the loop gain rho changes with it too: above the control's threshold region the corner goes down as the
//   resonance goes up, in the threshold region much less (a fitted law in the resonance and the control).
// - A rate is a floor (about 260 Hz: what is left when the cutoff control is below its threshold) plus the rate of a
//   voltage-controlled resistance: with the overdrive t at the integrator's own side and t - e at its input side
//   (e = the input node's level times a fitted factor), rate = (soft (t)^2 - soft (t - e)^2) / (2 e), where soft is
//   a smooth max (0, .). Small e: the rate is t (linear in the control); a large swing at the input node of a
//   resistance near its threshold opens it on one half-wave only, so the rate RISES with the swing (the band-pass
//   integrator at low cutoffs); t of the low-pass integrator FALLS with the low-pass node's level (the DC of the
//   voices routed into the filter lowers the corner, down to the floor), faster above a knee.
// - The control: the cutoff register goes through its own converter, an 11-bit terminated resistor ladder of ratio
//   2.18 (RenderTables.h ladderWeights; it makes the steps at the bit boundaries, e.g. 1023 -> 1024 falls by 8 %),
//   and the converter value d (0..1) through a fitted table (piecewise linear, one point per fitted cutoff) to the control in Hz;
//   `variation` shifts d by +-kVariationShift (0.5: the reference's curve).
// - Each sample: the nodes are predicted with the previous sample's rates, the two rates are computed from the
//   predicted nodes, and the trapezoidal loop is solved with them (tan pre-warped per integrator). The rates lie
//   between the floor and a limit whatever the signal (a quarter of the internal rate, or the integrator's own
//   rate at rest where that is higher, never above 0.45 x the internal rate), and the states are limited to +-64
//   (never reached by a signal the chip can produce), so the output is bounded for every input.
// - The outputs: as the fixed form, with plain gains on the band-pass state (it peaks at rho / k at every cutoff).
// INPUT GUARD. A non-finite or absurd input is replaced by 0 before it reaches the states (off the per-sample
// dependency chain), so the states can never become non-finite.
// THREAD AND COST. setCutoff / setResonance / setMode recompute the coefficients (one tan, a few table reads);
// process() costs about a dozen multiplications (Model B) or two dozen (Model A) per sample; Model A's
// signal-dependent form adds its two rates (four square roots, two divisions in one dependency chain) and costs
// several times as much (chip_bench filterAdyn).

#include "bside/chip/ChipTypes.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace vs::bside::chip
{
class Filter
{
public:
    /** The mode bits of register 0x18. */
    static constexpr std::uint8_t kLowPass = 0x10, kBandPass = 0x20, kHighPass = 0x40;
    static constexpr std::uint8_t kModeMask = 0x70;
    /** Model A: how far `variation` 0 or 1 moves the cutoff converter's value d (0..1) from the reference's
        curve (variation 0.5). A judgement call (decisions "B-Side S3a"): about 120 register units. */
    static constexpr double kVariationShift = 0.06;

    Filter() noexcept;

    /** Model, internal rate (Hz, > 0) and `variation` (0..1, Model A only). Keeps the state. */
    void prepare (Model model, double internalRate, float variation = 0.5f) noexcept;
    /** State to zero (the reference's power-on: every analog state at zero). */
    void reset() noexcept;

    /** Cutoff as a continuous register value 0 .. 2047 (Smooth; Authentic passes integers). Clamped; a
        non-finite value is taken as 0. */
    void setCutoff (float registerValue) noexcept;
    /** Resonance 0 .. 15 (register 0x17 high nibble). Clamped. */
    void setResonance (int value) noexcept;
    /** The mode bits of register 0x18 (bits 4..6; the other bits are ignored). */
    void setMode (std::uint8_t modeVolume) noexcept;
    /** Model A cutoff curve offset 0..1 (8.4.7 Config::variation). Clamped. */
    void setVariation (float value) noexcept;

    /** One internal sample: the selected outputs' sum for this input. */
    float process (float in) noexcept
    {
        // the trapezoidal state-variable filter written as its state-space form (identical arithmetic in exact
        // numbers, a shorter dependency chain per sample): hp = h (x - (k + g) s1 - s2), bp = g hp + s1,
        // lp = g bp + s2, s1' = 2 bp - s1, s2' = 2 lp - s2, every output a linear form in x, s1, s2
        const double x = guard (in);
        double out = ox * x + o1 * s1 + o2 * s2;
        if (mdl == Model::A)
            out += shape (lx * x + l1 * s1 + l2 * s2);
        const double n1 = a1x * x + a11 * s1 + a12 * s2;
        s2 = a2x * x + a21 * s1 + a22 * s2;
        s1 = n1;
        settle();
        return static_cast<float> (out);
    }

    /** One internal sample with a cutoff (register value) that may change every sample, without setCutoff's cost:
        the cutoff curve and a rational tan in line (Model B: identical to setCutoff + process within the tan's
        error; Model A also evaluates its converter and curve). The stored cutoff and Q are not changed. */
    float processWithCutoff (float in, float registerValue) noexcept;

    /** tan (x) for 0 <= x <= 1.42 (cutoffs up to 0.45 x the rate), rational ([5/4] Pade form): relative error
        below 1e-8 up to 0.75 (0.24 x the rate) and below 3e-5 up to 1.42. */
    static double fastTan (double x) noexcept
    {
        const double z = x * x;
        return x * (945.0 - z * (105.0 - z)) / (945.0 - z * (420.0 - 15.0 * z));
    }

    /** In place over n samples (identical to n calls of process). */
    void process (float* inOut, int n) noexcept;

    /** Model A's signal-dependent dynamics (header, "MODEL A, SIGNAL-DEPENDENT"), one internal sample: the two
        integrators' rates follow the node levels. Same input, output, registers and states as process; Model B: process. */
    float processSignalDependent (float in) noexcept;
    /** The same with a cutoff (register value) that may change every sample (Model B: processWithCutoff). */
    float processSignalDependentWithCutoff (float in, float registerValue) noexcept;
    /** In place over n samples (identical to n calls of processSignalDependent). */
    void processSignalDependent (float* inOut, int n) noexcept;

    /** The selected outputs for a constant input `dc` once settled (low-pass state = dc, band- and
        high-pass 0): what the filter makes of the voices' DC (OutputStage, volumeDc off). */
    double staticOutput (double dc) const noexcept { return mdl == Model::A ? shape (dc) : cLp * dc; }

    Model model() const noexcept { return mdl; }
    double cutoffHz() const noexcept { return fcHz; }
    double q() const noexcept { return 1.0 / k; }
    std::uint8_t mode() const noexcept { return modeBits; }
    float cutoff() const noexcept { return fcReg; }
    int resonance() const noexcept { return res; }

    /** The Model A cutoff converter (11-bit ladder) value of a register value, 0..1 (tests). */
    static double modelACutoffConverter (double registerValue) noexcept;
    /** Cutoff in Hz at resonance 0 for a model, register value and variation (tests, diagnostics). */
    static double cutoffCurveHz (Model model, double registerValue, double variation) noexcept;

private:
    /** A non-finite or absurd input (|x| >= 1e6) is taken as 0 before it reaches the states (off the per-sample
        dependency chain); the states can then never become non-finite. */
    static double guard (float in) noexcept
    {
        const double x = static_cast<double> (in);
        return std::abs (x) < 1e6 ? x : 0.0; // NaN fails the comparison too
    }

    /** States below 1e-15 together go to 0 (-300 dB; flushing one alone would freeze the other, which decays only
        through it): no subnormal state, and zero input ends at exactly 0. */
    void settle() noexcept
    {
        if (std::abs (s1) + std::abs (s2) < 1e-15)
        {
            s1 = 0.0;
            s2 = 0.0;
        }
    }

    /** Model A: the static function of the (limited) low-pass state that the selected outputs carry. */
    double shape (double lp) const noexcept
    {
        const double v = lp < lpMin ? lpMin : (lp > lpMax ? lpMax : lp);
        return ((((p5 * v + p4) * v + p3) * v + p2) * v + p1) * v;
    }

    /** Signal-dependent Model A: one sample with the two integrators' controls and largest rates (Hz) and the loop
        gain; Filter.cpp. */
    double stepA (double x, double control1, double control2, double limit1, double limit2, double loop) noexcept;
    /** Signal-dependent Model A: the predictor's memory (the previous sample's rates) as at rest. */
    void restDynamic() noexcept;
    void updateCoefficients() noexcept;
    void updateOutputs() noexcept;
    void updateStateSpace() noexcept;

    Model mdl = Model::B;
    double rate = 96000.0;
    float variation = 0.5f;
    float fcReg = 0.0f;
    int res = 0;
    std::uint8_t modeBits = 0;

    // state and coefficients
    double s1 = 0.0, s2 = 0.0;
    double g = 0.0, k = 1.0, kg = 1.0, h = 1.0, fcHz = 0.0, k0 = 1.0;
    // signal-dependent Model A: the integrators' controls and largest rates (Hz), the loop gain and damping, pi / rate,
    // 0.45 x rate, its gains on the band- and high-pass states, and the predictor's memory (the previous sample's two
    // tan values and the reciprocal of its solve)
    double cBpDyn = 0.0, cHpDyn = 0.0;
    double ctl1 = 0.0, ctl2 = 0.0, cap1 = 0.0, cap2 = 0.0, rho = 1.0, damp = 1.0, piOverRate = 0.0, rateMax = 0.0;
    double gPrev1 = 0.0, gPrev2 = 0.0, dPrev = 1.0;
    // output mix: Model B linear; Model A linear parts plus a quintic of the (limited) low-pass state
    double cLp = 0.0, cBp = 0.0, cHp = 0.0, p1 = 0.0, p2 = 0.0, p3 = 0.0, p4 = 0.0, p5 = 0.0;
    double lpMin = -1.0e9, lpMax = 1.0e9;
    // the state-space form (updateStateSpace): next states, the linear output and the low-pass output
    double a1x = 0.0, a11 = 1.0, a12 = 0.0, a2x = 0.0, a21 = 0.0, a22 = 1.0;
    double ox = 0.0, o1 = 0.0, o2 = 0.0, lx = 0.0, l1 = 0.0, l2 = 1.0;
};
} // namespace vs::bside::chip
