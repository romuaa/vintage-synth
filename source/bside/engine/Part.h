#pragma once

// One part (Lower or Upper) of Exitium B-Side (docs/architecture.md 8.5, 8.21.1): 16 note voices, each a whole chip
// driven by its own patch program engine, the shared allocator (assign modes, unison, pedal, steal fade), the host
// parameters as chip values (PartMappings.h), glide, bend, vibrato, the velocity gain, the Shared filter topology,
// the latency pad, one shaped conversion to float and the decimator. Satisfies shell::PartEngine.
//
// THE CHAIN (8.21.1, in double precision up to the conversion, 8.20.1): per note voice ProgramEngine -> sink ->
// ChipVoice (its own Filter Per Note) -> x note gain (x the 3 ms fade) -> the part's sum -> the latency pad -> x
// kChipGain -> ONE conversion to float through render::OutputRounding -> the decimator (common/engine/Oversampler.h,
// the factors of 4.4) -> out, mono, host rate.
//
// TIME (8.4.2). Chip time is an integer and nothing depends on the host's block length. All chips of a part keep
// one sample clock: every voice advances by the part's internal samples (rendered, or skipped while idle), and a
// reset or a structural switch resets them together, so a part sample has one cycle for every chip. An event at host
// sample k of a block acts at internal sample k x factor (the chip's cycle at that sample's start). A voice renders
// up to its engine's next tick (the sample containing it stays in progress), runs the tick, and goes on; segments end
// at events, control points, the end of a fade and every 64 internal samples. An idle voice leaves the sum and its
// chip's time keeps running (Chip::skipSamples, digital state only, caught up at the end of each render call and
// before a note starts on it); it starts settled (ChipVoice::suspend).
//
// CONTROL POINTS (8.21.1, review): every N = round (internal rate / 1000) internal samples of the part's own count
// (from prepare, reset and resume), in this order: the smoothed parameters, glide and the vibrato oscillator advance;
// then every voice whose engine runs gets its host inputs (setHost) and a sync at its current cycle; then the idle
// rule (8.5) is checked. A tick on the same cycle has run before (it ends the sample before).
//
// IDLE (8.5, 8.21.1): after its note-off a voice is idle when its three envelope counters are 0 in release and every
// wave lane has ended, or 30 s after the note-off: it gets enterIdle, renders on until its output at the part's
// output has stayed below -120 dB re full scale for one control interval (checked at control points; at most 500 ms,
// then a 3 ms fade), leaves the sum and the allocator gets voiceIdle (in the order the voices left: the sample, then
// the index).
//
// STRUCTURAL SWITCHES (8.21.1, review): chip.model, filter.topology, chip.clock, chip.resolution, chip.volume_dc. A
// change fades the part's sounding voices over 3 ms; when the fade ends every voice is reset into the new
// configuration (ChipVoice prototypes and filters built in prepare: no design work on the audio thread) and is idle;
// held keys do not retrigger (the allocator gets voiceIdle). A note started during the fade starts after it and is
// released as much later as it started (its length is kept; the same after a steal's fade); the fade is applied after
// the latency pad, so it reaches the output in full.
//
// SHARED (8.5, 8.19.4, 8.21.1): the part runs one Filter for its chips, fed per bus sample with the sum of their
// filterInputAc and the most recent note's staticFilterBusDc, and hands each chip its share (ChipVoice::finishShared).
// Cutoff, resonance and mode come from the most recently started note's writes: a cutoff from filterLagSamples ()
// after its sample (and run per sample while it moves, exactly as Per Note), a mode or resonance write at the sample
// ChipVoice::sharedSwitchSample () names (the nearest sample; the blend of ChipVoice.h "TOPOLOGY" is not built: the
// deviation of 8.19.4 is recorded). The voices render in lockstep: a segment is split where any voice's tick falls.
// One voice in Shared equals Per Note bit for bit (with no mode or resonance write after its start).
//
// LATENCY (8.21.1, review): one figure for both models, fixed in prepare: the longer chain's latency
// (ChipVoice::latencySamples) at the internal rate, the shorter chain padded to it by whole samples, plus the
// decimator's measured delay, rounded to the nearest host sample.
//
// Real-time: render, releaseAllNotes, reset, resume and the setters allocate nothing, take no lock, make no system
// call, throw nothing; every loop is bounded. prepare allocates and runs the design work (call it off the audio thread).

#include "bside/chip/ChipClock.h"
#include "bside/chip/ChipVoice.h"
#include "bside/chip/Filter.h"
#include "bside/chip/RenderTables.h"
#include "bside/engine/NoteVoice.h"
#include "bside/engine/ProgramEngine.h"
#include "bside/params/PartParamList.h"
#include "bside/program/Program.h"
#include "common/engine/NoteEvent.h"
#include "common/engine/VoiceAllocator.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace bs
{
class Part
{
public:
    static constexpr int kVoices = vs::kMaxVoices;
    /** Internal samples per lockstep segment at most in Shared (the chain's inner block). */
    static constexpr int kSegment = vs::bside::chip::ChipVoice::kBlock;
    /** Internal samples per span at most Per Note: each voice renders a whole span on its own (its control points,
        ticks, fades inside), so its chip's state stays in the cache for the span. */
    static constexpr int kSpan = 1024;
    /** The one gain between the chip's units and the part's output, both models (8.21.1): one note of the Init
        program at velocity 1 peaks at -12 dBFS +-0.5 dB at the part's output on Model B (tested; S4b's level rule
        may move it once). */
    static constexpr double kChipGain = 0.9739;
    static constexpr double kFadeSeconds = 0.003;          // steal, leave and structural-switch fades
    static constexpr double kSmoothSeconds = 0.005;        // continuous parameters, the note gain on a retrigger
    static constexpr double kQuietLevel = 1.0e-6;          // -120 dB re full scale at the part's output
    static constexpr double kLeaveSeconds = 0.5;           // the longest an idle voice renders on before its fade
    static constexpr double kIdleAfterNoteOffSeconds = 30.0;
    static constexpr double kQuietAfterSeconds = 0.1;      // silence before the quiet path (renderQuiet) is taken
    static constexpr std::uint32_t kClockHz[2] { 985248u, 1022727u }; // chip.clock: 50 Hz system, 60 Hz system

    Part();
    ~Part();
    Part (const Part&) = delete;
    Part& operator= (const Part&) = delete;

    /** Allocates and runs the design work: call from prepareToPlay only. `params` and `chipClock` (0 / 1) are the
        values the first block starts with (the smoothed values start at them). */
    void prepare (double hostRate, int maxBlockSize, int oversamplingFactor, const PartParams& params, int chipClock);

    // ---- once per block, before render (any order; applied at the start of the next render with samples) ----
    void setParams (const PartParams& p) noexcept { target = p; }
    void setChipClock (int index) noexcept { clockTarget = index != 0 ? 1 : 0; }
    /** The tempo for tick.rate = Tempo: the host's while it gives one, else arp.tempo (8.21.1). */
    void setTempo (double bpm) noexcept { tempo = bpm; }
    /** The part's committed table (8.14); a note copies it at its start. nullptr: no table. Must stay valid until
        the next call (the hand-over keeps the committed buffer until the next commit). */
    void setProgram (const vs::bside::program::Program* p) noexcept { program = p; }

    // ---- shell::PartEngine ----
    void render (float* out, int numSamples, std::span<const vs::NoteEvent> events) noexcept;
    void releaseAllNotes() noexcept;
    void reset() noexcept;
    void resume() noexcept;
    bool isSounding() const noexcept;
    bool isSustainDown() const noexcept { return allocator.isPedalDown(); }
    int getLatencySamples() const noexcept { return latencySamples; }

    // ---- tests and diagnostics (read between renders) ----
    double getExactLatency() const noexcept { return exactLatency; }
    int getOversamplingFactor() const noexcept { return osFactor; }
    double getInternalRate() const noexcept { return internalRate; }
    int getControlInterval() const noexcept { return controlInterval; }
    int getChainLatency (vs::bside::chip::Model m) const noexcept { return chainLatency[m == vs::bside::chip::Model::A ? 0 : 1]; }
    int getMaxChainLatency() const noexcept { return maxChainLatency; }
    /** The latency pad: how many internal samples it delays the current model's chain by, and what its ring holds. */
    int getPadDelay() const noexcept { return padDelay; }
    static constexpr int padCapacity() noexcept { return kPadRing; }
    std::int64_t getRenderedSamples() const noexcept { return partSample; }
    int numActiveVoices() const noexcept;
    const NoteVoice& getVoice (int i) const noexcept { return (*voices)[static_cast<std::size_t> (i)]; }
    const vs::VoiceAllocator& getAllocator() const noexcept { return allocator; }
    /** Bytes the part holds (its object, the voices, the prototypes; the decimator's buffers not counted). */
    static std::size_t memoryBytes() noexcept;
    /** Test aid: the Shared topology's filter (its cutoff, resonance and mode follow the most recent note). */
    const vs::bside::chip::Filter& getSharedFilter() const noexcept { return sharedFilter; }
    /** Structural switches completed since prepare. */
    int switchesDone() const noexcept { return switches; }
    /** Test aid: a pitch offset in semitones (double) added to every note's host pitch, so a test can place a note at an
        exact frequency (the alias gate's rational frequencies); 0 in the plugin. */
    void setTestDetune (double semitones) noexcept { testDetune = std::isfinite (semitones) ? semitones : 0.0; }

private:
    struct Structure
    {
        int model = 1;     // 0 Model A, 1 Model B (chip.model's order)
        bool shared = false;
        int clock = 0;
        bool authentic = false;
        bool volumeDc = false;
        bool operator== (const Structure&) const = default;
    };
    struct Smooth
    {
        double value = 0.0, step = 0.0, goal = 0.0;
        int left = 0;
        void jump (double v) noexcept
        {
            value = goal = v;
            left = 0;
            step = 0.0;
        }
        void setGoal (double v, int steps) noexcept;
        void advance() noexcept
        {
            if (left > 0)
            {
                value = --left == 0 ? goal : value + step;
            }
        }
    };
    enum Smoothed
    {
        sFine1, sFine2, sFine3, sPw1, sPw2, sPw3, sCutoff, sKeyFollow, sVariation, sPwOffset, sCutoffOffset, sSweep,
        sVelVolume, sVelCutoff, sVibratoRate, sWheelDepth, sUnisonDetune, sBend, sWheel, kNumSmoothed
    };
    static constexpr std::size_t kNumSmoothedValues = kNumSmoothed;
    struct SharedWrite
    {
        std::int64_t at = 0; // part sample (bus sample) from which it applies
        NoteVoiceSink::FilterWrite what = NoteVoiceSink::FilterWrite::cutoff;
        float cutoff = 0.0f;
        std::uint8_t value = 0;
    };
    static constexpr int kSharedQueue = 64;

    using Voices = std::array<NoteVoice, kVoices>;

    // ---- block and structure ----
    Structure structureOf (const PartParams& p, int clock) const noexcept;
    void applyBlockParams() noexcept;
    void configureVoices() noexcept; // every voice into `current` (reset, idle); off the per-sample path
    void beginSwitch() noexcept;
    void finishSwitch() noexcept;
    std::uint32_t tickPeriod() const noexcept;

    // ---- events and voices ----
    void handleEvent (const vs::NoteEvent& e) noexcept;
    void execute (const vs::VoiceAction& a) noexcept;
    /** The part-level values a control point produces (the smoothed parameters, the vibrato's phase) and whether a host
        input may have moved there; a voice reads the state in force at its position. */
    struct CtlState
    {
        std::array<double, kNumSmoothedValues> v {};
        double vibratoPhase = 0.0;
        bool moved = false, variationMoved = false;
    };
    CtlState liveState() const noexcept;
    void startNote (NoteVoice& v, int index, const vs::VoiceAction& a, bool stolen, const CtlState& c) noexcept;
    void startNote (NoteVoice& v, int index, const vs::VoiceAction& a, bool stolen) noexcept { startNote (v, index, a, stolen, liveState()); }
    /** A note that waited (a steal's fade, a structural switch) starts at part sample `at`: it remembers how late, and a
        release that came while it waited is scheduled as long after the start as it came after the note-on. */
    void startWaiting (NoteVoice& v, int index, bool stolen, const CtlState& c, std::int64_t at) noexcept;
    void noteOffVoice (NoteVoice& v) noexcept;
    void killVoice (NoteVoice& v) noexcept;
    void leaveSum (NoteVoice& v) noexcept;
    void catchUp (NoteVoice& v) noexcept;
    vs::bside::engine::HostInputs hostFor (const NoteVoice& v, const CtlState& c) const noexcept;
    vs::bside::engine::RestartPolicy policy() const noexcept;
    double noteGainFor (float velocity, const CtlState& c) const noexcept;

    // ---- rendering ----
    void renderInternal (float* dst, int count) noexcept;
    /** No voice sounds and the chain has been silent for kQuietAfterSeconds: time, control points and the idle voices'
        chip time move on, nothing is rendered (the chain after the sum is reset: it holds zeros). */
    void renderQuiet (int count) noexcept;
    void controlPoint() noexcept; // Shared and quiet: partControl, then voiceControl for every voice
    CtlState partControl() noexcept;
    /** A voice at a control point (glide, variation, host sync, the idle rule); true when it left the sum. */
    bool voiceControl (NoteVoice& v, const CtlState& c) noexcept;
    void renderSpanPerNote (float* dst, int count) noexcept;
    void renderVoiceSpan (NoteVoice& v, int index, int count, int nCtl, const CtlState& start) noexcept;
    void postProcess (float* dst, int len) noexcept;
    int segmentLimit (int wanted) const noexcept;
    void renderShared (int len) noexcept;
    void renderVoicePerNote (NoteVoice& v, double* buf, int len) noexcept;
    void mixVoice (NoteVoice& v, const double* buf, double* dst, int len) noexcept;
    void afterSegment (int len) noexcept;

    // ---- Shared ----
    static void sharedTap (void* context, int voice, NoteVoiceSink::FilterWrite what) noexcept;
    void sharedWrite (int voice, NoteVoiceSink::FilterWrite what) noexcept;
    void applySharedWrite (const SharedWrite& w, std::int64_t at) noexcept;
    void setMostRecent (int voice) noexcept;
    void sharedFollowVoice (int voice) noexcept;
    int mostRecentActive() const noexcept;

    // configuration
    double hostRate = 48000.0, internalRate = 96000.0;
    int osFactor = 2, maxBlock = 0, controlInterval = 96, fadeSamples = 288, smoothSteps = 5, rampSamples = 480;
    int latencySamples = 0;
    double exactLatency = 0.0;
    std::array<int, 2> chainLatency {};
    int maxChainLatency = 0;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::AudioBuffer<float> silence;

    // prototypes per model (built in prepare): a structural switch copies them
    std::unique_ptr<std::array<vs::bside::chip::ChipVoice, 2>> protoChain;
    std::array<vs::bside::chip::Filter, 2> protoFilter;
    vs::bside::chip::Filter sharedFilter;
    vs::bside::chip::Filter settleCopy; // Shared: the shared filter's static output for a chip that starts settled

    std::unique_ptr<Voices> voices;
    vs::VoiceAllocator allocator;

    // parameters
    PartParams target {}, applied {};
    int clockTarget = 0;
    double tempo = 120.0;
    const vs::bside::program::Program* program = nullptr;
    Structure current {}, wanted {};
    bool switchPending = false;
    int switchLeft = 0;
    int switches = 0;
    std::array<Smooth, kNumSmoothed> sm {};
    float bendValue = 0.0f, wheelValue = 0.0f;
    double vibratoPhase = 0.0;    // cycles, 0 .. 1
    double glideFactor = 0.0;     // per control interval (0: no glide)
    double testDetune = 0.0;      // setTestDetune
    bool hostDirty = true;        // a host input may have changed since the last control point's syncs
    std::int64_t silentRun = 0;   // internal samples since the sum was last non-zero (no voice active)
    bool quietChain = false;      // the quiet path is in use: the chain after the sum was reset
    std::uint32_t period = 19656;

    // time
    std::int64_t partSample = 0;  // internal samples since prepare, reset or resume
    std::int64_t controlDoneAt = -1;
    std::uint64_t startCounter = 0;
    vs::bside::chip::ChipClock refClock;
    std::uint64_t refCycle = 0;

    // the sum, the pad, the conversion
    std::array<double, kSpan> sum {};
    std::array<double, kSpan> scratch {};
    static constexpr int kMaxControls = 20;               // control points per span at most (the span is cut to fit)
    std::array<int, kMaxControls> ctlPos {};
    std::array<CtlState, kMaxControls> ctlStates {};
    std::array<int, kVoices> leftAt {};
    std::array<std::array<double, kSegment>, kVoices> voiceOut {}; // Shared: each voice's output of a segment
    // the pad delays the shorter chain by the DIFFERENCE of the two chains (never by a chain's whole length): at most
    // 2 x HalfBandDesign::kMaxPairs - 1 = 63 internal samples (Model A's 2x part, reached at host rates of 8 and
    // 11.025 kHz); the ring holds twice that, at every host rate the shell accepts (8.21.1 after the review)
    static constexpr int kPadRing = 128;
    static_assert (kPadRing >= 4 * vs::bside::chip::HalfBandDesign::kMaxPairs && (kPadRing & (kPadRing - 1)) == 0);
    std::array<double, kPadRing> pad {};
    int padPos = 0, padDelay = 0;
    vs::bside::chip::render::OutputRounding rounding {};

    // Shared
    std::array<SharedWrite, kSharedQueue> sharedQueue {};
    int sharedHead = 0, sharedCount = 0;
    int mostRecent = -1;
    std::int64_t tapPartSample = 0;
    float sharedCutoff = 0.0f;
    std::int64_t sharedLastChange = 0;
    bool sharedChanged = false, sharedMoving = false, sharedSettle = true;
    std::array<std::uint64_t, kSegment + 1> cycAt {};
    std::array<double, kSegment> shLinear {}, shLowPass {}, shStatic {};
    std::array<vs::bside::chip::Filter::Shape, kSegment> shShape {};

    // voices that left the sum in the segment, reported to the allocator at its end (sample, then index)
    std::array<int, kVoices> leftVoices {};
    int numLeft = 0;
};
} // namespace bs
