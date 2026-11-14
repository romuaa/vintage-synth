#include "bside/chip/Chip.h"

#include <cmath>
#include <type_traits>

namespace vs::bside::chip
{
static_assert (std::is_trivially_copyable_v<Chip>, "the chip owns no memory: copying it is a plain copy, it never allocates");

void Chip::prepare (const Config& c) noexcept
{
    cfg = c;
    clock.prepare (cfg.clockHz, cfg.internalRate);
    reset();
}

void Chip::reset() noexcept
{
    osc = {};
    env = {};
    regs = {};
    cutoff = 0.0f;
    cycleCount = 0;
    clock.reset();
}

void Chip::updateRatePeriod (int n) noexcept
{
    Env& e = env[idx (n)];
    switch (e.phase)
    {
        case EnvPhase::attack: e.ratePeriod = kRatePeriods[attackDecay (n) >> 4]; break;
        case EnvPhase::decaySustain: e.ratePeriod = kRatePeriods[attackDecay (n) & 15u]; break;
        case EnvPhase::release: e.ratePeriod = kRatePeriods[sustainRelease (n) & 15u]; break;
    }
}

void Chip::applyControl (int n, std::uint8_t before) noexcept
{
    const std::uint8_t c = ctl (n);
    Osc& o = osc[idx (n)];
    if ((c & ctrl::test) != 0)
    {
        o.phase = 0;
        o.noise = kNoiseReset;
    }
    Env& e = env[idx (n)];
    const bool gateWas = (before & ctrl::gate) != 0;
    const bool gateIs = (c & ctrl::gate) != 0;
    if (! gateWas && gateIs)
    {
        e.phase = EnvPhase::attack;
        updateRatePeriod (n);
    }
    else if (gateWas && ! gateIs)
    {
        e.phase = EnvPhase::release;
        updateRatePeriod (n);
    }
}

void Chip::write (std::uint8_t address, std::uint8_t value) noexcept
{
    const std::uint8_t r = address & reg::kAddressMask;
    if (r >= reg::kWritable)
        return;
    const std::uint8_t before = regs[r];
    regs[r] = value;
    if (r >= 3 * reg::oscStride)
        return; // filter, routing, mode and volume: analog stages only
    const int n = r / reg::oscStride;
    Osc& o = osc[idx (n)];
    const auto base = static_cast<std::size_t> (n * reg::oscStride);
    switch (r % reg::oscStride)
    {
        case reg::freqLo:
        case reg::freqHi:
            o.inc = static_cast<std::uint64_t> (regs[base + reg::freqLo] | (regs[base + reg::freqHi] << 8)) << kFractionBits;
            // A register write sets the whole increment; the accumulator keeps its value and its fraction.
            break;
        case reg::pwLo:
        case reg::pwHi:
            o.pw = static_cast<std::uint16_t> (regs[base + reg::pwLo] | ((regs[base + reg::pwHi] & 0x0f) << 8));
            o.pwSmooth = static_cast<float> (o.pw);
            break;
        case reg::control: applyControl (n, before); break;
        case reg::attackDecay:
        case reg::sustainRelease: updateRatePeriod (n); break;
        default: break;
    }
}

void Chip::setOscillator (int n, const OscInput& in) noexcept
{
    if (n < 0 || n > 2)
        return;
    Osc& o = osc[idx (n)];
    const auto base = static_cast<std::size_t> (n * reg::oscStride);
    const double f = std::isfinite (in.frequency) ? in.frequency : 0.0;
    const float pw = std::isfinite (in.pulseWidth) ? in.pulseWidth : 0.0f;
    std::uint32_t fReg = 0;
    if (cfg.authentic)
    {
        const double fr = std::fmin (std::fmax (std::nearbyint (f), 0.0), 65535.0);
        fReg = static_cast<std::uint32_t> (fr);
        o.inc = static_cast<std::uint64_t> (fReg) << kFractionBits;
        o.pw = static_cast<std::uint16_t> (std::fmin (std::fmax (std::nearbyint (pw), 0.0f), 4095.0f));
        o.pwSmooth = static_cast<float> (o.pw);
    }
    else
    {
        const double fc = std::fmin (std::fmax (f, 0.0), kMaxSmoothFrequency);
        const auto inc = static_cast<std::uint64_t> (std::llround (fc * 4294967296.0));
        o.inc = std::min (inc, kMaxIncrement);
        fReg = static_cast<std::uint32_t> (std::min<std::uint64_t> (o.inc >> kFractionBits, 65535));
        o.pwSmooth = std::fmin (std::fmax (pw, 0.0f), 4095.0f);
        o.pw = static_cast<std::uint16_t> (std::floor (o.pwSmooth)); // the integer part follows the register rules (8.4.3)
    }
    regs[base + reg::freqLo] = static_cast<std::uint8_t> (fReg & 0xff);
    regs[base + reg::freqHi] = static_cast<std::uint8_t> (fReg >> 8);
    regs[base + reg::pwLo] = static_cast<std::uint8_t> (o.pw & 0xff);
    regs[base + reg::pwHi] = static_cast<std::uint8_t> (o.pw >> 8);
    regs[base + reg::attackDecay] = in.attackDecay;
    regs[base + reg::sustainRelease] = in.sustainRelease;
    updateRatePeriod (n);
    const std::uint8_t before = regs[base + reg::control];
    regs[base + reg::control] = in.control;
    applyControl (n, before);
}

void Chip::setFilter (const FilterInput& in) noexcept
{
    const float fc = std::isfinite (in.cutoff) ? std::fmin (std::fmax (in.cutoff, 0.0f), 2047.0f) : 0.0f;
    cutoff = cfg.authentic ? std::nearbyint (fc) : fc;
    const auto fcReg = static_cast<std::uint32_t> (cfg.authentic ? cutoff : std::floor (fc));
    regs[reg::cutoffLo] = static_cast<std::uint8_t> (fcReg & 7u);
    regs[reg::cutoffHi] = static_cast<std::uint8_t> (fcReg >> 3);
    regs[reg::resonanceRouting] = static_cast<std::uint8_t> ((in.resonance << 4) | (in.routing & 0x0f));
    regs[reg::modeVolume] = in.modeVolume;
}

std::uint8_t Chip::read (std::uint8_t address) const noexcept
{
    switch (address & reg::kAddressMask)
    {
        case reg::osc3Output: return static_cast<std::uint8_t> (output (2) >> 4);
        case reg::env3Output: return env[2].counter;
        default: return 0;
    }
}

DigitalState Chip::state() const noexcept
{
    DigitalState s;
    s.cycle = cycleCount;
    for (int n = 0; n < 3; ++n)
    {
        const Osc& o = osc[idx (n)];
        auto& so = s.osc[idx (n)];
        so.acc = static_cast<std::uint32_t> (o.phase >> kFractionBits);
        so.accFraction = static_cast<std::uint32_t> (o.phase & 0xffffffffu);
        so.increment = o.inc;
        so.noise = o.noise;
        so.pulseWidth = o.pw;
        const Env& e = env[idx (n)];
        auto& se = s.env[idx (n)];
        se.counter = e.counter;
        se.phase = static_cast<std::uint8_t> (e.phase);
        se.rateCounter = e.rate;
        se.ratePeriod = e.ratePeriod;
        se.divider = e.div;
        se.dividerPeriod = e.divPeriod;
    }
    s.registers = regs;
    s.osc3Output = read (reg::osc3Output);
    s.env3Output = read (reg::env3Output);
    return s;
}
} // namespace vs::bside::chip
