#pragma once

// One chip's whole analog chain (docs/architecture.md 8.19.4, S3b-1 and S3b-2): the renderer's two buses
// (Chip::renderBuses, Model A's voices bent in the renderer) -> Filter (borrowed) -> OutputStage, with the anti-alias
// stages of the static curves between them ("ALIAS") -> the output in the reference's units, in double precision up to
// one shaped conversion to float where a float is needed (8.20.1). JUCE-free; allocates nothing (not even in prepare);
// no locks, no exceptions; every loop is bounded. Construct and prepare it off the audio thread (Chip::prepare builds
// tables).
//
// ALIAS (S3b-2; HalfBand.h). The static curves run on band-limited signals:
// - Model A's voice bend: in the renderer (Chip::renderBendVoices, on here; Chip.h "BEND"). No delay.
// - Model B: the mixer's input (gain x the sum of the filter's output and the direct bus) through a half-band FIR at the
//   internal rate (pass band to 20 kHz within 0.01 dB), the curve at the internal rate.
// - Model A: the same half-band FIR on both buses before the filter (the filter bus's audio and envelopes, its voices'
//   DC, and the direct bus's whole sum); the filter at the internal rate; then the filter's output function
//   (Filter::outputShape of its low-pass value), the mixer gain and the mixer curve at twice the internal rate, between
//   a half-band interpolator and a half-band decimator (pass band to 20 kHz). In Shared the 2x part runs here, per chip,
//   on this chip's share; the owner hands over the shared filter's linear output and low-pass value.
// - Delays: Model B the FIR's (2 P - 1 samples); Model A the bus FIR's plus the 2x part's (Pu + Pd - 1). Both added to
//   latencySamples (). Writes that the filter or the stage read land where the delayed signal lands ("TIMING").
// - Configurations blended across a volume, mode or resonance write (below) are blended BEFORE the anti-alias stages: the filter's
//   outputs of the configurations (each configuration's output equation on the filter's one state, "TIMING") and their
//   gains are blended linearly at the mixer's input, and what follows the curve (the volume, Y0, the static DC's subtraction, the post gain) is blended
//   as coefficients, delayed with the signal. One set of anti-alias filters serves every configuration (no per-copy
//   FIR state): exact for a volume write (only post-curve coefficients differ); for a mode write the curve sees the
//   blend of the two configurations' inputs instead of blending their outputs, a difference of second order in their
//   difference over the window (measured: the write budget below).
//
// TOPOLOGY (8.5). Per Note: the voice drives the filter it was lent from its own registers (cutoff, resonance, mode)
// and runs it (renderUntil). Shared: the owner runs one Filter for n chips: each voice renders its buses
// (renderBusesUntil), the owner sums filterInputAc over the chips plus one chip's staticFilterBusDc into the shared
// filter, PER BUS SAMPLE, and hands each voice the filter's output and, per sample too, the filter's static output
// for that one chip's static DC (finishShared); each voice takes its share (OutputStage::sharedShare: the static
// output in full, 1/n of the rest) and subtracts the static output with volume DC off, both per sample, so Shared
// output does not depend on the owner's block length, also across routing and mute writes. The owner sets the shared
// filter's cutoff, resonance and mode itself (the most recent note's), and applies each such write to the shared filter
// at a bus sample (bus sample i of a renderBusesUntil call started at renderSampleIndex () r is bus sample r + i):
// - a cutoff write filterLagSamples () after the chip sample the write was made in (the voice does the same Per Note);
// - a mode or resonance write at sharedSwitchSample () (asked of any one chip at the write): the bus sample whose
//   switch lies nearest the write's place. Per Note blends the two configurations over the write's window; a switch at
//   one sample is within half a sample of that (tested at every rate, six write positions and at every cycle of three
//   samples, mode and resonance, both models: the switch within 0.50 samples of Per Note's blend; the output's step
//   within 0.45 samples of Per Note's on Model B, 0.56 on Model A's mode writes, whose step the 2x part's interpolated
//   output function stretches about 1.2 times; identical outside the window). The 0.02-sample timing of 8.19.4 is Per
//   Note's. To reach it in Shared (S4 OBLIGATION, 8.19.4 after the second review: the plugin's owner does this or records
//   the half-sample deviation): take the write's place from Chip::renderEventTime () and its band-limited weights from
//   Chip::unitStepTaps () (as ChipVoice::queue does), run two filters over the write's window (one keeps the old
//   configuration, one takes the new; their states part for a resonance write, which the weights confine to the
//   window), and blend per bus sample their linear outputs, their low-pass values and their output functions
//   (Filter::Shape) with those weights before handing them to finishShared.
// The voice's own 0x18 still sets its stage (volume, the mixer's gain for the selected modes, oscillator 3's mute).
// Shared runs no filter copy in the voice. The owner hands over the shared filter's output function per bus sample too
// (finishShared: its Filter::Shape), so a mode or resonance write inside an owner block takes effect at its sample.
// - n = 1 is Per Note bit for bit (tested, routing and mute writes, volume DC on and off).
// - Model B with n chips equals n Per Note chips for IDENTICAL voices only (tested, n = 1, 2, 16). With different
//   notes it does not: each chip's mixer curve sees its share of the summed filter output, not its own filter's
//   output (a property of the topology, 8.5). Measured (2026-11-23, Model B, 96 kHz, oscillators 1 and 2 of each chip
//   on different notes and waveforms, all routed to the filter, low-pass at 900, resonance 4): the difference from
//   Per Note is -38.8 dB (n = 2) and -31.8 dB (n = 16) rms re Per Note's audio; with mixed routings -32 / -30 dB.
//   Pinned for n = 2 by "the Shared topology with different notes" (ChipVoiceTests.cpp).
//
// LATENCY. latencySamples (), fixed at prepare: the renderer's (Chip::kLatencySamples) plus the anti-alias stages'
// ("ALIAS"): at 96 kHz 8 + 21 (Model B) and 8 + 21 + 11 (Model A); at 88.2 kHz 8 + 37 and 8 + 37 + 13.
//
// TIMING. Audio, envelopes and voice counts reach the filter filterLagSamples () after the output sample that holds
// them leaves the renderer (its latency, plus Model A's bus pre-filter), and every register write that the filter or
// the stage read is applied at that signal time, so a write lands at its cycle in the output (Chip::renderEventTime);
// what follows the filter (the gain, the curve, the volume, the static DC) is delayed with the signal:
// - ROUTING (0x17 bits 0..2) and oscillator 3's mute (0x18 bit 7): in the renderer, a band-limited step of each
//   oscillator's bus membership, of the voice counts and of the envelope sums (Chip.h "BUSES"); the stage takes the
//   counts per sample (OutputStage::setVoices: the voice DC and the mixer's gain follow, band-limited). Like volume
//   and mode writes they take effect one cycle before the renderer's convention for an oscillator write, as the
//   reference shows them (Chip::kAnalogWriteLeadCycles).
// - ONE STATE (8.19.4 after the third review). Per Note the filter has one state, whatever writes are pending: every
//   change of its dynamics is applied to that state at its own place (a resonance write at the write's place between
//   two samples, a cutoff write from its own sample on), and every configuration being blended (below) is only an
//   output equation (Filter::peekLinearDouble: its parameters at the one state) plus its stage. So no write moves
//   another: a volume, mode, routing or cutoff write inside a resonance write's window leaves the resonance's place
//   where it was, and the cutoff reaches the one state at its sample. setFilter is safe at any time (the dynamics
//   changes still waiting belong to the filter left and are dropped; nothing is run on a missing filter).
// - VOLUME and MODE (0x18 bits 0..6): band-limited. The configuration before the write (a copy of the stage and, Per
//   Note, of the filter's output parameters, evaluated on the one state) and the one after it are both computed over
//   the write's window, and their mixer outputs (and post gains, volume DC off) are blended with the
//   long kernel's unit step at the write's position (Chip::unitStepTaps) before the board filters. With volume DC on
//   a volume write is then the chip's own step (sample playback) at its full size, band-limited, never smoothed.
//   Capacity: every volume, mode or resonance write that changes something opens a configuration for its window (the
//   long kernel, 16 samples); kMaxConfigs (4) earlier configurations and the current one, five in all, are blended at
//   once, and a further such write while four windows are open retires the oldest at once (a step that is not
//   band-limited: only with five or more changing volume, mode or resonance writes within 16 samples). Tested ("at
//   most kMaxConfigs + 1 configurations", six writes at one cycle: five blended, the last write's state reached).
// - RESONANCE (0x17 bits 4..7): its output equation is blended like a mode write's (the configuration before keeps the
//   old resonance's output parameters), so it lands at the write's place, band-limited (tested to 0.02 samples). Its
//   DYNAMICS change at the write's place (the centre of the blend's weights), which lies between two samples: the step
//   of the one state that contains it is mixed from the step with the old parameters (the part of the step before the
//   place) and with the new (the rest), exact to first order in the place (stepDynamics; the newest output parameters
//   are held apart, outNow, until then). Several changes placed in one sample mix in turn (up to kMaxStepMix = 4; more
//   in one sample: the last kept one takes the newest resonance at its place). Capacity: kMaxPending (32) changes may
//   wait for their place; a further one applies the oldest at once (only with 32 resonance changes inside one window).
//   They wait in order and never interact with the blend's capacity (kMaxConfigs): retiring a configuration early moves
//   no dynamics. Tested: the place by itself against a twin filter, to the 0.05-sample grid of the test (a place one
//   sample off fails); the excess over an ideal render at 32 times the rate for a write every tick, alone and with a
//   volume, mode or cutoff write 1, 10, 40 or 200 cycles before or after it, both models, 88.2, 96 and 192 kHz (at most
//   -90 dB up to 2 kHz; the old two-copy placement measured -81 dB with a volume write 10 cycles later).
// - CUTOFF (0x15, 0x16, setFilter): from the output sample of the write's own sample on (not blended; a cutoff jump is
//   the filter's own change of state, which the chip makes too), in the one state's dynamics and in every
//   configuration's output parameters alike, whatever else is pending (tested 30 cycles before, at and 30 cycles after
//   a resonance write: the twin with the cutoff at its own sample equals the voice). The cutoff runs
//   per sample through Filter::processWithCutoff while it moves (a change less than kCutoffSettle samples after the
//   previous one) and through setCutoff + process once it has stood still for kCutoffSettle samples.
// - A write before the first render is part of the state the render starts on.
// QUEUE. Writes pending in the output wait in two queues of kMaxPending, in order: the cutoff (0x15, 0x16, setFilter)
// in one, and the configurations in the other (0x18, and 0x17 for its resonance nibble, since resonance writes blend
// like mode writes). A
// write waits about latencySamples () + 1 samples, so a queue fills only with more than kMaxPending writes in that
// time (a cutoff write every few cycles at the lowest rates; any number of writes at one cycle). A write that finds
// its queue full first applies the OLDEST pending entry at once, as if its sample had come (the order and every
// value are kept; that one entry lands up to latencySamples () + 1 samples early; a 0x18 entry only opens its
// blending window early, where its weight is still 0, so it lands exactly), then waits in the queue itself. Tested
// ("a full write queue").
//
// SPLITS. The output does not depend on how render calls are split: the renderer's buses do not (Chip), and every
// decision here is taken per output sample from the sample index and the queued writes; the anti-alias filters run
// once per bus sample.

#include "bside/chip/Chip.h"
#include "bside/chip/Filter.h"
#include "bside/chip/HalfBand.h"
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
    /** The anti-alias filters' design (HalfBand.h): pass band to 20 kHz within 1.15e-3 (0.01 dB), which for a half-band
        is also the stop band's level (about -59 dB); the fewest pairs per rate that meet it. Three of them in Model A's
        path: at most 0.03 dB in the pass band. */
    static constexpr double kAntiAliasPassHz = 20000.0, kAntiAliasDeviation = 1.15e-3;
    /** Model A's 2x half-bands (interpolators and decimator): 1e-5, about 100 dB, so that what leaks through them (an image
        of the band at twice the rate, the decimator's stop band) stays below what the curves at four times the rate
        alias (8.19.4 after the review, statement (c)); a pass band within 0.0001 dB each. */
    static constexpr double kTwiceDeviation = 1.0e-5;

    ChipVoice() noexcept;

    /** The chain's delay against chip time, in internal samples: output sample m holds the signal at the start of
        internal sample m - latencySamples (). Fixed at prepare: the renderer's (Chip::kLatencySamples) plus the
        anti-alias stages' ("ALIAS"). */
    int latencySamples() const noexcept { return latency; }
    /** Output sample m was produced while the filter ran on the signal of chip sample m - filterLagSamples () (the
        renderer's latency + 1, plus Model A's bus pre-filter): where a cutoff or resonance write takes effect. */
    int filterLagSamples() const noexcept { return kRingLag + preDelay; }
    /** Shared ("TOPOLOGY"): the bus sample at which the owner of the shared filter switches it for a mode or resonance
        write made at the chip's current cycle (call it right before or after the write): the sample whose switch puts
        the step nearest the write's place, so within half a sample of Per Note's band-limited step. Bus sample i of a
        renderBusesUntil call that started at renderSampleIndex () r is bus sample r + i. -1: the write comes before
        the first render (part of the state the render starts on: apply it at once). */
    std::int64_t sharedSwitchSample() noexcept;

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
    /** What this chip adds to the shared filter's input at bus sample i (no voice DC; Model A: through its bus
        pre-filter, "ALIAS"). */
    double filterInputAc (int i) const noexcept;
    /** The static DC of this chip's filter bus (its voices' DC) at bus sample i (Model A: through its bus pre-filter):
        the owner adds one chip's. */
    double staticFilterBusDc (int i) const noexcept;
    /** Shared, second half, for the buses of the last renderBusesUntil (n samples), per bus sample i: sharedShape[i],
        the shared filter's output function at that sample (Filter::outputShapeCoefficients (); Model A; may be null on
        Model B); sharedLinear[i], its LINEAR output (Filter::processLinearDouble; Model B: its whole output);
        sharedLowPass[i], its low-pass value (Model A; may be null on Model B); filterStaticOut[i], its static output for
        the static DC the owner fed in at that sample (Filter::staticOutput (that chip's staticFilterBusDc (i))); then
        chips, the number of chips feeding it; out, to which the output is ADDED; n. */
    void finishShared (const Filter::Shape* sharedShape, const double* sharedLinear, const double* sharedLowPass, const double* filterStaticOut,
                       int chips, double* out, int n) noexcept;

    /** Test aid: the chain run on the given buses instead of the chip's (Per Note; the chip is not rendered, no write may
        be pending; the sample index counts on from the last such call): ADDS n output samples to out. */
    void processBuses (const Chip::BusBlock& in, int n, double* out) noexcept;

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
    /** Test aid: the cutoff (register value) the filter ran with at its last sample. */
    float cutoffInEffect() const noexcept { return cutoffValue; }
    /** Test aid: the filter's input and its whole output (Model A: with its output function) at its last sample (Per
        Note). */
    double lastFilterInput() const noexcept { return lastFilterIn; }
    double lastFilterOutput() const noexcept { return lastFilterOut; }
    /** Test aid: how many writes found their queue full (QUEUE above) since prepare or reset. */
    std::uint64_t queueOverflows() const noexcept { return overflows; }

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
    /** A change of the filter's dynamics at a place between two samples (a resonance write, "TIMING"): the step from
        filter sample `at` to `at + 1` is taken partly with the parameters before it (the fraction `alpha` of the step)
        and partly with the new resonance. */
    struct DynChange
    {
        std::int64_t at = 0;
        double alpha = 0.0;
        int resonance = 0;
    };

    /** A ring sample is output this many samples after it completes (the renderer's latency + 1). */
    static constexpr int kRingLag = Chip::kLatencySamples + 1;
    static constexpr int kDelayRing = 64; // > the longest delay of a post-filter quantity (2 x 32 - 1)

    void queue (std::uint8_t reg, std::uint8_t value, float cutoffValue) noexcept;
    void applyNow (const Pending& p) noexcept;
    void applyParam (const Pending& p, std::int64_t q) noexcept;
    void beginConfig (const Pending& p) noexcept;
    /** The output parameters of the configuration in effect (the newest written: they differ from the filter's own while a
        resonance change waits for its place). */
    const Filter& outParams() const noexcept { return outDiffers ? outNow : *filter; }
    Filter& outParams() noexcept { return outDiffers ? outNow : *filter; }
    /** One configuration's linear output and low-pass value at the filter's state (nothing advances). */
    double peekConfig (Filter& outputParams, double in, double& lowPass) noexcept;
    /** Advances the filter's state by one sample at filter time t, the dynamics changes placed in that sample mixed in. */
    void stepDynamics (double in, std::int64_t t) noexcept;
    /** Appends a dynamics change (beyond kMaxPending: the oldest is applied at once). */
    void pushDynChange (const DynChange& d) noexcept;
    void refreshStaticDc() noexcept;
    /** Applies what is due at the filter's signal time t (chip sample). */
    void advanceTo (std::int64_t t) noexcept;
    /** Model A: the bus pre-filters over bus samples 0 .. n of the block, and the voice counts at the filter's time. */
    void prepareBlock (int n) noexcept;
    /** One output sample from bus sample i at ring sample q (Per Note when `shared` is false; Shared: the shared filter's
        output function, linear output, low-pass value and static output at that sample, and the number of chips). */
    double chainSample (int i, std::int64_t q, bool shared, const Filter::Shape* sharedShape, const double* sharedLinear, const double* sharedLowPass,
                        const double* sharedStaticOut, int chips) noexcept;
    double runFilter (Filter& f, double in, double& lowPass) const noexcept;
    static double weightAt (std::int64_t q, std::uint64_t sample, int first, const std::array<double, render::kKernelTaps>& taps) noexcept;
    Chip::BusBlock block() noexcept;

    Chip c;
    OutputStage st;
    Filter* filter = nullptr;
    Config cfg {};
    bool board = true;
    int latency = Chip::kLatencySamples;
    std::uint64_t overflows = 0;

    // the anti-alias stages ("ALIAS")
    int preDelay = 0;   // Model A: the bus pre-filter's delay (0 on Model B)
    int postDelay = 0;  // from the mixer input's blend to the curve's output: Model B the FIR's, Model A the 2x part's
    int shapeDelay = 0; // Model A: the output function's coefficients' delay to the interpolated low-pass value
    HalfBandFir mixFir;                     // Model B
    HalfBandFir preAc, preDc, preDirect;    // Model A: the filter bus's audio and envelopes, its voices' DC, the direct sum
    HalfBandUp upLin, upLow;                // Model A: the 2x part's inputs
    HalfBandDown down;                      // Model A
    bool freshPre = true, freshPost = true; // the filters are primed with their first input after reset
    double preDcNow = 0.0;                  // Model A: the pre-filtered voices' DC the filter's static output was taken for
    std::array<double, kBlock> pAc {}, pDc {}, pDi {}, cF {}, cD {}; // per bus sample of the block (Model A pre-filtered)
    std::array<std::array<double, 3>, kDelayRing> postRing {};   // (postScale, postOffset, postGain) blended
    std::array<std::array<double, 2>, kDelayRing> countRing {};  // Model A: the voice counts per ring sample
    std::array<std::array<double, 5>, kDelayRing> shapeRing {};  // Model A: the blended output-function coefficients
    int postPos = 0, countPos = 0;
    double lpMin = -1.0e9, lpMax = 1.0e9;  // Model A's output function's limits (constant)
    double lastFilterIn = 0.0, lastFilterOut = 0.0;
    std::uint64_t injected = 0;            // processBuses' sample index

    // the buses of the current inner block
    std::array<double, kBlock> fa {}, da {}, fe {}, de {}, fv {}, dv {};
    std::uint64_t blockStart = 0; // the chip's sample index of bus sample 0

    // writes not yet applied in the output: parameters (cutoff, resonance) and configurations (0x18), each in order
    std::array<Pending, kMaxPending> params {}, configs {};
    int paramHead = 0, paramCount = 0, configHead = 0, configCount = 0;

    // configurations being blended, oldest first (the current one is `st` and outParams ()); in Per Note they share the
    // filter's one state, each with its own output parameters
    std::array<Old, kMaxConfigs> old {};
    int nConfigs = 0;
    // dynamics changes waiting for their place, in order; the newest written output parameters while any waits
    std::array<DynChange, kMaxPending> dyn {};
    int dynHead = 0, dynCount = 0;
    Filter outNow;
    bool outDiffers = false;
    static constexpr int kMaxStepMix = 4; // dynamics changes mixed into one sample's step; more in one sample: see stepDynamics
    std::array<Filter, kMaxStepMix + 1> stepCopy {};

    // the cutoff (register value) and whether it moves
    float cutoffValue = 0.0f;
    std::int64_t lastCutoffChange = 0;
    bool cutoffChanged = false, moving = false;
    std::uint8_t resonanceNow = 0, modeVolumeNow = 0;
    double lastFv = -1.0, lastDv = -1.0;

    render::OutputRounding rounding {};
};
} // namespace vs::bside::chip
