#pragma once

// Process-wide data of the band-limited waveform renderer (docs/architecture.md 8.4.5; Chip::render,
// ChipRender.cpp): the converter (DAC) model of each chip model and the step tables of the combined
// waveforms. Built once per (model, shape set) on first use, from prepare() or the Chip
// constructor, never on the audio thread. JUCE-free.
//
// THE CONVERTER MODEL. A resistor ladder: every bit drives its node through a leg of `ratio` x R, the nodes
// are chained by series resistors R from the least to the most significant bit, the output is the most
// significant node; a terminated ladder has one more leg of `ratio` x R from the least significant node to
// ground. By superposition the output is linear in the bits: d (v) = sum of w_i over the set bits of v,
// with weights w_i solved from the network (ladderWeights) and normalised so that d (all ones) = 1. A ratio
// of 2 with termination gives exact binary weights (the ideal converter). Model B uses the ideal converter
// for the 12-bit waveform and the 8-bit envelope ("near-ideal", 8.4.4); Model A an unterminated ladder with
// the ratio kModelARatio (8.4.4: "an unterminated ladder with a ratio of about 2.2"; docs/decisions.md).
// Our own model; no constant is taken from the reference emulator.
//
// For the renderer a converter is written as d (v) = g v + sum e_i bit_i (v) with g = 1 / 4095 and
// e_i = w_i - g 2^i: the linear part is rendered in closed form (BLEP/BLAMP), and every e_i term is a square
// wave whose edges the renderer places exactly (ChipRender.cpp). The ideal converter has e_i = 0.
//
// THE COMBINED WAVEFORMS (waveform bits 3, 5, 6, 7) are piecewise constant over the 4096 index cells, and
// our fitted shapes change value at only 51 to 664 of them per period. The renderer therefore renders them
// as exact band-limited staircases: every change of d (shape (index)) is a band-limited step (BLEP) at the
// exact time the line reaches that cell, which also makes the pulse gate, the ring fold, sync and the
// Model A pull-down exact (docs/decisions.md, S2b-2). For each combination: the converter outputs of the
// 4096 cells and, per cell, the distance to the next cell with another value.

#include "bside/chip/ChipTypes.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace vs::bside::chip::render
{
/** The Model A ladder ratio (8.4.4 "about 2.2"; docs/decisions.md, S2b-2). */
constexpr double kModelARatio = 2.2;

constexpr int kTableCombinations = 4; // waveform bits 3, 5, 6, 7

/** The stronger BLEP kernel (docs/decisions.md S2b-2): every step of a pulse or a combined waveform (both
    models) uses it, because the shared kernel (8 zero crossings, Kaiser beta 8) attenuates what folds into
    the band by only about 85 dB, too little for narrow pulses and spiky combined shapes. Every other step
    keeps the shared kernel (both have 8 zero crossings: one latency, and those steps are unchanged). Chosen
    as the smallest that keeps every row of both alias measurements >= 3 dB inside its budget: 8 zero
    crossings, Kaiser beta 12 (5.3 dB to spare at the narrowest pulses, 88.2 kHz, 3.85 kHz), in double
    precision, read with a second-order correction of the table interpolation (ChipRender.cpp). */
constexpr int kLongZeroCrossings = 8;
constexpr int kLongBetaTenths = 120;

/** The table slot of a combination of triangle / saw / pulse bits (3, 5, 6, 7), -1 for any other. */
constexpr int tableSlot (std::uint8_t tspBits) noexcept
{
    switch (tspBits)
    {
        case 3: return 0;
        case 5: return 1;
        case 6: return 2;
        case 7: return 3;
        default: return -1;
    }
}

/** Weights of an n-bit resistor ladder (see above), normalised to a sum of 1; entries from n on are 0.
    n <= 16. Allocates nothing. */
using LadderWeights = std::array<double, 16>;
LadderWeights ladderWeights (int bits, double ratio, bool terminated) noexcept;

struct Converter
{
    bool ideal = true;                      // e_i are all 0
    double g = 1.0 / 4095.0;                // linear part per 12-bit step
    std::array<double, 12> e {};            // per-bit deviation from g 2^i
    /** Sum of e_i / 2 over the index bits below imin (the mean of squares the renderer leaves out):
        `saw[imin]` for the saw (bits 0..imin-1), `tri[imin]` for the triangle (its bit j+1 follows index
        bit j, so index bits 0..imin-1 carry e_1..e_imin; triangle bit 0 is always 0). */
    std::array<double, 13> excludedSaw {}, excludedTri {};
    double center = 0.5;                    // (d (0) + d (4095)) / 2: the renderer outputs d (v) - center
    /** The converter terms as tables (render cost step): devSaw[v] = sum of e_i over the set bits i of v
        (0..11), devTri[v] = sum of e_(i+1) over the set bits i of v (0..10; the triangle's bit i+1 follows
        index bit i), triTail[imin] = sum of e_(i+1) for i = imin..10. The renderer's index bits below imin
        are 0, so devSaw and devTri give the sums over the bits from imin on in one read. */
    std::array<double, 4096> devSaw {};
    std::array<double, 2048> devTri {};
    std::array<double, 13> triTail {};
};

/** Above the register's highest frequency (Smooth only, up to 12 kHz) a combined waveform's cell steps inside
    one stretch of its line (between two other events, at most a sample) are emitted together as one step with
    their time moments (render cost review, docs/decisions.md "B-Side render cost step"): the band-limited
    sum of the steps a_i at t_i is sum over k of (-1)^k M_k / k! times the k-th derivative of the band-limited
    step at the stretch's centre T0, M_k = sum a_i (t_i - T0)^k, up to k = kStepMoments. Per cell the prefix
    sums of the step into it times its position (StepTable::momentPrefix) give the moments of any range of
    cells in a few operations. At and below the register's top every step is emitted on its own. */
constexpr int kStepMoments = 4;

/** One combined waveform: the converter output of every index cell (minus the centre) and, per cell c,
    the number of cells from c to the next cell whose value differs from c's (1 .. 4095; 0 for a constant
    shape). */
struct StepTable
{
    /** momentPrefix[j][c] = sum over the cells c' < c of a_c' (c' / 4096)^j, a_c' = value[c'] - value[c' - 1]
        (the step into cell c', cyclic), j = 0 .. kStepMoments (see kStepMoments). */
    std::array<std::array<double, 4097>, kStepMoments + 1> momentPrefix {};
    std::array<float, 4096> value {};
    std::array<std::uint16_t, 4096> shape {}; // the 12-bit shape itself (bit 11: the Model A pull-down)
    /** For a cell c of the upper half: the first cell >= c whose shape has bit 11 clear (4096: none); for a
        cell of the lower half, the value of 0x800. Where a Model A saw combination is pulled down. */
    std::array<std::uint16_t, 4096> firstClear {};
    /** The first cell of the upper half whose shape has bit 11 set (4096: none): a Model A saw combination
        landing below it is pulled down at once whatever its pulse gate (render cost step, renderBatch). */
    std::uint16_t firstSetUpper = 4096;
    std::array<std::uint16_t, 4096> nextChange {};
};

struct RenderTables
{
    Converter dac;
    std::array<float, 4096> wave {};  // d (v) - center, v = 0 .. 4095
    std::array<float, 256> envelope {}; // the 8-bit envelope converter, d (255) = 1, d (0) = 0
    std::array<StepTable, kTableCombinations> steps {};
};

/** Model A's voice bend (docs/decisions.md "B-Side S3a" round 2 R3; folded into the renderer by S3b-2, docs/architecture.md
    8.19.4): each voice's waveform after its converter, w (render units, minus the converter's centre), becomes
    f (w) = w + kBendA2 w^2 + kBendA3 w^3 before its envelope multiplies it (the bend's relative strength does not depend
    on the envelope level, measured). Fitted with the mixer and the filter's static functions (OutputStage.cpp). Model B
    has no bend. f is increasing over every value a converter gives (|w| <= 0.5: f' >= 0.75). */
constexpr double kBendA2 = -0.175104, kBendA3 = -0.097759;
constexpr double bend (double w) noexcept { return w + w * w * (kBendA2 + w * kBendA3); }
/** f' (w). */
constexpr double bendSlope (double w) noexcept { return 1.0 + w * (2.0 * kBendA2 + 3.0 * kBendA3 * w); }
/** The bend of one voice's contribution u = e w at envelope value e: e f (u / e) on Model A (u itself below e = 1e-4),
    u on Model B. For harnesses that bend recorded voices sample by sample (the S3a test-side chain); the renderer folds
    the bend into its steps instead (Chip::renderBendVoices). */
inline double shapeVoice (Model m, double u, double e) noexcept
{
    if (m == Model::B || ! (e > 1e-4))
        return u;
    return e * bend (u / e);
}

/** The tables of a chip model with a shape set (built once per process on first use, thread-safe). `bent`: Model A's
    tables with every converter value w replaced by bend (w) (wave[] and the combined waveforms' values and moments; the
    converter model itself is unchanged), the renderer's tables for a chip that bends its voices (Chip::renderBendVoices);
    Model B's are its plain tables. */
const RenderTables& tablesFor (Model m, ShapeSet s, bool bent = false);

/** Both BLEP kernels laid out for the renderer (render cost step, docs/decisions.md "B-Side render cost
    step"): per table point k (0 .. kKernelResolution, kKernelResolution + 1 points over one sample), the
    values of all kKernelTaps cells j = -8 .. 7 side by side, so one discontinuity reads two contiguous rows
    and every tap shares one interpolation weight (the taps' sub-sample offsets are equal).
    The short kernel (`common/dsp/BlepTable.h` 8 / 8.0, float) keeps its own points and its linear
    interpolation. The long kernel (8 zero crossings, Kaiser beta 12, double) is read by cubic Hermite
    interpolation between its own points with the exact derivatives (step' = the kernel, ramp' = step; the
    second-order correction of S2b-2 without its three kernel reads per tap), and for the cell steps of a
    combined waveform from its step refined that way to kFineResolution points and read linearly (two rows
    instead of four; an error of about 7e-8 of a step, which the nearly cancelling edge pairs of the narrowest
    pulses would show, so pulse edges keep the Hermite read). Built from the BLEP tables' own points
    (BasicBlepTable::step, ramp at whole points, kernelAt at multiples of 1 / 4096), on first use. */
constexpr int kKernelTaps = 2 * kLongZeroCrossings;
constexpr int kKernelResolution = 512;
/** The long kernel's step for the cell steps of a combined waveform, read linearly: its own 512 points refined
    by their cubic Hermite interpolation (error about 1e-12 of a step) to kFineResolution points per sample. */
constexpr int kFineResolution = 2048;
struct KernelTables
{
    static constexpr std::size_t kPoints = static_cast<std::size_t> (kKernelResolution + 1) * kKernelTaps;
    static constexpr std::size_t kFinePoints = static_cast<std::size_t> (kFineResolution + 1) * kKernelTaps;
    std::array<float, kPoints> shortStep {}, shortRamp {};
    std::array<double, kPoints> longStep {}, longRamp {}, longKernel {};
    /** The long kernel's first, second and third derivatives at the same points (the step moments' terms,
        kStepMoments), read linearly. */
    std::array<double, kPoints> longKernelD1 {}, longKernelD2 {}, longKernelD3 {};
    std::array<double, kFinePoints> longStepFine {};
};
const KernelTables& kernelTables();

/** The output's conversion to float with its rounding error noise-shaped (alias follow-up step, defect 1 of
    8.18.1; docs/decisions.md "B-Side alias follow-up step" rows 1 to 4). A plain rounding errs by up to half a
    float step of the output's value: 2^-26 where a narrow pulse sits half a converter range below zero. For a
    signal periodic in q samples those errors repeat every q samples and gather on the lines at multiples of
    rate / q: up to -97 dB re a width-4095 pulse's fundamental, where the rendering itself (in double) aliases at
    -115 dB. The error is fed back through a fixed FIR, so the error reaching the output is shaped by
    NTF (z) = (1 - 1.75 z^-1 + z^-2) (1 - 0.5 z^-1 + z^-2) = 1 - 2.25 z^-1 + 2.875 z^-2 - 2.25 z^-3 + z^-4:
    zeros at 0.505 and 1.318 rad, at most -8.2 dB from 0 to 20 kHz at every internal rate of 4.4 (the band is
    widest at 88.2 kHz: 0 .. 1.425 rad), up to +19.4 dB above it (removed by the decimator). An FIR in the error
    is stable for any input: the output stays within 4.75 float steps of the value (the sum of the coefficients'
    magnitudes, 9.5, times half a step). A constant input would make the loop cycle (idle tones of a few float
    steps where the output stood still and was exact before): once the value has stood still for kSettle (128)
    conversions it is rounded plainly and the error state is cleared, so a silent or held output is constant
    again and a later change starts the loop from zero. (With 32, the flat stretches between a 1.9 kHz pulse's
    edges at 96 kHz, about 33 samples, were rounded plainly and the exact measurement kept only 4 dB of margin;
    with 128 the exact scans give the figures of a loop that never settles.) The chip shapes its own output,
    which render adds to the caller's buffer; that addition's rounding is the caller's. Header-only so the tests
    can measure it directly (ChipRenderExactTests.cpp). */
class OutputRounding
{
public:
    static constexpr int kSettle = 128;

    float next (double y) noexcept
    {
        if (std::bit_cast<std::uint64_t> (y) == std::bit_cast<std::uint64_t> (last))
            same = same < kSettle ? same + 1 : kSettle;
        else
        {
            last = y;
            same = 0;
        }
        if (same >= kSettle)
        {
            e = {};
            return static_cast<float> (y);
        }
        const double v = y - (2.25 * e[0] - 2.875 * e[1] + 2.25 * e[2] - e[3]);
        const auto q = static_cast<float> (v);
        e[3] = e[2];
        e[2] = e[1];
        e[1] = e[0];
        e[0] = static_cast<double> (q) - v;
        return q;
    }

    /** Whether `y` would be rounded plainly with no state change (the value has stood still for kSettle). */
    bool settledOn (double y) const noexcept { return same >= kSettle && std::bit_cast<std::uint64_t> (y) == std::bit_cast<std::uint64_t> (last); }
    bool settled() const noexcept { return same >= kSettle; }
    /** Whether the error state is all zero (tests). */
    bool clear() const noexcept
    {
        for (const double x : e)
            if (std::fpclassify (x) != FP_ZERO)
                return false;
        return true;
    }

private:
    std::array<double, 4> e {}; // the last four rounding errors, newest first
    double last = 0.0;          // the last value converted (before the error feedback)
    int same = 0;               // conversions in a row with that value, saturating at kSettle
};

} // namespace vs::bside::chip::render
