#pragma once

// Reference files for the trace replay (docs/architecture.md 8.9.3). Both are strict, bounded text
// formats read like the trace file (at most 16 MB, one pass, no exception leaves the reader, the
// first 8 problems reported with line and column). Neither ever holds audio.
//
// .bsref: the digital state of the reference emulator at every checkpoint of one trace.
//
//   bside-ref 1
//   trace-sha256 <64 hex digits>      SHA-256 of the trace file's bytes
//   adapter <40 or 64 hex digits>     commit of the adapter that wrote this file (hashes only)
//   emulator <40 or 64 hex digits>    commit of the reference emulator
//   state <cycle> <20 hex fields>     one line per checkpoint, in the order of checkpointSchedule():
//     acc1 acc2 acc3 noise1 noise2 noise3 env1 env2 env3 phase1 phase2 phase3
//     rate1 rate2 rate3 div1 div2 div3 rd1B rd1C
//
// Field meaning and range: accN 24-bit accumulator; noiseN 23-bit shift register; envN 8-bit
// envelope counter; phaseN envelope phase (0 attack, 1 decay/sustain, 2 release); rateN 15-bit
// rate counter; divN exponential divider counter (0..255); rd1B, rd1C the two read registers.
//
// .bsspec: the audio features of the trace's output at 48 kHz, in the render-regression
// feature layout (23 bands, 25 ms frames, 0.1 dB integers), plus the 1/12-octave averaged
// spectrum of marked segments (the analog measurements).
//
//   bside-spec 1
//   trace-sha256 <64 hex digits>
//   rate 48000
//   bands 23
//   frames N                          1 .. 100000
//   frame <level> <23 band values>    exactly N lines; level = dBFS RMS of the hop, band = dBFS of the
//                                     band's mean square, all in 0.1 dB (-1200 .. 300)
//   segment <name> <first> <end> <120 values>   0 .. 64 lines; frames [first, end); bins are
//                                     20 Hz * 2^(k/12), k = 0..119, 0.1 dB
//
// Comments (#) and blank lines are allowed anywhere after the header line.

#include "bside/text/TextScanner.h"
#include "bside/trace/TraceFormat.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vs::bside::trace
{
constexpr std::size_t kMaxReferenceBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxReferenceLineBytes = 256;
constexpr std::size_t kMaxCheckpoints = kMaxWrites + static_cast<std::size_t> (kMaxPeriodicCheckpoints) + 1;
constexpr int kStateFields = 20;

struct StateVector
{
    std::array<std::uint32_t, kStateFields> v {};
    bool operator== (const StateVector& o) const noexcept { return v == o.v; }
};
extern const char* const kStateFieldNames[kStateFields];
extern const std::uint32_t kStateFieldMax[kStateFields];

struct Reference
{
    struct Point
    {
        std::uint64_t cycle = 0;
        StateVector state;
    };
    std::string traceSha256; // 64 lower-case hex digits
    std::string adapter;     // lower-case hex, 40 or 64 digits
    std::string emulator;
    std::vector<Point> points;
};

struct ReferenceResult
{
    bool ok = false;
    Reference reference;
    text::ErrorList errors;
    std::string message() const { return errors.format(); }
};

ReferenceResult readReference (std::string_view text) noexcept;
bool validateReference (const Reference& r, std::string* why = nullptr);
/** Canonical text (fixed-width lower-case hex). False when the reference does not validate. */
bool writeReference (const Reference& r, std::string& out);

/** Whether `r` belongs to `t`: same checkpoint count and cycles as checkpointSchedule(t), and (when
    traceBytes is given) the SHA-256 of those bytes. `why` names the first difference. */
bool referenceMatchesTrace (const Reference& r, const Trace& t, const std::string_view* traceBytes, std::string* why = nullptr);

// ---- .bsspec ----
constexpr int kSpecBands = 23;
constexpr int kSpecBins = 120;
constexpr int kSpecMaxFrames = 100000;
constexpr int kSpecMaxSegments = 64;
constexpr int kSpecMinTenthDb = -1200;
constexpr int kSpecMaxTenthDb = 300;
constexpr std::size_t kMaxSpecBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxSpecLineBytes = 1024;

struct SpecSegment
{
    std::string name; // [a-z][a-z0-9_]{0,31}
    std::uint32_t firstFrame = 0, endFrame = 0;
    std::array<std::int16_t, kSpecBins> spectrum {};
};

struct Spec
{
    std::string traceSha256;
    int frames = 0;
    std::vector<std::int16_t> levelTenthDb; // frames
    std::vector<std::int16_t> bandTenthDb;  // frames * kSpecBands
    std::vector<SpecSegment> segments;

    int band (int frame, int b) const { return bandTenthDb[static_cast<std::size_t> (frame * kSpecBands + b)]; }
};

struct SpecResult
{
    bool ok = false;
    Spec spec;
    text::ErrorList errors;
    std::string message() const { return errors.format(); }
};

SpecResult readSpec (std::string_view text) noexcept;
bool validateSpec (const Spec& s, std::string* why = nullptr);
bool writeSpec (const Spec& s, std::string& out);
} // namespace vs::bside::trace
