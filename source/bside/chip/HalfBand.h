#pragma once

// Linear-phase half-band filters for the chain's static curves (docs/architecture.md 8.19.4, S3b-2; docs/decisions.md
// "B-Side S3b-2"). JUCE-free, allocates nothing (not even in prepare), no locks, no exceptions; every loop is bounded.
//
// WHY. The mixer curves and Model A's filter output function are static polynomials. Run on the full-band signal at the
// internal rate they fold (Model B mixer -67 dB, Model A -53 dB for a 3.85 kHz sawtooth at 88.2 kHz). A polynomial of
// degree k spreads a band B over k B; a signal limited to 20 kHz plus a transition band that ends where its products
// cannot fold back below 20 kHz keeps the band clean. So:
// - Model B: one HalfBandFir on the mixer's input (pass band to 20 kHz), the curve at the internal rate.
// - Model A: the same HalfBandFir on both buses before the filter; after the filter its output function, the mixer gain
//   and the mixer curve run at twice the internal rate between a HalfBandUp and a HalfBandDown (pass band to 20 kHz at
//   twice the rate).
//
// THE DESIGN (designHalfBand). A half-band filter at rate r: the centre tap 1/2, every other tap at an even distance
// from the centre 0, the taps at odd distances 1, 3, ..., 2 P - 1 (P pairs) a Kaiser-windowed sinc normalised so that
// the DC gain is 1. Its zero-phase response satisfies H (f) + H (r / 2 - f) = 1, so the pass band's deviation from 1 up
// to the pass edge equals the stop band's level beyond r / 2 - pass edge: a pass band within 0.01 dB (deviation
// 1.15e-3) means a stop band at least 58.8 dB down. The design takes the fewest pairs for which some Kaiser window
// meets the deviation: from a little below Kaiser's length estimate, for each count the window's parameter is
// searched (2 .. 12 in 0.05 steps, on a coarse grid of the pass band) and the count is taken if its best parameter
// meets the deviation on the fine grid (bounded: kMaxPairs counts). The stop band then starts at r / 2 - pass edge, which for a pass
// edge of 20 kHz is at least (r - 20 kHz) / 3 for every rate from 80 kHz up (8.19.4's stop edge: the larger of the two).
// - Delay: 2 P - 1 samples of its rate (an integer: the centre tap).
// - Sizes as built (ChipVoice::kAntiAliasDeviation 1.15e-3, pass edge 20 kHz): at the internal rates 88.2 / 96 / 128 /
//   176.4 / 192 / 352.8 / 384 kHz P = 19 / 11 / 5 / 4 / 4 / 2 / 2; Model A's 2x part (ChipVoice::kTwiceDeviation 1e-5,
//   about 100 dB, at twice those rates) 7 / 6 / 5 / 5 / 4 / 4 / 4 (printed by "alias: the half-band designs ..." in
//   ChipVoiceAliasTests.cpp).
// - Cost of the design: the parameter search runs in prepare (ChipVoice::prepare designs the pre-filter and, Model A,
//   the 2x part): 0.5 to 8 ms per voice (the second review, under load; the most at 88.2 kHz, Model A). Off the audio
//   thread, but S4 prepares many voices: S4 either designs once per rate and shares the design (HalfBandDesign is a
//   plain value) or tables the seven rates' designs.
//
// THE FILTERS. HalfBandFir: one rate. HalfBandUp: rate r -> 2 r, two outputs per input (polyphase: one phase is the
// input delayed, the other the pairs). HalfBandDown: 2 r -> r, one output per two inputs (only the outputs kept are
// computed). Zero input gives exactly zero output once the filter's length has passed (no state survives: the filters
// are FIR). A constant input gives exactly that constant once the filter's length has passed, so a held static DC and
// the curve's static operating point subtracted from it cancel exactly: HalfBandFir returns a held input as it is once
// its whole window holds it (and skips its taps: the per-sample form is the plain one), HalfBandUp and HalfBandDown take
// each pair as its difference from the centre sample, (x[c - k] + x[c + k]) - 2 x[c], weighted and added to x[c].

#include "bside/chip/ChipTypes.h"

#include <array>
#include <bit>
#include <cstdint>

namespace vs::bside::chip
{
struct HalfBandDesign
{
    static constexpr int kMaxPairs = 32;
    double rate = 96000.0;    // the rate the filter runs at
    double passHz = 20000.0;  // the pass edge
    double deviation = 1e-3;  // the largest |H (f) - 1| over 0 .. passHz the design was made for
    int pairs = 1;            // P: non-zero taps at the odd distances 1, 3, ..., 2 P - 1 on each side
    std::array<double, kMaxPairs> h {}; // the tap at distance 2 j + 1 (both sides); the centre tap is 1/2

    /** The delay in samples of the design's rate (the centre tap's distance from the newest input). */
    int delay() const noexcept { return 2 * pairs - 1; }
    /** The zero-phase response at hz (real: the filter is symmetric; add the delay's linear phase for the causal one). */
    double response (double hz) const noexcept;
    /** The largest |response - 1| over 0 .. passHz (a fine grid and the edge). */
    double passDeviation() const noexcept;
    /** The same on a grid of the given number of intervals. */
    double passDeviationOn (int intervals) const noexcept;
};

/** A half-band low-pass at `rate` with its pass band to passHz within `deviation` (the stop band from rate / 2 - passHz
    at most `deviation`); the pass edge is limited to 0.24 x rate (a transition of at least 0.02 x rate). Allocates
    nothing; a few hundred cosines and Bessel series per call. */
HalfBandDesign designHalfBand (double rate, double passHz, double deviation) noexcept;

/** The design at one rate, per sample. Output = input filtered, delay () samples late. */
class HalfBandFir
{
public:
    void prepare (const HalfBandDesign& d) noexcept;
    void reset() noexcept;
    /** Fills the history with x (the filter then outputs x as if x had always been its input). */
    void prime (double x) noexcept;
    double process (double x) noexcept { return processInline (x); }
    /** process, inlined into its caller whatever its size (ChipVoice's block path; ChipTypes.h). */
    BS_CHIP_INLINE double processInline (double x) noexcept
    {
        pos = (pos + 1) & (kRing - 1);
        buf[static_cast<std::size_t> (pos)] = x;
        buf[static_cast<std::size_t> (pos + kRing)] = x;
        // a constant input: once the whole window holds it, the output is that constant exactly and the taps are skipped
        if (std::bit_cast<std::uint64_t> (x) == std::bit_cast<std::uint64_t> (held))
        {
            if (same < 4 * pairs - 2)
                ++same;
            else
                return x;
        }
        else
        {
            held = x;
            same = 0;
        }
        const double* e = buf.data() + pos + kRing - (2 * pairs - 1); // the centre
        double a0 = 0.0, a1 = 0.0;
        int j = 0;
        for (; j + 1 < pairs; j += 2)
        {
            a0 += h[static_cast<std::size_t> (j)] * (e[2 * j + 1] + e[-(2 * j + 1)]);
            a1 += h[static_cast<std::size_t> (j + 1)] * (e[2 * j + 3] + e[-(2 * j + 3)]);
        }
        if (j < pairs)
            a0 += h[static_cast<std::size_t> (j)] * (e[2 * j + 1] + e[-(2 * j + 1)]);
        return 0.5 * e[0] + (a0 + a1);
    }
    int delay() const noexcept { return 2 * pairs - 1; }

private:
    static constexpr int kRing = 128; // > 4 x kMaxPairs - 2: the whole window, newest at pos
    std::array<double, 2 * kRing> buf {};
    std::array<double, HalfBandDesign::kMaxPairs> h {};
    int pairs = 1, pos = 0;
    double held = 0.0; // the last input and how many inputs before it were the same (bit for bit)
    int same = 0;
};

/** Rate r -> 2 r. For each input x[m], the outputs y[2 m] (interpolated) and y[2 m + 1] (an input, delayed); y[k] is the
    input at 2x time k - delay2x (). The pass band's gain is 1 (the zero-stuffed signal is scaled by 2). */
class HalfBandUp
{
public:
    void prepare (const HalfBandDesign& d) noexcept; // d at the doubled rate
    void reset() noexcept;
    void prime (double x) noexcept;
    void process (double x, double& first, double& second) noexcept { processInline (x, first, second); }
    BS_CHIP_INLINE void processInline (double x, double& first, double& second) noexcept
    {
        pos = (pos + 1) & (kRing - 1);
        buf[static_cast<std::size_t> (pos)] = x;
        buf[static_cast<std::size_t> (pos + kRing)] = x;
        // newest x[m] at n[0]; y[2 m] = sum_j 2 h_j (x[m - P + 1 + j] + x[m - P - j]), y[2 m + 1] = x[m - P + 1]
        const double* r = buf.data() + pos + kRing - (pairs - 1); // x[m - P + 1]
        const double c2 = 2.0 * r[0];
        double a0 = 0.0, a1 = 0.0;
        int j = 0;
        for (; j + 1 < pairs; j += 2)
        {
            a0 += h2[static_cast<std::size_t> (j)] * ((r[j] + r[-j - 1]) - c2);
            a1 += h2[static_cast<std::size_t> (j + 1)] * ((r[j + 1] + r[-j - 2]) - c2);
        }
        if (j < pairs)
            a0 += h2[static_cast<std::size_t> (j)] * ((r[j] + r[-j - 1]) - c2);
        first = r[0] + (a0 + a1);
        second = r[0];
    }
    /** The delay in samples of the doubled rate (odd). */
    int delay2x() const noexcept { return 2 * pairs - 1; }
    int pairCount() const noexcept { return pairs; }

private:
    static constexpr int kRing = 64; // >= 2 x kMaxPairs: the 2 P newest inputs, newest at pos
    std::array<double, 2 * kRing> buf {};
    std::array<double, HalfBandDesign::kMaxPairs> h2 {}; // 2 h_j
    int pairs = 1, pos = 0;
};

/** 2 r -> r. Takes y[2 m], y[2 m + 1] and returns the filtered signal at 2x time 2 m - delay2x () (the outputs at the
    odd 2x times are not computed: only every second output is kept). */
class HalfBandDown
{
public:
    void prepare (const HalfBandDesign& d) noexcept; // d at the doubled rate
    void reset() noexcept;
    void prime (double x) noexcept;
    double process (double first, double second) noexcept { return processInline (first, second); }
    BS_CHIP_INLINE double processInline (double first, double second) noexcept
    {
        // out = 1/2 second[m - P] + sum_j h_j (first[m - P + 1 + j] + first[m - P - j]); second[m] joins after (it is
        // the centre of a later output)
        pos = (pos + 1) & (kRing - 1);
        fb[static_cast<std::size_t> (pos)] = first;
        fb[static_cast<std::size_t> (pos + kRing)] = first;
        const double* r = fb.data() + pos + kRing - (pairs - 1); // first[m - P + 1]
        // second[m - P]: the seconds are kept in their own ring, newest second[m - 1] before this call's
        const double centre = sb[static_cast<std::size_t> ((spos + kRing - (pairs - 1)) & (kRing - 1))];
        spos = (spos + 1) & (kRing - 1);
        sb[static_cast<std::size_t> (spos)] = second;
        const double c2 = 2.0 * centre;
        double a0 = 0.0, a1 = 0.0;
        int j = 0;
        for (; j + 1 < pairs; j += 2)
        {
            a0 += h[static_cast<std::size_t> (j)] * ((r[j] + r[-j - 1]) - c2);
            a1 += h[static_cast<std::size_t> (j + 1)] * ((r[j + 1] + r[-j - 2]) - c2);
        }
        if (j < pairs)
            a0 += h[static_cast<std::size_t> (j)] * ((r[j] + r[-j - 1]) - c2);
        return centre + (a0 + a1);
    }
    int delay2x() const noexcept { return 2 * pairs - 1; }
    int pairCount() const noexcept { return pairs; }

private:
    static constexpr int kRing = 64;
    std::array<double, 2 * kRing> fb {};
    std::array<double, kRing> sb {};
    std::array<double, HalfBandDesign::kMaxPairs> h {};
    int pairs = 1, pos = 0, spos = 0;
};
} // namespace vs::bside::chip
