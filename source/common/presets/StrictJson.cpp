#include "common/presets/StrictJson.h"

#include <algorithm>
#include <new>

namespace vs::presets::json
{
namespace
{
constexpr double kExactPow10[] = { 1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                   1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
constexpr int kMaxExactPow10 = 22;
constexpr std::uint64_t kTwoPow53 = 9007199254740992ull;

bool isDigit (char c) noexcept { return c >= '0' && c <= '9'; }

/** Length of the number token at p (grammar in StrictJson.h), or 0 if it is not one. */
size_t numberTokenLength (const char* p, size_t n) noexcept
{
    size_t i = 0;
    const auto digits = [&] (size_t maxCount) {
        size_t count = 0;
        while (i < n && isDigit (p[i]) && count <= maxCount)
        {
            ++i;
            ++count;
        }
        return count;
    };
    if (i < n && p[i] == '-')
        ++i;
    if (i < n && p[i] == '0')
        ++i;
    else if (const auto d = digits (9); d < 1 || d > 9)
        return 0;
    if (i < n && p[i] == '.')
    {
        ++i;
        if (const auto d = digits (20); d < 1 || d > 20)
            return 0;
    }
    if (i < n && (p[i] == 'e' || p[i] == 'E'))
    {
        ++i;
        if (i < n && (p[i] == '+' || p[i] == '-'))
            ++i;
        if (const auto d = digits (3); d < 1 || d > 3)
            return 0;
    }
    // Nothing number-like may stick to the token ("01", "1.2.3", "1e5e5", "1-2").
    if (i < n && (isDigit (p[i]) || p[i] == '.' || p[i] == 'e' || p[i] == 'E' || p[i] == '+' || p[i] == '-'))
        return 0;
    return i;
}

/** x * 10^k in double arithmetic: exact powers of ten, applied in steps of at most 10^22. */
double scaleByPow10 (double x, int k) noexcept
{
    while (k > kMaxExactPow10 && x > 0.0 && x <= 1.0e300)
    {
        x *= kExactPow10[kMaxExactPow10];
        k -= kMaxExactPow10;
    }
    while (k < -kMaxExactPow10 && x > 0.0)
    {
        x /= kExactPow10[kMaxExactPow10];
        k += kMaxExactPow10;
    }
    if (k > kMaxExactPow10)
        return x * 1.0e300 * 1.0e300; // overflows to infinity
    if (k > 0)
        return x * kExactPow10[k];
    if (k < -kMaxExactPow10)
        return 0.0;
    if (k < 0)
        return x / kExactPow10[-k];
    return x;
}

/** Converts a token known to be valid (numberTokenLength == n). */
double convertNumber (const char* p, size_t n) noexcept
{
    size_t i = 0;
    const bool negative = p[0] == '-';
    if (negative)
        ++i;
    std::uint64_t mantissa = 0;
    int significant = 0, dropped = 0, fractionDigits = 0;
    const auto take = [&] (char c) {
        const auto d = static_cast<std::uint64_t> (c - '0');
        if (mantissa == 0 && d == 0)
            return; // leading zeros carry no information
        if (significant < 19)
        {
            mantissa = mantissa * 10u + d;
            ++significant;
        }
        else
            ++dropped; // beyond 19 digits: below double precision
    };
    for (; i < n && isDigit (p[i]); ++i)
        take (p[i]);
    if (i < n && p[i] == '.')
        for (++i; i < n && isDigit (p[i]); ++i)
        {
            take (p[i]);
            ++fractionDigits;
        }
    int exponent = 0;
    if (i < n && (p[i] == 'e' || p[i] == 'E'))
    {
        ++i;
        bool negativeExponent = false;
        if (p[i] == '+' || p[i] == '-')
            negativeExponent = p[i++] == '-';
        for (; i < n && isDigit (p[i]); ++i)
            exponent = exponent * 10 + (p[i] - '0'); // at most 3 digits
        if (negativeExponent)
            exponent = -exponent;
    }
    // Dropped digits were integer or fraction digits that did not enter the mantissa: the mantissa
    // stands for value / 10^(dropped - fractionDigits).
    const int scale = exponent - fractionDigits + dropped;
    double v = 0.0;
    if (mantissa != 0)
    {
        if (mantissa < kTwoPow53 && scale >= -kMaxExactPow10 && scale <= kMaxExactPow10)
            v = scale >= 0 ? static_cast<double> (mantissa) * kExactPow10[scale]
                           : static_cast<double> (mantissa) / kExactPow10[-scale]; // exact operands: one rounding
        else
            v = scaleByPow10 (static_cast<double> (mantissa), scale);
    }
    return negative ? -v : v;
}

/** Length of the UTF-8 sequence at p (validated: no overlong forms, no surrogates, <= U+10FFFF), or 0. */
size_t utf8SequenceLength (const unsigned char* p, size_t n) noexcept
{
    const unsigned c = p[0];
    if (c < 0x80)
        return 1;
    const auto cont = [&] (size_t k) { return k < n && (p[k] & 0xC0u) == 0x80u; };
    if (c >= 0xC2 && c <= 0xDF)
        return cont (1) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF)
    {
        if (! cont (1) || ! cont (2))
            return 0;
        if (c == 0xE0 && p[1] < 0xA0)
            return 0; // overlong
        if (c == 0xED && p[1] >= 0xA0)
            return 0; // UTF-16 surrogate
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4)
    {
        if (! cont (1) || ! cont (2) || ! cont (3))
            return 0;
        if (c == 0xF0 && p[1] < 0x90)
            return 0; // overlong
        if (c == 0xF4 && p[1] >= 0x90)
            return 0; // above U+10FFFF
        return 4;
    }
    return 0;
}

void appendUtf8 (std::string& out, std::uint32_t cp)
{
    if (cp < 0x80)
        out += static_cast<char> (cp);
    else if (cp < 0x800)
    {
        out += static_cast<char> (0xC0u | (cp >> 6));
        out += static_cast<char> (0x80u | (cp & 0x3Fu));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char> (0xE0u | (cp >> 12));
        out += static_cast<char> (0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char> (0x80u | (cp & 0x3Fu));
    }
    else
    {
        out += static_cast<char> (0xF0u | (cp >> 18));
        out += static_cast<char> (0x80u | ((cp >> 12) & 0x3Fu));
        out += static_cast<char> (0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char> (0x80u | (cp & 0x3Fu));
    }
}

int hexValue (char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool read4Hex (const char* p, size_t n, size_t at, std::uint32_t& out) noexcept
{
    if (at + 4 > n)
        return false;
    out = 0;
    for (size_t k = 0; k < 4; ++k)
    {
        const int h = hexValue (p[at + k]);
        if (h < 0)
            return false;
        out = (out << 4) | static_cast<std::uint32_t> (h);
    }
    return true;
}
} // namespace

bool parseNumber (const char* text, size_t n, double& out) noexcept
{
    if (text == nullptr || n == 0 || numberTokenLength (text, n) != n)
        return false;
    out = convertNumber (text, n);
    return true;
}

int parseBoundedInt (std::string_view text, int lo, int hi, int fallback) noexcept
{
    size_t i = 0;
    const bool negative = ! text.empty() && text[0] == '-';
    if (negative)
        ++i;
    const size_t digits = text.size() - i;
    if (digits < 1 || digits > 9 || (digits > 1 && text[i] == '0'))
        return fallback;
    long long v = 0; // 9 digits fit with room to spare
    for (; i < text.size(); ++i)
    {
        if (! isDigit (text[i]))
            return fallback;
        v = v * 10 + (text[i] - '0');
    }
    if (negative)
        v = -v;
    if (v < lo || v > hi)
        return fallback;
    return static_cast<int> (v);
}

// ---------------------------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------------------------
void Document::clear() noexcept
{
    values.clear();
    items.clear();
    pool.clear();
    rootIndex = npos;
}

std::string_view Document::string (Index v) const noexcept
{
    if (type (v) != Type::String)
        return {};
    return { pool.data() + values[v].a, values[v].b };
}

size_t Document::size (Index v) const noexcept
{
    const auto t = type (v);
    return t == Type::Array || t == Type::Object ? values[v].b : 0u;
}

Document::Index Document::element (Index v, size_t i) const noexcept
{
    if (type (v) != Type::Array || i >= values[v].b)
        return npos;
    return items[values[v].a + i].value;
}

Document::Index Document::member (Index v, std::string_view key) const noexcept
{
    if (type (v) != Type::Object)
        return npos;
    const auto first = items.begin() + static_cast<std::ptrdiff_t> (values[v].a);
    const auto last = first + static_cast<std::ptrdiff_t> (values[v].b);
    const auto it = std::lower_bound (first, last, key, [this] (const Item& item, std::string_view k) { return keyOf (item) < k; });
    return it != last && keyOf (*it) == key ? it->value : npos;
}

std::string_view Document::memberKey (Index v, size_t i) const noexcept
{
    if (type (v) != Type::Object || i >= values[v].b)
        return {};
    return keyOf (items[values[v].a + i]);
}

Document::Index Document::memberValue (Index v, size_t i) const noexcept
{
    if (type (v) != Type::Object || i >= values[v].b)
        return npos;
    return items[values[v].a + i].value;
}

bool Document::parse (const char* text, size_t numBytes, const Limits& limits, std::string& error)
{
    try
    {
        if (parseImpl (text, numBytes, limits, error))
            return true;
    }
    catch (const std::bad_alloc&)
    {
        error = "out of memory";
    }
    catch (...)
    {
        error = "internal error";
    }
    clear();
    return false;
}

bool Document::parseImpl (const char* p, size_t n, const Limits& limits, std::string& error)
{
    clear();
    if (p == nullptr || n == 0)
    {
        error = "the text is empty";
        return false;
    }
    if (n > limits.maxInputBytes)
    {
        error = "the text is too large";
        return false;
    }
    // Offsets and counts are 32-bit; the caps keep them far below that.
    if (n > 0x7fffffffu)
    {
        error = "the text is too large";
        return false;
    }

    struct Frame
    {
        Index container;
        bool isObject;
        size_t pendingStart; // first of this container's items in `pending`
        Item keyInParent;    // the key this container is stored under (when its parent is an object)
    };
    std::vector<Frame> stack;
    stack.reserve (static_cast<size_t> (std::max (1, limits.maxDepth)));
    std::vector<Item> pending; // items of every open container, innermost last
    std::vector<std::uint32_t> order; // scratch: sorting an object's members

    size_t i = 0;
    const auto fail = [&] (const char* what) {
        error = std::string (what) + " at byte " + std::to_string (i);
        return false;
    };
    const auto skipSpace = [&] {
        while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r'))
            ++i;
    };
    const auto newValue = [&] (Type t) -> Index {
        if (values.size() >= static_cast<size_t> (limits.maxValues))
            return npos;
        values.push_back ({ t, 0.0, 0, 0 });
        return static_cast<Index> (values.size() - 1);
    };
    // A string starting at p[i] == '"': decoded into the pool. Returns false (with error) when invalid.
    std::uint32_t strOffset = 0, strLength = 0;
    const auto readString = [&]() -> bool {
        ++i; // opening quote
        const size_t start = pool.size();
        for (;;)
        {
            if (i >= n)
                return fail ("unterminated text");
            const auto c = static_cast<unsigned char> (p[i]);
            if (c == '"')
            {
                ++i;
                break;
            }
            if (c < 0x20)
                return fail ("a control character inside text");
            if (c == '\\')
            {
                if (i + 1 >= n)
                    return fail ("unterminated text");
                const char e = p[i + 1];
                i += 2;
                switch (e)
                {
                    case '"': pool += '"'; break;
                    case '\\': pool += '\\'; break;
                    case '/': pool += '/'; break;
                    case 'b': pool += '\b'; break;
                    case 'f': pool += '\f'; break;
                    case 'n': pool += '\n'; break;
                    case 'r': pool += '\r'; break;
                    case 't': pool += '\t'; break;
                    case 'u':
                    {
                        std::uint32_t cp = 0;
                        if (! read4Hex (p, n, i, cp))
                            return fail ("an invalid \\u escape");
                        i += 4;
                        if (cp >= 0xDC00 && cp <= 0xDFFF)
                            return fail ("a lone low surrogate");
                        if (cp >= 0xD800 && cp <= 0xDBFF)
                        {
                            std::uint32_t low = 0;
                            if (i + 2 > n || p[i] != '\\' || p[i + 1] != 'u' || ! read4Hex (p, n, i + 2, low) || low < 0xDC00
                                || low > 0xDFFF)
                                return fail ("a lone high surrogate");
                            i += 6;
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                        }
                        if (cp == 0)
                            return fail ("a NUL character inside text");
                        appendUtf8 (pool, cp);
                        break;
                    }
                    default: return fail ("an invalid escape");
                }
            }
            else
            {
                const auto len = utf8SequenceLength (reinterpret_cast<const unsigned char*> (p + i), n - i);
                if (len == 0)
                    return fail ("invalid UTF-8");
                pool.append (p + i, len);
                i += len;
            }
            if (pool.size() - start > limits.maxStringBytes)
                return fail ("text too long");
        }
        strOffset = static_cast<std::uint32_t> (start);
        strLength = static_cast<std::uint32_t> (pool.size() - start);
        return true;
    };

    enum class State
    {
        ExpectValue,      // a value must come
        ArrayValueOrEnd,  // after '['
        ObjectKeyOrEnd,   // after '{'
        ObjectKey,        // after ',' in an object
        AfterValue,       // a value inside a container ended: ',' or the closing bracket
        Done
    };
    State state = State::ExpectValue;
    Item pendingKey;

    // Attaches a finished value to its container (or makes it the root).
    const auto attach = [&] (Index v) {
        if (stack.empty())
        {
            rootIndex = v;
            state = State::Done;
            return true;
        }
        const auto& top = stack.back();
        const size_t count = pending.size() - top.pendingStart;
        if (top.isObject ? count >= static_cast<size_t> (limits.maxMembers) : count >= static_cast<size_t> (limits.maxElements))
            return fail (top.isObject ? "too many members in an object" : "too many elements in a list");
        Item item = top.isObject ? pendingKey : Item {};
        item.value = v;
        pending.push_back (item);
        state = State::AfterValue;
        return true;
    };

    const auto close = [&] {
        const Frame f = stack.back();
        stack.pop_back();
        const size_t count = pending.size() - f.pendingStart;
        const size_t first = items.size();
        if (f.isObject)
        {
            order.resize (count);
            for (size_t k = 0; k < count; ++k)
                order[k] = static_cast<std::uint32_t> (f.pendingStart + k);
            std::sort (order.begin(), order.end(), [&] (std::uint32_t a, std::uint32_t b) { return keyOf (pending[a]) < keyOf (pending[b]); });
            for (size_t k = 0; k < count; ++k)
            {
                if (k > 0 && keyOf (pending[order[k]]) == keyOf (pending[order[k - 1]]))
                {
                    error = "the key \"" + std::string (keyOf (pending[order[k]]).substr (0, 40)) + "\" appears twice in an object (near byte "
                            + std::to_string (i) + ")";
                    return false;
                }
                items.push_back (pending[order[k]]);
            }
        }
        else
            items.insert (items.end(), pending.begin() + static_cast<std::ptrdiff_t> (f.pendingStart), pending.end());
        pending.resize (f.pendingStart);
        values[f.container].a = static_cast<std::uint32_t> (first);
        values[f.container].b = static_cast<std::uint32_t> (count);
        ++i; // the closing bracket
        pendingKey = f.keyInParent; // keys read inside the container overwrote it
        return attach (f.container);
    };

    for (;;)
    {
        skipSpace();
        if (state == State::Done)
        {
            if (i != n)
                return fail ("unexpected text after the end");
            return true;
        }
        if (i >= n)
            return fail ("unexpected end of the text (truncated?)");
        const char c = p[i];
        switch (state)
        {
            case State::ArrayValueOrEnd:
                if (c == ']')
                {
                    if (! close())
                        return false;
                    continue;
                }
                state = State::ExpectValue;
                continue;

            case State::ObjectKeyOrEnd:
                if (c == '}')
                {
                    if (! close())
                        return false;
                    continue;
                }
                [[fallthrough]];
            case State::ObjectKey:
                if (c != '"')
                    return fail ("expected a key in double quotes");
                if (! readString())
                    return false;
                pendingKey = { strOffset, strLength, npos };
                skipSpace();
                if (i >= n || p[i] != ':')
                    return fail ("expected ':' after a key");
                ++i;
                state = State::ExpectValue;
                continue;

            case State::AfterValue:
            {
                const bool inObject = stack.back().isObject;
                if (c == ',')
                {
                    ++i;
                    state = inObject ? State::ObjectKey : State::ExpectValue;
                    continue;
                }
                if (c == (inObject ? '}' : ']'))
                {
                    if (! close())
                        return false;
                    continue;
                }
                return fail (inObject ? "expected ',' or '}'" : "expected ',' or ']'");
            }

            case State::ExpectValue:
            {
                if (c == '{' || c == '[')
                {
                    if (static_cast<int> (stack.size()) >= limits.maxDepth)
                        return fail ("nested too deeply");
                    const Index v = newValue (c == '{' ? Type::Object : Type::Array);
                    if (v == npos)
                        return fail ("too many values");
                    stack.push_back ({ v, c == '{', pending.size(), pendingKey });
                    ++i;
                    state = c == '{' ? State::ObjectKeyOrEnd : State::ArrayValueOrEnd;
                    continue;
                }
                Index v = npos;
                if (c == '"')
                {
                    if (! readString())
                        return false;
                    v = newValue (Type::String);
                    if (v != npos)
                    {
                        values[v].a = strOffset;
                        values[v].b = strLength;
                    }
                }
                else if (c == '-' || isDigit (c))
                {
                    const size_t len = numberTokenLength (p + i, n - i);
                    if (len == 0)
                        return fail ("an invalid number (or one with too many digits)");
                    v = newValue (Type::Number);
                    if (v != npos)
                        values[v].num = convertNumber (p + i, len);
                    i += len;
                }
                else
                {
                    struct Literal
                    {
                        const char* text;
                        size_t length;
                        Type type;
                        std::uint32_t flag;
                    };
                    static constexpr Literal literals[] = { { "true", 4, Type::Bool, 1 }, { "false", 5, Type::Bool, 0 }, { "null", 4, Type::Null, 0 } };
                    const Literal* match = nullptr;
                    for (const auto& l : literals)
                        if (n - i >= l.length && std::string_view (p + i, l.length) == std::string_view (l.text, l.length))
                            match = &l;
                    if (match == nullptr)
                        return fail ("expected a value");
                    // The literal must end there ("nullx", "true1").
                    const size_t end = i + match->length;
                    if (end < n && ((p[end] >= 'a' && p[end] <= 'z') || (p[end] >= 'A' && p[end] <= 'Z') || isDigit (p[end])))
                        return fail ("expected a value");
                    v = newValue (match->type);
                    if (v != npos)
                        values[v].a = match->flag;
                    i = end;
                }
                if (v == npos)
                    return fail ("too many values");
                if (! attach (v))
                    return false;
                continue;
            }

            case State::Done: break; // handled above
        }
    }
}
} // namespace vs::presets::json
