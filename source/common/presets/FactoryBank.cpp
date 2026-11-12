#include "common/presets/FactoryBank.h"

namespace vs::presets
{
Bank makeBankFromFiles (const Schema& schema, const juce::String& bankName, const std::vector<FactoryFile>& files,
                        juce::StringArray* errors)
{
    Bank bank = makeInitBank (schema);
    bank.name = bankName;
    bank.author = "Exitium";
    std::vector<bool> filled (static_cast<size_t> (schema.bankSize()), false);
    for (const auto& f : files)
    {
        const int slot = schema.instrument().slotFromFileName (f.fileName);
        if (slot < 0)
        {
            if (errors != nullptr)
                errors->add (f.fileName + ": the name does not start with a slot (A1 ... " + schema.instrument().positionName (schema.bankSize() - 1) + ")");
            continue;
        }
        if (filled[static_cast<size_t> (slot)])
        {
            if (errors != nullptr)
                errors->add (f.fileName + ": the slot is used twice");
            continue;
        }
        auto parsed = parseProgram (schema, f.data, f.size);
        if (! parsed.ok)
        {
            if (errors != nullptr)
                errors->add (f.fileName + ": " + parsed.error);
            continue;
        }
        bank.programs[static_cast<size_t> (slot)] = std::move (parsed.value);
        filled[static_cast<size_t> (slot)] = true;
    }
    return bank;
}
} // namespace vs::presets
