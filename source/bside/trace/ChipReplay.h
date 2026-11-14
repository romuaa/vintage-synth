#pragma once

// The replay driver (Replay.h) bound to the real chip (source/bside/chip/): the setup and snapshot
// callables it needs, a one-call replay, and the exact replay of the committed reference data with the
// shape exemption of docs/architecture.md 8.15.8 (ShapeExemption.h). JUCE-free.

#include "bside/chip/Chip.h"
#include "bside/trace/Replay.h"
#include "bside/trace/ShapeExemption.h"

#include <optional>

namespace vs::bside::trace
{
inline chip::Model chipModel (Model m) noexcept { return m == Model::A ? chip::Model::A : chip::Model::B; }

/** Configures model and clock from the trace (Authentic resolution) and resets the chip to cycle 0. */
inline void setupChip (chip::Chip& c, const Trace& t, chip::Model model) noexcept
{
    chip::Config cfg;
    cfg.model = model;
    cfg.clockHz = t.clockHz;
    cfg.authentic = true;
    c.prepare (cfg);
}
inline void setupChip (chip::Chip& c, const Trace& t) noexcept { setupChip (c, t, chipModel (t.model)); }

/** The 20 fields of the .bsref format (ReferenceFormat.h, 8.15.1), in its order:
    acc1-3 (24-bit accumulator), noise1-3 (23-bit register), env1-3 (counter), phase1-3 (0 attack,
    1 decay/sustain, 2 release), rate1-3 (rate counter position 0 .. 32 766), div1-3 (divider counter in
    bits 0-6, pending rate reset in bit 7), rd1B (oscillator 3's read value, bits 11..4), rd1C
    (envelope 3's read sample). */
inline StateVector snapshotChip (const chip::Chip& c) noexcept
{
    const chip::DigitalState s = c.state();
    StateVector v;
    for (std::size_t n = 0; n < 3; ++n)
    {
        v.v[n] = s.osc[n].acc;
        v.v[3 + n] = s.osc[n].noise;
        v.v[6 + n] = s.env[n].counter;
        v.v[9 + n] = s.env[n].phase;
        v.v[12 + n] = s.env[n].position;
        v.v[15 + n] = static_cast<std::uint32_t> (s.env[n].divider) | (s.env[n].resetPending ? 0x80u : 0u);
    }
    v.v[18] = s.osc3Output;
    v.v[19] = s.env3Output;
    return v;
}

/** Replays `trace` on `chip` against `reference` (see replay() for the checks made first). */
inline ReplayResult replayChip (chip::Chip& c, const Trace& trace, std::string_view traceBytes, const Reference& reference)
{
    return replay (c, trace, traceBytes, reference, [] (chip::Chip& ch, const Trace& t) { setupChip (ch, t); }, snapshotChip);
}

/** Field indices of the .bsref format that the exemption can cover. */
inline bool fieldExempt (const ShapeExemption& x, int field) noexcept
{
    if (field < 3)
        return x.accExempt (field);
    if (field < 6)
        return x.noiseExempt (field - 3);
    return field == 18 && x.readExempt();
}

struct ExactReplayResult
{
    std::string error;                // set when the replay could not run
    std::size_t checkpoints = 0;
    std::size_t exact = 0;            // every field compared and equal
    std::size_t exempt = 0;           // at least one field exempt (8.15.8), every other field equal
    std::size_t mismatched = 0;       // at least one non-exempt field differs
    std::size_t exemptButEqual = 0;   // exempt checkpoints whose exempt fields happen to equal the reference
    std::size_t fastPathDiffs = 0;    // checkpoints where advanceCycles' full state differs from stepCycle's
    std::vector<Mismatch> first;      // the first non-exempt differences (at most kMaxReportedMismatches)
    std::vector<Write> writesBefore;  // up to 8 writes up to the first mismatch's checkpoint
    bool ok() const noexcept { return error.empty() && mismatched == 0 && fastPathDiffs == 0; }
};

/** The digital replay of the reference data (8.9.4, 8.15.8): our chip is stepped cycle by cycle with
    stepCycle (the normative definition) and every checkpoint is compared exactly, except the fields the
    ShapeExemption marks; a second chip runs the same writes through advanceCycles and must equal the
    stepped one in its full DigitalState at every checkpoint. `model` overrides the trace's model (the
    envelope traces hold for both models; docs/bside-oracle-adapter.md). Not for the audio thread. */
inline ExactReplayResult replayChipExact (const Trace& trace, std::string_view traceBytes, const Reference& reference,
                                          std::optional<chip::Model> model = std::nullopt, bool exemptOwnPullDown = true)
{
    ExactReplayResult r;
    std::string why;
    if (! referenceMatchesTrace (reference, trace, &traceBytes, &why))
    {
        r.error = why;
        return r;
    }
    std::vector<Checkpoint> schedule;
    checkpointSchedule (trace, schedule);
    const chip::Model m = model.value_or (chipModel (trace.model));
    chip::Chip stepped, fast;
    setupChip (stepped, trace, m);
    setupChip (fast, trace, m);
    ShapeExemption tracker (m, exemptOwnPullDown);
    ShapeExemptionSink sink { &tracker };
    auto advanceTo = [&] (std::uint64_t c) {
        while (stepped.cycle() < c)
            stepped.stepCycle (sink);
        if (fast.cycle() < c)
            fast.advanceCycles (c - fast.cycle());
    };
    std::size_t lastWrite = 0;
    bool haveWrite = false;
    for (std::size_t i = 0; i < schedule.size(); ++i)
    {
        const auto& cp = schedule[i];
        if (cp.kind == Checkpoint::Kind::afterWrite)
        {
            const auto& w = trace.writes[cp.write];
            advanceTo (w.cycle);
            stepped.write (w.reg, w.value);
            fast.write (w.reg, w.value);
            tracker.write (w.reg, w.value);
            lastWrite = cp.write;
            haveWrite = true;
        }
        else
            advanceTo (cp.cycle);

        ++r.checkpoints;
        if (! (fast.state() == stepped.state()))
            ++r.fastPathDiffs;
        const StateVector ours = snapshotChip (stepped);
        const StateVector& ref = reference.points[i].state;
        bool differs = false, anyExempt = false, exemptDiffers = false;
        for (int k = 0; k < kStateFields; ++k)
        {
            const auto ki = static_cast<std::size_t> (k);
            const bool ex = fieldExempt (tracker, k);
            anyExempt = anyExempt || ex;
            if (ours.v[ki] == ref.v[ki])
                continue;
            if (ex)
            {
                exemptDiffers = true;
                continue;
            }
            differs = true;
            if (r.first.size() < kMaxReportedMismatches)
            {
                if (r.first.empty() && haveWrite)
                    for (std::size_t b = lastWrite + 1 > 8 ? lastWrite + 1 - 8 : 0; b <= lastWrite; ++b)
                        r.writesBefore.push_back (trace.writes[b]);
                r.first.push_back ({ i, cp.cycle, k, ours.v[ki], ref.v[ki] });
            }
        }
        if (differs)
            ++r.mismatched;
        else if (anyExempt)
        {
            ++r.exempt;
            if (! exemptDiffers)
                ++r.exemptButEqual;
        }
        else
            ++r.exact;
    }
    return r;
}
} // namespace vs::bside::trace
