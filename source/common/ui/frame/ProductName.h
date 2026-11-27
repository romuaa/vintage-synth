#pragma once

// The text that says which plugin an editor window belongs to (docs/architecture.md 9.3.8, "Product
// name"). It is made from what the plugin's own code passes in (AudioProcessor::getName() and the
// build's project version), so shared code holds no product name and the two plugins can never show
// each other's. Message thread only.

#include <juce_core/juce_core.h>

namespace vs::ui
{
struct ProductName
{
    juce::String company;  // the first word of the full name ("Exitium")
    juce::String product;  // the rest ("Vintage Synth")
    juce::String version;  // "0.9.0"

    static ProductName make (const juce::String& fullName, const juce::String& versionText)
    {
        // Words are split at any white space (tabs, runs of spaces) and joined with one space.
        juce::StringArray words;
        juce::String word;
        for (auto c : fullName)
        {
            if (juce::CharacterFunctions::isWhitespace (c))
            {
                if (word.isNotEmpty())
                    words.add (word);
                word = juce::String();
            }
            else
                word += juce::String::charToString (c);
        }
        if (word.isNotEmpty())
            words.add (word);
        ProductName p;
        if (words.size() > 0)
        {
            p.company = words[0];
            words.remove (0);
            p.product = words.joinIntoString (" ");
        }
        p.version = versionText;
        return p;
    }

    /** "Exitium Vintage Synth" */
    juce::String fullName() const { return product.isEmpty() ? company : company + " " + product; }
    /** The static text a screen reader announces: "Exitium Vintage Synth, version 0.9.0"; just the name when there
        is no version, and nothing when there is no name. */
    juce::String announcement() const
    {
        const auto name = fullName();
        return name.isEmpty() || version.isEmpty() ? name : name + ", version " + version;
    }
    /** The placeholder editor's one line: the name, two spaces, the version (either part may be missing). */
    juce::String line() const
    {
        const auto name = fullName();
        return name.isEmpty() ? juce::String() : (version.isEmpty() ? name : name + "  " + version);
    }
};
} // namespace vs::ui
