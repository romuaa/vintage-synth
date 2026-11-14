#include "bside/trace/ReferenceFormat.h"

#include "bside/trace/Sha256.h"

namespace vs::bside::trace
{
const char* const kStateFieldNames[kStateFields] = { "acc1",   "acc2",   "acc3",   "noise1", "noise2", "noise3", "env1",
                                                      "env2",   "env3",   "phase1", "phase2", "phase3", "rate1",  "rate2",
                                                      "rate3",  "div1",   "div2",   "div3",   "rd1B",   "rd1C" };
const std::uint32_t kStateFieldMax[kStateFields] = { 0xffffff, 0xffffff, 0xffffff, 0x7fffff, 0x7fffff, 0x7fffff, 0xff,
                                                      0xff,     0xff,     2,        2,        2,        0x7fff,   0x7fff,
                                                      0x7fff,   0xff,     0xff,     0xff,     0xff,     0xff };

namespace
{
using namespace vs::bside::text;

const char* kHex = "0123456789abcdef";

bool isLowerHexString (const std::string& s, std::size_t a, std::size_t b)
{
    if (s.size() != a && s.size() != b)
        return false;
    for (const char c : s)
        if (! (isDigit (c) || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

int hexWidth (std::uint32_t maxValue) noexcept
{
    int w = 1;
    while (maxValue > 15)
    {
        maxValue >>= 4;
        ++w;
    }
    return w;
}

void appendHex (std::string& s, std::uint32_t v, int width)
{
    for (int i = width - 1; i >= 0; --i)
        s += kHex[(v >> (4 * i)) & 15u];
}

bool validName (std::string_view n) noexcept
{
    if (n.empty() || n.size() > 32 || n[0] < 'a' || n[0] > 'z')
        return false;
    for (const char c : n)
        if (! ((c >= 'a' && c <= 'z') || isDigit (c) || c == '_'))
            return false;
    return true;
}

/** Shared line loop and helpers of the two readers. */
class Base
{
public:
    Base (std::string_view t, ErrorList& e) : text (t), errors (e) {}

protected:
    std::string_view text;
    ErrorList& errors;
    int lineNo = 0;

    /** Size and byte checks; false when the text is rejected (error added). */
    bool precheck (std::size_t maxBytes, const char* what)
    {
        if (text.size() > maxBytes)
        {
            errors.add (1, 1, std::string (what) + " too large (" + std::to_string (text.size()) + " bytes)", "at most " + std::to_string (maxBytes) + " bytes");
            return false;
        }
        int bl = 0, bc = 0;
        const char* why = nullptr;
        if (findInvalidByte (text, bl, bc, why))
        {
            errors.add (bl, bc, std::string ("invalid text: ") + why, "UTF-8 text without control characters");
            return false;
        }
        return true;
    }

    void endOfLine (Cursor& c)
    {
        c.skipWs();
        if (! c.atEol())
        {
            const int col = c.column();
            errors.add (lineNo, col, "unexpected " + quoted (tokenOrChar (c)), "the end of the line");
        }
    }

    /** Next token on the line; false (error added) when missing. */
    bool nextToken (Cursor& c, std::string_view& tok, int& col, const char* expected)
    {
        c.skipWs();
        col = c.column();
        if (c.atEol())
        {
            errors.add (lineNo, col, "missing value", expected);
            return false;
        }
        tok = c.token();
        return true;
    }

    bool decimal (std::string_view tok, int col, std::uint64_t hi, const std::string& expected, std::uint64_t& out)
    {
        const auto r = parseDecimal (tok, hi, out);
        if (r == NumberResult::ok)
            return true;
        errors.add (lineNo, col, r == NumberResult::notNumber ? quoted (tok) + " is not a number" : "value " + quoted (tok) + " is out of range", expected);
        return false;
    }

    /** A signed decimal integer in [lo, hi]. */
    bool signedInt (std::string_view tok, int col, int lo, int hi, const std::string& expected, int& out)
    {
        std::uint64_t v = 0;
        const bool neg = ! tok.empty() && tok[0] == '-';
        const auto r = parseDecimal (neg ? tok.substr (1) : tok, 99999, v);
        if (r == NumberResult::notNumber)
        {
            errors.add (lineNo, col, quoted (tok) + " is not a number", expected);
            return false;
        }
        const long value = neg ? -static_cast<long> (v) : static_cast<long> (v);
        if (r == NumberResult::outOfRange || value < lo || value > hi)
        {
            errors.add (lineNo, col, "value " + quoted (tok) + " is out of range", expected);
            return false;
        }
        out = static_cast<int> (value);
        return true;
    }

    bool hash (std::string_view tok, int col, std::size_t a, std::size_t b, std::string& out)
    {
        std::string s (tok);
        for (auto& ch : s)
            if (ch >= 'A' && ch <= 'F')
                ch = static_cast<char> (ch | 0x20);
        if (! isLowerHexString (s, a, b))
        {
            errors.add (lineNo, col, "bad hash " + quoted (tok),
                        a == b ? std::to_string (a) + " hex digits" : std::to_string (a) + " or " + std::to_string (b) + " hex digits");
            return false;
        }
        out = std::move (s);
        return true;
    }
};

// ---- .bsref ----
class RefReader : Base
{
public:
    RefReader (std::string_view t, ReferenceResult& r) : Base (t, r.errors), result (r) {}

    void run()
    {
        if (! precheck (kMaxReferenceBytes, "reference file"))
            return;
        Lines lines (text);
        std::string_view line;
        while (lines.next (line, lineNo))
        {
            if (line.size() > kMaxReferenceLineBytes)
            {
                errors.add (lineNo, static_cast<int> (kMaxReferenceLineBytes) + 1, "line too long (" + std::to_string (line.size()) + " bytes)",
                            "at most " + std::to_string (kMaxReferenceLineBytes) + " bytes");
                continue;
            }
            if (! handleLine (line))
                break;
        }
        if (! headerSeen)
            errors.add (1, 1, "missing header", "'bside-ref 1' as the first line");
        else if (stage < 3)
            errors.add (lineNo + 1, 1, "missing header words", "trace-sha256, adapter and emulator before the first state line");
        if (errors.empty())
        {
            result.reference = std::move (ref);
            result.ok = true;
        }
    }

private:
    ReferenceResult& result;
    Reference ref;
    bool headerSeen = false;
    int stage = 0; // header words read: 0 none, 1 sha, 2 adapter, 3 emulator
    std::uint64_t lastCycle = 0;

    bool handleLine (std::string_view line)
    {
        Cursor c { line, 0 };
        c.skipWs();
        if (c.atEol())
            return true;
        const int col = c.column();
        const auto word = c.token();
        if (! headerSeen)
        {
            headerSeen = true;
            if (word != "bside-ref")
            {
                errors.add (lineNo, col, "missing header", "'bside-ref 1' as the first line");
                return true;
            }
            c.skipWs();
            const int vcol = c.column();
            const auto v = c.token();
            if (v.empty())
                errors.add (lineNo, vcol, "missing reference version", "1");
            else if (v != "1")
                errors.add (lineNo, vcol, "unsupported reference version " + quoted (v), "1");
            else
                endOfLine (c);
            return true;
        }
        std::string_view v;
        int vcol = 0;
        if (word == "trace-sha256" || word == "adapter" || word == "emulator")
        {
            const int want = word == "trace-sha256" ? 0 : word == "adapter" ? 1 : 2;
            if (stage != want)
            {
                errors.add (lineNo, col, "header word " + quoted (word) + " out of order or repeated", "trace-sha256, adapter, emulator, each once, in this order");
                return true;
            }
            if (! nextToken (c, v, vcol, "a hash"))
                return true;
            std::string h;
            const bool ok = want == 0 ? hash (v, vcol, 64, 64, h) : hash (v, vcol, 40, 64, h);
            if (! ok)
                return true;
            (want == 0 ? ref.traceSha256 : want == 1 ? ref.adapter : ref.emulator) = std::move (h);
            ++stage;
            endOfLine (c);
            return true;
        }
        if (word != "state")
        {
            errors.add (lineNo, col, "unknown statement " + quoted (word.empty() ? line.substr (c.i, 1) : word),
                        "trace-sha256, adapter, emulator or a 'state' line");
            return true;
        }
        if (stage < 3)
        {
            errors.add (lineNo, col, "'state' before the header words", "trace-sha256, adapter and emulator first");
            return true;
        }
        if (ref.points.size() >= kMaxCheckpoints)
        {
            errors.add (lineNo, col, "too many checkpoints", "at most " + std::to_string (kMaxCheckpoints));
            return false;
        }
        Reference::Point pt;
        if (! nextToken (c, v, vcol, "a cycle") || ! decimal (v, vcol, kMaxCycle, "a cycle from 0 to 2^40", pt.cycle))
            return true;
        if (pt.cycle < lastCycle)
        {
            errors.add (lineNo, vcol, "cycle " + std::to_string (pt.cycle) + " is before the previous checkpoint (cycle " + std::to_string (lastCycle) + ")",
                        "cycles in ascending order");
            return true;
        }
        for (int k = 0; k < kStateFields; ++k)
        {
            if (! nextToken (c, v, vcol, "20 hex fields (see the format description)"))
                return true;
            std::uint64_t n = 0;
            const auto r = parseHex (v, static_cast<std::size_t> (hexWidth (kStateFieldMax[k])), kStateFieldMax[k], n);
            if (r != NumberResult::ok)
            {
                errors.add (lineNo, vcol, r == NumberResult::notNumber ? std::string (kStateFieldNames[k]) + ": " + quoted (v) + " is not hex"
                                                                       : std::string (kStateFieldNames[k]) + ": value " + quoted (v) + " is out of range",
                            "hex digits, at most " + std::to_string (hexWidth (kStateFieldMax[k])) + ", value at most " + std::to_string (kStateFieldMax[k]));
                return true;
            }
            pt.state.v[static_cast<std::size_t> (k)] = static_cast<std::uint32_t> (n);
        }
        c.skipWs();
        if (! c.atEol())
        {
            {
                const int ucol = c.column();
                errors.add (lineNo, ucol, "unexpected " + quoted (tokenOrChar (c)), "the end of the line (20 fields)");
            }
            return true;
        }
        lastCycle = pt.cycle;
        ref.points.push_back (pt);
        return true;
    }
};

// ---- .bsspec ----
class SpecReader : Base
{
public:
    SpecReader (std::string_view t, SpecResult& r) : Base (t, r.errors), result (r) {}

    void run()
    {
        if (! precheck (kMaxSpecBytes, "spec file"))
            return;
        Lines lines (text);
        std::string_view line;
        while (lines.next (line, lineNo))
        {
            if (line.size() > kMaxSpecLineBytes)
            {
                errors.add (lineNo, static_cast<int> (kMaxSpecLineBytes) + 1, "line too long (" + std::to_string (line.size()) + " bytes)",
                            "at most " + std::to_string (kMaxSpecLineBytes) + " bytes");
                continue;
            }
            handleLine (line);
        }
        if (! headerSeen)
            errors.add (1, 1, "missing header", "'bside-spec 1' as the first line");
        else if (stage < 4)
            errors.add (lineNo + 1, 1, "missing header words", "trace-sha256, rate, bands and frames");
        else if (framesRead != spec.frames && errors.empty())
            errors.add (lineNo + 1, 1, "only " + std::to_string (framesRead) + " frame lines", "exactly " + std::to_string (spec.frames));
        if (errors.empty())
        {
            result.spec = std::move (spec);
            result.ok = true;
        }
    }

private:
    SpecResult& result;
    Spec spec;
    bool headerSeen = false;
    int stage = 0; // 1 sha, 2 rate, 3 bands, 4 frames
    int framesRead = 0;
    bool segmentsStarted = false;

    void handleLine (std::string_view line)
    {
        Cursor c { line, 0 };
        c.skipWs();
        if (c.atEol())
            return;
        const int col = c.column();
        const auto word = c.token();
        if (! headerSeen)
        {
            headerSeen = true;
            if (word != "bside-spec")
            {
                errors.add (lineNo, col, "missing header", "'bside-spec 1' as the first line");
                return;
            }
            c.skipWs();
            const int vcol = c.column();
            const auto v = c.token();
            if (v.empty())
                errors.add (lineNo, vcol, "missing spec version", "1");
            else if (v != "1")
                errors.add (lineNo, vcol, "unsupported spec version " + quoted (v), "1");
            else
                endOfLine (c);
            return;
        }
        std::string_view v;
        int vcol = 0;
        std::uint64_t n = 0;
        if (word == "trace-sha256" || word == "rate" || word == "bands" || word == "frames")
        {
            const int want = word == "trace-sha256" ? 0 : word == "rate" ? 1 : word == "bands" ? 2 : 3;
            if (stage != want)
            {
                errors.add (lineNo, col, "header word " + quoted (word) + " out of order or repeated", "trace-sha256, rate, bands, frames, each once, in this order");
                return;
            }
            if (! nextToken (c, v, vcol, "a value"))
                return;
            bool ok = false;
            if (want == 0)
                ok = hash (v, vcol, 64, 64, spec.traceSha256);
            else if (want == 1)
                ok = decimal (v, vcol, 1000000, "48000", n) && (n == 48000 || (errors.add (lineNo, vcol, "unsupported sample rate " + quoted (v), "48000"), false));
            else if (want == 2)
                ok = decimal (v, vcol, 1000, "23", n) && (n == static_cast<std::uint64_t> (kSpecBands) || (errors.add (lineNo, vcol, "unsupported band count " + quoted (v), "23"), false));
            else
            {
                ok = decimal (v, vcol, 1u << 30, "a frame count from 1 to 100000", n);
                if (ok && (n < 1 || n > static_cast<std::uint64_t> (kSpecMaxFrames)))
                {
                    errors.add (lineNo, vcol, "frame count " + quoted (v) + " is out of range", "a frame count from 1 to 100000");
                    ok = false;
                }
                if (ok)
                    spec.frames = static_cast<int> (n);
            }
            if (! ok)
                return;
            ++stage;
            endOfLine (c);
            return;
        }
        if (stage < 4)
        {
            errors.add (lineNo, col, "statement before the header words", "trace-sha256, rate, bands and frames first");
            return;
        }
        if (word == "frame")
        {
            if (segmentsStarted)
            {
                errors.add (lineNo, col, "'frame' after a 'segment'", "all frames before the segments");
                return;
            }
            if (framesRead >= spec.frames)
            {
                errors.add (lineNo, col, "more frame lines than 'frames' says", "exactly " + std::to_string (spec.frames) + " frame lines");
                return;
            }
            int vals[1 + kSpecBands] = {};
            const std::string expected = "a value from -1200 to 300 (0.1 dB)";
            for (int k = 0; k < 1 + kSpecBands; ++k)
                if (! nextToken (c, v, vcol, "a level and 23 band values") || ! signedInt (v, vcol, kSpecMinTenthDb, kSpecMaxTenthDb, expected, vals[k]))
                    return;
            c.skipWs();
            if (! c.atEol())
            {
                {
                const int ucol = c.column();
                errors.add (lineNo, ucol, "unexpected " + quoted (tokenOrChar (c)), "the end of the line (24 values)");
            }
                return;
            }
            spec.levelTenthDb.push_back (static_cast<std::int16_t> (vals[0]));
            for (int k = 1; k <= kSpecBands; ++k)
                spec.bandTenthDb.push_back (static_cast<std::int16_t> (vals[k]));
            ++framesRead;
            return;
        }
        if (word == "segment")
        {
            if (framesRead != spec.frames)
            {
                errors.add (lineNo, col, "'segment' before all " + std::to_string (spec.frames) + " frames", "every frame line first");
                return;
            }
            segmentsStarted = true;
            if (static_cast<int> (spec.segments.size()) >= kSpecMaxSegments)
            {
                errors.add (lineNo, col, "too many segments", "at most " + std::to_string (kSpecMaxSegments));
                return;
            }
            SpecSegment seg;
            if (! nextToken (c, v, vcol, "a segment name"))
                return;
            if (! validName (v))
            {
                errors.add (lineNo, vcol, "bad segment name " + quoted (v), "a name: [a-z][a-z0-9_]{0,31}");
                return;
            }
            for (const auto& other : spec.segments)
                if (other.name == v)
                {
                    errors.add (lineNo, vcol, "duplicate segment " + quoted (v), "a name not used before");
                    return;
                }
            seg.name = std::string (v);
            std::uint64_t f0 = 0, f1 = 0;
            if (! nextToken (c, v, vcol, "a first frame") || ! decimal (v, vcol, static_cast<std::uint64_t> (spec.frames), "a frame number", f0))
                return;
            if (! nextToken (c, v, vcol, "an end frame") || ! decimal (v, vcol, static_cast<std::uint64_t> (spec.frames), "a frame number up to 'frames'", f1))
                return;
            if (f0 >= f1)
            {
                errors.add (lineNo, vcol, "empty or reversed frame range", "an end frame above the first frame");
                return;
            }
            seg.firstFrame = static_cast<std::uint32_t> (f0);
            seg.endFrame = static_cast<std::uint32_t> (f1);
            const std::string expected = "a value from -1200 to 300 (0.1 dB)";
            for (int k = 0; k < kSpecBins; ++k)
            {
                int val = 0;
                if (! nextToken (c, v, vcol, "120 spectrum values") || ! signedInt (v, vcol, kSpecMinTenthDb, kSpecMaxTenthDb, expected, val))
                    return;
                seg.spectrum[static_cast<std::size_t> (k)] = static_cast<std::int16_t> (val);
            }
            c.skipWs();
            if (! c.atEol())
            {
                {
                const int ucol = c.column();
                errors.add (lineNo, ucol, "unexpected " + quoted (tokenOrChar (c)), "the end of the line (120 values)");
            }
                return;
            }
            spec.segments.push_back (std::move (seg));
            return;
        }
        errors.add (lineNo, col, "unknown statement " + quoted (word.empty() ? line.substr (c.i, 1) : word), "'frame' or 'segment'");
    }
};
} // namespace

ReferenceResult readReference (std::string_view text) noexcept
{
    ReferenceResult result;
    try
    {
        RefReader (text, result).run();
    }
    catch (...)
    {
        result.ok = false;
        result.reference = {};
        try
        {
            result.errors = {};
            result.errors.add (1, 1, "out of memory while reading the reference file", "a smaller file");
        }
        catch (...)
        {
        }
    }
    if (! result.ok)
        result.reference = {};
    return result;
}

SpecResult readSpec (std::string_view text) noexcept
{
    SpecResult result;
    try
    {
        SpecReader (text, result).run();
    }
    catch (...)
    {
        result.ok = false;
        result.spec = {};
        try
        {
            result.errors = {};
            result.errors.add (1, 1, "out of memory while reading the spec file", "a smaller file");
        }
        catch (...)
        {
        }
    }
    if (! result.ok)
        result.spec = {};
    return result;
}

bool validateReference (const Reference& r, std::string* why)
{
    const auto fail = [&] (const char* m) {
        if (why != nullptr)
            *why = m;
        return false;
    };
    if (! isLowerHexString (r.traceSha256, 64, 64)) return fail ("trace hash is not 64 lower-case hex digits");
    if (! isLowerHexString (r.adapter, 40, 64) || ! isLowerHexString (r.emulator, 40, 64)) return fail ("adapter or emulator hash is not 40 or 64 lower-case hex digits");
    if (r.points.size() > kMaxCheckpoints) return fail ("too many checkpoints");
    std::uint64_t last = 0;
    for (const auto& p : r.points)
    {
        if (p.cycle > kMaxCycle || p.cycle < last) return fail ("checkpoint cycles out of range or not ascending");
        last = p.cycle;
        for (int k = 0; k < kStateFields; ++k)
            if (p.state.v[static_cast<std::size_t> (k)] > kStateFieldMax[k])
                return fail ("a state field is out of range");
    }
    return true;
}

bool writeReference (const Reference& r, std::string& out)
{
    out.clear();
    if (! validateReference (r))
        return false;
    std::string s = "bside-ref 1\ntrace-sha256 " + r.traceSha256 + "\nadapter " + r.adapter + "\nemulator " + r.emulator + "\n";
    for (const auto& p : r.points)
    {
        s += "state " + std::to_string (p.cycle);
        for (int k = 0; k < kStateFields; ++k)
        {
            s += ' ';
            appendHex (s, p.state.v[static_cast<std::size_t> (k)], hexWidth (kStateFieldMax[k]));
        }
        s += '\n';
    }
    if (s.size() > kMaxReferenceBytes)
        return false; // the reader would refuse it
    out = std::move (s);
    return true;
}

bool referenceMatchesTrace (const Reference& r, const Trace& t, const std::string_view* traceBytes, std::string* why)
{
    const auto fail = [&] (std::string m) {
        if (why != nullptr)
            *why = std::move (m);
        return false;
    };
    if (traceBytes != nullptr && sha256Hex (*traceBytes) != r.traceSha256)
        return fail ("the reference was generated from a different trace (SHA-256 differs)");
    std::vector<Checkpoint> schedule;
    if (! checkpointSchedule (t, schedule))
        return fail ("the trace is not valid");
    if (schedule.size() != r.points.size())
        return fail ("the reference has " + std::to_string (r.points.size()) + " checkpoints, the trace needs " + std::to_string (schedule.size()));
    for (std::size_t i = 0; i < schedule.size(); ++i)
        if (schedule[i].cycle != r.points[i].cycle)
            return fail ("checkpoint " + std::to_string (i) + " is at cycle " + std::to_string (r.points[i].cycle) + ", the trace needs cycle "
                         + std::to_string (schedule[i].cycle));
    return true;
}

bool validateSpec (const Spec& s, std::string* why)
{
    const auto fail = [&] (const char* m) {
        if (why != nullptr)
            *why = m;
        return false;
    };
    if (! isLowerHexString (s.traceSha256, 64, 64)) return fail ("trace hash is not 64 lower-case hex digits");
    if (s.frames < 1 || s.frames > kSpecMaxFrames) return fail ("frame count out of range");
    if (s.levelTenthDb.size() != static_cast<std::size_t> (s.frames) || s.bandTenthDb.size() != static_cast<std::size_t> (s.frames) * kSpecBands)
        return fail ("level or band count does not match the frame count");
    const auto okVal = [] (int v) { return v >= kSpecMinTenthDb && v <= kSpecMaxTenthDb; };
    for (const auto v : s.levelTenthDb)
        if (! okVal (v)) return fail ("level out of range");
    for (const auto v : s.bandTenthDb)
        if (! okVal (v)) return fail ("band value out of range");
    if (static_cast<int> (s.segments.size()) > kSpecMaxSegments) return fail ("too many segments");
    for (std::size_t i = 0; i < s.segments.size(); ++i)
    {
        const auto& g = s.segments[i];
        if (! validName (g.name)) return fail ("bad segment name");
        for (std::size_t j = 0; j < i; ++j)
            if (s.segments[j].name == g.name) return fail ("duplicate segment name");
        if (g.firstFrame >= g.endFrame || g.endFrame > static_cast<std::uint32_t> (s.frames)) return fail ("segment frame range out of range");
        for (const auto v : g.spectrum)
            if (! okVal (v)) return fail ("spectrum value out of range");
    }
    return true;
}

bool writeSpec (const Spec& s, std::string& out)
{
    out.clear();
    if (! validateSpec (s))
        return false;
    std::string o = "bside-spec 1\ntrace-sha256 " + s.traceSha256 + "\nrate 48000\nbands " + std::to_string (kSpecBands) + "\nframes " + std::to_string (s.frames) + "\n";
    for (int f = 0; f < s.frames; ++f)
    {
        o += "frame " + std::to_string (s.levelTenthDb[static_cast<std::size_t> (f)]);
        for (int b = 0; b < kSpecBands; ++b)
            o += " " + std::to_string (s.band (f, b));
        o += '\n';
    }
    for (const auto& g : s.segments)
    {
        o += "segment " + g.name + " " + std::to_string (g.firstFrame) + " " + std::to_string (g.endFrame);
        for (const auto v : g.spectrum)
            o += " " + std::to_string (v);
        o += '\n';
    }
    if (o.size() > kMaxSpecBytes)
        return false; // the reader would refuse it
    out = std::move (o);
    return true;
}
} // namespace vs::bside::trace
