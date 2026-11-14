#pragma once

// Interaction logic of the sequencer's step grid (docs/architecture.md 9.2.2, 9.3.5, 9.1.5).
// JUCE-free. 16 steps x (pitch, type, velocity) plus the length flag: 49 parameters.
//
//   pitch bar     drag y: the bar goes to the pointer (3 px per semitone at the design height,
//                 Shift snaps to octaves); a drag that crosses columns sets every crossed step
//   velocity bar  click or drag y: the value at the pointer, painted across columns the same way
//   R / T         (typeAfterPress) a press on the lit button returns the step to Note, else sets Rest / Tie
//   length flag   x snaps to a step boundary
//
// Gestures (9.1.5, "multi-parameter drag"): a parameter's gesture begins when it is first written
// and all of them end together at release (or cancel); a value is never written outside one.

#include "common/ui/models/ParamTarget.h"

#include <array>
#include <cstdint>

namespace vs::ui::model
{
class StepGridModel
{
public:
    static constexpr int kSteps = 16, kFields = 3, kNumTargets = kSteps * kFields + 1, kLengthIndex = kSteps * kFields;
    enum Field
    {
        Note = 0,
        Type = 1,
        Velocity = 2
    };
    enum class Lane
    {
        None,
        Pitch,
        Velocity,
        Flag
    };
    static constexpr int kMinSemitones = -24, kMaxSemitones = 24, kSemitoneRange = kMaxSemitones - kMinSemitones;

    /** Where the columns and lanes are, in the grid's own pixels. */
    struct Geometry
    {
        float x0 = 0.0f, colW = 60.0f;          // left edge of column 0, column width
        float pitchTop = 0.0f, pitchH = 144.0f; // pitch lane: +24 at the top, -24 at the bottom, 0 in the middle
        float velTop = 0.0f, velH = 60.0f;      // velocity lane: 1 at the top, 0 at the bottom

        float pxPerSemitone() const noexcept { return pitchH / static_cast<float> (kSemitoneRange); }
    };

    static constexpr int indexOf (int step, int field) noexcept { return step * kFields + field; }

    /** `targets[indexOf (step, field)]`, `targets[kLengthIndex]` = seq.length. */
    explicit StepGridModel (const std::array<ParamTarget*, kNumTargets>& t) noexcept : targets (t) {}

    void setGeometry (const Geometry& g) noexcept { geo = g; }
    const Geometry& geometry() const noexcept { return geo; }

    // ---- Pure mappings (also used to place the bars) ----
    /** Column under `x` clamped to the grid (a drag past either end keeps painting the end column). */
    int columnAt (float x) const noexcept
    {
        const int c = static_cast<int> (std::floor ((x - geo.x0) / geo.colW));
        return c < 0 ? 0 : (c >= kSteps ? kSteps - 1 : c);
    }
    static float pitchNormalised (int semitones) noexcept
    {
        return clamp01 (static_cast<float> (semitones - kMinSemitones) / static_cast<float> (kSemitoneRange));
    }
    static int semitonesOfNormalised (float v) noexcept
    {
        return kMinSemitones + static_cast<int> (std::lround (clamp01 (v) * static_cast<float> (kSemitoneRange)));
    }
    /** Semitones for a pointer at `y`: 0 at the lane's middle; octaves only with `snapOctaves`. */
    int semitonesAt (float y, bool snapOctaves) const noexcept
    {
        const float mid = geo.pitchTop + geo.pitchH * 0.5f;
        float st = (mid - y) / geo.pxPerSemitone();
        if (snapOctaves)
            st = std::round (st / 12.0f) * 12.0f;
        st = std::clamp (std::round (st), static_cast<float> (kMinSemitones), static_cast<float> (kMaxSemitones));
        return static_cast<int> (st);
    }
    float velocityAt (float y) const noexcept { return clamp01 ((geo.velTop + geo.velH - y) / geo.velH); }
    /** y of a pitch value / velocity in the grid. */
    float pitchY (int semitones) const noexcept
    {
        return geo.pitchTop + geo.pitchH * 0.5f - static_cast<float> (semitones) * geo.pxPerSemitone();
    }
    float velocityY (float v) const noexcept { return geo.velTop + geo.velH * (1.0f - clamp01 (v)); }
    /** Length (1..16) for a flag dragged to `x`: the nearest step boundary. */
    int lengthAt (float x) const noexcept
    {
        const int n = static_cast<int> (std::lround ((x - geo.x0) / geo.colW));
        return n < 1 ? 1 : (n > kSteps ? kSteps : n);
    }
    static float lengthNormalised (int length) noexcept { return clamp01 (static_cast<float> (length - 1) / static_cast<float> (kSteps - 1)); }

    // ---- Painting a lane ----
    /** Mouse-down in a bar lane: the pointer's value goes into the column under it. */
    void pressBar (Lane lane, float x, float y, bool snapOctaves)
    {
        if (lane != Lane::Pitch && lane != Lane::Velocity)
            return;
        cancel();
        active = lane;
        lastColumn = columnAt (x);
        paint (lane, lastColumn, lastColumn, y, snapOctaves);
    }
    /** Drag: every column crossed since the last event (and the one under the pointer) takes the
        pointer's value. */
    void dragBar (float x, float y, bool snapOctaves)
    {
        if (active != Lane::Pitch && active != Lane::Velocity)
            return;
        const int c = columnAt (x);
        paint (active, lastColumn, c, y, snapOctaves);
        lastColumn = c;
    }
    /** Mouse-up (any lane): every gesture this interaction began ends. */
    void release() { endAll(); }

    // ---- The length flag ----
    void pressFlag (float x)
    {
        cancel();
        active = Lane::Flag;
        write (kLengthIndex, lengthNormalised (lengthAt (x)));
    }
    void dragFlag (float x)
    {
        if (active == Lane::Flag)
            write (kLengthIndex, lengthNormalised (lengthAt (x)));
    }

    // ---- Rest / Tie buttons (StepTypeToggle): which = 0 R, 1 T ----
    /** The type index (0 Note, 1 Rest, 2 Tie) after a press on R or T: the lit button returns the
        step to Note, else the button's type is set. */
    static int typeAfterPress (int currentType, int which) noexcept
    {
        const int wanted = which == 0 ? 1 : 2;
        return currentType == wanted ? 0 : wanted;
    }

    /** Double-click on a bar: the parameter's default, as one gesture. */
    void reset (int step, int field)
    {
        if (step < 0 || step >= kSteps || field < 0 || field >= kFields)
            return;
        cancel();
        auto& t = *targets[static_cast<size_t> (indexOf (step, field))];
        writeOnce (t, t.defaultValue());
    }

    /** Ends every open gesture (the editor closes, the page is hidden mid-drag). */
    void cancel() { endAll(); }
    bool isInGesture() const noexcept { return begun != 0; }
    bool isInGesture (int targetIndex) const noexcept { return ((begun >> targetIndex) & 1u) != 0; }
    Lane activeLane() const noexcept { return active; }

private:
    void paint (Lane lane, int from, int to, float y, bool snapOctaves)
    {
        const int lo = from < to ? from : to, hi = from < to ? to : from;
        const float value = lane == Lane::Pitch ? pitchNormalised (semitonesAt (y, snapOctaves)) : velocityAt (y);
        for (int c = lo; c <= hi; ++c)
            write (indexOf (c, lane == Lane::Pitch ? Note : Velocity), value);
    }
    void write (int idx, float v)
    {
        auto& t = *targets[static_cast<size_t> (idx)];
        const std::uint64_t bit = std::uint64_t { 1 } << idx;
        if ((begun & bit) == 0)
        {
            t.beginGesture();
            begun |= bit;
        }
        t.setValue (snap (v, t.numSteps()));
    }
    void endAll()
    {
        for (int i = 0; i < kNumTargets && begun != 0; ++i)
            if (((begun >> i) & 1u) != 0)
            {
                begun &= ~(std::uint64_t { 1 } << i);
                targets[static_cast<size_t> (i)]->endGesture();
            }
        active = Lane::None;
    }

    std::array<ParamTarget*, kNumTargets> targets;
    Geometry geo;
    std::uint64_t begun = 0; // bit per target whose gesture is open
    Lane active = Lane::None;
    int lastColumn = 0;
};
} // namespace vs::ui::model
