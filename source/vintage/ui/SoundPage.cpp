#include "vintage/ui/SoundPage.h"

#include "vintage/params/PartParamList.h"

namespace vs::ui
{
namespace
{
using namespace metrics;

juce::String prefix (int part) { return part == 0 ? kLowerPrefix : kUpperPrefix; }
} // namespace

/** The part's signal path (static list); its last item opens the Effects page. */
class SoundPage::SignalPath final : public juce::Component
{
public:
    SignalPath (const Theme& t, int p, std::function<void()> openEffects) : theme (t), part (p)
    {
        setTitle ("Signal path of this part");
        effects.setButtonText ("Effects > Out");
        effects.setTitle ("Open the Effects page");
        effects.onClick = std::move (openEffects);
        addAndMakeVisible (effects);
    }
    void resized() override { effects.setBounds (itemBounds (5).toNearestInt()); }
    void paint (juce::Graphics& g) override
    {
        for (int i = 0; i < 5; ++i)
        {
            const auto r = itemBounds (i);
            const bool self = i == 4;
            g.setColour (self ? accentFor (part) : colour (Tok::BgWindow));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (colour (Tok::BgTrack));
            g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
            g.setColour (self ? colour (Tok::TextOnAccent) : colour (Tok::TextPrimary));
            g.setFont (self ? theme.fonts().textBold : theme.fonts().text);
            g.drawText (items[i], r, juce::Justification::centred, false);
            g.setColour (colour (Tok::TextSecondary));
            g.drawText (arrow, r.withY (r.getBottom()).withHeight (kStep - r.getHeight()), juce::Justification::centred, false);
        }
    }

private:
    static constexpr float kItem = 34.0f, kStep = 52.0f;
    juce::Rectangle<float> itemBounds (int i) const
    {
        return { 0.0f, static_cast<float> (i) * kStep, static_cast<float> (getWidth()), kItem };
    }
    const Theme& theme;
    int part;
    juce::TextButton effects;
    // Text made once: a literal converted to a juce::String in paint() allocates (9.5).
    const juce::String items[5] { "Osc 1 + Osc 2", "Mix", "High-pass", "Filter", "Amp (this)" }, arrow { "v" };
};

SoundPage::SoundPage (UiContext& c, int p)
    : ctx (c),
      part (p),
      osc1Panel (c.theme, "Oscillator 1", p),
      linkPanel (c.theme, "Link", p),
      osc2Panel (c.theme, "Oscillator 2", p),
      mixPanel (c.theme, "Mix", p),
      filterPanel (c.theme, "High-pass + filter", p),
      ampPanel (c.theme, "Amp", p),
      scope1 (c.theme),
      scope2 (c.theme),
      vco1Range (c, prefix (p) + "vco1.range", "Octave (feet)", p),
      vco1Wave (c, prefix (p) + "vco1.waveform", "Waveform", p),
      sync (c, prefix (p) + "vco2.sync", "Sync", p),
      crossMod (c, prefix (p) + "vco1.cross_mod", "Cross mod", p, Slider::Style::Vertical),
      vco2Range (c, prefix (p) + "vco2.range", "Octave (feet)", p),
      vco2Mode (c, prefix (p) + "vco2.mode", "Pitch mode", p, { "Normal", "Low freq" }),
      fineTune (c, prefix (p) + "vco2.fine_tune", "Fine tune", p, Slider::Style::Horizontal, true),
      vco2Wave (c, prefix (p) + "vco2.waveform", "Waveform", p),
      sourceMix (c, prefix (p) + "mixer.source_mix", juce::String::fromUTF8 ("Osc 1 \xe2\x86\x94 Osc 2"), p),
      slope (c, prefix (p) + "vcf.slope", {}, p),
      cutoff (c, prefix (p) + "vcf.cutoff", "Cutoff", p),
      resonance (c, prefix (p) + "vcf.resonance", "Resonance", p),
      hpf (c, prefix (p) + "hpf.cutoff", "High-pass", p),
      keyFollow (c, prefix (p) + "vcf.key_follow", "Key follow", p),
      level (c, prefix (p) + "vca.level", "Level", p, Slider::Style::Vertical),
      curveView (c.theme)
{
    setTitle (juce::String ("Sound, ") + (p == 0 ? "Lower" : "Upper") + " part");
    setComponentID (juce::String ("sound.") + (p == 0 ? "lower" : "upper"));

    for (auto* panel : { &osc1Panel, &linkPanel, &osc2Panel, &mixPanel, &filterPanel, &ampPanel })
        addAndMakeVisible (*panel);
    osc1Panel.addAndMakeVisible (scope1);
    osc1Panel.addAndMakeVisible (vco1Range);
    osc1Panel.addAndMakeVisible (vco1Wave);
    linkPanel.addAndMakeVisible (sync);
    linkPanel.addAndMakeVisible (crossMod);
    osc2Panel.addAndMakeVisible (scope2);
    osc2Panel.addAndMakeVisible (vco2Range);
    osc2Panel.addAndMakeVisible (vco2Mode);
    osc2Panel.addAndMakeVisible (fineTune);
    osc2Panel.addAndMakeVisible (vco2Wave);
    mixPanel.addAndMakeVisible (sourceMix);
    filterPanel.addAndMakeVisible (slope);
    filterPanel.addAndMakeVisible (curveView);
    for (auto* s : { &cutoff, &resonance, &hpf, &keyFollow })
        filterPanel.addAndMakeVisible (*s);
    ampPanel.addAndMakeVisible (level);
    signalPath = std::make_unique<SignalPath> (c.theme, p, [this] {
        if (onOpenEffects)
            onOpenEffects();
    });
    ampPanel.addAndMakeVisible (*signalPath);
    scope1.setAccent (accentFor (p));
    scope2.setAccent (accentFor (p));
    curveView.setAccent (accentFor (p));
    slope.setTitle (partPrefix (p) + "Filter steepness");
    slope.getProperties().set (helpLabelProperty(), "Filter steepness");

    // Filter graph handles (9.3.1): A on the curve at the cutoff (x cutoff, y resonance), B at the
    // high-pass corner (x high-pass). Secondary bindings: their primaries are the sliders above.
    const auto cutoffFromPx = [this] (float px) {
        const double hz = curveView.frequencyAxis().valueAt (curveView.frequencyAxis().clampPx (px));
        return model::clamp01 (map::cutoffFromHz (static_cast<float> (hz)));
    };
    const auto hpfFromPx = [this] (float px) {
        const double hz = std::clamp (curveView.frequencyAxis().valueAt (px), 5.0, 2560.0);
        return model::clamp01 (map::hpfFromHz (static_cast<float> (hz)));
    };
    handleA = &curveView.addHandle (std::make_unique<GraphHandle> (
        c, "Filter cutoff", p, id ("vcf.cutoff"), cutoffFromPx, id ("vcf.resonance"), 200.0f, [this] {
            const auto s = curveFor (mirrored ("vcf.cutoff"), mirrored ("vcf.resonance"), mirrored ("vcf.slope"), mirrored ("hpf.cutoff"));
            return cutoffHandleAt (mirrored ("vcf.cutoff"), s);
        }));
    handleB = &curveView.addHandle (std::make_unique<GraphHandle> (
        c, "High-pass", p, id ("hpf.cutoff"), hpfFromPx, juce::String(), 200.0f, [this] {
            return hpfHandleAt (curveFor (mirrored ("vcf.cutoff"), mirrored ("vcf.resonance"), mirrored ("vcf.slope"), mirrored ("hpf.cutoff")));
        }));
    for (auto* h : { handleA, handleB })
        h->onChange = [this] {
            // The dragging handle redraws from the values it just wrote; everything else follows
            // at the next tick.
            curve = curveFor (live ("vcf.cutoff"), live ("vcf.resonance"), live ("vcf.slope"), live ("hpf.cutoff"));
            recomputeCurve();
        };

#define VS_INDEX_FLOAT(member, suffix, name, mn, mx, def, kind) partIndices.push_back (ctx.mirror.indexOf (id (suffix)));
#define VS_INDEX_CHOICE(member, suffix, name, labels, def) partIndices.push_back (ctx.mirror.indexOf (id (suffix)));
    VS_PART_FLOAT_PARAMS (VS_INDEX_FLOAT)
    VS_PART_CHOICE_PARAMS (VS_INDEX_CHOICE)
#undef VS_INDEX_FLOAT
#undef VS_INDEX_CHOICE
    for (const auto* s : { "vcf.cutoff", "vcf.resonance", "vcf.slope", "hpf.cutoff", "vco1.waveform", "vco2.waveform", "vco1.range", "vco2.range",
                           "vco2.mode", "vco2.fine_tune", "vco2.sync", "vcomod.pwm", "vcomod.pwm_source", "vco1.cross_mod" })
        indices[s] = ctx.mirror.indexOf (id (s));
    for (const auto* s : { "vcf.cutoff", "vcf.resonance", "vcf.slope", "hpf.cutoff" })
        curveInputs.push_back (index (s));
    for (const auto* s : { "vco1.waveform", "vco2.waveform", "vco1.range", "vco2.range", "vco2.mode", "vco2.fine_tune", "vco2.sync",
                           "vcomod.pwm", "vcomod.pwm_source", "env1.polarity", "vco1.cross_mod" })
        scopeInputs.push_back (index (s));
    crossModIndex = index ("vco1.cross_mod");
    for (int i : curveInputs)
        ctx.mirror.subscribe (i, this);
    for (int i : scopeInputs)
        ctx.mirror.subscribe (i, this);
    ctx.mirror.addIdleClient (this);

    setSize (kDesignWidth - kRail, kDesignHeight - kTopBar - kHelpLine);
    curve = expectedCurveSettings();
    scope = expectedScopeSettings();
    recomputeCurve();
    recomputeScopes();
}

SoundPage::~SoundPage()
{
    cancelInteraction();
    ctx.mirror.unsubscribe (this);
    ctx.mirror.removeIdleClient (this);
}

void SoundPage::cancelInteraction()
{
    curveView.cancelInteraction();
    for (auto* b : std::initializer_list<BoundComponent*> { &vco1Range, &vco1Wave, &sync, &crossMod, &vco2Range, &vco2Mode, &fineTune, &vco2Wave,
                                                             &sourceMix, &slope, &cutoff, &resonance, &hpf, &keyFollow, &level })
        b->cancelInteraction();
}

juce::String SoundPage::id (const juce::String& suffix) const { return prefix (part) + suffix; }
int SoundPage::index (const juce::String& suffix) const
{
    // Looked up once per suffix (the constructor fills the cache); the pictures read values often.
    if (const auto it = indices.find (suffix); it != indices.end())
        return it->second;
    return ctx.mirror.indexOf (id (suffix));
}
float SoundPage::mirrored (const juce::String& suffix) const { return ctx.mirror.value (index (suffix)); }
float SoundPage::live (const juce::String& suffix) const { return ctx.mirror.parameter (index (suffix)).getValue(); }

void SoundPage::setSampleRate (double rate)
{
    if (rate > 1000.0 && std::abs (rate - sampleRate) > 0.5)
    {
        sampleRate = rate;
        curve = expectedCurveSettings();
        recomputeCurve();
    }
}

dsp::FilterCurveSettings SoundPage::curveFor (float cutoffV, float resonanceV, float slopeV, float hpfV) const
{
    return dsp::filterCurveSettings (cutoffV, resonanceV, model::stepIndex (slopeV, 2), hpfV, sampleRate);
}

dsp::FilterCurveSettings SoundPage::expectedCurveSettings() const
{
    return curveFor (mirrored ("vcf.cutoff"), mirrored ("vcf.resonance"), mirrored ("vcf.slope"), mirrored ("hpf.cutoff"));
}

PartParams SoundPage::mirroredParams() const
{
    // Every per-part parameter, normalised -> plain exactly as the processor's binding reads it
    // (the parameter's own convertFrom0to1; choices to their index).
    PartParams p;
    size_t k = 0;
    const auto plain = [&] () { const int i = partIndices[k++]; return ctx.mirror.parameter (i).convertFrom0to1 (ctx.mirror.value (i)); };
#define VS_READ_FLOAT(member, id, name, mn, mx, def, kind) p.member = plain();
#define VS_READ_CHOICE(member, id, name, labels, def) p.member = static_cast<int> (std::lround (plain()));
    VS_PART_FLOAT_PARAMS (VS_READ_FLOAT)
    VS_PART_CHOICE_PARAMS (VS_READ_CHOICE)
#undef VS_READ_FLOAT
#undef VS_READ_CHOICE
    return p;
}

OscScopeSettings SoundPage::expectedScopeSettings() const
{
    return oscScopeSettingsFor (mirroredParams()); // the voice's own mapping (OscPath.h)
}

void SoundPage::mirrorChanged (int changedIndex, bool valueChanged)
{
    if (! valueChanged)
        return;
    // Pictures are recomputed once at the end of the tick (mirrorIdle), however many inputs changed.
    if (std::find (curveInputs.begin(), curveInputs.end(), changedIndex) != curveInputs.end())
        curveDirty = true;
    if (std::find (scopeInputs.begin(), scopeInputs.end(), changedIndex) != scopeInputs.end())
        scopesDirty = true;
    if (changedIndex == crossModIndex)
        repaint (crossModCaptionArea()); // only the "off at 0" caption
}

void SoundPage::mirrorIdle (double)
{
    if (curveDirty)
    {
        curveDirty = false;
        const auto s = expectedCurveSettings();
        if (! (s == curve))
        {
            curve = s;
            recomputeCurve();
        }
        else
            placeHandles(); // same picture; the handles still follow (e.g. a clamped cutoff)
    }
    if (scopesDirty)
    {
        scopesDirty = false;
        const auto s = expectedScopeSettings();
        if (! (s == scope))
        {
            scope = s;
            recomputeScopes();
        }
    }
}

void SoundPage::recomputeCurve()
{
    ++curveRecomputes;
    const auto main = curve;
    auto other = curve;
    other.slope24 = ! curve.slope24;
    curveView.setCurves ([main] (double hz) { return dsp::filterMagnitudeDb (hz, main); },
                         [other] (double hz) { return dsp::filterMagnitudeDb (hz, other); });
    curveView.setMarkerHz (curve.hpfHz);
    // At k >= 4 the ladder oscillates by itself: its small-signal response has no finite peak.
    // The curve is clamped to the top of the axis and says so.
    curveView.setNote (curve.k >= kSelfOscillationK ? juce::String ("Rings by itself at the cutoff (self-oscillation)") : juce::String());
    placeHandles();
}

juce::Point<float> SoundPage::cutoffHandleAt (float cutoffV, const dsp::FilterCurveSettings& s) const
{
    const auto& xa = curveView.frequencyAxis();
    const auto& ya = curveView.dbAxis();
    const auto plot = curveView.plotArea();
    const double hz = std::clamp (static_cast<double> (map::cutoffHz (cutoffV)), xa.lo, xa.hi);
    const float x = xa.clampPx (xa.positionOf (hz));
    const float y = std::clamp (ya.positionOf (dsp::filterMagnitudeDb (std::min (hz, 0.49 * s.sampleRate), s)), plot.getY(), plot.getBottom());
    return { x, y };
}

juce::Point<float> SoundPage::hpfHandleAt (const dsp::FilterCurveSettings& s) const
{
    const auto& xa = curveView.frequencyAxis();
    const auto& ya = curveView.dbAxis();
    const auto plot = curveView.plotArea();
    const double hz = std::clamp (s.hpfHz, xa.lo, xa.hi);
    const float x = xa.clampPx (xa.positionOf (hz));
    const float y = std::clamp (ya.positionOf (dsp::filterMagnitudeDb (hz, s)), plot.getY(), plot.getBottom());
    return { x, y };
}

void SoundPage::placeHandles()
{
    if (handleA == nullptr || curveView.plotArea().isEmpty())
        return;
    // While a handle is dragged it sits where its own values put it; otherwise where the mirror's do.
    const bool dragging = handleA->getModel().isDragging() || handleB->getModel().isDragging();
    const float cutoffV = dragging ? live ("vcf.cutoff") : mirrored ("vcf.cutoff");
    handleA->setCentre (cutoffHandleAt (cutoffV, curve));
    handleB->setCentre (hpfHandleAt (curve));
}

void SoundPage::recomputeScopes()
{
    ++scopeRecomputes;
    renderOscScopes (scope, scopeData);
    const auto waveName = [] (int w) { return juce::String (kVcoWaveLabels[static_cast<size_t> (std::clamp (w, 0, 5))]); };
    scope1.setSamples (scopeData.osc1.data(), OscScopes::kPoints, waveName (scope.config.wave1));
    scope2.setSamples (scopeData.osc2.data(), OscScopes::kPoints, waveName (scope.config.wave2));
}

void SoundPage::resized()
{
    // Grid layout in logical px (9.2.3): row 1 oscillators, link and mix; row 2 filter and amp.
    osc1Panel.setBounds (12, 12, 470, 250);
    linkPanel.setBounds (494, 12, 76, 250);
    osc2Panel.setBounds (582, 12, 470, 250);
    mixPanel.setBounds (1064, 12, 172, 250);
    filterPanel.setBounds (12, 274, 812, 426);
    ampPanel.setBounds (836, 274, 400, 426);

    scope1.setBounds (12, 36, 210, 124);
    vco1Range.setBounds (234, 36, 224, kLabelRow + kSegment);
    vco1Wave.setBounds (12, 176, 446, kLabelRow + kSegment);

    sync.setBounds (8, 36, 60, kSegment);
    crossMod.setBounds (8, 96, 60, 146);

    scope2.setBounds (12, 36, 210, 124);
    vco2Range.setBounds (234, 36, 224, kLabelRow + kSegment);
    vco2Mode.setBounds (234, 86, 224, kLabelRow + kSegment);
    fineTune.setBounds (234, 134, 224, 36);
    vco2Wave.setBounds (12, 176, 446, kLabelRow + kSegment);

    sourceMix.setValueSlotWidth (36.0f); // values 0.0 ... 10.0: room for the label
    sourceMix.setBounds (12, 40, 148, 40);

    slope.setBounds (588, 2, 212, 24);
    curveView.setBounds (12, 36, 788, 300);
    cutoff.setBounds (12, 346, 185, 40);
    resonance.setBounds (209, 346, 185, 40);
    hpf.setBounds (406, 346, 185, 40);
    keyFollow.setBounds (603, 346, 197, 40);

    level.setBounds (16, 36, 80, 340);
    signalPath->setBounds (120, 40, 268, 300);
    placeHandles();
}

juce::Rectangle<int> SoundPage::crossModCaptionArea() const
{
    const auto link = linkPanel.getBounds();
    return { link.getX(), link.getY() + 80, link.getWidth(), 14 };
}

void SoundPage::paint (juce::Graphics& g)
{
    g.fillAll (colour (Tok::BgWindow));
}

void SoundPage::paintOverChildren (juce::Graphics& g)
{
    // Captions that belong to the panels (grid positions; never measured).
    const auto& theme = ctx.theme;
    g.setFont (theme.fonts().small);
    g.setColour (colour (Tok::TextSecondary));
    const auto link = linkPanel.getBounds().toFloat();
    g.drawText (captions.link, juce::Rectangle<float> (link.getX(), link.getY() + 66.0f, link.getWidth(), 14.0f),
                juce::Justification::centred, false);
    if (model::stepIndex (ctx.mirror.value (crossModIndex), 1001) == 0)
        g.drawText (captions.offAtZero, crossModCaptionArea().toFloat(), juce::Justification::centred, false);

    const auto mix = mixPanel.getBounds().toFloat();
    g.drawText (captions.osc1, juce::Rectangle<float> (mix.getX() + 12.0f, mix.getY() + 84.0f, 60.0f, 14.0f), juce::Justification::centredLeft, false);
    g.drawText (captions.osc2, juce::Rectangle<float> (mix.getRight() - 72.0f, mix.getY() + 84.0f, 60.0f, 14.0f), juce::Justification::centredRight, false);
    g.drawText (captions.toFilter, juce::Rectangle<float> (mix.getX() + 12.0f, mix.getY() + 120.0f, 148.0f, 14.0f), juce::Justification::centredLeft,
                false);

    const auto filter = filterPanel.getBounds().toFloat();
    g.setFont (theme.fonts().text);
    g.drawText (captions.steepness, juce::Rectangle<float> (filter.getX() + 480.0f, filter.getY() + 2.0f, 100.0f, 24.0f), juce::Justification::centredRight,
                false);
    g.setFont (theme.fonts().small);
    g.drawText (captions.dragHint,
                juce::Rectangle<float> (filter.getX() + 12.0f, filter.getY() + 392.0f, 788.0f, 14.0f), juce::Justification::centredLeft, true);
    g.drawText (captions.overTime,
                juce::Rectangle<float> (filter.getX() + 12.0f, filter.getY() + 406.0f, 788.0f, 14.0f), juce::Justification::centredLeft, true);

    const auto amp = ampPanel.getBounds().toFloat();
    g.drawText (captions.signalPath, juce::Rectangle<float> (amp.getX() + 120.0f, amp.getBottom() - 30.0f, 268.0f, 14.0f),
                juce::Justification::centredLeft, false);
}
} // namespace vs::ui
