#pragma once

// A ProgramSink that writes to the real chip (docs/architecture.md 8.6, 8.4.7). The chip must already be at the
// cycle of every call (the caller renders up to ProgramEngine::nextTickCycle() before running the tick).
// Authentic resolution: the register writes of the contract's order. Smooth: the engine's continuous values
// through Chip::setOscillator / setFilter. JUCE-free, allocation-free.
//
// Time: `chip.reset()` (a stolen voice) also zeroes the chip's own cycle count, while the engine keeps absolute
// cycles for the voice's lifetime. The sink keeps the difference: `cycle()` is the engine's time (chip cycle +
// offset); a caller renders `engine.nextTickCycle() - sink.cycle()` cycles to reach the next tick.
//
// Not yet possible: `clearEnvelopeTimers` (the Clean policy's direct operation) has no chip entry point yet;
// it is a no-op here until the chip offers one (reported at S2c).

#include "bside/chip/Chip.h"
#include "bside/engine/ProgramSink.h"

#include <cmath>

namespace vs::bside::engine
{
class ChipProgramSink final : public ProgramSink
{
public:
    ChipProgramSink (chip::Chip& c, bool authenticResolution) noexcept : chip (c), authentic (authenticResolution) {}

    /** The engine's time: the chip's cycle plus the cycles it had run before its last reset. */
    std::uint64_t cycle() const noexcept { return offset + chip.cycle(); }

    void frequency (std::uint64_t, int osc, double v) noexcept override
    {
        if (authentic)
        {
            const auto f = static_cast<unsigned> (std::fmin (std::fmax (v, 0.0), 65535.0));
            const auto base = static_cast<std::uint8_t> (chip::reg::oscStride * osc);
            chip.write (static_cast<std::uint8_t> (base + chip::reg::freqLo), static_cast<std::uint8_t> (f & 0xffu));
            chip.write (static_cast<std::uint8_t> (base + chip::reg::freqHi), static_cast<std::uint8_t> (f >> 8));
        }
        else
        {
            in[osc].frequency = v;
            chip.setOscillator (osc, in[osc]);
        }
    }
    void pulseWidth (std::uint64_t, int osc, double v) noexcept override
    {
        if (authentic)
        {
            const auto p = static_cast<unsigned> (std::fmin (std::fmax (v, 0.0), 4095.0));
            const auto base = static_cast<std::uint8_t> (chip::reg::oscStride * osc);
            chip.write (static_cast<std::uint8_t> (base + chip::reg::pwLo), static_cast<std::uint8_t> (p & 0xffu));
            chip.write (static_cast<std::uint8_t> (base + chip::reg::pwHi), static_cast<std::uint8_t> (p >> 8));
        }
        else
        {
            in[osc].pulseWidth = static_cast<float> (v);
            chip.setOscillator (osc, in[osc]);
        }
    }
    void control (std::uint64_t, int osc, std::uint8_t v) noexcept override
    {
        if (authentic)
            chip.write (static_cast<std::uint8_t> (chip::reg::oscStride * osc + chip::reg::control), v);
        else
        {
            in[osc].control = v;
            chip.setOscillator (osc, in[osc]);
        }
    }
    void envelope (std::uint64_t, int osc, std::uint8_t ad, std::uint8_t sr) noexcept override
    {
        if (authentic)
        {
            chip.write (static_cast<std::uint8_t> (chip::reg::oscStride * osc + chip::reg::attackDecay), ad);
            chip.write (static_cast<std::uint8_t> (chip::reg::oscStride * osc + chip::reg::sustainRelease), sr);
        }
        else
        {
            in[osc].attackDecay = ad;
            in[osc].sustainRelease = sr;
            chip.setOscillator (osc, in[osc]);
        }
    }
    void cutoff (std::uint64_t, double v) noexcept override
    {
        if (authentic)
        {
            const auto fc = static_cast<unsigned> (std::fmin (std::fmax (v, 0.0), 2047.0));
            chip.write (chip::reg::cutoffLo, static_cast<std::uint8_t> (fc & 7u));
            chip.write (chip::reg::cutoffHi, static_cast<std::uint8_t> (fc >> 3));
        }
        else
        {
            fin.cutoff = static_cast<float> (v);
            chip.setFilter (fin);
        }
    }
    void resonanceRouting (std::uint64_t, std::uint8_t v) noexcept override
    {
        if (authentic)
            chip.write (chip::reg::resonanceRouting, v);
        else
        {
            fin.resonance = static_cast<std::uint8_t> (v >> 4);
            fin.routing = static_cast<std::uint8_t> (v & 7u);
            chip.setFilter (fin);
        }
    }
    void modeVolume (std::uint64_t, std::uint8_t v) noexcept override
    {
        if (authentic)
            chip.write (chip::reg::modeVolume, v);
        else
        {
            fin.modeVolume = v;
            chip.setFilter (fin);
        }
    }
    void clearEnvelopeTimers (std::uint64_t) noexcept override {}
    void resetChip (std::uint64_t) noexcept override
    {
        offset += chip.cycle();
        chip.reset();
        in[0] = in[1] = in[2] = chip::OscInput {};
        fin = chip::FilterInput {};
    }

private:
    chip::Chip& chip;
    bool authentic;
    std::uint64_t offset = 0;
    chip::OscInput in[3] {};
    chip::FilterInput fin {};
};
} // namespace vs::bside::engine
