// S3a (docs/architecture.md 8.19.1): the chip's Filter and OutputStage on their own. The reference comparisons
// (corners, peaks, distortion, voice DC, modes) are in tests/bside/trace/; here: stability at every setting and
// internal rate of 4.4, writes every sample, decay after a loud input (no limit cycle, no denormal), block-size
// independence, no allocation, and the behaviours the contract names (signs, notch, resonance, the cutoff
// converter's steps, variation, volume DC).

#include "FuzzSupport.h"

#include "bside/chip/Filter.h"
#include "bside/chip/OutputStage.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace vs::bside::chip;

namespace
{
constexpr double kRates[] { 88200.0, 96000.0, 128000.0, 176400.0, 192000.0, 352800.0, 384000.0 };
constexpr double kPi = 3.14159265358979323846;

/** Uniform in [lo, hi). */
float uniform (bst::Rng& r, float lo, float hi)
{
    return lo + (hi - lo) * static_cast<float> (r.below (1u << 24)) / static_cast<float> (1u << 24);
}

/** Full scale at the filter's input (render units): three voices of +-0.5 plus their DC (OutputStage::filterBusLevel,
    Model A about 0.71 per voice at full envelope) and a margin. */
constexpr float kInLo = -2.0f, kInHi = 4.0f;

bool finiteAndBounded (float v, float bound) { return std::isfinite (v) && std::abs (v) < bound; }

/** Gain of the filter at `hz` (steady-state sine, after a settling time), relative to the input amplitude. */
double sineGain (Filter& f, double rate, double hz, double dc = 0.5)
{
    f.reset();
    const int settle = static_cast<int> (rate * 0.2), n = static_cast<int> (rate * 0.1);
    double s = 0.0, c = 0.0, mean = 0.0;
    std::vector<double> y (static_cast<std::size_t> (n));
    for (int i = 0; i < settle + n; ++i)
    {
        const double x = dc + 0.01 * std::sin (2.0 * kPi * hz * i / rate);
        const double o = f.process (static_cast<float> (x));
        if (i >= settle)
        {
            y[static_cast<std::size_t> (i - settle)] = o;
            mean += o;
        }
    }
    mean /= n;
    for (int i = 0; i < n; ++i)
    {
        const double ph = 2.0 * kPi * hz * (i + settle) / rate;
        s += (y[static_cast<std::size_t> (i)] - mean) * std::sin (ph);
        c += (y[static_cast<std::size_t> (i)] - mean) * std::cos (ph);
    }
    return 2.0 * std::sqrt (s * s + c * c) / n / 0.01;
}
} // namespace

TEST_CASE ("filter: finite and bounded at every setting and internal rate, with full-scale noise", "[filter][stability]")
{
    // every resonance, every mode combination, cutoffs from 0 to 2047 (Model A: across its converter's steps),
    // variation 0 / 0.5 / 1, every internal rate of 4.4: 3000 samples of full-scale noise each. Bound: 60 render
    // units (the largest output seen is about 13: Model B, resonance 15, band-pass, at its corner).
    constexpr float kBound = 60.0f;
    float largest = 0.0f;
    bst::Rng r (31);
    for (const Model m : { Model::A, Model::B })
        for (const double rate : kRates)
            for (const float var : { 0.0f, 0.5f, 1.0f })
            {
                if (m == Model::B && var > 0.0f)
                    continue; // variation is Model A's
                Filter f;
                f.prepare (m, rate, var);
                for (int res = 0; res < 16; ++res)
                    for (const float fc : { 0.0f, 1.0f, 200.5f, 511.0f, 512.0f, 1023.0f, 1024.0f, 1600.0f, 2047.0f })
                        for (std::uint8_t mode = 0; mode < 8; ++mode)
                        {
                            f.reset();
                            f.setCutoff (fc);
                            f.setResonance (res);
                            f.setMode (static_cast<std::uint8_t> (mode << 4));
                            for (int i = 0; i < 3000; ++i)
                            {
                                const float y = f.process (uniform (r, kInLo, kInHi));
                                largest = std::max (largest, std::abs (y));
                                if (! finiteAndBounded (y, kBound))
                                {
                                    INFO ("model " << (m == Model::A ? "A" : "B") << " rate " << rate << " variation " << var << " res " << res << " cutoff "
                                                   << fc << " mode " << int (mode) << " sample " << i << ": " << y);
                                    FAIL();
                                }
                            }
                        }
            }
    WARN ("largest filter output over all settings with full-scale noise: " << largest << " (bound " << kBound << ")");
}

TEST_CASE ("filter and output stage: a cutoff, resonance, mode or routing write every sample stays finite and bounded", "[filter][stability]")
{
    bst::Rng r (32);
    for (const Model m : { Model::A, Model::B })
        for (const double rate : kRates)
        {
            Filter f;
            f.prepare (m, rate, 0.5f);
            OutputStage o;
            o.prepare (m, rate, r.chance (50));
            for (int i = 0; i < 40000; ++i)
            {
                f.setCutoff (uniform (r, -10.0f, 2100.0f)); // also out of range: clamped
                if (r.chance (30))
                    f.setResonance (r.range (-1, 16));
                const auto mv = static_cast<std::uint8_t> (r.below (256));
                if (r.chance (20))
                {
                    f.setMode (mv);
                    o.setModeVolume (mv);
                    o.setRouting (static_cast<std::uint8_t> (r.below (256)));
                    o.setFilterStaticDc (f.staticOutput (o.staticFilterBusDc()));
                }
                const float y = f.process (o.filterBusLevel (uniform (r, -1.5f, 1.5f), uniform (r, 0.0f, 3.0f)));
                const float out = o.process (y, uniform (r, -1.5f, 1.5f), uniform (r, 0.0f, 3.0f));
                INFO ("model " << (m == Model::A ? "A" : "B") << " rate " << rate << " sample " << i);
                REQUIRE (finiteAndBounded (y, 60.0f));
                REQUIRE (finiteAndBounded (out, 10.0f));
            }
        }
}

TEST_CASE ("filter: after a loud input, silence decays below -120 dB with no limit cycle and no denormal", "[filter][stability]")
{
    // 0.1 s of full-scale noise, then a constant input (0, or the voices' DC): the output must settle on the
    // constant's static output (Filter::staticOutput) to within 1e-6 (-120 dB re 1); no output sample on the way
    // is a subnormal number. No limit cycle: with input 0 the output ends exactly at 0 (the states are flushed
    // below 1e-15), with a constant input it ends on the static output within the float output's rounding and
    // moves by less than 1e-12 (-240 dB; what is left is the rounding of the double states at high resonance). Lowest and highest cutoffs (slowest decay: Model B at 15 Hz, resonance
    // 15) at every rate; 3 s is enough for every setting (measured below).
    bst::Rng r (33);
    double slowest = 0.0;
    for (const Model m : { Model::A, Model::B })
        for (const double rate : kRates)
            for (const int res : { 0, 15 })
                for (const float fc : { 0.0f, 2047.0f })
                    for (std::uint8_t mode = 1; mode < 8; ++mode)
                        for (const float dc : { 0.0f, 0.7f })
                        {
                            Filter f;
                            f.prepare (m, rate, 0.5f);
                            f.setCutoff (fc);
                            f.setResonance (res);
                            f.setMode (static_cast<std::uint8_t> (mode << 4));
                            for (int i = 0; i < static_cast<int> (rate * 0.1); ++i)
                                f.process (uniform (r, kInLo, kInHi));
                            const double target = f.staticOutput (dc);
                            const int n = static_cast<int> (rate * 3.0);
                            int settledAt = -1;
                            float last = 0.0f;
                            for (int i = 0; i < n; ++i)
                            {
                                const float y = f.process (dc);
                                REQUIRE (std::fpclassify (y) != FP_SUBNORMAL);
                                const bool within = std::abs (static_cast<double> (y) - target) < 1e-6;
                                if (within && settledAt < 0)
                                    settledAt = i;
                                if (! within)
                                    settledAt = -1;
                                last = y;
                            }
                            INFO ("model " << (m == Model::A ? "A" : "B") << " rate " << rate << " res " << res << " cutoff " << fc << " mode " << int (mode)
                                           << " dc " << dc << ": last " << last << ", target " << target);
                            REQUIRE (settledAt >= 0);
                            slowest = std::max (slowest, settledAt / rate);
                            // no limit cycle
                            double tail = 0.0;
                            float lo = 1e30f, hi = -1e30f;
                            for (int i = 0; i < 1000; ++i)
                            {
                                const float y = f.process (dc);
                                lo = std::min (lo, y);
                                hi = std::max (hi, y);
                                tail = std::max (tail, std::abs (static_cast<double> (y) - target));
                            }
                            if (dc > 0.0f)
                            {
                                CHECK (tail < 2e-7);     // the float output's own rounding of the target
                                CHECK (hi - lo < 1e-12f); // measured at most 1e-15: rounding of the double states (-300 dB), no cycle
                            }
                            else
                                CHECK (! (tail > 0.0)); // exactly 0: the states are flushed
                        }
    WARN ("slowest decay to within 1e-6 of the static output: " << slowest << " s (allowed 3 s)");
}

TEST_CASE ("filter: a band-pass state decaying beside a constant low-pass state never becomes subnormal", "[filter][stability]")
{
    // The slowest decay of 4.4's rates (Model B, cutoff 0 = 15 Hz, resonance 0, 88.2 kHz, a constant input after a
    // loud one): the band-pass state falls to 1e-308 after about 10 s unless flushed. 20 s of chip time.
    bst::Rng r (36);
    Filter f;
    f.prepare (Model::B, 88200.0, 0.5f);
    f.setCutoff (0.0f);
    f.setMode (Filter::kLowPass | Filter::kBandPass);
    for (int i = 0; i < 8820; ++i)
        f.process (uniform (r, kInLo, kInHi));
    bool normal = true;
    for (int i = 0; i < 88200 * 20; ++i)
    {
        f.process (0.7f);
        normal = normal && f.statesNormal();
    }
    CHECK (normal);
}

TEST_CASE ("output stage: silence after a loud input decays below -120 dB through the board high-pass", "[output][stability]")
{
    bst::Rng r (34);
    for (const Model m : { Model::A, Model::B })
        for (const bool vdc : { false, true })
            for (const double rate : { 88200.0, 384000.0 })
            {
                OutputStage o;
                o.prepare (m, rate, vdc);
                o.setRouting (0);
                o.setModeVolume (0x0f);
                o.setFilterStaticDc (0.0);
                for (int i = 0; i < static_cast<int> (rate * 0.1); ++i)
                    o.process (0.0f, uniform (r, -1.5f, 1.5f), uniform (r, 0.0f, 3.0f));
                float last = 1.0f;
                for (int i = 0; i < static_cast<int> (rate * 3.0); ++i)
                {
                    last = o.process (0.0f, 0.0f, 0.0f);
                    REQUIRE (std::fpclassify (last) != FP_SUBNORMAL);
                }
                INFO ("model " << (m == Model::A ? "A" : "B") << " volume DC " << vdc << " rate " << rate);
                CHECK (std::abs (last) < 1e-6f);
            }
}

TEST_CASE ("filter and output stage: bit-identical for any block split", "[filter][output][split]")
{
    // The same input and the same parameter writes (at fixed samples) processed in blocks of 1, 7, 61, 512 and
    // random sizes, with Filter::process (block) and per sample.
    bst::Rng gen (35);
    const int n = 20000;
    std::vector<float> in (static_cast<std::size_t> (n)), d (static_cast<std::size_t> (n)), e (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        in[static_cast<std::size_t> (i)] = uniform (gen, -1.5f, 1.5f);
        d[static_cast<std::size_t> (i)] = uniform (gen, -1.5f, 1.5f);
        e[static_cast<std::size_t> (i)] = uniform (gen, 0.0f, 3.0f);
    }
    for (const Model m : { Model::A, Model::B })
    {
        auto run = [&] (int split) {
            bst::Rng sizes (99);
            Filter f;
            f.prepare (m, 96000.0, 0.3f);
            OutputStage o;
            o.prepare (m, 96000.0, true);
            std::vector<float> out (static_cast<std::size_t> (n));
            std::vector<float> buf;
            int i = 0;
            while (i < n)
            {
                int len = split > 0 ? split : 1 + static_cast<int> (sizes.below (300));
                // parameter writes land at multiples of 500 samples, also inside a block of another split
                const int nextWrite = (i / 500 + 1) * 500;
                len = std::min ({ len, n - i, nextWrite - i });
                if (i % 500 == 0)
                {
                    const int k = i / 500;
                    f.setCutoff (static_cast<float> ((k * 397) % 2048) + 0.25f);
                    f.setResonance (k % 16);
                    const auto mv = static_cast<std::uint8_t> (((k % 8) << 4) | (15 - k % 16) | (k % 3 == 0 ? 0x80 : 0));
                    f.setMode (mv);
                    o.setModeVolume (mv);
                    o.setRouting (static_cast<std::uint8_t> (k % 8));
                    o.setFilterStaticDc (f.staticOutput (o.staticFilterBusDc()));
                }
                buf.assign (in.begin() + i, in.begin() + i + len);
                for (int j = 0; j < len; ++j)
                    buf[static_cast<std::size_t> (j)] = o.filterBusLevel (buf[static_cast<std::size_t> (j)], e[static_cast<std::size_t> (i + j)]);
                if (split == 1)
                    for (auto& v : buf)
                        v = f.process (v);
                else
                    f.process (buf.data(), len);
                for (int j = 0; j < len; ++j)
                    out[static_cast<std::size_t> (i + j)] = o.process (buf[static_cast<std::size_t> (j)], d[static_cast<std::size_t> (i + j)], e[static_cast<std::size_t> (i + j)]);
                i += len;
            }
            return out;
        };
        const auto ref = run (1);
        for (const int split : { 7, 61, 512, 0 })
        {
            const auto o = run (split);
            int diff = 0;
            for (int i = 0; i < n; ++i)
                diff += (o[static_cast<std::size_t> (i)] < ref[static_cast<std::size_t> (i)] || o[static_cast<std::size_t> (i)] > ref[static_cast<std::size_t> (i)]) ? 1 : 0;
            INFO ("model " << (m == Model::A ? "A" : "B") << " split " << split);
            CHECK (diff == 0);
        }
    }
}

TEST_CASE ("filter and output stage: no allocation in prepare, the setters or processing", "[filter][output][rt]")
{
    if (! bst::allocationsAreCounted())
        SKIP ("allocation counting is off in this build (sanitizers)");
    Filter warm; // the converter weights' one-time initialisation
    const auto before = bst::totalAllocations();
    for (const Model m : { Model::A, Model::B })
    {
        Filter f;
        f.prepare (m, 96000.0, 0.5f);
        OutputStage o;
        o.prepare (m, 96000.0, false);
        float acc = 0.0f;
        for (int i = 0; i < 10000; ++i)
        {
            f.setCutoff (static_cast<float> (i % 2048));
            f.setResonance (i % 16);
            f.setMode (static_cast<std::uint8_t> (i));
            f.setVariation (static_cast<float> (i % 10) / 10.0f);
            o.setModeVolume (static_cast<std::uint8_t> (i));
            o.setRouting (static_cast<std::uint8_t> (i / 3));
            o.setVolumeDc ((i & 64) != 0);
            o.setFilterStaticDc (f.staticOutput (o.staticFilterBusDc()));
            acc += o.process (f.process (o.filterBusLevel (0.1f, 0.5f)), 0.2f, 1.0f);
        }
        CHECK (std::isfinite (acc));
    }
    CHECK (bst::totalAllocations() == before);
}

TEST_CASE ("filter: outputs, signs and the notch (8.19.1)", "[filter]")
{
    for (const Model m : { Model::A, Model::B })
    {
        Filter f;
        f.prepare (m, 192000.0, 0.5f);
        f.setCutoff (1024.0f);
        f.setResonance (0);
        INFO ("model " << (m == Model::A ? "A" : "B"));
        // low-pass inverted against the direct path (DC gain about -1), band- and high-pass block DC
        f.setMode (Filter::kLowPass);
        CHECK (f.staticOutput (0.5) < -0.3);
        if (m == Model::B)
        {
            CHECK (std::abs (f.staticOutput (0.5) + 0.5) < 0.01);
            f.setMode (Filter::kBandPass);
            CHECK (std::abs (f.staticOutput (0.5)) < 1e-12);
            f.setMode (Filter::kHighPass);
            CHECK (std::abs (f.staticOutput (0.5)) < 1e-12);
        }
        // low + high is a notch at the corner (band-pass passes there)
        const double fc = f.cutoffHz();
        f.setMode (Filter::kLowPass | Filter::kHighPass);
        const double notch = sineGain (f, 192000.0, fc);
        const double below = sineGain (f, 192000.0, fc / 8.0);
        f.setMode (Filter::kBandPass);
        const double band = sineGain (f, 192000.0, fc);
        f.setMode (0);
        const double none = sineGain (f, 192000.0, fc);
        WARN ("notch at the corner " << 20.0 * std::log10 (notch / below) << " dB below the low band; band-pass at the corner " << band);
        // Model B's outputs are ideal: a deep notch; Model A's are not (gains 1.0 / 0.59 and the leaks of Filter.h):
        // the reference's own Model A notch is about 7 dB deep (a_modes_a s03 at 3 kHz against s00), ours 9.8
        CHECK (notch < (m == Model::B ? 0.05 : 0.5) * below);
        CHECK (band > 0.3);
        CHECK (none < 1e-9);
    }
}

TEST_CASE ("filter: resonance raises the peak at every step, cutoff curves and Model A's converter steps", "[filter]")
{
    for (const Model m : { Model::A, Model::B })
    {
        Filter f;
        f.prepare (m, 192000.0, 0.5f);
        f.setCutoff (1024.0f);
        f.setMode (Filter::kLowPass);
        double previous = 0.0;
        for (int res = 0; res < 16; ++res)
        {
            f.setResonance (res);
            const double q = f.q();
            INFO ("model " << (m == Model::A ? "A" : "B") << " resonance " << res);
            CHECK (q > previous);
            previous = q;
        }
        CHECK (previous > 2.0); // Q at resonance 15: Model A 2.17, Model B 2.58 (fitted)
    }
    // Model B: close to linear in the register; Model A: steps down at the converter's bit boundaries
    CHECK (Filter::cutoffCurveHz (Model::B, 1024.0, 0.5) > 7000.0);
    CHECK (Filter::cutoffCurveHz (Model::B, 1024.0, 0.5) < 7400.0);
    CHECK (Filter::cutoffCurveHz (Model::B, 2047.0, 0.5) > 14000.0);
    const double a1023 = Filter::cutoffCurveHz (Model::A, 1023.0, 0.5), a1024 = Filter::cutoffCurveHz (Model::A, 1024.0, 0.5);
    WARN ("Model A cutoff at 1023: " << a1023 << " Hz, at 1024: " << a1024 << " Hz");
    CHECK (a1024 < 0.97 * a1023);
    CHECK (Filter::modelACutoffConverter (2047.0) > 0.999);
    CHECK (Filter::modelACutoffConverter (0.0) < 1e-12);
    // variation shifts the Model A curve (up with variation), not Model B's
    CHECK (Filter::cutoffCurveHz (Model::A, 512.0, 1.0) > 1.2 * Filter::cutoffCurveHz (Model::A, 512.0, 0.5));
    CHECK (Filter::cutoffCurveHz (Model::A, 512.0, 0.0) < 0.8 * Filter::cutoffCurveHz (Model::A, 512.0, 0.5));
    CHECK (! (Filter::cutoffCurveHz (Model::B, 512.0, 1.0) < Filter::cutoffCurveHz (Model::B, 512.0, 0.5)
              || Filter::cutoffCurveHz (Model::B, 512.0, 1.0) > Filter::cutoffCurveHz (Model::B, 512.0, 0.5)));
    // clock-independent: nothing in the filter takes a clock; the same register gives the same Hz at every rate
    Filter x, y;
    x.prepare (Model::A, 88200.0, 0.5f);
    y.prepare (Model::A, 384000.0, 0.5f);
    x.setCutoff (700.0f);
    y.setCutoff (700.0f);
    CHECK (std::abs (x.cutoffHz() - y.cutoffHz()) < 1e-9);
}

TEST_CASE ("output stage: volume DC on steps the output on a volume write, off it does not", "[output]")
{
    for (const Model m : { Model::A, Model::B })
    {
        const double expectStep = m == Model::A ? 0.35 : 0.06; // the reference's 0 -> 15 step (a_voice_dc_*)
        for (const bool vdc : { true, false })
        {
            OutputStage o;
            o.prepare (m, 96000.0, vdc);
            o.setRouting (0);
            o.setModeVolume (0x00);
            o.setFilterStaticDc (0.0);
            const double a = o.mixerOutput (0.0, 0.0, 0.0);
            o.setModeVolume (0x0f);
            const double b = o.mixerOutput (0.0, 0.0, 0.0);
            INFO ("model " << (m == Model::A ? "A" : "B") << " volume DC " << vdc << ": step " << b - a);
            if (vdc)
                CHECK (std::abs (std::abs (b - a) - expectStep) < 0.15 * expectStep);
            else
                CHECK (std::abs (b - a) < 1e-12);
        }
        // oscillator 3 off removes voice 3 from the direct path only
        OutputStage o;
        o.prepare (m, 96000.0, true);
        o.setRouting (0);
        o.setModeVolume (0x8f);
        CHECK (o.directVoices() == 2);
        CHECK (o.filterVoices() == 0);
        o.setRouting (4);
        CHECK (o.directVoices() == 2);
        CHECK (o.filterVoices() == 1);
        o.setModeVolume (0x0f);
        o.setRouting (3);
        CHECK (o.directVoices() == 1);
        CHECK (o.filterVoices() == 2);
    }
}
