#pragma once

// A model of one lane's tick, written from docs/architecture.md 8.6.2 and 8.6.3 (not from the
// reader), to test the guarantee "an accepted table executes at most kMaxRowsPerTick rows per tick".
//
// A tick runs rows from the lane's position until it has executed a row with delay >= 1, a sweep, a
// ping-pong or an end. `jump` continues at its target, `repeat` jumps to its target while its counter
// is below its count (then resets the counter and falls through). The model starts at any row, with
// any state of the repeat counters (a lane keeps them between ticks), which covers entering at the
// start of a note, after a delay, and at the `release` label.

#include "FuzzSupport.h"

#include "bside/program/Program.h"

namespace bst
{
/** Rows executed by one tick that starts at `start` with the given repeat counters; `cap` stops a runaway. */
inline long long rowsInTick (const vs::bside::program::Program& p, int lane, int start, int (&counters)[64], long long cap)
{
    using namespace vs::bside::program;
    const auto& li = p.lanes[lane];
    int r = start;
    long long n = 0;
    while (r >= 0 && r < li.numRows && n < cap)
    {
        ++n;
        const Row& row = p.rows[li.firstRow + r];
        const Cmd& last = p.cmds[row.firstCmd + row.numCmds - 1];
        const auto op = static_cast<Op> (last.op);
        if (isTickBound (op))
            break;
        int next = r + 1;
        if (op == Op::jump)
            next = last.a;
        else if (op == Op::repeat)
        {
            if (counters[r] < last.b)
            {
                ++counters[r];
                next = last.a;
            }
            else
            {
                counters[r] = 0;
            }
        }
        if (row.delay >= 1)
            break;
        r = next;
    }
    return n;
}

/** The most rows any tick of any lane can run, over every start row (and the release row), with
    `trials` random states of the repeat counters per start. Capped at `cap`. */
inline long long maxRowsPerTick (const vs::bside::program::Program& p, Rng& rng, int trials = 6, long long cap = 100000)
{
    using namespace vs::bside::program;
    long long worst = 0;
    for (int lane = 0; lane < kNumLanes; ++lane)
        for (int start = 0; start < p.lanes[lane].numRows; ++start)
            for (int t = 0; t < trials; ++t)
            {
                int counters[64] = {};
                for (int r = 0; r < p.lanes[lane].numRows; ++r)
                {
                    const Row& row = p.rows[p.lanes[lane].firstRow + r];
                    const Cmd& last = p.cmds[row.firstCmd + row.numCmds - 1];
                    if (static_cast<Op> (last.op) == Op::repeat)
                        counters[r] = t == 0 ? 0 : rng.range (0, last.b);
                }
                const auto n = rowsInTick (p, lane, start, counters, cap);
                if (n > worst)
                    worst = n;
            }
    return worst;
}
} // namespace bst
