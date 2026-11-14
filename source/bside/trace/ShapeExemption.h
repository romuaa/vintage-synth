#pragma once

// The exemption of shape-dependent fields from the exact comparison (docs/architecture.md 8.15.8).
// Our combined-waveform tables are our own fits, so every field that a combined-waveform table value
// reaches can differ from the reference until the state it reached is overwritten. This tracker marks
// those fields from the trace's writes and our own chip's per-step events (Chip.h, OscStepEvent) by
// following where a table value can flow, as 8.15.2 defines the flows:
//
//   output (c)       a combined waveform (more than one waveform bit), an uncertain accumulator top bit
//                    of its own (pulse comparison, index) or of its ring source (ring set, saw clear),
//                    or an uncertain noise output when noise is selected
//   read value       Model A and Model B without triangle/saw: the output; Model B with triangle or saw:
//                    the read latch (a shape from the previous such step), the masks of this step and
//                    the combined table; a fade step copies the output
//   Model B latch    this step's shape
//   accumulator      Model A, saw selected and an uncertain output: the pull-down of bit 23 (bit 23 only);
//                    a sync whose source's rise (or, for the chain rule, its source's rise) is uncertain
//                    (every bit); cleared only by the test bit (acc := 0)
//   noise register   a write-back of an uncertain output (c step 6, or the latch's tap bits at a shift,
//                    8.15.3, also at the write that clears the test bit); shifts at uncertain times (an
//                    accumulator uncertain in every bit, running); cleared when a fill under the test bit
//                    has made the register all ones (register, latch and noise output are then known)
//
// The `.bsref` fields this exempts: accN (accumulator), noiseN (noise register), rd1B (oscillator 3's
// read value). Envelope fields never depend on an oscillator, so they are never exempt. JUCE-free.

#include "bside/chip/Chip.h"

#include <array>
#include <cstdint>

namespace vs::bside::trace
{
class ShapeExemption
{
public:
    /** `accOwnPullDown`: also exempt an oscillator's own accumulator after a Model A pull-down by an
        uncertain output (8.15.8 names only the sync target; see docs/decisions.md). */
    explicit ShapeExemption (chip::Model m, bool accOwnPullDown = true) noexcept : model (m), ownPullDown (accOwnPullDown) {}

    /** A register write, applied to the chip at the same moment. */
    void write (std::uint8_t address, std::uint8_t value) noexcept
    {
        const std::uint8_t r = address & chip::reg::kAddressMask;
        if (r >= 3 * chip::reg::oscStride)
            return;
        const int n = r / chip::reg::oscStride;
        const auto i = static_cast<std::size_t> (n);
        const int field = r % chip::reg::oscStride;
        if (field == chip::reg::freqLo)
            freq[i] = static_cast<std::uint16_t> ((freq[i] & 0xff00u) | value);
        else if (field == chip::reg::freqHi)
            freq[i] = static_cast<std::uint16_t> ((freq[i] & 0x00ffu) | (static_cast<unsigned> (value) << 8));
        else if (field == chip::reg::control)
        {
            const std::uint8_t before = control[i];
            control[i] = value;
            const bool testWas = (before & chip::ctrl::test) != 0, testIs = (value & chip::ctrl::test) != 0;
            if (! testWas && testIs)
                accTop[i] = accAll[i] = false;
            else if (testWas && ! testIs && chip::wave::shiftWritesBack (model, chip::wave::bits (before), chip::wave::bits (value)) && out[i])
                noise[i] = true;
        }
    }

    /** One oscillator's step (the chip reports oscillators 1, 2, 3 of each step in order). */
    void oscStep (const chip::OscStepEvent& ev) noexcept
    {
        using namespace chip;
        const int n = ev.osc;
        const auto i = static_cast<std::size_t> (n);
        const auto s = static_cast<std::size_t> (detail::syncSource (n));
        const std::uint8_t c = ev.control;
        const std::uint8_t w = wave::bits (c);
        // (a)
        if (ev.shiftWroteBack && out[i])
            noise[i] = true;
        if (ev.filledToOnes)
            noise[i] = false;
        if (accAll[i] && freq[i] != 0 && (c & ctrl::test) == 0)
            noise[i] = true;
        // (c)
        if (ev.outputComputed)
        {
            const bool own = accTop[i] || accAll[i];
            const bool src = (c & ctrl::ring) != 0 && (c & ctrl::saw) == 0 && (accTop[s] || accAll[s]);
            const bool shapeT = wave::isCombined (w) || own || src;
            const bool pulseT = (c & ctrl::pulse) != 0 && (c & ctrl::test) == 0 && own;
            const bool noiseT = (c & ctrl::noise) != 0 && noise[i];
            const bool outT = shapeT || pulseT || noiseT;
            if (model == chip::Model::A && (c & ctrl::saw) != 0 && outT && ownPullDown)
                accTop[i] = true;
            if (model == chip::Model::B && (c & (ctrl::triangle | ctrl::saw)) != 0)
            {
                read[i] = latch[i] || pulseT || noiseT || wave::isCombined (w);
                latch[i] = shapeT;
            }
            else
                read[i] = outT;
            if (wave::writesBack (c))
                noise[i] = noise[i] || outT;
            out[i] = outT;
            if (model == chip::Model::A && (c & ctrl::saw) != 0 && outT)
                pulledDownUncertain[i] = true;
        }
        else if (ev.faded)
            read[i] = out[i];
        // (d), once all three oscillators of the step are in
        if (n == 2)
        {
            for (int k = 0; k < 3; ++k)
            {
                const auto t = static_cast<std::size_t> (detail::syncTarget (k));
                const auto ks = static_cast<std::size_t> (detail::syncSource (k));
                const auto kk = static_cast<std::size_t> (k);
                if ((control[t] & ctrl::sync) == 0)
                    continue;
                const bool riseT = sourceUncertain (kk) || ((control[kk] & ctrl::sync) != 0 && sourceUncertain (ks));
                if (riseT)
                    accAll[t] = true;
            }
            for (std::size_t k = 0; k < 3; ++k)
                pulledDownUncertain[k] = false;
        }
    }

    bool accExempt (int n) const noexcept { return accTop[static_cast<std::size_t> (n)] || accAll[static_cast<std::size_t> (n)]; }
    bool noiseExempt (int n) const noexcept { return noise[static_cast<std::size_t> (n)]; }
    bool readExempt() const noexcept { return read[2]; }

private:
    /** Whether oscillator k's rise flag (after (c)) can differ from the reference's. */
    bool sourceUncertain (std::size_t k) const noexcept { return accTop[k] || accAll[k] || pulledDownUncertain[k]; }

    chip::Model model;
    bool ownPullDown;
    std::array<std::uint8_t, 3> control {};
    std::array<std::uint16_t, 3> freq {};
    std::array<bool, 3> accTop {}, accAll {}, noise {}, out {}, read {}, latch {}, pulledDownUncertain {};
};

/** The chip sink that feeds a ShapeExemption (step events only; the chip then steps every cycle). */
struct ShapeExemptionSink
{
    static constexpr bool kWantsNoiseEvents = false;
    static constexpr bool kWantsStepEvents = true;
    ShapeExemption* tracker = nullptr;
    void syncReset (const chip::SyncResetEvent&) noexcept {}
    void noiseClock (const chip::NoiseClockEvent&) noexcept {}
    void noiseWriteBack (const chip::NoiseWriteBackEvent&) noexcept {}
    void oscStep (const chip::OscStepEvent& e) noexcept { tracker->oscStep (e); }
};
} // namespace vs::bside::trace
