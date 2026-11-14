#include "common/ui/StepGrid.h"

#include "common/params/FxArpParams.h"

namespace vs::ui
{
namespace
{
using model::StepGridModel;

double now() { return juce::Time::getMillisecondCounterHiRes(); }
bool fineModifier (const juce::ModifierKeys& m) { return m.isShiftDown() || m.isCommandDown(); }

std::array<ParamTarget*, StepGridModel::kNumTargets> targetsOf (std::vector<std::unique_ptr<ParamHandle>>& h)
{
    std::array<ParamTarget*, StepGridModel::kNumTargets> t {};
    for (size_t i = 0; i < t.size(); ++i)
        t[i] = h[i].get();
    return t;
}

/** Screen readers: the parameter's own text as the value. */
class BarValue final : public juce::AccessibilityTextValueInterface
{
public:
    BarValue (BoundComponent& c, ParamHandle& h) : comp (c), handle (h) {}
    bool isReadOnly() const override { return false; }
    juce::String getCurrentValueAsString() const override { return comp.shownText(); }
    void setValueAsString (const juce::String& t) override
    {
        float v = 0.0f;
        if (handle.parse (t, v))
            model::writeOnce (handle, v);
    }

private:
    BoundComponent& comp;
    ParamHandle& handle;
};

model::Key keyOf (int code, bool& known)
{
    known = true;
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey)
        return model::Key::Increase;
    if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey)
        return model::Key::Decrease;
    if (code == juce::KeyPress::pageUpKey)
        return model::Key::PageUp;
    if (code == juce::KeyPress::pageDownKey)
        return model::Key::PageDown;
    if (code == juce::KeyPress::homeKey)
        return model::Key::Home;
    if (code == juce::KeyPress::endKey)
        return model::Key::End;
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
        return model::Key::Reset;
    known = false;
    return model::Key::Reset;
}
} // namespace

// ---- One pitch or velocity bar ----
class StepGrid::Bar final : public BoundComponent
{
public:
    Bar (UiContext& c, StepGrid& g, int stepIndex, StepGridModel::Field f)
        : BoundComponent (c, seqStepParamID (stepIndex + 1, f == StepGridModel::Note ? SeqStepField::Note : SeqStepField::Velocity),
                          "Step " + juce::String (stepIndex + 1) + (f == StepGridModel::Note ? " pitch" : " velocity"), -1, "StepBar"),
          grid (g),
          step (stepIndex),
          field (f),
          keys (handle)
    {
        refresh();
    }
    ~Bar() override { keys.cancel(); }

    int stepIndex() const noexcept { return step; }
    bool isPitch() const noexcept { return field == StepGridModel::Note; }
    int targetIndex() const noexcept { return StepGridModel::indexOf (step, field); }
    /** Re-reads what the bar draws (after the model wrote a value). */
    void update() { refresh(); }
    /** The semitones (pitch bar) as drawn now. */
    int drawnSemitones() const { return StepGridModel::semitonesOfNormalised (drawnValue()); }
    /** Where the bar's value sits, in the grid's coordinates (the top of a velocity fill, the
        centre line of the pitch block). */
    float valueY() const
    {
        return isPitch() ? grid.getModel().pitchY (drawnSemitones()) : grid.getModel().velocityY (drawnValue());
    }

    float shownValue() const override
    {
        return grid.getModel().isInGesture (targetIndex()) || keys.isInGesture() ? handle.value() : BoundComponent::shownValue();
    }
    void cancelInteraction() override { keys.cancel(); }

    void paint (juce::Graphics& g) override
    {
        using namespace metrics;
        const bool dim = grid.stepIsDimmed (step);
        const int type = grid.mirroredType (step);
        const float alpha = (dim ? 0.4f : 1.0f) * (grid.isOff() ? 0.5f : 1.0f);
        auto cell = getLocalBounds().toFloat();
        if (isPitch())
        {
            g.setColour (colour (Tok::TextSecondary).withAlpha (alpha));
            g.setFont (ctx.theme.font (kTextSmall, true));
            drawFittedText (g, pitchLabel, cell.removeFromTop (static_cast<float> (kValueRow)), juce::Justification::centred);
        }
        g.setColour (colour (Tok::BgWindow));
        g.fillRoundedRectangle (cell, 4.0f);
        g.setColour (colour (Tok::BgTrack).withAlpha (alpha));
        g.drawRoundedRectangle (cell.reduced (0.5f), 4.0f, 1.0f);

        if (isPitch())
        {
            const float mid = cell.getCentreY();
            g.setColour (colour (Tok::BgTrack).withAlpha (0.8f * alpha));
            g.drawHorizontalLine (juce::roundToInt (mid), cell.getX() + 3.0f, cell.getRight() - 3.0f);
            if (type == 1)
            {
                g.setColour (colour (Tok::TextSecondary).withAlpha (alpha));
                g.setFont (ctx.theme.font (kTextSmall));
                g.drawText (restText, cell.withSizeKeepingCentre (cell.getWidth(), 16.0f).translated (0.0f, -12.0f), juce::Justification::centred, false);
            }
            const float y = valueY() - static_cast<float> (getY());
            const float top = std::min (y, mid), bottom = std::max (y, mid);
            auto block = juce::Rectangle<float>::leftTopRightBottom (cell.getX() + 6.0f, top, cell.getRight() - 6.0f, bottom);
            if (block.getHeight() < 6.0f)
                block = block.withSizeKeepingCentre (block.getWidth(), 6.0f).withCentre ({ block.getCentreX(), y });
            g.setColour (colour (Tok::AccentLower).withAlpha (alpha * (type == 1 ? 0.3f : 1.0f)));
            g.fillRoundedRectangle (block, 3.0f);
        }
        else
        {
            const float y = valueY() - static_cast<float> (getY());
            auto fill = juce::Rectangle<float>::leftTopRightBottom (cell.getX() + 6.0f, y, cell.getRight() - 6.0f, cell.getBottom() - 3.0f);
            g.setColour (colour (Tok::AccentUpper).withAlpha (alpha * (type == 0 ? 1.0f : 0.45f)));
            g.fillRoundedRectangle (fill, 3.0f);
        }
        if (markerAlpha > 0.0f)
            drawExternalMarker (g, cell.reduced (1.0f), {}, markerAlpha, ctx.theme, false);
        if (hasKeyboardFocus (false))
            drawFocusRing (g, cell, 4.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            showContextMenu();
            return;
        }
        grid.pressBar (isPitch() ? StepGridModel::Lane::Pitch : StepGridModel::Lane::Velocity, grid.toGrid (e), e.mods.isShiftDown());
    }
    void mouseDrag (const juce::MouseEvent& e) override { grid.dragBar (grid.toGrid (e), e.mods.isShiftDown()); }
    void mouseUp (const juce::MouseEvent&) override { grid.releaseBars(); }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            grid.resetBar (step, field);
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const float delta = (std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
        keys.wheel (delta * 8.0f, fineModifier (e.mods), now());
        refresh();
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        bool known = false;
        const auto key = keyOf (k.getKeyCode(), known);
        if (known)
        {
            keys.key (key, fineModifier (k.getModifiers()), now());
            refresh();
            return true;
        }
        if (k.getKeyCode() == juce::KeyPress::returnKey)
        {
            showTypedEntry (getLocalBounds());
            return true;
        }
        return false;
    }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::slider, juce::AccessibilityActions {},
                                                             juce::AccessibilityHandler::Interfaces { std::make_unique<BarValue> (*this, handle) });
    }

private:
    void idle (double nowMs) override { keys.idle (nowMs); }
    bool isInGesture() const override { return grid.getModel().isInGesture (targetIndex()) || keys.isInGesture(); }
    void valueChanged() override
    {
        pitchLabel = drawnText().replace (" st", ""); // prepared here, paint() only draws
        repaint();
    }

    StepGrid& grid;
    juce::String pitchLabel, restText { "rest" };
    int step;
    StepGridModel::Field field;
    model::SliderModel keys; // arrow keys, wheel (mouse painting is the grid model's)
};

// ---- The length flag ----
class StepGrid::LengthFlag final : public BoundComponent
{
public:
    static constexpr int kWidth = 84;

    explicit LengthFlag (StepGrid& g)
        : BoundComponent (g.ctx, ParamID::seqLength, "Length flag", -1, "GraphHandle", Placement::Secondary), grid (g), keys (handle)
    {
        refresh();
    }
    ~LengthFlag() override { keys.cancel(); }

    int shownLength() const { return 1 + static_cast<int> (std::lround (model::clamp01 (drawnValue()) * 15.0f)); }
    float shownValue() const override
    {
        return grid.getModel().activeLane() == StepGridModel::Lane::Flag || keys.isInGesture() ? handle.value() : BoundComponent::shownValue();
    }
    bool showsMirroredValue (juce::String& why) const override
    {
        if (! BoundComponent::showsMirroredValue (why))
            return false;
        const int expected = 1 + static_cast<int> (std::lround (model::clamp01 (ctx.mirror.value (handle.getIndex())) * 15.0f));
        if (grid.shownLength() != expected || shownLength() != expected)
        {
            why << getComponentID() << ": flag at " << grid.shownLength() << ", parameter " << expected;
            return false;
        }
        return true;
    }
    void cancelInteraction() override { keys.cancel(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f, 2.0f);
        g.setColour (colour (Tok::AccentGlobal));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (colour (Tok::TextOnAccent));
        g.setFont (ctx.theme.font (metrics::kText, true));
        drawFittedText (g, caption, r, juce::Justification::centred);
        if (markerAlpha > 0.0f)
            drawExternalMarker (g, r, {}, markerAlpha, ctx.theme, false);
        if (hasKeyboardFocus (false))
            drawFocusRing (g, r, 6.0f);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            showContextMenu();
            return;
        }
        const auto p = grid.toGrid (e);
        grab = p.x - boundaryX();
        grid.getModel().pressFlag (p.x - grab);
        refreshAll();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        grid.getModel().dragFlag (grid.toGrid (e).x - grab);
        refreshAll();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        grid.getModel().release();
        refreshAll();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        grid.getModel().cancel();
        model::writeOnce (handle, handle.defaultValue());
        refreshAll();
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        bool known = false;
        const auto key = keyOf (k.getKeyCode(), known);
        if (! known)
            return false;
        keys.key (key, fineModifier (k.getModifiers()), now());
        refresh();
        return true;
    }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::slider, juce::AccessibilityActions {},
                                                             juce::AccessibilityHandler::Interfaces { std::make_unique<BarValue> (*this, handle) });
    }

private:
    void refreshAll() { refresh(); }
    float boundaryX() const
    {
        return static_cast<float> (grid.getModel().geometry().x0) + static_cast<float> (shownLength() * grid.columnWidth());
    }
    void valueChanged() override
    {
        caption = "Length " + juce::String (shownLength()); // prepared here, paint() only draws
        repaint();
        grid.lengthShown (shownLength());
    }
    void idle (double nowMs) override { keys.idle (nowMs); }
    bool isInGesture() const override { return grid.getModel().activeLane() == StepGridModel::Lane::Flag || keys.isInGesture(); }

    StepGrid& grid;
    juce::String caption;
    model::SliderModel keys;
    float grab = 0.0f;
};

BoundComponent& StepGrid::lengthFlag() noexcept
{
    return *flag;
}
juce::Component& StepGrid::pitchBar (int step) noexcept
{
    return *pitch[static_cast<size_t> (step)];
}
juce::Component& StepGrid::velocityBar (int step) noexcept
{
    return *velocity[static_cast<size_t> (step)];
}
int StepGrid::drawnSemitones (int step) const
{
    return pitch[static_cast<size_t> (step)]->drawnSemitones();
}
float StepGrid::barValueY (int step, bool pitchBarNotVelocity) const
{
    return (pitchBarNotVelocity ? pitch : velocity)[static_cast<size_t> (step)]->valueY();
}
juce::String StepGrid::barText (int step, bool pitchBarNotVelocity) const
{
    return (pitchBarNotVelocity ? pitch : velocity)[static_cast<size_t> (step)]->drawnText();
}

void StepGrid::TypeListener::mirrorChanged (int index, bool)
{
    for (size_t i = 0; i < indices.size(); ++i)
        if (indices[i] == index && owner != nullptr && owner->pitch[i] != nullptr)
            owner->pitch[i]->repaint(), owner->velocity[i]->repaint();
}

std::vector<std::unique_ptr<ParamHandle>> StepGrid::makeHandles (ParamMirror& mirror)
{
    std::vector<std::unique_ptr<ParamHandle>> h;
    h.reserve (StepGridModel::kNumTargets);
    for (int step = 0; step < StepGridModel::kSteps; ++step)
        for (int f = 0; f < StepGridModel::kFields; ++f)
            h.push_back (std::make_unique<ParamHandle> (mirror, mirror.indexOf (seqStepParamID (step + 1, static_cast<SeqStepField> (f)))));
    h.push_back (std::make_unique<ParamHandle> (mirror, mirror.indexOf (ParamID::seqLength)));
    return h;
}

StepGrid::StepGrid (UiContext& c) : ctx (c), handles (makeHandles (c.mirror)), model (targetsOf (handles))
{
    setTitle ("Step sequencer grid");
    setComponentID ("seq.grid");
    rowLabels.add ("Pitch");
    rowLabels.add ("(semitones)");
    rowLabels.add ("Velocity");
    rowLabels.add ("Rest / Tie");
    for (int i = 1; i <= 16; ++i)
        numbers.add (juce::String (i));

    for (int i = 0; i < 16; ++i)
    {
        const auto si = static_cast<size_t> (i);
        pitch[si] = std::make_unique<Bar> (c, *this, i, StepGridModel::Note);
        velocity[si] = std::make_unique<Bar> (c, *this, i, StepGridModel::Velocity);
        types[si] = std::make_unique<StepTypeToggle> (c, i + 1);
        addAndMakeVisible (*pitch[si]);
        addAndMakeVisible (*velocity[si]);
        addAndMakeVisible (*types[si]);
        typeListener.indices[si] = c.mirror.indexOf (seqStepParamID (i + 1, SeqStepField::Type));
        c.mirror.subscribe (typeListener.indices[si], &typeListener);
    }
    typeListener.owner = this;
    flag = std::make_unique<LengthFlag> (*this);
    addAndMakeVisible (*flag);
    length = flag->shownLength();
    lengthShown (length);
}

StepGrid::~StepGrid()
{
    cancelInteraction();
    ctx.mirror.unsubscribe (&typeListener);
}

int StepGrid::mirroredType (int step) const
{
    return model::stepIndex (ctx.mirror.value (typeListener.indices[static_cast<size_t> (step)]), 3);
}

void StepGrid::cancelInteraction()
{
    model.cancel();
    for (auto& b : pitch)
        if (b != nullptr)
            b->cancelInteraction();
    for (auto& b : velocity)
        if (b != nullptr)
            b->cancelInteraction();
    for (auto& t : types)
        if (t != nullptr)
            t->cancelInteraction();
    if (flag != nullptr)
        flag->cancelInteraction();
}

juce::Point<float> StepGrid::toGrid (const juce::MouseEvent& e)
{
    return e.getEventRelativeTo (this).position;
}

void StepGrid::refreshOpen()
{
    for (size_t i = 0; i < 16; ++i)
    {
        if (model.isInGesture (StepGridModel::indexOf (static_cast<int> (i), StepGridModel::Note)))
            pitch[i]->update();
        if (model.isInGesture (StepGridModel::indexOf (static_cast<int> (i), StepGridModel::Velocity)))
            velocity[i]->update();
    }
}

void StepGrid::pressBar (StepGridModel::Lane lane, juce::Point<float> p, bool octaves)
{
    model.pressBar (lane, p.x, p.y, octaves);
    refreshOpen();
}

void StepGrid::dragBar (juce::Point<float> p, bool octaves)
{
    model.dragBar (p.x, p.y, octaves);
    refreshOpen();
}

void StepGrid::releaseBars()
{
    // Bars that were written show the value they ended at; the next tick shows whatever the
    // parameter then holds (a contested drag writes nothing here).
    std::array<bool, 32> open {};
    for (size_t i = 0; i < 16; ++i)
    {
        open[i] = model.isInGesture (StepGridModel::indexOf (static_cast<int> (i), StepGridModel::Note));
        open[16 + i] = model.isInGesture (StepGridModel::indexOf (static_cast<int> (i), StepGridModel::Velocity));
    }
    model.release();
    for (size_t i = 0; i < 16; ++i)
    {
        if (open[i])
            pitch[i]->update();
        if (open[16 + i])
            velocity[i]->update();
    }
}

void StepGrid::resetBar (int step, int field)
{
    model.reset (step, field);
    (field == StepGridModel::Note ? pitch : velocity)[static_cast<size_t> (step)]->update();
}

void StepGrid::lengthShown (int newLength)
{
    newLength = juce::jlimit (1, 16, newLength);
    const int old = length;
    length = newLength;
    if (types[0] == nullptr)
        return; // still being built
    for (int i = 0; i < 16; ++i)
        types[static_cast<size_t> (i)]->setDimmed (i >= newLength || off);
    layoutFlag();
    rebuildOverlay();
    repaint();
    if (old != newLength)
        for (size_t i = 0; i < 16; ++i)
        {
            pitch[i]->repaint();
            velocity[i]->repaint();
        }
}

void StepGrid::rebuildOverlay()
{
    // Paths are made when an input changes; paintOverChildren() only draws them.
    const float x = static_cast<float> (kLabelW + length * colW);
    const float bottom = static_cast<float> (typeBounds (0).getBottom());
    lengthDash.clear();
    if (length < 16)
    {
        const float dashes[] { 4.0f, 3.0f };
        juce::Path line;
        line.startNewSubPath (x, 0.0f);
        line.lineTo (x, bottom);
        juce::PathStrokeType (1.5f).createDashedStroke (lengthDash, line, dashes, 2);
    }
    playMark.clear();
    if (playing >= 0)
    {
        const float cx = static_cast<float> (kLabelW + playing * colW) + static_cast<float> (colW) * 0.5f;
        const float ty = bottom + 2.0f + static_cast<float> (kNumberH) + 4.0f;
        playMark.addTriangle (cx - 5.0f, ty + 8.0f, cx + 5.0f, ty + 8.0f, cx, ty);
    }
}

void StepGrid::setOff (bool isOff)
{
    if (off == isOff)
        return;
    off = isOff;
    for (size_t i = 0; i < 16; ++i)
    {
        pitch[i]->setInactive (off);
        velocity[i]->setInactive (off);
        types[i]->setInactive (off);
        types[i]->setDimmed (static_cast<int> (i) >= length || off);
    }
    flag->setInactive (off);
    repaint();
}

void StepGrid::setPlayingStep (int step)
{
    step = step >= 0 && step < 16 ? step : -1;
    if (step == playing)
        return;
    playing = step;
    rebuildOverlay();
    repaint();
}

juce::Rectangle<int> StepGrid::pitchBounds (int step) const
{
    return { kLabelW + step * colW + 3, 0, colW - 6, kValueRow + kPitchH };
}
juce::Rectangle<int> StepGrid::velocityBounds (int step) const
{
    return { kLabelW + step * colW + 3, kValueRow + kPitchH + kGap, colW - 6, kVelH };
}
juce::Rectangle<int> StepGrid::typeBounds (int step) const
{
    return { kLabelW + step * colW + 3, kValueRow + kPitchH + kGap + kVelH + kGap, colW - 6, kTypeH };
}

void StepGrid::resized()
{
    colW = std::max (8, (getWidth() - kLabelW) / 16);
    StepGridModel::Geometry geo;
    geo.x0 = static_cast<float> (kLabelW);
    geo.colW = static_cast<float> (colW);
    geo.pitchTop = static_cast<float> (kValueRow);
    geo.pitchH = static_cast<float> (kPitchH);
    geo.velTop = static_cast<float> (kValueRow + kPitchH + kGap);
    geo.velH = static_cast<float> (kVelH);
    model.setGeometry (geo);
    for (int i = 0; i < 16; ++i)
    {
        const auto si = static_cast<size_t> (i);
        pitch[si]->setBounds (pitchBounds (i));
        velocity[si]->setBounds (velocityBounds (i));
        types[si]->setBounds (typeBounds (i));
    }
    layoutFlag();
    rebuildOverlay();
}

void StepGrid::layoutFlag()
{
    if (flag == nullptr)
        return;
    const int y = typeBounds (0).getBottom() + 2 + kNumberH + 2;
    const int cx = kLabelW + length * colW;
    const int x = juce::jlimit (kLabelW, std::max (kLabelW, getWidth() - LengthFlag::kWidth), cx - LengthFlag::kWidth / 2);
    flag->setBounds (x, y, LengthFlag::kWidth, kFlagH);
}

void StepGrid::paint (juce::Graphics& g)
{
    using namespace metrics;
    g.setColour (colour (Tok::TextSecondary));
    g.setFont (ctx.theme.font (kText));
    const juce::Rectangle<float> col (0.0f, 0.0f, static_cast<float> (kLabelW - 6), 18.0f);
    drawFittedText (g, rowLabels[0], col.translated (0.0f, static_cast<float> (kValueRow + 4)), juce::Justification::centredLeft);
    g.setFont (ctx.theme.font (kTextSmall));
    drawFittedText (g, rowLabels[1], col.translated (0.0f, static_cast<float> (kValueRow + 22)), juce::Justification::centredLeft);
    g.setFont (ctx.theme.font (kText));
    drawFittedText (g, rowLabels[2], col.translated (0.0f, static_cast<float> (kValueRow + kPitchH + kGap + 4)), juce::Justification::centredLeft);
    drawFittedText (g, rowLabels[3], col.translated (0.0f, static_cast<float> (typeBounds (0).getY() + 4)), juce::Justification::centredLeft);

    g.setFont (ctx.theme.font (kTextSmall));
    const int numberY = typeBounds (0).getBottom() + 2;
    for (int i = 0; i < 16; ++i)
    {
        g.setColour (i >= length ? colour (Tok::TextDisabled) : (i == playing ? colour (Tok::TextPrimary) : colour (Tok::TextSecondary)));
        drawFittedText (g, numbers[i], { static_cast<float> (kLabelW + i * colW), static_cast<float> (numberY), static_cast<float> (colW), static_cast<float> (kNumberH) },
                        juce::Justification::centred);
    }
}

void StepGrid::paintOverChildren (juce::Graphics& g)
{
    // The length: a dashed line down the grid where the pattern wraps; the playing step outlined.
    g.setColour (colour (Tok::AccentGlobal));
    g.fillPath (lengthDash);
    if (playing >= 0)
    {
        const float bottom = static_cast<float> (typeBounds (0).getBottom());
        const auto r = juce::Rectangle<float> (static_cast<float> (kLabelW + playing * colW + 1), 0.0f, static_cast<float> (colW - 2), bottom).reduced (0.5f);
        g.setColour (colour (Tok::TextPrimary));
        g.drawRoundedRectangle (r, 5.0f, 2.0f);
        g.fillPath (playMark);
    }
}

std::unique_ptr<juce::AccessibilityHandler> StepGrid::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::group);
}
} // namespace vs::ui
