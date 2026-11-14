#pragma once

// B-Side's patch tables in the preset system (docs/architecture.md 8.8): the validator, canonicaliser
// and limits that turn Instrument tables on, and the sink that compiles each applied table and
// publishes it to the audio thread (program/ProgramHandover.h). The reader, compiler and writer are
// the JUCE-free ones in bside/program/; the common preset code only sees the functions below.

#include "bside/program/ProgramHandover.h"
#include "common/presets/PresetManager.h"

#include <string>
#include <vector>

namespace bs
{
/** The limits of the format (8.6.5, 8.13), the largest legitimate bank's file size and JSON size,
    and the validator and canonicaliser. Sets the fields of Instrument documented in PresetModel.h. */
void enableTables (vs::presets::Instrument& instrument);

/** "" when the lines are a table the reader accepts, else its messages (bounded in length). */
juce::String validateTableText (const std::vector<std::string>& lines);
/** The canonical lines of an accepted table (comments dropped, fixed spacing); empty if the table is rejected.
    Used only to compare tables; tables are stored and saved verbatim. */
std::vector<std::string> canonicalTableLines (const std::vector<std::string>& lines);

/** Compiles each table the preset manager hands over (before the hold) and publishes the compiled
    program inside the hold, one hand-over per part (tone 0 = Lower, 1 = Upper). */
class BSideTables final : public vs::presets::TableSink
{
public:
    static constexpr int kParts = 2;

    /** The hand-over of a part; the audio thread's API is documented in ProgramHandover.h. */
    vs::bside::program::ProgramHandover& handover (int part) noexcept { return parts[part == 0 ? 0 : 1]; }

    /** Compiles the verbatim lines. Allocates; returns nullptr if the reader rejects them (the manager then rejects the table). */
    std::unique_ptr<vs::presets::PreparedTable> prepareTable (int tone, const std::vector<std::string>& lines) override;
    /** Copies the compiled program into a free buffer of the part's hand-over: no allocation, cannot fail. */
    void publishTable (int tone, vs::presets::PreparedTable& prepared) noexcept override;
    vs::presets::PreparedTable& emptyTable (int tone) noexcept override;

    struct Compiled final : vs::presets::PreparedTable
    {
        vs::bside::program::Program program = vs::bside::program::emptyProgram();
    };

private:
    vs::bside::program::ProgramHandover parts[kParts];
    Compiled empty; // the program of an empty table, ready in advance
};
} // namespace bs
