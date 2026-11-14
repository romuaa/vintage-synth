#include "vintage/ui/ArpSeqPage.h"

#include "common/engine/KeyRouter.h"
#include "common/params/GlobalParams.h"
#include "common/params/ParamFactory.h"
#include "common/shell/ShellSupport.h" // detail::partSelected: the processor's own rule

#include <algorithm>

namespace vs::ui
{
namespace
{
using namespace metrics;

constexpr int kW = 1248;
constexpr int kModeH = 112;

// The five pattern pictures: the order of the steps as dots on a line (x, y in 0..1, y up).
struct Pt
{
    float x, y;
};
constexpr Pt kUp[] { { 0.0f, 0.0f }, { 0.33f, 0.33f }, { 0.66f, 0.66f }, { 1.0f, 1.0f } };
constexpr Pt kDown[] { { 0.0f, 1.0f }, { 0.33f, 0.66f }, { 0.66f, 0.33f }, { 1.0f, 0.0f } };
constexpr Pt kUpDown[] { { 0.0f, 0.0f }, { 0.25f, 0.5f }, { 0.5f, 1.0f }, { 0.75f, 0.5f }, { 1.0f, 0.0f } };
constexpr Pt kRandom[] { { 0.0f, 0.3f }, { 0.25f, 0.9f }, { 0.5f, 0.1f }, { 0.75f, 0.7f }, { 1.0f, 0.4f } };
constexpr Pt kAsPlayed[] { { 0.0f, 0.2f }, { 0.25f, 0.8f }, { 0.5f, 0.0f }, { 0.75f, 1.0f }, { 1.0f, 0.5f } };

void iconPaths (int pattern, juce::Rectangle<float> area, float dotRadius, juce::Path& line, juce::Path& dotsPath)
{
    const Pt* pts = kUp;
    int n = 4;
    switch (pattern)
    {
        case 1: pts = kDown; break;
        case 2: pts = kUpDown; n = 5; break;
        case 3: pts = kRandom; n = 5; break;
        case 4: pts = kAsPlayed; n = 5; break;
        default: break;
    }
    line.clear();
    dotsPath.clear();
    for (int i = 0; i < n; ++i)
    {
        const juce::Point<float> q (area.getX() + pts[i].x * area.getWidth(), area.getBottom() - pts[i].y * area.getHeight());
        if (i == 0)
            line.startNewSubPath (q);
        else
            line.lineTo (q);
        dotsPath.addEllipse (q.x - dotRadius, q.y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
    }
}

const char* sentenceFor (int mode)
{
    switch (mode)
    {
        case 1: return "Hold a chord on your keyboard. The arpeggiator plays its notes one at a time, in time with your host.";
        case 2: return "Hold a key. The sequence plays from the lowest key you hold and transposes with it.";
        default: return "Off: choose Arpeggiator or Sequencer to hear it.";
    }
}
} // namespace

ArpSeqPage::ArpSeqPage (UiContext& c, UiFeed& f, juce::AudioProcessorValueTreeState& state, shell::EditorState& es)
    : ctx (c),
      feed (f),
      editorState (es),
      patternPanel (c.theme, "1 Pick a pattern", -1),
      timingPanel (c.theme, "2 Set speed and feel", -1),
      previewPanel (c.theme, "3 Preview", -1),
      stepsPanel (c.theme, "Steps", -1),
      modeSwitch (c, ParamID::arpMode, {}, -1),
      target (c, ParamID::arpTarget, {}, -1),
      pattern (c, ParamID::arpPattern, {}, -1, { "Up", "Down", "Up + Down", "Random", "As played" }),
      octaves (c, ParamID::arpOctaves, "Octaves", -1),
      rate (c, ParamID::arpRate, "Rate (steps per beat, follows host tempo)", -1,
            { "1/4", "1/8", "1/8 triplet", "1/16", "1/16 triplet", "1/32" }),
      gate (c, ParamID::arpGate, "Gate (how long each note rings)", -1),
      swing (c, ParamID::arpSwing, "Swing (every 2nd note later)", -1),
      tempo (c, ParamID::arpTempo, "Tempo when the host is stopped", -1),
      length (c, ParamID::seqLength, "Length (steps)", -1),
      hold (c, ParamID::arpHold, "Hold (keep playing)", -1),
      grid (c),
      roll (c.theme)
{
    binding.bind (state);
    setTitle ("Arp + Seq");
    setComponentID ("arpseq.page");

    modeIdx = c.mirror.indexOf (ParamID::arpMode);
    targetIdx = c.mirror.indexOf (ParamID::arpTarget);
    keyModeIdx = c.mirror.indexOf (ParamID::keyMode);
    splitIdx = c.mirror.indexOf (ParamID::keySplitPoint);
    for (const char* id : { ParamID::arpMode, ParamID::arpTarget, ParamID::arpRate, ParamID::arpSwing, ParamID::arpGate, ParamID::arpHold,
                            ParamID::arpPattern, ParamID::arpOctaves, ParamID::keyMode, ParamID::keySplitPoint })
        previewInputs.push_back (c.mirror.indexOf (id));
    for (int i : previewInputs)
        c.mirror.subscribe (i, this);
    c.mirror.addIdleClient (this);

    // The Plays-through switch names its parts in their colours (Both: split in two).
    target.setSegmentPainter ([] (juce::Graphics& g, juce::Rectangle<float> seg, int i, bool lit) {
        const auto lower = accentFor (0), upper = accentFor (1);
        if (lit)
        {
            if (i == 2)
            {
                g.setColour (lower);
                g.fillRect (seg.withWidth (seg.getWidth() * 0.5f));
                g.setColour (upper);
                g.fillRect (seg.withLeft (seg.getCentreX()));
            }
            else
            {
                g.setColour (i == 0 ? lower : upper);
                g.fillRoundedRectangle (seg, 3.0f);
            }
        }
        else
        {
            const auto line = seg.withTop (seg.getBottom() - 3.0f).reduced (6.0f, 0.0f);
            if (i == 2)
            {
                g.setColour (lower.withAlpha (0.7f));
                g.fillRect (line.withWidth (line.getWidth() * 0.5f));
                g.setColour (upper.withAlpha (0.7f));
                g.fillRect (line.withLeft (line.getCentreX()));
            }
            else
            {
                g.setColour ((i == 0 ? lower : upper).withAlpha (0.7f));
                g.fillRect (line);
            }
        }
    });
    pattern.setSegmentPainter ([this] (juce::Graphics& g, juce::Rectangle<float>, int i, bool lit) {
        g.setColour (lit ? colour (Tok::TextOnAccent) : colour (Tok::TextPrimary));
        g.strokePath (iconLines[static_cast<size_t> (i)], juce::PathStrokeType (1.6f));
        g.fillPath (iconDots[static_cast<size_t> (i)]);
    }, true);
    modeSwitch.setLargeStyle (kTextLarge, 48);
    modeSwitch.setAccent (colour (Tok::AccentGlobal));
    for (auto* s : { &target, &pattern, &octaves, &rate })
        s->setAccent (colour (Tok::AccentGlobal));
    for (auto* s : { &gate, &swing, &tempo, &length })
        s->setAccent (colour (Tok::AccentGlobal));
    hold.setAccent (colour (Tok::AccentGlobal));
    target.setTitle ("Plays through");
    target.getProperties().set (helpLabelProperty(), "Plays through");

    setHelp (grid, "ui.seq_grid", "Step grid", c.help);
    setHelp (roll, "ui.arp_preview", "Preview", c.help);

    for (auto* panel : { &patternPanel, &timingPanel, &previewPanel, &stepsPanel })
        addAndMakeVisible (*panel);
    addAndMakeVisible (modeSwitch);
    addAndMakeVisible (target);
    patternPanel.addAndMakeVisible (pattern);
    patternPanel.addAndMakeVisible (octaves);
    for (auto* b : std::initializer_list<juce::Component*> { &rate, &gate, &swing, &tempo, &length, &hold })
        timingPanel.addAndMakeVisible (*b);
    previewPanel.addAndMakeVisible (roll);
    stepsPanel.addAndMakeVisible (grid);
    controls = { &target, &pattern, &octaves, &rate, &gate, &swing, &tempo, &length, &hold };

    updateState (juce::Time::getMillisecondCounterHiRes());
    recomputePreview();
    applyView();
    applyInactive();
}

ArpSeqPage::~ArpSeqPage()
{
    cancelInteraction();
    ctx.mirror.removeIdleClient (this);
    ctx.mirror.unsubscribe (this);
}

void ArpSeqPage::cancelInteraction()
{
    grid.cancelInteraction();
    for (auto* s : { &modeSwitch, &target, &pattern, &octaves, &rate })
        s->cancelInteraction();
    for (auto* s : { &gate, &swing, &tempo, &length })
        s->cancelInteraction();
    hold.cancelInteraction();
}

ArpSettings ArpSeqPage::engineSettings() const
{
    ArpSettings s;
    binding.read (s);
    return s;
}

juce::Rectangle<int> ArpSeqPage::panelBounds (int which) const
{
    switch (which)
    {
        case 0: return { 0, 0, kW, kModeH };
        case 1: return { 0, 124, 616, 264 };
        case 2: return view == 0 ? juce::Rectangle<int> (628, 124, 620, 264) : juce::Rectangle<int> (0, 532, kW, 180);
        case 3: return { 0, 400, kW, 312 };
        default: return { 0, 124, kW, 396 };
    }
}

void ArpSeqPage::resized()
{
    layoutAll();
}

void ArpSeqPage::layoutAll()
{
    modeSwitch.setBounds (12, 14, 780, 74);
    target.setBounds (816, 30, 420, 44);
    patternPanel.setBounds (panelBounds (1));
    previewPanel.setBounds (panelBounds (3));
    stepsPanel.setBounds (panelBounds (4));
    timingPanel.setBounds (panelBounds (2));

    // Arpeggiator view
    pattern.setBounds (12, 36, 592, 124);
    octaves.setBounds (12, 176, 270, 54);
    roll.setBounds (12, 36, kW - 24, previewPanel.getHeight() - 36 - 12);
    // Sequencer view
    grid.setBounds (12, 36, kW - 24, StepGrid::kHeight);

    if (view == 0)
    {
        rate.setBounds (12, 36, 596, 48);
        gate.setBounds (12, 100, 284, 44);
        swing.setBounds (324, 100, 284, 44);
        tempo.setBounds (12, 164, 284, 44);
        hold.setBounds (324, 172, 220, 32);
    }
    else
    {
        rate.setBounds (12, 36, 560, 48);
        length.setBounds (12, 100, 560, 44);
        gate.setBounds (596, 36, 296, 44);
        swing.setBounds (916, 36, 296, 44);
        tempo.setBounds (596, 100, 296, 44);
        hold.setBounds (916, 106, 220, 32);
    }
    rebuildIcons();
}

void ArpSeqPage::rebuildIcons()
{
    for (int i = 0; i < 5; ++i)
    {
        const auto seg = pattern.segmentBounds (i).reduced (1.0f); // the icon paints in the switch's own coordinates
        iconPaths (i, seg.withTrimmedBottom (24.0f).reduced (18.0f, 14.0f), 3.2f, iconLines[static_cast<size_t> (i)], iconDots[static_cast<size_t> (i)]);
    }
}

void ArpSeqPage::applyView()
{
    const bool arpView = view == 0;
    patternPanel.setVisible (arpView);
    previewPanel.setVisible (arpView);
    stepsPanel.setVisible (! arpView);
    length.setVisible (! arpView);
    timingPanel.setText (arpView ? "2 Set speed and feel" : "Timing and feel");
    layoutAll();
    sentence = sentenceFor (modeNow);
    repaint();
}

void ArpSeqPage::applyInactive()
{
    for (auto* b : controls)
        b->setInactive (off || (b == &tempo && hostPlaying));
    grid.setOff (off);
    roll.setDimmed (off);
}

void ArpSeqPage::mirrorChanged (int index, bool valueChanged)
{
    if (valueChanged && std::find (previewInputs.begin(), previewInputs.end(), index) != previewInputs.end())
        previewDirty = true;
}

void ArpSeqPage::updateState (double nowMs)
{
    // Mode and view: the view follows the mode; while it is Off, the last one used.
    modeNow = model::stepIndex (ctx.mirror.value (modeIdx), 3);
    if (modeNow != 0 && editorState.arpView.load (std::memory_order_relaxed) != modeNow - 1)
        editorState.arpView.store (modeNow - 1, std::memory_order_relaxed);
    const int wantView = modeNow == 0 ? juce::jlimit (0, 1, editorState.arpView.load (std::memory_order_relaxed)) : modeNow - 1;
    const bool wantOff = modeNow == 0;
    const bool viewChanged = wantView != view || sentence != sentenceFor (modeNow);
    const bool offChanged = wantOff != off;
    view = wantView;
    off = wantOff;
    if (viewChanged)
        applyView();

    // The feed: is the audio running, the held keys, the playing step, the host tempo.
    alive.update (feed.blockCount.load (std::memory_order_relaxed), nowMs);
    for (int p = 0; p < 2; ++p)
    {
        UiFeed::Order order;
        if (feed.readOrder (p, order) && memory[static_cast<size_t> (p)].update (order, nowMs))
            previewDirty = true;
    }
    const int keyMode = model::stepIndex (ctx.mirror.value (keyModeIdx), 3);
    const int tgt = model::stepIndex (ctx.mirror.value (targetIdx), 3);
    grid.setPlayingStep (model::playingStepOf (modeNow == 2, alive.alive(), tgt, keyMode, feed.part[0].seqStep.load (std::memory_order_relaxed),
                                               feed.part[1].seqStep.load (std::memory_order_relaxed)));

    const bool playing = feed.hostPlaying.load (std::memory_order_relaxed);
    const int bpm = static_cast<int> (std::lround (feed.hostBpm.load (std::memory_order_relaxed)));
    const bool hostChanged = playing != hostPlaying || (playing && bpm != hostBpm);
    if (hostChanged)
    {
        hostPlaying = playing;
        hostBpm = bpm;
        hostText = playing ? "host: " + juce::String (bpm) + " bpm" : juce::String();
        repaint();
    }
    if (offChanged || hostChanged)
        applyInactive();
}

void ArpSeqPage::mirrorIdle (double nowMs)
{
    updateState (nowMs);
    if (previewDirty)
        recomputePreview();
}

void ArpSeqPage::recomputePreview()
{
    previewDirty = false;
    ++previewRuns;

    // The engine's own settings reading (atomics), the processor's own part rule.
    ArpSettings s = engineSettings();
    input.settings = s.settings;
    const auto keyMode = static_cast<KeyMode> (model::stepIndex (ctx.mirror.value (keyModeIdx), 3));
    int parts = 0;
    for (int p = 0; p < 2; ++p)
    {
        input.partOn[p] = shell::detail::partSelected (s.target, keyMode, p);
        parts |= input.partOn[p] ? (1 << p) : 0;
        input.keys[p].clear();
    }

    usingKeys = false;
    for (const auto& m : memory)
        for (auto k : m.current())
            usingKeys = usingKeys || k != 0xFF;

    if (usingKeys)
    {
        for (int p = 0; p < 2; ++p)
            input.keys[p] = memory[static_cast<size_t> (p)].asVector();
        sourceText = "your keys";
    }
    else
    {
        // An example chord pressed in this order, routed to the parts as the processor routes keys.
        KeyRouter router;
        router.setMode (keyMode);
        router.setSplitPoint (juce::roundToInt (ctx.mirror.parameter (splitIdx).convertFrom0to1 (ctx.mirror.value (splitIdx))));
        const std::array<NoteEvent, 3> chord { NoteEvent { 0, EventType::NoteOn, 60, input.velocity }, NoteEvent { 0, EventType::NoteOn, 64, input.velocity },
                                               NoteEvent { 0, EventType::NoteOn, 67, input.velocity } };
        std::array<NoteEvent, 3> lower {}, upper {};
        const auto counts = router.route (chord, lower, upper);
        for (int i = 0; i < counts.lower; ++i)
            input.keys[0].push_back (lower[static_cast<size_t> (i)].note);
        for (int i = 0; i < counts.upper; ++i)
            input.keys[1].push_back (upper[static_cast<size_t> (i)].note);
        sourceText = "example chord C E G";
    }
    previewModel.compute (input);

    std::vector<int> shown;
    for (int p = 0; p < 2; ++p)
        if (input.partOn[p])
            for (int k : input.keys[p])
                if (std::find (shown.begin(), shown.end(), k) == shown.end())
                    shown.push_back (k);
    roll.setPreview (previewModel.notes(), previewModel.beats(), shown, parts, sourceText);
    previewPanel.setText ("3 Preview: what will play (" + sourceText + ")");
}

void ArpSeqPage::paint (juce::Graphics& g)
{
    const auto mode = panelBounds (0).toFloat();
    g.setColour (colour (Tok::BgPanel));
    g.fillRoundedRectangle (mode, static_cast<float> (kRadius));
    g.setFont (ctx.theme.font (kText));
    g.setColour (colour (Tok::TextSecondary));
    drawFittedText (g, playsThrough, { 816.0f, 8.0f, 300.0f, 20.0f }, juce::Justification::centredLeft);
    g.setColour (colour (Tok::TextPrimary));
    drawFittedText (g, sentence, { 12.0f, 80.0f, static_cast<float> (kW - 24), 26.0f }, juce::Justification::centredLeft);
}

void ArpSeqPage::paintOverChildren (juce::Graphics& g)
{
    g.setFont (ctx.theme.font (kTextSmall));
    g.setColour (colour (Tok::TextSecondary));
    if (view == 0)
    {
        const auto r = panelBounds (3).translated (0, 0).toFloat();
        drawFittedText (g, legendRoll,
                        { r.getRight() - 560.0f, r.getBottom() - 30.0f, 540.0f, 16.0f }, juce::Justification::centredRight);
    }
    else
    {
        const auto r = panelBounds (4).toFloat();
        drawFittedText (g, legendRestTie,
                        { r.getX() + 12.0f + StepGrid::kLabelW, r.getY() + 36.0f + StepGrid::kHeight + 4.0f, 1000.0f, 16.0f }, juce::Justification::centredLeft);
        drawFittedText (g, legendBars,
                        { r.getX() + 12.0f + StepGrid::kLabelW, r.getY() + 36.0f + StepGrid::kHeight + 22.0f, 1000.0f, 16.0f }, juce::Justification::centredLeft);
    }
    if (! hostText.isEmpty())
    {
        const auto t = timingPanel.getBounds().translated (tempo.getX(), tempo.getY());
        drawFittedText (g, hostText, { static_cast<float> (t.getX()), static_cast<float> (t.getY() + tempo.getHeight()), 200.0f, 14.0f }, juce::Justification::centredLeft);
    }
}
} // namespace vs::ui
