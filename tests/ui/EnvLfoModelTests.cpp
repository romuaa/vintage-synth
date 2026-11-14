// Envelope and LFO pictures (docs/architecture.md 9.3.2, 9.3.3, 9.4, 9.6 "Models" and "Derived
// pictures"): the handle maths of the two graphs and the shapes the engine's own envelope and LFO
// code draw. JUCE-free code only. The expected values are written from the contract's formulas,
// not read back from the models.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "common/ui/models/EnvelopeModel.h"
#include "common/ui/models/GraphModels.h"
#include "common/ui/models/LfoModel.h"
#include "vintage/dsp/Envelope.h"
#include "vintage/dsp/Lfo.h"
#include "vintage/ui/EnvelopePicture.h"

#include <cmath>
#include <cstring>
#include <random>

using namespace vs;
using namespace vs::ui;
using namespace vs::ui::model;
using Catch::Approx;

namespace
{
/** A parameter that records writes (and enforces begin/end pairing). */
struct Param final : ParamTarget
{
    explicit Param (float initial, float def = 0.0f) : v (initial), d (def) {}
    float value() const noexcept override { return v; }
    float defaultValue() const noexcept override { return d; }
    int numSteps() const noexcept override { return 0; }
    void beginGesture() override { ++begins; open = true; }
    void setValue (float x) override
    {
        outside += open ? 0 : 1;
        v = x;
    }
    void endGesture() override { ++ends; open = false; }
    float v, d;
    bool open = false;
    int begins = 0, ends = 0, outside = 0;
};

/** Contract 9.3.2, written out: slot S = 0.8 W / 3, each time segment S x (0.1 + 0.9 v), plateau 0.2 W. */
float segment (float W, float v) { return 0.8f * W / 3.0f * (0.1f + 0.9f * v); }
} // namespace

TEST_CASE ("Envelope layout: slot, segment widths, the fixed plateau and the four handle points follow the contract", "[ui][models][envelope]")
{
    EnvelopeLayout l;
    l.x0 = 20.0f;
    l.width = 1000.0f;
    l.yTop = 10.0f;
    l.yBase = 210.0f;
    const EnvelopeValues v { 0.25f, 0.5f, 0.3f, 1.0f };
    const auto p = l.points (v);
    CHECK (p.peakX == Approx (20.0f + segment (1000.0f, 0.25f)));
    CHECK (p.decayEndX == Approx (p.peakX + segment (1000.0f, 0.5f)));
    CHECK (p.plateauEndX == Approx (p.decayEndX + 200.0f)); // 0.2 W
    CHECK (p.releaseEndX == Approx (p.plateauEndX + segment (1000.0f, 1.0f)));
    CHECK (p.topY == Approx (10.0f));
    CHECK (p.baseY == Approx (210.0f));
    CHECK (p.sustainY == Approx (210.0f - 0.3f * 200.0f)); // linear level

    // The longest picture (every time at maximum) is exactly as wide as the plot; the shortest has the
    // plateau and three 0.1-slot segments.
    const auto longest = l.points ({ 1.0f, 1.0f, 0.5f, 1.0f });
    CHECK (longest.releaseEndX == Approx (20.0f + 1000.0f));
    const auto shortest = l.points ({ 0.0f, 0.0f, 0.5f, 0.0f });
    CHECK (shortest.releaseEndX == Approx (20.0f + 3.0f * 0.1f * 0.8f * 1000.0f / 3.0f + 200.0f));
}

TEST_CASE ("Envelope layout: dragging a handle writes the time that puts it there (attack, decay from the peak, release from the plateau)",
           "[ui][models][envelope]")
{
    EnvelopeLayout l;
    l.x0 = 14.0f;
    l.width = 554.0f;
    l.yTop = 14.0f;
    l.yBase = 200.0f;
    std::mt19937 rng (3);
    std::uniform_real_distribution<float> uni (0.0f, 1.0f);
    for (int i = 0; i < 500; ++i)
    {
        const EnvelopeValues v { uni (rng), uni (rng), uni (rng), uni (rng) };
        const auto p = l.points (v);
        CHECK (l.attackFromX (p.peakX) == Approx (v.attack).margin (1.0e-5));
        CHECK (l.decayFromX (p.decayEndX, p.peakX) == Approx (v.decay).margin (1.0e-5));
        CHECK (l.releaseFromX (p.releaseEndX, p.plateauEndX) == Approx (v.release).margin (1.0e-5));
        CHECK (l.levelAt (p.sustainY) == Approx (v.sustain).margin (1.0e-5));
    }
    // Past either end of the scale the value is clamped, never extrapolated.
    CHECK (l.attackFromX (l.x0 - 50.0f) == Approx (0.0f));
    CHECK (l.attackFromX (l.x0 + l.width) == Approx (1.0f));
    CHECK (l.decayFromX (0.0f, 100.0f) == Approx (0.0f));
    CHECK (l.releaseFromX (1.0e6f, 100.0f) == Approx (1.0f));
}

TEST_CASE ("Envelope handle drags through the handle model: x is absolute with the grab offset, y is the level, fine drag is a tenth", "[ui][models][envelope]")
{
    EnvelopeLayout l;
    l.x0 = 14.0f;
    l.width = 554.0f;
    l.yTop = 14.0f;
    l.yBase = 200.0f;

    // The decay / sustain handle: x writes the decay measured from the peak, y the sustain level.
    Param attack (0.3f), decay (0.4f), sustain (0.5f);
    HandleModel m;
    m.setX (&decay, [&] (float px) { return l.decayFromX (px, l.points ({ attack.v, decay.v, sustain.v, 0.0f }).peakX); });
    m.setY (&sustain, l.yBase - l.yTop);
    const auto start = l.points ({ attack.v, decay.v, sustain.v, 0.0f });
    m.mouseDown (start.decayEndX + 3.0f, start.sustainY - 2.0f, start.decayEndX, false); // grabbed 3 px right, 2 px above the centre
    m.mouseDrag (start.decayEndX + 3.0f + 40.0f, start.sustainY - 2.0f - 37.2f, false);   // 37.2 px up = 0.2 of the 186 px level range
    CHECK (decay.v == Approx (l.decayFromX (start.decayEndX + 40.0f, start.peakX)).margin (1.0e-5)); // the grab offset is kept
    CHECK (sustain.v == Approx (0.7f).margin (1.0e-4));
    CHECK (decay.begins == 1);
    CHECK (sustain.begins == 1);
    m.mouseUp();
    CHECK (decay.ends == 1);
    CHECK (sustain.ends == 1);
    CHECK (decay.outside == 0);
    CHECK (sustain.outside == 0);

    // Fine: the pointer's motion counts a tenth.
    Param rel (0.5f);
    HandleModel r;
    const float plateauEnd = l.points ({ 0.3f, 0.4f, 0.5f, 0.5f }).plateauEndX;
    r.setX (&rel, [&] (float px) { return l.releaseFromX (px, plateauEnd); });
    const float at = l.points ({ 0.3f, 0.4f, 0.5f, 0.5f }).releaseEndX;
    r.mouseDown (at, 100.0f, at, true);
    r.mouseDrag (at + 100.0f, 100.0f, true);
    CHECK (rel.v == Approx (l.releaseFromX (at + 10.0f, plateauEnd)).margin (1.0e-5));
    r.mouseUp();
}

TEST_CASE ("Envelope time labels: spread so they never overlap, stay inside the plot, keep their order", "[ui][models][envelope]")
{
    std::mt19937 rng (11);
    std::uniform_real_distribution<float> uni (-50.0f, 650.0f);
    for (int i = 0; i < 2000; ++i)
    {
        float desired[3] { uni (rng), uni (rng), uni (rng) };
        std::sort (desired, desired + 3);
        float out[3];
        spreadLabels (desired, out, 3, 72.0f, 0.0f, 582.0f);
        for (int k = 0; k < 3; ++k)
        {
            CHECK (out[k] - 36.0f >= -1.0e-3f);
            CHECK (out[k] + 36.0f <= 582.0f + 1.0e-3f);
            if (k > 0)
                CHECK (out[k] - out[k - 1] >= 72.0f - 1.0e-3f);
        }
    }
    // Labels that already fit stay under their handles.
    const float apart[3] { 100.0f, 250.0f, 450.0f };
    float out[3];
    spreadLabels (apart, out, 3, 72.0f, 0.0f, 582.0f);
    for (int k = 0; k < 3; ++k)
        CHECK (out[k] == Approx (apart[k]));
    // Three handles at the same x are pushed apart around it.
    const float same[3] { 300.0f, 300.0f, 300.0f };
    spreadLabels (same, out, 3, 72.0f, 0.0f, 582.0f);
    CHECK (out[1] - out[0] == Approx (72.0f));
    CHECK (out[2] - out[1] == Approx (72.0f));
}

TEST_CASE ("LFO window: the smallest of 0.5, 1, 2, 5, 10, 20 s that holds the delay plus two cycles", "[ui][models][lfo]")
{
    CHECK (lfoWindowFor (0.0f, 4.0f) == Approx (0.5f));  // two cycles of 4 Hz = 0.5 s exactly
    CHECK (lfoWindowFor (0.0f, 3.0f) == Approx (1.0f));  // 0.667 s
    CHECK (lfoWindowFor (0.6f, 3.0f) == Approx (2.0f));  // 1.267 s
    CHECK (lfoWindowFor (0.0f, 40.0f) == Approx (0.5f)); // fastest rate: shortest window
    CHECK (lfoWindowFor (5.0f, 40.0f) == Approx (10.0f)); // 5.05 s
    CHECK (lfoWindowFor (0.0f, 0.5f) == Approx (5.0f));  // 4 s
    CHECK (lfoWindowFor (0.0f, 0.05f) == Approx (20.0f)); // 40 s needed: 20 s at most
    CHECK (lfoWindowFor (5.0f, 0.05f) == Approx (20.0f));
    // The window always holds what it can: delay + two cycles, whenever that is within 20 s.
    for (float delay : { 0.0f, 0.3f, 1.0f, 4.0f })
        for (float rate : { 0.2f, 1.0f, 2.5f, 12.0f })
        {
            const float w = lfoWindowFor (delay, rate);
            if (delay + 2.0f / rate <= 20.0f)
                CHECK (delay + 2.0f / rate <= w + 1.0e-4f);
        }
}

TEST_CASE ("LFO handles: delay and cycle round-trip through the parameters' own mappings, clamped to 0 - 5 s and 0.05 - 40 Hz", "[ui][models][lfo]")
{
    std::mt19937 rng (5);
    std::uniform_real_distribution<float> uni (0.0f, 1.0f);
    for (int i = 0; i < 500; ++i)
    {
        const float d = uni (rng), r = uni (rng);
        // Contract 9.3.3: lfo.delay = lfoDelayFromSec (t), lfo.rate = lfoRateFromHz (1 / cycleSec).
        const float delaySec = 5.0f * d * d;
        const float rateHz = 0.05f * std::pow (800.0f, r);
        CHECK (lfoDelayHandleSec (d) == Approx (delaySec));
        CHECK (lfoCycleHandleSec (d, r) == Approx (delaySec + 1.0f / rateHz));
        CHECK (lfoDelayFromHandle (delaySec) == Approx (d).margin (1.0e-4));
        CHECK (lfoRateFromHandle (delaySec + 1.0f / rateHz, d) == Approx (r).margin (1.0e-4));
    }
    CHECK (lfoDelayFromHandle (-1.0f) == Approx (0.0f));
    CHECK (lfoDelayFromHandle (60.0f) == Approx (1.0f)); // 5 s at most
    CHECK (lfoRateFromHandle (0.5f, 0.0f) == Approx (std::log (1.0f / 0.5f / 0.05f) / std::log (800.0f)).margin (1.0e-4)); // a 0.5 s cycle
    CHECK (lfoRateFromHandle (0.0f, 0.0f) == Approx (1.0f)); // a cycle shorter than 25 ms: the fastest rate
    CHECK (lfoRateFromHandle (500.0f, 0.0f) == Approx (0.0f).margin (1.0e-4)); // longer than 20 s: the slowest rate
    CHECK (lfoRateFromHandle (3.0f, 1.0f) >= 0.0f); // dragged left of the delay (cycle <= 0): clamped, never NaN
    CHECK (std::isfinite (lfoRateFromHandle (0.0f, 1.0f)));
}

TEST_CASE ("LFO axis: time <-> x is linear over the plot", "[ui][models][lfo]")
{
    const LfoAxis a { 14.0f, 1000.0f, 2.0f };
    CHECK (a.xOf (0.0f) == Approx (14.0f));
    CHECK (a.xOf (2.0f) == Approx (1014.0f));
    CHECK (a.xOf (0.5f) == Approx (264.0f));
    CHECK (a.timeAt (264.0f) == Approx (0.5f));
}

// ---- Derived pictures (9.4) ----

TEST_CASE ("Envelope shape: every segment end equals the real envelope rendered at 48 kHz at the same instant (0.5 %)", "[ui][pictures][envelope]")
{
    const float values[] { 0.0f, 0.4f, 1.0f };
    const float sustains[] { 0.0f, 0.3f, 1.0f };
    int compared = 0;
    for (float a : values)
        for (float d : values)
            for (float r : values)
                for (float s : sustains)
                {
                    const float ta = map::envTimeSec (a), td = map::envTimeSec (d), tr = map::envTimeSec (r);
                    const auto shape = computeEnvelopeShape ({ a, d, s, r });

                    constexpr float rate = 48000.0f;
                    dsp::Envelope ref;
                    ref.setSampleRate (rate);
                    ref.setTimes (ta, td, tr);
                    ref.noteOn();
                    const auto run = [&] (double seconds) {
                        const int n = static_cast<int> (std::lround (seconds * static_cast<double> (rate)));
                        float level = 0.0f;
                        for (int i = 0; i < n; ++i)
                            level = ref.process (s);
                        return level;
                    };
                    INFO ("attack " << a << " decay " << d << " sustain " << s << " release " << r);
                    const float peak = run (ta);
                    CHECK (shape.attack.back() == Approx (std::min (peak, 1.0f)).margin (0.005));
                    CHECK (shape.attack.front() == Approx (0.0f).margin (1.0e-6));
                    if (ta >= 0.02f)
                    {
                        // halfway through the segment (the RC curve, not a straight line)
                        dsp::Envelope half;
                        half.setSampleRate (rate);
                        half.setTimes (ta, td, tr);
                        half.noteOn();
                        float level = 0.0f;
                        const int n = static_cast<int> (std::lround (0.5 * static_cast<double> (ta) * static_cast<double> (rate)));
                        for (int i = 0; i < n; ++i)
                            level = half.process (s);
                        CHECK (shape.attack[model::EnvelopeShape::kSteps / 2] == Approx (level).margin (0.005));
                    }
                    const float decayEnd = run (td);
                    CHECK (shape.decay.front() == Approx (1.0f).margin (1.0e-6));
                    CHECK (shape.decay.back() == Approx (decayEnd).margin (0.005));
                    CHECK (shape.release.front() == Approx (shape.decay.back()).margin (1.0e-6)); // continuous at the plateau
                    ref.noteOff();
                    const float releaseEnd = run (tr);
                    CHECK (shape.release.back() == Approx (releaseEnd).margin (0.005));
                    ++compared;
                }
    CHECK (compared == 81);
}

TEST_CASE ("Envelope shape: the sustain is the level the decay settles at, and the curve is monotone within each segment", "[ui][pictures][envelope]")
{
    for (float s : { 0.0f, 0.25f, 0.75f, 1.0f })
    {
        const auto shape = computeEnvelopeShape ({ 0.3f, 0.5f, s, 0.6f });
        CHECK (shape.sustain == Approx (s));
        CHECK (shape.decay.back() == Approx (s).margin (0.011)); // 99 % of the way
        for (size_t i = 1; i < shape.attack.size(); ++i)
            CHECK (shape.attack[i] >= shape.attack[i - 1] - 1.0e-6f);
        for (size_t i = 1; i < shape.release.size(); ++i)
            CHECK (shape.release[i] <= shape.release[i - 1] + 1.0e-6f);
        // The decay approaches the sustain from above without crossing it.
        for (const float level : shape.decay)
            CHECK (level >= s - 1.0e-5f);
    }
}

TEST_CASE ("LFO preview: 1000 points of the engine's own LFO, silent for half the delay, then fading in; the handles sit where the fade ends and a cycle ends",
           "[ui][pictures][lfo]")
{
    constexpr int n = LfoPreview::kPoints;
    // Delay 0.5 s, rate 2 Hz, window 2 s: 500 points per second, so the fade ends at point 249 (t = 0.5 s)
    // and the first whole cycle after it ends at point 499 (t = 1.0 s), both exactly on a point.
    LfoPreview delayed, plain;
    computeLfoPreview (0, 2.0f, 0.5f, 2.0f, delayed);   // sine
    computeLfoPreview (0, 2.0f, 0.0f, 2.0f, plain);
    CHECK (delayed.windowSec == Approx (2.0f));

    // A direct run of the same object, point for point.
    dsp::Lfo lfo;
    lfo.prepare (static_cast<float> (n) / 2.0f, kLfoPreviewSeed);
    lfo.setWave (dsp::LfoWave::Sine);
    lfo.setRate (2.0f);
    lfo.setDelay (0.5f);
    lfo.trigger();
    for (int i = 0; i < n; ++i)
        REQUIRE (sameBits (delayed.y[static_cast<size_t> (i)], lfo.process()));

    for (int i = 0; i < 124; ++i) // t < 0.25 s = half the delay
        CHECK (delayed.y[static_cast<size_t> (i)] == Approx (0.0f).margin (1.0e-6));
    bool fading = false;
    for (int i = 130; i < 249; ++i)
        fading = fading || std::abs (delayed.y[static_cast<size_t> (i)]) > 1.0e-3f;
    CHECK (fading);
    // From the delay on the picture equals the undelayed LFO: the fade-in ends exactly at the delay handle.
    for (int i = 249; i < n; ++i)
        CHECK (delayed.y[static_cast<size_t> (i)] == Approx (plain.y[static_cast<size_t> (i)]).margin (1.0e-4));
    // One whole cycle after the delay the wave is where it was at the delay (the cycle handle).
    CHECK (delayed.y[499] == Approx (delayed.y[249]).margin (2.0e-3));
    CHECK (delayed.y[749] == Approx (delayed.y[249]).margin (2.0e-3)); // and again
    // ... but not a quarter of a cycle after it (point 311: the sine's crest).
    CHECK (std::abs (delayed.y[311] - delayed.y[249]) > 0.9f);
}

TEST_CASE ("LFO preview: every waveform is drawn (index order of the choice), the random ones deterministically", "[ui][pictures][lfo]")
{
    LfoPreview a, b;
    for (int wave = 0; wave < 6; ++wave)
    {
        computeLfoPreview (wave, 3.0f, 0.0f, 2.0f, a);
        computeLfoPreview (wave, 3.0f, 0.0f, 2.0f, b);
        float peak = 0.0f;
        for (int i = 0; i < LfoPreview::kPoints; ++i)
        {
            REQUIRE (sameBits (a.y[static_cast<size_t> (i)], b.y[static_cast<size_t> (i)])); // fixed preview seed
            peak = std::max (peak, std::abs (a.y[static_cast<size_t> (i)]));
        }
        INFO ("wave " << wave);
        CHECK (peak > 0.3f);
        CHECK (peak <= 1.0f + 1.0e-6f);
    }
    // Square: two levels; saw: falling ramp; triangle starts rising (kLfoWaveLabels order).
    computeLfoPreview (3, 1.0f, 0.0f, 2.0f, a);
    CHECK (a.y[100] == Approx (1.0f).margin (1.0e-3));
    CHECK (a.y[350] == Approx (-1.0f).margin (1.0e-3));
    computeLfoPreview (1, 1.0f, 0.0f, 2.0f, a);
    CHECK (a.y[10] > 0.0f);
    CHECK (a.y[10] < a.y[100]);
}
