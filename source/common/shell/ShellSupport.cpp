#include "common/shell/ShellSupport.h"

#include <cstdio>
#include <cstdlib>

namespace vs::shell
{
void reportFatalConfigError (const char* what) noexcept
{
    std::fprintf (stderr, "Exitium plugin: setup error, the plugin cannot start: %s\n", what);
    std::fflush (stderr);
    juce::Logger::writeToLog (juce::String ("Exitium plugin: setup error, the plugin cannot start: ") + what);
    std::abort();
}

juce::ValueTree takeChildren (juce::ValueTree& state, const char* type)
{
    juce::ValueTree first;
    const juce::Identifier id (type);
    // From the back: erasing at the end of the child array costs nothing, erasing at the front shifts everything.
    for (int i = state.getNumChildren() - 1; i >= 0; --i)
    {
        auto child = state.getChild (i);
        if (child.hasType (id))
        {
            first = child; // ends as the lowest index, the first one in the document
            state.removeChild (i, nullptr);
        }
    }
    return first;
}

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
