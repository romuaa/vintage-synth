// Every limit of the table format (docs/architecture.md 8.6.5): at the limit it is accepted, one
// above it is rejected with a position. Also the size of the compiled program (8.6.7).

#include "TableTestUtil.h"

#include <algorithm>

using namespace bst;

namespace
{
/** Pads `text` with comment lines (and at most one empty line) to exactly n bytes. */
std::string padTo (std::string text, std::size_t n)
{
    REQUIRE (text.size() <= n);
    while (text.size() < n)
    {
        const auto remaining = n - text.size();
        const auto chunk = std::min<std::size_t> (remaining, 100);
        if (chunk == 1)
            text += "\n";
        else
            text += "#" + std::string (chunk - 2, 'x') + "\n";
    }
    return text;
}
} // namespace

TEST_CASE ("table bounds: text size 32 KB", "[bside][table][bounds]")
{
    const auto atLimit = padTo ("bside-table 1\n[wave1]\ngate on\n", 32768);
    REQUIRE (atLimit.size() == 32768);
    // 330 lines: well inside the line limit.
    CHECK (readTable (atLimit).ok);
    const auto tooBig = padTo ("bside-table 1\n[wave1]\ngate on\n", 32769);
    BS_EXPECT_ERROR (tooBig, 1, 1, "table too large");
    const auto r = readTable (tooBig);
    CHECK (r.errors.total() == 1); // rejected before any parsing
}

TEST_CASE ("table bounds: 1024 lines", "[bside][table][bounds]")
{
    std::string t = "bside-table 1\n" + repeatLine ("# c", 1023);
    CHECK (readTable (t).ok);
    t += "# one too many\n";
    BS_EXPECT_ERROR (t, 1025, 1, "too many lines");
    // Without a final newline the last line still counts.
    std::string u = "bside-table 1\n" + repeatLine ("# c", 1022) + "# last";
    CHECK (readTable (u).ok);
    u += "\n#x\n";
    BS_EXPECT_ERROR (u, 1025, 1, "too many lines");
}

TEST_CASE ("table bounds: 200 bytes per line", "[bside][table][bounds]")
{
    const std::string ok = "bside-table 1\n#" + std::string (199, 'x') + "\n";
    CHECK (readTable (ok).ok);
    const std::string bad = "bside-table 1\n#" + std::string (200, 'x') + "\n[wave1]\nfoo\n";
    BS_EXPECT_ERROR (bad, 2, 201, "line too long");
    // The long line is skipped; the rest is still read and its errors reported.
    const auto r = readTable (bad);
    CHECK (r.errors.total() == 2);
    // CRLF does not count towards the 200.
    const std::string crlf = "bside-table 1\r\n#" + std::string (199, 'x') + "\r\n";
    CHECK (readTable (crlf).ok);
    // The header line is also bounded.
    BS_EXPECT_ERROR ("bside-table 1 #" + std::string (190, 'x') + "\n", 1, 201, "line too long");
}

TEST_CASE ("table bounds: 64 rows per lane", "[bside][table][bounds]")
{
    for (const char* lane : { "wave1", "pulse2", "filter" })
    {
        const std::string row = lane[0] == 'w' ? "gate on" : lane[0] == 'p' ? "set 1" : "res 1";
        const auto ok = std::string ("bside-table 1\n[") + lane + "]\n" + repeatLine (row, 64);
        const auto r = readTable (ok);
        INFO (lane << ": " << r.message());
        REQUIRE (r.ok);
        CHECK (r.program.numRows == 64);
        const auto bad = ok + row + "\n" + row + "\n";
        // Reported once per lane, at the 65th row.
        const auto b = readTable (bad);
        REQUIRE_FALSE (b.ok);
        CHECK (b.errors.total() == 1);
        CHECK (b.errors.stored()[0].line == 2 + 64 + 1);
        CHECK (b.errors.stored()[0].column == 1);
        CHECK (b.errors.stored()[0].what.find ("too many rows") != std::string::npos);
    }
    // Labels and comments are not rows.
    CHECK (readBody ("[wave1]\nx:\n" + repeatLine ("gate on", 64) + "# comment\n").ok);
}

TEST_CASE ("table bounds: 16 labels per lane", "[bside][table][bounds]")
{
    std::string ok = "[wave1]\n";
    for (int i = 0; i < 16; ++i)
        ok += "l" + std::to_string (i) + ":\ngate on\n";
    CHECK (readBody (ok).ok);
    const auto bad = ok + "l16:\ngate on\n";
    BS_EXPECT_BODY_ERROR (bad, 35, 1, "too many labels");
    // The limit is per lane.
    std::string two = ok + "[wave2]\n";
    for (int i = 0; i < 16; ++i)
        two += "l" + std::to_string (i) + ":\ngate on\n";
    CHECK (readBody (two).ok);
    // Two labels on one row both count.
    std::string dup = "[wave1]\n";
    for (int i = 0; i < 17; ++i)
        dup += "l" + std::to_string (i) + ":\n";
    dup += "gate on\n";
    const auto r = readBody (dup);
    REQUIRE_FALSE (r.ok);
    CHECK (r.errors.stored()[0].what.find ("too many labels") != std::string::npos);
}

TEST_CASE ("table bounds: label names", "[bside][table][bounds]")
{
    CHECK (readBody ("[wave1]\naaaaaaaaaaaaaaaa:\ngate on\njump aaaaaaaaaaaaaaaa\n").ok); // 16 characters
    CHECK (readBody ("[wave1]\na_9:\ngate on\n").ok);
    BS_EXPECT_BODY_ERROR ("[wave1]\naaaaaaaaaaaaaaaaa:\ngate on\n", 3, 1, "bad label name");
    BS_EXPECT_BODY_ERROR ("[wave1]\n_a:\ngate on\n", 3, 1, "bad label name");
}

TEST_CASE ("table bounds: 512 commands in the table", "[bside][table][bounds]")
{
    // wave1 and wave2: 64 rows of 3 commands; wave3: 64 rows of 2 commands: exactly 512.
    const auto three = repeatLine ("gate on, gate off, pitch ~", 64);
    std::string t = "[wave1]\n" + three + "[wave2]\n" + three + "[wave3]\n" + repeatLine ("gate on, pitch ~", 64);
    const auto r = readBody (t);
    INFO (r.message());
    REQUIRE (r.ok);
    CHECK (r.program.numCmds == 512);
    CHECK (r.program.numRows == 192);
    t += "[pulse1]\nset 1\n";
    const auto bad = readBody (t);
    REQUIRE_FALSE (bad.ok);
    CHECK (bad.errors.total() == 1);
    CHECK (bad.errors.stored()[0].line == 198);
    CHECK (bad.errors.stored()[0].what.find ("too many commands") != std::string::npos);
}

TEST_CASE ("table bounds: commands per row are bounded by the line length", "[bside][table][bounds]")
{
    // 200 bytes of "gate on," pieces: at most 25 commands.
    std::string row;
    while (row.size() + 9 < 190)
        row += "gate on, ";
    row += "end";
    REQUIRE (row.size() <= 200);
    CHECK (readBody ("[wave1]\n" + row + "\n").ok);
}

TEST_CASE ("table bounds: the largest table and the compiled size", "[bside][table][bounds]")
{
    static_assert (sizeof (Program) <= 8192, "docs/architecture.md 8.6.7");
    CHECK (sizeof (Program) == 8 + 28 + 4 + 448 * 4 + 512 * 8);

    std::string t;
    for (const char* lane : { "wave1", "wave2", "wave3" })
        t += std::string ("[") + lane + "]\n" + repeatLine ("pitch +1 ticks 3", 64);
    for (const char* lane : { "pulse1", "pulse2", "pulse3" })
        t += std::string ("[") + lane + "]\n" + repeatLine ("set 100", 64);
    t += "[filter]\n" + repeatLine ("res 3", 64);
    const auto r = readBody (t);
    INFO (r.message());
    REQUIRE (r.ok);
    CHECK (r.program.numRows == 448);
    CHECK (r.program.numCmds == 448);
    std::string why;
    CHECK (validate (r.program, &why));
    std::string text;
    REQUIRE (writeTable (r.program, text));
    CHECK (text.size() <= 32768); // the canonical form of the largest table fits the text limit
    const auto again = readTable (text);
    REQUIRE (again.ok);
    CHECK (again.program == r.program);
}

TEST_CASE ("table bounds: the canonical form of a maximal table stays within the limits", "[bside][table][bounds]")
{
    // 3 wave lanes x 64 rows of the widest legal row (8 commands), within 512 commands in total:
    // the writer's lines are at most 200 bytes and the whole text at most 32 KB.
    std::string t;
    t = "[wave1]\n";
    for (int i = 0; i < 64; ++i)
        t += "wave tri+saw+pulse+noise+gate+sync+ring+test -96, adsr f f f f, pitch -96.00 ticks 255\n";
    const auto r = readBody (t);
    INFO (r.message());
    REQUIRE (r.ok);
    std::string text;
    REQUIRE (writeTable (r.program, text));
    std::size_t longest = 0, cur = 0;
    for (const char c : text)
    {
        cur = c == '\n' ? 0 : cur + 1;
        longest = std::max (longest, cur);
    }
    CHECK (longest <= 200);
    CHECK (text.size() <= 32768);
}
