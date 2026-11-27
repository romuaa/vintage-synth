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
        const auto name = fullName.trim();
        ProductName p;
        p.company = name.upToFirstOccurrenceOf (" ", false, false);
        p.product = name.fromFirstOccurrenceOf (" ", false, false).trim();
        p.version = versionText;
        return p;
    }

    /** "Exitium Vintage Synth" */
    juce::String fullName() const { return product.isEmpty() ? company : company + " " + product; }
    /** The one-line text of the placeholder editor and the static text a screen reader announces:
        "Exitium Vintage Synth, version 0.9.0". */
    juce::String announcement() const { return version.isEmpty() ? fullName() : fullName() + ", version " + version; }
};
} // namespace vs::ui
