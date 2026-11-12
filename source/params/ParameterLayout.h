#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "fx/Chorus.h"
#include "fx/Delay.h"
#include "fx/Reverb.h"
#include "params/Mappings.h"
#include "params/PartParamList.h"
#include "seq/Settings.h"

namespace vs
{
namespace ParamID
{
// Global (non-part) parameters. Dotted, stable IDs. Frozen after v1.0 — append only.
inline constexpr const char* masterVolume = "master.volume";
inline constexpr const char* vintage = "global.vintage";
// M3 (appended after vintage, in this order).
inline constexpr const char* keyMode = "key.mode";
inline constexpr const char* keySplitPoint = "key.split_point";
inline constexpr const char* mixBalance = "mix.balance";
// Wave 3: effects (appended after mix.balance, in this order; docs/architecture.md 6a.2).
inline constexpr const char* fxChorusMode = "fx.chorus.mode";
inline constexpr const char* fxChorusMix = "fx.chorus.mix";
inline constexpr const char* fxDelayTime = "fx.delay.time";
inline constexpr const char* fxDelayFeedback = "fx.delay.feedback";
inline constexpr const char* fxDelayDamping = "fx.delay.damping";
inline constexpr const char* fxDelayPingPong = "fx.delay.ping_pong";
inline constexpr const char* fxDelayLevel = "fx.delay.level";
inline constexpr const char* fxReverbDecay = "fx.reverb.decay";
inline constexpr const char* fxReverbDamping = "fx.reverb.damping";
inline constexpr const char* fxReverbPreDelay = "fx.reverb.predelay";
inline constexpr const char* fxReverbLevel = "fx.reverb.level";
// Wave 3: arpeggiator / sequencer (appended after the effects, in this order), then the 16 x 3
// step parameters from seqStepParamID().
inline constexpr const char* arpMode = "arp.mode";
inline constexpr const char* arpTarget = "arp.target";
inline constexpr const char* arpRate = "arp.rate";
inline constexpr const char* arpSwing = "arp.swing";
inline constexpr const char* arpGate = "arp.gate";
inline constexpr const char* arpHold = "arp.hold";
inline constexpr const char* arpTempo = "arp.tempo";
inline constexpr const char* arpPattern = "arp.pattern";
inline constexpr const char* arpOctaves = "arp.octaves";
inline constexpr const char* seqLength = "seq.length";
} // namespace ParamID

/** "Key Mode" choices; order == vs::KeyMode. */
inline constexpr std::array<const char*, 3> kKeyModeLabels { "Whole", "Dual", "Split" };
inline constexpr int kDefaultSplitPoint = 60; // C4

// ---- Wave 3 choice labels. Order == the module enums (static_asserted in ParameterLayout.cpp). ----
inline constexpr std::array<const char*, 3> kChorusModeLabels { "Off", "I", "II" };              // fx::ChorusMode
inline constexpr std::array<const char*, 3> kArpModeLabels { "Off", "Arpeggiator", "Sequencer" }; // seq::Mode
inline constexpr std::array<const char*, 3> kArpTargetLabels { "Lower", "Upper", "Both" };        // ArpTarget
inline constexpr std::array<const char*, 6> kArpRateLabels { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" }; // seq::Rate
inline constexpr std::array<const char*, 5> kArpPatternLabels { "Up", "Down", "Up & Down", "Random", "As Played" }; // seq::ArpPattern
inline constexpr std::array<const char*, 3> kSeqStepTypeLabels { "Note", "Rest", "Tie" };         // seq::StepType

/** Which part(s) the arpeggiator/sequencer sits in front of. Order == "Arp/Seq Part" choices. */
enum class ArpTarget : std::uint8_t
{
    Lower,
    Upper,
    Both
};

/** The step parameters: field order inside a step == the order they are appended in. */
enum class SeqStepField : std::uint8_t
{
    Note,
    Type,
    Velocity
};
inline constexpr int kNumSeqStepFields = 3;

/** "seq.step01.note" ... "seq.step16.velocity" (step 1..16). */
juce::String seqStepParamID (int step, SeqStepField field);
/** "Seq Step 01 Note" ... */
juce::String seqStepParamName (int step, SeqStepField field);
/** Step note display: "+3 st", "0 st", "-12 st"; and back (clamped to -24..+24). */
juce::String stepSemitonesText (int semitones);
int stepSemitonesFromText (const juce::String& text);

/** Fixed seed of the Random arp pattern (renders are reproducible; docs/architecture.md 6a.2). */
inline constexpr std::uint32_t kArpSeed = 1u;

/** Every effect setting in the block's units. */
struct FxSettings
{
    fx::ChorusParams chorus;
    fx::DelayParams delay;
    fx::ReverbParams reverb;
};

/** The arpeggiator/sequencer settings plus the part selector. */
struct ArpSettings
{
    seq::Settings settings;
    ArpTarget target = ArpTarget::Both;
};

/** MIDI note -> name, middle C (60) = "C4". */
juce::String noteName (int note);
/** "C4", "c#4", "Db-1", "60" -> MIDI note (clamped to 0..127); -1 if unreadable. */
int noteFromName (const juce::String& text);

// Version hint for every parameter. JUCE/VST3 uses it to keep parameter order stable.
inline constexpr int kParamVersionHint = 1;

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** Cached raw-value pointers for one part. Bound once, read once per block (lock-free). */
class PartParamBinding
{
public:
    void bind (juce::AudioProcessorValueTreeState& state, const juce::String& prefix);

    /** Audio-thread safe: atomic loads only. */
    void read (PartParams& out) const noexcept;

private:
#define VS_DECLARE_PTR(member, ...) std::atomic<float>* member = nullptr;
    VS_PART_FLOAT_PARAMS (VS_DECLARE_PTR)
    VS_PART_CHOICE_PARAMS (VS_DECLARE_PTR)
#undef VS_DECLARE_PTR
};

/**
    One parameter's raw-value atomic plus its range and default, captured at bind time. Every read
    is sanitised (Mappings.h): a non-finite value a host may have written becomes the default and
    anything else is clamped to the range, so a bad host value never reaches the engine.
*/
struct BoundParam
{
    std::atomic<float>* value = nullptr;
    float lo = 0.0f, hi = 1.0f, def = 0.0f;

    void bind (juce::AudioProcessorValueTreeState& state, const juce::String& id);
    /** Plain value in the parameter's range (lock-free, any thread). */
    float get() const noexcept { return map::sanitiseParam (value->load (std::memory_order_relaxed), lo, hi, def); }
    /** Choice index or integer value. */
    int index() const noexcept
    {
        return map::sanitiseIndex (value->load (std::memory_order_relaxed), static_cast<int> (lo), static_cast<int> (hi),
                                   static_cast<int> (def));
    }
};

/** Cached raw-value pointers of the 11 effect parameters. Read from any thread (atomics only). */
class FxParamBinding
{
public:
    void bind (juce::AudioProcessorValueTreeState& state);
    /** Atomic loads plus the Mappings.h conversions: lock-free and allocation-free. */
    void read (FxSettings& out) const noexcept;

private:
    BoundParam chorusMode;
    BoundParam chorusMix;
    BoundParam delayTime;
    BoundParam delayFeedback;
    BoundParam delayDamping;
    BoundParam delayPingPong;
    BoundParam delayLevel;
    BoundParam reverbDecay;
    BoundParam reverbDamping;
    BoundParam reverbPreDelay;
    BoundParam reverbLevel;
};

/** Cached raw-value pointers of the 58 arpeggiator/sequencer parameters. */
class ArpParamBinding
{
public:
    void bind (juce::AudioProcessorValueTreeState& state);
    /** Atomic loads only; the seed is kArpSeed. Audio-thread safe. */
    void read (ArpSettings& out) const noexcept;

private:
    BoundParam mode;
    BoundParam target;
    BoundParam rate;
    BoundParam swing;
    BoundParam gate;
    BoundParam hold;
    BoundParam tempo;
    BoundParam pattern;
    BoundParam octaves;
    BoundParam length;
    std::array<std::array<BoundParam, kNumSeqStepFields>, seq::kMaxSeqSteps> steps {};
};
} // namespace vs
