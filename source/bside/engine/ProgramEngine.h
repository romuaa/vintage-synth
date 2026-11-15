#pragma once

// The patch program engine of one note voice (docs/architecture.md 8.6, 8.14, 8.15): the tick clock, the
// seven lanes, every command, hard restart and the three restart policies, turning the compiled table and
// the host parameters into register-level writes for the chip (ProgramSink.h). JUCE-free, no allocation,
// locks, I/O or exceptions after construction; the voice owns a private copy of the program (8.6.7).
//
// Time is the chip cycle (an integer). The caller drives the voice like the chip:
//   - `nextTickCycle()` is when the next tick must run; the caller renders up to that cycle, then calls
//     `runTick()` (or `runUntil`). Ticks never depend on how the caller split the audio into blocks.
//   - `noteOn`, `noteOff`, `sync` and `enterIdle` are called at their cycle after every tick with a cycle
//     <= theirs has run (a tick on the cycle of a note-off runs first: 8.13).
//
// Reading of the contract (every choice is listed in docs/decisions.md, "S2c"):
//   * Writes: a lane command always writes (literally, in row order). A register that no command has
//     written for this note follows the host parameters: the engine writes it when its effective value
//     differs from the last value it wrote ("sync"). Order inside one oscillator: frequency, envelope,
//     control (gate last); lanes in the order wave1-3, pulse1-3, filter, after the restart writes.
//   * Macros (pulse width offset, cutoff offset) are added after the lane value, clamped, for lane and
//     host values alike; the sweep depth scales every sweep and ping-pong step.

#include "bside/engine/ProgramSink.h"
#include "bside/program/Program.h"

#include <cstdint>

namespace vs::bside::engine
{
enum class RestartPolicy : std::uint8_t { clean, precondition, authentic };

struct EngineConfig
{
    std::uint32_t clockHz = 985248;
    bool authentic = false; // chip.resolution: integer register values (8.4.6)
};

/** What the host parameters say about one oscillator (the caller folds in the note, glide, bend, vibrato,
    coarse and fine tuning and the unison detune). */
struct HostOscillator
{
    double semitones = 60.0;          // MIDI note number with all host pitch contributions, fractional
    bool enabled = false;             // oscN.wave is not Off: gate follows the note
    std::uint8_t waveBits = 0;        // control bits 4..7 (triangle, saw, pulse, noise)
    bool sync = false, ring = false;
    float pulseWidth = 2048.0f;       // 0..4095
    std::uint8_t attackDecay = 0x00;
    std::uint8_t sustainRelease = 0xf6;
};

struct HostFilter
{
    float cutoff = 1024.0f;           // 0..2047
    std::uint8_t resonance = 0;       // 0..15
    std::uint8_t routing = 7;         // bit n-1: oscillator n through the filter
    std::uint8_t mode = 1;            // 1 low-pass, 2 band-pass, 4 high-pass
    bool osc3Off = false;
    std::uint8_t volume = 15;         // 0..15
};

struct HostMacros
{
    float pulseWidthOffset = 0.0f;    // register units, added after the lane value
    float cutoffOffset = 0.0f;
    float sweepDepth = 1.0f;          // scales every sweep and ping-pong step
};

struct HostInputs
{
    HostOscillator osc[3];
    HostFilter filter;
    HostMacros macro;
};

class ProgramEngine
{
public:
    /** Resets everything; the chip is assumed to be in its power-on state (all registers 0). */
    void prepare (const EngineConfig& config) noexcept;

    /** Tick period in cycles (tickPeriodCycles), at least 256. Applies to the ticks scheduled from now on:
        the tick already scheduled keeps its cycle. */
    void setTickPeriod (std::uint32_t cycles) noexcept;
    std::uint32_t tickPeriod() const noexcept { return period; }

    /** The host parameters; takes effect at the next write opportunity (a tick or `sync`). */
    void setHost (const HostInputs& h) noexcept { host = h; }

    /** Writes every register that follows the host parameters and whose value has changed. */
    void sync (std::uint64_t cycle, ProgramSink& sink) noexcept;

    /** A new note: tick 0 runs now. `stolen` is a voice that was faded out first. `policy` is restart.policy. */
    void noteOn (std::uint64_t cycle, const program::Program& program, RestartPolicy policy, bool stolen, ProgramSink& sink) noexcept;

    /** Note-off: lanes with a `release` label jump there; a wave lane without one clears its gate. */
    void noteOff (std::uint64_t cycle, ProgramSink& sink) noexcept;

    /** The voice became idle: Precondition and Clean write the restart state at once (8.6.4) and the engine stops. */
    void enterIdle (std::uint64_t cycle, const program::Restart& restart, RestartPolicy policy, ProgramSink& sink) noexcept;

    /** Stops the ticks without writing anything. */
    void stop() noexcept { running = false; }

    bool isRunning() const noexcept { return running; }
    /** Cycle of the next tick, or UINT64_MAX when no note is running. */
    std::uint64_t nextTickCycle() const noexcept { return running ? nextTick : ~std::uint64_t { 0 }; }
    void runTick (ProgramSink& sink) noexcept;
    void runUntil (std::uint64_t cycle, ProgramSink& sink) noexcept
    {
        while (running && nextTick <= cycle)
            runTick (sink);
    }
    std::uint64_t tickIndex() const noexcept { return tick; }
    /** True while a wave lane can still write (not ended); used for the idle rule of 8.5. */
    bool anyWaveLaneRunning() const noexcept;

private:
    enum class Mode : std::uint8_t { normal, sweeping, pingpong };
    struct LaneRun
    {
        std::uint16_t row = 0;
        std::uint8_t wait = 0;
        bool active = false;
        Mode mode = Mode::normal;
        std::uint8_t sweepLeft = 0;
        double pingLo = 0, pingHi = 0, pingStep = 0, pingDir = 1;
        std::uint8_t repeats[program::kMaxRowsPerLane] = {};
    };
    struct WaveValues
    {
        bool ctrlOwned = false, gateOwned = false, pitchOwned = false, pitchAbsolute = false, envOwned = false, gate = false;
        std::uint8_t ctrl = 0, ad = 0, sr = 0;
        double pitch = 0;
    };
    struct PulseValues { bool owned = false; double value = 0; };
    struct FilterValues
    {
        bool cutoffOwned = false, resOwned = false, routeOwned = false, modeOwned = false, volOwned = false;
        double cutoff = 0;
        std::uint8_t res = 0, route = 0, mode = 0, vol = 0;
    };
    struct Shadow // the last value written to the chip, per register group
    {
        double freq[3] = {}, pw[3] = {}, cutoff = 0;
        std::uint8_t ctrl[3] = {}, ad[3] = {}, sr[3] = {}, resRoute = 0, modeVol = 0;
    };

    void resetChipShadow() noexcept { shadow = Shadow {}; }
    void applyRestart (std::uint64_t cycle, const program::Restart& r, ProgramSink& sink) noexcept;
    void tickBody (std::uint64_t cycle, ProgramSink& sink) noexcept;
    void tickLane (int lane, std::uint64_t cycle, ProgramSink& sink) noexcept;
    void execBurst (int lane, std::uint64_t cycle, ProgramSink& sink) noexcept;
    void execCommand (int lane, const program::Cmd& c, std::uint64_t cycle, ProgramSink& sink) noexcept;
    void stepSweep (int lane, const program::Cmd& c, std::uint64_t cycle, ProgramSink& sink) noexcept;
    void stepPingPong (int lane, std::uint64_t cycle, ProgramSink& sink) noexcept;
    void syncWave (int n, std::uint64_t cycle, ProgramSink& sink) noexcept;
    void syncPulse (int n, std::uint64_t cycle, ProgramSink& sink) noexcept;
    void syncFilter (std::uint64_t cycle, ProgramSink& sink) noexcept;

    bool oscActive (int n) const noexcept;
    double frequencyRegister (double semitones) const noexcept;
    double effFrequency (int n) const noexcept;
    double effPulse (int n) const noexcept;
    std::uint8_t effControl (int n) const noexcept;
    void effEnvelope (int n, std::uint8_t& ad, std::uint8_t& sr) const noexcept;
    double effCutoff() const noexcept;
    std::uint8_t effResRoute() const noexcept;
    std::uint8_t effModeVol() const noexcept;
    double hostPulse (int n) const noexcept;
    double hostCutoff() const noexcept;

    void emitFrequency (int n, std::uint64_t cycle, ProgramSink& sink) noexcept;
    void emitPulse (int n, std::uint64_t cycle, ProgramSink& sink) noexcept;
    void emitControl (int n, std::uint64_t cycle, ProgramSink& sink) noexcept;
    void emitEnvelope (int n, std::uint64_t cycle, ProgramSink& sink) noexcept;
    void emitCutoff (std::uint64_t cycle, ProgramSink& sink) noexcept;
    void emitResRoute (std::uint64_t cycle, ProgramSink& sink) noexcept;
    void emitModeVol (std::uint64_t cycle, ProgramSink& sink) noexcept;

    EngineConfig cfg;
    HostInputs host;
    program::Program prog = program::emptyProgram();
    LaneRun lanes[program::kNumLanes];
    WaveValues wave[3];
    PulseValues pulse[3];
    FilterValues filt;
    Shadow shadow;
    std::uint32_t period = 19656;
    std::uint64_t nextTick = 0;
    std::uint64_t tick = 0;
    bool running = false;
    bool held = false;
    bool firstWaveActive = false; // tick 0 until the wave lanes' row 0 has run (8.6.4 firstwave)
    // Restart state of an idle voice (Precondition, Clean)
    bool inRestartState = false;
    std::uint64_t restartCycle = 0;
    program::Restart restartState;
};
} // namespace vs::bside::engine
