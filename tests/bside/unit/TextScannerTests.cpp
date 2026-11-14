#include "bside/text/TextScanner.h"

#include <catch2/catch_test_macros.hpp>

using namespace vs::bside::text;

namespace
{
bool bad (const std::string& s, int& line, int& col)
{
    const char* why = nullptr;
    return findInvalidByte (s, line, col, why);
}
} // namespace

TEST_CASE ("text: valid text passes the byte check", "[bside][text]")
{
    int l = 0, c = 0;
    CHECK_FALSE (bad ("", l, c));
    CHECK_FALSE (bad ("abc\n\tdef\r\nsecond\n", l, c));
    CHECK_FALSE (bad ("# caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xb9\n", l, c));
    CHECK_FALSE (bad ("\xf4\x8f\xbf\xbf", l, c));            // U+10FFFF
    CHECK_FALSE (bad ("\xed\x9f\xbf \xee\x80\x80", l, c));    // U+D7FF, U+E000
}

TEST_CASE ("text: invalid bytes are found with their position", "[bside][text]")
{
    int l = 0, c = 0;
    const char* why = nullptr;
    REQUIRE (findInvalidByte (std::string ("ab\ncd\0ef", 8), l, c, why));
    CHECK (l == 2);
    CHECK (c == 3);
    CHECK (std::string (why) == "control character");
    CHECK (bad ("x\x7f", l, c));
    CHECK (bad ("a\x01", l, c));
    CHECK (bad ("a\rb", l, c));               // stray CR
    CHECK (bad ("a\r", l, c));                // CR at the very end
    CHECK (bad ("\x1b[0m", l, c));
    CHECK (bad ("\x80", l, c));               // continuation byte alone
    CHECK (bad ("\xc0\x80", l, c));           // overlong
    CHECK (bad ("\xc1\xbf", l, c));
    CHECK (bad ("\xe0\x80\x80", l, c));       // overlong
    CHECK (bad ("\xf0\x80\x80\x80", l, c));   // overlong
    CHECK (bad ("\xed\xa0\x80", l, c));       // surrogate
    CHECK (bad ("\xf4\x90\x80\x80", l, c));   // above U+10FFFF
    CHECK (bad ("\xf5\x80\x80\x80", l, c));
    CHECK (bad ("\xff", l, c));
    CHECK (bad ("\xc3", l, c));               // truncated at the end
    CHECK (bad ("\xe2\x82", l, c));
    CHECK (bad ("\xe2\x82\x28", l, c));       // bad continuation
    CHECK (bad ("a\n\xf0\x9f\x8e", l, c));
    CHECK (l == 2);
    CHECK (c == 1);
}

TEST_CASE ("text: line iteration", "[bside][text]")
{
    const auto collect = [] (std::string_view t) {
        Lines ls (t);
        std::vector<std::string> out;
        std::string_view line;
        int n = 0, last = 0;
        while (ls.next (line, n))
        {
            CHECK (n == last + 1);
            last = n;
            out.emplace_back (line);
        }
        return out;
    };
    CHECK (collect ("").empty());
    CHECK (collect ("\n") == std::vector<std::string> { "" });
    CHECK (collect ("a\nb") == std::vector<std::string> { "a", "b" });
    CHECK (collect ("a\nb\n") == std::vector<std::string> { "a", "b" });
    CHECK (collect ("a\r\nb\r\n\r\n") == std::vector<std::string> { "a", "b", "" });
    CHECK (collect ("\n\nx") == std::vector<std::string> { "", "", "x" });
}

TEST_CASE ("text: numbers", "[bside][text]")
{
    std::uint64_t v = 0;
    CHECK (parseDecimal ("0", 10, v) == NumberResult::ok);
    CHECK (v == 0);
    CHECK (parseDecimal ("10", 10, v) == NumberResult::ok);
    CHECK (v == 10);
    CHECK (parseDecimal ("11", 10, v) == NumberResult::outOfRange);
    CHECK (parseDecimal ("", 10, v) == NumberResult::notNumber);
    CHECK (parseDecimal ("-1", 10, v) == NumberResult::notNumber);
    CHECK (parseDecimal ("+1", 10, v) == NumberResult::notNumber);
    CHECK (parseDecimal ("1 ", 10, v) == NumberResult::notNumber);
    CHECK (parseDecimal ("00000000000000000000001", 10, v) == NumberResult::ok); // leading zeros are free
    CHECK (parseDecimal ("10000000000000000000000", 10, v) == NumberResult::outOfRange); // 23 digits: never wraps
    CHECK (parseDecimal ("99999999999999999999", ~0ull, v) == NumberResult::outOfRange);
    CHECK (parseDecimal ("1099511627776", 1ull << 40, v) == NumberResult::ok);
    CHECK (v == (1ull << 40));
    CHECK (parseHex ("ff", 2, 255, v) == NumberResult::ok);
    CHECK (v == 255);
    CHECK (parseHex ("Ab", 2, 255, v) == NumberResult::ok);
    CHECK (v == 0xab);
    CHECK (parseHex ("100", 2, 4095, v) == NumberResult::outOfRange);
    CHECK (parseHex ("g", 2, 255, v) == NumberResult::notNumber);
    CHECK (parseHex ("", 2, 255, v) == NumberResult::notNumber);
    CHECK (parseHex ("0x1", 4, 255, v) == NumberResult::notNumber);
    CHECK (parseHex ("fff", 3, 0xff, v) == NumberResult::outOfRange);
    CHECK (parseHex ("ffffffffffffffffff", 99, ~0ull, v) == NumberResult::outOfRange);
}

TEST_CASE ("text: the error list keeps eight and counts the rest", "[bside][text]")
{
    ErrorList e;
    CHECK (e.empty());
    CHECK (e.format().empty());
    e.add (3, 5, "bad thing", "a good thing");
    e.add (4, 1, "no expectation");
    CHECK (e.total() == 2);
    CHECK (e.format() == "line 3, column 5: bad thing (expected a good thing)\nline 4, column 1: no expectation");
    for (int i = 0; i < 20; ++i)
        e.add (10 + i, 1, "x");
    CHECK (e.total() == 22);
    CHECK (e.stored().size() == 8);
    const auto f = e.format();
    CHECK (f.substr (f.rfind ('\n') + 1) == "and 14 more");
}

TEST_CASE ("text: cursor tokens", "[bside][text]")
{
    Cursor c { "  wave tri+gate,x # c", 0 };
    CHECK (c.skipWs());
    CHECK (c.column() == 3);
    CHECK (c.token() == "wave");
    CHECK_FALSE (c.skipWs() == false);
    CHECK (c.token() == "tri+gate");
    CHECK (c.peek() == ',');
    CHECK (c.token().empty());
    ++c.i;
    CHECK (c.token() == "x");
    c.skipWs();
    CHECK (c.atEol());
    CHECK_FALSE (c.atEnd());
}
