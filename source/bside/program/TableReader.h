#pragma once

// The strict reader and compiler of the patch table text format (docs/architecture.md 8.6.5).
// A bounded parser of untrusted input, in the style of the preset reader: one pass over the lines,
// no recursion, linear time, memory bounded by the limits (about 32 KB of scratch), no exception
// leaves readTable(). The table is rejected as a whole; up to 8 messages are kept.

#include "bside/program/Program.h"
#include "bside/text/TextScanner.h"

#include <string>
#include <string_view>

namespace vs::bside::program
{
struct ReadResult
{
    bool ok = false;
    Program program = emptyProgram(); // valid only when ok
    text::ErrorList errors;

    /** The messages in the contract's format ("line L, column C: ... (expected ...)", then "and N more"). */
    std::string message() const { return errors.format(); }
};

/** Parses and compiles a table. Never throws. */
ReadResult readTable (std::string_view text) noexcept;
} // namespace vs::bside::program
