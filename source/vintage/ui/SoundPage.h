#pragma once

// The Sound page of one part (docs/architecture.md 9.3.1): oscillators, link (sync, cross mod),
// mix, high-pass + filter with its response curve, amp with the part's signal path. 15 primary
// controls; the curve's handles are secondary bindings of cutoff, resonance and high-pass. The
// pictures (curve, oscilloscopes) are computed by the engine's own code (FilterResponse.h,
// OscScope.h) when one of their inputs changes.

#include "common/ui/controls/Panel.h"
#include "common/ui/controls/SegmentedSwitch.h"
#include "common/ui/controls/Slider.h"
#include "common/ui/graphs/GraphView.h"
#include "vintage/dsp/FilterResponse.h"
#include "vintage/engine/OscScope.h"

#include <map>

namespace vs::ui
{
class SoundPage final : public juce::Component,
                        public InteractiveContent,
                        private ParamMirror::Listener,
                        private ParamMirror::IdleClient
{
public:
    SoundPage (UiContext& ctx, int part);
    ~SoundPage() override;

    /** Feedback at and above which the filter self-oscillates (the curve shows a note there). */
    static constexpr double kSelfOscillationK = 4.0;
    /** The voice's real-time rate (UiFeed::internalRate): the curve is evaluated there. */
    void setSampleRate (double rate);
    /** Opens the Effects page (the signal path's last item). */
    std::function<void()> onOpenEffects;

    int getPart() const noexcept { return part; }
    const dsp::FilterCurveSettings& curveSettings() const noexcept { return curve; }
    const OscScopeSettings& scopeSettings() const noexcept { return scope; }
    const OscScopes& scopes() const noexcept { return scopeData; }
    ResponseCurveView& getCurveView() noexcept { return curveView; }
    ScopeView& getScope (int osc) noexcept { return osc == 0 ? scope1 : scope2; }
    /** The settings the picture should show for the mirror's current values (tests). */
    dsp::FilterCurveSettings expectedCurveSettings() const;
    OscScopeSettings expectedScopeSettings() const;
    /** This part's parameters as the mirror holds them, converted as the engine reads them. */
    PartParams mirroredParams() const;
    /** How often each picture was computed (tests: once per tick at most, only when an input changed). */
    int curveComputations() const noexcept { return curveRecomputes; }
    int scopeComputations() const noexcept { return scopeRecomputes; }
    /** Ends every open gesture (editor closing). */
    void cancelInteraction() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;

private:
    class SignalPath;

    void mirrorChanged (int index, bool valueChanged) override;
    void mirrorIdle (double nowMs) override; // after every notification of a tick: recompute once
    void recomputeCurve();
    void recomputeScopes();
    void placeHandles();
    juce::Rectangle<int> crossModCaptionArea() const;
    float live (const juce::String& suffix) const;   // the parameter's live normalised value
    float mirrored (const juce::String& suffix) const; // the mirror's value
    int index (const juce::String& suffix) const;
    juce::String id (const juce::String& suffix) const;
    juce::Point<float> cutoffHandleAt (float cutoff, const dsp::FilterCurveSettings& s) const;
    juce::Point<float> hpfHandleAt (const dsp::FilterCurveSettings& s) const;
    dsp::FilterCurveSettings curveFor (float cutoff, float resonance, float slope, float hpf) const;

    UiContext& ctx;
    int part;
    double sampleRate = 96000.0;

    Panel osc1Panel, linkPanel, osc2Panel, mixPanel, filterPanel, ampPanel;
    ScopeView scope1, scope2;
    SegmentedSwitch vco1Range, vco1Wave;
    Toggle sync;
    Slider crossMod;
    SegmentedSwitch vco2Range, vco2Mode;
    Slider fineTune;
    SegmentedSwitch vco2Wave;
    Slider sourceMix;
    SegmentedSwitch slope;
    Slider cutoff, resonance, hpf, keyFollow, level;
    ResponseCurveView curveView;
    std::unique_ptr<SignalPath> signalPath;
    GraphHandle* handleA = nullptr;
    GraphHandle* handleB = nullptr;

    dsp::FilterCurveSettings curve;
    OscScopeSettings scope;
    OscScopes scopeData;
    std::vector<int> partIndices; // mirror index of every per-part parameter, in PartParamList order
    std::vector<int> curveInputs, scopeInputs;
    bool curveDirty = false, scopesDirty = false;
    int curveRecomputes = 0, scopeRecomputes = 0;
    int crossModIndex = -1;
    std::map<juce::String, int> indices; // suffix -> mirror index, built once
    // Captions made once: a literal converted to a juce::String in paint() allocates (9.5).
    struct Captions
    {
        const juce::String link { juce::String::fromUTF8 ("2 \xe2\x86\x92 1") }, offAtZero { "off at 0" }, osc1 { "Osc 1" }, osc2 { "Osc 2" },
            toFilter { "goes to the filter >" }, steepness { "Steepness" },
            dragHint { "Drag the dots: cutoff left/right, resonance up/down; high-pass left/right. Dashed: the other steepness." },
            overTime { "How the filter moves over time: see the Envelopes and LFO pages." }, signalPath { "Signal path of this part" };
    } captions;
};
} // namespace vs::ui
