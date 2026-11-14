#include "bside/trace/TraceFormat.h"

namespace vs::bside::trace
{
namespace
{
using namespace vs::bside::text;

const char* kHex = "0123456789abcdef";

std::string quoted (std::string_view s)
{
    return "'" + std::string (s.substr (0, 24)) + (s.size() > 24 ? "...'" : "'");
}

std::uint64_t periodicCount (const Trace& t) noexcept
{
    return (t.check > 0 && t.endCycle > 0) ? (t.endCycle - 1) / t.check : 0;
}

class Reader
{
public:
    Reader (std::string_view t, TraceResult& r) : text (t), result (r), errors (r.errors) {}

    void run()
    {
        if (text.size() > kMaxTraceBytes)
        {
            errors.add (1, 1, "trace too large (" + std::to_string (text.size()) + " bytes)", "at most " + std::to_string (kMaxTraceBytes) + " bytes");
            return;
        }
        int bl = 0, bc = 0;
        const char* why = nullptr;
        if (findInvalidByte (text, bl, bc, why))
        {
            errors.add (bl, bc, std::string ("invalid text: ") + why, "UTF-8 text without control characters");
            return;
        }
        Lines lines (text);
        std::string_view line;
        while (lines.next (line, lineNo))
        {
            const auto hash = line.find ('#');
            const auto statementBytes = hash == std::string_view::npos ? line.size() : hash;
            if (line.size() > kMaxTraceFullLineBytes || statementBytes > kMaxTraceLineBytes)
            {
                const bool whole = line.size() > kMaxTraceFullLineBytes;
                errors.add (lineNo, static_cast<int> (whole ? kMaxTraceFullLineBytes : kMaxTraceLineBytes) + 1,
                            "line too long (" + std::to_string (whole ? line.size() : statementBytes) + " bytes" + (whole ? ")" : " before the comment)"),
                            "at most " + std::to_string (whole ? kMaxTraceFullLineBytes : kMaxTraceLineBytes) + " bytes"
                                + (whole ? " per line" : " per statement"));
                continue;
            }
            if (! handleLine (line))
                break;
        }
        if (! headerSeen)
            errors.add (1, 1, "missing header", "'bside-trace 1' as the first line");
        else if (! ended && ! stopped)
            errors.add (lineNo + 1, 1, "missing 'end'", "'end <cycle>' as the last statement");
        if (errors.empty())
        {
            result.trace = std::move (trace);
            result.ok = true;
        }
    }

private:
    std::string_view text;
    TraceResult& result;
    ErrorList& errors;
    Trace trace;
    int lineNo = 0;
    bool headerSeen = false, haveClock = false, haveModel = false, haveCheck = false, ended = false, stopped = false;
    std::uint64_t lastCycle = 0;

    void endOfLine (Cursor& c)
    {
        c.skipWs();
        if (! c.atEol())
        {
            const int col = c.column();
            errors.add (lineNo, col, "unexpected " + quoted (tokenOrChar (c)), "the end of the line");
        }
    }

    /** The value token of a header word; false (error added) when missing. */
    bool value (Cursor& c, std::string_view& v, int& col, const char* expected)
    {
        c.skipWs();
        col = c.column();
        if (c.atEol())
        {
            errors.add (lineNo, col, "missing value", expected);
            return false;
        }
        v = c.token();
        return true;
    }

    bool number (std::string_view v, int col, std::uint64_t lo, std::uint64_t hi, const std::string& expected, std::uint64_t& out)
    {
        const auto r = parseDecimal (v, hi, out);
        if (r == NumberResult::ok && out >= lo)
            return true;
        errors.add (lineNo, col, r == NumberResult::notNumber ? quoted (v) + " is not a number" : "value " + quoted (v) + " is out of range", expected);
        return false;
    }

    void requireHeader()
    {
        if (! haveClock)
            errors.add (lineNo, 1, "missing 'clock' before the first write", "'clock <Hz>'");
        if (! haveModel)
            errors.add (lineNo, 1, "missing 'model' before the first write", "'model A' or 'model B'");
        headerChecked = true;
    }
    bool headerChecked = false;

    /** Returns false to stop reading (too many writes). */
    bool handleLine (std::string_view line)
    {
        Cursor c { line, 0 };
        c.skipWs();
        if (c.atEol())
            return true;
        if (! headerSeen)
        {
            headerSeen = true;
            const int col = c.column();
            if (c.token() != "bside-trace")
            {
                errors.add (lineNo, col, "missing header", "'bside-trace 1' as the first line");
                return true;
            }
            c.skipWs();
            const int vcol = c.column();
            const auto v = c.token();
            if (v.empty())
                errors.add (lineNo, vcol, "missing trace version", "1");
            else if (v != "1")
                errors.add (lineNo, vcol, "unsupported trace version " + quoted (v), "1");
            else
                endOfLine (c);
            return true;
        }
        const int col = c.column();
        if (ended)
        {
            errors.add (lineNo, col, "statement after 'end'", "nothing after the 'end' statement");
            return true;
        }
        const auto word = c.token();
        std::string_view v;
        int vcol = 0;
        std::uint64_t n = 0;
        if (word == "clock" || word == "model" || word == "check")
        {
            if (! trace.writes.empty())
            {
                errors.add (lineNo, col, "header word " + quoted (word) + " after the first write", "header words before the first write");
                return true;
            }
            if (word == "clock")
            {
                if (haveClock)
                    errors.add (lineNo, col, "'clock' given twice", "one 'clock'");
                else if (value (c, v, vcol, "a clock in Hz"))
                {
                    haveClock = true;
                    if (number (v, vcol, kMinClockHz, kMaxClockHz, "a clock from 1000 to 100000000 Hz", n))
                        trace.clockHz = static_cast<std::uint32_t> (n);
                    else
                        return true;
                    endOfLine (c);
                }
            }
            else if (word == "model")
            {
                if (haveModel)
                    errors.add (lineNo, col, "'model' given twice", "one 'model'");
                else if (value (c, v, vcol, "A or B"))
                {
                    haveModel = true;
                    if (v == "A")
                        trace.model = Model::A;
                    else if (v == "B")
                        trace.model = Model::B;
                    else
                    {
                        errors.add (lineNo, vcol, "bad model " + quoted (v), "A or B");
                        return true;
                    }
                    endOfLine (c);
                }
            }
            else
            {
                if (haveCheck)
                    errors.add (lineNo, col, "'check' given twice", "one 'check'");
                else if (value (c, v, vcol, "a number of cycles"))
                {
                    haveCheck = true;
                    if (number (v, vcol, 1, kMaxCycle, "a number of cycles from 1 to 2^40", n))
                        trace.check = n;
                    else
                        return true;
                    endOfLine (c);
                }
            }
            return true;
        }
        if (word == "end")
        {
            if (! headerChecked)
                requireHeader();
            if (! value (c, v, vcol, "the last cycle simulated"))
                return true;
            if (! number (v, vcol, 0, kMaxCycle, "a cycle from 0 to 2^40", n))
                return true;
            if (n < lastCycle)
            {
                errors.add (lineNo, vcol, "end cycle " + std::to_string (n) + " is before the last write (cycle " + std::to_string (lastCycle) + ")",
                            "an end cycle of at least " + std::to_string (lastCycle));
                return true;
            }
            trace.endCycle = n;
            if (periodicCount (trace) > kMaxPeriodicCheckpoints)
            {
                errors.add (lineNo, vcol, "'check' would give " + std::to_string (periodicCount (trace)) + " periodic checkpoints",
                            "at most " + std::to_string (kMaxPeriodicCheckpoints));
                return true;
            }
            endOfLine (c);
            ended = true;
            return true;
        }
        if (! word.empty() && isDigit (word[0]))
        {
            if (! headerChecked)
                requireHeader();
            if (trace.writes.size() >= kMaxWrites)
            {
                errors.add (lineNo, col, "too many writes", "at most " + std::to_string (kMaxWrites) + " writes");
                stopped = true;
                return false;
            }
            std::uint64_t cycle = 0;
            if (! number (word, col, 0, kMaxCycle, "a cycle from 0 to 2^40", cycle))
                return true;
            if (cycle < lastCycle)
            {
                errors.add (lineNo, col, "cycle " + std::to_string (cycle) + " is before the previous write (cycle " + std::to_string (lastCycle) + ")",
                            "cycles in ascending order");
                return true;
            }
            std::uint64_t reg = 0, val = 0;
            c.skipWs();
            const int rcol = c.column();
            const auto rt = c.token();
            if (rt.size() != 2 || parseHex (rt, 2, kMaxWritableRegister, reg) != NumberResult::ok)
            {
                errors.add (lineNo, rcol, rt.empty() ? "missing register" : "bad register " + quoted (rt), "a register as two hex digits, 00 to 18");
                return true;
            }
            c.skipWs();
            const int vcol2 = c.column();
            const auto vt = c.token();
            if (vt.size() != 2 || parseHex (vt, 2, 255, val) != NumberResult::ok)
            {
                errors.add (lineNo, vcol2, vt.empty() ? "missing value" : "bad value " + quoted (vt), "a value as two hex digits, 00 to ff");
                return true;
            }
            c.skipWs();
            if (! c.atEol())
            {
                {
                const int ucol = c.column();
                errors.add (lineNo, ucol, "unexpected " + quoted (tokenOrChar (c)), "the end of the line");
            }
                return true;
            }
            lastCycle = cycle;
            trace.writes.push_back ({ cycle, static_cast<std::uint8_t> (reg), static_cast<std::uint8_t> (val) });
            return true;
        }
        errors.add (lineNo, col, "unknown statement " + quoted (word.empty() ? line.substr (c.i, 1) : word),
                    "clock, model, check, a write like '0 18 0f', or end");
        return true;
    }
};
} // namespace

TraceResult readTrace (std::string_view text) noexcept
{
    TraceResult result;
    try
    {
        Reader (text, result).run();
    }
    catch (...)
    {
        result.ok = false;
        result.trace = {};
        try
        {
            result.errors = {};
            result.errors.add (1, 1, "out of memory while reading the trace", "a smaller trace");
        }
        catch (...)
        {
        }
    }
    if (! result.ok)
        result.trace = {};
    return result;
}

bool validateTrace (const Trace& t, std::string* why)
{
    const auto fail = [&] (const char* m) {
        if (why != nullptr)
            *why = m;
        return false;
    };
    if (t.clockHz < kMinClockHz || t.clockHz > kMaxClockHz) return fail ("clock out of range");
    if (t.check > kMaxCycle) return fail ("check out of range");
    if (t.writes.size() > kMaxWrites) return fail ("too many writes");
    std::uint64_t last = 0;
    for (const auto& w : t.writes)
    {
        if (w.reg > kMaxWritableRegister) return fail ("register not writable");
        if (w.cycle > kMaxCycle) return fail ("cycle out of range");
        if (w.cycle < last) return fail ("cycles not ascending");
        last = w.cycle;
    }
    if (t.endCycle > kMaxCycle || t.endCycle < last) return fail ("end cycle out of range or before the last write");
    if (periodicCount (t) > kMaxPeriodicCheckpoints) return fail ("too many periodic checkpoints");
    return true;
}

bool writeTrace (const Trace& t, std::string& out)
{
    out.clear();
    if (! validateTrace (t))
        return false;
    std::string s = "bside-trace 1\nclock " + std::to_string (t.clockHz) + "\nmodel " + (t.model == Model::A ? "A" : "B") + "\n";
    if (t.check != 0)
        s += "check " + std::to_string (t.check) + "\n";
    s.reserve (s.size() + t.writes.size() * 16 + 32);
    for (const auto& w : t.writes)
    {
        s += std::to_string (w.cycle);
        s += ' ';
        s += kHex[w.reg >> 4];
        s += kHex[w.reg & 15];
        s += ' ';
        s += kHex[w.value >> 4];
        s += kHex[w.value & 15];
        s += '\n';
    }
    s += "end " + std::to_string (t.endCycle) + "\n";
    out = std::move (s);
    return true;
}

bool checkpointSchedule (const Trace& t, std::vector<Checkpoint>& out)
{
    out.clear();
    if (! validateTrace (t))
        return false;
    const std::uint64_t periodic = periodicCount (t);
    out.reserve (t.writes.size() + static_cast<std::size_t> (periodic) + 1);
    std::uint64_t next = 1; // index of the next periodic checkpoint (cycle = next * check)
    const auto emitPeriodicUpTo = [&] (std::uint64_t cycle) {
        while (next <= periodic && next * t.check <= cycle)
        {
            out.push_back ({ Checkpoint::Kind::periodic, next * t.check, 0 });
            ++next;
        }
    };
    for (std::size_t i = 0; i < t.writes.size(); ++i)
    {
        emitPeriodicUpTo (t.writes[i].cycle);
        out.push_back ({ Checkpoint::Kind::afterWrite, t.writes[i].cycle, static_cast<std::uint32_t> (i) });
    }
    emitPeriodicUpTo (t.endCycle);
    out.push_back ({ Checkpoint::Kind::end, t.endCycle, 0 });
    return true;
}
} // namespace vs::bside::trace
