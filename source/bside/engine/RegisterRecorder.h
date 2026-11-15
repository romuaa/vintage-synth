#pragma once

// A ProgramSink that logs the register writes the program engine would send to the chip (docs/architecture.md
// 8.9.6): the tool of the golden write traces. Allocates (it is a test and tool class, never used on the audio
// thread). JUCE-free.
//
// A sink call becomes the writes of the contract's order: frequency low then high byte, pulse width low byte then
// high nibble, envelope attack/decay then sustain/release, cutoff low bits then high byte. The Clean policy's
// direct operations and a chip reset are not registers: they are kept as events and written as `#` comment lines
// in the trace text, so the text stays a valid trace and shows where they happened.

#include "bside/chip/ChipTypes.h"
#include "bside/engine/ProgramSink.h"
#include "bside/trace/TraceFormat.h"

#include <cmath>
#include <string>
#include <vector>

namespace vs::bside::engine
{
class RegisterRecorder final : public ProgramSink
{
public:
    enum class Kind : std::uint8_t { write, clearEnvelopeTimers, resetChip };
    struct Entry
    {
        std::uint64_t cycle = 0;
        Kind kind = Kind::write;
        std::uint8_t reg = 0, value = 0;
    };

    void frequency (std::uint64_t cycle, int osc, double v) noexcept override
    {
        const auto f = static_cast<unsigned> (clampTo (v, 0.0, 65535.0));
        const auto base = static_cast<std::uint8_t> (chip::reg::oscStride * osc);
        put (cycle, static_cast<std::uint8_t> (base + chip::reg::freqLo), static_cast<std::uint8_t> (f & 0xffu));
        put (cycle, static_cast<std::uint8_t> (base + chip::reg::freqHi), static_cast<std::uint8_t> (f >> 8));
    }
    void pulseWidth (std::uint64_t cycle, int osc, double v) noexcept override
    {
        const auto p = static_cast<unsigned> (clampTo (v, 0.0, 4095.0));
        const auto base = static_cast<std::uint8_t> (chip::reg::oscStride * osc);
        put (cycle, static_cast<std::uint8_t> (base + chip::reg::pwLo), static_cast<std::uint8_t> (p & 0xffu));
        put (cycle, static_cast<std::uint8_t> (base + chip::reg::pwHi), static_cast<std::uint8_t> (p >> 8));
    }
    void control (std::uint64_t cycle, int osc, std::uint8_t v) noexcept override
    {
        put (cycle, static_cast<std::uint8_t> (chip::reg::oscStride * osc + chip::reg::control), v);
    }
    void envelope (std::uint64_t cycle, int osc, std::uint8_t ad, std::uint8_t sr) noexcept override
    {
        put (cycle, static_cast<std::uint8_t> (chip::reg::oscStride * osc + chip::reg::attackDecay), ad);
        put (cycle, static_cast<std::uint8_t> (chip::reg::oscStride * osc + chip::reg::sustainRelease), sr);
    }
    void cutoff (std::uint64_t cycle, double v) noexcept override
    {
        const auto fc = static_cast<unsigned> (clampTo (v, 0.0, 2047.0));
        put (cycle, chip::reg::cutoffLo, static_cast<std::uint8_t> (fc & 7u));
        put (cycle, chip::reg::cutoffHi, static_cast<std::uint8_t> (fc >> 3));
    }
    void resonanceRouting (std::uint64_t cycle, std::uint8_t v) noexcept override { put (cycle, chip::reg::resonanceRouting, v); }
    void modeVolume (std::uint64_t cycle, std::uint8_t v) noexcept override { put (cycle, chip::reg::modeVolume, v); }
    void clearEnvelopeTimers (std::uint64_t cycle) noexcept override { entries.push_back ({ cycle, Kind::clearEnvelopeTimers, 0, 0 }); }
    void resetChip (std::uint64_t cycle) noexcept override { entries.push_back ({ cycle, Kind::resetChip, 0, 0 }); }

    void clear() { entries.clear(); }
    const std::vector<Entry>& log() const noexcept { return entries; }

    /** The register writes only, as a trace (model B, the given clock). */
    trace::Trace toTrace (std::uint32_t clockHz, std::uint64_t endCycle, trace::Model model = trace::Model::B) const
    {
        trace::Trace t;
        t.clockHz = clockHz;
        t.model = model;
        t.endCycle = endCycle;
        for (const auto& e : entries)
            if (e.kind == Kind::write)
                t.writes.push_back ({ e.cycle, e.reg, e.value });
        return t;
    }

    /** The canonical trace text with the non-register events as comment lines. Empty if the trace is not valid. */
    std::string toText (std::uint32_t clockHz, std::uint64_t endCycle, trace::Model model = trace::Model::B) const
    {
        std::string plain;
        if (! trace::writeTrace (toTrace (clockHz, endCycle, model), plain))
            return {};
        std::string out;
        const std::size_t headerEnd = plain.find ("\n", plain.find ("model ")) + 1;
        out = plain.substr (0, headerEnd);
        for (const auto& e : entries)
        {
            const std::string cycle = std::to_string (e.cycle);
            if (e.kind == Kind::write)
                out += cycle + ' ' + hex (e.reg) + ' ' + hex (e.value) + '\n';
            else if (e.kind == Kind::clearEnvelopeTimers)
                out += "# " + cycle + " direct: rate counters and dividers cleared\n";
            else
                out += "# " + cycle + " direct: chip reset to power-on\n";
        }
        out += "end " + std::to_string (endCycle) + "\n";
        return out;
    }

private:
    static double clampTo (double v, double lo, double hi) noexcept
    {
        if (! std::isfinite (v))
            return lo;
        return std::floor (std::fmin (std::fmax (v, lo), hi) + 0.5);
    }
    static std::string hex (std::uint8_t v)
    {
        static const char* const digits = "0123456789abcdef";
        return std::string { digits[v >> 4], digits[v & 15] };
    }
    void put (std::uint64_t cycle, std::uint8_t reg, std::uint8_t value) { entries.push_back ({ cycle, Kind::write, reg, value }); }

    std::vector<Entry> entries;
};
} // namespace vs::bside::engine
