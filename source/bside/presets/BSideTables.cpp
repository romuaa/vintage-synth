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
    accepts is 32 768 bytes and 1024 lines. The file writer needs at most 2 bytes for one byte of a table
    (found by code in the test "The largest legitimate bank fits the caps": a tab, a quotation mark and a
    backslash cost 2, every other byte 1 for 1, a tab being written as the two-byte escape), and about 12
    bytes per line for indent, quotes, comma and line end. Worst case: every line made of 31 tabs:
    75 782 bytes per table, 9.70 MB for 128 tables; with the parameters, names and values of 64 programs at
    their longest the constructed worst bank is 9 884 536 bytes. The cap leaves 27 % on top. */
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
    const auto message = r.message();
    // Lenient UTF-8 (the message may quote the offending text), cut between characters.
    return vs::presets::truncateUtf8 (juce::String::fromUTF8 (message.data(), static_cast<int> (message.size())), kMaxMessageBytes);
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

std::unique_ptr<vs::presets::PreparedTable> BSideTables::prepareTable (int tone, const std::vector<std::string>& lines)
{
    using namespace vs::bside::program;
    if (tone < 0 || tone >= kParts)
        return nullptr;
    auto compiled = std::make_unique<Compiled>();
    if (! lines.empty())
    {
        const auto r = readTable (joined (lines));
        if (! r.ok)
            return nullptr;
        compiled->program = r.program;
    }
    return compiled;
}

void BSideTables::publishTable (int tone, vs::presets::PreparedTable& prepared) noexcept
{
    if (tone < 0 || tone >= kParts)
        return;
    parts[tone].publish (static_cast<Compiled&> (prepared).program); // prepared by prepareTable or emptyTable: always a Compiled
}

vs::presets::PreparedTable& BSideTables::emptyTable (int) noexcept { return empty; }
} // namespace bs
