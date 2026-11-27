// Exitium B-Side's per-part level, mix.level (docs/architecture.md 8.21.2, "One new parameter, first and alone"): a
// smoothed gain on the part's sum, before the one gain and the one conversion to float. The parameter's own table and
// the old-state checks are in ParamTests.cpp; the allocation check is the parameter churn of ShellTests.cpp.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "PartTestSupport.h"
#include "bside/PluginProcessor.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace bs_part_test;

namespace
{
bs::PartParams withLevel (float dB)
{
    bs::PartParams p;
    p.mixLevel = dB;
    return p;
}

double rms (const std::vector<float>& y, std::size_t from, std::size_t to)
{
    double s = 0.0;
    for (std::size_t i = from; i < to; ++i)
        s += static_cast<double> (y[i]) * static_cast<double> (y[i]);
    return std::sqrt (s / static_cast<double> (to - from));
}

bool identical (const std::vector<float>& a, const std::vector<float>& b, std::size_t from = 0)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = from; i < a.size(); ++i)
        if (std::memcmp (&a[i], &b[i], sizeof (float)) != 0)
            return false;
    return true;
}

#if defined(__APPLE__) && defined(__aarch64__)
/** FNV-1a over the bits of every sample. */
std::uint64_t fingerprint (const std::vector<float>& y)
{
    std::uint64_t h = 1469598103934665603ull;
    for (const float v : y)
    {
        std::uint32_t bits = 0;
        std::memcpy (&bits, &v, sizeof bits);
        for (int k = 0; k < 4; ++k)
        {
            h ^= (bits >> (8 * k)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
    return h;
}
#endif

std::vector<vs::NoteEvent> playing()
{
    return { on (0, 60), on (3000, 67, 0.6f), off (20000, 60), on (30000, 72), off (40000, 72) };
}
} // namespace

TEST_CASE ("Part level: at 0 dB the output is bit-identical to a part that never had the parameter set", "[bside][part][level][slow]")
{
    constexpr int kTotal = 48000;
    Rig reference ({ 48000.0, 2 }, bs::PartParams {});
    const auto a = reference.run (kTotal, playing());
    REQUIRE (peakOf (a) > 0.1);
    // The output of the part before the level existed (this render's fingerprint, taken with the gain not in the chain at
    // all): a gain of exactly 1.0 must not change a bit. Regenerated when kChipGain moved from 0.9739 to 1.2822 (8.21.2; it
    // was 0x1eec9dec60bde539 before): the fingerprint is of this render at the new gain, with the level at 0 dB.
    // (Taken on macOS arm64; the other targets' libm may differ in a last bit of the chip's tables, so they skip it.)
#if defined(__APPLE__) && defined(__aarch64__)
    INFO ("fingerprint " << fingerprint (a));
    CHECK (fingerprint (a) == 0xcc5c9c3f74e56c40ull);
#endif

    // an explicit 0 dB from the start, whatever the block length; and what a host hands over as 0 dB: the normalised
    // value 0.8 comes back from the range as 3.6e-7 dB, which is still exactly 0 dB
    for (const float zero : { 0.0f, 3.58e-7f, -3.58e-7f })
        for (const int block : { 512, 61 })
        {
            Rig explicitZero ({ 48000.0, 2 }, withLevel (zero));
            const auto b = explicitZero.run (kTotal, playing(), block);
            INFO ("level " << zero << " dB, block " << block);
            CHECK (identical (a, b));
        }

    // a level that was +6 dB and came back to 0 dB: from the end of the 5 ms ramp (240 host samples after the next, plus the decimator's delay
    // block's start) the output is the same up to the output rounding's error state (noise-shaped, so a one-step
    // difference of the past stays inside the float steps of the value; the chain before the gain does not know the level)
    constexpr int kSplit = 10240;
    std::vector<vs::NoteEvent> first, rest;
    for (auto e : playing())
    {
        if (e.offset < kSplit)
            first.push_back (e);
        else
        {
            e.offset -= kSplit;
            rest.push_back (e);
        }
    }
    const auto twoParts = [&] (float firstDb) {
        Rig rig ({ 48000.0, 2 }, withLevel (firstDb));
        auto y = rig.run (kSplit, first, 512);
        rig.set (bs::PartParams {});
        const auto tail = rig.run (kTotal - kSplit, rest, 512);
        y.insert (y.end(), tail.begin(), tail.end());
        return y;
    };
    const auto steady = twoParts (0.0f), returned = twoParts (6.0f);
    double worst = 0.0;
    std::size_t at = 0;
    for (std::size_t i = kSplit + 300; i < steady.size(); ++i)
        if (std::abs (static_cast<double> (steady[i]) - static_cast<double> (returned[i])) > worst)
        {
            worst = std::abs (static_cast<double> (steady[i]) - static_cast<double> (returned[i]));
            at = i;
        }
    INFO ("largest difference after the ramp " << worst << " at " << at);
    CHECK (worst < 1.0e-6);
    CHECK_FALSE (identical (steady, returned, kSplit)); // the ramp itself differs: the test can tell
}

TEST_CASE ("Part level: at -6, -12, -24 and +6 dB a held note's level moves by that amount within 0.05 dB", "[bside][part][level][slow]")
{
    Rig reference ({ 48000.0, 2 }, bs::PartParams {});
    const auto a = reference.run (48000, { on (0, 60) });
    const double base = rms (a, 14400, 43200);
    REQUIRE (base > 0.01);
    for (const float dB : { -6.0f, -12.0f, -24.0f, 6.0f })
    {
        Rig rig ({ 48000.0, 2 }, withLevel (dB));
        const auto y = rig.run (48000, { on (0, 60) });
        const double moved = db (rms (y, 14400, 43200) / base);
        INFO ("level " << dB << " dB: measured " << moved << " dB");
        CHECK (std::abs (moved - static_cast<double> (dB)) < 0.05);
    }
}

TEST_CASE ("Part level: a jump from 0 to -24 dB while a note sounds has no step; it is a 5 ms ramp", "[bside][part][level]")
{
    constexpr int kJump = 24000, kTotal = 36000;
    Rig reference ({ 48000.0, 2 }, bs::PartParams {});
    const auto a = reference.run (kTotal, { on (0, 60) });

    Rig rig ({ 48000.0, 2 }, bs::PartParams {});
    auto b = rig.run (kJump, { on (0, 60) });
    rig.set (withLevel (-24.0f));
    const auto tail = rig.run (kTotal - kJump, {});
    b.insert (b.end(), tail.begin(), tail.end());
    for (int i = 0; i < kJump; ++i)
        REQUIRE (std::memcmp (&a[static_cast<std::size_t> (i)], &b[static_cast<std::size_t> (i)], sizeof (float)) == 0);

    // the ratio to the render without the jump, where the reference is well away from zero
    const double floor = 0.3 * peakOf (a);
    const double final = std::pow (10.0, -24.0 / 20.0);
    double largestStep = 0.0, previous = 1.0;
    int counted = 0, inRamp = 0;
    bool havePrevious = true;
    for (int i = kJump; i < kTotal; ++i)
    {
        const double ref = static_cast<double> (a[static_cast<std::size_t> (i)]);
        if (std::abs (ref) < floor)
        {
            havePrevious = false;
            continue;
        }
        const double r = static_cast<double> (b[static_cast<std::size_t> (i)]) / ref;
        if (havePrevious)
            largestStep = std::max (largestStep, std::abs (r - previous));
        previous = r;
        havePrevious = true;
        ++counted;
        if (r > final * 1.05 && r < 0.99)
            ++inRamp;
        if (i > kJump + 400)
            CHECK (r == Catch::Approx (final).epsilon (0.01));
    }
    // 5 ms is 240 host samples: a straight ramp from 1 to 0.063 moves 0.0039 a sample; twice that is the allowance
    const double perSample = (1.0 - final) / 240.0;
    INFO ("largest step " << largestStep << " (ramp " << perSample << " a sample), " << inRamp << " ramp samples of " << counted);
    CHECK (largestStep <= 2.0 * perSample);
    CHECK (inRamp > 40); // it did ramp: the samples that lie on the way
}

TEST_CASE ("Part level: Lower's level does not change Upper's output (bit for bit); Upper's does not change Lower's", "[bside][part][level][shell][slow]")
{
    constexpr int kBlock = 256, kBlocks = 200, kLead = 60; // the mixer's pan ramp settles during the silent lead
    struct Out
    {
        std::vector<float> left, right;
    };
    const auto render = [&] (float lowerDb, float upperDb) {
        juce::ScopedJuceInitialiser_GUI init;
        bs::BSideProcessor proc;
        proc.setNonRealtime (false);
        proc.setPlayConfigDetails (0, 2, 48000.0, kBlock);
        proc.prepareToPlay (48000.0, kBlock);
        const auto set = [&] (const char* id, float plain) {
            auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
            REQUIRE (p != nullptr);
            p->setValueNotifyingHost (p->convertTo0to1 (plain));
        };
        set ("key.mode", 1.0f);          // Dual: both parts play every note
        set ("lower.mix.pan", -1.0f);    // Lower on the left only, Upper on the right only
        set ("upper.mix.pan", 1.0f);
        set ("master.limiter", 0.0f);
        set ("lower.mix.level", lowerDb);
        set ("upper.mix.level", upperDb);
        Out out;
        juce::AudioBuffer<float> buffer (2, kBlock);
        for (int b = 0; b < kBlocks; ++b)
        {
            juce::MidiBuffer m;
            if (b == kLead)
                m.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 5);
            if (b == kLead + 50)
                m.addEvent (juce::MidiMessage::noteOn (1, 67, static_cast<juce::uint8> (90)), 17);
            buffer.clear();
            proc.processBlock (buffer, m);
            for (int i = 0; i < kBlock; ++i)
            {
                out.left.push_back (buffer.getSample (0, i));
                out.right.push_back (buffer.getSample (1, i));
            }
        }
        return out;
    };
    const auto base = render (0.0f, 0.0f);
    REQUIRE (peakOf (base.left) > 0.05);
    REQUIRE (peakOf (base.right) > 0.05);
    const auto lowerMoved = render (-12.0f, 0.0f);
    {
        double worst = 0.0;
        std::size_t first = 0;
        for (std::size_t i = 0; i < base.right.size(); ++i)
            if (std::abs (static_cast<double> (base.right[i]) - static_cast<double> (lowerMoved.right[i])) > worst)
            {
                worst = std::abs (static_cast<double> (base.right[i]) - static_cast<double> (lowerMoved.right[i]));
                first = i;
            }
        INFO ("worst right difference " << worst << " at " << first);
        CHECK (identical (base.right, lowerMoved.right)); // Upper is untouched
    }
    CHECK_FALSE (identical (base.left, lowerMoved.left));
    CHECK (db (rms (lowerMoved.left, 30000, 38000) / rms (base.left, 30000, 38000)) == Catch::Approx (-12.0).margin (0.1));
    const auto upperMoved = render (0.0f, 6.0f);
    CHECK (identical (base.left, upperMoved.left)); // Lower is untouched
    CHECK_FALSE (identical (base.right, upperMoved.right));
}
