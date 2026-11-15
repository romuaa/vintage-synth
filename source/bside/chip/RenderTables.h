#pragma once

// Process-wide data of the band-limited waveform renderer (docs/architecture.md 8.4.5; Chip::render,
// ChipRender.cpp): the converter (DAC) model of each chip model and the mip-mapped band-limited tables of
// the combined waveforms. Built once per (model, shape set) on first use, from prepare() or the Chip
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
};

/** One combined waveform: the converter output of every index cell (minus the centre) and, per cell c,
    the number of cells from c to the next cell whose value differs from c's (1 .. 4095; 0 for a constant
    shape). */
struct StepTable
{
    std::array<float, 4096> value {};
    std::array<std::uint16_t, 4096> shape {}; // the 12-bit shape itself (bit 11: the Model A pull-down)
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

} // namespace vs::bside::chip::render
