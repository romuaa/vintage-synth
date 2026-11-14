#include "bside/presets/BSideTables.h"

#include "bside/program/TableReader.h"

namespace bs
{
namespace
{
constexpr size_t kMaxMessageBytes = 1024; // an error message is shown to a user and stored: bounded

/** The text the reader gets: every line with its LF. */
std::string joined (const std::vector<std::string>& lines) { return vs::presets::joinTableLines (lines); }

/** The largest legitimate bank file. 64 programs hold two tables each; the largest table the reader
    accepts is 32 768 bytes and 1024 lines. In the worst case every table is made of lines of 31
    quotation marks after a "#" (a comment: accepted), each of which the file writes with a
    backslash before every one: 2 x (32 768 - 1024) bytes of text + 14 bytes of indent, quotes, comma
    and line end per line = 77 824 bytes per table, 9 961 472 bytes for 128 tables; the parameters
    and names of 64 programs add less than 0.4 MB. The cap leaves about 20 % on top. Measured by the
    test "The largest legitimate bank fits the caps". */
constexpr size_t kMaxFileBytes = 12u * 1024u * 1024u;

/** Values in the JSON tree of that bank: per table one array and up to 1024 strings, per program
    its own members (about 190), for 64 programs: 64 x (2 x 1025 + 190) = 143 000; rounded up. */
constexpr int kMaxJsonValues = 160000;
} // namespace

juce::String validateTableText (const std::vector<std::string>& lines)
{
    const auto r = vs::bside::program::readTable (joined (lines));
    if (r.ok)
        return {};
    auto message = r.message();
    if (message.size() > kMaxMessageBytes)
        message.resize (kMaxMessageBytes);
    return juce::String::fromUTF8 (message.data(), static_cast<int> (message.size()));
}

std::vector<std::string> canonicalTableLines (const std::vector<std::string>& lines)
{
    const auto r = vs::bside::program::readTable (joined (lines));
    std::string text;
    std::vector<std::string> out;
    if (! r.ok || ! vs::bside::program::writeTable (r.program, text))
        return out;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n')
        {
            out.emplace_back (text, start, i - start);
            start = i + 1;
        }
    if (start < text.size())
        out.emplace_back (text, start, text.size() - start);
    return out;
}

void enableTables (vs::presets::Instrument& i)
{
    i.maxTableLines = vs::bside::program::kMaxLines;
    i.maxTableBytes = vs::bside::program::kMaxTextBytes;
    i.maxFileBytes = kMaxFileBytes;
    i.maxJsonValues = kMaxJsonValues;
    i.validateTable = [] (const std::vector<std::string>& lines) { return validateTableText (lines); };
    i.canonicaliseTable = [] (const std::vector<std::string>& lines) { return canonicalTableLines (lines); };
}

void BSideTables::publishTable (int tone, const std::vector<std::string>& lines) noexcept
{
    using namespace vs::bside::program;
    if (tone < 0 || tone >= kParts)
        return;
    try
    {
        // Compiled here, off the audio thread; the hand-over copies it into a free buffer.
        if (! lines.empty())
        {
            const auto r = readTable (joined (lines));
            if (r.ok)
            {
                parts[tone].publish (r.program);
                return;
            }
        }
    }
    catch (...) // allocation failure: the part gets no table rather than a crash in a host callback
    {
    }
    parts[tone].publish (emptyProgram());
}
} // namespace bs
