#include "vintage/ui/PlayPage.h"

#include "vintage/params/PartParamList.h"

namespace vs::ui
{
namespace
{
using namespace metrics;

juce::String prefix (int part) { return part == 0 ? kLowerPrefix : kUpperPrefix; }
} // namespace

juce::String PlayPage::sentenceFor (int keyMode)
{
    switch (keyMode)
    {
        case 1: return "Dual: both sounds play together on every key.";
        case 2: return "Split: Lower plays below the split key, Upper plays from it upwards.";
        default: return "Whole: one sound, the Lower part, on every key.";
    }
}

PlayPage::PlayPage (UiContext& c, const UiFeed& feed, UiNotes* notes)
    : ctx (c),
      layoutPanel (c.theme, "Keyboard layout", -1),
      characterPanel (c.theme, "Character", -1),
      partPanel { Panel (c.theme, "Lower part", 0), Panel (c.theme, "Upper part", 1) },
      mode (c, "key.mode", {}, -1, { "Whole", "Dual", "Split" }),
      strip (c, feed, notes),
      balance (c, "mix.balance", juce::String::fromUTF8 ("Balance Lower \xe2\x86\x94 Upper"), -1, Slider::Style::Horizontal, true),
      vintage (c, "global.vintage", "Vintage (analog drift and part-to-part looseness)", -1),
      stripCaption ("Press the keys to play. Drag the Split pill to choose the split key. Keys you hold are tinted in the part's colour; a small bar at the foot of a key shows a note sounding."),
      vintageCaption ("0 is clean. More adds slow pitch drift and small differences between voices."),
      glideCaption ("Glide between notes (portamento)"),
      fonts (c.theme)
{
    setTitle ("Play");
    setComponentID ("play");
    mode.setTitle ("Key mode");
    mode.getProperties().set (helpLabelProperty(), "Key mode");

    for (auto* panel : { &layoutPanel, &characterPanel, &partPanel[0], &partPanel[1] })
        addAndMakeVisible (*panel);
    layoutPanel.addAndMakeVisible (mode);
    layoutPanel.addAndMakeVisible (strip);
    layoutPanel.addAndMakeVisible (balance);
    characterPanel.addAndMakeVisible (vintage);

    for (int p = 0; p < 2; ++p)
    {
        auto& pc = parts[p];
        auto& panel = partPanel[p];
        pc.voices = std::make_unique<SegmentedSwitch> (c, prefix (p) + "assign.mode", "Voices", p);
        pc.unisonVoices = std::make_unique<SegmentedSwitch> (c, prefix (p) + "assign.unison_voices", "Unison voices", p);
        pc.detune = std::make_unique<Slider> (c, prefix (p) + "unison.detune", "Unison detune", p);
        pc.glideTime = std::make_unique<Slider> (c, prefix (p) + "porta.time", "Glide time", p);
        pc.glideMode = std::make_unique<SegmentedSwitch> (c, prefix (p) + "porta.mode", "Glide", p, juce::StringArray { "Always", "Legato only" });
        pc.bend = std::make_unique<Slider> (c, prefix (p) + "bend.range", "Pitch bend range", p);
        pc.vibrato = std::make_unique<Slider> (c, prefix (p) + "mod.wheel_vco", "Mod wheel vibrato", p);
        pc.pan = std::make_unique<Slider> (c, prefix (p) + "mix.pan", juce::String::fromUTF8 ("Pan L \xe2\x86\x94 R"), p, Slider::Style::Horizontal, true);
        for (auto* b : std::initializer_list<juce::Component*> { pc.voices.get(), pc.unisonVoices.get(), pc.detune.get(), pc.glideTime.get(),
                                                                  pc.glideMode.get(), pc.bend.get(), pc.vibrato.get(), pc.pan.get() })
            panel.addAndMakeVisible (*b);
    }

    modeIndex = ctx.mirror.indexOf ("key.mode");
    ctx.mirror.subscribe (modeIndex, this);
    for (int p = 0; p < 2; ++p)
    {
        assignIndex[p] = ctx.mirror.indexOf (prefix (p) + "assign.mode");
        portaIndex[p] = ctx.mirror.indexOf (prefix (p) + "porta.time");
        ctx.mirror.subscribe (assignIndex[p], this);
        ctx.mirror.subscribe (portaIndex[p], this);
    }
    updateStates();
    setSize (kDesignWidth - kRail, kDesignHeight - kTopBar - kHelpLine);
}

PlayPage::~PlayPage()
{
    cancelInteraction();
    ctx.mirror.unsubscribe (this);
}

void PlayPage::cancelInteraction()
{
    strip.cancelInteraction();
    for (auto* b : std::initializer_list<BoundComponent*> { &mode, &balance, &vintage })
        b->cancelInteraction();
    for (auto& pc : parts)
        for (auto* b : std::initializer_list<BoundComponent*> { pc.voices.get(), pc.unisonVoices.get(), pc.detune.get(), pc.glideTime.get(),
                                                                 pc.glideMode.get(), pc.bend.get(), pc.vibrato.get(), pc.pan.get() })
            if (b != nullptr)
                b->cancelInteraction();
}

void PlayPage::mirrorChanged (int, bool valueChanged)
{
    if (valueChanged)
        updateStates();
}

void PlayPage::updateStates()
{
    // 9.3.9 (display only: the controls stay editable).
    const int keyMode = model::stepIndex (ctx.mirror.value (modeIndex), 3);
    balance.setInactive (keyMode == 0);
    for (int p = 0; p < 2; ++p)
    {
        auto& pc = parts[p];
        const bool unison = model::stepIndex (ctx.mirror.value (assignIndex[p]), 3) == 1;
        pc.unisonVoices->setInactive (! unison);
        pc.detune->setInactive (! unison);
        pc.glideMode->setInactive (std::fpclassify (ctx.mirror.value (portaIndex[p])) == FP_ZERO);
    }
    partPanel[1].setText (keyMode == 0 ? "Upper part (not played in Whole mode)" : "Upper part");
    const auto next = sentenceFor (keyMode);
    if (next != sentence)
    {
        sentence = next;
        repaint (layoutPanel.getBounds());
    }
}

void PlayPage::resized()
{
    // Grid layout in logical px (docs/architecture.md 9.2.3). Content area 1248 x 712.
    layoutPanel.setBounds (12, 12, 1224, 306);
    characterPanel.setBounds (12, 330, 1224, 76);
    partPanel[0].setBounds (12, 418, 606, 282);
    partPanel[1].setBounds (630, 418, 606, 282);

    mode.setBounds (12, 38, 330, kSegment + 4);
    balance.setBounds (880, 34, 332, 40);
    strip.setBounds (12, 86, 1200, 186);

    vintage.setBounds (12, 34, 760, 36);

    for (auto& pc : parts)
    {
        pc.voices->setBounds (12, 36, 270, kLabelRow + kSegment);
        pc.unisonVoices->setBounds (12, 90, 270, kLabelRow + kSegment);
        pc.detune->setBounds (12, 144, 270, 40);
        pc.bend->setBounds (312, 36, 270, 40);
        pc.vibrato->setBounds (312, 84, 270, 40);
        pc.pan->setBounds (312, 132, 270, 40);
        pc.glideTime->setBounds (12, 214, 270, 40);
        pc.glideMode->setBounds (312, 208, 270, kLabelRow + kSegment);
    }
}

void PlayPage::paint (juce::Graphics& g)
{
    g.fillAll (colour (Tok::BgWindow));
}

void PlayPage::paintOverChildren (juce::Graphics& g)
{
    g.setColour (colour (Tok::TextPrimary));
    g.setFont (fonts.text);
    const auto layout = layoutPanel.getBounds().toFloat();
    drawFittedText (g, sentence, juce::Rectangle<float> (layout.getX() + 360.0f, layout.getY() + 38.0f, 500.0f, 32.0f), juce::Justification::centredLeft);
    g.setColour (colour (Tok::TextSecondary));
    g.setFont (fonts.small);
    drawFittedText (g, stripCaption,
                    juce::Rectangle<float> (layout.getX() + 12.0f, layout.getBottom() - 22.0f, 1200.0f, 16.0f), juce::Justification::centredLeft);

    const auto character = characterPanel.getBounds().toFloat();
    drawFittedText (g, vintageCaption,
                    juce::Rectangle<float> (character.getX() + 792.0f, character.getY() + 34.0f, 420.0f, 36.0f), juce::Justification::centredLeft);

    for (int p = 0; p < 2; ++p)
    {
        const auto panel = partPanel[p].getBounds().toFloat();
        g.setColour (colour (Tok::TextSecondary));
        g.setFont (fonts.textBold);
        drawFittedText (g, glideCaption, juce::Rectangle<float> (panel.getX() + 12.0f, panel.getY() + 190.0f, 570.0f, 18.0f),
                        juce::Justification::centredLeft);
        g.setColour (colour (Tok::BgTrack));
        g.fillRect (panel.getX() + 12.0f, panel.getY() + 188.0f, 570.0f, 1.0f);
    }
}
} // namespace vs::ui
