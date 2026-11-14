// Review follow-ups of S1c: the canonical text always reads back (B1), no zero-time bursts (S1),
// mutually consistent file limits (S2), and the smaller fixes (messages, '#', control characters).

#include "FuzzSupport.h"
#include "RowsPerTick.h"
#include "TableTestUtil.h"

#include "bside/trace/ReferenceFormat.h"
#include "bside/trace/Sha256.h"
#include "bside/trace/TraceFormat.h"

#include <algorithm>

using namespace bst;
namespace tr = vs::bside::trace;

namespace
{
std::string repeatJoined (const std::string& item, int n, const std::string& sep)
{
    std::string s;
    for (int i = 0; i < n; ++i)
        s += (i > 0 ? sep : std::string()) + item;
    return s;
}

std::size_t longestLine (const std::string& s)
{
    std::size_t best = 0, cur = 0;
    for (const char c : s)
    {
        cur = c == '\n' ? 0 : cur + 1;
        best = std::max (best, cur);
    }
    return best;
}
} // namespace

// ---- B1: the canonical text reads back ----

TEST_CASE ("canonical text: a compact row whose canonical form is too long is rejected by the reader", "[bside][table][canonical]")
{
    SECTION ("33 x 'res 1' (197 bytes in the input, 233 canonical)")
    {
        const auto row = repeatJoined ("res 1", 33, ",");
        REQUIRE (row.size() == 197);
        const auto r = readBody ("[filter]\n" + row + "\n");
        REQUIRE_FALSE (r.ok);
        const auto& e = r.errors.stored().front();
        CHECK (e.line == 3);
        CHECK (e.column == 1);
        CHECK (e.what.find ("canonical form of this row is 233 bytes long") != std::string::npos);
    }
    SECTION ("25 x 'gate on'")
    {
        const auto r = readBody ("[wave1]\n" + repeatJoined ("gate on", 25, ",") + "\n");
        REQUIRE_FALSE (r.ok);
        CHECK (r.errors.stored().front().what.find ("227 bytes") != std::string::npos);
    }
    SECTION ("spellings that get longer: 'set 25%' -> 'set 1024', 'sweep 5 x3' -> 'sweep +5 x3'")
    {
        const auto r = readBody ("[pulse1]\n" + repeatJoined ("set 25%", 24, ",") + "\n");
        REQUIRE_FALSE (r.ok);
        CHECK (r.errors.stored().front().what.find ("canonical form of this row") != std::string::npos);
        const auto q = readBody ("[pulse1]\n" + repeatJoined ("set 25%", 19, ",") + "\n");
        INFO (q.message());
        CHECK (q.ok);
    }
    SECTION ("the largest row that fits (200 bytes) is accepted and round-trips")
    {
        // 4 + n * 5 + (n - 1) * 2 = 200 -> 28 commands is 198 bytes, 29 is 205.
        const auto ok = readBody ("[filter]\n" + repeatJoined ("res 1", 28, ",") + "\n");
        REQUIRE (ok.ok);
        std::string text;
        REQUIRE (writeTable (ok.program, text));
        CHECK (longestLine (text) == 198);
        const auto again = readTable (text);
        REQUIRE (again.ok);
        CHECK (again.program == ok.program);
        CHECK_FALSE (readBody ("[filter]\n" + repeatJoined ("res 1", 29, ",") + "\n").ok);
    }
    SECTION ("an ' ticks N' suffix counts")
    {
        // 4 + 26 * 5 + 25 * 2 = 184, + " ticks 255" (10) = 194 fits; 27 commands does not.
        CHECK (readBody ("[filter]\n" + repeatJoined ("res 1", 26, ", ") + " ticks 255\n").ok);
        CHECK_FALSE (readBody ("[filter]\n" + repeatJoined ("res 1", 27, ", ") + " ticks 255\n").ok);
    }
}

TEST_CASE ("canonical text: validate() refuses what the reader would refuse", "[bside][table][canonical]")
{
    SECTION ("a row of 100 commands")
    {
        auto p = emptyProgram();
        p.rows[0] = { 0, 100, 1 };
        for (int k = 0; k < 100; ++k)
            p.cmds[k] = { static_cast<std::uint8_t> (Op::gateOn), 0, 0, 0, 0 };
        p.lanes[0].numRows = 1;
        for (int l = 1; l < kNumLanes; ++l)
            p.lanes[l].firstRow = 1;
        p.numRows = 1;
        p.numCmds = 100;
        std::string why;
        CHECK_FALSE (validate (p, &why));
        CHECK (why.find ("canonical text of a row") != std::string::npos);
        std::string out = "x";
        CHECK_FALSE (writeTable (p, out));
        CHECK (out.empty());
    }
    SECTION ("17 distinct jump targets in one lane")
    {
        auto p = emptyProgram();
        int nr = 0, nc = 0;
        for (int k = 0; k < 17; ++k)
        {
            p.rows[nr++] = { static_cast<std::uint16_t> (nc), 1, 1 };
            p.cmds[nc++] = { static_cast<std::uint8_t> (Op::gateOn), 0, 0, 0, 0 };
        }
        for (int k = 0; k < 17; ++k)
        {
            p.rows[nr++] = { static_cast<std::uint16_t> (nc), 1, 1 };
            p.cmds[nc++] = { static_cast<std::uint8_t> (Op::jump), 0, static_cast<std::int16_t> (k), 0, 0 };
        }
        p.lanes[Wave1].numRows = static_cast<std::uint8_t> (nr);
        for (int l = 1; l < kNumLanes; ++l)
            p.lanes[l].firstRow = static_cast<std::uint16_t> (nr);
        p.numRows = static_cast<std::uint16_t> (nr);
        p.numCmds = static_cast<std::uint16_t> (nc);
        std::string why;
        CHECK_FALSE (validate (p, &why));
        CHECK (why.find ("jump targets") != std::string::npos);
        // 16 targets are fine.
        p.cmds[nc - 1].a = 0;
        CHECK (validate (p, &why));
        std::string text;
        REQUIRE (writeTable (p, text));
        const auto again = readTable (text);
        INFO (again.message());
        REQUIRE (again.ok);
        CHECK (again.program == p);
    }
    SECTION ("release counts as a label")
    {
        auto p = emptyProgram();
        int nr = 0, nc = 0;
        for (int k = 0; k < 16; ++k)
        {
            p.rows[nr++] = { static_cast<std::uint16_t> (nc), 1, 1 };
            p.cmds[nc++] = { static_cast<std::uint8_t> (Op::gateOn), 0, 0, 0, 0 };
        }
        for (int k = 0; k < 16; ++k)
        {
            p.rows[nr++] = { static_cast<std::uint16_t> (nc), 1, 1 };
            p.cmds[nc++] = { static_cast<std::uint8_t> (Op::jump), 0, static_cast<std::int16_t> (k), 0, 0 };
        }
        p.lanes[Wave1].numRows = static_cast<std::uint8_t> (nr);
        p.lanes[Wave1].release = 20; // a 17th target
        for (int l = 1; l < kNumLanes; ++l)
            p.lanes[l].firstRow = static_cast<std::uint16_t> (nr);
        p.numRows = static_cast<std::uint16_t> (nr);
        p.numCmds = static_cast<std::uint16_t> (nc);
        CHECK_FALSE (validate (p));
        p.lanes[Wave1].release = 3; // already a target
        CHECK (validate (p));
    }
}

TEST_CASE ("canonical text: every table in a large random family round-trips", "[bside][table][canonical]")
{
    // Compact rows of random commands up to the point where the canonical line would be too long.
    Rng rng (808);
    static const char* const filterCmds[] = { "res 1", "res 2", "res 3", "res 9" }; // all 5 bytes: rows of 29 to 33 commands are under 200 bytes as written and over as canonical
    int accepted = 0, rejected = 0;
    for (int i = 0; i < 300; ++i)
    {
        std::string row;
        const int n = rng.range (20, 36);
        for (int k = 0; k < n; ++k)
            row += (k > 0 ? "," : "") + std::string (filterCmds[rng.below (4)]); // no sweep in the middle
        if (row.size() > 200)
            continue; // over the input line limit as written
        const auto r = readBody ("[filter]\n" + row + "\n");
        if (! r.ok)
        {
            ++rejected;
            CHECK (r.errors.stored().front().what.find ("canonical form") != std::string::npos);
            continue;
        }
        ++accepted;
        std::string text;
        REQUIRE (writeTable (r.program, text));
        const auto again = readTable (text);
        INFO (text << again.message());
        REQUIRE (again.ok);
        CHECK (again.program == r.program);
    }
    CHECK (accepted > 20);
    CHECK (rejected > 20);
}

// ---- S1: no zero-time bursts ----

TEST_CASE ("zero-time bursts: repeat edges count in the loop check", "[bside][table][loops]")
{
    SECTION ("nested repeats 8 deep over a ticks 0 row")
    {
        std::string t = "[wave1]\na:\npitch +1 ticks 0\n";
        for (int k = 0; k < 8; ++k)
            t += "repeat a 255\n";
        t += "pitch +2\n";
        BS_EXPECT_BODY_ERROR (t, 4, 1, "delay 0 form a loop in lane wave1"); // reported at the first row of the loop
    }
    SECTION ("crossing forward and backward repeats")
    {
        BS_EXPECT_BODY_ERROR ("[wave1]\na:\nrepeat c 255\nb:\nrepeat a 255\nc:\nrepeat b 255\nend\n", 4, 1, "delay 0 form a loop");
    }
    SECTION ("a loop entered from release:")
    {
        BS_EXPECT_BODY_ERROR ("[wave1]\ngate on\nend\nrelease:\nx:\ngate off ticks 0\nrepeat x 200\nrepeat x 200\ngate on\n", 7, 1, "delay 0 form a loop");
        BS_EXPECT_BODY_ERROR ("[wave1]\ngate on\nend\nrelease:\nrepeat release 255\nend\n", 6, 1, "delay 0 form a loop");
    }
    SECTION ("a repeat to itself, and a forward repeat into a loop of zero-delay rows")
    {
        BS_EXPECT_BODY_ERROR ("[wave1]\nx:\nrepeat x 3\npitch +1\n", 4, 1, "delay 0 form a loop");
        CHECK_FALSE (readBody ("[wave1]\nrepeat y 3\nx:\npitch +1 ticks 0\njump x\ny:\nend\n").ok);
    }
    SECTION ("repeats that wait are fine")
    {
        CHECK (readBody ("[wave1]\nx:\npitch +1\nrepeat x 255\ngate on\n").ok);
        CHECK (readBody ("[wave1]\nx:\npitch +1 ticks 0\nrepeat x 255 ticks 1\ngate on\n").ok);
        CHECK (readBody ("[wave1]\nx:\npitch +1 ticks 0\ngate on ticks 0\nrepeat x 5 ticks 2\n").ok);
    }
}

TEST_CASE ("rows per tick: an accepted table runs at most 64 rows in a tick", "[bside][table][loops]")
{
    static_assert (vs::bside::program::kMaxRowsPerTick == 64);
    Rng rng (64);
    // The longest chain: 64 rows, 63 of them without delay, the last waits; entered at row 0.
    {
        std::string t = "[wave1]\n" + repeatLine ("pitch +1 ticks 0", 63) + "pitch +2\n";
        const auto r = readBody (t);
        REQUIRE (r.ok);
        CHECK (maxRowsPerTick (r.program, rng) == 64);
    }
    // Forward repeats and jumps through every row still end within 64 rows.
    {
        std::string t = "[wave1]\n";
        for (int i = 0; i < 15; ++i)
            t += "l" + std::to_string (i) + ":\nrepeat l" + std::to_string (i + 1) + " 255\npitch +1 ticks 0\n";
        t += "l15:\ngate on\n";
        const auto r = readBody (t);
        INFO (r.message());
        REQUIRE (r.ok);
        CHECK (maxRowsPerTick (r.program, rng, 12) <= 64);
    }
    // Every table of a large random family, from every start row and with random repeat counters.
    int accepted = 0;
    for (int i = 0; i < 600; ++i)
    {
        const auto r = readTable (generateTable (rng));
        if (! r.ok)
            continue;
        ++accepted;
        CHECK (maxRowsPerTick (r.program, rng) <= 64);
    }
    CHECK (accepted > 200);
}

// ---- S2: file limits agree ----

TEST_CASE ("limits: a trace at its limits has a reference the reader accepts", "[bside][trace][bounds]")
{
    using namespace tr;
    Trace t;
    t.clockHz = 985248;
    t.model = Model::B;
    t.check = 1;
    t.endCycle = 50000;
    for (std::size_t i = 0; i < kMaxWrites; ++i)
        t.writes.push_back ({ i / 2, 0x00, 0x01 });
    std::string text;
    REQUIRE (writeTrace (t, text));
    CHECK (text.size() <= kMaxTraceBytes);
    const auto back = readTrace (text);
    INFO (back.message());
    REQUIRE (back.ok);
    std::vector<Checkpoint> schedule;
    REQUIRE (checkpointSchedule (back.trace, schedule));
    CHECK (schedule.size() == kMaxCheckpoints);

    Reference ref;
    ref.traceSha256 = sha256Hex (text);
    ref.adapter = std::string (64, 'a');
    ref.emulator = std::string (64, 'b');
    for (const auto& c : schedule)
    {
        Reference::Point p;
        p.cycle = c.cycle;
        for (int k = 0; k < kStateFields; ++k)
            p.state.v[static_cast<std::size_t> (k)] = kStateFieldMax[k];
        ref.points.push_back (p);
    }
    std::string refText;
    REQUIRE (writeReference (ref, refText));
    CHECK (refText.size() <= kMaxReferenceBytes);
    const auto refBack = readReference (refText);
    INFO (refBack.message());
    REQUIRE (refBack.ok);
    std::string why;
    const std::string_view bytes (text);
    CHECK (referenceMatchesTrace (refBack.reference, back.trace, &bytes, &why));

    // One more write or periodic checkpoint is over the limits, in the reader and in the writer.
    t.writes.push_back ({ 25000, 0x00, 0x01 });
    CHECK_FALSE (writeTrace (t, text));
    t.writes.pop_back();
    t.endCycle = 50001;
    CHECK_FALSE (writeTrace (t, text));
}

TEST_CASE ("limits: the largest reference lines (13-digit cycles, 6-digit fields) fit the file limit", "[bside][ref][bounds]")
{
    using namespace tr;
    Reference ref;
    ref.traceSha256 = std::string (64, 'a');
    ref.adapter = std::string (64, 'b');
    ref.emulator = std::string (64, 'c');
    for (std::size_t i = 0; i < kMaxCheckpoints; ++i)
    {
        Reference::Point p;
        p.cycle = kMaxCycle;
        for (int k = 0; k < kStateFields; ++k)
            p.state.v[static_cast<std::size_t> (k)] = kStateFieldMax[k];
        ref.points.push_back (p);
    }
    std::string text;
    REQUIRE (writeReference (ref, text));
    CHECK (text.size() <= kMaxReferenceBytes);
    CHECK (text.size() > kMaxReferenceBytes - 800000); // the limit is nearly used: the lines really are 107 bytes
    CHECK (readReference (text).ok);
    ref.points.push_back (ref.points.back());
    CHECK_FALSE (writeReference (ref, text)); // one more checkpoint than a reader takes
    CHECK (text.empty());
}

TEST_CASE ("limits: a spec at its limits writes within the reader's file limit", "[bside][spec][bounds]")
{
    using namespace tr;
    Spec s;
    s.traceSha256 = std::string (64, 'a');
    s.frames = kSpecMaxFrames;
    s.levelTenthDb.assign (static_cast<std::size_t> (s.frames), static_cast<std::int16_t> (-1200));
    s.bandTenthDb.assign (static_cast<std::size_t> (s.frames) * kSpecBands, static_cast<std::int16_t> (-1200));
    for (int k = 0; k < kSpecMaxSegments; ++k)
    {
        SpecSegment g;
        g.name = std::string (32 - 2, 'a') + std::to_string (10 + k);
        g.firstFrame = 0;
        g.endFrame = 1;
        g.spectrum.fill (-1200);
        s.segments.push_back (g);
    }
    std::string text;
    REQUIRE (writeSpec (s, text));
    CHECK (text.size() <= kMaxSpecBytes);
    const auto back = readSpec (text);
    INFO (back.message());
    CHECK (back.ok);
}

TEST_CASE ("numbers: leading zeros are free, long digit strings still are not", "[bside][trace][text]")
{
    CHECK (tr::readTrace ("bside-trace 1\nclock 000000000000000000000985248\nmodel A\nend 00000000000000000001\n").ok);
    CHECK_FALSE (tr::readTrace ("bside-trace 1\nclock 985248\nmodel A\nend 00000000000000000000001" + std::string (40, '0') + "\n").ok);
    CHECK_FALSE (tr::readTrace ("bside-trace 1\nclock 985248\nmodel A\nend 99999999999999999999\n").ok);
    CHECK (readBody ("[filter]\ncutoff 000000000000000000000000002047\n").ok);
    CHECK_FALSE (readBody ("[filter]\ncutoff 000000000000000000000000002048\n").ok);
    CHECK (readBody ("[filter]\ncutoff 0000000000000000002047\n").ok);
    CHECK_FALSE (readBody ("[wave1]\ngate on ticks 0001\n").ok); // the contract's 1*3DIGIT stays
}

// ---- messages, '#', control characters ----

TEST_CASE ("messages: a quoted token is cut on a character boundary", "[bside][text]")
{
    using namespace vs::bside::text;
    std::string name = "a";
    for (int i = 0; i < 20; ++i)
        name += "\xc3\xa9"; // 41 bytes of e-acute
    const auto q = quoted (name);
    int l = 0, c = 0;
    const char* why = nullptr;
    CHECK_FALSE (findInvalidByte (q, l, c, why));
    CHECK (q.size() <= 24 + 6);
    CHECK (q.find ("...'") != std::string::npos);
    CHECK (quoted ("short") == "'short'");
    CHECK (quoted (std::string (24, 'x')) == "'" + std::string (24, 'x') + "'");
    CHECK (quoted (std::string (25, 'x')) == "'" + std::string (24, 'x') + "...'");
    // Three- and four-byte sequences at the cut.
    for (const std::string cp : { "\xe2\x82\xac", "\xf0\x9f\x8e\xb9" })
    {
        std::string s;
        while (s.size() < 40)
            s += cp;
        CHECK_FALSE (findInvalidByte (quoted (s), l, c, why));
        CHECK_FALSE (findInvalidByte (quoted ("x" + s), l, c, why));
        CHECK_FALSE (findInvalidByte (quoted ("xx" + s), l, c, why));
        CHECK_FALSE (findInvalidByte (quoted ("xxx" + s), l, c, why));
    }
    // And through a real message.
    const auto r = readBody ("[wave1]\n" + name + "\n");
    REQUIRE_FALSE (r.ok);
    const auto m = r.message();
    CHECK_FALSE (findInvalidByte (m, l, c, why));
}

TEST_CASE ("'#': part of a note name only directly after the note letter", "[bside][table]")
{
    const auto note = [] (const std::string& tok) { return readBody ("[wave1]\npitch " + tok + "\n"); };
    // Sharps.
    CHECK (note ("=c#4").ok);
    CHECK (note ("=C#6 # sharp").ok);
    CHECK (readBody ("[wave1]\npitch =f#3, pitch ~\n").ok);
    // Everywhere else a '#' starts a comment, consistently: the token before it is read as it is.
    CHECK (note ("=55# note").ok);
    CHECK (note ("=5# note").ok);
    CHECK (note ("=a3# note").ok);
    CHECK (note ("+5# note").ok);
    CHECK (note ("~# note").ok);
    CHECK (note ("=55# note").program.cmds[0].b == 55);
    CHECK (note ("=a3# note").program.cmds[0].b == 57);
    CHECK_FALSE (note ("=a# note").ok);  // "=a" is not a note: it needs an octave
    CHECK_FALSE (note ("=#4").ok);
    CHECK_FALSE (note ("=c##4").ok);
    CHECK (note ("=c4#4").ok); // "=c4" then a comment "#4"
}

TEST_CASE ("text: C1 controls and the byte order mark have their own messages", "[bside][text]")
{
    using namespace vs::bside::text;
    int l = 0, c = 0;
    const char* why = nullptr;
    REQUIRE (findInvalidByte ("a\xc2\x80", l, c, why));
    CHECK (std::string (why) == "C1 control character");
    CHECK (c == 2);
    CHECK (findInvalidByte ("\xc2\x85", l, c, why)); // NEL
    CHECK (findInvalidByte ("x\xc2\x9f", l, c, why));
    CHECK_FALSE (findInvalidByte ("\xc2\xa0", l, c, why)); // U+00A0 is not a control
    CHECK (findInvalidByte ("\xc2\x7f", l, c, why)); // invalid UTF-8, not C1
    REQUIRE (findInvalidByte ("\xef\xbb\xbf" "bside-table 1\n", l, c, why));
    CHECK (std::string (why) == "byte order mark (U+FEFF)");
    CHECK (l == 1);
    CHECK (c == 1);
    BS_EXPECT_ERROR (std::string ("\xef\xbb\xbf") + "bside-table 1\n", 1, 1, "byte order mark");
    BS_EXPECT_ERROR ("bside-table 1\n# \xc2\x85 comment\n", 2, 3, "C1 control character");
    const auto t = tr::readTrace ("bside-trace 1\n# \xc2\x9b\n");
    REQUIRE_FALSE (t.ok);
    CHECK (t.errors.stored().front().what.find ("C1 control character") != std::string::npos);
}
