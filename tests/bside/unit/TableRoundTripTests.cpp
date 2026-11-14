// Round trip of the table format: text -> program -> canonical text -> the same program, and the
// writer is a fixed point (docs/architecture.md 8.6.5, "Writer").

#include "FuzzSupport.h"
#include "TableTestUtil.h"

using namespace bst;

namespace
{
/** Checks the whole round-trip contract for an accepted table. */
void checkRoundTrip (const std::string& text)
{
    const auto first = readTable (text);
    INFO (text << "\n" << first.message());
    REQUIRE (first.ok);
    std::string why;
    CHECK (validate (first.program, &why));
    INFO (why);

    std::string canonical;
    REQUIRE (writeTable (first.program, canonical));
    const auto second = readTable (canonical);
    INFO ("canonical text:\n" << canonical << second.message());
    REQUIRE (second.ok);
    CHECK (second.program == first.program);

    std::string again;
    REQUIRE (writeTable (second.program, again));
    CHECK (again == canonical); // writing twice gives the same text
}
} // namespace

TEST_CASE ("table round trip: the four example tables", "[bside][table][roundtrip]")
{
    for (const auto* t : kExamples)
        checkRoundTrip (t);
}

TEST_CASE ("table round trip: canonical text of the examples", "[bside][table][roundtrip]")
{
    std::string text;
    REQUIRE (writeTable (mustRead (kSnare).program, text));
    CHECK (text == "bside-table 1\n"
                   "[restart]\n"
                   "mode classic\n"
                   "[wave1]\n"
                   "    adsr 0 9 0 9, wave noise+gate =97\n"
                   "    wave pulse+gate =57\n"
                   "    wave pulse+gate =53\n"
                   "    wave noise+gate =86 ticks 4\n"
                   "    wave noise =86\n"
                   "    end\n"
                   "[pulse1]\n"
                   "    set 2048\n");
    REQUIRE (writeTable (mustRead (kArpeggio).program, text));
    CHECK (text == "bside-table 1\n"
                   "[wave1]\n"
                   "    wave pulse+gate +0 ticks 0\n"
                   "r1:\n"
                   "    pitch +0\n"
                   "    pitch +4\n"
                   "    pitch +7\n"
                   "    jump r1\n"
                   "release:\n"
                   "    gate off ticks 0\n"
                   "    jump r1\n"
                   "[pulse1]\n"
                   "    set 1024\n");
    REQUIRE (writeTable (mustRead (kPulseLead).program, text));
    CHECK (text == "bside-table 1\n"
                   "[restart]\n"
                   "mode phase\n"
                   "[wave1]\n"
                   "    wave pulse+gate +0 ticks 12\n"
                   "r1:\n"
                   "    pitch +0.12 ticks 2\n"
                   "    pitch -0.12 ticks 2\n"
                   "    jump r1\n"
                   "[pulse1]\n"
                   "    set 512\n"
                   "    pingpong 512 3584 48\n");
    REQUIRE (writeTable (mustRead (kFilterBass).program, text));
    CHECK (text == "bside-table 1\n"
                   "[restart]\n"
                   "mode phase\n"
                   "[wave1]\n"
                   "    adsr 0 9 a 9, wave saw+gate +0\n"
                   "    end\n"
                   "[filter]\n"
                   "    route 1, mode lp, res 12, volume 15, cutoff 1800\n"
                   "    sweep -96 x16\n"
                   "    end\n");
}

TEST_CASE ("table round trip: the empty table and defaults collapse", "[bside][table][roundtrip]")
{
    std::string text;
    REQUIRE (writeTable (emptyProgram(), text));
    CHECK (text == "bside-table 1\n");
    // Defaults written out explicitly, empty sections and comments vanish; the program is the same.
    const auto a = mustRead ("bside-table 1\n# c\n[restart]\nmode off\nlead 2\ngateoff 2\nad 00\nsr 00\n[wave1]\n[filter]\n");
    CHECK (a.program == emptyProgram());
}

TEST_CASE ("table round trip: every command and spelling", "[bside][table][roundtrip]")
{
    checkRoundTrip ("bside-table 1\n"
                    "[restart]\nmode classic\nlead 7\nad 1f\nsr a0\ngateoff 1\nfirstwave tri+gate\n"
                    "[wave1]\nwave none\nwave tri+saw+pulse+noise+gate+sync+ring+test +96\nwave saw -96\nwave pulse ~\nwave tri =c#4\n"
                    "pitch +0.01\npitch -0.1\npitch +12.34\npitch ~\npitch =0\npitch =127\ngate on\ngate off\nadsr 0 f 8 1\nend\n"
                    "[wave2]\nx:\nrelease:\ngate on, pitch +1 ticks 3\nrepeat x 255\njump release\n"
                    "[wave3]\nwave pulse+gate +0 ticks 0\nl:\npitch +0\nrepeat l 2\nend\n"
                    "[pulse1]\nset 0\nset 4095\nset 33.33%\nsweep +4095 x255\nsweep -1 x1\n"
                    "[pulse2]\npingpong 0 4095 4095\n"
                    "[pulse3]\nset 1\nloop:\nset 2\nsweep 0 x5\n"
                    "[filter]\ncutoff 0\ncutoff 2047, res 15, volume 0\nmode off\nmode lp+bp\nmode bp+hp\nmode lp+hp\nmode lp+bp+hp\nmode hp\nmode bp\nmode lp\n"
                    "route none\nroute 3\nroute 21\nsweep +2047 x255\n");
}

TEST_CASE ("table round trip: label names are regenerated", "[bside][table][roundtrip]")
{
    const auto a = mustRead ("bside-table 1\n[wave1]\nfoo:\ngate on\nbar:\ngate off\njump foo\nunused:\nrepeat bar 3\n");
    std::string text;
    REQUIRE (writeTable (a.program, text));
    CHECK (text == "bside-table 1\n[wave1]\nr0:\n    gate on\nr1:\n    gate off\n    jump r0\n    repeat r1 3\n");
    // A label on a row that is also the release row is called `release` everywhere.
    const auto b = mustRead ("bside-table 1\n[wave1]\nrelease:\nl:\ngate on\njump l\n");
    REQUIRE (writeTable (b.program, text));
    CHECK (text == "bside-table 1\n[wave1]\nrelease:\n    gate on\n    jump release\n");
}

TEST_CASE ("table round trip: random valid tables", "[bside][table][roundtrip]")
{
    Rng rng (2024);
    int accepted = 0;
    for (int i = 0; i < 400; ++i)
    {
        const auto text = generateTable (rng);
        const auto r = readTable (text);
        if (! r.ok)
            continue;
        ++accepted;
        checkRoundTrip (text);
    }
    CHECK (accepted > 150); // the generator mostly produces valid tables
}

TEST_CASE ("table writer: refuses a program that does not validate", "[bside][table][roundtrip]")
{
    std::string out = "stale";
    auto p = mustRead (kSnare).program;
    p.cmds[0].op = 200;
    CHECK_FALSE (writeTable (p, out));
    CHECK (out.empty());
    p = mustRead (kSnare).program;
    p.lanes[Wave1].numRows = 99;
    CHECK_FALSE (validate (p));
    p = mustRead (kArpeggio).program;
    p.cmds[p.rows[p.lanes[Wave1].firstRow + 4].firstCmd].a = 40; // jump outside the lane
    CHECK_FALSE (validate (p));
    p = mustRead (kSnare).program;
    p.rows[p.numRows].delay = 3; // unused storage must be zero (byte equality)
    CHECK_FALSE (validate (p));
}

TEST_CASE ("table program: byte equality is program equality", "[bside][table][roundtrip]")
{
    const auto a = mustRead (kSnare).program;
    auto b = a;
    CHECK (a == b);
    b.restart.lead = 3;
    CHECK (a != b);
    // The same table with other spacing, order of sections, comments and an explicit default is the same program.
    const auto c = mustRead ("# hi\nbside-table 1\n[pulse1]\n   set 2048 # x\n[restart]\n lead 2\n mode classic\n"
                             "[wave1]\n adsr 0 9 0 9,wave noise+gate =c#7\n wave pulse+gate =A3\n\twave pulse+gate =F3\n"
                             " wave noise+gate =d6   ticks   4\n wave noise =D6\n end\n");
    CHECK (c.program == a);
}
