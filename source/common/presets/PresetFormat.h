#pragma once

// Preset files (docs/architecture.md 6b.2): JSON, UTF-8, LF line ends, a `format` number.
//
//   program   { "format": 1, "instrument": "<id>", "kind": "program", "name", "category", "author",
//               "global": { "<id>": value, ... }, "<tone key>": { "<id without prefix>": value, ... }, ... }
//   tone      { "format": 1, "instrument": "<id>", "kind": "tone", "name", "category", "author", "params": { ... } }
//   bank      { "format": 1, "instrument": "<id>", "kind": "bank", "name", "author", "programs": [ { program body }, ... ] }
// "instrument" is the Schema's Instrument::id: a file of another instrument is rejected as a whole.
//
// Output is deterministic: fixed key order (header, then parameters in the schema's order), one
// parameter per line, each number written with the fewest digits that read back as the same float.
// A saved file read back and saved again is byte-identical.
//
// Numbers are written and read without any locale-dependent function (formatNumber, and
// StrictJson's number conversion), so a host that changes the process locale changes nothing.
//
// The text is parsed by the strict reader in StrictJson.h (RFC 8259 subset, bounded; duplicate keys
// rejected). Reading is tolerant per 6b.2: unknown keys and IDs are ignored, missing parameters take their
// default, values are clamped (discrete ones rounded to a whole step), a number that is not finite
// or a value that is not a number becomes the default, over-long or odd names are cleaned. The
// file as a whole is rejected, with a message and without any result, when it is not valid JSON or
// is truncated, has a different `format`, `instrument` or `kind`, is not UTF-8, is too large or too
// deeply nested, or has a section of the wrong type (a program that is not an object, a name that
// is not a string, more programs than the bank holds). Parsing has no side effects: a rejected
// file changes nothing.

#include "common/presets/PresetModel.h"

#include <string>

namespace vs::presets
{
template <typename T>
struct Parsed
{
    bool ok = false;
    juce::String error; // set when !ok
    T value {};
};

std::string toJson (const Schema& schema, const Program& program);
std::string toJson (const Schema& schema, const Tone& tone);
std::string toJson (const Schema& schema, const Bank& bank);

Parsed<Program> parseProgram (const Schema& schema, const char* utf8, size_t numBytes);
Parsed<Tone> parseTone (const Schema& schema, const char* utf8, size_t numBytes);
Parsed<Bank> parseBank (const Schema& schema, const char* utf8, size_t numBytes);

inline Parsed<Program> parseProgram (const Schema& schema, const std::string& s) { return parseProgram (schema, s.data(), s.size()); }
inline Parsed<Tone> parseTone (const Schema& schema, const std::string& s) { return parseTone (schema, s.data(), s.size()); }
inline Parsed<Bank> parseBank (const Schema& schema, const std::string& s) { return parseBank (schema, s.data(), s.size()); }

/** The shortest decimal text that reads back as the same float ("0.65", "3", "-0.4") through this
    module's own reader, in printf's %g layout. Locale-independent (no printf, no strtod). */
std::string formatNumber (float v);
} // namespace vs::presets
