#include "common/shell/ShellSupport.h"

namespace vs::shell
{
void addMissingParameters (juce::ValueTree& state, const juce::Array<juce::AudioProcessorParameter*>& params)
{
    static const juce::Identifier paramType ("PARAM"), idProperty ("id"); // JUCE's state format
    for (auto* p : params)
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
            if (! state.getChildWithProperty (idProperty, withId->paramID).isValid())
            {
                juce::ValueTree entry (paramType);
                entry.setProperty (idProperty, withId->paramID, nullptr);
                state.appendChild (entry, nullptr);
            }
}
} // namespace vs::shell
