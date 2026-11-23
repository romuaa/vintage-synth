#pragma once

// One chip's whole analog chain (docs/architecture.md 8.19.4, S3b-1): the renderer's two buses (Chip::renderBuses)
// -> Filter (borrowed) -> OutputStage -> the output in the reference's units, in double precision up to one shaped
// conversion to float where a float is needed (8.20.1). JUCE-free; allocates nothing (not even in prepare); no locks,
// no exceptions; every loop is bounded. Construct and prepare it off the audio thread (Chip::prepare builds tables).
//
// TOPOLOGY (8.5). Per Note: the voice drives the filter it was lent from its own registers (cutoff, resonance, mode)
// and runs it (renderUntil). Shared: the owner runs one Filter for n chips: each voice renders its buses
// (renderBusesUntil), the owner sums filterInputAc over the chips plus one chip's staticFilterBusDc into the shared
// filter, and hands each voice the filter's output (finishShared), which takes its share
// (OutputStage::sharedShare: the static output of one chip's static DC in full, 1/n of the rest). The owner then sets
// the shared filter's cutoff, resonance and mode itself (the most recent note's); the voice's own 0x18 still sets its
// stage (volume, the mixer's gain for the selected modes, oscillator 3's mute).
//
// TIMING. Audio, envelopes and voice counts reach the stage kLatencySamples late (the renderer's latency, 8.17); every
// register write that the filter or the stage reads is applied at the same place in the output, so a write lands at
// its cycle there (Chip::renderEventTime):
// - ROUTING (0x17 bits 0..2) and oscillator 3's mute (0x18 bit 7): in the renderer, a band-limited step of each
//   oscillator's bus membership, of the voice counts and of the envelope sums (Chip.h "BUSES"); the stage takes the
//   counts per sample (OutputStage::setVoices: the voice DC and the mixer's gain follow, band-limited).
// - VOLUME and MODE (0x18 bits 0..6): band-limited. The configuration before the write (a copy of the stage and, Per
//   Note, of the filter, which runs on beside the current one on the same input) and the one after it are both
//   computed over the write's window, and their mixer outputs (and post gains, volume DC off) are blended with the
//   long kernel's unit step at the write's position (Chip::unitStepTaps) before the board filters. With volume DC on
//   a volume write is then the chip's own step (sample playback) at its full size, band-limited, never smoothed. Up to
//   kMaxConfigs configurations are blended at once (writes less than a window apart); a further write retires the
//   oldest at once (a step that is not band-limited: only with more than three volume or mode writes in 17 samples).
// - CUTOFF (0x15, 0x16, setFilter) and RESONANCE (0x17 bits 4..7): from the output sample of the write's own sample
//   on (not blended; a cutoff jump is the filter's own change of state, which the chip makes too). The cutoff runs
//   per sample through Filter::processWithCutoff while it moves (a change less than kCutoffSettle samples after the
//   previous one) and through setCutoff + process once it has stood still for kCutoffSettle samples.
// - A write before the first render is part of the state the render starts on.
// Writes pending in the output are held in a queue of kMaxPending; a write beyond it is applied at once (bounded; not
// reached by a write every sample).
//
// SPLITS. The output does not depend on how render calls are split: the renderer's buses do not (Chip), and every
// decision here is taken per output sample from the sample index and the queued writes.

#include "bside/chip/Chip.h"
#include "bside/chip/Filter.h"
#include "bside/chip/OutputStage.h"
#include "bside/chip/RenderTables.h"

#include <array>
#include <cstdint>

namespace vs::bside::chip
{
class ChipVoice
{
public:
    /** Samples rendered per inner block (the buses are kept in fixed arrays of this size). */
    static constexpr int kBlock = 64;
    static constexpr int kMaxConfigs = 4;
    static constexpr int kMaxPending = 32;
    /** A cutoff that changes again within this many samples counts as moving (processWithCutoff). */
    static constexpr int kCutoffSettle = 256;
    static constexpr int kLatencySamples = Chip::kLatencySamples;

    ChipVoice() noexcept;

    /** The chip's configuration (model, clock, rate, resolution, volume DC, variation); resets everything. A Filter
        lent with setFilter is prepared for the same model, rate and variation by its owner. */
    void prepare (const Config& c) noexcept;
    /** The Filter this voice drives and runs Per Note (null: none; renderUntil then needs one). */
    void setFilter (Filter* f) noexcept;
    /** Power-on state at cycle 0 of the chip, the stage's board filters, every pending write; the lent filter's state too. */
    void reset() noexcept;

    // ---- the chip's inputs, at the chip's current cycle ----
    void write (std::uint8_t reg, std::uint8_t value) noexcept;
    void setOscillator (int n, const OscInput& in) noexcept;
    void setFilterInput (const FilterInput& in) noexcept;
    void powerOnKeepingTime() noexcept;

    /** Per Note: the chain's output (the reference's units, after the board filters) ADDED to out[0 .. n), until
        the chip's cycle reaches untilCycle or maxSamples are complete (Chip::renderUntil's rules). */
    int renderUntil (double* out, int maxSamples, std::uint64_t untilCycle) noexcept;
    void render (double* out, int numInternalSamples) noexcept;
    /** The same converted to float once, its rounding noise-shaped (render::OutputRounding, 8.20.1), ADDED to out. */
    int renderUntil (float* out, int maxSamples, std::uint64_t untilCycle) noexcept;

    /** Shared, first half: the buses of up to kBlock samples (returned count); then filterInputAc (i) for the shared
        filter's input and finishShared. */
    int renderBusesUntil (int maxSamples, std::uint64_t untilCycle) noexcept;
    /** What this chip adds to the shared filter's input at bus sample i (no voice DC). */
    double filterInputAc (int i) const noexcept;
    /** The static DC of this chip's filter bus (its voices' DC) at bus sample i: the owner adds one chip's. */
    double staticFilterBusDc (int i) const noexcept;
    /** Shared, second half: the shared filter's output for the buses of the last renderBusesUntil (n samples), the
        number of chips feeding it and the filter's static output for one chip's static DC; output ADDED to out. */
    void finishShared (const double* sharedFilterOut, int chips, double filterStaticOut, double* out, int n) noexcept;

    Chip& chip() noexcept { return c; }
    const Chip& chip() const noexcept { return c; }
    OutputStage& stage() noexcept { return st; }
    /** Test aid: off skips the board filters (the output is the blended mixer output times the blended post gain:
        "the chain's output before the board filters", 8.19.4's write budget). On by default. */
    void boardFiltersEnabled (bool on) noexcept { board = on; }
    /** Test aid: the configurations blended at the last sample (1 when no write's window is open). */
    int configsActive() const noexcept { return nConfigs + 1; }
    /** Test aid: whether the last sample ran the filter per sample (processWithCutoff). */
    bool cutoffMoving() const noexcept { return moving; }

private:
    struct Pending
    {
        std::uint64_t sample = 0; // the chip sample the write happened in (Chip::EventTime)
        int first = 0;            // the unit step's first tap, relative to `sample`
        std::array<double, render::kKernelTaps> taps {};
        std::uint8_t reg = 0, value = 0;
        float cutoff = 0.0f; // cutoff writes: the value after the write
    };
    struct Old
    {
        OutputStage stage;
        Filter filter;               // Per Note only
        std::uint64_t sample = 0;    // the transition to the next configuration (Pending's fields)
        int first = 0;
        std::array<double, render::kKernelTaps> taps {};
    };

    void queue (std::uint8_t reg, std::uint8_t value, float cutoffValue) noexcept;
    void applyNow (const Pending& p) noexcept;
    void applyParam (const Pending& p, std::int64_t q) noexcept;
    void beginConfig (const Pending& p) noexcept;
    void refreshStaticDc() noexcept;
    /** Applies what is due at ring sample q (the output sample q + kLatencySamples + 1). */
    void advanceTo (std::int64_t q) noexcept;
    /** One output sample from bus sample i (Per Note when sharedOut is null). */
    double chainSample (int i, std::int64_t q, const double* sharedOut, int chips, double filterStaticOut) noexcept;
    double runFilter (Filter& f, double in) const noexcept;
    static double weightAt (std::int64_t q, std::uint64_t sample, int first, const std::array<double, render::kKernelTaps>& taps) noexcept;
    Chip::BusBlock block() noexcept;

    Chip c;
    OutputStage st;
    Filter* filter = nullptr;
    Config cfg {};
    bool board = true;

    // the buses of the current inner block
    std::array<double, kBlock> fa {}, da {}, fe {}, de {}, fv {}, dv {};
    std::uint64_t blockStart = 0; // the chip's sample index of bus sample 0

    // writes not yet applied in the output: parameters (cutoff, resonance) and configurations (0x18), each in order
    std::array<Pending, kMaxPending> params {}, configs {};
    int paramHead = 0, paramCount = 0, configHead = 0, configCount = 0;

    // configurations being blended, oldest first (the current one is `st` and `filter`)
    std::array<Old, kMaxConfigs> old {};
    int nConfigs = 0;

    // the cutoff (register value) and whether it moves
    float cutoffValue = 0.0f;
    std::int64_t lastCutoffChange = 0;
    bool cutoffChanged = false, moving = false;
    std::uint8_t resonanceNow = 0, modeVolumeNow = 0;
    double lastFv = -1.0, lastDv = -1.0;

    render::OutputRounding rounding {};
};
} // namespace vs::bside::chip
