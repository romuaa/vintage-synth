#include "bside/engine/ProgramEngine.h"

#include "bside/chip/ChipTypes.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace vs::bside::engine
{
using namespace program;

namespace
{
constexpr std::uint8_t kGate = 0x01, kTest = 0x08;
constexpr double kMaxFrequencyAuthentic = 65535.0;
constexpr double kMaxPulse = 4095.0, kMaxCutoff = 2047.0;
/** A fundamental of 12 kHz is the most Smooth resolution plays (8.4.6). */
constexpr double kMaxSmoothHz = 12000.0;
/** The runtime's backstop for the rows of one burst; the reader's guarantee (8.13) is 64. */
constexpr int kBurstCap = 2 * kMaxRowsPerTick;

bool sameValue (double a, double b) noexcept
{
    return std::bit_cast<std::uint64_t> (a) == std::bit_cast<std::uint64_t> (b);
}

double finiteOr (double v, double fallback) noexcept { return std::isfinite (v) ? v : fallback; }

double roundHalfUp (double v) noexcept { return std::floor (v + 0.5); }

const Cmd& lastCommand (const Program& p, const Row& row) noexcept { return p.cmds[row.firstCmd + row.numCmds - 1]; }
} // namespace

void ProgramEngine::prepare (const EngineConfig& config) noexcept
{
    cfg = config;
    cfg.clockHz = std::clamp<std::uint32_t> (cfg.clockHz, 1000u, 100000000u);
    host = HostInputs {};
    prog = emptyProgram();
    for (auto& l : lanes)
        l = LaneRun {};
    for (auto& w : wave)
        w = WaveValues {};
    for (auto& p : pulse)
        p = PulseValues {};
    filt = FilterValues {};
    shadow = Shadow {};
    nextTick = 0;
    tick = 0;
    running = false;
    held = false;
    firstWaveActive = false;
    inRestartState = false;
    restartCycle = 0;
    restartState = Restart {};
}

void ProgramEngine::setTickPeriod (std::uint32_t cycles) noexcept
{
    period = std::clamp<std::uint32_t> (cycles, 256u, 1u << 30);
}

bool ProgramEngine::anyWaveLaneRunning() const noexcept
{
    if (! running)
        return false;
    for (int n = 0; n < 3; ++n)
        if (lanes[n].active)
            return true;
    return false;
}

// ---- effective values -------------------------------------------------------------------------------

bool ProgramEngine::oscActive (int n) const noexcept
{
    return host.osc[n].enabled || prog.lanes[n].numRows > 0;
}

double ProgramEngine::frequencyRegister (double semitones) const noexcept
{
    const double m = finiteOr (semitones, 60.0);
    const double hz = 440.0 * std::exp2 ((std::clamp (m, -200.0, 400.0) - 69.0) / 12.0);
    const double reg = hz * 16777216.0 / static_cast<double> (cfg.clockHz);
    if (cfg.authentic)
        return std::clamp (roundHalfUp (reg), 1.0, kMaxFrequencyAuthentic);
    const double top = std::min (kMaxSmoothHz * 16777216.0 / static_cast<double> (cfg.clockHz), chip::kMaxSmoothFrequency - 1.0);
    return std::clamp (reg, 0.0, top);
}

double ProgramEngine::effFrequency (int n) const noexcept
{
    const auto& w = wave[n];
    const double hostM = host.osc[n].semitones;
    if (! w.pitchOwned)
        return frequencyRegister (hostM);
    return frequencyRegister (w.pitchAbsolute ? w.pitch : finiteOr (hostM, 60.0) + w.pitch);
}

double ProgramEngine::hostPulse (int n) const noexcept
{
    return std::clamp (finiteOr (static_cast<double> (host.osc[n].pulseWidth), 2048.0), 0.0, kMaxPulse);
}

double ProgramEngine::effPulse (int n) const noexcept
{
    const double base = pulse[n].owned ? pulse[n].value : hostPulse (n);
    const double v = std::clamp (base + finiteOr (static_cast<double> (host.macro.pulseWidthOffset), 0.0), 0.0, kMaxPulse);
    return cfg.authentic ? roundHalfUp (v) : v;
}

std::uint8_t ProgramEngine::effControl (int n) const noexcept
{
    const auto& w = wave[n];
    const auto& h = host.osc[n];
    const bool gate = w.gateOwned ? w.gate : (held && h.enabled);
    if (firstWaveActive && oscActive (n))
        return static_cast<std::uint8_t> ((prog.restart.firstWave & ~kGate) | (held ? kGate : 0));
    std::uint8_t base = 0;
    if (w.ctrlOwned)
        base = w.ctrl;
    else if (h.enabled)
        base = static_cast<std::uint8_t> ((h.waveBits & 0xf0) | (h.sync ? 0x02 : 0) | (h.ring ? 0x04 : 0));
    return static_cast<std::uint8_t> (base | (gate ? kGate : 0));
}

void ProgramEngine::effEnvelope (int n, std::uint8_t& ad, std::uint8_t& sr) const noexcept
{
    if (wave[n].envOwned)
    {
        ad = wave[n].ad;
        sr = wave[n].sr;
    }
    else
    {
        ad = host.osc[n].attackDecay;
        sr = host.osc[n].sustainRelease;
    }
}

double ProgramEngine::hostCutoff() const noexcept
{
    return std::clamp (finiteOr (static_cast<double> (host.filter.cutoff), 1024.0), 0.0, kMaxCutoff);
}

double ProgramEngine::effCutoff() const noexcept
{
    const double base = filt.cutoffOwned ? filt.cutoff : hostCutoff();
    const double v = std::clamp (base + finiteOr (static_cast<double> (host.macro.cutoffOffset), 0.0), 0.0, kMaxCutoff);
    return cfg.authentic ? roundHalfUp (v) : v;
}

std::uint8_t ProgramEngine::effResRoute() const noexcept
{
    const unsigned res = filt.resOwned ? filt.res : host.filter.resonance;
    const unsigned route = filt.routeOwned ? filt.route : host.filter.routing;
    return static_cast<std::uint8_t> (((res & 15u) << 4) | (route & 7u));
}

std::uint8_t ProgramEngine::effModeVol() const noexcept
{
    const unsigned mode = filt.modeOwned ? filt.mode : host.filter.mode;
    const unsigned vol = filt.volOwned ? filt.vol : host.filter.volume;
    return static_cast<std::uint8_t> ((host.filter.osc3Off ? 0x80u : 0u) | ((mode & 7u) << 4) | (vol & 15u));
}

// ---- writes -----------------------------------------------------------------------------------------

void ProgramEngine::emitFrequency (int n, std::uint64_t cycle, ProgramSink& sink) noexcept
{
    const double v = effFrequency (n);
    sink.frequency (cycle, n, v);
    shadow.freq[n] = v;
}

void ProgramEngine::emitPulse (int n, std::uint64_t cycle, ProgramSink& sink) noexcept
{
    const double v = effPulse (n);
    sink.pulseWidth (cycle, n, v);
    shadow.pw[n] = v;
}

void ProgramEngine::emitControl (int n, std::uint64_t cycle, ProgramSink& sink) noexcept
{
    const std::uint8_t v = effControl (n);
    sink.control (cycle, n, v);
    shadow.ctrl[n] = v;
}

void ProgramEngine::emitEnvelope (int n, std::uint64_t cycle, ProgramSink& sink) noexcept
{
    std::uint8_t ad = 0, sr = 0;
    effEnvelope (n, ad, sr);
    sink.envelope (cycle, n, ad, sr);
    shadow.ad[n] = ad;
    shadow.sr[n] = sr;
}

void ProgramEngine::emitCutoff (std::uint64_t cycle, ProgramSink& sink) noexcept
{
    const double v = effCutoff();
    sink.cutoff (cycle, v);
    shadow.cutoff = v;
}

void ProgramEngine::emitResRoute (std::uint64_t cycle, ProgramSink& sink) noexcept
{
    const std::uint8_t v = effResRoute();
    sink.resonanceRouting (cycle, v);
    shadow.resRoute = v;
}

void ProgramEngine::emitModeVol (std::uint64_t cycle, ProgramSink& sink) noexcept
{
    const std::uint8_t v = effModeVol();
    sink.modeVolume (cycle, v);
    shadow.modeVol = v;
}

void ProgramEngine::syncWave (int n, std::uint64_t cycle, ProgramSink& sink) noexcept
{
    if (oscActive (n))
    {
        if (! sameValue (effFrequency (n), shadow.freq[n]))
            emitFrequency (n, cycle, sink);
        std::uint8_t ad = 0, sr = 0;
        effEnvelope (n, ad, sr);
        if (ad != shadow.ad[n] || sr != shadow.sr[n])
            emitEnvelope (n, cycle, sink);
    }
    if (effControl (n) != shadow.ctrl[n])
        emitControl (n, cycle, sink);
}

void ProgramEngine::syncPulse (int n, std::uint64_t cycle, ProgramSink& sink) noexcept
{
    if ((oscActive (n) || pulse[n].owned) && ! sameValue (effPulse (n), shadow.pw[n]))
        emitPulse (n, cycle, sink);
}

void ProgramEngine::syncFilter (std::uint64_t cycle, ProgramSink& sink) noexcept
{
    if (! sameValue (effCutoff(), shadow.cutoff))
        emitCutoff (cycle, sink);
    if (effResRoute() != shadow.resRoute)
        emitResRoute (cycle, sink);
    if (effModeVol() != shadow.modeVol)
        emitModeVol (cycle, sink);
}

void ProgramEngine::sync (std::uint64_t cycle, ProgramSink& sink) noexcept
{
    for (int n = 0; n < 3; ++n)
        syncWave (n, cycle, sink);
    for (int n = 0; n < 3; ++n)
        syncPulse (n, cycle, sink);
    syncFilter (cycle, sink);
}

// ---- restart ----------------------------------------------------------------------------------------

void ProgramEngine::applyRestart (std::uint64_t cycle, const Restart& r, ProgramSink& sink) noexcept
{
    const bool phase = r.mode == static_cast<std::uint8_t> (RestartMode::phase);
    for (int n = 0; n < 3; ++n)
    {
        sink.envelope (cycle, n, r.ad, r.sr);
        shadow.ad[n] = r.ad;
        shadow.sr[n] = r.sr;
        const auto c = static_cast<std::uint8_t> ((shadow.ctrl[n] & ~kGate) | (phase ? kTest : 0));
        sink.control (cycle, n, c);
        shadow.ctrl[n] = c;
    }
}

void ProgramEngine::enterIdle (std::uint64_t cycle, const Restart& restart, RestartPolicy policy, ProgramSink& sink) noexcept
{
    running = false;
    held = false;
    if (policy == RestartPolicy::authentic || restart.mode == static_cast<std::uint8_t> (RestartMode::off))
    {
        inRestartState = false;
        return;
    }
    applyRestart (cycle, restart, sink);
    inRestartState = true;
    restartCycle = cycle;
    restartState = restart;
}

// ---- notes and ticks --------------------------------------------------------------------------------

void ProgramEngine::noteOn (std::uint64_t cycle, const Program& program, RestartPolicy policy, bool stolen, ProgramSink& sink) noexcept
{
    prog = program;
    for (auto& l : lanes)
        l = LaneRun {};
    for (auto& w : wave)
        w = WaveValues {};
    for (auto& p : pulse)
        p = PulseValues {};
    filt = FilterValues {};
    held = true;
    running = true;
    tick = 0;
    nextTick = cycle;
    firstWaveActive = false;

    const bool restartOn = prog.restart.mode != static_cast<std::uint8_t> (RestartMode::off);
    if (stolen && policy != RestartPolicy::authentic)
    {
        sink.resetChip (cycle);
        resetChipShadow();
        inRestartState = false;
        if (restartOn)
        {
            applyRestart (cycle, prog.restart, sink);
            inRestartState = true;
            restartCycle = cycle;
            restartState = prog.restart;
        }
    }

    if (inRestartState && std::memcmp (&restartState, &prog.restart, sizeof (Restart)) != 0)
        inRestartState = false; // preconditioned with another restart section: does not count

    if (policy == RestartPolicy::clean)
    {
        const std::uint64_t lead = static_cast<std::uint64_t> (prog.restart.lead) * period;
        const bool fullLead = inRestartState && cycle >= restartCycle && cycle - restartCycle >= lead;
        if (! fullLead)
            sink.clearEnvelopeTimers (cycle);
    }
    if (policy != RestartPolicy::authentic && ! restartOn)
    {
        // A retrigger always starts an attack: a gate that is still high is cleared first.
        for (int n = 0; n < 3; ++n)
            if ((shadow.ctrl[n] & kGate) != 0)
            {
                const auto c = static_cast<std::uint8_t> (shadow.ctrl[n] & ~kGate);
                sink.control (cycle, n, c);
                shadow.ctrl[n] = c;
            }
    }

    // Tick 0 runs at the note-on cycle.
    runTick (sink);
}

void ProgramEngine::runTick (ProgramSink& sink) noexcept
{
    if (! running)
        return;
    const std::uint64_t cycle = nextTick;
    tickBody (cycle, sink);
    ++tick;
    nextTick = cycle + period;
}

void ProgramEngine::tickBody (std::uint64_t cycle, ProgramSink& sink) noexcept
{
    const bool restartOn = prog.restart.mode != static_cast<std::uint8_t> (RestartMode::off);
    if (tick == 0)
    {
        if (restartOn && ! inRestartState)
            applyRestart (cycle, prog.restart, sink);
        inRestartState = false;
        firstWaveActive = restartOn && prog.restart.hasFirstWave != 0;
        for (int n = 0; n < 3; ++n)
        {
            LaneRun& l = lanes[n];
            l.active = prog.lanes[n].numRows > 0;
            if (firstWaveActive)
                l.wait = 1; // row 0 runs at tick 1
        }
        for (int n = 3; n < kNumLanes; ++n)
            lanes[n].active = prog.lanes[n].numRows > 0;
    }
    else if (tick == 1)
        firstWaveActive = false;

    for (int n = 0; n < 3; ++n)
    {
        tickLane (n, cycle, sink);
        syncWave (n, cycle, sink);
    }
    for (int n = 0; n < 3; ++n)
    {
        tickLane (3 + n, cycle, sink);
        syncPulse (n, cycle, sink);
    }
    tickLane (6, cycle, sink);
    syncFilter (cycle, sink);
}

void ProgramEngine::noteOff (std::uint64_t cycle, ProgramSink& sink) noexcept
{
    if (! running)
        return;
    held = false;
    for (int lane = 0; lane < kNumLanes; ++lane)
    {
        const LaneInfo& li = prog.lanes[lane];
        if (li.numRows > 0 && li.release != kNoRelease)
        {
            LaneRun& l = lanes[lane];
            l.row = li.release;
            l.wait = 0;
            l.mode = Mode::normal;
            l.active = true;
            execBurst (lane, cycle, sink);
        }
        else if (lane < 3 && li.numRows > 0)
        {
            wave[lane].gateOwned = true;
            wave[lane].gate = false;
            emitControl (lane, cycle, sink);
        }
        if (lane < 3)
            syncWave (lane, cycle, sink);
        else if (lane < 6)
            syncPulse (lane - 3, cycle, sink);
        else
            syncFilter (cycle, sink);
    }
}

// ---- lanes ------------------------------------------------------------------------------------------

void ProgramEngine::tickLane (int lane, std::uint64_t cycle, ProgramSink& sink) noexcept
{
    LaneRun& l = lanes[lane];
    if (! l.active)
        return;
    if (l.wait > 0)
    {
        --l.wait;
        return;
    }
    if (l.mode == Mode::sweeping)
    {
        const LaneInfo& li = prog.lanes[lane];
        const Row& row = prog.rows[li.firstRow + l.row];
        stepSweep (lane, lastCommand (prog, row), cycle, sink);
        if (--l.sweepLeft == 0)
        {
            l.mode = Mode::normal;
            ++l.row;
        }
        return;
    }
    if (l.mode == Mode::pingpong)
    {
        stepPingPong (lane, cycle, sink);
        return;
    }
    execBurst (lane, cycle, sink);
}

void ProgramEngine::execBurst (int lane, std::uint64_t cycle, ProgramSink& sink) noexcept
{
    LaneRun& l = lanes[lane];
    const LaneInfo& li = prog.lanes[lane];
    for (int executed = 0; executed < kBurstCap; ++executed)
    {
        if (l.row >= li.numRows)
        {
            l.active = false;
            return;
        }
        const std::uint16_t rowIndex = l.row;
        const Row& row = prog.rows[li.firstRow + rowIndex];
        std::uint16_t next = static_cast<std::uint16_t> (rowIndex + 1);
        bool stopTick = false;
        for (int c = 0; c < row.numCmds; ++c)
        {
            const Cmd& cmd = prog.cmds[row.firstCmd + c];
            switch (static_cast<Op> (cmd.op))
            {
                case Op::jump: next = static_cast<std::uint16_t> (cmd.a); break;
                case Op::repeat:
                    if (l.repeats[rowIndex] < cmd.b)
                    {
                        ++l.repeats[rowIndex];
                        next = static_cast<std::uint16_t> (cmd.a);
                    }
                    else
                        l.repeats[rowIndex] = 0;
                    break;
                case Op::end:
                    l.active = false;
                    stopTick = true;
                    break;
                case Op::pulseSweep:
                case Op::cutoffSweep:
                    l.mode = Mode::sweeping;
                    l.sweepLeft = cmd.u0;
                    stepSweep (lane, cmd, cycle, sink);
                    if (--l.sweepLeft == 0)
                    {
                        l.mode = Mode::normal;
                        l.row = next;
                    }
                    else
                        l.row = rowIndex; // stays on the sweep row
                    return;
                case Op::pulsePingPong:
                    l.mode = Mode::pingpong;
                    l.pingLo = cmd.a;
                    l.pingHi = cmd.b;
                    l.pingStep = cmd.c;
                    l.pingDir = 1.0;
                    l.row = rowIndex;
                    stepPingPong (lane, cycle, sink);
                    return;
                case Op::none:
                case Op::wave:
                case Op::pitch:
                case Op::gateOn:
                case Op::gateOff:
                case Op::adsr:
                case Op::pulseSet:
                case Op::cutoff:
                case Op::res:
                case Op::volume:
                case Op::mode:
                case Op::route:
                case Op::count_: execCommand (lane, cmd, cycle, sink); break;
            }
        }
        if (stopTick)
            return;
        l.row = next;
        if (row.delay >= 1)
        {
            l.wait = static_cast<std::uint8_t> (row.delay - 1);
            return;
        }
    }
    // Backstop (never reached by an accepted table): the lane resumes at this row on the next tick.
}

void ProgramEngine::execCommand (int lane, const Cmd& c, std::uint64_t cycle, ProgramSink& sink) noexcept
{
    const int n = lane < 3 ? lane : lane - 3;
    switch (static_cast<Op> (c.op))
    {
        case Op::wave:
        case Op::pitch:
        {
            auto& w = wave[n];
            const bool isWave = static_cast<Op> (c.op) == Op::wave;
            const auto kind = static_cast<PitchKind> (c.a);
            if (kind != PitchKind::unchanged)
            {
                w.pitchOwned = true;
                w.pitchAbsolute = kind == PitchKind::absolute;
                w.pitch = w.pitchAbsolute ? static_cast<double> (c.b) : static_cast<double> (c.b) / 100.0;
                emitFrequency (n, cycle, sink);
            }
            if (isWave)
            {
                w.ctrlOwned = true;
                w.ctrl = static_cast<std::uint8_t> (c.u0 & ~kGate);
                w.gateOwned = true;
                w.gate = (c.u0 & kGate) != 0;
                emitControl (n, cycle, sink);
            }
            break;
        }
        case Op::gateOn:
        case Op::gateOff:
            wave[n].gateOwned = true;
            wave[n].gate = static_cast<Op> (c.op) == Op::gateOn;
            emitControl (n, cycle, sink);
            break;
        case Op::adsr:
            wave[n].envOwned = true;
            wave[n].ad = static_cast<std::uint8_t> (c.a);
            wave[n].sr = static_cast<std::uint8_t> (c.b);
            emitEnvelope (n, cycle, sink);
            break;
        case Op::pulseSet:
            pulse[n].owned = true;
            pulse[n].value = c.a;
            emitPulse (n, cycle, sink);
            break;
        case Op::cutoff:
            filt.cutoffOwned = true;
            filt.cutoff = c.a;
            emitCutoff (cycle, sink);
            break;
        case Op::res:
            filt.resOwned = true;
            filt.res = static_cast<std::uint8_t> (c.a);
            emitResRoute (cycle, sink);
            break;
        case Op::route:
            filt.routeOwned = true;
            filt.route = c.u0;
            emitResRoute (cycle, sink);
            break;
        case Op::volume:
            filt.volOwned = true;
            filt.vol = static_cast<std::uint8_t> (c.a);
            emitModeVol (cycle, sink);
            break;
        case Op::mode:
            filt.modeOwned = true;
            filt.mode = c.u0;
            emitModeVol (cycle, sink);
            break;
        case Op::none:
        case Op::pulseSweep:
        case Op::pulsePingPong:
        case Op::cutoffSweep:
        case Op::jump:
        case Op::repeat:
        case Op::end:
        case Op::count_: break; // flow commands and sweeps are handled by execBurst
    }
}

void ProgramEngine::stepSweep (int lane, const Cmd& c, std::uint64_t cycle, ProgramSink& sink) noexcept
{
    const double depth = std::max (0.0, finiteOr (static_cast<double> (host.macro.sweepDepth), 1.0));
    if (lane == 6)
    {
        if (! filt.cutoffOwned)
        {
            filt.cutoffOwned = true;
            filt.cutoff = hostCutoff();
        }
        filt.cutoff = std::clamp (filt.cutoff + static_cast<double> (c.a) * depth, 0.0, kMaxCutoff);
        emitCutoff (cycle, sink);
    }
    else
    {
        const int n = lane - 3;
        if (! pulse[n].owned)
        {
            pulse[n].owned = true;
            pulse[n].value = hostPulse (n);
        }
        pulse[n].value = std::clamp (pulse[n].value + static_cast<double> (c.a) * depth, 0.0, kMaxPulse);
        emitPulse (n, cycle, sink);
    }
}

void ProgramEngine::stepPingPong (int lane, std::uint64_t cycle, ProgramSink& sink) noexcept
{
    LaneRun& l = lanes[lane];
    const int n = lane - 3;
    const double depth = std::max (0.0, finiteOr (static_cast<double> (host.macro.sweepDepth), 1.0));
    if (! pulse[n].owned)
    {
        pulse[n].owned = true;
        pulse[n].value = hostPulse (n);
    }
    double v = std::clamp (pulse[n].value, l.pingLo, l.pingHi) + l.pingDir * l.pingStep * depth;
    if (v >= l.pingHi)
    {
        v = l.pingHi;
        l.pingDir = -1.0;
    }
    else if (v <= l.pingLo)
    {
        v = l.pingLo;
        l.pingDir = 1.0;
    }
    pulse[n].value = v;
    emitPulse (n, cycle, sink);
}
} // namespace vs::bside::engine
