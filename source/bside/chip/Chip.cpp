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
    shapes = &combined::tablesFor (cfg.model, cfg.shapes);
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

void Chip::writeAttackDecay (int n) noexcept
{
    Env& e = env[idx (n)];
    if (e.phase == static_cast<std::uint8_t> (EnvPhase::attack))
        e.compare = attackMatch (n);
    else if (e.phase == static_cast<std::uint8_t> (EnvPhase::decaySustain))
        e.compare = decayMatch (n);
}

void Chip::writeSustainRelease (int n) noexcept
{
    Env& e = env[idx (n)];
    if (e.phase == static_cast<std::uint8_t> (EnvPhase::release))
        e.compare = releaseMatch (n);
}

void Chip::writeControl (int n, std::uint8_t before) noexcept
{
    const std::uint8_t c = ctl (n);
    Osc& o = osc[idx (n)];
    const ModelTiming t = timingFor (cfg.model);
    // 8.15.5: waveform bits changed to none -> the fade's hold time starts
    if (wave::bits (before) != wave::bits (c) && wave::bits (c) == 0)
        o.fadeTimer = t.fadeHold;
    // 8.15.5: test bit
    const bool testWas = (before & ctrl::test) != 0;
    const bool testIs = (c & ctrl::test) != 0;
    if (! testWas && testIs)
    {
        o.phase = 0;
        o.noisePipe = 0;
        o.latch = o.noise;
        o.fillTimer = t.testHold;
    }
    else if (testWas && ! testIs)
        noiseShift (n, wave::bits (before), wave::bits (c));
    // 8.15.4 "Writes": gate edges
    Env& e = env[idx (n)];
    const bool gateWas = (before & ctrl::gate) != 0;
    const bool gateIs = (c & ctrl::gate) != 0;
    if (! gateWas && gateIs)
    {
        e.target = static_cast<std::uint8_t> (EnvPhase::attack);
        e.phasePipeline = 2;
        if (e.resetPending || e.dividerPipeline == 2)
            e.stepPipeline = (e.dividerPeriod == 1 || e.dividerPipeline == 2) ? 2 : 4;
        else if (e.dividerPipeline == 1)
            e.phasePipeline = 3;
    }
    else if (gateWas && ! gateIs)
    {
        e.target = static_cast<std::uint8_t> (EnvPhase::release);
        e.phasePipeline = e.stepPipeline > 0 ? 3 : 2;
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
            // The whole increment, used from the next step; the accumulator keeps its value and its fraction.
            o.inc = static_cast<std::uint64_t> (regs[base + reg::freqLo] | (regs[base + reg::freqHi] << 8)) << kFractionBits;
            break;
        case reg::pwLo:
        case reg::pwHi:
            o.pw = static_cast<std::uint16_t> (regs[base + reg::pwLo] | ((regs[base + reg::pwHi] & 0x0f) << 8));
            o.pwSmooth = static_cast<float> (o.pw);
            break;
        case reg::control: writeControl (n, before); break;
        case reg::attackDecay: writeAttackDecay (n); break;
        case reg::sustainRelease: writeSustainRelease (n); break;
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
        // round() = half away from zero, as 8.4.6 says "round". The chip accepts 0 (a register write may be
        // 0); the 1..65535 clamp of 8.4.6 belongs to the pitch-to-register mapping of the program engine.
        const double fr = std::fmin (std::fmax (std::round (f), 0.0), 65535.0);
        fReg = static_cast<std::uint32_t> (fr);
        o.inc = static_cast<std::uint64_t> (fReg) << kFractionBits;
        o.pw = static_cast<std::uint16_t> (std::fmin (std::fmax (std::round (pw), 0.0f), 4095.0f));
        o.pwSmooth = static_cast<float> (o.pw);
    }
    else
    {
        const double fc = std::fmin (std::fmax (f, 0.0), kMaxSmoothFrequency);
        const auto inc = static_cast<std::uint64_t> (std::llround (fc * 4294967296.0));
        o.inc = std::min (inc, kMaxIncrement);
        fReg = static_cast<std::uint32_t> (std::min<std::uint64_t> (o.inc >> kFractionBits, 65535));
        o.pwSmooth = std::fmin (std::fmax (pw, 0.0f), 4095.0f);
        o.pw = static_cast<std::uint16_t> (std::floor (o.pwSmooth)); // the integer part follows the register rules
    }
    regs[base + reg::freqLo] = static_cast<std::uint8_t> (fReg & 0xff);
    regs[base + reg::freqHi] = static_cast<std::uint8_t> (fReg >> 8);
    regs[base + reg::pwLo] = static_cast<std::uint8_t> (o.pw & 0xff);
    regs[base + reg::pwHi] = static_cast<std::uint8_t> (o.pw >> 8);
    regs[base + reg::attackDecay] = in.attackDecay;
    writeAttackDecay (n);
    regs[base + reg::sustainRelease] = in.sustainRelease;
    writeSustainRelease (n);
    const std::uint8_t before = regs[base + reg::control];
    regs[base + reg::control] = in.control;
    writeControl (n, before);
}

void Chip::setFilter (const FilterInput& in) noexcept
{
    const float fc = std::isfinite (in.cutoff) ? std::fmin (std::fmax (in.cutoff, 0.0f), 2047.0f) : 0.0f;
    cutoff = cfg.authentic ? std::round (fc) : fc;
    const auto fcReg = static_cast<std::uint32_t> (cfg.authentic ? cutoff : std::floor (fc));
    regs[reg::cutoffLo] = static_cast<std::uint8_t> (fcReg & 7u);
    regs[reg::cutoffHi] = static_cast<std::uint8_t> (fcReg >> 3);
    // Bit 3 (external input) is not modelled and always 0 (8.4.1); a trace's write() stores what it wrote.
    regs[reg::resonanceRouting] = static_cast<std::uint8_t> (((in.resonance & 0x0f) << 4) | (in.routing & 0x07));
    regs[reg::modeVolume] = in.modeVolume;
}

std::uint8_t Chip::read (std::uint8_t address) const noexcept
{
    switch (address & reg::kAddressMask)
    {
        case reg::osc3Output: return static_cast<std::uint8_t> (osc[2].read >> 4);
        case reg::env3Output: return env[2].readSample;
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
        so.pulseWidth = o.pw;
        so.noise = o.noise;
        so.noiseLatch = o.latch;
        so.noiseOutput = o.noiseOut;
        so.noisePipeline = o.noisePipe;
        so.firstShift = o.firstShift;
        so.rise = o.rise;
        so.fillTimer = o.fillTimer;
        so.fadeTimer = o.fadeTimer;
        so.output = o.out;
        so.readValue = o.read;
        so.readLatch = o.readLatch;
        so.pulseHigh = o.pulseHigh;
    }
    s.env = env;
    s.registers = regs;
    s.osc3Output = read (reg::osc3Output);
    s.env3Output = read (reg::env3Output);
    return s;
}
} // namespace vs::bside::chip
