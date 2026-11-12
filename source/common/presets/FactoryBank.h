#pragma once

// The read-only factory bank: program files embedded at build time (the instrument's CMake target
// turns presets/factory/*.json into binary data), parsed once at start-up from memory: no file
// I/O at run time. A file's name starts with its slot, "A1" ... "H8"; slots without a file are
// "Init". Parsing is the tolerant reader of PresetFormat.h; a file that fails to parse leaves its
// slot at "Init" and is reported in `errors` (a test requires none).

#include "common/presets/PresetFormat.h"

namespace vs::presets
{
struct FactoryFile
{
    juce::String fileName;
    const char* data = nullptr;
    size_t size = 0;
};

/** "A1" -> 0, "H8" -> 63 (case-insensitive, from the start of the text); -1 if it does not start with a slot. */
int slotFromFileName (const juce::String& fileName);

Bank makeBankFromFiles (const Schema& schema, const juce::String& bankName, const std::vector<FactoryFile>& files,
                        juce::StringArray* errors = nullptr);
} // namespace vs::presets
