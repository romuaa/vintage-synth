#pragma once

// The tick period P in chip cycles (docs/architecture.md 8.6.1). Pure function of the parameters;
// the caller reads it at block start and gives it to every voice's engine. JUCE-free.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vs::bside::engine
{
enum class TickRate : std::uint8_t { frame, free, tempo };

struct TickParams
{
    TickRate rate = TickRate::frame;
    std::uint32_t clockHz = 985248; // chip.clock
    int mult = 1;                   // tick.mult 1..8
    double freeHz = 50.0;           // tick.free_hz 10..400
    double bpm = 120.0;             // host tempo, or arp.tempo when stopped
    int sync = 24;                  // tick.sync: ticks per beat (6, 12, 24, 48)
};

constexpr std::uint32_t kMinTickPeriod = 256;
constexpr std::uint32_t kMaxTickPeriod = 1u << 30;
constexpr std::uint32_t kFrameCycles50 = 19656; // "50 Hz system"
constexpr std::uint32_t kFrameCycles60 = 17095; // "60 Hz system"

/** Frame length for a chip clock: the 60 Hz system's for clocks nearer to 1 022 727 Hz than to 985 248 Hz. */
constexpr std::uint32_t frameCycles (std::uint32_t clockHz) noexcept
{
    return clockHz >= 1003988u ? kFrameCycles60 : kFrameCycles50;
}

/** P = round (frame / mult), round (clock / (free_hz x mult)) or round (clock x 60 / (bpm x sync x mult)),
    at least 256 cycles. Out-of-range or non-finite inputs are clamped to their parameter ranges. */
inline std::uint32_t tickPeriodCycles (const TickParams& p) noexcept
{
    const double mult = static_cast<double> (std::clamp (p.mult, 1, 8));
    const double clock = static_cast<double> (std::clamp<std::uint32_t> (p.clockHz, 1000u, 100000000u));
    double cycles = 0.0;
    switch (p.rate)
    {
        case TickRate::frame: cycles = static_cast<double> (frameCycles (p.clockHz)) / mult; break;
        case TickRate::free:
        {
            const double hz = std::isfinite (p.freeHz) ? std::clamp (p.freeHz, 1.0, 100000.0) : 50.0;
            cycles = clock / (hz * mult);
            break;
        }
        case TickRate::tempo:
        {
            const double bpm = std::isfinite (p.bpm) ? std::clamp (p.bpm, 1.0, 1000.0) : 120.0;
            const double sync = static_cast<double> (std::clamp (p.sync, 1, 96));
            cycles = clock * 60.0 / (bpm * sync * mult);
            break;
        }
    }
    const double r = std::floor (cycles + 0.5);
    return static_cast<std::uint32_t> (std::clamp (r, static_cast<double> (kMinTickPeriod), static_cast<double> (kMaxTickPeriod)));
}
} // namespace vs::bside::engine
