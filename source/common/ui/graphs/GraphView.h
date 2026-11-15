#pragma once

// Graphs (docs/architecture.md 9.2.2): GraphView, the base with draggable handles; the response
// curve (log frequency x dB) and the oscilloscope picture. A handle writes up to two parameters
// (x and y) whose primary controls are on the same page: they register as secondary bindings.
// Pictures are prepared when their inputs change (the page calls set...); paint() only draws.

#include "common/ui/controls/BoundComponent.h"
#include "common/ui/models/GraphModels.h"

#include <functional>
#include <memory>
#include <vector>

namespace vs::ui
{
class GraphHandle final : public juce::Component, public ParamMirror::IdleClient
{
public:
    /** `xParam` / `yParam` may be empty. `expected`: where the page's mapping puts the handle for
        the mirror's current values (the automation-follow test compares it with the drawn place). */
    GraphHandle (UiContext& ctx, const juce::String& name, int part, const juce::String& xParam, model::HandleModel::PxToValue xMap,
                 const juce::String& yParam, float yPxPerRange, std::function<juce::Point<float>()> expected);
    ~GraphHandle() override;

    /** Places the handle (centre, in the parent's coordinates). */
    void setCentre (juce::Point<float> c);
    juce::Point<float> getCentre() const noexcept { return centre; }
    juce::Point<float> expectedCentre() const { return expected ? expected() : centre; }
    model::HandleModel& getModel() noexcept { return model; }
    /** Called after the handle wrote values (the page redraws from the live values). */
    std::function<void()> onChange;
    void setAccent (juce::Colour c) { accentColour = c; repaint(); }
    void setMarkerAlpha (float a);
    float getMarkerAlpha() const noexcept { return markerAlpha; }
    void cancelInteraction() { model.cancel(); }
    /** "1.28 kHz, VCF Resonance 5.5": the parameters' own text (screen readers, tests). */
    juce::String valueText() const;
    /** Typed value for the handle's first parameter; false (nothing changes) if unreadable. */
    bool setValueFromText (const juce::String& text);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    void mirrorIdle (double nowMs) override { model.idle (nowMs); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    struct AxisBinding;
    void changed();
    void showContextMenu();
    void showTypedEntry();

    UiContext& ctx;
    std::unique_ptr<ParamHandle> xHandle, yHandle;
    std::vector<std::unique_ptr<AxisBinding>> bindings;
    model::HandleModel model;
    std::function<juce::Point<float>()> expected;
    juce::Point<float> centre;
    juce::Colour accentColour;
    float markerAlpha = 0.0f;
    std::unique_ptr<juce::TextEditor> entry;
};

/** Base of every graph: a panel-coloured plot area with handles as child components. */
class GraphView : public juce::Component
{
public:
    explicit GraphView (const Theme& t) : theme (t) { setWantsKeyboardFocus (false); }
    GraphHandle& addHandle (std::unique_ptr<GraphHandle> h);
    int numHandles() const noexcept { return static_cast<int> (handles.size()); }
    GraphHandle& getHandle (int i) noexcept { return *handles[static_cast<size_t> (i)]; }
    void cancelInteraction();
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

protected:
    const Theme& theme;
    std::vector<std::unique_ptr<GraphHandle>> handles;
};

/** Magnitude response on a log-frequency axis 20 Hz - 20 kHz and a dB axis -48 ... +24 dB. */
class ResponseCurveView final : public GraphView
{
public:
    using Magnitude = std::function<double (double hz)>;
    /** The frequency axis covers the high-pass's whole range (5 Hz) up to 20 kHz. */
    static constexpr double kAxisLowHz = 5.0, kAxisHighHz = 20000.0;

    explicit ResponseCurveView (const Theme& t);
    /** Recomputes the curves (main, and the dashed comparison curve, may be empty). */
    void setCurves (const Magnitude& main, const Magnitude& other);
    void setAccent (juce::Colour c);
    /** A dashed vertical marker (the high-pass corner), or NaN for none. */
    void setMarkerHz (double hz);
    /** A short note drawn at the top right of the plot (empty: none). */
    void setNote (const juce::String& text);
    const juce::String& getNote() const noexcept { return note; }
    const model::LogAxis& frequencyAxis() const noexcept { return xAxis; }
    const model::LinearAxis& dbAxis() const noexcept { return yAxis; }
    /** Plot area in local coordinates. */
    juce::Rectangle<float> plotArea() const noexcept { return plot; }
    /** Number of times the curves were recomputed (tests: derived pictures follow their inputs). */
    int recomputeCount() const noexcept { return recomputes; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void rebuild();

    Magnitude mainCurve, otherCurve;
    model::LogAxis xAxis;
    model::LinearAxis yAxis;
    juce::Rectangle<float> plot;
    juce::Path mainPath, fillPath, otherPath, otherDashed, markerDashed;
    juce::StringArray freqLabels, dbLabels; // prepared once: paint() builds no strings
    juce::Colour accentColour;
    double markerHz = std::numeric_limits<double>::quiet_NaN();
    juce::String note;
    int recomputes = 0;
};

/** One or two cycles of a waveform (display only). */
class ScopeView final : public juce::Component
{
public:
    explicit ScopeView (const Theme& t);
    void setSamples (const float* samples, int count, const juce::String& waveName);
    void setAccent (juce::Colour c);
    int recomputeCount() const noexcept { return recomputes; }
    void paint (juce::Graphics&) override;
    void resized() override { rebuild(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void rebuild();
    std::vector<float> data;
    juce::Path path;
    juce::Colour accentColour;
    int recomputes = 0;
};
} // namespace vs::ui
