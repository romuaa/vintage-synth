#include "common/params/FxArpParams.h"

namespace vs
{
void addFxParameters (juce::AudioProcessorValueTreeState::ParameterLayout& layout)
{
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
}

void addArpSeqParameters (juce::AudioProcessorValueTreeState::ParameterLayout& layout)
{
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
} // namespace vs
