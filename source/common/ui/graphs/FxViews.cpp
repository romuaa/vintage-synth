#include "common/ui/graphs/FxViews.h"

namespace vs::ui
{
namespace
{
using namespace metrics;

juce::Rectangle<float> plotOf (juce::Rectangle<float> bounds)
{
    // Inset by a handle's radius, so a handle at either end of an axis is drawn whole.
    return bounds.reduced (static_cast<float> (kHandleRadius) + 4.0f, 2.0f).withTrimmedBottom (18.0f);
}

juce::Colour pictureColour (bool on) { return on ? colour (Tok::AccentGlobal) : colour (Tok::TextDisabled); }
} // namespace

// ---- ChorusView ----

ChorusView::ChorusView (const Theme& t) : theme (t)
{
    setTitle ("Chorus picture");
}

void ChorusView::setState (int newMode, float newMix)
{
    newMode = juce::jlimit (0, 2, newMode);
    if (stateSet && newMode == mode && std::abs (newMix - mix) < 1.0e-6f)
        return;
    const bool modeChanged = newMode != mode || ! stateSet;
    stateSet = true;
    mode = newMode;
    mix = newMix;
    if (modeChanged)
    {
        const auto c = model::chorusPicture (mode);
        note = mode == 0 ? juce::String ("Chorus is off") : "Delay of the two copies: " + juce::String (c.rateHz, 2) + " Hz, +-" + juce::String (c.depthMs, 2) + " ms";
        setDescription (mode == 0 ? juce::String ("Chorus off") : "Chorus " + juce::String (mode == 1 ? "I" : "II") + ": " + note);
        rebuild();
    }
    repaint();
}

void ChorusView::resized()
{
    plot = getLocalBounds().toFloat().reduced (10.0f, 26.0f);
    rebuild();
}

float ChorusView::tapAtX (float x, bool anti) const
{
    const double sec = static_cast<double> ((x - plot.getX()) / plot.getWidth()) * kWindowSec;
    return model::chorusTapMs (model::chorusPicture (mode), sec, anti);
}

void ChorusView::rebuild()
{
    ++recomputes;
    pathA.clear();
    pathB.clear();
    if (plot.isEmpty())
        return;
    const auto pic = model::chorusPicture (mode);
    const auto yOf = [&] (float ms) {
        return plot.getBottom() - juce::jlimit (0.0f, 1.0f, ms / fx::Chorus::kMaxDelayMs) * plot.getHeight();
    };
    constexpr int points = 160;
    for (int i = 0; i <= points; ++i)
    {
        const float x = plot.getX() + plot.getWidth() * static_cast<float> (i) / static_cast<float> (points);
        const double sec = kWindowSec * static_cast<double> (i) / static_cast<double> (points);
        const float ya = yOf (model::chorusTapMs (pic, sec, false)), yb = yOf (model::chorusTapMs (pic, sec, true));
        if (i == 0)
        {
            pathA.startNewSubPath (x, ya);
            pathB.startNewSubPath (x, yb);
        }
        else
        {
            pathA.lineTo (x, ya);
            pathB.lineTo (x, yb);
        }
    }
    repaint();
}

void ChorusView::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (colour (Tok::BgWindow));
    g.fillRoundedRectangle (r, 4.0f);
    g.setFont (theme.font (kTextSmall));
    g.setColour (colour (Tok::TextSecondary));
    drawFittedText (g, note, r.withHeight (22.0f).reduced (10.0f, 4.0f), juce::Justification::centredLeft);
    // Axis: 0 ms = no delay (the dry sound, grey), 6 ms top.
    g.setColour (colour (Tok::BgTrack));
    g.fillRect (plot.getX(), plot.getBottom() - 1.0f, plot.getWidth(), 2.0f);
    g.setColour (colour (Tok::TextSecondary));
    g.drawText ("dry (0 ms)", juce::Rectangle<float> (plot.getX() + 2.0f, plot.getBottom() - 16.0f, 80.0f, 14.0f), juce::Justification::centredLeft, false);
    g.drawText ("6 ms", juce::Rectangle<float> (plot.getX() + 2.0f, plot.getY() - 2.0f, 60.0f, 14.0f), juce::Justification::centredLeft, false);
    g.drawText ("4 s", juce::Rectangle<float> (plot.getRight() - 40.0f, plot.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centredRight, false);
    if (mode != 0)
    {
        const float alpha = 0.35f + 0.65f * juce::jlimit (0.0f, 1.0f, mix);
        g.setColour (pictureColour (true).withAlpha (alpha));
        g.strokePath (pathA, juce::PathStrokeType (2.0f));
        g.setColour (pictureColour (true).withAlpha (alpha * 0.6f));
        g.strokePath (pathB, juce::PathStrokeType (2.0f));
    }
    else
    {
        g.setColour (pictureColour (false));
        g.strokePath (pathA, juce::PathStrokeType (2.0f));
    }
    g.setColour (colour (Tok::TextSecondary));
    g.drawText ("The wobble makes the sound wide and shimmering.", r.withTrimmedTop (r.getHeight() - 22.0f).reduced (10.0f, 2.0f),
                juce::Justification::centredLeft, true);
}

std::unique_ptr<juce::AccessibilityHandler> ChorusView::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::image);
}

// ---- DelayView ----

DelayView::DelayView (const Theme& t) : GraphView (t)
{
    setTitle ("Delay picture");
    tickLabels.add ("0.1 s");
    tickLabels.add ("0.5 s");
    tickLabels.add ("1 s");
    tickLabels.add ("2 s");
    tickLabels.add ("4 s");
}

void DelayView::resized()
{
    plot = plotOf (getLocalBounds().toFloat());
    laneH = juce::jmax (8.0f, (plot.getHeight() - 8.0f) * 0.5f);
    inputX = plot.getX() + 24.0f;
    axis = model::delayTimeAxis (plot.getX() + 24.0f, plot.getWidth() - 24.0f - 10.0f);
    pic = model::delayPicture (time, feedback, pingPong, axis.hi);
    ++recomputes;
    repaint();
}

void DelayView::setState (double timeSec, float feedbackGain, bool newPingPong, bool newOn, const juce::String& newTimeText)
{
    if (stateSet && std::abs (timeSec - time) < 1.0e-9 && std::abs (feedbackGain - feedback) < 1.0e-7f && newPingPong == pingPong && newOn == on
        && newTimeText == timeText)
        return;
    stateSet = true;
    time = timeSec;
    feedback = feedbackGain;
    pingPong = newPingPong;
    on = newOn;
    timeText = newTimeText;
    pic = model::delayPicture (time, feedback, pingPong, axis.hi);
    ++recomputes;
    setDescription ("Echoes every " + timeText + (pingPong ? ", bouncing left and right" : "") + (on ? "" : ". The delay is off."));
    repaint();
}

juce::Point<float> DelayView::handleAt (double timeSec, float feedbackGain, bool pingPongOn) const
{
    const float x = axis.clampPx (axis.positionOf (2.0 * timeSec));
    const float h = juce::jlimit (0.0f, 1.0f, feedbackGain);
    return { x, laneBottom (pingPongOn ? 1 : 0) - h * laneH };
}

double DelayView::timeAtHandleX (float px) const noexcept
{
    return axis.valueAt (axis.clampPx (px)) * 0.5;
}

void DelayView::paint (juce::Graphics& g)
{
    g.setColour (colour (Tok::BgWindow));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
    const auto accent = pictureColour (on);

    // Time ruler: 1 ms ... 1 s.
    g.setFont (theme.font (kTextSmall));
    static constexpr double ticks[] { 0.1, 0.5, 1.0, 2.0, 4.0 };
    for (int i = 0; i < 5; ++i)
    {
        const float x = axis.positionOf (ticks[i]);
        g.setColour (colour (Tok::BgTrack).withAlpha (0.5f));
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.setColour (colour (Tok::TextSecondary));
        g.drawText (tickLabels[i], juce::Rectangle<float> (x - 24.0f, plot.getBottom() + 2.0f, 48.0f, 14.0f), juce::Justification::centred, false);
    }

    // Two lanes, left on top, right below; the input is the grey bar at the start.
    for (int lane = 0; lane < 2; ++lane)
    {
        const float bottom = laneBottom (lane);
        g.setColour (colour (Tok::BgTrack));
        g.fillRect (plot.getX(), bottom - 1.0f, plot.getWidth(), 1.0f);
        g.setColour (colour (Tok::TextSecondary));
        g.drawText (lane == 0 ? "L" : "R", juce::Rectangle<float> (plot.getX() + 1.0f, bottom - laneH, 12.0f, 14.0f), juce::Justification::centred, false);
        g.setColour (colour (Tok::TextSecondary));
        g.fillRect (inputX - 3.0f, bottom - laneH, 6.0f, laneH);
    }
    g.setColour (colour (Tok::TextSecondary));
    g.drawText ("time", juce::Rectangle<float> (plot.getRight() - 40.0f, plot.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centredRight, false);
    g.drawText ("input", juce::Rectangle<float> (inputX + 6.0f, plot.getY() + 1.0f, 40.0f, 14.0f), juce::Justification::centredLeft, false);

    for (int i = 0; i < pic.count; ++i)
    {
        const auto& e = pic.echoes[static_cast<size_t> (i)];
        const float x = axis.positionOf (e.timeSec);
        const float h = juce::jmax (1.5f, e.height * laneH);
        g.setColour (accent.withAlpha (juce::jlimit (0.5f, 1.0f, 0.5f + e.height * 0.5f)));
        for (int lane = 0; lane < 2; ++lane)
            if (e.lane == 2 || e.lane == lane)
                g.fillRect (x - 4.0f, laneBottom (lane) - h, 8.0f, h);
    }

    g.setColour (colour (on ? Tok::TextSecondary : Tok::TextDisabled));
    g.setFont (theme.font (kTextSmall, true));
    const juce::String caption = on ? "echoes every " + timeText : "OFF: delay level is 0";
    g.drawText (caption, juce::Rectangle<float> (plot.getX() + 80.0f, plot.getY() + 1.0f, plot.getWidth() - 88.0f, 14.0f), juce::Justification::centredRight,
                true);
}

// ---- ReverbView ----

ReverbView::ReverbView (const Theme& t) : GraphView (t)
{
    setTitle ("Reverb picture");
    tickLabels.add ("0");
    tickLabels.add ("100 ms");
    tickLabels.add ("1 s");
    tickLabels.add ("10 s");
}

void ReverbView::resized()
{
    plot = plotOf (getLocalBounds().toFloat());
    axis = model::ReverbTimeAxis (plot.getX() + 16.0f, plot.getWidth() - 16.0f - 10.0f);
    ++recomputes;
    repaint();
}

void ReverbView::setState (float preDelaySec, float decaySec, bool newOn, const juce::String& newDecayText)
{
    const auto next = model::reverbPicture (preDelaySec, decaySec);
    if (stateSet && std::abs (next.preDelaySec - pic.preDelaySec) < 1.0e-9 && std::abs (next.endSec - pic.endSec) < 1.0e-9 && newOn == on
        && newDecayText == decayText)
        return;
    stateSet = true;
    pic = next;
    on = newOn;
    decayText = newDecayText;
    ++recomputes;
    setDescription ("The tail starts after " + juce::String (preDelaySec * 1000.0f, 0) + " ms and fades by 60 dB in " + decayText + (on ? "" : ". The reverb is off."));
    repaint();
}

juce::Point<float> ReverbView::preDelayHandleAt (float preDelaySec) const noexcept
{
    return { axis.axis.clampPx (axis.positionOf (static_cast<double> (preDelaySec))), topY() };
}

juce::Point<float> ReverbView::decayHandleAt (float preDelaySec, float decaySec) const noexcept
{
    return { axis.axis.clampPx (axis.positionOf (static_cast<double> (preDelaySec) + static_cast<double> (decaySec))), floorY() };
}

void ReverbView::paint (juce::Graphics& g)
{
    g.setColour (colour (Tok::BgWindow));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
    const auto accent = pictureColour (on);

    g.setFont (theme.font (kTextSmall));
    static constexpr double ticks[] { 0.0, 0.1, 1.0, 10.0 };
    for (int i = 0; i < 4; ++i)
    {
        const float x = axis.positionOf (ticks[i]);
        g.setColour (colour (Tok::BgTrack).withAlpha (0.5f));
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.setColour (colour (Tok::TextSecondary));
        g.drawText (tickLabels[i], juce::Rectangle<float> (x - 24.0f, plot.getBottom() + 2.0f, 48.0f, 14.0f), juce::Justification::centred, false);
    }
    g.setColour (colour (Tok::TextSecondary));
    g.drawText ("time", juce::Rectangle<float> (plot.getRight() - 40.0f, plot.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centredRight, false);

    // The input (grey spike), the -60 dB line, the tail: silent for the pre-delay, then a straight
    // line in dB reaching -60 dB at pre-delay + decay time.
    const float x0 = axis.positionOf (0.0), xs = axis.positionOf (pic.preDelaySec), xe = axis.positionOf (pic.endSec);
    g.setColour (colour (Tok::TextSecondary));
    g.fillRect (x0 - 1.5f, topY(), 3.0f, floorY() - topY());
    g.setColour (colour (Tok::BgTrack));
    g.fillRect (plot.getX(), floorY(), plot.getWidth(), 1.0f);
    g.setColour (colour (Tok::TextSecondary));
    g.drawText ("-60 dB", juce::Rectangle<float> (plot.getRight() - 52.0f, floorY() - 15.0f, 52.0f, 14.0f), juce::Justification::centredRight, false);
    g.drawText ("0 dB", juce::Rectangle<float> (plot.getRight() - 52.0f, plot.getY(), 52.0f, 14.0f), juce::Justification::centredRight, false);

    juce::Path wedge;
    wedge.startNewSubPath (xs, topY());
    wedge.lineTo (xe, floorY());
    wedge.lineTo (xs, floorY());
    wedge.closeSubPath();
    g.setColour (accent.withAlpha (0.22f));
    g.fillPath (wedge);
    g.setColour (accent);
    g.drawLine (xs, topY(), xe, floorY(), 2.0f);
    g.drawLine (x0, topY(), xs, topY(), 1.0f);

    g.setFont (theme.font (kTextSmall, true));
    g.setColour (colour (on ? Tok::TextSecondary : Tok::TextDisabled));
    const juce::String caption = on ? "fades by 60 dB in " + decayText : "OFF: reverb level is 0";
    g.drawText (caption, juce::Rectangle<float> (plot.getRight() - 260.0f, plot.getY() + 18.0f, 252.0f, 14.0f), juce::Justification::centredRight, true);
}
} // namespace vs::ui
