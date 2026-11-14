#include "bside/program/TableReader.h"

#include <memory>
#include <new>

namespace vs::bside::program
{
namespace
{
using namespace vs::bside::text;

constexpr unsigned kW = 1, kP = 2, kF = 4, kAll = 7;

struct CmdInfo
{
    const char* name;
    unsigned lanes;
};
constexpr CmdInfo kCommands[] = { { "wave", kW },     { "pitch", kW },   { "gate", kW },  { "adsr", kW },  { "set", kP },
                                  { "sweep", kP | kF }, { "pingpong", kP }, { "cutoff", kF }, { "res", kF },   { "volume", kF },
                                  { "mode", kF },     { "route", kF },   { "jump", kAll }, { "repeat", kAll }, { "end", kAll } };

constexpr const char* kSectionNames[8] = { "wave1", "wave2", "wave3", "pulse1", "pulse2", "pulse3", "filter", "restart" };
constexpr int kRestartSection = 7;

unsigned kindBit (LaneKind k) noexcept
{
    return k == LaneKind::Wave ? kW : k == LaneKind::Pulse ? kP : kF;
}

const CmdInfo* findCommand (std::string_view w) noexcept
{
    for (const auto& c : kCommands)
        if (w == c.name)
            return &c;
    return nullptr;
}

std::string commandList (LaneKind k)
{
    std::string s;
    for (const auto& c : kCommands)
        if ((c.lanes & kindBit (k)) != 0)
            s += (s.empty() ? "" : ", ") + std::string (c.name);
    return s;
}

const char* laneKindName (LaneKind k) noexcept
{
    return k == LaneKind::Wave ? "wave" : k == LaneKind::Pulse ? "pulse" : "filter";
}

bool isNameStart (char c) noexcept { return c >= 'a' && c <= 'z'; }
bool isNameChar (char c) noexcept { return isNameStart (c) || isDigit (c) || c == '_'; }

bool validLabelName (std::string_view n) noexcept
{
    if (n.empty() || n.size() > 16 || ! isNameStart (n[0]))
        return false;
    for (const char c : n)
        if (! isNameChar (c))
            return false;
    return true;
}

/** Parses "none" or control/waveform names joined by '+'. On failure `badAt` is the offset of the
    offending piece in tok and `duplicate` says whether it was a repeat (else unknown or empty). */
bool parseControlBits (std::string_view tok, std::uint8_t& out, size_t& badAt, bool& duplicate) noexcept
{
    static constexpr struct { const char* name; std::uint8_t bit; } names[] = {
        { "tri", bitTri }, { "saw", bitSaw }, { "pulse", bitPulse }, { "noise", bitNoise },
        { "gate", bitGate }, { "sync", bitSync }, { "ring", bitRing }, { "test", bitTest } };
    out = 0;
    duplicate = false;
    if (tok == "none")
        return true;
    size_t from = 0;
    for (;;)
    {
        const size_t plus = tok.find ('+', from);
        const auto piece = tok.substr (from, plus == std::string_view::npos ? std::string_view::npos : plus - from);
        std::uint8_t bit = 0;
        for (const auto& nm : names)
            if (piece == nm.name)
                bit = nm.bit;
        badAt = from;
        if (bit == 0)
            return false;
        if ((out & bit) != 0)
        {
            duplicate = true;
            return false;
        }
        out = static_cast<std::uint8_t> (out | bit);
        if (plus == std::string_view::npos)
            return true;
        from = plus + 1;
    }
}

struct Fail
{
    bool failed = false;
    int col = 0;
    std::string what, expected;
    bool set (int c, std::string w, std::string e = {})
    {
        if (! failed)
        {
            failed = true;
            col = c;
            what = std::move (w);
            expected = std::move (e);
        }
        return false;
    }
};

std::string quoted (std::string_view s)
{
    // Keep messages short and printable: the text was checked to be valid UTF-8 without control characters.
    return "'" + std::string (s.substr (0, 24)) + (s.size() > 24 ? "...'" : "'");
}

struct RowOut
{
    Cmd cmds[64];
    int n = 0;
    int delay = 1;
    bool hasRef = false;
    std::string_view refName;
    int refCol = 0;
};

class RowParser
{
public:
    RowParser (Cursor& c, LaneKind k, Fail& f) : cur (c), kind (k), fail (f) {}

    bool parse (RowOut& out)
    {
        std::string_view lastWord;
        for (;;)
        {
            cur.skipWs();
            Cmd c {};
            std::string_view word;
            if (! parseCommand (c, out, word))
                return false;
            if (out.n >= 64)
                return fail.set (cur.column(), "too many commands in one row", "at most 64");
            out.cmds[out.n++] = c;
            lastWord = word;
            const auto op = static_cast<Op> (c.op);
            cur.skipWs();
            if (cur.atEol())
                break;
            if (cur.peek() == ',')
            {
                ++cur.i;
                cur.skipWs();
                if (cur.atEol())
                    return fail.set (cur.column(), "missing command after ','", "a command");
                if (isRowTerminator (op))
                    return fail.set (cur.column(), "command after '" + std::string (lastWord) + "' in the same row",
                                     "'" + std::string (lastWord) + "' to be the last command of its row");
                continue;
            }
            const int col = cur.column();
            const auto tok = raw();
            if (tok != "ticks")
                return fail.set (col, "unexpected " + quoted (tok), "',', 'ticks' or the end of the line");
            if (isTickBound (op))
                return fail.set (col, "'" + std::string (lastWord) + "' rows take no ticks", "the end of the row");
            int vcol = 0;
            std::string_view v;
            if (! arg ("a number of ticks from 0 to 255", v, vcol))
                return false;
            std::uint64_t t = 0;
            if (v.size() > 3 || parseDecimal (v, 255, t) != NumberResult::ok)
                return fail.set (vcol, parseDecimal (v, 255, t) == NumberResult::notNumber ? quoted (v) + " is not a number"
                                                                                           : "ticks " + quoted (v) + " out of range",
                                 "a number of ticks from 0 to 255 (at most 3 digits)");
            out.delay = static_cast<int> (t);
            hasTicks = true;
            cur.skipWs();
            if (! cur.atEol())
            {
                const int c2 = cur.column();
                return fail.set (c2, "unexpected " + quoted (raw()), "the end of the line");
            }
            break;
        }
        // Default delays: a flow row (jump, repeat) costs no tick unless it says so; the rows that
        // stop the lane for the tick (end, sweep, ping-pong) always store 1.
        const auto lastOp = static_cast<Op> (out.cmds[out.n - 1].op);
        if (isTickBound (lastOp))
            out.delay = 1;
        else if (! hasTicks)
            out.delay = (lastOp == Op::jump || lastOp == Op::repeat) ? 0 : 1;
        return true;
    }

private:
    Cursor& cur;
    LaneKind kind;
    Fail& fail;
    bool hasTicks = false;

    /** A token; '#' inside a note name ("=C#6") belongs to it, anywhere else '#' starts a comment. */
    std::string_view raw() noexcept
    {
        const size_t from = cur.i;
        while (cur.i < cur.s.size())
        {
            const char ch = cur.s[cur.i];
            if (ch == ' ' || ch == '\t' || ch == ',')
                break;
            if (ch == '#' && ! (cur.s[from] == '=' && cur.i - from == 2))
                break;
            ++cur.i;
        }
        return cur.s.substr (from, cur.i - from);
    }

    bool arg (const char* expected, std::string_view& tok, int& col)
    {
        cur.skipWs();
        col = cur.column();
        if (cur.atEol() || cur.peek() == ',')
            return fail.set (col, "missing argument", expected);
        tok = raw();
        return true;
    }

    bool integer (std::string_view tok, int col, bool signedOk, long lo, long hi, const std::string& expected, int& out)
    {
        size_t k = 0;
        bool neg = false;
        if (signedOk && ! tok.empty() && (tok[0] == '+' || tok[0] == '-'))
        {
            neg = tok[0] == '-';
            k = 1;
        }
        std::uint64_t v = 0;
        const auto r = parseDecimal (tok.substr (k), 999999999ull, v);
        if (r == NumberResult::notNumber)
            return fail.set (col, quoted (tok) + " is not a number", expected);
        if (r == NumberResult::outOfRange)
            return fail.set (col, "value " + quoted (tok) + " is out of range", expected);
        const long value = neg ? -static_cast<long> (v) : static_cast<long> (v);
        if (value < lo || value > hi)
            return fail.set (col, "value " + quoted (tok) + " is out of range", expected);
        out = static_cast<int> (value);
        return true;
    }

    bool hexNibble (std::string_view tok, int col, int& out)
    {
        std::uint64_t v = 0;
        if (tok.size() != 1 || parseHex (tok, 1, 15, v) != NumberResult::ok)
            return fail.set (col, quoted (tok) + " is not a hex digit", "one hex digit 0-f");
        out = static_cast<int> (v);
        return true;
    }

    bool bits (std::string_view tok, int col, std::uint8_t& out)
    {
        size_t at = 0;
        bool dup = false;
        if (parseControlBits (tok, out, at, dup))
            return true;
        const auto rest = tok.substr (at);
        const auto piece = rest.substr (0, rest.find ('+'));
        return fail.set (col + static_cast<int> (at),
                         dup ? quoted (piece) + " given twice" : piece.empty() ? "empty waveform or control name" : "unknown name " + quoted (piece),
                         "none, or tri, saw, pulse, noise, gate, sync, ring, test joined by '+'");
    }

    bool pitch (std::string_view tok, int col, PitchKind& kindOut, int& value)
    {
        const char* expected = "a semitone offset (+7, -12, +0.15), '=' and a note (=C#6, =61), or '~'";
        kindOut = PitchKind::unchanged;
        value = 0;
        if (tok == "~")
            return true;
        if (tok.substr (0, 3) == "key" && tok.size() == 4 && tok[3] >= '1' && tok[3] <= '3')
            return fail.set (col, quoted (tok) + " is not available yet", expected);
        if (tok[0] == '+' || tok[0] == '-')
        {
            const bool neg = tok[0] == '-';
            const auto body = tok.substr (1);
            const size_t dot = body.find ('.');
            const auto ip = body.substr (0, dot);
            const auto fp = dot == std::string_view::npos ? std::string_view() : body.substr (dot + 1);
            std::uint64_t iv = 0, fv = 0;
            if (ip.empty() || ip.size() > 2 || parseDecimal (ip, 99, iv) != NumberResult::ok
                || (dot != std::string_view::npos && (fp.empty() || fp.size() > 2 || parseDecimal (fp, 99, fv) != NumberResult::ok)))
                return fail.set (col, "bad semitone offset " + quoted (tok), expected);
            if (fp.size() == 1)
                fv *= 10;
            const int h = static_cast<int> (iv * 100 + fv);
            if (h > kMaxRelativeHundredths)
                return fail.set (col, "semitone offset " + quoted (tok) + " is out of range", "at most 96 semitones either way");
            kindOut = PitchKind::relative;
            value = neg ? -h : h;
            return true;
        }
        if (tok[0] == '=')
        {
            const auto body = tok.substr (1);
            if (body.empty())
                return fail.set (col + 1, "missing note after '='", expected);
            std::uint64_t n = 0;
            if (isDigit (body[0]))
            {
                const auto r = parseDecimal (body, 127, n);
                if (r != NumberResult::ok)
                    return fail.set (col + 1, r == NumberResult::notNumber ? "bad note " + quoted (body) : "note " + quoted (body) + " is out of range",
                                     "a note name or a MIDI note from 0 to 127");
                kindOut = PitchKind::absolute;
                value = static_cast<int> (n);
                return true;
            }
            // Note name: letter, optional '#' or 'b', optional '-', octave digits (C4 = 60).
            const char l = static_cast<char> (body[0] | 0x20);
            static constexpr int pc[7] = { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g
            if (l < 'a' || l > 'g')
                return fail.set (col + 1, "bad note " + quoted (body), "a note name like C#6, Db4, a3, or a number 0-127");
            size_t i = 1;
            int acc = 0;
            if (i < body.size() && body[i] == '#')
            {
                acc = 1;
                ++i;
            }
            else if (i < body.size() && (body[i] == 'b' || body[i] == 'B'))
            {
                acc = -1;
                ++i;
            }
            bool negOct = false;
            if (i < body.size() && body[i] == '-')
            {
                negOct = true;
                ++i;
            }
            std::uint64_t oct = 0;
            if (i >= body.size() || body.size() - i > 2 || parseDecimal (body.substr (i), 99, oct) != NumberResult::ok)
                return fail.set (col + 1, "bad note " + quoted (body), "a note name like C#6, Db4, a3, or a number 0-127");
            const int octave = negOct ? -static_cast<int> (oct) : static_cast<int> (oct);
            const int midi = 12 * (octave + 1) + pc[l - 'a'] + acc;
            if (midi < 0 || midi > kMaxNote)
                return fail.set (col + 1, "note " + quoted (body) + " is out of range", "a note from C-1 to G9");
            kindOut = PitchKind::absolute;
            value = midi;
            return true;
        }
        return fail.set (col, "bad pitch " + quoted (tok), expected);
    }

    bool parseCommand (Cmd& c, RowOut& out, std::string_view& word)
    {
        const int wcol = cur.column();
        if (cur.atEol() || cur.peek() == ',')
            return fail.set (wcol, "missing command", "a command (" + commandList (kind) + ")");
        word = raw();
        const auto* info = findCommand (word);
        if (info == nullptr)
            return fail.set (wcol, "unknown command " + quoted (word), "one of " + commandList (kind));
        if ((info->lanes & kindBit (kind)) == 0)
            return fail.set (wcol, "command " + quoted (word) + " is not available in a " + laneKindName (kind) + " lane",
                             "one of " + commandList (kind));
        std::string_view t;
        int col = 0;
        int v = 0;

        if (word == "wave")
        {
            if (! arg ("none, or tri, saw, pulse, noise, gate, sync, ring, test joined by '+'", t, col))
                return false;
            std::uint8_t b = 0;
            if (! bits (t, col, b))
                return false;
            PitchKind pk = PitchKind::unchanged;
            int pv = 0;
            cur.skipWs();
            const char n = cur.peek();
            if (n == '+' || n == '-' || n == '=' || n == '~' || n == 'k')
            {
                const int pcol = cur.column();
                if (! pitch (raw(), pcol, pk, pv))
                    return false;
            }
            c.op = static_cast<std::uint8_t> (Op::wave);
            c.u0 = b;
            c.a = static_cast<std::int16_t> (pk);
            c.b = static_cast<std::int16_t> (pv);
            return true;
        }
        if (word == "pitch")
        {
            if (! arg ("a semitone offset, '=' and a note, or '~'", t, col))
                return false;
            PitchKind pk = PitchKind::unchanged;
            int pv = 0;
            if (! pitch (t, col, pk, pv))
                return false;
            c.op = static_cast<std::uint8_t> (Op::pitch);
            c.a = static_cast<std::int16_t> (pk);
            c.b = static_cast<std::int16_t> (pv);
            return true;
        }
        if (word == "gate")
        {
            if (! arg ("on or off", t, col))
                return false;
            if (t != "on" && t != "off")
                return fail.set (col, "bad gate state " + quoted (t), "on or off");
            c.op = static_cast<std::uint8_t> (t == "on" ? Op::gateOn : Op::gateOff);
            return true;
        }
        if (word == "adsr")
        {
            int n[4] = {};
            for (int k = 0; k < 4; ++k)
            {
                if (! arg ("four hex digits: attack decay sustain release", t, col) || ! hexNibble (t, col, n[k]))
                    return false;
            }
            c.op = static_cast<std::uint8_t> (Op::adsr);
            c.a = static_cast<std::int16_t> (n[0] << 4 | n[1]);
            c.b = static_cast<std::int16_t> (n[2] << 4 | n[3]);
            return true;
        }
        if (word == "set")
        {
            const char* expected = "a pulse width 0 to 4095, or a percentage like 25%";
            if (! arg (expected, t, col))
                return false;
            if (t.back() == '%')
            {
                const auto body = t.substr (0, t.size() - 1);
                const size_t dot = body.find ('.');
                const auto ip = body.substr (0, dot);
                const auto fp = dot == std::string_view::npos ? std::string_view() : body.substr (dot + 1);
                std::uint64_t iv = 0, fv = 0;
                if (ip.empty() || ip.size() > 3 || parseDecimal (ip, 999, iv) != NumberResult::ok
                    || (dot != std::string_view::npos && (fp.empty() || fp.size() > 2 || parseDecimal (fp, 99, fv) != NumberResult::ok)))
                    return fail.set (col, "bad percentage " + quoted (t), expected);
                if (fp.size() == 1)
                    fv *= 10;
                const auto h = iv * 100 + fv; // hundredths of a percent
                if (h > 10000)
                    return fail.set (col, "percentage " + quoted (t) + " is out of range", "0% to 100%");
                auto pw = (h * 4096 + 5000) / 10000;
                if (pw > 4095)
                    pw = 4095;
                c.a = static_cast<std::int16_t> (pw);
            }
            else
            {
                if (! integer (t, col, false, 0, 4095, expected, v))
                    return false;
                c.a = static_cast<std::int16_t> (v);
            }
            c.op = static_cast<std::uint8_t> (Op::pulseSet);
            return true;
        }
        if (word == "sweep")
        {
            const bool pulse = kind == LaneKind::Pulse;
            const int limit = pulse ? 4095 : 2047;
            const std::string expectD = std::string ("a signed step, -") + (pulse ? "4095 to +4095" : "2047 to +2047");
            if (! arg (expectD.c_str(), t, col) || ! integer (t, col, true, -limit, limit, expectD, v))
                return false;
            int n = 0, ncol = 0;
            if (! arg ("a tick count like x16 (x1 to x255)", t, ncol))
                return false;
            if (t[0] != 'x' || ! integer (t.substr (1), ncol + 1, false, 1, 255, "a tick count from 1 to 255", n))
            {
                if (! fail.failed)
                    fail.set (ncol, "bad tick count " + quoted (t), "a tick count like x16 (x1 to x255)");
                return false;
            }
            c.op = static_cast<std::uint8_t> (pulse ? Op::pulseSweep : Op::cutoffSweep);
            c.a = static_cast<std::int16_t> (v);
            c.u0 = static_cast<std::uint8_t> (n);
            return true;
        }
        if (word == "pingpong")
        {
            int n[3] = {};
            const char* what[3] = { "lower limit 0 to 4095", "upper limit 0 to 4095", "step 1 to 4095" };
            int cols[3] = {};
            for (int k = 0; k < 3; ++k)
                if (! arg (what[k], t, cols[k]) || ! integer (t, cols[k], false, k == 2 ? 1 : 0, 4095, what[k], n[k]))
                    return false;
            if (n[0] >= n[1])
                return fail.set (cols[1], "upper limit " + std::to_string (n[1]) + " is not above the lower limit " + std::to_string (n[0]),
                                 "an upper limit above the lower limit");
            c.op = static_cast<std::uint8_t> (Op::pulsePingPong);
            c.a = static_cast<std::int16_t> (n[0]);
            c.b = static_cast<std::int16_t> (n[1]);
            c.c = static_cast<std::int16_t> (n[2]);
            return true;
        }
        if (word == "cutoff" || word == "res" || word == "volume")
        {
            const bool isCut = word == "cutoff";
            const std::string expected = isCut ? "an integer from 0 to 2047" : "an integer from 0 to 15";
            if (! arg (expected.c_str(), t, col) || ! integer (t, col, false, 0, isCut ? 2047 : 15, expected, v))
                return false;
            c.op = static_cast<std::uint8_t> (isCut ? Op::cutoff : word == "res" ? Op::res : Op::volume);
            c.a = static_cast<std::int16_t> (v);
            return true;
        }
        if (word == "mode")
        {
            const char* expected = "off, lp, bp, hp, lp+bp, bp+hp, lp+hp or lp+bp+hp";
            if (! arg (expected, t, col))
                return false;
            static constexpr struct { const char* name; std::uint8_t mask; } modes[] = {
                { "off", 0 }, { "lp", 1 }, { "bp", 2 }, { "hp", 4 }, { "lp+bp", 3 }, { "bp+hp", 6 }, { "lp+hp", 5 }, { "lp+bp+hp", 7 } };
            for (const auto& m : modes)
                if (t == m.name)
                {
                    c.op = static_cast<std::uint8_t> (Op::mode);
                    c.u0 = m.mask;
                    return true;
                }
            return fail.set (col, "bad filter mode " + quoted (t), expected);
        }
        if (word == "route")
        {
            const char* expected = "none, or oscillator digits 1-3 like 13";
            if (! arg (expected, t, col))
                return false;
            std::uint8_t mask = 0;
            if (t != "none")
            {
                for (size_t k = 0; k < t.size(); ++k)
                {
                    if (t[k] < '1' || t[k] > '3')
                        return fail.set (col + static_cast<int> (k), "bad oscillator " + quoted (t.substr (k, 1)), expected);
                    const auto bit = static_cast<std::uint8_t> (1u << (t[k] - '1'));
                    if ((mask & bit) != 0)
                        return fail.set (col + static_cast<int> (k), "oscillator " + quoted (t.substr (k, 1)) + " given twice", expected);
                    mask = static_cast<std::uint8_t> (mask | bit);
                }
            }
            c.op = static_cast<std::uint8_t> (Op::route);
            c.u0 = mask;
            return true;
        }
        if (word == "jump" || word == "repeat")
        {
            if (! arg ("a label name", t, col))
                return false;
            if (! validLabelName (t))
                return fail.set (col, "bad label name " + quoted (t), "a label name: [a-z][a-z0-9_]{0,15}");
            out.hasRef = true;
            out.refName = t;
            out.refCol = col;
            if (word == "jump")
            {
                c.op = static_cast<std::uint8_t> (Op::jump);
                return true;
            }
            int ncol = 0;
            if (! arg ("a repeat count from 1 to 255", t, ncol) || ! integer (t, ncol, false, 1, 255, "a repeat count from 1 to 255", v))
                return false;
            c.op = static_cast<std::uint8_t> (Op::repeat);
            c.b = static_cast<std::int16_t> (v);
            return true;
        }
        // end
        c.op = static_cast<std::uint8_t> (Op::end);
        return true;
    }
};

struct RowScratch
{
    int firstCmd = 0; // index into the pool
    int numCmds = 0;
    int delay = 1;
    int line = 0, col = 0;
};
struct LabelDef
{
    std::string_view name;
    int row = 0; // -1: names no row
    int line = 0, col = 0;
};
struct JumpRef
{
    int lane = 0;
    int cmd = 0; // index into the pool
    std::string_view name;
    int line = 0, col = 0;
};
struct LaneScratch
{
    bool seen = false;
    bool rowOverflow = false;
    int numRows = 0;
    int numLabels = 0;
    RowScratch rows[kMaxRowsPerLane];
    LabelDef labels[kMaxLabelsPerLane];
};
struct Scratch
{
    LaneScratch lanes[kNumLanes];
    Cmd pool[kMaxCommands];
    int poolSize = 0;
    bool commandOverflow = false;
    JumpRef refs[kMaxCommands];
    int numRefs = 0;
    Restart restart = emptyProgram().restart;
    bool restartSeen = false;
    unsigned settingsSeen = 0;
    bool gateoffSet = false;
};

class Reader
{
public:
    Reader (std::string_view t, ReadResult& r) : text (t), result (r), errors (r.errors) {}

    void run()
    {
        if (text.size() > kMaxTextBytes)
        {
            errors.add (1, 1, "table too large (" + std::to_string (text.size()) + " bytes)", "at most " + std::to_string (kMaxTextBytes) + " bytes");
            return;
        }
        int bl = 0, bc = 0;
        const char* why = nullptr;
        if (findInvalidByte (text, bl, bc, why))
        {
            errors.add (bl, bc, std::string ("invalid text: ") + why, "UTF-8 text without control characters");
            return;
        }
        s = std::make_unique<Scratch>();
        Lines lines (text);
        std::string_view line;
        int number = 0;
        while (lines.next (line, number))
        {
            if (number > kMaxLines)
            {
                errors.add (number, 1, "too many lines", "at most " + std::to_string (kMaxLines) + " lines");
                break;
            }
            if (line.size() > static_cast<size_t> (kMaxLineBytes))
            {
                errors.add (number, kMaxLineBytes + 1, "line too long (" + std::to_string (line.size()) + " bytes)",
                            "at most " + std::to_string (kMaxLineBytes) + " bytes");
                continue;
            }
            handleLine (line, number);
        }
        if (! headerSeen)
            errors.add (1, 1, "missing header", "'bside-table 1' as the first line");
        finish();
    }

private:
    std::string_view text;
    ReadResult& result;
    ErrorList& errors;
    std::unique_ptr<Scratch> s;
    bool headerSeen = false;
    int section = -1;

    static std::string_view plainToken (Cursor& c) noexcept { return c.token(); }

    void handleLine (std::string_view line, int number)
    {
        Cursor c { line, 0 };
        c.skipWs();
        if (c.atEol())
            return;
        if (! headerSeen)
        {
            headerSeen = true;
            const int col = c.column();
            const auto w = plainToken (c);
            if (w != "bside-table")
            {
                errors.add (number, col, "missing header", "'bside-table 1' as the first line");
                return;
            }
            c.skipWs();
            const int vcol = c.column();
            const auto v = plainToken (c);
            if (v.empty())
                errors.add (number, vcol, "missing table version", "1");
            else if (v != "1")
                errors.add (number, vcol, "unsupported table version " + quoted (v), "1");
            else
                endOfLine (c, number);
            return;
        }
        if (c.peek() == '[')
        {
            handleSection (c, number);
            return;
        }
        // A label: a name directly followed by ':'.
        size_t j = c.i;
        while (j < line.size() && isNameChar (line[j]))
            ++j;
        if (j > c.i && j < line.size() && line[j] == ':')
        {
            handleLabel (c, j, number);
            return;
        }
        if (section < 0)
        {
            errors.add (number, c.column(), "statement outside of a section", "a [section] header first");
            return;
        }
        if (section == kRestartSection)
            handleSetting (c, number);
        else
            handleRow (c, number);
    }

    void endOfLine (Cursor& c, int number)
    {
        c.skipWs();
        if (! c.atEol())
        {
            const int col = c.column();
            errors.add (number, col, "unexpected " + quoted (tokenOrChar (c)), "the end of the line");
        }
    }

    void handleSection (Cursor& c, int number)
    {
        const int col = c.column();
        const auto tok = plainToken (c);
        int idx = -1;
        for (int k = 0; k < 8; ++k)
            if (tok.size() == std::string_view (kSectionNames[k]).size() + 2 && tok.front() == '[' && tok.back() == ']'
                && tok.substr (1, tok.size() - 2) == kSectionNames[k])
                idx = k;
        if (idx < 0)
        {
            errors.add (number, col, "unknown section " + quoted (tok), "[wave1] [wave2] [wave3] [pulse1] [pulse2] [pulse3] [filter] or [restart]");
            section = -1;
            return;
        }
        bool& seenFlag = idx == kRestartSection ? s->restartSeen : s->lanes[idx].seen;
        if (seenFlag)
            errors.add (number, col, "section " + quoted (tok) + " appears twice", "each section at most once");
        seenFlag = true;
        section = idx;
        endOfLine (c, number);
    }

    void handleLabel (Cursor& c, size_t colon, int number)
    {
        const int col = c.column();
        const auto name = c.s.substr (c.i, colon - c.i);
        c.i = colon + 1;
        if (section < 0)
        {
            errors.add (number, col, "label outside of a section", "a lane section first");
            return;
        }
        if (section == kRestartSection)
        {
            errors.add (number, col, "label in [restart]", "a setting");
            return;
        }
        if (! validLabelName (name))
        {
            errors.add (number, col, "bad label name " + quoted (name), "a label name: [a-z][a-z0-9_]{0,15}");
            return;
        }
        endOfLine (c, number);
        auto& lane = s->lanes[section];
        for (int k = 0; k < lane.numLabels; ++k)
            if (lane.labels[k].name == name)
            {
                errors.add (number, col, "duplicate label " + quoted (name) + " (first defined at line " + std::to_string (lane.labels[k].line) + ")",
                            "a label name not used before in this lane");
                return;
            }
        if (lane.numLabels >= kMaxLabelsPerLane)
        {
            errors.add (number, col, "too many labels in lane " + std::string (laneName (section)), "at most " + std::to_string (kMaxLabelsPerLane) + " labels");
            return;
        }
        lane.labels[lane.numLabels++] = { name, lane.numRows, number, col };
    }

    void handleRow (Cursor& c, int number)
    {
        auto& lane = s->lanes[section];
        const int col = c.column();
        if (lane.numRows >= kMaxRowsPerLane)
        {
            if (! lane.rowOverflow)
                errors.add (number, col, "too many rows in lane " + std::string (laneName (section)), "at most " + std::to_string (kMaxRowsPerLane) + " rows");
            lane.rowOverflow = true;
            return;
        }
        Fail fail;
        RowOut out;
        RowParser parser (c, laneKind (section), fail);
        if (! parser.parse (out))
        {
            errors.add (number, fail.col, fail.what, fail.expected);
            return;
        }
        if (s->poolSize + out.n > kMaxCommands)
        {
            if (! s->commandOverflow)
                errors.add (number, col, "too many commands in the table", "at most " + std::to_string (kMaxCommands) + " commands");
            s->commandOverflow = true;
            return;
        }
        RowScratch& row = lane.rows[lane.numRows++];
        row.firstCmd = s->poolSize;
        row.numCmds = out.n;
        row.delay = out.delay;
        row.line = number;
        row.col = col;
        for (int k = 0; k < out.n; ++k)
            s->pool[s->poolSize++] = out.cmds[k];
        if (out.hasRef)
            s->refs[s->numRefs++] = { section, s->poolSize - 1, out.refName, number, out.refCol };
    }

    void handleSetting (Cursor& c, int number)
    {
        const int col = c.column();
        const auto key = plainToken (c);
        static constexpr const char* keys[6] = { "mode", "lead", "ad", "sr", "gateoff", "firstwave" };
        int k = -1;
        for (int i = 0; i < 6; ++i)
            if (key == keys[i])
                k = i;
        if (k < 0)
        {
            if (findCommand (key) != nullptr)
                errors.add (number, col, "command " + quoted (key) + " in [restart]", "a setting: mode, lead, ad, sr, gateoff or firstwave");
            else
                errors.add (number, col, "unknown setting " + quoted (key), "mode, lead, ad, sr, gateoff or firstwave");
            return;
        }
        if ((s->settingsSeen & (1u << k)) != 0)
        {
            errors.add (number, col, "setting " + quoted (key) + " given twice", "each setting at most once");
            return;
        }
        s->settingsSeen |= 1u << k;
        c.skipWs();
        const int vcol = c.column();
        if (c.atEol())
        {
            errors.add (number, vcol, "missing value for " + quoted (key), settingExpected (k));
            return;
        }
        const auto v = plainToken (c);
        std::uint64_t n = 0;
        bool ok = true;
        auto& r = s->restart;
        switch (k)
        {
            case 0:
                if (v == "off") r.mode = static_cast<std::uint8_t> (RestartMode::off);
                else if (v == "classic") r.mode = static_cast<std::uint8_t> (RestartMode::classic);
                else if (v == "phase") r.mode = static_cast<std::uint8_t> (RestartMode::phase);
                else ok = false;
                break;
            case 1:
            case 4:
                ok = parseDecimal (v, 8, n) == NumberResult::ok && n >= 1;
                if (ok)
                {
                    (k == 1 ? r.lead : r.gateoff) = static_cast<std::uint8_t> (n);
                    s->gateoffSet = s->gateoffSet || k == 4;
                }
                break;
            case 2:
            case 3:
                ok = v.size() == 2 && parseHex (v, 2, 255, n) == NumberResult::ok;
                if (ok)
                    (k == 2 ? r.ad : r.sr) = static_cast<std::uint8_t> (n);
                break;
            default:
            {
                size_t at = 0;
                bool dup = false;
                ok = parseControlBits (v, r.firstWave, at, dup);
                if (ok)
                    r.hasFirstWave = 1;
                break;
            }
        }
        if (! ok)
        {
            errors.add (number, vcol, "bad value " + quoted (v) + " for " + quoted (key), settingExpected (k));
            return;
        }
        endOfLine (c, number);
    }

    static const char* settingExpected (int k) noexcept
    {
        switch (k)
        {
            case 0: return "off, classic or phase";
            case 1:
            case 4: return "a number of ticks from 1 to 8";
            case 2:
            case 3: return "two hex digits like 0f";
            default: return "none, or tri, saw, pulse, noise, gate, sync, ring, test joined by '+'";
        }
    }

    void finish()
    {
        // Labels that name no row (nothing follows them in their lane).
        for (int l = 0; l < kNumLanes; ++l)
        {
            auto& lane = s->lanes[l];
            for (int k = 0; k < lane.numLabels; ++k)
                if (lane.labels[k].row >= lane.numRows)
                {
                    errors.add (lane.labels[k].line, lane.labels[k].col, "label " + quoted (lane.labels[k].name) + " names no row",
                                "a row after the label, in the same lane");
                    lane.labels[k].row = -1;
                }
        }
        // Jump targets.
        for (int k = 0; k < s->numRefs; ++k)
        {
            const auto& ref = s->refs[k];
            const auto& lane = s->lanes[ref.lane];
            int target = -2;
            for (int i = 0; i < lane.numLabels; ++i)
                if (lane.labels[i].name == ref.name)
                    target = lane.labels[i].row;
            if (target == -1)
                continue; // already reported
            if (target >= 0)
            {
                s->pool[ref.cmd].a = static_cast<std::int16_t> (target);
                continue;
            }
            int other = -1;
            for (int l = 0; l < kNumLanes && other < 0; ++l)
                if (l != ref.lane)
                    for (int i = 0; i < s->lanes[l].numLabels; ++i)
                        if (s->lanes[l].labels[i].name == ref.name)
                            other = l;
            if (other >= 0)
                errors.add (ref.line, ref.col, "jump target " + quoted (ref.name) + " is in lane " + laneName (other) + ", not in " + laneName (ref.lane),
                            "a label of the same lane");
            else
                errors.add (ref.line, ref.col, "undefined label " + quoted (ref.name), "a label defined in lane " + std::string (laneName (ref.lane)));
        }
        if (! errors.empty())
            return;

        // Emit the compact program in lane order.
        Program p = emptyProgram();
        p.restart = s->restart;
        if (! s->gateoffSet)
            p.restart.gateoff = p.restart.lead;
        int rowCursor = 0, cmdCursor = 0;
        for (int l = 0; l < kNumLanes; ++l)
        {
            const auto& lane = s->lanes[l];
            auto& li = p.lanes[l];
            li.firstRow = static_cast<std::uint16_t> (rowCursor);
            li.numRows = static_cast<std::uint8_t> (lane.numRows);
            for (int i = 0; i < lane.numLabels; ++i)
                if (lane.labels[i].name == "release")
                    li.release = static_cast<std::uint8_t> (lane.labels[i].row);
            for (int r = 0; r < lane.numRows; ++r)
            {
                const auto& row = lane.rows[r];
                auto& out = p.rows[rowCursor++];
                out.firstCmd = static_cast<std::uint16_t> (cmdCursor);
                out.numCmds = static_cast<std::uint8_t> (row.numCmds);
                out.delay = static_cast<std::uint8_t> (row.delay);
                for (int k = 0; k < row.numCmds; ++k)
                    p.cmds[cmdCursor++] = s->pool[row.firstCmd + k];
            }
        }
        p.numRows = static_cast<std::uint16_t> (rowCursor);
        p.numCmds = static_cast<std::uint16_t> (cmdCursor);

        for (int l = 0; l < kNumLanes; ++l)
        {
            const int loopRow = findZeroDelayLoop (p, l);
            if (loopRow >= 0)
            {
                const auto& row = s->lanes[l].rows[loopRow];
                errors.add (row.line, row.col, "rows with delay 0 form a loop in lane " + std::string (laneName (l)),
                            "a row with ticks of 1 or more, or an end, sweep or pingpong, in the loop");
            }
        }
        if (! errors.empty())
            return;
        result.program = p;
        result.ok = true;
    }
};
} // namespace

ReadResult readTable (std::string_view text) noexcept
{
    ReadResult result;
    try
    {
        Reader (text, result).run();
    }
    catch (...)
    {
        // Allocation failure is the only thing that can get here; report it like any other error.
        result.ok = false;
        result.program = emptyProgram();
        try
        {
            result.errors = {};
            result.errors.add (1, 1, "out of memory while reading the table", "a smaller table");
        }
        catch (...)
        {
        }
    }
    if (! result.ok)
        result.program = emptyProgram();
    return result;
}
} // namespace vs::bside::program
