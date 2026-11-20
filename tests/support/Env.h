// Portable, header-only environment-variable access for tests and tools (never product code).
//
// MSVC flags std::getenv as deprecated (C4996), which is an error in our warnings-as-errors targets.
// On Windows this uses _dupenv_s (the function the warning recommends, no suppression needed); elsewhere
// it uses std::getenv. Values are copied, so the result stays valid after the call.
#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace testenv
{
inline std::optional<std::string> get (const char* name)
{
#if defined(_WIN32)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s (&buf, &len, name) != 0 || buf == nullptr)
        return std::nullopt;
    std::string v (buf);
    std::free (buf);
    return v;
#else
    if (const char* v = std::getenv (name))
        return std::string (v);
    return std::nullopt;
#endif
}

inline bool isSet (const char* name) { return get (name).has_value(); }
} // namespace testenv
