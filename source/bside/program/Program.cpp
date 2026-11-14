#include "bside/program/Program.h"

#include <cstdlib>
#include <cstring>

namespace vs::bside::program
{
const char* laneName (int lane) noexcept
{
    static constexpr const char* names[kNumLanes] = { "wave1", "wave2", "wave3", "pulse1", "pulse2", "pulse3", "filter" };
    return lane >= 0 && lane < kNumLanes ? names[lane] : "?";
}

Program emptyProgram() noexcept
{
    Program p {};
    p.restart.lead = 2;
    p.restart.gateoff = 2;
    for (auto& l : p.lanes)
        l.release = kNoRelease;
    return p;
}

bool operator== (const Program& a, const Program& b) noexcept
{
    return std::memcmp (&a, &b, sizeof (Program)) == 0;
}

namespace
{
enum Used : unsigned { uU0 = 1, uA = 2, uB = 4, uC = 8 };

bool laneAllows (LaneKind kind, Op op) noexcept
{
    switch (op)
    {
        case Op::jump:
        case Op::repeat:
        case Op::end: return true;
        case Op::wave:
        case Op::pitch:
        case Op::gateOn:
        case Op::gateOff:
        case Op::adsr: return kind == LaneKind::Wave;
        case Op::pulseSet:
        case Op::pulseSweep:
        case Op::pulsePingPong: return kind == LaneKind::Pulse;
        case Op::cutoff:
        case Op::cutoffSweep:
        case Op::res:
        case Op::volume:
        case Op::mode:
        case Op::route: return kind == LaneKind::Filter;
        case Op::none:
        case Op::count_: break;
    }
    return false;
}

bool inRange (int v, int lo, int hi) noexcept { return v >= lo && v <= hi; }

/** Returns nullptr when the command is valid, else what is wrong. */
const char* checkCmd (const Cmd& c, LaneKind kind, int laneRows) noexcept
{
    if (c.op == 0 || c.op >= static_cast<std::uint8_t> (Op::count_))
        return "unknown operation";
    const auto op = static_cast<Op> (c.op);
    if (! laneAllows (kind, op))
        return "operation not allowed in this lane";
    unsigned used = 0;
    switch (op)
    {
        case Op::wave: used = uU0 | uA | uB; break;
        case Op::pitch: used = uA | uB; break;
        case Op::adsr: used = uA | uB; break;
        case Op::pulseSweep:
        case Op::cutoffSweep: used = uU0 | uA; break;
        case Op::pulsePingPong: used = uA | uB | uC; break;
        case Op::pulseSet:
        case Op::cutoff:
        case Op::res:
        case Op::volume:
        case Op::jump: used = uA; break;
        case Op::mode:
        case Op::route: used = uU0; break;
        case Op::repeat: used = uA | uB; break;
        case Op::gateOn:
        case Op::gateOff:
        case Op::end:
        case Op::none:
        case Op::count_: break;
    }
    if ((used & uU0) == 0 && c.u0 != 0) return "unused field not zero";
    if ((used & uA) == 0 && c.a != 0) return "unused field not zero";
    if ((used & uB) == 0 && c.b != 0) return "unused field not zero";
    if ((used & uC) == 0 && c.c != 0) return "unused field not zero";

    switch (op)
    {
        case Op::wave:
        case Op::pitch:
            if (! inRange (c.a, 0, 2)) return "pitch kind out of range";
            if (c.a == static_cast<int> (PitchKind::unchanged) && c.b != 0) return "pitch value without a pitch";
            if (c.a == static_cast<int> (PitchKind::relative) && ! inRange (c.b, -kMaxRelativeHundredths, kMaxRelativeHundredths))
                return "relative pitch out of range";
            if (c.a == static_cast<int> (PitchKind::absolute) && ! inRange (c.b, 0, kMaxNote)) return "note out of range";
            break;
        case Op::adsr:
            if (! inRange (c.a, 0, 255) || ! inRange (c.b, 0, 255)) return "envelope value out of range";
            break;
        case Op::pulseSet:
            if (! inRange (c.a, 0, 4095)) return "pulse width out of range";
            break;
        case Op::pulseSweep:
            if (! inRange (c.a, -4095, 4095) || c.u0 < 1) return "sweep out of range";
            break;
        case Op::cutoffSweep:
            if (! inRange (c.a, -2047, 2047) || c.u0 < 1) return "sweep out of range";
            break;
        case Op::pulsePingPong:
            if (! inRange (c.a, 0, 4095) || ! inRange (c.b, 0, 4095) || c.a >= c.b || ! inRange (c.c, 1, 4095))
                return "ping-pong out of range";
            break;
        case Op::cutoff:
            if (! inRange (c.a, 0, 2047)) return "cutoff out of range";
            break;
        case Op::res:
        case Op::volume:
            if (! inRange (c.a, 0, 15)) return "value out of range";
            break;
        case Op::mode:
        case Op::route:
            if (c.u0 > 7) return "mask out of range";
            break;
        case Op::jump:
            if (! inRange (c.a, 0, laneRows - 1)) return "jump target outside the lane";
            break;
        case Op::repeat:
            if (! inRange (c.a, 0, laneRows - 1)) return "jump target outside the lane";
            if (! inRange (c.b, 1, 255)) return "repeat count out of range";
            break;
        case Op::gateOn:
        case Op::gateOff:
        case Op::end:
        case Op::none:
        case Op::count_: break;
    }
    return nullptr;
}

bool allZero (const void* p, size_t n) noexcept
{
    const auto* b = static_cast<const unsigned char*> (p);
    for (size_t i = 0; i < n; ++i)
        if (b[i] != 0)
            return false;
    return true;
}
} // namespace

int findZeroDelayLoop (const Program& p, int lane) noexcept
{
    const auto& li = p.lanes[lane];
    const int n = li.numRows;
    if (n <= 0 || n > kMaxRowsPerLane)
        return -1;
    const auto rowAt = [&] (int r) -> const Row& { return p.rows[li.firstRow + r]; };
    const auto lastCmd = [&] (int r) -> const Cmd& { return p.cmds[rowAt (r).firstCmd + rowAt (r).numCmds - 1]; };
    const auto inGraph = [&] (int r) { return rowAt (r).delay == 0 && ! isTickBound (static_cast<Op> (lastCmd (r).op)); };

    // Edges of a row: `jump` goes to its target; `repeat` to its target and to the next row (it may
    // jump back or fall through); everything else to the next row. Depth-first search with three
    // colours over the delay-0 rows that do not stop the lane; an edge back to a row on the current
    // path is a loop. At most two edges per row, so at most 2 * 64 steps.
    int colour[kMaxRowsPerLane] = {}; // 0 new, 1 on the current path, 2 done
    int stackRow[kMaxRowsPerLane];
    int stackEdge[kMaxRowsPerLane];
    for (int start = 0; start < n; ++start)
    {
        if (colour[start] != 0 || ! inGraph (start))
            continue;
        int sp = 0;
        stackRow[0] = start;
        stackEdge[0] = 0;
        colour[start] = 1;
        while (sp >= 0)
        {
            const int r = stackRow[sp];
            const auto& c = lastCmd (r);
            const auto op = static_cast<Op> (c.op);
            int next = -1;
            const int edge = stackEdge[sp]++;
            if (op == Op::jump)
                next = edge == 0 ? c.a : -1;
            else if (op == Op::repeat)
                next = edge == 0 ? c.a : edge == 1 ? r + 1 : -1;
            else
                next = edge == 0 ? r + 1 : -1;
            if (edge >= 2 || (edge == 1 && op != Op::repeat))
            {
                colour[r] = 2;
                --sp;
                continue;
            }
            if (next < 0 || next >= n || ! inGraph (next))
                continue;
            if (colour[next] == 1)
                return next;
            if (colour[next] == 0)
            {
                ++sp;
                stackRow[sp] = next;
                stackEdge[sp] = 0;
                colour[next] = 1;
            }
        }
    }
    return -1;
}

bool validate (const Program& p, std::string* why)
{
    const auto fail = [&] (const char* msg, int lane = -1) {
        if (why != nullptr)
        {
            *why = msg;
            if (lane >= 0)
                *why += std::string (" (lane ") + laneName (lane) + ")";
        }
        return false;
    };
    const auto& r = p.restart;
    if (r.mode > 2) return fail ("restart mode out of range");
    if (r.lead < 1 || r.lead > 8 || r.gateoff < 1 || r.gateoff > 8) return fail ("restart lead or gateoff out of range");
    if (r.hasFirstWave > 1 || (r.hasFirstWave == 0 && r.firstWave != 0)) return fail ("restart firstwave inconsistent");
    if (r.reserved != 0) return fail ("reserved byte not zero");
    if (p.numRows > kMaxRows || p.numCmds > kMaxCommands) return fail ("too many rows or commands");

    int rowCursor = 0, cmdCursor = 0;
    for (int lane = 0; lane < kNumLanes; ++lane)
    {
        const auto& li = p.lanes[lane];
        if (li.firstRow != rowCursor) return fail ("lane rows not contiguous", lane);
        if (li.numRows > kMaxRowsPerLane) return fail ("too many rows in a lane", lane);
        if (li.release != kNoRelease && li.release >= li.numRows) return fail ("release row outside the lane", lane);
        if (rowCursor + li.numRows > p.numRows) return fail ("lane rows exceed the row count", lane);
        const auto kind = laneKind (lane);
        for (int k = 0; k < li.numRows; ++k)
        {
            const auto& row = p.rows[rowCursor + k];
            if (row.firstCmd != cmdCursor || row.numCmds < 1) return fail ("row commands not contiguous", lane);
            if (cmdCursor + row.numCmds > p.numCmds) return fail ("row commands exceed the command count", lane);
            for (int c = 0; c < row.numCmds; ++c)
            {
                const auto& cmd = p.cmds[cmdCursor + c];
                if (const char* bad = checkCmd (cmd, kind, li.numRows))
                    return fail (bad, lane);
                const bool terminator = isRowTerminator (static_cast<Op> (cmd.op));
                if (terminator && c != row.numCmds - 1) return fail ("command after a flow, sweep or ping-pong command", lane);
                if (c == row.numCmds - 1 && isTickBound (static_cast<Op> (cmd.op)) && row.delay != 1)
                    return fail ("end, sweep and ping-pong rows take no ticks", lane);
            }
            cmdCursor += row.numCmds;
        }
        rowCursor += li.numRows;
    }
    if (rowCursor != p.numRows || cmdCursor != p.numCmds) return fail ("row or command count does not match the lanes");
    if (! allZero (p.rows + p.numRows, sizeof (Row) * static_cast<size_t> (kMaxRows - p.numRows))
        || ! allZero (p.cmds + p.numCmds, sizeof (Cmd) * static_cast<size_t> (kMaxCommands - p.numCmds)))
        return fail ("unused storage not zero");
    for (int lane = 0; lane < kNumLanes; ++lane)
        if (findZeroDelayLoop (p, lane) >= 0)
            return fail ("rows with delay 0 form a loop", lane);
    // Everything the text format limits, so that a valid program always has a readable canonical text.
    for (int lane = 0; lane < kNumLanes; ++lane)
    {
        const auto& li = p.lanes[lane];
        bool target[kMaxRowsPerLane] = {};
        int labels = 0;
        const auto mark = [&] (int row) {
            if (! target[row])
            {
                target[row] = true;
                ++labels;
            }
        };
        for (int k = 0; k < li.numRows; ++k)
        {
            const auto& row = p.rows[li.firstRow + k];
            const auto& last = p.cmds[row.firstCmd + row.numCmds - 1];
            if (static_cast<Op> (last.op) == Op::jump || static_cast<Op> (last.op) == Op::repeat)
                mark (last.a);
        }
        if (li.release != kNoRelease)
            mark (li.release);
        if (labels > kMaxLabelsPerLane)
            return fail ("more jump targets than a lane may have labels", lane);
        for (int k = 0; k < li.numRows; ++k)
            if (canonicalRowBytes (p, lane, k) > static_cast<std::size_t> (kMaxLineBytes))
                return fail ("canonical text of a row is longer than a line may be", lane);
    }
    if (canonicalTextBytes (p) > kMaxTextBytes)
        return fail ("canonical text is longer than a table may be");
    return true;
}
} // namespace vs::bside::program
