#include "params/ParameterLayout.h"

#include "params/Mappings.h"

namespace vs
{
namespace
{
enum class Display
{
    Panel10,
    CutoffHz,
    HpfHz,
    LfoRate,
    LfoDelay,
    EnvTime,
    Cents,
    PortaTime,
    DetuneCents,
    Percent,
    Pan,     // -1..1: "L 50", "C", "R 50"
    Balance, // -1..1: "Lower 50", "Centre", "Upper 50"
    // Wave 3
    DelayTime,    // 0..1 -> 1 ms .. 2 s
    Feedback,     // 0..1 -> 0 .. 95 %
    ReverbDecay,  // 0..1 -> 0.3 .. 20 s
    PreDelay,     // 0..1 -> 0 .. 200 ms
    PercentValue, // the value is a percentage already (swing, gate)
    Bpm           // the value is in bpm
};

/** "350 ms" / "1.2 s" -> seconds; a bare number counts as seconds (as the envelope times). */
float parseSeconds (const juce::String& t)
{
    return t.containsIgnoreCase ("ms") ? t.getFloatValue() / 1000.0f : t.getFloatValue();
}

juce::String formatHz (float hz)
{
    return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " kHz" : juce::String (hz, hz < 10.0f ? 2 : 1) + " Hz";
}

juce::String formatSeconds (float s)
{
    return s >= 1.0f ? juce::String (s, 2) + " s" : juce::String (s * 1000.0f, s < 0.01f ? 1 : 0) + " ms";
}

/** -1..1 -> "<neg> 50" / centre / "<pos> 50" (percent of the way to the side). */
juce::String formatSide (float v, const char* negative, const char* centre, const char* positive)
{
    const int pct = juce::roundToInt (std::abs (v) * 100.0f);
    if (pct == 0)
        return centre;
    return juce::String (v < 0.0f ? negative : positive) + " " + juce::String (pct);
}

/** Inverse of formatSide: a leading negative/positive word (or its first letter) gives the side. */
float parseSide (const juce::String& text, const char* negative, const char* positive)
{
    const auto t = text.trim();
    const float amount = juce::jlimit (0.0f, 100.0f, std::abs (t.retainCharacters ("0123456789.").getFloatValue())) / 100.0f;
    const auto startsWithWord = [&t] (const char* word) {
        return t.startsWithIgnoreCase (word) || (t.isNotEmpty() && juce::CharacterFunctions::toLowerCase (t[0]) == juce::CharacterFunctions::toLowerCase (static_cast<juce::juce_wchar> (word[0])));
    };
    if (startsWithWord (negative))
        return -amount;
    if (startsWithWord (positive))
        return amount;
    return juce::jlimit (-1.0f, 1.0f, t.getFloatValue() / 100.0f); // plain signed percent; "C"/"Centre" = 0
}

/** Parse "1.2 kHz" / "350 Hz" -> Hz, and "350 ms" / "1.2 s" -> seconds. */
float parseScaled (const juce::String& text, const char* kiloUnit, float kiloFactor)
{
    const auto value = text.retainCharacters ("0123456789.-").getFloatValue();
    return text.containsIgnoreCase (kiloUnit) ? value * kiloFactor : value;
}

juce::String toText (Display d, float v)
{
    switch (d)
    {
        case Display::Panel10:  return juce::String (v * 10.0f, 1);
        case Display::CutoffHz: return formatHz (map::cutoffHz (v));
        case Display::HpfHz:    return formatHz (map::hpfHz (v));
        case Display::LfoRate:  return formatHz (map::lfoRateHz (v));
        case Display::LfoDelay: return formatSeconds (map::lfoDelaySec (v));
        case Display::EnvTime:  return formatSeconds (map::envTimeSec (v));
        case Display::Cents:    return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (map::fineTuneSemis (v) * 100.0f)) + " ct";
        case Display::PortaTime: return v <= 0.0f ? juce::String ("Off") : formatSeconds (map::portaTimeSec (v));
        case Display::DetuneCents: return juce::String (map::unisonDetuneCents (v), 1) + " ct";
        case Display::Percent:  return juce::String (juce::roundToInt (v * 100.0f)) + " %";
        case Display::Pan:      return formatSide (v, "L", "C", "R");
        case Display::Balance:  return formatSide (v, "Lower", "Centre", "Upper");
        case Display::DelayTime: return formatSeconds (map::delayTimeSec (v));
        case Display::Feedback: return juce::String (map::delayFeedback (v) * 100.0f, 1) + " %";
        case Display::ReverbDecay: return formatSeconds (map::reverbDecaySec (v));
        case Display::PreDelay: return juce::String (map::reverbPreDelaySec (v) * 1000.0f, 1) + " ms";
        case Display::PercentValue: return juce::String (v, 1) + " %";
        case Display::Bpm:      return juce::String (v, 1) + " bpm";
    }
    return {};
}

float fromText (Display d, const juce::String& t)
{
    switch (d)
    {
        case Display::Panel10:  return t.getFloatValue() / 10.0f;
        case Display::CutoffHz: return map::cutoffFromHz (juce::jmax (1.0f, parseScaled (t, "k", 1000.0f)));
        case Display::HpfHz:    return map::hpfFromHz (juce::jmax (1.0f, parseScaled (t, "k", 1000.0f)));
        case Display::LfoRate:  return map::lfoRateFromHz (juce::jmax (0.001f, parseScaled (t, "k", 1000.0f)));
        case Display::LfoDelay: return map::lfoDelayFromSec (juce::jmax (0.0f, t.containsIgnoreCase ("ms") ? t.getFloatValue() / 1000.0f : t.getFloatValue()));
        case Display::EnvTime:  return map::envTimeFromSec (juce::jmax (0.0001f, t.containsIgnoreCase ("ms") ? t.getFloatValue() / 1000.0f : t.getFloatValue()));
        case Display::Cents:    return t.getFloatValue() / 100.0f;
        case Display::PortaTime:
            if (t.trim().equalsIgnoreCase ("off"))
                return 0.0f;
            return map::portaFromSec (t.containsIgnoreCase ("ms") ? t.getFloatValue() / 1000.0f : t.getFloatValue());
        case Display::DetuneCents: return t.getFloatValue() / map::unisonDetuneCents (1.0f);
        case Display::Percent:  return t.getFloatValue() / 100.0f;
        case Display::Pan:      return parseSide (t, "L", "R");
        case Display::Balance:  return parseSide (t, "Lower", "Upper");
        case Display::DelayTime: return map::delayTimeFromSec (parseSeconds (t));
        case Display::Feedback: return map::delayFeedbackFromGain (t.getFloatValue() / 100.0f);
        case Display::ReverbDecay: return map::reverbDecayFromSec (parseSeconds (t));
        case Display::PreDelay:
            // "20 ms" or a bare number: milliseconds; "0.02 s": seconds.
            return map::reverbPreDelayFromSec (t.containsIgnoreCase ("ms") || ! t.containsIgnoreCase ("s")
                                                   ? t.getFloatValue() / 1000.0f
                                                   : t.getFloatValue());
        case Display::PercentValue: return t.getFloatValue();
        case Display::Bpm:      return t.getFloatValue();
    }
    return 0.0f;
}

std::unique_ptr<juce::AudioParameterFloat> makeFloat (const juce::String& id, const juce::String& name,
                                                      float min, float max, float def, Display d)
{
    auto attrs = juce::AudioParameterFloatAttributes()
                     .withStringFromValueFunction ([d] (float v, int) { return toText (d, v); })
                     .withValueFromStringFunction ([d, min, max] (const juce::String& t) {
                         return juce::jlimit (min, max, fromText (d, t));
                     });

    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, kParamVersionHint }, name,
                                                        juce::NormalisableRange<float> { min, max }, def,
                                                        std::move (attrs));
}

template <size_t N>
std::unique_ptr<juce::AudioParameterChoice> makeChoice (const juce::String& id, const juce::String& name,
                                                        const std::array<const char*, N>& labels, int def)
{
    juce::StringArray choices;
    for (const auto* l : labels)
        choices.add (l);

    return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, kParamVersionHint }, name,
                                                         choices, def);
}

/** An integer parameter that hosts treat as stepped. juce::AudioParameterInt reports itself as
    continuous, so a VST3 host would offer (and store) values between the integers. */
class SteppedIntParameter final : public juce::AudioParameterInt
{
public:
    using juce::AudioParameterInt::AudioParameterInt;
    bool isDiscrete() const override { return true; }
};

void addPart (juce::AudioProcessorValueTreeState::ParameterLayout& layout, const juce::String& prefix,
              const juce::String& namePrefix)
{
    // Order is part of the host-facing contract: never reorder, only append.
#define VS_ADD_FLOAT(member, id, name, mn, mx, def, kind) \
    layout.add (makeFloat (prefix + id, namePrefix + juce::String::fromUTF8 (name), mn, mx, def, Display::kind));
#define VS_ADD_CHOICE(member, id, name, labels, def) \
    layout.add (makeChoice (prefix + id, namePrefix + juce::String::fromUTF8 (name), labels, def));
    VS_PART_FLOAT_PARAMS (VS_ADD_FLOAT)
    VS_PART_CHOICE_PARAMS (VS_ADD_CHOICE)
#undef VS_ADD_FLOAT
#undef VS_ADD_CHOICE
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamID::masterVolume, kParamVersionHint },
        "Master Volume",
        juce::NormalisableRange<float> { -60.0f, 6.0f, 0.1f },
        -6.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    addPart (layout, kLowerPrefix, "Lower ");
    addPart (layout, kUpperPrefix, "Upper ");

    layout.add (makeFloat (ParamID::vintage, "Vintage", 0.0f, 1.0f, 0.0f, Display::Panel10));

    // M3: key modes and the part mixer (appended, docs/architecture.md 4.5).
    layout.add (makeChoice (ParamID::keyMode, "Key Mode", kKeyModeLabels, 0));
    // The text parser needs the parameter's own value (unreadable text leaves it unchanged); the
    // parameter does not exist yet when the lambda is made, so it reads it through a shared slot.
    auto splitPointHolder = std::make_shared<juce::AudioParameterInt*> (nullptr);
    auto split = std::make_unique<SteppedIntParameter> ( // stepped for hosts (same ID and range as M3)
        juce::ParameterID { ParamID::keySplitPoint, kParamVersionHint }, "Split Point", 0, 127, kDefaultSplitPoint,
        juce::AudioParameterIntAttributes()
            .withStringFromValueFunction ([] (int v, int) { return noteName (v); })
            .withValueFromStringFunction ([current = splitPointHolder] (const juce::String& t) {
                const int n = noteFromName (t);
                return n >= 0 ? n : (*current != nullptr ? (*current)->get() : kDefaultSplitPoint);
            }));
    *splitPointHolder = split.get(); // owned by the processor's parameter tree for its whole life
    layout.add (std::move (split));
    layout.add (makeFloat (ParamID::mixBalance, "Balance", -1.0f, 1.0f, 0.0f, Display::Balance));

    // Wave 3: effects (appended, docs/architecture.md 6a.2). Continuous ones are smoothed by the
    // effect blocks themselves.
    layout.add (makeChoice (ParamID::fxChorusMode, "Chorus", kChorusModeLabels, 0));
    layout.add (makeFloat (ParamID::fxChorusMix, "Chorus Mix", 0.0f, 1.0f, 0.5f, Display::Percent));
    layout.add (makeFloat (ParamID::fxDelayTime, "Delay Time", 0.0f, 1.0f, map::delayTimeFromSec (0.7f), Display::DelayTime));
    layout.add (makeFloat (ParamID::fxDelayFeedback, "Delay Feedback", 0.0f, 1.0f, map::delayFeedbackFromGain (0.35f), Display::Feedback));
    layout.add (makeFloat (ParamID::fxDelayDamping, "Delay Damping", 0.0f, 1.0f, 0.3f, Display::Percent));
    layout.add (makeChoice (ParamID::fxDelayPingPong, "Delay Ping-Pong", kOffOnLabels, 1));
    layout.add (makeFloat (ParamID::fxDelayLevel, "Delay Level", 0.0f, 1.0f, 0.0f, Display::Percent));
    layout.add (makeFloat (ParamID::fxReverbDecay, "Reverb Decay", 0.0f, 1.0f, map::reverbDecayFromSec (2.5f), Display::ReverbDecay));
    layout.add (makeFloat (ParamID::fxReverbDamping, "Reverb Damping", 0.0f, 1.0f, 0.5f, Display::Percent));
    layout.add (makeFloat (ParamID::fxReverbPreDelay, "Reverb Pre-Delay", 0.0f, 1.0f, map::reverbPreDelayFromSec (0.02f), Display::PreDelay));
    layout.add (makeFloat (ParamID::fxReverbLevel, "Reverb Level", 0.0f, 1.0f, 0.0f, Display::Percent));

    // Wave 3: arpeggiator / sequencer (one settings set for both instances).
    layout.add (makeChoice (ParamID::arpMode, "Arp/Seq Mode", kArpModeLabels, 0));
    layout.add (makeChoice (ParamID::arpTarget, "Arp/Seq Part", kArpTargetLabels, static_cast<int> (ArpTarget::Both)));
    layout.add (makeChoice (ParamID::arpRate, "Arp/Seq Rate", kArpRateLabels, static_cast<int> (seq::Rate::Sixteenth)));
    layout.add (makeFloat (ParamID::arpSwing, "Arp/Seq Swing", 50.0f, 75.0f, 50.0f, Display::PercentValue));
    layout.add (makeFloat (ParamID::arpGate, "Arp/Seq Gate", 5.0f, 100.0f, 50.0f, Display::PercentValue));
    layout.add (makeChoice (ParamID::arpHold, "Arp/Seq Hold", kOffOnLabels, 0));
    layout.add (makeFloat (ParamID::arpTempo, "Internal Tempo", 20.0f, 300.0f, 120.0f, Display::Bpm));
    layout.add (makeChoice (ParamID::arpPattern, "Arp Pattern", kArpPatternLabels, 0));
    layout.add (std::make_unique<SteppedIntParameter> (juce::ParameterID { ParamID::arpOctaves, kParamVersionHint },
                                                           "Arp Octaves", 1, seq::kMaxOctaves, 1));
    layout.add (std::make_unique<SteppedIntParameter> (juce::ParameterID { ParamID::seqLength, kParamVersionHint },
                                                           "Seq Length", 1, seq::kMaxSeqSteps, seq::kMaxSeqSteps));

    // The 16 steps, each appended as note, type, velocity (step 01 first). One definition for all.
    const seq::SeqStep defaultStep {};
    for (int step = 1; step <= seq::kMaxSeqSteps; ++step)
    {
        layout.add (std::make_unique<SteppedIntParameter> (
            juce::ParameterID { seqStepParamID (step, SeqStepField::Note), kParamVersionHint },
            seqStepParamName (step, SeqStepField::Note), -24, 24, defaultStep.semitones,
            juce::AudioParameterIntAttributes()
                .withStringFromValueFunction ([] (int v, int) { return stepSemitonesText (v); })
                .withValueFromStringFunction ([] (const juce::String& t) { return stepSemitonesFromText (t); })));
        layout.add (makeChoice (seqStepParamID (step, SeqStepField::Type), seqStepParamName (step, SeqStepField::Type),
                                kSeqStepTypeLabels, static_cast<int> (defaultStep.type)));
        layout.add (makeFloat (seqStepParamID (step, SeqStepField::Velocity), seqStepParamName (step, SeqStepField::Velocity),
                               0.0f, 1.0f, defaultStep.velocity, Display::Percent));
    }

    return layout;
}

juce::String stepSemitonesText (int semitones)
{
    const int v = map::clampStepSemitones (semitones);
    return (v > 0 ? "+" : "") + juce::String (v) + " st";
}

int stepSemitonesFromText (const juce::String& text)
{
    return map::clampStepSemitones (text.trim().removeCharacters ("+").getIntValue());
}

juce::String seqStepParamID (int step, SeqStepField field)
{
    static constexpr const char* suffix[] { "note", "type", "velocity" };
    return "seq.step" + juce::String (step).paddedLeft ('0', 2) + "." + suffix[static_cast<int> (field)];
}

juce::String seqStepParamName (int step, SeqStepField field)
{
    static constexpr const char* suffix[] { "Note", "Type", "Velocity" };
    return "Seq Step " + juce::String (step).paddedLeft ('0', 2) + " " + suffix[static_cast<int> (field)];
}

// The choice labels must stay in the order of the module enums they are cast to.
static_assert (static_cast<int> (fx::ChorusMode::II) == 2 && kChorusModeLabels.size() == 3);
static_assert (static_cast<int> (seq::Mode::Seq) == 2 && kArpModeLabels.size() == 3);
static_assert (static_cast<int> (seq::Rate::ThirtySecond) == 5 && kArpRateLabels.size() == 6);
static_assert (static_cast<int> (seq::ArpPattern::AsPlayed) == 4 && kArpPatternLabels.size() == 5);
static_assert (static_cast<int> (seq::StepType::Tie) == 2 && kSeqStepTypeLabels.size() == 3);
static_assert (static_cast<int> (ArpTarget::Both) == 2 && kArpTargetLabels.size() == 3);

void BoundParam::bind (juce::AudioProcessorValueTreeState& state, const juce::String& id)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter (id));
    value = state.getRawParameterValue (id);
    jassert (p != nullptr && value != nullptr);
    if (p != nullptr)
    {
        lo = p->getNormalisableRange().start;
        hi = p->getNormalisableRange().end;
        def = p->convertFrom0to1 (p->getDefaultValue());
    }
}

void FxParamBinding::bind (juce::AudioProcessorValueTreeState& state)
{
    chorusMode.bind (state, ParamID::fxChorusMode);
    chorusMix.bind (state, ParamID::fxChorusMix);
    delayTime.bind (state, ParamID::fxDelayTime);
    delayFeedback.bind (state, ParamID::fxDelayFeedback);
    delayDamping.bind (state, ParamID::fxDelayDamping);
    delayPingPong.bind (state, ParamID::fxDelayPingPong);
    delayLevel.bind (state, ParamID::fxDelayLevel);
    reverbDecay.bind (state, ParamID::fxReverbDecay);
    reverbDamping.bind (state, ParamID::fxReverbDamping);
    reverbPreDelay.bind (state, ParamID::fxReverbPreDelay);
    reverbLevel.bind (state, ParamID::fxReverbLevel);
}

void FxParamBinding::read (FxSettings& out) const noexcept
{
    out.chorus.mode = static_cast<fx::ChorusMode> (chorusMode.index());
    out.chorus.mix = chorusMix.get();
    out.delay.timeSeconds = map::delayTimeSec (delayTime.get());
    out.delay.feedback = map::delayFeedback (delayFeedback.get());
    out.delay.damping = delayDamping.get();
    out.delay.pingPong = delayPingPong.index() == 1;
    out.delay.level = delayLevel.get();
    out.reverb.decaySeconds = map::reverbDecaySec (reverbDecay.get());
    out.reverb.damping = reverbDamping.get();
    out.reverb.preDelaySeconds = map::reverbPreDelaySec (reverbPreDelay.get());
    out.reverb.level = reverbLevel.get();
}

void ArpParamBinding::bind (juce::AudioProcessorValueTreeState& state)
{
    mode.bind (state, ParamID::arpMode);
    target.bind (state, ParamID::arpTarget);
    rate.bind (state, ParamID::arpRate);
    swing.bind (state, ParamID::arpSwing);
    gate.bind (state, ParamID::arpGate);
    hold.bind (state, ParamID::arpHold);
    tempo.bind (state, ParamID::arpTempo);
    pattern.bind (state, ParamID::arpPattern);
    octaves.bind (state, ParamID::arpOctaves);
    length.bind (state, ParamID::seqLength);
    for (int s = 0; s < seq::kMaxSeqSteps; ++s)
        for (int f = 0; f < kNumSeqStepFields; ++f)
            steps[static_cast<size_t> (s)][static_cast<size_t> (f)].bind (state, seqStepParamID (s + 1, static_cast<SeqStepField> (f)));
}

void ArpParamBinding::read (ArpSettings& out) const noexcept
{
    auto& s = out.settings;
    s.mode = static_cast<seq::Mode> (mode.index());
    out.target = static_cast<ArpTarget> (target.index());
    s.rate = static_cast<seq::Rate> (rate.index());
    s.swing = map::percentToFraction (swing.get());
    s.gate = map::percentToFraction (gate.get());
    s.hold = hold.index() == 1;
    s.internalBpm = static_cast<double> (tempo.get());
    s.arpPattern = static_cast<seq::ArpPattern> (pattern.index());
    s.arpOctaves = octaves.index();
    s.seed = kArpSeed;
    s.seqLength = length.index();
    for (size_t i = 0; i < steps.size(); ++i)
    {
        auto& st = s.seqSteps[i];
        st.semitones = map::clampStepSemitones (steps[i][0].index());
        st.type = static_cast<seq::StepType> (steps[i][1].index());
        st.velocity = steps[i][2].get();
    }
}

juce::String noteName (int note)
{
    static constexpr const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int n = juce::jlimit (0, 127, note);
    return juce::String (names[n % 12]) + juce::String (n / 12 - 1);
}

int noteFromName (const juce::String& text)
{
    const auto t = text.trim();
    if (t.isEmpty())
        return -1;
    const auto isInteger = [] (const juce::String& x) {
        // An optional minus sign, then at least one digit, nothing else.
        const int start = x.startsWithChar ('-') ? 1 : 0;
        return x.length() > start && x.substring (start).containsOnly ("0123456789");
    };
    int note = -1;
    if (isInteger (t))
    {
        note = t.getIntValue(); // a plain MIDI note number
    }
    else
    {
        static constexpr int semis[] { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
        const auto letter = static_cast<int> (juce::CharacterFunctions::toUpperCase (t[0]));
        if (letter < 'A' || letter > 'G')
            return -1;
        int pc = semis[static_cast<size_t> (letter - 'A')];
        int i = 1;
        for (; i < t.length() && (static_cast<int> (t[i]) == '#' || static_cast<int> (t[i]) == 'b'); ++i)
            pc += static_cast<int> (t[i]) == '#' ? 1 : -1;
        const auto octaveText = t.substring (i).trim();
        if (! isInteger (octaveText)) // "C-" or "C#x": not a note
            return -1;
        note = (octaveText.getIntValue() + 1) * 12 + pc;
    }
    return note >= 0 && note <= 127 ? note : -1; // out of range ("Cb-1", "G#9", "200"): rejected
}

void PartParamBinding::bind (juce::AudioProcessorValueTreeState& state, const juce::String& prefix)
{
#define VS_BIND(member, id, ...)                                  \
    member = state.getRawParameterValue (prefix + id);            \
    jassert (member != nullptr);
    VS_PART_FLOAT_PARAMS (VS_BIND)
    VS_PART_CHOICE_PARAMS (VS_BIND)
#undef VS_BIND
}

void PartParamBinding::read (PartParams& out) const noexcept
{
    // Sanitised (Mappings.h): a non-finite host value becomes the default, anything else is clamped
    // to the range; in-range values pass bit-exactly.
#define VS_READ_FLOAT(member, id, name, mn, mx, def, kind) \
    out.member = map::sanitiseParam (member->load (std::memory_order_relaxed), mn, mx, def);
#define VS_READ_CHOICE(member, id, name, labels, def) \
    out.member = map::sanitiseIndex (member->load (std::memory_order_relaxed), 0, static_cast<int> (labels.size()) - 1, def);
    VS_PART_FLOAT_PARAMS (VS_READ_FLOAT)
    VS_PART_CHOICE_PARAMS (VS_READ_CHOICE)
#undef VS_READ_FLOAT
#undef VS_READ_CHOICE
}
} // namespace vs
