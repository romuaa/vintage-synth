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
#include "vintage/dsp/OscScope.h"

#include <map>

namespace vs::ui
{
class SoundPage final : public juce::Component, private ParamMirror::Listener, private ParamMirror::IdleClient
{
public:
    SoundPage (UiContext& ctx, int part);
    ~SoundPage() override;

    /** The voice's real-time rate (UiFeed::internalRate): the curve is evaluated there. */
    void setSampleRate (double rate);
    /** Opens the Effects page (the signal path's last item). */
    std::function<void()> onOpenEffects;

    int getPart() const noexcept { return part; }
    const dsp::FilterCurveSettings& curveSettings() const noexcept { return curve; }
    const dsp::OscScopeSettings& scopeSettings() const noexcept { return scope; }
    const dsp::OscScopes& scopes() const noexcept { return scopeData; }
    ResponseCurveView& getCurveView() noexcept { return curveView; }
    ScopeView& getScope (int osc) noexcept { return osc == 0 ? scope1 : scope2; }
    /** The settings the picture should show for the mirror's current values (tests). */
    dsp::FilterCurveSettings expectedCurveSettings() const;
    dsp::OscScopeSettings expectedScopeSettings() const;
    /** How often each picture was computed (tests: once per tick at most, only when an input changed). */
    int curveComputations() const noexcept { return curveRecomputes; }
    int scopeComputations() const noexcept { return scopeRecomputes; }
    /** Ends every open gesture (editor closing). */
    void cancelInteraction();

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
    float live (const juce::String& suffix) const;   // the parameter's live normalised value
    float mirrored (const juce::String& suffix) const; // the mirror's value
    int index (const juce::String& suffix) const;
    juce::String id (const juce::String& suffix) const;
    juce::Point<float> cutoffHandleAt (float cutoff, const dsp::FilterCurveSettings& s) const;
    juce::Point<float> hpfHandleAt (const dsp::FilterCurveSettings& s) const;
    dsp::FilterCurveSettings curveFor (float cutoff, float resonance, float slope, float hpf) const;
    dsp::OscScopeSettings scopeFor (const std::function<float (const juce::String&)>& value) const;

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
    dsp::OscScopeSettings scope;
    dsp::OscScopes scopeData;
    std::vector<int> curveInputs, scopeInputs;
    bool curveDirty = false, scopesDirty = false;
    int curveRecomputes = 0, scopeRecomputes = 0;
    int crossModIndex = -1;
    std::map<juce::String, int> indices; // suffix -> mirror index, built once
};
} // namespace vs::ui
