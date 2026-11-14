#pragma once

// The register trace file (.bstrace, docs/architecture.md 8.9.2): a list of chip register writes
// at exact cycles. A bounded, strict reader of untrusted input (same style as the table reader):
// at most 16 MB, 10^6 writes, cycles up to 2^40, 64 bytes per statement (256 per line, comment included); one pass, no recursion, no
// exception leaves readTrace(); the first 8 problems are reported as
// "line L, column C: <what> (expected <what>)".
//
//   bside-trace 1
//   clock 985248              chip clock in Hz (1000 .. 100 000 000), once, before the writes
//   model A                   A or B, once, before the writes
//   check 4096                optional, once, before the writes: compare every N cycles
//   0 18 0f                   <cycle decimal> <register: 2 hex digits, 00..18> <value: 2 hex digits>
//   end 2000000               last cycle simulated; must be the last statement
//
// Cycles are non-decreasing (equal cycles apply in file order). `#` starts a comment; blank lines
// are ignored. Header words must come before the first write.

#include "bside/text/TextScanner.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vs::bside::trace
{
constexpr std::size_t kMaxTraceBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxWrites = 1000000;
constexpr std::uint64_t kMaxCycle = 1ull << 40;
/** The statement part of a line (the bytes before a '#') is at most 64 bytes; with its comment the
    whole line at most 256 (the contract's own example has longer lines with comments). */
constexpr std::size_t kMaxTraceLineBytes = 64;
constexpr std::size_t kMaxTraceFullLineBytes = 256;
constexpr std::uint32_t kMinClockHz = 1000;
constexpr std::uint32_t kMaxClockHz = 100000000;
constexpr std::uint8_t kMaxWritableRegister = 0x18;
/** Periodic checkpoints (every `check` cycles) a trace may ask for. */
constexpr std::uint64_t kMaxPeriodicCheckpoints = 1000000;

enum class Model : std::uint8_t { A, B };

struct Write
{
    std::uint64_t cycle = 0;
    std::uint8_t reg = 0;
    std::uint8_t value = 0;
};

struct Trace
{
    std::uint32_t clockHz = 0;
    Model model = Model::B;
    std::uint64_t check = 0; // 0: only after each write and at the end
    std::vector<Write> writes;
    std::uint64_t endCycle = 0;
};

struct TraceResult
{
    bool ok = false;
    Trace trace;
    text::ErrorList errors;
    std::string message() const { return errors.format(); }
};

/** Parses a trace. Never throws. */
TraceResult readTrace (std::string_view text) noexcept;

/** Whether the trace satisfies every rule of the format (ranges, order, bounds). */
bool validateTrace (const Trace& t, std::string* why = nullptr);

/** Canonical text: fixed layout, lower-case hex, two digits, `check` only when set, no comments.
    Returns false (and leaves `out` empty) when the trace does not validate. */
bool writeTrace (const Trace& t, std::string& out);

/** One point at which the digital state is compared. */
struct Checkpoint
{
    enum class Kind : std::uint8_t
    {
        periodic,   // at a multiple of `check`, before the writes of that cycle
        afterWrite, // right after write `write`
        end         // at endCycle, after every write
    };
    Kind kind = Kind::end;
    std::uint64_t cycle = 0;
    std::uint32_t write = 0; // afterWrite only: index into Trace::writes
};

/** The checkpoints in the order the replay visits them: after every write, every `check` cycles
    (strictly before the end), and at the end. Returns false for a trace that does not validate. */
bool checkpointSchedule (const Trace& t, std::vector<Checkpoint>& out);
} // namespace vs::bside::trace
