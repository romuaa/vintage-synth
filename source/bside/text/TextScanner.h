#pragma once

// Shared plumbing of B-Side's strict text readers (patch tables, trace, reference and spec files):
// a bounded error list in the contract's message format, a byte-level text check (UTF-8, control
// characters), a line splitter and a small cursor. JUCE-free; no exceptions of its own (the callers
// guard allocation failure at their API boundary).
//
// Columns are 1-based byte offsets within the line. Lines end at LF or CRLF.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vs::bside::text
{
constexpr int kMaxStoredErrors = 8;

struct Error
{
    int line = 0;
    int column = 0;
    std::string what;
    std::string expected; // may be empty
};

/** Collects errors: the first kMaxStoredErrors are kept, the rest are only counted. */
class ErrorList
{
public:
    void add (int line, int column, std::string what, std::string expected = {});
    bool empty() const noexcept { return total_ == 0; }
    int total() const noexcept { return total_; }
    const std::vector<Error>& stored() const noexcept { return stored_; }
    /** "line L, column C: <what> (expected <what>)" per message, then "and N more". */
    std::string format() const;

private:
    std::vector<Error> stored_;
    int total_ = 0;
};

/** Finds the first byte that may not appear in a text file: NUL and other control characters
    (except TAB, LF, and CR directly before LF), DEL, and invalid UTF-8 (overlong forms,
    surrogates, values above U+10FFFF, truncated sequences). Returns false when the text is clean. */
bool findInvalidByte (std::string_view text, int& line, int& column, const char*& what) noexcept;

/** Iterates the lines of a text (LF or CRLF; no empty line after a final EOL). */
class Lines
{
public:
    explicit Lines (std::string_view t) noexcept : text_ (t) {}
    bool next (std::string_view& line, int& number) noexcept;

private:
    std::string_view text_;
    size_t pos_ = 0;
    int number_ = 0;
};

inline bool isDigit (char c) noexcept { return c >= '0' && c <= '9'; }
inline bool isHexDigit (char c) noexcept { return isDigit (c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline int hexValue (char c) noexcept { return isDigit (c) ? c - '0' : (c | 0x20) - 'a' + 10; }

enum class NumberResult { ok, notNumber, outOfRange };

/** Decimal digits only (no sign, no spaces), at most 19 digits, value <= maxValue. */
NumberResult parseDecimal (std::string_view t, std::uint64_t maxValue, std::uint64_t& out) noexcept;
/** Hex digits only (either case), 1..maxDigits digits, value <= maxValue. */
NumberResult parseHex (std::string_view t, size_t maxDigits, std::uint64_t maxValue, std::uint64_t& out) noexcept;

/** Position inside one line. */
struct Cursor
{
    std::string_view s;
    size_t i = 0;

    bool atEnd() const noexcept { return i >= s.size(); }
    char peek() const noexcept { return i < s.size() ? s[i] : '\0'; }
    int column() const noexcept { return static_cast<int> (i) + 1; }
    /** Skips spaces and tabs; true if it skipped any. */
    bool skipWs() noexcept
    {
        const size_t from = i;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        return i > from;
    }
    /** End of the line or the start of a comment. */
    bool atEol() const noexcept { return i >= s.size() || s[i] == '#'; }
    /** A run of characters up to white space, ',' or '#' (empty at those). */
    std::string_view token() noexcept
    {
        const size_t from = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != ',' && s[i] != '#')
            ++i;
        return s.substr (from, i - from);
    }
};

/** The token at the cursor, or (when it is empty, e.g. at ',' or '#') the single character there; for messages. */
inline std::string_view tokenOrChar (Cursor& c) noexcept
{
    const auto t = c.token();
    return t.empty() ? c.s.substr (c.i, 1) : t;
}
} // namespace vs::bside::text
