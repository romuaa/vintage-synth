// Spec tests of the table text format (docs/architecture.md 8.6.3 and 8.6.5): every command, every
// accepted spelling, every error class with its position.

#include "TableTestUtil.h"

using namespace bst;

namespace
{
const Cmd& cmd (const Program& p, int lane, int row, int k = 0)
{
    const auto& li = p.lanes[lane];
    return p.cmds[p.rows[li.firstRow + row].firstCmd + k];
}
const Row& rowOf (const Program& p, int lane, int row)
{
    return p.rows[p.lanes[lane].firstRow + row];
}
} // namespace

TEST_CASE ("table: the four example tables read and have the expected content", "[bside][table]")
{
    SECTION ("snare")
    {
        const auto r = mustRead (kSnare);
        const auto& p = r.program;
        CHECK (p.restart.mode == static_cast<int> (RestartMode::classic));
        CHECK (p.restart.lead == 2);
        CHECK (p.restart.gateoff == 2);
        REQUIRE (p.lanes[Wave1].numRows == 6);
        CHECK (rowOf (p, Wave1, 0).numCmds == 2);
        CHECK (cmd (p, Wave1, 0, 0).op == static_cast<int> (Op::adsr));
        CHECK (cmd (p, Wave1, 0, 0).a == 0x09);
        CHECK (cmd (p, Wave1, 0, 0).b == 0x09);
        CHECK (cmd (p, Wave1, 0, 1).u0 == (bitNoise | bitGate));
        CHECK (cmd (p, Wave1, 0, 1).a == static_cast<int> (PitchKind::absolute));
        CHECK (cmd (p, Wave1, 0, 1).b == 97); // C#7 = 12 * 8 + 1
        CHECK (cmd (p, Wave1, 1, 0).b == 57); // A3
        CHECK (cmd (p, Wave1, 2, 0).b == 53); // F3
        CHECK (rowOf (p, Wave1, 3).delay == 4);
        CHECK (cmd (p, Wave1, 3, 0).b == 86); // D6
        CHECK (rowOf (p, Wave1, 4).delay == 1);
        CHECK (cmd (p, Wave1, 4, 0).u0 == bitNoise);
        CHECK (cmd (p, Wave1, 5, 0).op == static_cast<int> (Op::end));
        REQUIRE (p.lanes[Pulse1].numRows == 1);
        CHECK (cmd (p, Pulse1, 0).op == static_cast<int> (Op::pulseSet));
        CHECK (cmd (p, Pulse1, 0).a == 2048);
        CHECK (p.lanes[Wave2].numRows == 0);
    }
    SECTION ("arpeggio")
    {
        const auto r = mustRead (kArpeggio);
        const auto& p = r.program;
        REQUIRE (p.lanes[Wave1].numRows == 7);
        CHECK (rowOf (p, Wave1, 0).delay == 0);
        CHECK (cmd (p, Wave1, 0).b == 0);
        CHECK (cmd (p, Wave1, 0).a == static_cast<int> (PitchKind::relative));
        CHECK (cmd (p, Wave1, 3).b == 700);
        // `loop:` names row 1, `release:` row 5.
        CHECK (cmd (p, Wave1, 4).op == static_cast<int> (Op::jump));
        CHECK (cmd (p, Wave1, 4).a == 1);
        CHECK (p.lanes[Wave1].release == 5);
        CHECK (cmd (p, Wave1, 5).op == static_cast<int> (Op::gateOff));
        CHECK (rowOf (p, Wave1, 5).delay == 0);
        CHECK (cmd (p, Wave1, 6).a == 1);
        // A jump row costs no tick; a plain row costs one.
        CHECK (rowOf (p, Wave1, 4).delay == 0);
        CHECK (rowOf (p, Wave1, 6).delay == 0);
        CHECK (rowOf (p, Wave1, 1).delay == 1);
        CHECK (cmd (p, Pulse1, 0).a == 1024); // 25 %
    }
    SECTION ("pulse-width lead")
    {
        const auto r = mustRead (kPulseLead);
        const auto& p = r.program;
        CHECK (p.restart.mode == static_cast<int> (RestartMode::phase));
        REQUIRE (p.lanes[Wave1].numRows == 4);
        CHECK (rowOf (p, Wave1, 0).delay == 12);
        CHECK (cmd (p, Wave1, 1).b == 12);
        CHECK (cmd (p, Wave1, 2).b == -12);
        CHECK (cmd (p, Wave1, 3).a == 1); // vib: row 1
        REQUIRE (p.lanes[Pulse1].numRows == 2);
        CHECK (cmd (p, Pulse1, 1).op == static_cast<int> (Op::pulsePingPong));
        CHECK (cmd (p, Pulse1, 1).a == 512);
        CHECK (cmd (p, Pulse1, 1).b == 3584);
        CHECK (cmd (p, Pulse1, 1).c == 48);
        CHECK (rowOf (p, Pulse1, 1).delay == 1);
    }
    SECTION ("filter bass")
    {
        const auto r = mustRead (kFilterBass);
        const auto& p = r.program;
        CHECK (p.restart.ad == 0);
        CHECK (p.restart.sr == 0);
        REQUIRE (p.lanes[Filter].numRows == 3);
        CHECK (rowOf (p, Filter, 0).numCmds == 5);
        CHECK (cmd (p, Filter, 0, 0).u0 == 1); // route 1
        CHECK (cmd (p, Filter, 0, 1).u0 == 1); // lp
        CHECK (cmd (p, Filter, 0, 2).a == 12);
        CHECK (cmd (p, Filter, 0, 3).a == 15);
        CHECK (cmd (p, Filter, 0, 4).a == 1800);
        CHECK (cmd (p, Filter, 1).op == static_cast<int> (Op::cutoffSweep));
        CHECK (cmd (p, Filter, 1).a == -96);
        CHECK (cmd (p, Filter, 1).u0 == 16);
    }
}

TEST_CASE ("table: the empty table and layout rules", "[bside][table]")
{
    SECTION ("header only")
    {
        const auto r = mustRead ("bside-table 1\n");
        CHECK (r.program == emptyProgram());
    }
    SECTION ("comments, blank lines, CRLF, tabs, no final newline")
    {
        const auto r = mustRead ("# first\r\n\r\n  bside-table\t1   # header\r\n[wave1] # lane\r\n\tgate on\t# on\r\n[pulse1]\r\nset 5");
        CHECK (r.program.lanes[Wave1].numRows == 1);
        CHECK (cmd (r.program, Pulse1, 0).a == 5);
    }
    SECTION ("sections in any order, empty sections")
    {
        const auto r = mustRead ("bside-table 1\n[filter]\n[pulse3]\nset 1\n[wave2]\ngate on\n[restart]\n");
        CHECK (r.program.lanes[Wave2].numRows == 1);
        CHECK (r.program.lanes[Pulse3].numRows == 1);
        CHECK (r.program.lanes[Filter].numRows == 0);
    }
}

TEST_CASE ("table: wave lane commands", "[bside][table]")
{
    SECTION ("wave bits and pitches")
    {
        const auto r = readBody ("[wave1]\nwave none\nwave tri+saw+pulse+noise+gate+sync+ring+test +7\nwave Gate\n");
        CHECK_FALSE (r.ok); // keywords are lower case
        const auto g = mustRead ("bside-table 1\n[wave1]\nwave none\nwave tri+saw+pulse+noise+gate+sync+ring+test +7\nwave saw ~\nwave pulse -12\nwave tri +0.15\nwave tri +0.5\nwave tri +1.05\nwave tri -0\n");
        const auto& p = g.program;
        CHECK (cmd (p, Wave1, 0).u0 == 0);
        CHECK (cmd (p, Wave1, 0).a == static_cast<int> (PitchKind::unchanged));
        CHECK (cmd (p, Wave1, 1).u0 == 255);
        CHECK (cmd (p, Wave1, 1).b == 700);
        CHECK (cmd (p, Wave1, 2).a == static_cast<int> (PitchKind::unchanged));
        CHECK (cmd (p, Wave1, 3).b == -1200);
        CHECK (cmd (p, Wave1, 4).b == 15);
        CHECK (cmd (p, Wave1, 5).b == 50);
        CHECK (cmd (p, Wave1, 6).b == 105);
        CHECK (cmd (p, Wave1, 7).b == 0);
    }
    SECTION ("absolute notes, case-insensitive")
    {
        const auto g = mustRead ("bside-table 1\n[wave1]\npitch =c4\npitch =C4\npitch =Db4\npitch =DB4\npitch =c#6\npitch =61\npitch =0\npitch =127\npitch =c-1\npitch =G9\npitch =bb3\npitch =b3\npitch =a0\n");
        const auto& p = g.program;
        const int expect[] = { 60, 60, 61, 61, 85, 61, 0, 127, 0, 127, 58, 59, 21 };
        for (int i = 0; i < 13; ++i)
            CHECK (cmd (p, Wave1, i).b == expect[i]);
    }
    SECTION ("pitch ~, gate, adsr")
    {
        const auto g = mustRead ("bside-table 1\n[wave1]\npitch ~\ngate on\ngate off\nadsr 0 9 a F\nadsr f e d c\n");
        const auto& p = g.program;
        CHECK (cmd (p, Wave1, 0).op == static_cast<int> (Op::pitch));
        CHECK (cmd (p, Wave1, 0).a == 0);
        CHECK (cmd (p, Wave1, 1).op == static_cast<int> (Op::gateOn));
        CHECK (cmd (p, Wave1, 2).op == static_cast<int> (Op::gateOff));
        CHECK (cmd (p, Wave1, 3).a == 0x09);
        CHECK (cmd (p, Wave1, 3).b == 0xaf);
        CHECK (cmd (p, Wave1, 4).a == 0xfe);
        CHECK (cmd (p, Wave1, 4).b == 0xdc);
    }
    SECTION ("all three wave lanes take the same commands")
    {
        for (const char* lane : { "wave1", "wave2", "wave3" })
            CHECK (readBody (std::string ("[") + lane + "]\nwave pulse+gate +0\n").ok);
    }
}

TEST_CASE ("table: pulse lane commands", "[bside][table]")
{
    const auto g = mustRead ("bside-table 1\n[pulse2]\nset 0\nset 4095\nset 50%\nset 100%\nset 0%\nset 33.33%\nset 12.5%\nsweep +5 x3\nsweep -4095 x255\n[pulse3]\npingpong 0 4095 1\n");
    const auto& p = g.program;
    CHECK (cmd (p, Pulse2, 0).a == 0);
    CHECK (cmd (p, Pulse2, 1).a == 4095);
    CHECK (cmd (p, Pulse2, 2).a == 2048);
    CHECK (cmd (p, Pulse2, 3).a == 4095); // 100 % clamps to the register maximum
    CHECK (cmd (p, Pulse2, 4).a == 0);
    CHECK (cmd (p, Pulse2, 5).a == 1365); // 33.33 % of 4096
    CHECK (cmd (p, Pulse2, 6).a == 512);
    CHECK (cmd (p, Pulse2, 7).op == static_cast<int> (Op::pulseSweep));
    CHECK (cmd (p, Pulse2, 7).a == 5);
    CHECK (cmd (p, Pulse2, 7).u0 == 3);
    CHECK (cmd (p, Pulse2, 8).a == -4095);
    CHECK (cmd (p, Pulse2, 8).u0 == 255);
    CHECK (cmd (p, Pulse3, 0).c == 1);
}

TEST_CASE ("table: filter lane commands", "[bside][table]")
{
    const auto g = mustRead ("bside-table 1\n[filter]\ncutoff 0\ncutoff 2047\nres 15\nvolume 0\n"
                             "mode off\nmode lp\nmode bp\nmode hp\nmode lp+bp\nmode bp+hp\nmode lp+hp\nmode lp+bp+hp\n"
                             "route none\nroute 1\nroute 13\nroute 321\nroute 123\nsweep +2047 x1\nsweep -2047 x2\n");
    const auto& p = g.program;
    CHECK (cmd (p, Filter, 1).a == 2047);
    CHECK (cmd (p, Filter, 2).a == 15);
    const int masks[] = { 0, 1, 2, 4, 3, 6, 5, 7 };
    for (int i = 0; i < 8; ++i)
        CHECK (cmd (p, Filter, 4 + i).u0 == masks[i]);
    CHECK (cmd (p, Filter, 12).u0 == 0);
    CHECK (cmd (p, Filter, 13).u0 == 1);
    CHECK (cmd (p, Filter, 14).u0 == 5);
    CHECK (cmd (p, Filter, 15).u0 == 7);
    CHECK (cmd (p, Filter, 16).u0 == 7);
    CHECK (cmd (p, Filter, 17).op == static_cast<int> (Op::cutoffSweep));
    CHECK (cmd (p, Filter, 17).a == 2047);
    CHECK (cmd (p, Filter, 18).a == -2047);
}

TEST_CASE ("table: flow commands, labels and delays", "[bside][table]")
{
    SECTION ("repeat, jump, release, label names")
    {
        const auto g = mustRead ("bside-table 1\n[wave3]\na_1:\nrelease:\nb:\ngate on\npitch +1\nrepeat a_1 255\njump b\nend\n");
        const auto& p = g.program;
        CHECK (p.lanes[Wave3].release == 0);
        CHECK (cmd (p, Wave3, 2).op == static_cast<int> (Op::repeat));
        CHECK (cmd (p, Wave3, 2).a == 0);
        CHECK (cmd (p, Wave3, 2).b == 255);
        CHECK (cmd (p, Wave3, 3).a == 0);
        CHECK (p.lanes[Wave1].release == kNoRelease);
    }
    SECTION ("a label may be used before and after its definition, `release` may be a target")
    {
        CHECK (readBody ("[wave1]\njump later\npitch +1\nlater:\ngate on\njump release\nrelease:\nend\n").ok);
    }
    SECTION ("delays")
    {
        const auto g = mustRead ("bside-table 1\n[wave1]\npitch +1\npitch +2 ticks 0\npitch +3 ticks 255\nx:\npitch +4, pitch +5 ticks 7\njump x ticks 2\nrepeat x 3\nend\n");
        const auto& p = g.program;
        CHECK (rowOf (p, Wave1, 0).delay == 1);
        CHECK (rowOf (p, Wave1, 1).delay == 0);
        CHECK (rowOf (p, Wave1, 2).delay == 255);
        CHECK (rowOf (p, Wave1, 3).delay == 7);
        CHECK (rowOf (p, Wave1, 3).numCmds == 2);
        CHECK (rowOf (p, Wave1, 4).delay == 2);
        CHECK (rowOf (p, Wave1, 5).delay == 0);
        CHECK (rowOf (p, Wave1, 6).delay == 1);
    }
    SECTION ("a finite zero-delay repeat loop is allowed")
    {
        CHECK (readBody ("[wave1]\nx:\npitch +1 ticks 0\nrepeat x 3\npitch +2\n").ok);
    }
    SECTION ("a loop containing a row with ticks is allowed")
    {
        CHECK (readBody ("[wave1]\nx:\npitch +1 ticks 0\npitch +2\njump x\n").ok);
    }
}

TEST_CASE ("table: restart section", "[bside][table]")
{
    SECTION ("defaults")
    {
        const auto g = mustRead ("bside-table 1\n[restart]\n");
        CHECK (g.program == emptyProgram());
        CHECK (g.program.restart.lead == 2);
        CHECK (g.program.restart.gateoff == 2);
    }
    SECTION ("every key")
    {
        const auto g = mustRead ("bside-table 1\n[restart]\nmode classic\nlead 5\nad 1F\nsr a0\ngateoff 8\nfirstwave pulse+gate\n");
        const auto& r = g.program.restart;
        CHECK (r.mode == 1);
        CHECK (r.lead == 5);
        CHECK (r.ad == 0x1f);
        CHECK (r.sr == 0xa0);
        CHECK (r.gateoff == 8);
        CHECK (r.hasFirstWave == 1);
        CHECK (r.firstWave == (bitPulse | bitGate));
    }
    SECTION ("gateoff defaults to the lead, whatever the order")
    {
        CHECK (mustRead ("bside-table 1\n[restart]\nlead 6\n").program.restart.gateoff == 6);
        CHECK (mustRead ("bside-table 1\n[restart]\ngateoff 3\nlead 6\n").program.restart.gateoff == 3);
    }
    SECTION ("firstwave none")
    {
        const auto g = mustRead ("bside-table 1\n[restart]\nfirstwave none\n");
        CHECK (g.program.restart.hasFirstWave == 1);
        CHECK (g.program.restart.firstWave == 0);
    }
}

// ---- errors: one test per class, with the position ----

TEST_CASE ("table errors: header", "[bside][table][errors]")
{
    BS_EXPECT_ERROR ("", 1, 1, "missing header");
    BS_EXPECT_ERROR ("# only a comment\n\n", 1, 1, "missing header");
    BS_EXPECT_ERROR ("[wave1]\ngate on\n", 1, 1, "missing header");
    BS_EXPECT_ERROR ("# c\nbside-table\n", 2, 12, "missing table version");
    BS_EXPECT_ERROR ("bside-table 2\n", 1, 13, "unsupported table version");
    BS_EXPECT_ERROR ("bside-table 1 x\n", 1, 15, "unexpected");
    BS_EXPECT_ERROR ("Bside-table 1\n", 1, 1, "missing header");
}

TEST_CASE ("table errors: sections and statements outside them", "[bside][table][errors]")
{
    BS_EXPECT_BODY_ERROR ("[wave4]\n", 2, 1, "unknown section");
    BS_EXPECT_BODY_ERROR ("[ wave1]\n", 2, 1, "unknown section");
    BS_EXPECT_BODY_ERROR ("[Wave1]\n", 2, 1, "unknown section");
    BS_EXPECT_BODY_ERROR ("[wave1]\n[wave1]\n", 3, 1, "appears twice");
    BS_EXPECT_BODY_ERROR ("[restart]\n[restart]\n", 3, 1, "appears twice");
    BS_EXPECT_BODY_ERROR ("[wave1] x\n", 2, 9, "unexpected");
    BS_EXPECT_BODY_ERROR ("gate on\n", 2, 1, "outside of a section");
    BS_EXPECT_BODY_ERROR ("lbl:\n", 2, 1, "label outside of a section");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on\n[nope]\ngate off\n", 4, 1, "unknown section"); // and the section is dropped:
    BS_EXPECT_BODY_ERROR ("[nope]\ngate off\n", 2, 1, "unknown section");
    CHECK (readBody ("[wave1]\ngate on\n[nope]\ngate off\n").errors.total() == 2);
}

TEST_CASE ("table errors: unknown and misplaced commands", "[bside][table][errors]")
{
    BS_EXPECT_BODY_ERROR ("[wave1]\nfoo 1\n", 3, 1, "unknown command 'foo'");
    BS_EXPECT_BODY_ERROR ("[wave1]\n    Wave tri\n", 3, 5, "unknown command 'Wave'");
    BS_EXPECT_BODY_ERROR ("[wave1]\nset 5\n", 3, 1, "not available in a wave lane");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nwave tri\n", 3, 1, "not available in a pulse lane");
    BS_EXPECT_BODY_ERROR ("[pulse1]\ncutoff 5\n", 3, 1, "not available in a pulse lane");
    BS_EXPECT_BODY_ERROR ("[filter]\npingpong 1 2 3\n", 3, 1, "not available in a filter lane");
    BS_EXPECT_BODY_ERROR ("[filter]\nset 1\n", 3, 1, "not available in a filter lane");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on, bogus\n", 3, 10, "unknown command 'bogus'");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on,\n", 3, 9, "missing command after ','");
    BS_EXPECT_BODY_ERROR ("[wave1]\n, gate on\n", 3, 1, "missing command");
    BS_EXPECT_BODY_ERROR ("[restart]\nwave tri\n", 3, 1, "command 'wave' in [restart]");
    BS_EXPECT_BODY_ERROR ("[restart]\nspeed 3\n", 3, 1, "unknown setting");
    BS_EXPECT_BODY_ERROR ("[restart]\nlbl:\n", 3, 1, "label in [restart]");
}

TEST_CASE ("table errors: syntax of arguments", "[bside][table][errors]")
{
    BS_EXPECT_BODY_ERROR ("[wave1]\nwave\n", 3, 5, "missing argument");
    BS_EXPECT_BODY_ERROR ("[wave1]\nwave tri+\n", 3, 10, "empty waveform");
    BS_EXPECT_BODY_ERROR ("[wave1]\nwave tri++saw\n", 3, 10, "empty waveform");
    BS_EXPECT_BODY_ERROR ("[wave1]\nwave tri+bogus\n", 3, 10, "unknown name 'bogus'");
    BS_EXPECT_BODY_ERROR ("[wave1]\nwave tri+tri\n", 3, 10, "given twice");
    BS_EXPECT_BODY_ERROR ("[wave1]\nwave none+gate\n", 3, 6, "unknown name");
    BS_EXPECT_BODY_ERROR ("[wave1]\nwave tri +7x\n", 3, 10, "bad semitone offset");
    BS_EXPECT_BODY_ERROR ("[wave1]\nwave tri +\n", 3, 10, "bad semitone offset");
    BS_EXPECT_BODY_ERROR ("[wave1]\nwave tri +1.234\n", 3, 10, "bad semitone offset");
    BS_EXPECT_BODY_ERROR ("[wave1]\nwave tri +1.\n", 3, 10, "bad semitone offset");
    BS_EXPECT_BODY_ERROR ("[wave1]\npitch 7\n", 3, 7, "bad pitch");
    BS_EXPECT_BODY_ERROR ("[wave1]\npitch =\n", 3, 8, "missing note");
    BS_EXPECT_BODY_ERROR ("[wave1]\npitch =h4\n", 3, 8, "bad note");
    BS_EXPECT_BODY_ERROR ("[wave1]\npitch =c\n", 3, 8, "bad note");
    BS_EXPECT_BODY_ERROR ("[wave1]\npitch =c#\n", 3, 8, "bad note");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate maybe\n", 3, 6, "bad gate state");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate\n", 3, 5, "missing argument");
    BS_EXPECT_BODY_ERROR ("[wave1]\nadsr 0 9 a\n", 3, 11, "missing argument");
    BS_EXPECT_BODY_ERROR ("[wave1]\nadsr 0 9 a gg\n", 3, 12, "not a hex digit");
    BS_EXPECT_BODY_ERROR ("[wave1]\nadsr 10 9 a 1\n", 3, 6, "not a hex digit");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nset abc\n", 3, 5, "not a number");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nset -5\n", 3, 5, "not a number");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nset 5.5\n", 3, 5, "not a number");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nset %\n", 3, 5, "bad percentage");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nset 1.234%\n", 3, 5, "bad percentage");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nsweep 5\n", 3, 8, "missing argument");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nsweep 5 16\n", 3, 9, "bad tick count");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nsweep 5 x\n", 3, 10, "not a number");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nsweep x x2\n", 3, 7, "not a number");
    BS_EXPECT_BODY_ERROR ("[pulse1]\npingpong 1 2\n", 3, 13, "missing argument");
    BS_EXPECT_BODY_ERROR ("[filter]\nmode lp+lp\n", 3, 6, "bad filter mode");
    BS_EXPECT_BODY_ERROR ("[filter]\nmode bp+lp\n", 3, 6, "bad filter mode");
    BS_EXPECT_BODY_ERROR ("[filter]\nmode LP\n", 3, 6, "bad filter mode");
    BS_EXPECT_BODY_ERROR ("[filter]\nroute 4\n", 3, 7, "bad oscillator");
    BS_EXPECT_BODY_ERROR ("[filter]\nroute 11\n", 3, 8, "given twice");
    BS_EXPECT_BODY_ERROR ("[filter]\nroute\n", 3, 6, "missing argument");
    BS_EXPECT_BODY_ERROR ("[wave1]\njump\n", 3, 5, "missing argument");
    BS_EXPECT_BODY_ERROR ("[wave1]\njump Loop\n", 3, 6, "bad label name");
    BS_EXPECT_BODY_ERROR ("[wave1]\njump 1x\n", 3, 6, "bad label name");
    BS_EXPECT_BODY_ERROR ("[wave1]\njump aaaaaaaaaaaaaaaaa\n", 3, 6, "bad label name"); // 17 characters
    BS_EXPECT_BODY_ERROR ("[wave1]\nx:\nrepeat x\n", 4, 9, "missing argument");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on now\n", 3, 9, "unexpected 'now'");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on ticks\n", 3, 14, "missing argument");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on ticks x\n", 3, 15, "not a number");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on ticks 1 2\n", 3, 17, "unexpected");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on ticks -1\n", 3, 15, "not a number");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngateon\n", 3, 1, "unknown command 'gateon'");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on ticks 1 # ok\nend x\n", 4, 5, "unexpected 'x'");
}

TEST_CASE ("table errors: ranges (out of range is an error, never clamped)", "[bside][table][errors]")
{
    BS_EXPECT_BODY_ERROR ("[filter]\ncutoff 2048\n", 3, 8, "out of range");
    BS_EXPECT_BODY_ERROR ("[filter]\ncutoff 99999999999999999999999\n", 3, 8, "out of range");
    BS_EXPECT_BODY_ERROR ("[filter]\nres 16\n", 3, 5, "out of range");
    BS_EXPECT_BODY_ERROR ("[filter]\nvolume 16\n", 3, 8, "out of range");
    BS_EXPECT_BODY_ERROR ("[filter]\nsweep +2048 x2\n", 3, 7, "out of range");
    BS_EXPECT_BODY_ERROR ("[filter]\nsweep -2048 x2\n", 3, 7, "out of range");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nsweep +4096 x2\n", 3, 7, "out of range");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nsweep 5 x0\n", 3, 10, "out of range");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nsweep 5 x256\n", 3, 10, "out of range");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nset 4096\n", 3, 5, "out of range");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nset 101%\n", 3, 5, "out of range");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nset 100.01%\n", 3, 5, "out of range");
    BS_EXPECT_BODY_ERROR ("[pulse1]\npingpong 5 5 1\n", 3, 12, "not above the lower limit");
    BS_EXPECT_BODY_ERROR ("[pulse1]\npingpong 9 5 1\n", 3, 12, "not above the lower limit");
    BS_EXPECT_BODY_ERROR ("[pulse1]\npingpong 0 4096 1\n", 3, 12, "out of range");
    BS_EXPECT_BODY_ERROR ("[pulse1]\npingpong 0 9 0\n", 3, 14, "out of range");
    BS_EXPECT_BODY_ERROR ("[pulse1]\npingpong 0 9 4096\n", 3, 14, "out of range");
    BS_EXPECT_BODY_ERROR ("[wave1]\nx:\nrepeat x 0\n", 4, 10, "out of range");
    BS_EXPECT_BODY_ERROR ("[wave1]\nx:\nrepeat x 256\n", 4, 10, "out of range");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on ticks 256\n", 3, 15, "out of range");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on ticks 0001\n", 3, 15, "out of range"); // at most three digits
    BS_EXPECT_BODY_ERROR ("[wave1]\npitch +97\n", 3, 7, "out of range");
    BS_EXPECT_BODY_ERROR ("[wave1]\npitch -96.01\n", 3, 7, "out of range");
    BS_EXPECT_BODY_ERROR ("[wave1]\npitch =128\n", 3, 8, "out of range");
    BS_EXPECT_BODY_ERROR ("[wave1]\npitch =c#10\n", 3, 8, "out of range");
    BS_EXPECT_BODY_ERROR ("[wave1]\npitch =g#9\n", 3, 8, "out of range");
    BS_EXPECT_BODY_ERROR ("[wave1]\npitch =cb-1\n", 3, 8, "out of range");
    BS_EXPECT_BODY_ERROR ("[restart]\nlead 0\n", 3, 6, "bad value");
    BS_EXPECT_BODY_ERROR ("[restart]\nlead 9\n", 3, 6, "bad value");
    BS_EXPECT_BODY_ERROR ("[restart]\ngateoff 0\n", 3, 9, "bad value");
    BS_EXPECT_BODY_ERROR ("[restart]\ngateoff 9\n", 3, 9, "bad value");
    BS_EXPECT_BODY_ERROR ("[restart]\nad 1\n", 3, 4, "bad value");
    BS_EXPECT_BODY_ERROR ("[restart]\nad 100\n", 3, 4, "bad value");
    BS_EXPECT_BODY_ERROR ("[restart]\nsr zz\n", 3, 4, "bad value");
    BS_EXPECT_BODY_ERROR ("[restart]\nmode on\n", 3, 6, "bad value");
    BS_EXPECT_BODY_ERROR ("[restart]\nfirstwave tri+tri\n", 3, 11, "bad value");
    BS_EXPECT_BODY_ERROR ("[restart]\nlead\n", 3, 5, "missing value");
    BS_EXPECT_BODY_ERROR ("[restart]\nlead 2\nlead 3\n", 4, 1, "given twice");
    BS_EXPECT_BODY_ERROR ("[restart]\nlead 2 3\n", 3, 8, "unexpected");
}

TEST_CASE ("table errors: not yet available pitches", "[bside][table][errors]")
{
    BS_EXPECT_BODY_ERROR ("[wave1]\nwave tri key1\n", 3, 10, "not available yet");
    BS_EXPECT_BODY_ERROR ("[wave1]\npitch key3\n", 3, 7, "not available yet");
}

TEST_CASE ("table errors: labels and jumps", "[bside][table][errors]")
{
    BS_EXPECT_BODY_ERROR ("[wave1]\njump nowhere\n", 3, 6, "undefined label 'nowhere'");
    BS_EXPECT_BODY_ERROR ("[wave1]\nx:\ngate on\nx:\ngate off\n", 5, 1, "duplicate label 'x'");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on\njump x\n[wave2]\nx:\ngate on\n", 4, 6, "is in lane wave2, not in wave1");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on\njump x\n[pulse1]\nx:\nset 1\n", 4, 6, "is in lane pulse1");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on\nend:\n", 4, 1, "names no row");
    BS_EXPECT_BODY_ERROR ("[wave1]\nlast:\n[wave2]\ngate on\n", 3, 1, "names no row");
    BS_EXPECT_BODY_ERROR ("[wave1]\nBad:\ngate on\n", 3, 1, "unknown command"); // upper case is not a name: parsed as a command
    BS_EXPECT_BODY_ERROR ("[wave1]\n1a:\ngate on\n", 3, 1, "bad label name");
    BS_EXPECT_BODY_ERROR ("[wave1]\naaaaaaaaaaaaaaaaa:\ngate on\n", 3, 1, "bad label name");
    BS_EXPECT_BODY_ERROR ("[wave1]\nx: y\ngate on\n", 3, 4, "unexpected");
    // The same label name in two lanes is two labels.
    CHECK (readBody ("[wave1]\nx:\ngate on\njump x\n[wave2]\nx:\ngate on\njump x\n").ok);
}

TEST_CASE ("table errors: rows that may not coexist in one row", "[bside][table][errors]")
{
    BS_EXPECT_BODY_ERROR ("[wave1]\nx:\ngate on\njump x, gate off\n", 5, 9, "command after 'jump'");
    BS_EXPECT_BODY_ERROR ("[wave1]\nx:\ngate on\nrepeat x 2, gate off\n", 5, 13, "command after 'repeat'");
    BS_EXPECT_BODY_ERROR ("[wave1]\nend, gate off\n", 3, 6, "command after 'end'");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nsweep 5 x2, set 1\n", 3, 13, "command after 'sweep'");
    BS_EXPECT_BODY_ERROR ("[pulse1]\npingpong 1 2 3, set 1\n", 3, 17, "command after 'pingpong'");
    BS_EXPECT_BODY_ERROR ("[filter]\nsweep 5 x2, cutoff 1\n", 3, 13, "command after 'sweep'");
    BS_EXPECT_BODY_ERROR ("[pulse1]\nsweep 5 x2 ticks 3\n", 3, 12, "'sweep' rows take no ticks");
    BS_EXPECT_BODY_ERROR ("[pulse1]\npingpong 1 2 3 ticks 3\n", 3, 16, "'pingpong' rows take no ticks");
    BS_EXPECT_BODY_ERROR ("[filter]\nsweep 5 x2 ticks 0\n", 3, 12, "'sweep' rows take no ticks");
    BS_EXPECT_BODY_ERROR ("[wave1]\nend ticks 3\n", 3, 5, "'end' rows take no ticks");
    // Commands before a flow command are fine.
    CHECK (readBody ("[wave1]\nx:\ngate on, pitch +1 ticks 2\ngate off, jump x\n").ok);
    CHECK (readBody ("[wave1]\ngate on, end\n").ok);
}

TEST_CASE ("table errors: zero-delay loops", "[bside][table][errors]")
{
    // The error points at the first row of the loop.
    BS_EXPECT_BODY_ERROR ("[wave1]\nx:\npitch +1 ticks 0\njump x\n", 4, 1, "delay 0 form a loop in lane wave1");
    BS_EXPECT_BODY_ERROR ("[wave1]\nx:\njump x\n", 4, 1, "delay 0 form a loop");
    BS_EXPECT_BODY_ERROR ("[wave1]\ngate on\nx:\npitch +1 ticks 0\npitch +2 ticks 0\njump x\n", 5, 1, "delay 0 form a loop");
    BS_EXPECT_BODY_ERROR ("[pulse2]\nx:\nset 1 ticks 0\njump x\n", 4, 1, "delay 0 form a loop in lane pulse2");
    BS_EXPECT_BODY_ERROR ("[filter]\nres 1 ticks 0\nres 2 ticks 0\nres 3 ticks 0\nx:\ncutoff 1 ticks 0\njump x\n", 7, 1, "form a loop in lane filter");
    // A jump into a loop from outside is not itself a loop, but the loop it enters is.
    BS_EXPECT_BODY_ERROR ("[wave1]\njump x\npitch +1\nx:\npitch +2 ticks 0\njump x\n", 6, 1, "delay 0 form a loop");
    // Loops that wait, stop or sweep are fine.
    CHECK (readBody ("[wave1]\nx:\npitch +1\njump x\n").ok);
    CHECK (readBody ("[wave1]\nx:\npitch +1 ticks 0\npitch +2 ticks 0\npitch +3\njump x\n").ok);
    CHECK (readBody ("[pulse1]\nx:\nset 5 ticks 0\nsweep 1 x5\njump x\n").ok);
    CHECK (readBody ("[wave1]\nx:\ngate on ticks 0\nend\njump x\n").ok);
}

TEST_CASE ("table errors: several messages and the 8-message cap", "[bside][table][errors]")
{
    SECTION ("two errors on two lines are both reported, in order")
    {
        const auto r = readBody ("[wave1]\nfoo\nbar\n");
        REQUIRE_FALSE (r.ok);
        REQUIRE (r.errors.stored().size() == 2);
        CHECK (r.errors.stored()[0].line == 3);
        CHECK (r.errors.stored()[1].line == 4);
        CHECK (r.message() == "line 3, column 1: unknown command 'foo' (expected one of wave, pitch, gate, adsr, jump, repeat, end)\n"
                              "line 4, column 1: unknown command 'bar' (expected one of wave, pitch, gate, adsr, jump, repeat, end)");
    }
    SECTION ("more than eight: eight kept, the rest counted")
    {
        const auto r = readBody ("[wave1]\n" + repeatLine ("foo", 12));
        REQUIRE_FALSE (r.ok);
        CHECK (r.errors.stored().size() == 8);
        CHECK (r.errors.total() == 12);
        const auto m = r.message();
        CHECK (m.substr (m.rfind ('\n') + 1) == "and 4 more");
        CHECK (m.find ("line 10, column 1") != std::string::npos);
        CHECK (m.find ("line 11, column 1") == std::string::npos);
    }
    SECTION ("a rejected table gives the empty program")
    {
        CHECK (readBody ("[wave1]\nfoo\n").program == emptyProgram());
    }
}

TEST_CASE ("table errors: the contract's message format", "[bside][table][errors]")
{
    const auto r = readBody ("[pulse1]\nset 5000\n");
    REQUIRE_FALSE (r.ok);
    CHECK (r.message() == "line 3, column 5: value '5000' is out of range (expected a pulse width 0 to 4095, or a percentage like 25%)");
}
