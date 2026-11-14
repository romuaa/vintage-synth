#include "vintage/ui/LfoPage.h"

#include "vintage/ui/EnvelopePicture.h"
#include "vintage/params/PartParamList.h"

namespace vs::ui
{
namespace
{
using namespace metrics;

juce::String prefix (int part) { return part == 0 ? kLowerPrefix : kUpperPrefix; }

constexpr int kPulseWave = 4; // kVcoWaveLabels: Sine, Triangle, Saw, Square, Pulse, Noise
constexpr int kTopH = 352, kSubY = 402, kSubH = 298, kSubW = 297, kInnerW = 273;
} // namespace

LfoPage::LfoPage (UiContext& c, int p)
    : ctx (c),
      part (p),
      lfoPanel (c.theme, "LFO (a slow wobble)", p),
      pitchPanel (c.theme, "The pitch (vibrato)", p),
      pwmPanel (c.theme, "The pulse width", p),
      filterPanel (c.theme, "The filter (wah)", p),
      volumePanel (c.theme, "The volume (tremolo)", p),
      view (c, p, { prefix (p) + "lfo.waveform", prefix (p) + "lfo.rate", prefix (p) + "lfo.delay" },
            [] (int wave, float rateHz, float delaySec, float windowSec, model::LfoPreview& out) { computeLfoPreview (wave, rateHz, delaySec, windowSec, out); }),
      waveform (c, prefix (p) + "lfo.waveform", "Waveform", p),
      rate (c, prefix (p) + "lfo.rate", "Rate", p),
      delay (c, prefix (p) + "lfo.delay", "Delay until full strength", p),
      pitchAmount (c, prefix (p) + "vcomod.lfo_mod", "Amount", p),
      pitchTarget (c, prefix (p) + "vcomod.target", "Pitch goes to", p, { "Osc 1", "Both", "Osc 2" }),
      pwmAmount (c, prefix (p) + "vcomod.pwm", "Amount", p),
      pwmSource (c, prefix (p) + "vcomod.pwm_source", "Pulse width is driven by", p, { "LFO", "Manual", "Env 1" }),
      filterAmount (c, prefix (p) + "vcf.lfo_mod", "Amount", p),
      volumeAmount (c, prefix (p) + "vca.lfo_mod", "Amount (4 fixed steps)", p),
      smallFont (c.theme.font (kTextSmall)),
      boldFont (c.theme.font (kText, true)),
      movesCaption ("The LFO moves ..."),
      notePitch ("This choice also applies to the Envelope 1 pitch amount."),
      notePwm ("Only audible when an oscillator is set to Pulse."),
      noteFilter ("The filter sweeps up and down."),
      noteVolume ("0 = no wobble, 3 = the strongest.")
{
    setTitle (juce::String ("LFO, ") + (p == 0 ? "Lower" : "Upper") + " part");
    setComponentID (juce::String ("lfo.") + (p == 0 ? "lower" : "upper"));

    for (auto* panel : { &lfoPanel, &pitchPanel, &pwmPanel, &filterPanel, &volumePanel })
        addAndMakeVisible (*panel);
    lfoPanel.addAndMakeVisible (view);
    lfoPanel.addAndMakeVisible (waveform);
    lfoPanel.addAndMakeVisible (rate);
    lfoPanel.addAndMakeVisible (delay);
    pitchPanel.addAndMakeVisible (pitchAmount);
    pitchPanel.addAndMakeVisible (pitchTarget);
    pwmPanel.addAndMakeVisible (pwmAmount);
    pwmPanel.addAndMakeVisible (pwmSource);
    filterPanel.addAndMakeVisible (filterAmount);
    volumePanel.addAndMakeVisible (volumeAmount);

    // "Amount" alone is ambiguous for screen readers: name what it moves.
    const auto rename = [p] (BoundComponent& comp, const juce::String& name) {
        comp.setTitle (partPrefix (p) + name);
        comp.getProperties().set (helpLabelProperty(), name);
    };
    rename (pitchAmount, "LFO pitch amount");
    rename (pwmAmount, "Pulse width amount");
    rename (filterAmount, "LFO filter amount");
    rename (volumeAmount, "LFO volume steps");
    rename (delay, "LFO delay");
    rename (rate, "LFO rate");
    rename (waveform, "LFO waveform");

    view.setExampleWaves ({ 4, 5 }); // Random and Noise are different every time (kLfoWaveLabels order)

    waveIndex[0] = ctx.mirror.indexOf (prefix (p) + "vco1.waveform");
    waveIndex[1] = ctx.mirror.indexOf (prefix (p) + "vco2.waveform");
    for (int i : waveIndex)
        ctx.mirror.subscribe (i, this);
    updateInactive();

    setSize (kDesignWidth - kRail, kDesignHeight - kTopBar - kHelpLine);
}

LfoPage::~LfoPage()
{
    cancelInteraction();
    ctx.mirror.unsubscribe (this);
}

void LfoPage::cancelInteraction()
{
    view.cancelInteraction();
    for (auto* b : std::initializer_list<BoundComponent*> { &waveform, &rate, &delay, &pitchAmount, &pitchTarget, &pwmAmount, &pwmSource, &filterAmount,
                                                             &volumeAmount })
        b->cancelInteraction();
}

bool LfoPage::pulseWidthExpectedInactive() const
{
    // Neither oscillator's waveform is Pulse (9.3.9).
    for (int i : waveIndex)
        if (model::stepIndex (ctx.mirror.value (i), static_cast<int> (ctx.mirror.parameter (i).getNumSteps())) == kPulseWave)
            return false;
    return true;
}

void LfoPage::mirrorChanged (int, bool valueChanged)
{
    if (valueChanged)
        updateInactive();
}

void LfoPage::updateInactive()
{
    const bool inactive = pulseWidthExpectedInactive();
    pwmAmount.setInactive (inactive);
    pwmSource.setInactive (inactive);
}

void LfoPage::resized()
{
    // Grid layout in logical px (9.2.3): the picture panel on top, the four "moves" panels below.
    lfoPanel.setBounds (12, 12, 1224, kTopH);
    view.setBounds (12, 36, 1200, 240);
    waveform.setBounds (12, 292, 470, kLabelRow + kSegment);
    rate.setBounds (512, 296, 330, 40);
    delay.setBounds (872, 296, 340, 40);

    pitchPanel.setBounds (12, kSubY, kSubW, kSubH);
    pwmPanel.setBounds (12 + kSubW + 12, kSubY, kSubW, kSubH);
    filterPanel.setBounds (12 + 2 * (kSubW + 12), kSubY, kSubW, kSubH);
    volumePanel.setBounds (12 + 3 * (kSubW + 12), kSubY, kSubW, kSubH);

    pitchAmount.setBounds (12, 44, kInnerW, 40);
    pitchTarget.setBounds (12, 112, kInnerW, kLabelRow + kSegment);
    pwmAmount.setBounds (12, 44, kInnerW, 40);
    pwmSource.setBounds (12, 112, kInnerW, kLabelRow + kSegment);
    filterAmount.setBounds (12, 44, kInnerW, 40);
    volumeAmount.setBounds (12, 44, kInnerW, kLabelRow + kSegment);
}

void LfoPage::paint (juce::Graphics& g)
{
    g.fillAll (colour (Tok::BgWindow));
}

void LfoPage::paintOverChildren (juce::Graphics& g)
{
    // Captions that belong to the panels (grid positions; never measured).
    g.setColour (colour (Tok::TextPrimary));
    g.setFont (boldFont);
    g.drawText (movesCaption, juce::Rectangle<float> (12.0f, static_cast<float> (kTopH + 24), 400.0f, 20.0f), juce::Justification::centredLeft, false);
    g.setFont (smallFont);
    g.setColour (colour (Tok::TextSecondary));
    const auto note = [&] (const Panel& panel, const juce::String& text, int y) {
        g.drawFittedText (text, juce::Rectangle<int> (panel.getX() + 12, panel.getY() + y, kInnerW, 30), juce::Justification::topLeft, 2, 1.0f);
    };
    note (pitchPanel, notePitch, 168);
    note (pwmPanel, notePwm, 168);
    note (filterPanel, noteFilter, 92);
    note (volumePanel, noteVolume, 100);
}
} // namespace vs::ui
