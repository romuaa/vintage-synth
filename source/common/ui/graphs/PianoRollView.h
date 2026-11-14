#pragma once

// PianoRollView (docs/architecture.md 9.2.2, 9.3.5): note bars over beats, display only. The page
// hands it the notes the engine's own arpeggiator plays (models/ArpPreviewModel.h); everything
// drawn is prepared in setPreview() / resized(), paint() only draws.

#include "common/ui/Theme.h"
#include "common/ui/models/ArpPreviewModel.h"

#include <vector>

namespace vs::ui
{
class PianoRollView final : public juce::Component
{
public:
    explicit PianoRollView (const Theme& t);

    /** `notes`: what plays over `beats` beats; `keys`: the keys it is made from (marked at the
        left); `parts`: bit 0 Lower, bit 1 Upper (the colours); `source`: where the keys come from. */
    void setPreview (const std::vector<model::PreviewNote>& notes, double beats, const std::vector<int>& keys, int parts, const juce::String& source);
    void setDimmed (bool dim);

    const juce::String& summary() const noexcept { return summaryText; }
    const juce::String& source() const noexcept { return sourceText; }
    int numBars() const noexcept { return static_cast<int> (bars.size()); }
    /** Bar `i` (in the order of the notes given) in local coordinates; the note it shows. */
    juce::Rectangle<float> barBounds (int i) const noexcept { return bars[static_cast<size_t> (i)].rect; }
    int barNote (int i) const noexcept { return bars[static_cast<size_t> (i)].note; }
    /** The octave labels fill x = 2 ... kLabelRight; the marks of the held keys sit to the right of that. */
    static constexpr float kLabelRight = 46.0f;
    int numKeyMarks() const noexcept { return static_cast<int> (keyMarks.size()); }
    juce::Rectangle<float> keyMarkBounds (int i) const noexcept { return keyMarks[static_cast<size_t> (i)]; }
    int lowestRow() const noexcept { return rowLo; }
    int highestRow() const noexcept { return rowHi; }
    /** How often the picture was rebuilt (tests: only when an input changed). */
    int recomputeCount() const noexcept { return recomputes; }

    void paint (juce::Graphics&) override;
    void resized() override { rebuild(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    struct Bar
    {
        juce::Rectangle<float> rect;
        int note, part;
        float velocity;
    };
    void rebuild();
    float rowY (int note) const noexcept;

    const Theme& theme;
    std::vector<model::PreviewNote> notesIn;
    std::vector<int> keysIn;
    double beatsIn = 4.0;
    int partsMask = 1;
    juce::String sourceText, summaryText;
    bool dimmed = false;

    juce::Rectangle<float> plot;
    int rowLo = 48, rowHi = 72;
    std::vector<Bar> bars;
    std::vector<juce::Rectangle<float>> keyMarks;
    juce::StringArray rowLabels, beatLabels;
    std::vector<int> rowLabelNotes;
    int recomputes = 0;
};
} // namespace vs::ui
