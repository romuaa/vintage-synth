#pragma once

// SHA-256 (FIPS 180-4) over a byte string, as 64 lower-case hex digits. Used to bind a reference
// or spec file to the exact bytes of the trace it was generated from. JUCE-free, no allocation
// besides the result string.

#include <string>
#include <string_view>

namespace vs::bside::trace
{
std::string sha256Hex (std::string_view data);
} // namespace vs::bside::trace
