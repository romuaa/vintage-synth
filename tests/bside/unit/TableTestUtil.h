#pragma once

// Helpers and the four example tables of docs/architecture.md 8.6.6 for the table reader tests.

#include "bside/program/Program.h"
#include "bside/program/TableReader.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace bst
{
using namespace vs::bside::program;

inline const char* kSnare = R"(bside-table 1
[restart]
mode classic
lead 2
[wave1]
    adsr 0 9 0 9, wave noise+gate =C#7
    wave pulse+gate =A3
    wave pulse+gate =F3
    wave noise+gate =D6 ticks 4
    wave noise =D6        # gate off: the release rings out
    end
[pulse1]
    set 2048
)";

inline const char* kArpeggio = R"(bside-table 1
[wave1]
    wave pulse+gate +0 ticks 0
loop:
    pitch +0
    pitch +4
    pitch +7
    jump loop
release:
    gate off ticks 0
    jump loop
[pulse1]
    set 25%
)";

inline const char* kPulseLead = R"(bside-table 1
[restart]
mode phase
lead 2
[wave1]
    wave pulse+gate +0 ticks 12
vib:
    pitch +0.12 ticks 2
    pitch -0.12 ticks 2
    jump vib
[pulse1]
    set 512
    pingpong 512 3584 48
)";

inline const char* kFilterBass = R"(bside-table 1
[restart]
mode phase
lead 2
ad 00
sr 00
[wave1]
    adsr 0 9 a 9, wave saw+gate +0
    end
[filter]
    route 1, mode lp, res 12, volume 15, cutoff 1800
    sweep -96 x16
    end
)";

inline const char* const kExamples[4] = { kSnare, kArpeggio, kPulseLead, kFilterBass };

/** Reads "bside-table 1\n" + body (so body line 1 is line 2 of the text). */
inline ReadResult readBody (const std::string& body)
{
    return readTable ("bside-table 1\n" + body);
}

inline ReadResult mustRead (const std::string& text)
{
    auto r = readTable (text);
    INFO (r.message());
    REQUIRE (r.ok);
    return r;
}

/** The first stored error of a rejected text. */
inline vs::bside::text::Error firstError (const std::string& text)
{
    const auto r = readTable (text);
    INFO ("expected an error for:\n" << text);
    REQUIRE_FALSE (r.ok);
    REQUIRE (! r.errors.stored().empty());
    return r.errors.stored().front();
}

inline std::string repeatLine (const std::string& line, int n)
{
    std::string s;
    for (int i = 0; i < n; ++i)
        s += line + "\n";
    return s;
}
} // namespace bst

/** The text is rejected, and its first message is at (line, column) and mentions `what`. */
#define BS_EXPECT_ERROR(text, ln, col, phrase)                                                            \
    do                                                                                                  \
    {                                                                                                   \
        const auto e_ = bst::firstError (text);                                                         \
        INFO ("message: line " << e_.line << ", column " << e_.column << ": " << e_.what                \
                               << " (expected " << e_.expected << ")");                                 \
        CHECK (e_.line == (ln));                                                                        \
        CHECK (e_.column == (col));                                                                     \
        CHECK (e_.what.find (phrase) != std::string::npos);                                               \
    } while (false)

/** Same for a body placed after the header line (body line 1 is text line 2). */
#define BS_EXPECT_BODY_ERROR(body, ln, col, phrase) BS_EXPECT_ERROR (std::string ("bside-table 1\n") + (body), ln, col, phrase)
