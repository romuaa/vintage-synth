// The register trace format (docs/architecture.md 8.9.2): grammar, every error class, every bound.

#include "FuzzSupport.h"

#include "bside/trace/TraceFormat.h"

#include <catch2/catch_test_macros.hpp>

using namespace vs::bside::trace;

namespace
{
const char* kContractExample = R"(bside-trace 1
clock 985248                # chip clock, Hz
model A                     # A or B
check 4096                  # optional: compare the state every N cycles (and always after each write)
# <cycle> <register hex> <value hex>; cycles ascending, equal cycles apply in file order
0 18 0f
0 05 09
40 04 11
end 2000000                 # last cycle simulated
)";

std::string withHeader (const std::string& body)
{
    return "bside-trace 1\nclock 985248\nmodel B\n" + body;
}

void expectTraceError (const std::string& text, int line, int col, const std::string& what)
{
    const auto r = readTrace (text);
    INFO ("text:\n" << text << "message: " << r.message());
    REQUIRE_FALSE (r.ok);
    REQUIRE (! r.errors.stored().empty());
    const auto& e = r.errors.stored().front();
    CHECK (e.line == line);
    CHECK (e.column == col);
    CHECK (e.what.find (what) != std::string::npos);
}
} // namespace

TEST_CASE ("trace: the contract example reads", "[bside][trace]")
{
    const auto r = readTrace (kContractExample);
    INFO (r.message());
    REQUIRE (r.ok);
    const auto& t = r.trace;
    CHECK (t.clockHz == 985248);
    CHECK (t.model == Model::A);
    CHECK (t.check == 4096);
    REQUIRE (t.writes.size() == 3);
    CHECK (t.writes[0].cycle == 0);
    CHECK (t.writes[0].reg == 0x18);
    CHECK (t.writes[0].value == 0x0f);
    CHECK (t.writes[1].reg == 0x05);
    CHECK (t.writes[2].cycle == 40);
    CHECK (t.writes[2].value == 0x11);
    CHECK (t.endCycle == 2000000);
}

TEST_CASE ("trace: canonical text and round trip", "[bside][trace]")
{
    const auto r = readTrace (kContractExample);
    REQUIRE (r.ok);
    std::string text;
    REQUIRE (writeTrace (r.trace, text));
    CHECK (text == "bside-trace 1\nclock 985248\nmodel A\ncheck 4096\n0 18 0f\n0 05 09\n40 04 11\nend 2000000\n");
    const auto again = readTrace (text);
    REQUIRE (again.ok);
    std::string text2;
    REQUIRE (writeTrace (again.trace, text2));
    CHECK (text2 == text);
    CHECK (again.trace.writes.size() == r.trace.writes.size());
    // No `check` line when it is not set.
    auto t = r.trace;
    t.check = 0;
    REQUIRE (writeTrace (t, text));
    CHECK (text.find ("check") == std::string::npos);
}

TEST_CASE ("trace: accepted spellings", "[bside][trace]")
{
    const auto r = readTrace ("\n# lead\r\nbside-trace   1 # v\r\n\tclock\t1022727\r\nmodel B\r\n   5   18   Ff   # upper-case hex\r\n5 00 00\r\nend 5");
    INFO (r.message());
    REQUIRE (r.ok);
    CHECK (r.trace.clockHz == 1022727);
    CHECK (r.trace.writes[0].reg == 0x18);
    CHECK (r.trace.writes[0].value == 0xff);
    CHECK (r.trace.writes.size() == 2);
    CHECK (r.trace.endCycle == 5);
    // Header words in any order; writes optional.
    CHECK (readTrace ("bside-trace 1\nmodel A\ncheck 10\nclock 5000\nend 0\n").ok);
    CHECK (readTrace (withHeader ("end 0\n# trailing comment\n\n")).ok);
}

TEST_CASE ("trace errors: header and structure", "[bside][trace][errors]")
{
    expectTraceError ("", 1, 1, "missing header");
    expectTraceError ("clock 5000\n", 1, 1, "missing header");
    expectTraceError ("bside-trace 2\n", 1, 13, "unsupported trace version");
    expectTraceError ("bside-trace\n", 1, 12, "missing trace version");
    expectTraceError ("bside-trace 1 x\n", 1, 15, "unexpected");
    expectTraceError ("bside-trace 1\nmodel B\n0 00 00\nend 0\n", 3, 1, "missing 'clock'");
    expectTraceError ("bside-trace 1\nclock 5000\n0 00 00\nend 0\n", 3, 1, "missing 'model'");
    expectTraceError ("bside-trace 1\nclock 5000\nend 0\n", 3, 1, "missing 'model'");
    expectTraceError (withHeader (""), 4, 1, "missing 'end'");
    expectTraceError (withHeader ("0 00 00\n"), 5, 1, "missing 'end'");
    expectTraceError (withHeader ("end 5\nend 6\n"), 5, 1, "statement after 'end'");
    expectTraceError (withHeader ("end 5\n6 00 00\n"), 5, 1, "statement after 'end'");
    expectTraceError (withHeader ("clock 6000\nend 1\n"), 4, 1, "'clock' given twice");
    expectTraceError (withHeader ("model A\nend 1\n"), 4, 1, "'model' given twice");
    expectTraceError (withHeader ("check 5\ncheck 6\nend 1\n"), 5, 1, "'check' given twice");
    expectTraceError (withHeader ("0 00 00\nclock 6000\nend 1\n"), 5, 1, "after the first write");
    expectTraceError (withHeader ("0 00 00\ncheck 6\nend 1\n"), 5, 1, "after the first write");
    expectTraceError (withHeader ("bogus 1\nend 1\n"), 4, 1, "unknown statement 'bogus'");
    expectTraceError (withHeader ("Clock 1\nend 1\n"), 4, 1, "unknown statement");
    expectTraceError (withHeader ("[x]\nend 1\n"), 4, 1, "unknown statement");
}

TEST_CASE ("trace errors: header values", "[bside][trace][errors]")
{
    expectTraceError ("bside-trace 1\nclock\n", 2, 6, "missing value");
    expectTraceError ("bside-trace 1\nclock abc\n", 2, 7, "not a number");
    expectTraceError ("bside-trace 1\nclock 999\n", 2, 7, "out of range");
    expectTraceError ("bside-trace 1\nclock 100000001\n", 2, 7, "out of range");
    expectTraceError ("bside-trace 1\nclock -5\n", 2, 7, "not a number");
    expectTraceError ("bside-trace 1\nclock 5000 x\n", 2, 12, "unexpected");
    expectTraceError ("bside-trace 1\nclock 5000\nmodel C\n", 3, 7, "bad model");
    expectTraceError ("bside-trace 1\nclock 5000\nmodel a\n", 3, 7, "bad model");
    expectTraceError ("bside-trace 1\nclock 5000\nmodel\n", 3, 6, "missing value");
    expectTraceError (withHeader ("check 0\nend 1\n"), 4, 7, "out of range");
    expectTraceError (withHeader ("check 1099511627777\nend 1\n"), 4, 7, "out of range");
    expectTraceError (withHeader ("end\n"), 4, 4, "missing value");
    expectTraceError (withHeader ("end x\n"), 4, 5, "not a number");
    expectTraceError (withHeader ("end 1099511627777\n"), 4, 5, "out of range");
    expectTraceError (withHeader ("end 1 2\n"), 4, 7, "unexpected");
}

TEST_CASE ("trace errors: writes", "[bside][trace][errors]")
{
    expectTraceError (withHeader ("0 19 00\nend 0\n"), 4, 3, "bad register");
    expectTraceError (withHeader ("0 1b 00\nend 0\n"), 4, 3, "bad register");
    expectTraceError (withHeader ("0 ff 00\nend 0\n"), 4, 3, "bad register");
    expectTraceError (withHeader ("0 5 00\nend 0\n"), 4, 3, "bad register"); // exactly two digits
    expectTraceError (withHeader ("0 005 00\nend 0\n"), 4, 3, "bad register");
    expectTraceError (withHeader ("0 0g 00\nend 0\n"), 4, 3, "bad register");
    expectTraceError (withHeader ("0\nend 0\n"), 4, 2, "missing register");
    expectTraceError (withHeader ("0 00\nend 0\n"), 4, 5, "missing value");
    expectTraceError (withHeader ("0 00 0\nend 0\n"), 4, 6, "bad value");
    expectTraceError (withHeader ("0 00 100\nend 0\n"), 4, 6, "bad value");
    expectTraceError (withHeader ("0 00 zz\nend 0\n"), 4, 6, "bad value");
    expectTraceError (withHeader ("0 00 00 00\nend 0\n"), 4, 9, "unexpected");
    expectTraceError (withHeader ("1099511627777 00 00\nend 1\n"), 4, 1, "out of range");
    expectTraceError (withHeader ("99999999999999999999999 00 00\nend 1\n"), 4, 1, "out of range");
    expectTraceError (withHeader ("1x 00 00\nend 1\n"), 4, 1, "not a number");
    expectTraceError (withHeader ("10 00 00\n9 00 00\nend 20\n"), 5, 1, "before the previous write");
    expectTraceError (withHeader ("10 00 00\nend 9\n"), 5, 5, "before the last write");
    // Equal cycles are fine; the maximum cycle is 2^40.
    CHECK (readTrace (withHeader ("10 00 00\n10 01 01\n10 02 02\nend 10\n")).ok);
    CHECK (readTrace (withHeader ("1099511627776 00 00\nend 1099511627776\n")).ok);
}

TEST_CASE ("trace bounds: lines, writes, size, checkpoints", "[bside][trace][bounds]")
{
    SECTION ("64 bytes per statement, 256 per line with its comment")
    {
        const std::string stmt = "0 00 00";
        CHECK (readTrace (withHeader (stmt + std::string (64 - stmt.size(), ' ') + "\nend 0\n")).ok);
        expectTraceError (withHeader (stmt + std::string (65 - stmt.size(), ' ') + "\nend 0\n"), 4, 65, "line too long");
        CHECK (readTrace (withHeader (stmt + std::string (57, ' ') + "#" + std::string (256 - 65, 'x') + "\nend 0\n")).ok);
        expectTraceError (withHeader (stmt + std::string (58, ' ') + "#" + std::string (10, 'x') + "\nend 0\n"), 4, 65, "line too long");
        CHECK (readTrace (withHeader ("#" + std::string (255, 'x') + "\nend 0\n")).ok);
        expectTraceError (withHeader ("#" + std::string (256, 'x') + "\nend 0\n"), 4, 257, "line too long");
    }
    SECTION ("10^6 writes")
    {
        std::string body;
        body.reserve (12000000);
        for (int i = 0; i < 1000000; ++i)
            body += "0 00 00\n";
        // 8 MB: inside the 16 MB file limit.
        auto r = readTrace (withHeader (body + "end 0\n"));
        INFO (r.message());
        REQUIRE (r.ok);
        CHECK (r.trace.writes.size() == 1000000);
        body += "0 00 00\n";
        r = readTrace (withHeader (body + "end 0\n"));
        REQUIRE_FALSE (r.ok);
        CHECK (r.errors.total() == 1);
        CHECK (r.errors.stored()[0].line == 3 + 1000000 + 1);
        CHECK (r.errors.stored()[0].what.find ("too many writes") != std::string::npos);
    }
    SECTION ("16 MB file")
    {
        std::string t = withHeader ("end 0\n");
        std::string big = t + std::string (kMaxTraceBytes - t.size(), '\n');
        REQUIRE (big.size() == kMaxTraceBytes);
        CHECK (readTrace (big).ok);
        big += "\n";
        const auto r = readTrace (big);
        REQUIRE_FALSE (r.ok);
        CHECK (r.errors.stored()[0].what.find ("trace too large") != std::string::npos);
        CHECK (r.errors.total() == 1);
    }
    SECTION ("periodic checkpoints")
    {
        // check 1 up to cycle 1,000,001 gives exactly 1,000,000 periodic checkpoints (cycles 1 .. 10^6)
        std::string head = "bside-trace 1\nclock 985248\nmodel B\ncheck 1\n";
        CHECK (readTrace (head + "end 1000001\n").ok);
        expectTraceError (head + "end 1000002\n", 5, 5, "periodic checkpoints");
        CHECK (readTrace ("bside-trace 1\nclock 985248\nmodel B\ncheck 1073741824\nend 1099511627776\n").ok);
        CHECK (readTrace ("bside-trace 1\nclock 985248\nmodel B\ncheck 1073741824\nend 1099511627776\n").ok);
    }
}

TEST_CASE ("trace: invalid bytes", "[bside][trace][errors]")
{
    expectTraceError (std::string ("bside-trace 1\nclock 5000\0\nmodel B\nend 0\n", 40), 2, 11, "invalid text");
    expectTraceError ("bside-trace 1\n\xff\n", 2, 1, "invalid text");
    expectTraceError ("bside-trace 1\r\nclock 5000\rmodel B\n", 2, 11, "invalid text");
}

TEST_CASE ("trace: checkpoint schedule", "[bside][trace]")
{
    Trace t;
    t.clockHz = 985248;
    t.model = Model::B;
    t.endCycle = 100;
    std::vector<Checkpoint> s;
    SECTION ("no check: after every write, then the end")
    {
        t.writes = { { 0, 0x18, 0x0f }, { 0, 0x05, 0x09 }, { 40, 0x04, 0x11 } };
        REQUIRE (checkpointSchedule (t, s));
        REQUIRE (s.size() == 4);
        CHECK (s[0].kind == Checkpoint::Kind::afterWrite);
        CHECK (s[0].write == 0);
        CHECK (s[1].write == 1);
        CHECK (s[1].cycle == 0);
        CHECK (s[2].cycle == 40);
        CHECK (s[3].kind == Checkpoint::Kind::end);
        CHECK (s[3].cycle == 100);
    }
    SECTION ("check 40: periodic before the writes of that cycle, never at the end cycle")
    {
        t.check = 40;
        t.endCycle = 120;
        t.writes = { { 40, 0x00, 0x01 }, { 80, 0x00, 0x02 }, { 80, 0x01, 0x03 }, { 120, 0x02, 0x04 } };
        REQUIRE (checkpointSchedule (t, s));
        // periodic 40, write0, periodic 80, write1, write2, (120 is the end: write3 then end)
        const std::vector<Checkpoint::Kind> kinds = { Checkpoint::Kind::periodic, Checkpoint::Kind::afterWrite, Checkpoint::Kind::periodic,
                                                      Checkpoint::Kind::afterWrite, Checkpoint::Kind::afterWrite, Checkpoint::Kind::afterWrite,
                                                      Checkpoint::Kind::end };
        REQUIRE (s.size() == kinds.size());
        for (size_t i = 0; i < kinds.size(); ++i)
            CHECK (s[i].kind == kinds[i]);
        CHECK (s[0].cycle == 40);
        CHECK (s[2].cycle == 80);
        CHECK (s[5].cycle == 120);
        CHECK (s[6].cycle == 120);
    }
    SECTION ("periodic checkpoints between and after writes")
    {
        t.check = 30;
        t.endCycle = 100;
        t.writes = { { 50, 0x00, 0x01 } };
        REQUIRE (checkpointSchedule (t, s));
        // 30, write@50, 60, 90, end@100
        REQUIRE (s.size() == 5);
        CHECK (s[0].cycle == 30);
        CHECK (s[1].kind == Checkpoint::Kind::afterWrite);
        CHECK (s[2].cycle == 60);
        CHECK (s[3].cycle == 90);
        CHECK (s[4].kind == Checkpoint::Kind::end);
    }
    SECTION ("an invalid trace has no schedule")
    {
        t.writes = { { 50, 0x00, 0x01 }, { 40, 0x00, 0x01 } };
        CHECK_FALSE (checkpointSchedule (t, s));
        CHECK (s.empty());
        std::string out = "x";
        CHECK_FALSE (writeTrace (t, out));
        CHECK (out.empty());
    }
}

TEST_CASE ("trace: random traces round trip", "[bside][trace]")
{
    bst::Rng rng (77);
    for (int i = 0; i < 200; ++i)
    {
        Trace t;
        t.clockHz = static_cast<std::uint32_t> (rng.range (1000, 2000000));
        t.model = rng.chance (50) ? Model::A : Model::B;
        t.check = rng.chance (50) ? static_cast<std::uint64_t> (rng.range (1, 5000)) : 0;
        std::uint64_t c = 0;
        const int n = rng.range (0, 60);
        for (int k = 0; k < n; ++k)
        {
            c += rng.chance (30) ? 0 : static_cast<std::uint64_t> (rng.range (0, 100000));
            t.writes.push_back ({ c, static_cast<std::uint8_t> (rng.range (0, 0x18)), static_cast<std::uint8_t> (rng.range (0, 255)) });
        }
        t.endCycle = c + static_cast<std::uint64_t> (rng.range (0, 100000));
        std::string text;
        REQUIRE (writeTrace (t, text));
        const auto r = readTrace (text);
        INFO (text << r.message());
        REQUIRE (r.ok);
        CHECK (r.trace.clockHz == t.clockHz);
        CHECK (r.trace.model == t.model);
        CHECK (r.trace.check == t.check);
        CHECK (r.trace.endCycle == t.endCycle);
        REQUIRE (r.trace.writes.size() == t.writes.size());
        for (size_t k = 0; k < t.writes.size(); ++k)
            CHECK ((r.trace.writes[k].cycle == t.writes[k].cycle && r.trace.writes[k].reg == t.writes[k].reg && r.trace.writes[k].value == t.writes[k].value));
        std::string again;
        REQUIRE (writeTrace (r.trace, again));
        CHECK (again == text);
    }
}
