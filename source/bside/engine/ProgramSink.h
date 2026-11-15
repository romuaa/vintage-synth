#pragma once

// Where the patch program engine (docs/architecture.md 8.6) sends what it computes. One call is one
// register-level write at a cycle, in the contract's order (8.6.2), so the same engine can drive the real
// chip (ChipProgramSink.h) or a recorder that logs the register writes (RegisterRecorder.h, 8.9.6).
// JUCE-free. Every call is noexcept and real-time safe in the engine's own implementations.
//
// Values: Authentic resolution delivers integers only (frequency 1..65535, pulse width 0..4095, cutoff
// 0..2047); Smooth delivers continuous values in the same units (8.4.6).

#include <cstdint>

namespace vs::bside::engine
{
class ProgramSink
{
public:
    virtual ~ProgramSink() = default;

    /** Oscillator `osc` (0..2) frequency, in register units (registers 0x00/0x01 + 7 osc). */
    virtual void frequency (std::uint64_t cycle, int osc, double registerValue) noexcept = 0;
    /** Pulse width 0..4095 (registers 0x02/0x03 + 7 osc). */
    virtual void pulseWidth (std::uint64_t cycle, int osc, double value) noexcept = 0;
    /** Control register (0x04 + 7 osc). */
    virtual void control (std::uint64_t cycle, int osc, std::uint8_t value) noexcept = 0;
    /** Attack/decay then sustain/release (registers 0x05 and 0x06 + 7 osc, in that order). */
    virtual void envelope (std::uint64_t cycle, int osc, std::uint8_t attackDecay, std::uint8_t sustainRelease) noexcept = 0;
    /** Filter cutoff 0..2047 (registers 0x15 low bits, then 0x16 high bits). */
    virtual void cutoff (std::uint64_t cycle, double value) noexcept = 0;
    /** Register 0x17: resonance in the high nibble, routing in bits 0..2. */
    virtual void resonanceRouting (std::uint64_t cycle, std::uint8_t value) noexcept = 0;
    /** Register 0x18: bit 7 oscillator 3 off, bits 6..4 modes, low nibble volume. */
    virtual void modeVolume (std::uint64_t cycle, std::uint8_t value) noexcept = 0;
    /** The Clean policy's direct operation (8.6.4): the three rate counters and dividers are set to 0.
        Not a register write and not part of a register trace. */
    virtual void clearEnvelopeTimers (std::uint64_t cycle) noexcept = 0;
    /** The chip returns to its power-on state (a stolen voice under Clean or Precondition, 8.6.4). */
    virtual void resetChip (std::uint64_t cycle) noexcept = 0;

protected:
    ProgramSink() = default;
    ProgramSink (const ProgramSink&) = default;
    ProgramSink (ProgramSink&&) = default;
    ProgramSink& operator= (const ProgramSink&) = default;
    ProgramSink& operator= (ProgramSink&&) = default;
};
} // namespace vs::bside::engine
