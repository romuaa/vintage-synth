#pragma once

// A small strict JSON reader for preset files (untrusted input). It replaces juce::JSON for
// reading because that parser accepts more than JSON ('single-quoted' strings), overflows a signed
// int on long digit strings, recurses without a depth limit, keeps duplicate keys with a linear
// search per insertion (quadratic time) and has no memory bound.
//
// What it accepts (RFC 8259, minus what presets never need):
//   - objects, arrays, double-quoted strings with the standard escapes (\" \\ \/ \b \f \n \r \t
//     and \uXXXX, surrogate pairs combined; a lone surrogate or \u0000 is an error),
//   - true, false, null,
//   - numbers in the grammar  -?(0|[1-9][0-9]{0,8})(\.[0-9]{1,20})?([eE][+-]?[0-9]{1,3})?
//     (at most 9 integer digits, 20 fraction digits and a 3-digit exponent: nothing a preset
//     needs is longer, and nothing longer can make the conversion misbehave),
//   - white space: space, tab, CR, LF. Text must be valid UTF-8 (checked inside strings; outside
//     them only ASCII tokens are allowed). Nothing may follow the top-level value.
// Duplicate keys in an object are an error.
//
// Bounds: input size, nesting depth, members per object, elements per array, values in the whole
// document and bytes per string are capped (Limits). Parsing is iterative (an explicit stack no
// deeper than maxDepth) and runs in O(n log m) time for n bytes and at most m members per object
// (each object's keys are sorted once, to find duplicates and to look members up). Memory is
// bounded by the caps, not by the input's shape. No exception leaves parse(): an allocation
// failure is reported as an error.
//
// The result is a flat in-memory tree (values in one vector, object members sorted by key).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vs::presets::json
{
struct Limits
{
    size_t maxInputBytes = 1u << 20;
    int maxDepth = 12;
    int maxMembers = 4096;        // per object
    int maxElements = 4096;       // per array
    int maxValues = 65536;        // in the whole document
    size_t maxStringBytes = 4096; // one decoded string (keys too)
};

enum class Type : std::uint8_t
{
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

class Document
{
public:
    using Index = std::uint32_t;
    static constexpr Index npos = 0xffffffffu;

    /** Parses numBytes of UTF-8 text. Returns false and sets `error` (with the byte offset) when the
        text is not accepted; the document is then empty. Never throws. */
    bool parse (const char* text, size_t numBytes, const Limits& limits, std::string& error);

    bool empty() const noexcept { return rootIndex == npos; }
    Index root() const noexcept { return rootIndex; }
    Type type (Index v) const noexcept { return v < values.size() ? values[v].type : Type::Null; }
    bool isNumber (Index v) const noexcept { return type (v) == Type::Number; }
    double number (Index v) const noexcept { return isNumber (v) ? values[v].num : 0.0; }
    bool boolean (Index v) const noexcept { return type (v) == Type::Bool && values[v].a != 0; }
    /** The decoded UTF-8 text of a string value (empty for anything else). */
    std::string_view string (Index v) const noexcept;
    /** Elements of an array, members of an object, 0 for anything else. */
    size_t size (Index v) const noexcept;
    /** Element i of an array (npos when out of range or not an array). */
    Index element (Index v, size_t i) const noexcept;
    /** The value of the member with this key (npos when absent or not an object). */
    Index member (Index v, std::string_view key) const noexcept;
    /** Member i of an object, in key order. */
    std::string_view memberKey (Index v, size_t i) const noexcept;
    Index memberValue (Index v, size_t i) const noexcept;
    /** Values in the document (all levels). */
    size_t numValues() const noexcept { return values.size(); }

private:
    struct Value
    {
        Type type = Type::Null;
        double num = 0.0;
        std::uint32_t a = 0, b = 0; // string: pool offset, length; array/object: first item, count; bool: a
    };
    struct Item
    {
        std::uint32_t keyOffset = 0, keyLength = 0; // objects only
        Index value = npos;
    };

    bool parseImpl (const char* text, size_t n, const Limits& limits, std::string& error);
    void clear() noexcept;
    std::string_view keyOf (const Item& item) const noexcept { return { pool.data() + item.keyOffset, item.keyLength }; }

    std::vector<Value> values;
    std::vector<Item> items;
    std::string pool;
    Index rootIndex = npos;
};

/** Converts exactly one number token in the grammar above (the whole of [text, text + n)).
    Locale-independent. A value too large for a double becomes +-infinity (callers treat it as not
    finite); a value too small becomes 0. Returns false if the text is not exactly one valid token. */
bool parseNumber (const char* text, size_t n, double& out) noexcept;

/** A whole decimal integer in [lo, hi]: an optional '-', then 1 to 9 digits, nothing else (no
    spaces, no '+', no leading zeros except "0"). Returns `fallback` otherwise. Locale-independent. */
int parseBoundedInt (std::string_view text, int lo, int hi, int fallback) noexcept;
} // namespace vs::presets::json
