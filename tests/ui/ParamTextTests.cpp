// Host-visible time text of the delay, reverb, LFO and envelope parameters: whole milliseconds from
// 10 ms to 1 s, one decimal below 10 ms, two decimals in seconds; text -> value -> text is stable.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vintage/PluginProcessor.h"
#include "common/fx/SoftLimiter.h"
#include "common/params/ParamFactory.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <string>
#include <vector>

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

namespace
{
std::vector<juce::String> hostileTexts()
{
    std::vector<juce::String> t { "nan", "NaN", "-nan", "+nan", "nan(0x1)", "inf", "-inf", "Inf", "infinity", "-Infinity",
                                  "1e999", "-1e999", "1e999 ms", "0x1p3", "0x1p9999", "", " ", "abc", "1e", "--5", "5 5",
                                  "nan %", "inf Hz", "NaN ms", "- inf st", ".", "-", "+", "1e-999", "e5" };
    t.push_back (juce::String (std::string (10000, '9')));
    t.push_back ("-" + juce::String (std::string (10000, '9')));
    t.push_back (juce::String (std::string (10000, '9')) + " ms");
    t.push_back (juce::String (std::string (5000, '1')) + "." + juce::String (std::string (5000, '7')));
    const char withNul[] { '1', '2', '\0', '3', 'n', 'a', 'n' };
    t.push_back (juce::String::fromUTF8 (withNul, static_cast<int> (sizeof (withNul))));
    return t;
}
} // namespace

TEST_CASE ("Hostile parameter text never gives a non-finite or out-of-range value (every parameter)", "[params][text][hostile]")
{
    juce::ScopedJuceInitialiser_GUI init;
    vs::VintageSynthProcessor proc;
    const auto texts = hostileTexts();

    int count = 0;
    for (auto* p : proc.getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            ++count;
            for (const auto& text : texts)
            {
                const float v = r->getValueForText (text);
                INFO (r->paramID.toStdString() << " text '" << text.substring (0, 40).toStdString() << "' value " << v);
                CHECK (std::isfinite (v));
                CHECK (v >= 0.0f);
                CHECK (v <= 1.0f);
            }
        }
    CHECK (count == 172);
}

TEST_CASE ("Ordinary parameter text still parses as before", "[params][text]")
{
    // Display::* parsers: unit suffix or none, spaces, out-of-range values clamp.
    CHECK (fromText (Display::Percent, " 50 % ") == Catch::Approx (0.5f));
    CHECK (fromText (Display::Percent, "50") == Catch::Approx (0.5f));
    CHECK (fromText (Display::Panel10, "7.5") == Catch::Approx (0.75f));
    CHECK (fromText (Display::Cents, "-50 ct") == Catch::Approx (-0.5f));
    CHECK (fromText (Display::Pan, "L 50") == Catch::Approx (-0.5f));
    CHECK (fromText (Display::Pan, "R 30") == Catch::Approx (0.3f));
    CHECK (fromText (Display::Balance, "Upper 100") == Catch::Approx (1.0f));
    CHECK (fromText (Display::PortaTime, "off") == Catch::Approx (0.0f).margin (1e-9));
    CHECK (fromText (Display::EnvTime, "45 ms") == Catch::Approx (map::envTimeFromSec (0.045f)));
    CHECK (fromText (Display::DelayTime, "1.2 s") == Catch::Approx (map::delayTimeFromSec (1.2f)));
    CHECK (fromText (Display::CutoffHz, "1.5 kHz") == Catch::Approx (map::cutoffFromHz (1500.0f)));
    CHECK (fromText (Display::Bpm, "140") == Catch::Approx (140.0f));

    // Through the real parameters: normalised value of the typed text.
    juce::ScopedJuceInitialiser_GUI init;
    vs::VintageSynthProcessor proc;
    const auto normalised = [&] (const char* id, const char* text) {
        auto* r = dynamic_cast<juce::RangedAudioParameter*> (proc.getParameters()[0]);
        for (auto* p : proc.getParameters())
            if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (p); q != nullptr && q->paramID == id)
                r = q;
        return r->getValueForText (text);
    };
    CHECK (normalised ("seq.step01.velocity", "50 %") == Catch::Approx (0.5f));
    CHECK (normalised ("seq.step01.velocity", "250") == Catch::Approx (1.0f)); // clamps
    CHECK (normalised ("seq.step01.velocity", "-3") == Catch::Approx (0.0f));
    CHECK (normalised ("arp.tempo", " 160 bpm ") == Catch::Approx ((160.0f - 20.0f) / 280.0f));
    CHECK (normalised ("arp.tempo", "1000") == Catch::Approx (1.0f));
    CHECK (normalised ("master.limiter.ceiling", "-3.0") == Catch::Approx ((-3.0f - fx::SoftLimiter::kMinCeilingDb) / (fx::SoftLimiter::kMaxCeilingDb - fx::SoftLimiter::kMinCeilingDb)).margin (1e-4));
    CHECK (normalised ("master.volume", "-12 dB") == Catch::Approx ((-12.0f + 60.0f) / 66.0f).margin (1e-3));
    CHECK (normalised ("seq.step01.note", "+7 st") == Catch::Approx ((7.0f + 24.0f) / 48.0f));
}
