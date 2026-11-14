#include "vintage/ui/EffectsPage.h"

namespace vs::ui
{
namespace
{
using namespace metrics;

constexpr int kPictureHeight = 250, kPanelWidth = 400, kInner = kPanelWidth - 2 * kPad;
} // namespace

EffectsPage::EffectsPage (UiContext& c, shell::EditorState& state)
    : ctx (c),
      chorusPanel (c.theme, "Chorus", -1),
      delayPanel (c.theme, "Delay", -1),
      reverbPanel (c.theme, "Reverb", -1),
      chorusView (c.theme),
      delayView (c.theme),
      reverbView (c.theme),
      chorusMode (c, "fx.chorus.mode", "Mode", -1),
      pingPong (c, "fx.delay.ping_pong", "Ping-pong (bounce left / right)", -1),
      chorusMix (c, "fx.chorus.mix", "Mix", -1),
      delayTime (c, "fx.delay.time", "Time", -1),
      delayFeedback (c, "fx.delay.feedback", "Repeats (feedback)", -1),
      delayDamping (c, "fx.delay.damping", "Darkening (damping)", -1),
      delayLevel (c, "fx.delay.level", "Level (0 = off)", -1),
      reverbPreDelay (c, "fx.reverb.predelay", "Pre-delay", -1),
      reverbDecay (c, "fx.reverb.decay", "Decay time", -1),
      reverbDamping (c, "fx.reverb.damping", "Darkening (damping)", -1),
      reverbLevel (c, "fx.reverb.level", "Level (0 = off)", -1),
      delayPower (c, "fx.delay.level", "Delay", state.fxLastLevel[0]),
      reverbPower (c, "fx.reverb.level", "Reverb", state.fxLastLevel[1])
{
    setTitle ("Effects");
    setComponentID ("effects");
    delayHeaderText = "Delay";
    reverbHeaderText = "Reverb";

    for (auto* panel : { &chorusPanel, &delayPanel, &reverbPanel })
        addAndMakeVisible (*panel);
    chorusPanel.addAndMakeVisible (chorusView);
    chorusPanel.addAndMakeVisible (chorusMode);
    chorusPanel.addAndMakeVisible (chorusMix);
    delayPanel.addAndMakeVisible (delayView);
    for (auto* b : std::initializer_list<juce::Component*> { &pingPong, &delayTime, &delayFeedback, &delayDamping, &delayLevel, &delayPower })
        delayPanel.addAndMakeVisible (*b);
    reverbPanel.addAndMakeVisible (reverbView);
    for (auto* b : std::initializer_list<juce::Component*> { &reverbPreDelay, &reverbDecay, &reverbDamping, &reverbLevel, &reverbPower })
        reverbPanel.addAndMakeVisible (*b);

    chorusMode.setTitle ("Chorus mode");
    pingPong.setTitle ("Delay ping-pong");

    iChorusMode = ctx.mirror.indexOf ("fx.chorus.mode");
    iChorusMix = ctx.mirror.indexOf ("fx.chorus.mix");
    iDelayTime = ctx.mirror.indexOf ("fx.delay.time");
    iDelayFeedback = ctx.mirror.indexOf ("fx.delay.feedback");
    iPingPong = ctx.mirror.indexOf ("fx.delay.ping_pong");
    iDelayLevel = ctx.mirror.indexOf ("fx.delay.level");
    iPreDelay = ctx.mirror.indexOf ("fx.reverb.predelay");
    iDecay = ctx.mirror.indexOf ("fx.reverb.decay");
    iReverbLevel = ctx.mirror.indexOf ("fx.reverb.level");
    inputs = { iChorusMode, iChorusMix, iDelayTime, iDelayFeedback, iPingPong, iDelayLevel, iPreDelay, iDecay, iReverbLevel };

    // The pictures' handles (9.3.6). Their positions come from the pictures' own axes; the views are
    // sized first so the feedback handle knows how many pixels a full range is.
    delayView.setBounds (0, 0, kInner, kPictureHeight);
    reverbView.setBounds (0, 0, kInner, kPictureHeight);
    const float feedbackPxPerRange = delayView.laneHeight() / map::kDelayMaxFeedback;

    // Delay: one handle on top of the second echo. Its x moves the delay time (the whole echo
    // train), its y the repeats: the second echo is `feedback` times as loud as the first.
    delayView.addHandle (std::make_unique<GraphHandle> (
        c, "Delay echoes", -1, "fx.delay.time",
        [this] (float px) { return model::clamp01 (map::delayTimeFromSec (static_cast<float> (delayView.timeAtHandleX (px)))); }, "fx.delay.feedback",
        feedbackPxPerRange, [this] {
            return delayView.handleAt (static_cast<double> (map::delayTimeSec (value (iDelayTime, false))),
                                       map::delayFeedback (value (iDelayFeedback, false)), model::stepIndex (value (iPingPong, false), 2) == 1);
        }));

    // Reverb: the pre-delay handle where the tail starts, the decay handle where it has fallen 60 dB.
    reverbView.addHandle (std::make_unique<GraphHandle> (
        c, "Reverb pre-delay", -1, "fx.reverb.predelay",
        [this] (float px) { return model::clamp01 (map::reverbPreDelayFromSec (static_cast<float> (reverbView.timeAxis().timeAt (px)))); }, juce::String(),
        200.0f, [this] { return reverbView.preDelayHandleAt (map::reverbPreDelaySec (value (iPreDelay, false))); }));
    reverbView.addHandle (std::make_unique<GraphHandle> (
        c, "Reverb decay time", -1, "fx.reverb.decay",
        [this] (float px) {
            const double pre = static_cast<double> (map::reverbPreDelaySec (value (iPreDelay, true)));
            return model::clamp01 (map::reverbDecayFromSec (static_cast<float> (reverbView.timeAxis().timeAt (px) - pre)));
        },
        juce::String(), 200.0f, [this] {
            return reverbView.decayHandleAt (map::reverbPreDelaySec (value (iPreDelay, false)), map::reverbDecaySec (value (iDecay, false)));
        }));
    for (int i = 0; i < delayView.numHandles(); ++i)
        delayView.getHandle (i).onChange = [this] { recomputePictures (true); };
    for (int i = 0; i < reverbView.numHandles(); ++i)
        reverbView.getHandle (i).onChange = [this] { recomputePictures (true); };

    for (int i : inputs)
        ctx.mirror.subscribe (i, this);
    ctx.mirror.addIdleClient (this);

    setSize (kDesignWidth - kRail, kDesignHeight - kTopBar - kHelpLine);
    updateStates();
    recomputePictures (false);
}

EffectsPage::~EffectsPage()
{
    cancelInteraction();
    ctx.mirror.unsubscribe (this);
    ctx.mirror.removeIdleClient (this);
}

void EffectsPage::cancelInteraction()
{
    delayView.cancelInteraction();
    reverbView.cancelInteraction();
    for (auto* b : std::initializer_list<BoundComponent*> { &chorusMode, &pingPong, &chorusMix, &delayTime, &delayFeedback, &delayDamping, &delayLevel,
                                                             &reverbPreDelay, &reverbDecay, &reverbDamping, &reverbLevel })
        b->cancelInteraction();
}

Slider& EffectsPage::getSlider (const juce::String& id) noexcept
{
    for (auto* s : { &chorusMix, &delayTime, &delayFeedback, &delayDamping, &delayLevel, &reverbPreDelay, &reverbDecay, &reverbDamping, &reverbLevel })
        if (s->getComponentID() == id)
            return *s;
    jassertfalse;
    return chorusMix;
}

float EffectsPage::value (int index, bool fromLive) const
{
    return fromLive ? ctx.mirror.parameter (index).getValue() : ctx.mirror.value (index);
}

void EffectsPage::mirrorChanged (int, bool valueChanged)
{
    if (valueChanged)
        dirty = true; // pictures and rules follow once, at the end of the tick (mirrorIdle)
}

void EffectsPage::mirrorIdle (double)
{
    if (! dirty)
        return;
    dirty = false;
    updateStates();
    bool dragging = false;
    for (int i = 0; i < delayView.numHandles(); ++i)
        dragging = dragging || delayView.getHandle (i).getModel().isDragging();
    for (int i = 0; i < reverbView.numHandles(); ++i)
        dragging = dragging || reverbView.getHandle (i).getModel().isDragging();
    recomputePictures (dragging);
}

void EffectsPage::updateStates()
{
    // 9.3.9: display only. Everything but Level and the power switch dims while the level is 0.
    chorusMix.setInactive (model::stepIndex (value (iChorusMode, false), 3) == 0);
    const bool delayOn = value (iDelayLevel, false) > 0.0f, reverbOn = value (iReverbLevel, false) > 0.0f;
    for (auto* b : std::initializer_list<BoundComponent*> { &pingPong, &delayTime, &delayFeedback, &delayDamping })
        b->setInactive (! delayOn);
    for (auto* b : std::initializer_list<BoundComponent*> { &reverbPreDelay, &reverbDecay, &reverbDamping })
        b->setInactive (! reverbOn);
    if (delayOn != delayWasOn || delayHeaderText.isEmpty())
    {
        delayWasOn = delayOn;
        delayHeaderText = delayOn ? "Delay" : "Delay - OFF";
        delayPanel.setText (delayHeaderText);
    }
    if (reverbOn != reverbWasOn || reverbHeaderText.isEmpty())
    {
        reverbWasOn = reverbOn;
        reverbHeaderText = reverbOn ? "Reverb" : "Reverb - OFF";
        reverbPanel.setText (reverbHeaderText);
    }
}

model::DelayPicture EffectsPage::expectedDelay() const
{
    return model::delayPicture (static_cast<double> (map::delayTimeSec (value (iDelayTime, false))), map::delayFeedback (value (iDelayFeedback, false)),
                                model::stepIndex (value (iPingPong, false), 2) == 1, delayView.timeAxis().hi);
}

model::ReverbPicture EffectsPage::expectedReverb() const
{
    return model::reverbPicture (map::reverbPreDelaySec (value (iPreDelay, false)), map::reverbDecaySec (value (iDecay, false)));
}

void EffectsPage::recomputePictures (bool fromLive)
{
    ++computations;
    const auto text = [&] (int index) {
        auto& p = ctx.mirror.parameter (index);
        const auto label = p.getLabel();
        const auto t = p.getText (value (index, fromLive), 1024);
        return label.isEmpty() ? t : t + " " + label;
    };
    chorusView.setState (model::stepIndex (value (iChorusMode, fromLive), 3), value (iChorusMix, fromLive));
    const double time = static_cast<double> (map::delayTimeSec (value (iDelayTime, fromLive)));
    const float gain = map::delayFeedback (value (iDelayFeedback, fromLive));
    const bool pp = model::stepIndex (value (iPingPong, fromLive), 2) == 1;
    delayView.setState (time, gain, pp, value (iDelayLevel, fromLive) > 0.0f, text (iDelayTime));
    const float pre = map::reverbPreDelaySec (value (iPreDelay, fromLive));
    const float decay = map::reverbDecaySec (value (iDecay, fromLive));
    reverbView.setState (pre, decay, value (iReverbLevel, fromLive) > 0.0f, text (iDecay));
    placeHandles (fromLive);
}

void EffectsPage::placeHandles (bool fromLive)
{
    if (delayView.plotArea().isEmpty() || reverbView.plotArea().isEmpty())
        return;
    delayView.getHandle (0).setCentre (delayView.handleAt (static_cast<double> (map::delayTimeSec (value (iDelayTime, fromLive))),
                                                           map::delayFeedback (value (iDelayFeedback, fromLive)),
                                                           model::stepIndex (value (iPingPong, fromLive), 2) == 1));
    const float pre = map::reverbPreDelaySec (value (iPreDelay, fromLive));
    reverbView.getHandle (0).setCentre (reverbView.preDelayHandleAt (pre));
    reverbView.getHandle (1).setCentre (reverbView.decayHandleAt (pre, map::reverbDecaySec (value (iDecay, fromLive))));
}

void EffectsPage::resized()
{
    chorusPanel.setBounds (12, 12, kPanelWidth, 688);
    delayPanel.setBounds (424, 12, kPanelWidth, 688);
    reverbPanel.setBounds (836, 12, kPanelWidth, 688);

    chorusView.setBounds (kPad, 36, kInner, kPictureHeight);
    chorusMode.setBounds (kPad, 304, kInner, kLabelRow + kSegment);
    chorusMix.setBounds (kPad, 362, kInner, 40);

    delayView.setBounds (kPad, 36, kInner, kPictureHeight);
    pingPong.setBounds (kPad, 304, kInner, kLabelRow + kSegment);
    delayTime.setBounds (kPad, 362, kInner, 40);
    delayFeedback.setBounds (kPad, 410, kInner, 40);
    delayDamping.setBounds (kPad, 458, kInner, 40);
    delayLevel.setBounds (kPad, 506, kInner, 40);
    delayPower.setBounds (kPanelWidth - kPad - 56, 4, 56, 20);

    reverbView.setBounds (kPad, 36, kInner, kPictureHeight);
    reverbPreDelay.setBounds (kPad, 304, kInner, 40);
    reverbDecay.setBounds (kPad, 352, kInner, 40);
    reverbDamping.setBounds (kPad, 400, kInner, 40);
    reverbLevel.setBounds (kPad, 448, kInner, 40);
    reverbPower.setBounds (kPanelWidth - kPad - 56, 4, 56, 20);
    placeHandles (false);
}

void EffectsPage::paint (juce::Graphics& g)
{
    g.fillAll (colour (Tok::BgWindow));
}
} // namespace vs::ui
