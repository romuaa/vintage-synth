#include "common/presets/FactoryBank.h"

namespace vs::presets
{
int slotFromFileName (const juce::String& fileName)
{
    if (fileName.length() < 2)
        return -1;
    const auto group = static_cast<int> (juce::CharacterFunctions::toUpperCase (fileName[0])) - 'A';
    const auto slot = static_cast<int> (fileName[1]) - '1';
    if (group < 0 || group >= kBankSize / 8 || slot < 0 || slot >= 8)
        return -1;
    return group * 8 + slot;
}

Bank makeBankFromFiles (const Schema& schema, const juce::String& bankName, const std::vector<FactoryFile>& files,
                        juce::StringArray* errors)
{
    Bank bank = makeInitBank (schema);
    bank.name = bankName;
    bank.author = "Exitium";
    std::vector<bool> filled (static_cast<size_t> (kBankSize), false);
    for (const auto& f : files)
    {
        const int slot = slotFromFileName (f.fileName);
        if (slot < 0)
        {
            if (errors != nullptr)
                errors->add (f.fileName + ": the name does not start with a slot (A1 ... H8)");
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
