#include "common/ui/graphs/PianoRollView.h"

#include "common/params/ParamFactory.h" // noteName

#include <algorithm>

namespace vs::ui
{
namespace
{
bool isBlackKey (int note)
{
    const int pc = note % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
// Left strip: the octave labels (2 ... 46) and, right of them, the marks of the keys held (plot - 32 ... plot - 6).
constexpr float kLeft = 76.0f, kTop = 18.0f, kPad = 6.0f;
} // namespace

PianoRollView::PianoRollView (const Theme& t) : theme (t)
{
    setTitle ("Arpeggiator preview");
    setInterceptsMouseClicks (true, false);
}

void PianoRollView::setDimmed (bool dim)
{
    if (dimmed == dim)
        return;
    dimmed = dim;
    repaint();
}

void PianoRollView::setPreview (const std::vector<model::PreviewNote>& notes, double beats, const std::vector<int>& keys, int parts, const juce::String& source)
{
    notesIn = notes;
    keysIn = keys;
    beatsIn = beats;
    partsMask = parts;
    sourceText = source;

    // The text summary for screen readers: the distinct notes in the order they first play, and the steps.
    juce::String text;
    std::vector<int> seen;
    for (const auto& n : notes)
        if (std::find (seen.begin(), seen.end(), n.note) == seen.end() && seen.size() < 16)
        {
            seen.push_back (n.note);
            text << (text.isEmpty() ? "" : " ") << noteName (n.note);
        }
    int steps = 0; // steps of the first part that plays (both parts play the same steps)
    for (const auto& n : notes)
        steps += n.part == notes.front().part ? 1 : 0;
    summaryText = (text.isEmpty() ? juce::String ("no notes") : text) + ", " + juce::String (steps) + (steps == 1 ? " note" : " notes") + " in "
                  + juce::String (beats, 0) + " beats (" + source + ")";
    setDescription (summaryText);
    rebuild();
}

void PianoRollView::rebuild()
{
    ++recomputes;
    bars.clear();
    keyMarks.clear();
    rowLabels.clear();
    rowLabelNotes.clear();
    beatLabels.clear();

    plot = getLocalBounds().toFloat().withTrimmedLeft (kLeft).withTrimmedTop (kTop).reduced (kPad, kPad);
    if (plot.isEmpty())
        return;

    int lo = 60, hi = 72;
    if (! notesIn.empty() || ! keysIn.empty())
    {
        lo = 127;
        hi = 0;
        for (const auto& n : notesIn)
        {
            lo = std::min (lo, n.note);
            hi = std::max (hi, n.note);
        }
        for (int k : keysIn)
        {
            lo = std::min (lo, k);
            hi = std::max (hi, k);
        }
    }
    lo = std::max (0, lo - 2);
    hi = std::min (127, hi + 2);
    while (hi - lo < 24)
    {
        if (lo > 0)
            --lo;
        if (hi - lo < 24 && hi < 127)
            ++hi;
    }
    rowLo = lo;
    rowHi = hi;

    const float rowH = plot.getHeight() / static_cast<float> (rowHi - rowLo + 1);
    const float beatW = plot.getWidth() / static_cast<float> (beatsIn);
    for (const auto& n : notesIn)
    {
        const float x = plot.getX() + static_cast<float> (n.startBeat) * beatW;
        const float w = std::max (3.0f, static_cast<float> (n.lengthBeats) * beatW - 1.0f);
        const float y = rowY (n.note);
        // Both parts playing: each note is split in two halves (Lower above, Upper below), so neither hides the other.
        const float h = std::max (2.0f, rowH - 2.0f);
        const bool both = partsMask == 3;
        const float barH = both ? std::max (1.5f, h * 0.5f) : h;
        const float barY = y + 1.0f + (both && n.part == 1 ? h - barH : 0.0f);
        bars.push_back ({ { x, barY, std::min (w, plot.getRight() - x), barH }, n.note, n.part, n.velocity });
    }
    for (int k : keysIn)
        keyMarks.push_back ({ plot.getX() - 32.0f, rowY (k) + 1.0f, 26.0f, std::max (2.0f, rowH - 2.0f) });
    for (int n = rowLo; n <= rowHi; ++n)
        if (n % 12 == 0)
        {
            rowLabels.add (noteName (n));
            rowLabelNotes.push_back (n);
        }
    for (int b = 0; b < static_cast<int> (beatsIn); ++b)
        beatLabels.add ("beat " + juce::String (b + 1));
    repaint();
}

float PianoRollView::rowY (int note) const noexcept
{
    const float rowH = plot.getHeight() / static_cast<float> (rowHi - rowLo + 1);
    return plot.getY() + static_cast<float> (rowHi - note) * rowH;
}

void PianoRollView::paint (juce::Graphics& g)
{
    using namespace metrics;
    const float alpha = dimmed ? 0.4f : 1.0f;
    g.setColour (colour (Tok::BgWindow));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
    if (plot.isEmpty())
        return;

    const float rowH = plot.getHeight() / static_cast<float> (rowHi - rowLo + 1);
    for (int n = rowLo; n <= rowHi; ++n)
        if (isBlackKey (n))
        {
            g.setColour (colour (Tok::BgPanel).withAlpha (0.55f));
            g.fillRect (plot.getX(), rowY (n), plot.getWidth(), rowH);
        }
    const float beatW = plot.getWidth() / static_cast<float> (beatsIn);
    g.setFont (theme.font (kTextSmall));
    for (int b = 0; b < beatLabels.size(); ++b)
    {
        const float x = plot.getX() + static_cast<float> (b) * beatW;
        g.setColour (colour (Tok::BgTrack).withAlpha (0.9f));
        g.drawVerticalLine (static_cast<int> (x), plot.getY(), plot.getBottom());
        g.setColour (colour (Tok::TextSecondary).withAlpha (alpha));
        drawFittedText (g, beatLabels[b], { x + 4.0f, 2.0f, beatW - 6.0f, kTop - 4.0f }, juce::Justification::centredLeft);
    }
    for (int i = 0; i < rowLabels.size(); ++i)
    {
        const float y = rowY (rowLabelNotes[static_cast<size_t> (i)]);
        g.setColour (colour (Tok::BgTrack).withAlpha (0.9f));
        g.drawHorizontalLine (static_cast<int> (y + rowH * 0.5f), plot.getX(), plot.getRight());
        g.setColour (colour (Tok::TextSecondary).withAlpha (alpha));
        drawFittedText (g, rowLabels[i], { 2.0f, y + rowH * 0.5f - 8.0f, kLabelRight - 2.0f, 16.0f }, juce::Justification::centredLeft);
    }
    // The keys the pattern is made from, at the left.
    g.setColour (accentFor (partsMask == 2 ? 1 : (partsMask == 3 ? -1 : 0)).withAlpha (alpha));
    for (const auto& k : keyMarks)
        g.fillRoundedRectangle (k, 2.0f);
    for (const auto& b : bars)
    {
        const auto acc = accentFor ((partsMask == 3) ? b.part : (partsMask == 2 ? 1 : 0));
        g.setColour (acc.withAlpha (alpha * (0.55f + 0.45f * std::clamp (b.velocity, 0.0f, 1.0f))));
        g.fillRoundedRectangle (b.rect, 2.0f);
    }
    g.setColour (colour (Tok::BgTrack));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
}

std::unique_ptr<juce::AccessibilityHandler> PianoRollView::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::image);
}
} // namespace vs::ui
