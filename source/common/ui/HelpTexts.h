#pragma once

// Hover help texts (docs/architecture.md 9.3.8): parses the embedded docs/ui/help-texts.md, the
// single source. JUCE-free (plain strings), so the parser rules are tested everywhere.
//
// Format: one entry per line, the line starting with "- `<id>`: <text>" where <id> is one token. `<part>.` expands to
// `lower.` and `upper.`; `NN` in an id expands to 01..16 and every "NN" in its text becomes the
// step number. Anything else in the file is ignored.

#include <string>
#include <string_view>
#include <vector>

namespace vs::ui
{
class HelpTexts
{
public:
    struct Entry
    {
        std::string id, text;
        int line = 0; // 1-based line in the source
    };

    HelpTexts() = default;
    static HelpTexts parse (std::string_view markdown);

    /** Every expanded entry, in file order (an id listed twice appears twice: a test catches it). */
    const std::vector<Entry>& entries() const noexcept { return list; }
    /** The text for `id`, or nullptr. */
    const std::string* find (std::string_view id) const noexcept;
    int countOf (std::string_view id) const noexcept;

    /** Sentences in a text: '.', '!' or '?' followed by a space or the end (not inside a number). */
    static int sentenceCount (std::string_view text) noexcept;
    /** Characters (UTF-8 code points) in a text. */
    static int characterCount (std::string_view text) noexcept;

private:
    std::vector<Entry> list;
};
} // namespace vs::ui
