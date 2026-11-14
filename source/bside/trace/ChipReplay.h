#pragma once

// The replay driver (Replay.h) bound to the real chip (source/bside/chip/): the setup and snapshot
// callables it needs, and a one-call replay. JUCE-free.

#include "bside/chip/Chip.h"
#include "bside/trace/Replay.h"

namespace vs::bside::trace
{
inline chip::Model chipModel (Model m) noexcept { return m == Model::A ? chip::Model::A : chip::Model::B; }

/** Configures model and clock from the trace (Authentic resolution) and resets the chip to cycle 0. */
inline void setupChip (chip::Chip& c, const Trace& t) noexcept
{
    chip::Config cfg;
    cfg.model = chipModel (t.model);
    cfg.clockHz = t.clockHz;
    cfg.authentic = true;
    c.prepare (cfg);
}

/** The 20 fields of the .bsref format (ReferenceFormat.h), in its order:
    acc1-3 (24-bit accumulator), noise1-3 (23-bit register), env1-3 (counter), phase1-3 (0 attack,
    1 decay/sustain, 2 release), rate1-3 (15-bit rate counter), div1-3 (exponential divider counter),
    rd1B, rd1C (the read registers). */
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
        v.v[12 + n] = s.env[n].rateCounter;
        v.v[15 + n] = s.env[n].divider;
    }
    v.v[18] = s.osc3Output;
    v.v[19] = s.env3Output;
    return v;
}

/** Replays `trace` on `chip` against `reference` (see replay() for the checks made first). */
inline ReplayResult replayChip (chip::Chip& c, const Trace& trace, std::string_view traceBytes, const Reference& reference)
{
    return replay (c, trace, traceBytes, reference, setupChip, snapshotChip);
}
} // namespace vs::bside::trace
