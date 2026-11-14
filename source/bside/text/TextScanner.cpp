#include "bside/text/TextScanner.h"

namespace vs::bside::text
{
void ErrorList::add (int line, int column, std::string what, std::string expected)
{
    ++total_;
    if (static_cast<int> (stored_.size()) < kMaxStoredErrors)
        stored_.push_back ({ line, column, std::move (what), std::move (expected) });
}

std::string ErrorList::format() const
{
    std::string out;
    for (const auto& e : stored_)
    {
        if (! out.empty())
            out += '\n';
        out += "line " + std::to_string (e.line) + ", column " + std::to_string (e.column) + ": " + e.what;
        if (! e.expected.empty())
            out += " (expected " + e.expected + ")";
    }
    if (total_ > static_cast<int> (stored_.size()))
        out += "\nand " + std::to_string (total_ - static_cast<int> (stored_.size())) + " more";
    return out;
}

bool findInvalidByte (std::string_view t, int& line, int& column, const char*& what) noexcept
{
    int ln = 1;
    size_t lineStart = 0;
    const size_t n = t.size();
    const auto fail = [&] (size_t at, const char* why) {
        line = ln;
        column = static_cast<int> (at - lineStart) + 1;
        what = why;
        return true;
    };
    size_t i = 0;
    while (i < n)
    {
        const auto c = static_cast<unsigned char> (t[i]);
        if (c == '\n')
        {
            ++ln;
            lineStart = ++i;
            continue;
        }
        if (c == '\r')
        {
            if (i + 1 < n && t[i + 1] == '\n')
            {
                ++i;
                continue;
            }
            return fail (i, "stray carriage return");
        }
        if (c == '\t' || (c >= 0x20 && c < 0x7f))
        {
            ++i;
            continue;
        }
        if (c < 0x20 || c == 0x7f)
            return fail (i, "control character");
        // UTF-8 multi-byte sequence.
        size_t extra = 0;
        std::uint32_t minValue = 0, value = 0;
        if (c >= 0xc2 && c <= 0xdf) { extra = 1; minValue = 0x80; value = c & 0x1fu; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; minValue = 0x800; value = c & 0x0fu; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; minValue = 0x10000; value = c & 0x07u; }
        else return fail (i, "invalid UTF-8");
        if (i + extra >= n)
            return fail (i, "truncated UTF-8 sequence");
        for (size_t k = 1; k <= extra; ++k)
        {
            const auto cc = static_cast<unsigned char> (t[i + k]);
            if ((cc & 0xc0u) != 0x80u)
                return fail (i, "invalid UTF-8");
            value = (value << 6) | (cc & 0x3fu);
        }
        if (value < minValue || value > 0x10ffffu || (value >= 0xd800u && value <= 0xdfffu))
            return fail (i, "invalid UTF-8");
        i += extra + 1;
    }
    return false;
}

bool Lines::next (std::string_view& line, int& number) noexcept
{
    if (pos_ >= text_.size())
        return false;
    size_t end = pos_;
    while (end < text_.size() && text_[end] != '\n')
        ++end;
    size_t stop = end;
    if (stop > pos_ && text_[stop - 1] == '\r')
        --stop;
    line = text_.substr (pos_, stop - pos_);
    pos_ = end < text_.size() ? end + 1 : end;
    number = ++number_;
    return true;
}

NumberResult parseDecimal (std::string_view t, std::uint64_t maxValue, std::uint64_t& out) noexcept
{
    if (t.empty())
        return NumberResult::notNumber;
    for (const char c : t)
        if (! isDigit (c))
            return NumberResult::notNumber;
    if (t.size() > 19)
        return NumberResult::outOfRange;
    std::uint64_t v = 0;
    for (const char c : t)
        v = v * 10 + static_cast<std::uint64_t> (c - '0');
    if (v > maxValue)
        return NumberResult::outOfRange;
    out = v;
    return NumberResult::ok;
}

NumberResult parseHex (std::string_view t, size_t maxDigits, std::uint64_t maxValue, std::uint64_t& out) noexcept
{
    if (t.empty())
        return NumberResult::notNumber;
    for (const char c : t)
        if (! isHexDigit (c))
            return NumberResult::notNumber;
    if (t.size() > maxDigits || t.size() > 16)
        return NumberResult::outOfRange;
    std::uint64_t v = 0;
    for (const char c : t)
        v = (v << 4) | static_cast<std::uint64_t> (hexValue (c));
    if (v > maxValue)
        return NumberResult::outOfRange;
    out = v;
    return NumberResult::ok;
}
} // namespace vs::bside::text
