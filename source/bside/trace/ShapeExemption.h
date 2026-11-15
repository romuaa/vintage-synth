#pragma once

// The exemption of shape-dependent fields from the exact comparison (docs/architecture.md 8.15.8).
// Our combined-waveform tables are our own fits, so every field that a combined-waveform table value
// reaches can differ from the reference until the state it reached is overwritten. This tracker marks
// those fields from the trace's writes and our own chip's per-step events (Chip.h, OscStepEvent) by
// following where a table value can flow, as 8.15.2 defines the flows:
//
//   output (c)       a combined-waveform table (wave::usesTable: two or more of triangle, saw and pulse, or
//                    pulse with noise; noise with only triangle or only saw uses none), an uncertain accumulator top bit
//                    of its own (pulse comparison, index) or of its ring source (ring set, saw clear),
//                    or an uncertain noise output when noise is selected
//   read value       Model A and Model B without triangle/saw: the output; Model B with triangle or saw:
//                    the read latch (a shape from the previous such step), the masks of this step and
//                    the combined table; a fade step copies the output
//   Model B latch    this step's shape
//   accumulator      Model A, saw selected and an uncertain output: the pull-down of bit 23 (bit 23 only);
//                    a sync whose source's rise (or, for the chain rule, its source's rise) is uncertain
//                    (every bit); cleared by the test bit (acc := 0) and by a sync reset whose source's rise
//                    is certain (acc := 0)
//   noise register   (with its latch) a write-back of an uncertain output (c step 6, or the latch's tap bits
//                    at a shift, 8.15.3, also at the write that clears the test bit); shifts at uncertain
//                    times (an accumulator uncertain in every bit, running); cleared when a fill under the
//                    test bit has made the register all ones
//   noise output     the register's at every shift and fill; ANDed with the output at a write-back; the
//                    output itself where step 6 copies it (pipeline in its latch step, or the test bit)
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
            else if (testWas && ! testIs)
            {
                // The shift at the write: the latch's tap bits may take the output first (8.15.3).
                if (chip::wave::shiftWritesBack (model, chip::wave::bits (before), chip::wave::bits (value)) && out[i])
                    noiseReg[i] = true;
                noiseOut[i] = noiseReg[i];
            }
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
        if (accAll[i] && freq[i] != 0 && (c & ctrl::test) == 0)
            noiseReg[i] = noiseOut[i] = true; // bit-19 rises (and so shifts) at uncertain times
        if (ev.shiftWroteBack && out[i])
            noiseReg[i] = true;
        if (ev.shifted || ev.filled)
            noiseOut[i] = noiseReg[i]; // recomputed from the register
        if (ev.filledToOnes)
            noiseReg[i] = noiseOut[i] = false; // register, latch and noise output all ones / their output
        // (c)
        if (ev.outputComputed)
        {
            const bool own = accTop[i] || accAll[i];
            const bool src = (c & ctrl::ring) != 0 && (c & ctrl::saw) == 0 && (accTop[s] || accAll[s]);
            const bool shapeT = wave::usesTable (w) || own || src;
            const bool pulseT = (c & ctrl::pulse) != 0 && (c & ctrl::test) == 0 && own;
            const bool noiseT = (c & ctrl::noise) != 0 && noiseOut[i];
            const bool outT = shapeT || pulseT || noiseT;
            if (model == chip::Model::A && (c & ctrl::saw) != 0 && outT && ownPullDown)
                accTop[i] = true;
            if (model == chip::Model::B && (c & (ctrl::triangle | ctrl::saw)) != 0)
            {
                read[i] = latch[i] || pulseT || noiseT || wave::usesTable (w);
                latch[i] = shapeT;
            }
            else
                read[i] = outT;
            if (ev.wroteBack)
            {
                noiseReg[i] = noiseReg[i] || outT;
                noiseOut[i] = noiseOut[i] || outT;
            }
            else if (wave::writesBack (c))
                noiseOut[i] = outT; // noise output := output (pipeline in its latch step, or the test bit)
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
                else if (syncedThisStep[t])
                    accTop[t] = accAll[t] = false; // a certain reset: the accumulator is 0, as in the reference
            }
            for (std::size_t k = 0; k < 3; ++k)
                pulledDownUncertain[k] = syncedThisStep[k] = false;
        }
    }

    /** A sync reset of `target` in the current step (the chip reports it before the step's oscStep events). */
    void syncReset (int target) noexcept { syncedThisStep[static_cast<std::size_t> (target)] = true; }

    bool accExempt (int n) const noexcept { return accTop[static_cast<std::size_t> (n)] || accAll[static_cast<std::size_t> (n)]; }
    bool noiseExempt (int n) const noexcept { return noiseReg[static_cast<std::size_t> (n)]; }
    bool readExempt() const noexcept { return read[2]; }

private:
    /** Whether oscillator k's rise flag (after (c)) can differ from the reference's. */
    bool sourceUncertain (std::size_t k) const noexcept { return accTop[k] || accAll[k] || pulledDownUncertain[k]; }

    chip::Model model;
    bool ownPullDown;
    std::array<std::uint8_t, 3> control {};
    std::array<std::uint16_t, 3> freq {};
    std::array<bool, 3> accTop {}, accAll {}, noiseReg {}, noiseOut {}, out {}, read {}, latch {}, pulledDownUncertain {}, syncedThisStep {};
};

/** The chip sink that feeds a ShapeExemption (step events only; the chip then steps every cycle). */
struct ShapeExemptionSink
{
    static constexpr bool kWantsNoiseEvents = false;
    static constexpr bool kWantsStepEvents = true;
    ShapeExemption* tracker = nullptr;
    void syncReset (const chip::SyncResetEvent& e) noexcept { tracker->syncReset (e.target); }
    void noiseClock (const chip::NoiseClockEvent&) noexcept {}
    void noiseWriteBack (const chip::NoiseWriteBackEvent&) noexcept {}
    void oscStep (const chip::OscStepEvent& e) noexcept { tracker->oscStep (e); }
};
} // namespace vs::bside::trace
