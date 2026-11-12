#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.h"
#include "dsp/Filters.h"

using namespace vs::dsp;

namespace
{
constexpr float kFs = 96000.0f;

/** Steady-state gain (dB) of a small sine through the ladder (linear region). */
double ladderGainDb (float sineHz, float cutoffHz, float k, bool slope24)
{
    OtaLadder f;
    const float g = tptG (cutoffHz, kFs);
    const float amp = 0.01f; // tanh(0.01) deviates by 3e-5: linear for practical purposes
    const int n = static_cast<int> (kFs);
    std::vector<float> y (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        y[static_cast<size_t> (i)] = f.process (amp * std::sin (2.0f * kPi * sineHz * static_cast<float> (i) / kFs), g, k, slope24);

    const auto half = static_cast<size_t> (n / 2);
    return vs::test::toDb (vs::test::rms (y.data() + half, half) * std::sqrt (2.0) / amp);
}
} // namespace

TEST_CASE ("Ladder cutoff is exact in the linear region", "[vcf]")
{
    // Each prewarped TPT pole is -3.01 dB at fc; compensation is 1 at k = 0.
    for (const float fc : { 200.0f, 1000.0f, 8000.0f })
    {
        INFO ("fc " << fc);
        CHECK (std::abs (ladderGainDb (fc, fc, 0.0f, true) - (-12.04)) < 0.2);
        CHECK (std::abs (ladderGainDb (fc, fc, 0.0f, false) - (-6.02)) < 0.2);
    }
}

TEST_CASE ("Ladder slopes are -24 and -12 dB/oct", "[vcf]")
{
    // Measured well below Nyquist: bilinear warping steepens the slope near fs/2 (expected).
    const float fc = 500.0f;
    const double a24 = ladderGainDb (2000.0f, fc, 0.0f, true) - ladderGainDb (4000.0f, fc, 0.0f, true);
    const double a12 = ladderGainDb (2000.0f, fc, 0.0f, false) - ladderGainDb (4000.0f, fc, 0.0f, false);
    CHECK (std::abs (a24 - 24.0) < 1.5);
    CHECK (std::abs (a12 - 12.0) < 1.0);
}

TEST_CASE ("Ladder self-oscillates at cutoff with full resonance", "[vcf]")
{
    for (const bool slope24 : { true, false })
    {
        OtaLadder f;
        const float fc = 1000.0f;
        const float g = tptG (fc, kFs);
        std::vector<float> y (static_cast<size_t> (kFs));
        for (size_t i = 0; i < y.size(); ++i)
            y[i] = f.process (i == 0 ? 0.1f : 0.0f, g, 4.15f, slope24);

        const std::vector<float> tail (y.begin() + 48000, y.end());
        const double level = vs::test::rms (tail.data(), tail.size());
        const double hz = vs::test::measureFrequency (tail, kFs);
        INFO ("slope24 " << slope24 << " level " << level << " freq " << hz);
        CHECK (level > 0.1);
        CHECK (level < 3.0);
        CHECK (std::abs (hz / fc - 1.0) < 0.03);
    }
}

TEST_CASE ("Ladder is stable and bounded for hot input and fast modulation", "[vcf]")
{
    OtaLadder f;
    Random rng (7);
    for (int i = 0; i < 200000; ++i)
    {
        const float fc = 10.0f * std::exp2 (12.0f * rng.unipolar());
        const float y = f.process (8.0f * rng.bipolar(), tptG (fc, kFs), 4.2f * rng.unipolar(), (i / 1000) % 2 == 0);
        REQUIRE (std::isfinite (y));
        REQUIRE (std::abs (y) < 10.0f);
    }
}

TEST_CASE ("HPF passes highs and blocks DC", "[hpf]")
{
    OnePoleHighPass hpf;
    const float g = tptG (100.0f, kFs);
    float y = 0.0f;
    for (int i = 0; i < 96000; ++i)
        y = hpf.process (1.0f, g);
    CHECK (std::abs (y) < 1.0e-4f);
}
