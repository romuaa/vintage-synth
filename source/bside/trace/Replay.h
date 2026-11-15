#pragma once

// The replay driver (docs/architecture.md 8.9.4): it sets the chip up from the trace, advances it
// to each write's cycle, writes, and compares the digital state with the reference at every
// checkpoint. It is a template over the chip: the tests prove it against a stub, and
// ChipReplay.h binds it to the real chip (source/bside/chip/, S2a). JUCE-free.
//
// Chip requirements (the contract's interface, 8.4.7):
//   void advanceCycles (std::uint64_t cycles) noexcept;       // digital state only
//   void write (std::uint8_t reg, std::uint8_t value) noexcept; // at the current cycle
//   std::uint64_t cycle() const noexcept;
// The reference must belong to the trace: replay() checks the checkpoint schedule and the
// reference's SHA-256 against `traceBytes`, the exact bytes of the trace file the reference was
// generated from (the original file, not a re-written one), and refuses to run otherwise.
// The caller supplies two callables:
//   setup (chip, trace)   configures model and clock from the trace and resets the chip (cycle 0)
//   snapshot (chip)       returns a StateVector in the field order of the .bsref format

#include "bside/trace/ReferenceFormat.h"
#include "bside/trace/TraceFormat.h"

#include <string>
#include <string_view>
#include <vector>

namespace vs::bside::trace
{
struct Mismatch
{
    std::size_t checkpoint = 0;
    std::uint64_t cycle = 0;
    int field = 0; // index into kStateFieldNames
    std::uint32_t ours = 0, expected = 0;
};

struct ReplayResult
{
    bool ok = false;
    std::string error;                 // set when the replay could not run (reference does not fit the trace)
    std::size_t compared = 0;          // checkpoints compared
    std::size_t mismatchedPoints = 0;  // checkpoints with at least one differing field
    std::vector<Mismatch> first;       // the first differing fields (at most 8), in visiting order
};

constexpr std::size_t kMaxReportedMismatches = 8;

template <class Chip, class Setup, class Snapshot>
ReplayResult replay (Chip& chip, const Trace& trace, std::string_view traceBytes, const Reference& reference, Setup&& setup, Snapshot&& snapshot)
{
    ReplayResult result;
    std::string why;
    if (! referenceMatchesTrace (reference, trace, &traceBytes, &why))
    {
        result.error = why;
        return result;
    }
    std::vector<Checkpoint> schedule;
    checkpointSchedule (trace, schedule);

    setup (chip, trace);
    for (std::size_t i = 0; i < schedule.size(); ++i)
    {
        const auto& cp = schedule[i];
        if (cp.kind == Checkpoint::Kind::afterWrite)
        {
            const auto& w = trace.writes[cp.write];
            if (w.cycle > chip.cycle())
                chip.advanceCycles (w.cycle - chip.cycle());
            chip.write (w.reg, w.value);
        }
        else if (cp.cycle > chip.cycle())
            chip.advanceCycles (cp.cycle - chip.cycle());

        const StateVector ours = snapshot (chip);
        ++result.compared;
        bool differs = false;
        for (int k = 0; k < kStateFields; ++k)
        {
            const auto idx = static_cast<std::size_t> (k);
            if (ours.v[idx] != reference.points[i].state.v[idx])
            {
                differs = true;
                if (result.first.size() < kMaxReportedMismatches)
                    result.first.push_back ({ i, cp.cycle, k, ours.v[idx], reference.points[i].state.v[idx] });
            }
        }
        if (differs)
            ++result.mismatchedPoints;
    }
    result.ok = result.mismatchedPoints == 0;
    return result;
}

/** "checkpoint 12 (cycle 4096): acc1 ours 0xff00, expected 0xfe00; ...": the cycle is decimal, the fields hex. */
inline std::string hexText (std::uint32_t v)
{
    std::string s;
    do
    {
        s.insert (s.begin(), "0123456789abcdef"[v & 15u]);
        v >>= 4;
    } while (v != 0);
    return "0x" + s;
}

inline std::string describe (const ReplayResult& r)
{
    if (! r.error.empty())
        return "replay did not run: " + r.error;
    std::string s;
    for (const auto& m : r.first)
    {
        if (! s.empty())
            s += "; ";
        s += "checkpoint " + std::to_string (m.checkpoint) + " (cycle " + std::to_string (m.cycle) + "): " + kStateFieldNames[m.field]
             + " ours " + hexText (m.ours) + ", expected " + hexText (m.expected);
    }
    if (r.mismatchedPoints > 0)
        s += " [" + std::to_string (r.mismatchedPoints) + " of " + std::to_string (r.compared) + " checkpoints differ]";
    return s;
}
} // namespace vs::bside::trace
