#pragma once

// The compiled patch program (docs/architecture.md 8.6): a fixed-size, trivially copyable struct
// of fewer than 8 KB with every jump target resolved to a row index and every number in register
// units. It has no padding, so two programs are the same program exactly when their bytes are
// equal (operator==). JUCE-free. The runtime (tick engine, lanes) is not part of this step.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace vs::bside::program
{
constexpr int kNumLanes = 7;
constexpr int kMaxRowsPerLane = 64;
constexpr int kMaxLabelsPerLane = 16;
/** Not in the contract's list of limits, but needed to keep the compiled program under 8 KB:
    all commands of all lanes together (a row has at least one). */
constexpr int kMaxCommands = 512;
constexpr int kMaxRows = kNumLanes * kMaxRowsPerLane;
constexpr std::uint8_t kNoRelease = 0xff;

constexpr std::size_t kMaxTextBytes = 32 * 1024;
constexpr int kMaxLines = 1024;
constexpr int kMaxLineBytes = 200;
constexpr int kMaxErrors = 8;

enum Lane : std::uint8_t { Wave1, Wave2, Wave3, Pulse1, Pulse2, Pulse3, Filter };
enum class LaneKind : std::uint8_t { Wave, Pulse, Filter };
constexpr LaneKind laneKind (int lane) noexcept
{
    return lane < 3 ? LaneKind::Wave : lane < 6 ? LaneKind::Pulse : LaneKind::Filter;
}
const char* laneName (int lane) noexcept; // "wave1" ... "filter"

enum class Op : std::uint8_t
{
    none = 0,
    wave,          // u0 control bits; a PitchKind; b pitch value
    pitch,         // a PitchKind; b pitch value
    gateOn,
    gateOff,
    adsr,          // a = attack<<4 | decay; b = sustain<<4 | release
    pulseSet,      // a 0..4095
    pulseSweep,    // a delta (-4095..4095); u0 ticks 1..255
    pulsePingPong, // a lo; b hi (lo < hi); c step 1..4095
    cutoff,        // a 0..2047
    cutoffSweep,   // a delta (-2047..2047); u0 ticks 1..255
    res,           // a 0..15
    volume,        // a 0..15
    mode,          // u0 mask: 1 low-pass, 2 band-pass, 4 high-pass
    route,         // u0 mask: bit n-1 = oscillator n
    jump,          // a target row (index within the lane)
    repeat,        // a target row; b count 1..255
    end,
    count_
};

enum class PitchKind : std::int16_t { unchanged = 0, relative = 1, absolute = 2 };
constexpr int kMaxRelativeHundredths = 9600; // +-96 semitones
constexpr int kMaxNote = 127;

/** Control register bits of the wave command. */
enum ControlBit : std::uint8_t
{
    bitGate = 1, bitSync = 2, bitRing = 4, bitTest = 8, bitTri = 16, bitSaw = 32, bitPulse = 64, bitNoise = 128
};

struct Cmd
{
    std::uint8_t op = 0;
    std::uint8_t u0 = 0;
    std::int16_t a = 0, b = 0, c = 0;
};

struct Row
{
    std::uint16_t firstCmd = 0; // index into Program::cmds
    std::uint8_t numCmds = 0;
    std::uint8_t delay = 0;     // ticks before the next row: default 1; 0 for a row ending in jump or
                                // repeat (a flow row costs no tick); always 1 for end, sweep, ping-pong
};

struct LaneInfo
{
    std::uint16_t firstRow = 0; // index into Program::rows
    std::uint8_t numRows = 0;
    std::uint8_t release = 0;   // row of the `release` label; kNoRelease if none (see emptyProgram)
};

enum class RestartMode : std::uint8_t { off = 0, classic = 1, phase = 2 };

struct Restart
{
    std::uint8_t mode = 0;
    std::uint8_t lead = 0;       // ticks 1..8 (default 2, see emptyProgram)
    std::uint8_t gateoff = 0;    // ticks 1..8 (the default is the lead)
    std::uint8_t hasFirstWave = 0;
    std::uint8_t firstWave = 0;  // control bits
    std::uint8_t ad = 0;
    std::uint8_t sr = 0;
    std::uint8_t reserved = 0;   // always 0
};

struct Program
{
    Restart restart;
    LaneInfo lanes[kNumLanes];
    std::uint16_t numRows = 0;
    std::uint16_t numCmds = 0;
    Row rows[kMaxRows];
    Cmd cmds[kMaxCommands];
};

static_assert (std::is_trivially_copyable_v<Program>);
static_assert (std::is_standard_layout_v<Program>);
static_assert (std::has_unique_object_representations_v<Program>, "the program must have no padding");
static_assert (sizeof (Program) <= 8192, "docs/architecture.md 8.6.7: at most 8 KB");

/** The program of an empty table: every lane empty, restart off (lead and gateoff 2). All other bytes zero. */
Program emptyProgram() noexcept;

bool operator== (const Program& a, const Program& b) noexcept;
inline bool operator!= (const Program& a, const Program& b) noexcept { return ! (a == b); }

/** True for a command that must end its row (a flow command, a sweep or a ping-pong). */
constexpr bool isRowTerminator (Op op) noexcept
{
    return op == Op::jump || op == Op::repeat || op == Op::end || op == Op::pulseSweep
           || op == Op::pulsePingPong || op == Op::cutoffSweep;
}
/** True for a command after which the lane does not continue within the same tick (and that takes no ticks). */
constexpr bool isTickBound (Op op) noexcept
{
    return op == Op::pulseSweep || op == Op::pulsePingPong || op == Op::cutoffSweep || op == Op::end;
}

/** Whether every field is inside the ranges of the contract and the layout is consistent
    (lanes in order and contiguous, targets inside their lane, unused storage zero, no zero-delay
    loop). Fills `why` (if given) with the first problem. A program produced by the reader always passes. */
bool validate (const Program& p, std::string* why = nullptr);

/** A cycle of rows that all have delay 0 and do not stop the lane: the row where it is detected
    (lowest row index on the path), or -1. Rows of the lane must be valid. */
int findZeroDelayLoop (const Program& p, int lane) noexcept;

/** Canonical text of a program (8.6.5). Returns false (and leaves `out` empty) for a program that
    does not validate. Deterministic; no comments; label names are generated: `release` and `r<row>`. */
bool writeTable (const Program& p, std::string& out);
} // namespace vs::bside::program
