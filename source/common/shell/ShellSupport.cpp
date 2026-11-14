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

namespace
{
// Attribute names of the UiState child (plain strings: no static juce::Identifier objects).
constexpr const char* uiPage = "page";
constexpr const char* uiPart = "part";
constexpr const char* uiArpView = "arpView";
constexpr const char* uiSize = "size";
constexpr const char* uiDelayLast = "delayLast";
constexpr const char* uiReverbLast = "reverbLast";
} // namespace

juce::ValueTree createUiState (const EditorState& state, std::span<const char* const> pageNames)
{
    const auto t = toText (state, pageNames);
    juce::ValueTree child (kUiStateType);
    child.setProperty (uiPage, juce::String (t.page), nullptr);
    child.setProperty (uiPart, juce::String (t.part), nullptr);
    child.setProperty (uiArpView, juce::String (t.arpView), nullptr);
    child.setProperty (uiSize, juce::String (t.size), nullptr);
    child.setProperty (uiDelayLast, juce::String (t.delayLast), nullptr);
    child.setProperty (uiReverbLast, juce::String (t.reverbLast), nullptr);
    return child;
}

void restoreUiState (EditorState& state, const juce::ValueTree& child, std::span<const char* const> pageNames)
{
    if (! child.isValid())
    {
        state.resetToDefaults();
        return;
    }
    // Attribute text is untrusted input: bounded length, parsed strictly (EditorState.h).
    const auto text = [&] (const char* id) { return child.getProperty (id).toString().substring (0, 32).toStdString(); };
    EditorStateText t;
    t.page = text (uiPage);
    t.part = text (uiPart);
    t.arpView = text (uiArpView);
    t.size = text (uiSize);
    t.delayLast = text (uiDelayLast);
    t.reverbLast = text (uiReverbLast);
    fromText (state, t, pageNames);
}
} // namespace vs::shell
