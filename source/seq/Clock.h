#pragma once

// Shared clock of the arpeggiator and the step sequencer.
//
// Steps live on a musical grid: step k starts at ppq position pos(k) (see Grid), computed from the
// integer index k, never accumulated. A block maps ppq to samples with the block's own
// (ppq0, samples-per-ppq): sample = ceil((ppq - ppq0) * spp) - 1e-6 tolerance. Hence two blocks
// split anywhere give the same positions as one, and nothing drifts.
//
// Time source: the host (`playing`, ppq and bpm from the Transport) or, when stopped, an internal
// clock at the module's own tempo that the front-end starts on the first key. The internal clock
// is "ppq = 0 at the anchor sample, then (sample - anchor) / spp", so it is block-size independent.

#include "Settings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vs::seq
{
/** Host tempo range the clock accepts; anything else (and non-finite values) is unusable and the
    internal clock takes over. */
inline constexpr double kMinHostBpm = 1.0;
inline constexpr double kMaxHostBpm = 1000.0;

/** How far (quarter notes) the host position may differ from where the previous block ended and
    still count as the same playback ("drift": tempo ramps, rounding, a host that reports tempo once
    per block). Larger differences are jumps (loop, locate): the module releases and resynchronises.

    Threshold = half a step, but never less than the duration of the block just rendered (a tempo
    ramp can mis-predict the end of a block by a fraction of its length), and never more than one
    step: with less than a step of drift at most one step can be overdue, so it can be fired once
    (never skipped, never doubled); with more, steps would have to be skipped or bunched up, and
    resynchronising on the new position is the honest handling. */
inline double driftLimitPpq (double stepPpq, double lastBlockPpq) noexcept
{
    return std::min (std::max (0.5 * stepPpq, lastBlockPpq), stepPpq);
}

namespace detail
{
/** Sample index of a fractional sample position: the first sample at or after it. A position
    within 1e-6 samples above an integer counts as that integer (float noise; real positions of
    rational tempi are never that close without being exact). */
inline std::int64_t ceilSamples (double x) noexcept
{
    constexpr double kEps = 1.0e-6;
    constexpr double kLimit = 1.0e15;
    if (std::isnan (x))
        return static_cast<std::int64_t> (kLimit); // "never"
    const double c = std::ceil (x - kEps);
    return static_cast<std::int64_t> (std::clamp (c, -kLimit, kLimit));
}

/** a and b hold the same value (written without ==, which GCC's -Wfloat-equal rejects). */
inline bool sameValue (double a, double b) noexcept
{
    return ! (a < b) && ! (a > b);
}
} // namespace detail

/** The step grid for one rate and swing. Swing delays every second (odd) step by (swing - 0.5) of
    a step pair. */
struct Grid
{
    int stepsPerQuarter = 4;
    double swing = 0.5;

    /** Start of step k in quarter notes. Step pair = 2 steps: pair start + swing * pair for odd k. */
    double pos (std::int64_t k) const noexcept
    {
        const double offset = (k & 1) != 0 ? (2.0 * swing - 1.0) : 0.0;
        return (static_cast<double> (k) + offset) / static_cast<double> (stepsPerQuarter);
    }
};

class Clock
{
public:
    /** Mapping between ppq and the samples of the current block. */
    struct View
    {
        double ppq0 = 0.0; // position at sample 0 of the block
        double spp = 1.0;  // samples per quarter note
        bool hostSynced = false;

        std::int64_t sampleOf (double ppq) const noexcept { return detail::ceilSamples ((ppq - ppq0) * spp); }
        double ppqAt (int sample) const noexcept { return ppq0 + static_cast<double> (sample) / spp; }
    };

    struct Block
    {
        View view;
        bool sourceChanged = false; // host <-> internal (never on the first block)
        bool jumped = false;        // first block, source change, or a discontinuity of the host position
    };

    void reset() noexcept { *this = Clock {}; }

    /** Call once at the start of every process call. `internalBpm` is used when the host is not playing. */
    Block begin (const Transport& tr, int numSamples, double internalBpm, double stepPpq = 0.25) noexcept
    {
        const double sr = (std::isfinite (tr.sampleRate) && tr.sampleRate >= 1000.0 && tr.sampleRate <= 1.0e7) ? tr.sampleRate : 44100.0;
        const bool hostOk = tr.playing && std::isfinite (tr.bpm) && tr.bpm >= kMinHostBpm && tr.bpm <= kMaxHostBpm
                            && std::isfinite (tr.ppqPosition) && std::fabs (tr.ppqPosition) < 1.0e9;
        Block b;
        if (hostOk)
        {
            b.view.hostSynced = true;
            b.view.spp = 60.0 * sr / tr.bpm;
            b.view.ppq0 = tr.ppqPosition;
            if (! havePrev_)
                b.jumped = true;
            else if (! prevHost_)
                b.sourceChanged = b.jumped = true;
            else if (std::fabs (tr.ppqPosition - expectedPpq_) > driftLimitPpq (stepPpq, lastBlockPpq_))
                b.jumped = true;
            lastBlockPpq_ = static_cast<double> (std::max (numSamples, 0)) / b.view.spp;
            expectedPpq_ = tr.ppqPosition + lastBlockPpq_;
            intRunning_ = false;
        }
        else
        {
            const double bpm = (std::isfinite (internalBpm) && internalBpm > 0.0) ? internalBpm : 120.0;
            b.view.hostSynced = false;
            b.view.spp = 60.0 * sr / bpm;
            if (havePrev_ && prevHost_)
                b.sourceChanged = b.jumped = true;
            if (intRunning_)
            {
                if (! detail::sameValue (b.view.spp, intSpp_))
                {
                    // Tempo (or sample rate) change: continue from the current position.
                    intAnchorPpq_ += static_cast<double> (counter_ - intAnchorSample_) / intSpp_;
                    intAnchorSample_ = counter_;
                    intSpp_ = b.view.spp;
                }
                b.view.ppq0 = intAnchorPpq_ + static_cast<double> (counter_ - intAnchorSample_) / intSpp_;
            }
        }
        prevHost_ = hostOk;
        havePrev_ = true;
        return b;
    }

    /** Call at the end of every process call. */
    void end (int numSamples) noexcept { counter_ += std::max (numSamples, 0); }

    /** Internal clock only: ppq = 0 at sample `t` of the current block. Updates `view`. */
    void startInternal (View& view, int t) noexcept
    {
        intRunning_ = true;
        intSpp_ = view.spp;
        intAnchorPpq_ = 0.0;
        intAnchorSample_ = counter_ + t;
        view.ppq0 = static_cast<double> (counter_ - intAnchorSample_) / intSpp_;
    }

    void stopInternal() noexcept { intRunning_ = false; }
    bool internalRunning() const noexcept { return intRunning_; }

private:
    bool havePrev_ = false;
    bool prevHost_ = false;
    double expectedPpq_ = 0.0; // host position expected at the start of the next block
    double lastBlockPpq_ = 0.0; // length of the previous host block in quarter notes
    std::int64_t counter_ = 0; // samples processed so far (internal clock time base)
    bool intRunning_ = false;
    double intAnchorPpq_ = 0.0;
    std::int64_t intAnchorSample_ = 0;
    double intSpp_ = 1.0;
};
} // namespace vs::seq
