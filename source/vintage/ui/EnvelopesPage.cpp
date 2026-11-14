#include "vintage/ui/EnvelopesPage.h"

#include "vintage/ui/EnvelopePicture.h"
#include "vintage/params/PartParamList.h"

namespace vs::ui
{
namespace
{
using namespace metrics;

juce::String prefix (int part) { return part == 0 ? kLowerPrefix : kUpperPrefix; }

/** Names a control for screen readers and the help line ("Lower Envelope 1 attack"). */
void rename (BoundComponent& c, int part, const juce::String& name)
{
    c.setTitle (partPrefix (part) + name);
    c.getProperties().set (helpLabelProperty(), name);
}

EnvelopeView::ShapeFn shapeFunction()
{
    return [] (const model::EnvelopeValues& v) { return computeEnvelopeShape (v); };
}

constexpr int kPanelW = 606, kPanelH = 688, kInnerW = 582;
constexpr int kViewY = 36, kViewH = 244, kSlidersY = 290, kRowY = 342, kDividerY = 396;
} // namespace

EnvelopesPage::EnvelopesPage (UiContext& c, int p)
    : ctx (c),
      part (p),
      panel1 (c.theme, "Envelope 1", p),
      panel2 (c.theme, "Envelope 2", p),
      view1 (c, p, { prefix (p) + "env1.attack", prefix (p) + "env1.decay", prefix (p) + "env1.sustain", prefix (p) + "env1.release",
                     prefix (p) + "env1.polarity" },
             "Envelope 1", shapeFunction()),
      view2 (c, p, { prefix (p) + "env2.attack", prefix (p) + "env2.decay", prefix (p) + "env2.sustain", prefix (p) + "env2.release", {} },
             "Envelope 2", shapeFunction()),
      attack1 (c, prefix (p) + "env1.attack", "Attack", p),
      decay1 (c, prefix (p) + "env1.decay", "Decay", p),
      sustain1 (c, prefix (p) + "env1.sustain", "Sustain", p),
      release1 (c, prefix (p) + "env1.release", "Release", p),
      keyFollow1 (c, prefix (p) + "env1.key_follow", "Key follow", p),
      direction (c, prefix (p) + "env1.polarity", "Direction", p, { "Normal", "Inverted" }),
      filterAmount (c, prefix (p) + "vcf.env_mod", "Opens the filter by", p),
      filterSource (c, prefix (p) + "vcf.env_source", "Filter is moved by", p, { "Envelope 1", "Envelope 2" }),
      pitchAmount (c, prefix (p) + "vcomod.env_mod", "Bends the pitch by", p),
      velFilter (c, prefix (p) + "vel.vcf", "Playing softer closes the filter by", p),
      attack2 (c, prefix (p) + "env2.attack", "Attack", p),
      decay2 (c, prefix (p) + "env2.decay", "Decay", p),
      sustain2 (c, prefix (p) + "env2.sustain", "Sustain", p),
      release2 (c, prefix (p) + "env2.release", "Release", p),
      keyFollow2 (c, prefix (p) + "env2.key_follow", "Key follow", p),
      velAmp (c, prefix (p) + "vel.vca", "Playing softer makes it quieter by", p),
      smallFont (c.theme.font (kTextSmall)),
      boldFont (c.theme.font (kText, true)),
      what1 ("What Envelope 1 does"),
      what2 ("What Envelope 2 does"),
      noteTarget ("Which oscillators the pitch bend moves: see the LFO page, Pitch goes to."),
      note2a ("It always shapes the loudness of each note, and decides how long a note lives."),
      note2b ("The filter and loudness of every note follow these two shapes.")
{
    setTitle (juce::String ("Envelopes, ") + (p == 0 ? "Lower" : "Upper") + " part");
    setComponentID (juce::String ("envelopes.") + (p == 0 ? "lower" : "upper"));

    for (auto* panel : { &panel1, &panel2 })
        addAndMakeVisible (*panel);
    panel1.addAndMakeVisible (view1);
    panel2.addAndMakeVisible (view2);
    for (auto* s : { &attack1, &decay1, &sustain1, &release1, &filterAmount, &pitchAmount, &velFilter })
        panel1.addAndMakeVisible (*s);
    panel1.addAndMakeVisible (keyFollow1);
    panel1.addAndMakeVisible (direction);
    panel1.addAndMakeVisible (filterSource);
    for (auto* s : { &attack2, &decay2, &sustain2, &release2, &velAmp })
        panel2.addAndMakeVisible (*s);
    panel2.addAndMakeVisible (keyFollow2);

    rename (attack1, p, "Envelope 1 attack");
    rename (decay1, p, "Envelope 1 decay");
    rename (sustain1, p, "Envelope 1 sustain");
    rename (release1, p, "Envelope 1 release");
    rename (keyFollow1, p, "Envelope 1 key follow");
    rename (direction, p, "Envelope 1 direction");
    rename (attack2, p, "Envelope 2 attack");
    rename (decay2, p, "Envelope 2 decay");
    rename (sustain2, p, "Envelope 2 sustain");
    rename (release2, p, "Envelope 2 release");
    rename (keyFollow2, p, "Envelope 2 key follow");

    sourceIndex = ctx.mirror.indexOf (prefix (p) + "vcf.env_source");
    ctx.mirror.subscribe (sourceIndex, this);
    title1 = expectedTitle (0);
    title2 = expectedTitle (1);
    panel1.setText (title1);
    panel2.setText (title2);

    setSize (kDesignWidth - kRail, kDesignHeight - kTopBar - kHelpLine);
}

EnvelopesPage::~EnvelopesPage()
{
    cancelInteraction();
    ctx.mirror.unsubscribe (this);
}

void EnvelopesPage::cancelInteraction()
{
    view1.cancelInteraction();
    view2.cancelInteraction();
    for (auto* b : std::initializer_list<BoundComponent*> { &attack1, &decay1, &sustain1, &release1, &keyFollow1, &direction, &filterAmount, &filterSource,
                                                             &pitchAmount, &velFilter, &attack2, &decay2, &sustain2, &release2, &keyFollow2, &velAmp })
        b->cancelInteraction();
}

juce::String EnvelopesPage::expectedTitle (int envelope) const
{
    // The code drives the filter with Envelope 1 or 2 (vcf.env_source), the pitch with Envelope 1
    // only and the loudness with Envelope 2 always (9.3.2).
    const bool env1ForFilter = model::stepIndex (ctx.mirror.value (sourceIndex), 2) == 0;
    if (envelope == 0)
        return env1ForFilter ? "Envelope 1 (filter, pitch)" : "Envelope 1 (pitch)";
    return env1ForFilter ? "Envelope 2 (loudness)" : "Envelope 2 (loudness, filter)";
}

void EnvelopesPage::mirrorChanged (int index, bool valueChanged)
{
    if (index == sourceIndex && valueChanged)
        updateTitles();
}

void EnvelopesPage::updateTitles()
{
    title1 = expectedTitle (0);
    title2 = expectedTitle (1);
    panel1.setText (title1);
    panel2.setText (title2);
}

void EnvelopesPage::resized()
{
    // Grid layout in logical px (9.2.3): two panels side by side, each a picture, its four
    // sliders, key follow, and "what it does" below a divider.
    panel1.setBounds (12, 12, kPanelW, kPanelH);
    panel2.setBounds (kPanelW + 24, 12, kPanelW, kPanelH);

    view1.setBounds (12, kViewY, kInnerW, kViewH);
    view2.setBounds (12, kViewY, kInnerW, kViewH);
    constexpr int sliderW = 138, gap = 10;
    int i = 0;
    for (auto* s : { &attack1, &decay1, &sustain1, &release1 })
        s->setBounds (12 + i++ * (sliderW + gap), kSlidersY, sliderW, 40);
    i = 0;
    for (auto* s : { &attack2, &decay2, &sustain2, &release2 })
        s->setBounds (12 + i++ * (sliderW + gap), kSlidersY, sliderW, 40);

    keyFollow1.setBounds (12, kRowY + 14, 120, kSegment);
    direction.setBounds (170, kRowY - 2, 260, kLabelRow + kSegment);
    keyFollow2.setBounds (12, kRowY + 14, 120, kSegment);

    filterAmount.setBounds (12, 436, kInnerW, 40);
    filterSource.setBounds (12, 490, 300, kLabelRow + kSegment);
    pitchAmount.setBounds (12, 552, kInnerW, 40);
    velFilter.setBounds (12, 624, kInnerW, 40);

    velAmp.setBounds (12, 480, kInnerW, 40);
}

void EnvelopesPage::paint (juce::Graphics& g)
{
    g.fillAll (colour (Tok::BgWindow));
}

void EnvelopesPage::paintOverChildren (juce::Graphics& g)
{
    // Captions that belong to the panels (grid positions; never measured).
    g.setColour (colour (Tok::BgTrack));
    for (auto* panel : { &panel1, &panel2 })
        g.drawHorizontalLine (panel->getY() + kDividerY, static_cast<float> (panel->getX() + 12), static_cast<float> (panel->getRight() - 12));
    g.setColour (colour (Tok::TextPrimary));
    g.setFont (boldFont);
    const auto at = [] (const Panel& panel, int x, int y, int w, int h) {
        return juce::Rectangle<float> (static_cast<float> (panel.getX() + x), static_cast<float> (panel.getY() + y), static_cast<float> (w),
                                       static_cast<float> (h));
    };
    g.drawText (what1, at (panel1, 12, kDividerY + 8, kInnerW, 20), juce::Justification::centredLeft, false);
    g.drawText (what2, at (panel2, 12, kDividerY + 8, kInnerW, 20), juce::Justification::centredLeft, false);
    g.setFont (smallFont);
    g.setColour (colour (Tok::TextSecondary));
    g.drawText (noteTarget, at (panel1, 12, 594, kInnerW, 14), juce::Justification::centredLeft, true);
    g.drawText (note2a, at (panel2, 12, kDividerY + 34, kInnerW, 14), juce::Justification::centredLeft, true);
    g.drawText (note2b, at (panel2, 12, 540, kInnerW, 14), juce::Justification::centredLeft, true);
}
} // namespace vs::ui
