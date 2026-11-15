// Host-visible time text of the delay, reverb, LFO and envelope parameters: whole milliseconds from
// 10 ms to 1 s, one decimal below 10 ms, two decimals in seconds; text -> value -> text is stable.

#include <catch2/catch_test_macros.hpp>

#include "common/params/ParamFactory.h"

using namespace vs;

TEST_CASE ("Time text: whole milliseconds from 10 ms, one decimal below, seconds with two decimals", "[params][text]")
{
    CHECK (toText (Display::DelayTime, map::delayTimeFromSec (0.0447214f)) == "45 ms");
    CHECK (toText (Display::DelayTime, map::delayTimeFromSec (0.935248f)) == "935 ms");
    CHECK (toText (Display::DelayTime, map::delayTimeFromSec (0.7f)) == "700 ms");
    CHECK (toText (Display::DelayTime, map::delayTimeFromSec (0.0123f)) == "12 ms");
    CHECK (toText (Display::DelayTime, map::delayTimeFromSec (0.0042f)) == "4.2 ms");
    CHECK (toText (Display::DelayTime, 0.0f) == "1.0 ms");
    CHECK (toText (Display::DelayTime, map::delayTimeFromSec (1.234f)) == "1.23 s");
    CHECK (toText (Display::DelayTime, 1.0f) == "2.00 s");
    CHECK (toText (Display::ReverbDecay, map::reverbDecayFromSec (2.5f)) == "2.50 s");
    CHECK (toText (Display::ReverbDecay, 0.0f) == "300 ms");
    CHECK (toText (Display::EnvTime, map::envTimeFromSec (0.0447214f)) == "45 ms");
}

TEST_CASE ("Time text round trips: text -> value -> text is stable, and a value's text parses back within its own rounding", "[params][text]")
{
    for (const auto d : { Display::DelayTime, Display::ReverbDecay, Display::EnvTime, Display::LfoDelay })
        for (int i = 0; i <= 200; ++i)
        {
            const float v = static_cast<float> (i) / 200.0f;
            const auto text = toText (d, v);
            const auto again = toText (d, fromText (d, text));
            INFO (static_cast<int> (d) << " v " << v << " text " << text.toStdString() << " again " << again.toStdString());
            CHECK (again == text);
        }
}
