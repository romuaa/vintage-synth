#pragma once

// The chip's filter (docs/architecture.md 8.4.4, 8.5, 8.19.1, 8.19.3; docs/decisions.md "B-Side S3a", "B-Side S3a
// closing step"). JUCE-free, allocates nothing (not even in prepare), no locks, no exceptions; every loop is bounded.
//
// WHAT IT IS. A 2-pole state-variable filter run sample by sample at the internal rate (4.4), written in the
// zero-delay-feedback (trapezoidal) form with parameters g and k. Its analog PROTOTYPE is the state-variable filter
// with corner fc and damping k = 1 / Q whose output is cL LP + cB BP + cH HP (LP = 1 / D, BP = s / D, HP = s^2 / D,
// D = s^2 + k s + 1, s in units of the corner). Its input is the level of the voices routed to it, in the units
// Chip::render produces (one voice at full envelope swings +-0.5), with the voices' own DC added by
// OutputStage::filterBusLevel. Its output is the sum of the outputs the mode bits select (register 0x18 bits 4..6),
// in the same units, with the reference's signs: low-pass and high-pass come out inverted against the direct path,
// band-pass not (measured, decisions row 6), so low + high is a notch. The filter is clock-independent (8.15.7): the
// cutoff in Hz never depends on the chip clock. Nothing in it reads a chip, so a Part can own one and lend it to
// several chips (8.5, Shared).
//
// RATE (8.19.3). The prototype is mapped to the discrete filter so that its response does not depend on the internal
// rate (within 0.5 dB, 20 Hz - 20 kHz, every rate of 4.4, every mode sum; tested), at no per-sample cost: the
// state-variable form keeps its arithmetic, only its parameters and output weights change.
// - POLES: matched (impulse-invariant): the prototype's poles p map to exp (p T). The trapezoidal form's (g, k) are
//   solved for them: with 1 + a1 z^-1 + a2 z^-2 the matched denominator, g^2 = (1 + a1 + a2) / (1 - a1 + a2) and
//   g k = 2 (1 - a2) / (1 - a1 + a2) (computed without cancellation, real poles included).
// - NUMERATOR: the three outputs' weights realise any second-order numerator. It is designed per mode combination
//   from the prototype's magnitude in closed form: its squared magnitude, a quadratic in sin^2 (w / 2), takes the
//   prototype's DC gain exactly (so the static output is unchanged) and its magnitude at two nodes, min (fc, n2 / 2)
//   and n2 = min (20 kHz, 0.4 x rate); it is then factored into a numerator with the prototype's signs at DC and at
//   half the rate.
// - PHASE: of the two factors with that magnitude (one is the other with every zero reflected through the unit
//   circle) the one is kept whose zeros lie on the side of the circle where the matched images exp (s T) of the
//   prototype numerator's zeros (cL + cB s + cH s^2) lie: no zero in the right half-plane -> zeros inside; two (a
//   pair or two real) -> outside; one (real) -> only that one outside (the factor whose zeros lie nearer the images).
//   The numerator is therefore not always minimum-phase. Tested ([rate]): the phase of the linear response is within
//   30 degrees of the prototype's over the magnitude gate's set and band, except seven settings at 88.2 and 96 kHz
//   gated at their own figure + 2 (at most 33.7 degrees; all at 20 kHz, the band's top, low cutoffs: band-pass and
//   low + band of both models; the magnitude design leaves no freedom there at no per-sample cost;
//   FilterOutputTests.cpp phaseAllowed lists them).
// - A true notch (Model B low + high: no band term, zeros on the axis) instead puts its zeros on the unit circle at
//   the prototype's notch frequency. A few transcendental functions per update.
// MODEL B is close to linear in the register: fc = a cubic in FC (fitted, 220 Hz at 32 to 14.6 kHz at 2047),
// Q = 0.71 at resonance 0 and a fitted 16-step law up to 2.6, and three linear outputs: the filter is linear.
// KNOWN DEVIATION (decisions "B-Side S3a closing step" C6): on noise at mid and full level the reference's high-pass
// stop band has a floor about 6 to 7 dB above this filter's (its cause is not located; a second-order term fitted on
// noise floors alone was tried and removed: it was wrong on tones, aliased and broke Shared). A later attempt must
// fit tones and noise together and meet the alias budget.
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
// - RATE: the mapping above applies to Model A's linear part (the prototype weights: the linear terms of the static
//   functions on LP, the band weights on BP, the high weight on HP). The static functions stay functions of the
//   low-pass state (now the low-pass of the mapped g, k); the linear output row carries the designed numerator minus
//   the static functions' linear term, so the linear part is the designed one and the outputs keep their meaning.
// processWithCutoff (a cutoff every sample) reads the mapped parameters from a table over the cutoff (Model B: the
// register, a knot every 64; Model A: log2 of the corner at resonance 0, four knots per octave, on the Q law's own
// knots), g' stored against tan (pi fc / rate), linear in between: its complex response within 0.1 dB and 1 degree of
// setCutoff + process (tested, FilterOutputTests.cpp). The table depends on the rate, the resonance and the mode
// only; it is rebuilt (27 or 33 designs, bounded) at the first processWithCutoff after one of them changed.
// INPUT GUARD. A non-finite input or one of magnitude 1e6 or more is replaced by 0 before it reaches the states (off
// the per-sample dependency chain), so the states can never become non-finite (tested at the threshold). A
// subnormal input is not flushed: it reaches the output through the direct term ox x (the states flush below 1e-15);
// callers run with denormals disabled (juce::ScopedNoDenormals on the audio thread).
// THREAD AND COST. setCutoff / setResonance / setMode / setVariation redo the mapping (an exp, two sines, a few
// square roots, and up to four logarithms and two exponentials for the zeros' side); process() costs about a dozen
// multiplications (Model B) or two dozen (Model A) per sample. processWithCutoff rebuilds its table at its first call
// after a resonance, mode or rate change (with the write's own update about 0.6 microseconds on Model B and 0.85 on
// Model A, measured at 96 kHz on the development machine): a resonance or mode write every sample therefore costs that
// every sample; write them only when they change.

#include "bside/chip/ChipTypes.h"

#include <array>
#include <cmath>
#include <complex>
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
    float process (float in) noexcept { return static_cast<float> (processDouble (static_cast<double> (in))); }
    /** The same in double precision, input and output (the chain, ChipVoice: 8.20.1). */
    double processDouble (double in) noexcept
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
        return out;
    }

    /** The same sample split for a chain that applies Model A's static functions at another rate (ChipVoice, S3b-2):
        returns the output without them (Model B: the whole output) and sets lowPass to the low-pass value they are
        functions of; processDouble (x) = processLinearDouble (x, lp) + outputShape (lp), the state advanced alike. */
    double processLinearDouble (double in, double& lowPass) noexcept
    {
        const double x = guard (in);
        const double out = ox * x + o1 * s1 + o2 * s2;
        lowPass = lx * x + l1 * s1 + l2 * s2;
        const double n1 = a1x * x + a11 * s1 + a12 * s2;
        s2 = a2x * x + a21 * s1 + a22 * s2;
        s1 = n1;
        settle();
        return out;
    }
    /** Model A's static functions of the low-pass value (0 on Model B). */
    double outputShape (double lowPass) const noexcept { return mdl == Model::A ? shape (lowPass) : 0.0; }
    /** Their coefficients: outputShape (lp) = ((((p5 v + p4) v + p3) v + p2) v + p1) v, v = lp limited to [lpMin, lpMax]
        (all 0 on Model B). The limits do not depend on the mode or the resonance. */
    struct Shape
    {
        double p1 = 0.0, p2 = 0.0, p3 = 0.0, p4 = 0.0, p5 = 0.0, lpMin = -1.0e9, lpMax = 1.0e9;
    };
    Shape outputShapeCoefficients() const noexcept
    {
        if (mdl != Model::A)
            return {};
        return { p1, p2, p3, p4, p5, lpMin, lpMax };
    }

    /** One internal sample with a cutoff (register value) that may change every sample, without setCutoff's cost:
        the cutoff curve, a rational tan and the mapped parameters interpolated from a table (see "processWithCutoff"
        above), with the damping, output weights and `variation` of the cutoff given (Model A's Q and band-pass gain
        follow its cutoff). The stored cutoff is not changed. */
    float processWithCutoff (float in, float registerValue) noexcept
    {
        return static_cast<float> (processWithCutoffDouble (static_cast<double> (in), registerValue));
    }
    /** The same in double precision, input and output. */
    double processWithCutoffDouble (double in, float registerValue) noexcept;
    /** processWithCutoffDouble split as processLinearDouble splits processDouble. */
    double processWithCutoffLinearDouble (double in, float registerValue, double& lowPass) noexcept;

    /** tan (x) for 0 <= x <= 1.42 (cutoffs up to 0.45 x the rate), rational ([5/4] Pade form): relative error
        below 1e-8 up to 0.75 (0.24 x the rate) and below 3e-5 up to 1.42. */
    static double fastTan (double x) noexcept
    {
        const double z = x * x;
        return x * (945.0 - z * (105.0 - z)) / (945.0 - z * (420.0 - 15.0 * z));
    }

    /** In place over n samples (identical to n calls of process). */
    void process (float* inOut, int n) noexcept;

    /** The selected outputs for a constant input `dc` once settled (low-pass state = dc, band- and
        high-pass 0): what the filter makes of the voices' DC (OutputStage, volumeDc off). */
    double staticOutput (double dc) const noexcept { return mdl == Model::A ? shape (dc) : protoL * dc; }

    Model model() const noexcept { return mdl; }
    /** The prototype's corner (Hz) and Q (the discrete filter's are mapped from them, see "RATE"). */
    double cutoffHz() const noexcept { return fcHz; }
    double q() const noexcept { return 1.0 / k; }
    std::uint8_t mode() const noexcept { return modeBits; }
    float cutoff() const noexcept { return fcReg; }
    int resonance() const noexcept { return res; }
    /** The prototype's weights on its low-, band- and high-pass responses (Model A: its linear part). */
    std::array<double, 3> prototypeWeights() const noexcept { return { protoL, protoB, protoH }; }

    /** The linear part of the filter exactly as the per-sample code runs it (tests): states s' = A s + a x, output
        o x + c s (Model A: its static functions counted with their linear term; Model B: the whole filter). */
    struct Linear
    {
        double a11, a12, a21, a22, a1x, a2x, ox, o1, o2;
    };
    Linear linearPart() const noexcept;
    /** The same for processWithCutoff at a register value (builds its table if needed). */
    Linear linearPartWithCutoff (float registerValue) noexcept;
    /** The frequency response of a linear part at `hz` for a rate. */
    static std::complex<double> response (const Linear& l, double hz, double rate) noexcept;
    /** The same at a point z of the unit circle (z = exp (j 2 pi hz / rate)). */
    static std::complex<double> responseAt (const Linear& l, std::complex<double> z) noexcept;
    /** The largest pole magnitude of a linear part (below 1: stable). */
    static double spectralRadius (const Linear& l) noexcept;

    /** The Model A cutoff converter (11-bit ladder) value of a register value, 0..1 (tests). */
    static double modelACutoffConverter (double registerValue) noexcept;
    /** Cutoff in Hz at resonance 0 for a model, register value and variation (tests, diagnostics). */
    static double cutoffCurveHz (Model model, double registerValue, double variation) noexcept;

private:
    /** The mapped trapezoidal parameters and the output weights on its low-, band- and high-pass. */
    struct Mapped
    {
        double g, k, wL, wB, wH;
    };
    static constexpr int kCutKnots = 33;

    /** A non-finite or absurd input (|x| >= 1e6) is taken as 0 before it reaches the states (off the per-sample
        dependency chain); the states can then never become non-finite. */
    static double guard (double x) noexcept
    {
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

    /** The matched mapping of a prototype (corner fc Hz, damping k, weights cL, cB, cH; `notch`: Model B low + high)
        at a rate, with the node n2 and sin^2 (pi n2 / rate) (see "RATE"). Low-, band- and high-pass weights. */
    static Mapped mapMatched (double fc, double k, double cL, double cB, double cH, bool notch, double rate, double n2,
                              double phi2) noexcept;
    /** Model A's prototype weights for the mode bits, a resonance and the damping at resonance 0. */
    static std::array<double, 3> modelAWeights (std::uint8_t mode, int resonance, double dampingAtZero) noexcept;
    void update() noexcept;
    void buildCutTable() noexcept;
    Mapped cutAt (float registerValue) const noexcept;
    static Linear stateSpace (double g, double k, double wL, double wB, double wH) noexcept;

    Model mdl = Model::B;
    double rate = 96000.0;
    float variation = 0.5f;
    float fcReg = 0.0f;
    int res = 0;
    std::uint8_t modeBits = 0;

    // state
    double s1 = 0.0, s2 = 0.0;
    // the prototype: corner, damping, Model A's damping at resonance 0, weights; the node n2 and sin^2 (pi n2 / rate)
    double fcHz = 0.0, k = 1.0, k0 = 1.0, protoL = 0.0, protoB = 0.0, protoH = 0.0;
    double node2 = 20000.0, node2Phi = 0.0;
    // Model A's static functions of the (limited) low-pass state
    double p1 = 0.0, p2 = 0.0, p3 = 0.0, p4 = 0.0, p5 = 0.0;
    double lpMin = -1.0e9, lpMax = 1.0e9;
    // the state-space form (update): next states, the linear output and the low-pass output
    double a1x = 0.0, a11 = 1.0, a12 = 0.0, a2x = 0.0, a21 = 0.0, a22 = 1.0;
    double ox = 0.0, o1 = 0.0, o2 = 0.0, lx = 0.0, l1 = 0.0, l2 = 1.0;
    // processWithCutoff: {g' / tan (pi fc / rate), k', wL, wB, wH} per knot (see "processWithCutoff")
    std::array<std::array<double, 5>, kCutKnots> cutTable {};
    bool cutValid = false;
};
} // namespace vs::bside::chip
