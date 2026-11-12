#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.h"
#include "vintage/dsp/Envelope.h"
#include "vintage/dsp/Lfo.h"

using namespace vs::dsp;

namespace
{
constexpr float kFs = 96000.0f;

int samplesUntil (Envelope& env, float sustain, auto predicate, int limit)
{
    for (int i = 1; i <= limit; ++i)
    {
        env.process (sustain);
        if (predicate (env))
            return i;
    }
    return -1;
}
} // namespace

TEST_CASE ("Envelope attack reaches the peak in the set time", "[env]")
{
    for (const float attack : { 0.005f, 0.1f, 2.0f })
    {
        Envelope env;
        env.setSampleRate (kFs);
        env.setTimes (attack, 0.5f, 0.5f);
        env.noteOn();
        const int n = samplesUntil (env, 0.5f, [] (const Envelope& e) { return e.getStage() == Envelope::Stage::Decay; },
                                    static_cast<int> (kFs * 5));
        INFO ("attack " << attack << " took " << static_cast<float> (n) / kFs);
        CHECK (std::abs (static_cast<float> (n) / kFs - attack) < attack * 0.01f + 2.0f / kFs);
    }
}

TEST_CASE ("Envelope decay and release cover 99% in the set time", "[env]")
{
    Envelope env;
    env.setSampleRate (kFs);
    const float decay = 0.3f, release = 0.8f, sustain = 0.4f;
    env.setTimes (0.001f, decay, release);
    env.noteOn();
    samplesUntil (env, sustain, [] (const Envelope& e) { return e.getStage() == Envelope::Stage::Decay; }, 100000);

    for (int i = 0; i < static_cast<int> (decay * kFs); ++i)
        env.process (sustain);
    CHECK (std::abs (env.getLevel() - (sustain + 0.01f * (1.0f - sustain))) < 0.002f);

    for (int i = 0; i < static_cast<int> (kFs); ++i)
        env.process (sustain); // settle
    env.noteOff();
    for (int i = 0; i < static_cast<int> (release * kFs); ++i)
        env.process (sustain);
    CHECK (std::abs (env.getLevel() - 0.01f * sustain) < 0.001f);

    // ... and ends (voice can be freed).
    const int n = samplesUntil (env, sustain, [] (const Envelope& e) { return ! e.isActive(); }, static_cast<int> (kFs * 10));
    CHECK (n > 0);
}

TEST_CASE ("Envelope retrigger continues from the current level", "[env]")
{
    Envelope env;
    env.setSampleRate (kFs);
    env.setTimes (0.01f, 0.1f, 0.5f);
    env.noteOn();
    for (int i = 0; i < 20000; ++i)
        env.process (0.7f);
    env.noteOff();
    for (int i = 0; i < 1000; ++i)
        env.process (0.7f);
    const float before = env.getLevel();
    env.noteOn();
    const float after = env.process (0.7f);
    CHECK (after >= before);
    CHECK (after - before < 0.02f);
}

TEST_CASE ("LFO rate is accurate", "[lfo]")
{
    for (const auto wave : { LfoWave::Sine, LfoWave::Triangle, LfoWave::Square })
    {
        Lfo lfo;
        lfo.prepare (kFs, 1);
        lfo.setWave (wave);
        lfo.setRate (5.0f);
        std::vector<float> x (static_cast<size_t> (kFs * 4));
        for (auto& s : x)
            s = lfo.process();
        CHECK (std::abs (vs::test::measureFrequency (x, kFs) - 5.0) < 0.01);
    }
}

TEST_CASE ("LFO delay holds, then fades in", "[lfo]")
{
    Lfo lfo;
    lfo.prepare (kFs, 1);
    lfo.setWave (LfoWave::Square);
    lfo.setRate (10.0f);
    lfo.setDelay (2.0f);
    lfo.trigger();

    float maxFirstSecond = 0.0f;
    for (int i = 0; i < static_cast<int> (kFs); ++i)
        maxFirstSecond = std::max (maxFirstSecond, std::abs (lfo.process()));
    CHECK (maxFirstSecond < 1.0e-3f);

    CHECK (std::abs (lfo.delayAmount()) < 0.01f);
    for (int i = 0; i < static_cast<int> (kFs / 2); ++i)
        lfo.process();
    CHECK (std::abs (lfo.delayAmount() - 0.5f) < 0.01f);
    for (int i = 0; i < static_cast<int> (kFs); ++i)
        lfo.process();
    CHECK (std::abs (lfo.delayAmount() - 1.0f) < 1.0e-6f);
}

TEST_CASE ("LFO outputs stay in [-1, 1] for all waves", "[lfo]")
{
    for (int w = 0; w < static_cast<int> (kNumLfoWaves); ++w)
    {
        Lfo lfo;
        lfo.prepare (kFs, 3);
        lfo.setWave (static_cast<LfoWave> (w));
        lfo.setRate (37.0f);
        for (int i = 0; i < 96000; ++i)
        {
            const float v = lfo.process();
            REQUIRE (v >= -1.0001f);
            REQUIRE (v <= 1.0001f);
        }
    }
}
