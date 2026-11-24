#pragma once

// ProgramSinks that write to the real chip (docs/architecture.md 8.6, 8.4.7, 8.21.1). The target must already be at
// the cycle of every call (the caller renders up to ProgramEngine::nextTickCycle() before running the tick).
// Authentic resolution: the register writes of the contract's order. Smooth: the engine's continuous values
// through setOscillator and the filter input. JUCE-free, allocation-free.
//
// Two targets, one code (BasicChipProgramSink):
//   ChipProgramSink       the bare digital core, chip::Chip (the engine and the chip proven together: ProgramEngineChipTests);
//   ChipVoiceProgramSink  the whole chain of one note, chip::ChipVoice (the part, 8.21.1 "Sink"): only the voice lands
//                         the writes the filter and the output stage read at their place in the delayed signal, so the
//                         part's writes go to it and never to the Chip inside it.
//
// Time: a stolen voice's chip is restarted with `powerOnKeepingTime()`, so the chip's own cycle count keeps running and
// `cycle()` is the engine's time. Clean's direct clear (8.6.4) clears the envelope counters of all three oscillators of
// the voice's chip (the contract says "the rate counter and divider"; it names no oscillator, and a restart concerns
// the whole voice); it is not a register write and has no analog effect.

#include "bside/chip/Chip.h"
#include "bside/chip/ChipVoice.h"
#include "bside/engine/ProgramSink.h"

#include <cmath>
#include <type_traits>

namespace vs::bside::engine
{
template <class Target>
class BasicChipProgramSink final : public ProgramSink
{
    static_assert (std::is_same_v<Target, chip::Chip> || std::is_same_v<Target, chip::ChipVoice>, "a chip or a chip voice");

public:
    BasicChipProgramSink (Target& t, bool authenticResolution) noexcept : target (&t), authentic (authenticResolution) {}

    /** The engine's time: the chip's cycle (it keeps running through a reset). */
    std::uint64_t cycle() const noexcept { return digital().cycle(); }
    /** The resolution of the writes (chip.resolution, 8.4.6); the target's own configuration must match it. */
    void setAuthentic (bool on) noexcept { authentic = on; }
    /** Forgets the continuous inputs it has sent (after the target was reset: power-on inputs). */
    void forgetInputs() noexcept
    {
        in[0] = in[1] = in[2] = chip::OscInput {};
        fin = chip::FilterInput {};
    }

    void frequency (std::uint64_t, int osc, double v) noexcept override
    {
        if (authentic)
        {
            const auto f = static_cast<unsigned> (std::fmin (std::fmax (v, 0.0), 65535.0));
            const auto base = static_cast<std::uint8_t> (chip::reg::oscStride * osc);
            target->write (static_cast<std::uint8_t> (base + chip::reg::freqLo), static_cast<std::uint8_t> (f & 0xffu));
            target->write (static_cast<std::uint8_t> (base + chip::reg::freqHi), static_cast<std::uint8_t> (f >> 8));
        }
        else
        {
            in[osc].frequency = v;
            target->setOscillator (osc, in[osc]);
        }
    }
    void pulseWidth (std::uint64_t, int osc, double v) noexcept override
    {
        if (authentic)
        {
            const auto p = static_cast<unsigned> (std::fmin (std::fmax (v, 0.0), 4095.0));
            const auto base = static_cast<std::uint8_t> (chip::reg::oscStride * osc);
            target->write (static_cast<std::uint8_t> (base + chip::reg::pwLo), static_cast<std::uint8_t> (p & 0xffu));
            target->write (static_cast<std::uint8_t> (base + chip::reg::pwHi), static_cast<std::uint8_t> (p >> 8));
        }
        else
        {
            in[osc].pulseWidth = static_cast<float> (v);
            target->setOscillator (osc, in[osc]);
        }
    }
    void control (std::uint64_t, int osc, std::uint8_t v) noexcept override
    {
        if (authentic)
            target->write (static_cast<std::uint8_t> (chip::reg::oscStride * osc + chip::reg::control), v);
        else
        {
            in[osc].control = v;
            target->setOscillator (osc, in[osc]);
        }
    }
    void envelope (std::uint64_t, int osc, std::uint8_t ad, std::uint8_t sr) noexcept override
    {
        if (authentic)
        {
            target->write (static_cast<std::uint8_t> (chip::reg::oscStride * osc + chip::reg::attackDecay), ad);
            target->write (static_cast<std::uint8_t> (chip::reg::oscStride * osc + chip::reg::sustainRelease), sr);
        }
        else
        {
            in[osc].attackDecay = ad;
            in[osc].sustainRelease = sr;
            target->setOscillator (osc, in[osc]);
        }
    }
    void cutoff (std::uint64_t, double v) noexcept override
    {
        if (authentic)
        {
            const auto fc = static_cast<unsigned> (std::fmin (std::fmax (v, 0.0), 2047.0));
            target->write (chip::reg::cutoffLo, static_cast<std::uint8_t> (fc & 7u));
            target->write (chip::reg::cutoffHi, static_cast<std::uint8_t> (fc >> 3));
        }
        else
        {
            fin.cutoff = static_cast<float> (v);
            setFilterInput();
        }
    }
    void resonanceRouting (std::uint64_t, std::uint8_t v) noexcept override
    {
        if (authentic)
            target->write (chip::reg::resonanceRouting, v);
        else
        {
            fin.resonance = static_cast<std::uint8_t> (v >> 4);
            fin.routing = static_cast<std::uint8_t> (v & 7u);
            setFilterInput();
        }
    }
    void modeVolume (std::uint64_t, std::uint8_t v) noexcept override
    {
        if (authentic)
            target->write (chip::reg::modeVolume, v);
        else
        {
            fin.modeVolume = v;
            setFilterInput();
        }
    }
    void clearEnvelopeTimers (std::uint64_t) noexcept override
    {
        for (int n = 0; n < 3; ++n)
            digital().clearEnvelopeCounters (n);
    }
    void resetChip (std::uint64_t) noexcept override
    {
        target->powerOnKeepingTime();
        forgetInputs();
    }

private:
    chip::Chip& digital() const noexcept
    {
        if constexpr (std::is_same_v<Target, chip::Chip>)
            return *target;
        else
            return target->chip();
    }
    void setFilterInput() noexcept
    {
        if constexpr (std::is_same_v<Target, chip::Chip>)
            target->setFilter (fin);
        else
            target->setFilterInput (fin);
    }

    Target* target;
    bool authentic;
    chip::OscInput in[3] {};
    chip::FilterInput fin {};
};

using ChipProgramSink = BasicChipProgramSink<chip::Chip>;
using ChipVoiceProgramSink = BasicChipProgramSink<chip::ChipVoice>;
} // namespace vs::bside::engine
