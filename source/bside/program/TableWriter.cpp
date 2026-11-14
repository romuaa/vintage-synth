#include "bside/program/Program.h"

namespace vs::bside::program
{
namespace
{
const char* kHex = "0123456789abcdef";

std::string bitsText (std::uint8_t b)
{
    static constexpr struct { const char* name; std::uint8_t bit; } order[] = {
        { "tri", bitTri }, { "saw", bitSaw }, { "pulse", bitPulse }, { "noise", bitNoise },
        { "gate", bitGate }, { "sync", bitSync }, { "ring", bitRing }, { "test", bitTest } };
    std::string s;
    for (const auto& o : order)
        if ((b & o.bit) != 0)
            s += (s.empty() ? "" : "+") + std::string (o.name);
    return s.empty() ? "none" : s;
}

std::string pitchText (int kind, int value)
{
    if (kind == static_cast<int> (PitchKind::absolute))
        return "=" + std::to_string (value);
    if (kind == static_cast<int> (PitchKind::relative))
    {
        const int a = value < 0 ? -value : value;
        std::string s = value < 0 ? "-" : "+";
        s += std::to_string (a / 100);
        if (const int f = a % 100; f != 0)
            s += (f % 10 == 0) ? "." + std::to_string (f / 10) : (f < 10 ? ".0" : ".") + std::to_string (f);
        return s;
    }
    return "~";
}

std::string signedText (int v)
{
    return (v < 0 ? "-" : "+") + std::to_string (v < 0 ? -v : v);
}

std::string routeText (std::uint8_t m)
{
    if (m == 0)
        return "none";
    std::string s;
    for (int i = 0; i < 3; ++i)
        if ((m & (1u << i)) != 0)
            s += static_cast<char> ('1' + i);
    return s;
}

const char* modeText (std::uint8_t m)
{
    static constexpr const char* names[8] = { "off", "lp", "bp", "lp+bp", "hp", "lp+hp", "bp+hp", "lp+bp+hp" };
    return names[m & 7];
}

std::string labelName (int row, int release)
{
    return row == release ? "release" : "r" + std::to_string (row);
}

std::string cmdText (const Cmd& c, int release)
{
    switch (static_cast<Op> (c.op))
    {
        case Op::wave:
            return "wave " + bitsText (c.u0) + (c.a == 0 ? "" : " " + pitchText (c.a, c.b));
        case Op::pitch: return "pitch " + pitchText (c.a, c.b);
        case Op::gateOn: return "gate on";
        case Op::gateOff: return "gate off";
        case Op::adsr:
            return std::string ("adsr ") + kHex[(c.a >> 4) & 15] + ' ' + kHex[c.a & 15] + ' ' + kHex[(c.b >> 4) & 15] + ' ' + kHex[c.b & 15];
        case Op::pulseSet: return "set " + std::to_string (c.a);
        case Op::pulseSweep:
        case Op::cutoffSweep: return "sweep " + signedText (c.a) + " x" + std::to_string (c.u0);
        case Op::pulsePingPong: return "pingpong " + std::to_string (c.a) + " " + std::to_string (c.b) + " " + std::to_string (c.c);
        case Op::cutoff: return "cutoff " + std::to_string (c.a);
        case Op::res: return "res " + std::to_string (c.a);
        case Op::volume: return "volume " + std::to_string (c.a);
        case Op::mode: return std::string ("mode ") + modeText (c.u0);
        case Op::route: return "route " + routeText (c.u0);
        case Op::jump: return "jump " + labelName (c.a, release);
        case Op::repeat: return "repeat " + labelName (c.a, release) + " " + std::to_string (c.b);
        case Op::end: return "end";
        case Op::none:
        case Op::count_: break;
    }
    return {};
}
std::string rowText (const Program& p, int lane, int k)
{
    const auto& li = p.lanes[lane];
    const auto& row = p.rows[li.firstRow + k];
    std::string s = "    ";
    for (int c = 0; c < row.numCmds; ++c)
    {
        if (c > 0)
            s += ", ";
        s += cmdText (p.cmds[row.firstCmd + c], li.release);
    }
    const auto lastOp = static_cast<Op> (p.cmds[row.firstCmd + row.numCmds - 1].op);
    const int defaultDelay = (lastOp == Op::jump || lastOp == Op::repeat) ? 0 : 1;
    if (row.delay != defaultDelay)
        s += " ticks " + std::to_string (row.delay);
    return s;
}

/** The canonical text of a structurally valid program. */
std::string render (const Program& p)
{
    std::string s = "bside-table 1\n";
    const auto& r = p.restart;
    std::string rs;
    if (r.mode != 0)
        rs += std::string ("mode ") + (r.mode == 1 ? "classic" : "phase") + "\n";
    if (r.lead != 2)
        rs += "lead " + std::to_string (r.lead) + "\n";
    if (r.ad != 0)
        rs += std::string ("ad ") + kHex[r.ad >> 4] + kHex[r.ad & 15] + "\n";
    if (r.sr != 0)
        rs += std::string ("sr ") + kHex[r.sr >> 4] + kHex[r.sr & 15] + "\n";
    if (r.gateoff != r.lead)
        rs += "gateoff " + std::to_string (r.gateoff) + "\n";
    if (r.hasFirstWave != 0)
        rs += "firstwave " + bitsText (r.firstWave) + "\n";
    if (! rs.empty())
        s += "[restart]\n" + rs;

    for (int lane = 0; lane < kNumLanes; ++lane)
    {
        const auto& li = p.lanes[lane];
        if (li.numRows == 0)
            continue;
        s += std::string ("[") + laneName (lane) + "]\n";
        bool target[kMaxRowsPerLane] = {};
        for (int k = 0; k < li.numRows; ++k)
        {
            const auto& row = p.rows[li.firstRow + k];
            const auto& last = p.cmds[row.firstCmd + row.numCmds - 1];
            if (static_cast<Op> (last.op) == Op::jump || static_cast<Op> (last.op) == Op::repeat)
                target[last.a] = true;
        }
        if (li.release != kNoRelease)
            target[li.release] = true;
        for (int k = 0; k < li.numRows; ++k)
        {
            if (target[k])
                s += labelName (k, li.release) + ":\n";
            s += rowText (p, lane, k) + '\n';
        }
    }
    return s;
}
} // namespace

std::size_t canonicalRowBytes (const Program& p, int lane, int row)
{
    return rowText (p, lane, row).size();
}

std::size_t canonicalTextBytes (const Program& p)
{
    return render (p).size();
}

bool writeTable (const Program& p, std::string& out)
{
    out.clear();
    if (! validate (p))
        return false;
    out = render (p);
    return true;
}
} // namespace vs::bside::program
