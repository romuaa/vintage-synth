#pragma once

// A control model's view of one host parameter (docs/architecture.md 9.1.2, 9.1.5): reads the
// live value, the default and the step count from the parameter itself, and writes through the
// mirror's gestures (beginChangeGesture -> setValueNotifyingHost ... -> endChangeGesture).

#include "common/ui/ParamMirror.h"
#include "common/ui/models/ParamTarget.h"

namespace vs::ui
{
class ParamHandle final : public ParamTarget
{
public:
    ParamHandle (ParamMirror& m, int parameterIndex) noexcept : mirror (m), index (parameterIndex) {}

    float value() const noexcept override { return param().getValue(); }
    float defaultValue() const noexcept override { return param().getDefaultValue(); }
    int numSteps() const noexcept override
    {
        const auto& p = param();
        if (! p.isDiscrete())
            return 0;
        const int n = p.getNumSteps();
        return n >= 2 && n <= 4096 ? n : 0;
    }
    void beginGesture() override { mirror.beginGesture (index); }
    void setValue (float normalised) override { mirror.setValue (index, normalised); }
    void endGesture() override { mirror.endGesture (index); }

    int getIndex() const noexcept { return index; }
    juce::RangedAudioParameter& param() const noexcept { return mirror.parameter (index); }
    ParamMirror& getMirror() const noexcept { return mirror; }

    /** The parameter's own display text for a normalised value, with its unit label (9.2.1). */
    juce::String textFor (float normalised) const
    {
        auto& p = param();
        auto t = p.getText (normalised, 1024); // what getCurrentValueAsText() shows for that value
        const auto label = p.getLabel();
        return label.isEmpty() ? t : t + " " + label;
    }

    /** Typed text -> normalised value through the parameter (getValueForText); false if the text
        cannot be read (then nothing changes). */
    bool parse (const juce::String& text, float& out) const
    {
        auto& p = param();
        const auto t = text.trim();
        if (t.isEmpty())
            return false;
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&p))
        {
            const int i = choice->choices.indexOf (t, true);
            if (i < 0)
                return false;
            out = p.convertTo0to1 (static_cast<float> (i));
            return true;
        }
        if (! t.containsAnyOf ("0123456789") && ! p.getText (p.getDefaultValue(), 0).equalsIgnoreCase (t))
        {
            // Words a parameter's text uses for a value ("Off", "Centre", "C") still parse.
            bool known = false;
            for (float v : { 0.0f, 0.5f, 1.0f })
                known = known || p.getText (v, 0).equalsIgnoreCase (t);
            if (! known)
                return false;
        }
        const float v = p.getValueForText (t);
        if (! std::isfinite (v))
            return false;
        out = model::clamp01 (v);
        return true;
    }

private:
    ParamMirror& mirror;
    int index;
};
} // namespace vs::ui
