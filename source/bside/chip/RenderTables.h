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

/** Coarse levels of a combined waveform's staircase, rendered above the register's highest frequency
    (render cost step, docs/decisions.md "B-Side render cost step"): level L (1 .. kCoarseLevels) has
    4096 >> L cells per period whose harmonics below half that count are the fine staircase's (RenderTables.cpp);
    at and below the register's highest frequency (3.85 kHz at 985 248 Hz) the staircase is always rendered
    cell by cell. */
constexpr int kCoarseLevels = 9;
/** The finest coarse level built and used: 128 cells per period, at most 2 per sample above the register's range
    at every internal rate up to 384 kHz (ChipRender.cpp clamps to it at higher rates). */
constexpr int kFirstCoarseLevel = 5;
/** Where level L (1 .. kCoarseLevels) starts in StepTable::coarseValue / coarseNext (4096 >> L entries). */
constexpr std::size_t coarseOffset (int level) noexcept { return 4096u - (8192u >> level); }

/** One combined waveform: the converter output of every index cell (minus the centre) and, per cell c,
    the number of cells from c to the next cell whose value differs from c's (1 .. 4095; 0 for a constant
    shape). */
struct StepTable
{
    /** The coarse levels (see kCoarseLevels): the mean value of each coarse cell, and the number of coarse
        cells to the next one with another value (0 for a constant level), packed level after level. */
    std::array<float, 4096> coarseValue {};
    std::array<std::uint16_t, 4096> coarseNext {};
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

/** The tables of a chip model with a shape set (built once per process on first use, thread-safe). */
const RenderTables& tablesFor (Model m, ShapeSet s);

/** Both BLEP kernels laid out for the renderer (render cost step, docs/decisions.md "B-Side render cost
    step"): per table point k (0 .. kKernelResolution, kKernelResolution + 1 points over one sample), the
    values of all kKernelTaps cells j = -8 .. 7 side by side, so one discontinuity reads two contiguous rows
    and every tap shares one interpolation weight (the taps' sub-sample offsets are equal).
    The short kernel (`common/dsp/BlepTable.h` 8 / 8.0, float) keeps its own points and its linear
    interpolation. The long kernel (8 zero crossings, Kaiser beta 12, double) is read by cubic Hermite
    interpolation between its points with the exact derivatives (step' = the kernel, ramp' = step): the
    second-order correction of S2b-2 without its three kernel reads per tap. Built from the BLEP tables'
    own points (BasicBlepTable::step, ramp at whole points, kernelAt at multiples of 1 / 4096), on first use. */
constexpr int kKernelTaps = 2 * kLongZeroCrossings;
constexpr int kKernelResolution = 512;
struct KernelTables
{
    static constexpr std::size_t kPoints = static_cast<std::size_t> (kKernelResolution + 1) * kKernelTaps;
    std::array<float, kPoints> shortStep {}, shortRamp {};
    std::array<double, kPoints> longStep {}, longRamp {}, longKernel {};
};
const KernelTables& kernelTables();

} // namespace vs::bside::chip::render
