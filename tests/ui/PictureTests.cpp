// Derived pictures of the Sound page (docs/architecture.md 9.4, 9.6): the filter curve is the voice
// filter's real response, and the oscilloscopes run the voice's own oscillator code.
// (That the page feeds both with the engine's own parameter read is checked in EditorTests.)

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "common/dsp/FastMath.h"
#include "vintage/dsp/FilterResponse.h"
#include "vintage/dsp/Filters.h"
#include "vintage/engine/OscScope.h"
#include "vintage/engine/Voice.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace vs;
using Catch::Approx;

namespace
{
constexpr double kPi = 3.14159265358979323846;

/** |H(f)| in dB of the real filter chain at middle C as Voice runs it: the cutoff through the
    engine's exp2 and clamp, both prewarps through the engine's tanPrewarp, OnePoleHighPass ->
    OtaLadder, driven by a sine at -60 dBFS (the linear region), I/Q-demodulated over whole cycles
    after the transient. */
double measuredDb (float cutoff, float resonance, bool slope24, float hpf, double rate, double hz)
{
    const float fs = static_cast<float> (rate);
    const float piOverFs = dsp::kPi / fs;
    const float maxLog2 = std::log2 (0.42f * fs);
    const float fc = dsp::fast::exp2 (dsp::clamp (map::cutoffLog2Hz (cutoff), 3.0f, maxLog2));
    const float hpfHz = 5.0f * dsp::fast::exp2 (9.0f * hpf);
    const float hpfG = dsp::tptOnePoleG (dsp::fast::tanPrewarp (hpfHz * piOverFs));
    const auto c = dsp::OtaLadder::makeCoefs (dsp::fast::tanPrewarp (fc * piOverFs), map::resonanceK (resonance));
    dsp::OnePoleHighPass hp;
    dsp::OtaLadder vcf;
    constexpr double amp = 0.001; // -60 dBFS
    const double w = 2.0 * kPi * hz / rate;
    // Settle for the slowest pole: the high-pass, the test tone, or the resonant cutoff (whose decay
    // time grows as 1 / (4 - k)).
    const double resonantSeconds = 2.0 / (static_cast<double> (fc) * std::max (0.02, 4.0 - static_cast<double> (c.k)));
    const double seconds = std::max ({ 12.0 / std::min ({ static_cast<double> (hpfHz), hz }), 12.0 * resonantSeconds, 0.05 });
    const int settle = static_cast<int> (std::min (seconds, 6.0) * rate);
    const int cycles = std::max (20, static_cast<int> (std::ceil (0.2 * hz)));
    const int measure = static_cast<int> (std::round (cycles * rate / hz));
    double re = 0.0, im = 0.0;
    for (int n = 0; n < settle + measure; ++n)
    {
        const double x = amp * std::sin (w * n);
        const float h = hp.processG (static_cast<float> (x), hpfG);
        const float y = vcf.tick (h, c.G, c.c1, c.c2, c.c3, c.c4, c.k, c.inv, slope24) * c.comp;
        if (n >= settle)
        {
            re += y * std::sin (w * n);
            im += y * std::cos (w * n);
        }
    }
    return 20.0 * std::log10 (2.0 * std::sqrt (re * re + im * im) / measure / amp);
}
} // namespace

TEST_CASE ("Filter curve: FilterResponse equals the real filter (linear region), 30 Hz ... fs/4, at every real-time voice rate", "[ui][pictures][slow]")
{
    // Real-time voice rates: 44.1 and 48 kHz hosts at 2x, 96 and 192 kHz hosts at 1x.
    // Resonance up to 0.95 (k = 3.94): the linear picture holds below self-oscillation (k < 4,
    // resonance < 0.964). Points predicted below -100 dB are skipped: there the float filter's own
    // rounding dominates. Within 0.2 dB, and within 0.5 dB on a resonant peak (k > 3.8), where the
    // magnitude is extremely sensitive to the last bit of the coefficients.
    int compared = 0, belowFloor = 0;
    for (double rate : { 88200.0, 96000.0, 192000.0 })
        for (float cutoff : { 0.35f, 0.6f, 0.85f })
            for (float resonance : { 0.0f, 0.5f, 0.9f, 0.95f })
                for (int slope : { 0, 1 })
                    for (float hpf : { 0.0f, 0.45f })
                    {
                        const auto s = dsp::filterCurveSettings (cutoff, resonance, slope, hpf, rate);
                        REQUIRE (s.k < 4.0);
                        for (double hz : { 30.0, 125.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0, rate / 4.0 })
                        {
                            const double predicted = dsp::filterMagnitudeDb (hz, s);
                            if (predicted < -100.0)
                            {
                                ++belowFloor;
                                continue;
                            }
                            const double measured = measuredDb (cutoff, resonance, slope == 1, hpf, rate, hz);
                            const bool peak = s.k > 3.8 && std::abs (std::log2 (hz / s.cutoffHz)) < 0.5;
                            INFO ("rate " << rate << " cutoff " << cutoff << " res " << resonance << " slope " << slope << " hpf " << hpf << " at "
                                          << hz << " Hz");
                            CHECK (measured == Approx (predicted).margin (peak ? 0.5 : 0.2));
                            ++compared;
                        }
                    }
    CHECK (compared > 1000);
    WARN ("filter curve: " << compared << " points compared, " << belowFloor << " below -100 dB skipped");
}

TEST_CASE ("Filter curve: the slope index selects the tap (behaviour, not only the layout)", "[ui][pictures]")
{
    // -24 dB: four poles; -12 dB: two. One octave above a low cutoff the -24 dB curve is far lower.
    const auto steep = dsp::filterCurveSettings (0.4f, 0.0f, 1, 0.0f, 96000.0);
    const auto gentle = dsp::filterCurveSettings (0.4f, 0.0f, 0, 0.0f, 96000.0);
    REQUIRE (steep.slope24);
    REQUIRE_FALSE (gentle.slope24);
    const double f = steep.cutoffHz * 4.0;
    CHECK (dsp::filterMagnitudeDb (f, steep) == Approx (-48.0).margin (2.0));
    CHECK (dsp::filterMagnitudeDb (f, gentle) == Approx (-24.0).margin (2.0));
}

TEST_CASE ("Filter curve: the cutoff is clamped like the voice (8 Hz ... 0.42 fs) and middle C has no key offset", "[ui][pictures]")
{
    const auto top = dsp::filterCurveSettings (1.0f, 0.0f, 1, 0.0f, 48000.0);
    CHECK (top.cutoffHz == Approx (0.42 * 48000.0));
    const auto bottom = dsp::filterCurveSettings (0.0f, 0.0f, 1, 0.0f, 96000.0);
    CHECK (bottom.cutoffHz == Approx (20.0)); // 20 Hz is above the 8 Hz floor
    const auto mid = dsp::filterCurveSettings (0.5f, 0.25f, 0, 1.0f, 96000.0);
    CHECK (mid.cutoffHz == Approx (map::cutoffHz (0.5f)));
    CHECK (mid.k == Approx (map::resonanceK (0.25f)));
    CHECK_FALSE (mid.slope24);
    CHECK (mid.hpfHz == Approx (map::hpfHz (1.0f)));
    CHECK (dsp::filterMagnitudeDb (100.0, dsp::filterCurveSettings (0.9f, 0.0f, 1, 0.0f, 96000.0)) == Approx (0.0).margin (0.05));
    // Self-oscillation starts at k = 4 (resonance 0.964); above it the response has no finite peak.
    CHECK (dsp::filterCurveSettings (0.5f, 0.97f, 1, 0.0f, 96000.0).k >= 4.0);
}

TEST_CASE ("Oscillator scopes: the settings come from the voice's own configuration and resting pulse width", "[ui][pictures]")
{
    // A real Voice applies the same parameters; its configuration must be what the picture uses.
    for (int source = 0; source < 3; ++source)
        for (int polarity = 0; polarity < 2; ++polarity)
            for (float depth : { 0.0f, 0.4f, 1.0f })
                for (int sync = 0; sync < 2; ++sync)
                    for (float cross : { 0.0f, 0.5f })
                    {
                        PartParams p;
                        p.vco1Wave = 4;
                        p.vco2Wave = (source + polarity) % 6;
                        p.pwmSource = source;
                        p.env1Polarity = polarity;
                        p.pwmDepth = depth;
                        p.vcoSync = sync;
                        p.vco1CrossMod = cross;
                        p.vco1Range = 3;
                        p.vco2Mode = source == 2 ? 1 : 0;
                        Voice v;
                        v.prepare (96000.0f, 1);
                        v.setBlockParams (p, 0.0f);
                        const auto s = oscScopeSettingsFor (p);
                        INFO ("source " << source << " polarity " << polarity << " depth " << depth << " sync " << sync << " cross " << cross);
                        CHECK (s.config == v.getOscConfig());
                        // At rest: LFO at its centre (amount 0.5), ENV-1 at 0 (amount 0, or 1 inverted), Manual 1.
                        const float amount = source == 0 ? 0.5f : (source == 1 ? 1.0f : (polarity == 1 ? 1.0f : 0.0f));
                        CHECK (s.width == Approx (0.5f - 0.45f * depth * amount));
                        CHECK (s.crossModOct == Approx (map::crossModOct (cross)));
                    }
}

TEST_CASE ("Oscillator scopes: the picture is the voice's per-sample oscillator code (oscPairSample) after its warm-up", "[ui][pictures]")
{
    for (int wave = 0; wave < 6; ++wave)
        for (bool sync : { false, true })
            for (float cross : { 0.0f, 0.6f })
            {
                OscScopeSettings s;
                s.config.wave1 = wave;
                s.config.wave2 = (wave + 2) % 6;
                s.config.octave1 = 1;
                s.config.sync = sync;
                s.crossModOct = map::crossModOct (cross);
                s.width = 0.3f;
                OscScopes scopes;
                renderOscScopes (s, scopes);

                // The same oscillators stepped by the voice's own sample function.
                dsp::Oscillator v1, v2;
                v1.setWave (static_cast<dsp::Wave> (s.config.wave1));
                v2.setWave (static_cast<dsp::Wave> (s.config.wave2));
                v1.setSeed (0x51u);
                v2.setSeed (0x52u);
                v1.reset();
                v2.reset();
                const float dt2 = 2.0f / OscScopes::kPoints;
                const bool coupled = sync || cross > 0.0f;
                const float dt1 = coupled ? std::min (dt2 * 2.0f, 0.25f) : dt2;
                std::vector<float> o1, o2;
                for (int i = -dsp::Oscillator::kLatency; i < OscScopes::kPoints; ++i)
                {
                    float a1, a2;
                    oscPairSample (v1, v2, dt1, dt2, 0.3f, cross > 0.0f, s.crossModOct, sync, a1, a2);
                    if (i >= 0)
                    {
                        o1.push_back (a1);
                        o2.push_back (a2);
                    }
                }
                INFO ("wave " << wave << " sync " << sync << " cross " << cross);
                for (size_t i = 0; i < o1.size(); ++i)
                {
                    REQUIRE (std::memcmp (&o1[i], &scopes.osc1[i], sizeof (float)) == 0);
                    REQUIRE (std::memcmp (&o2[i], &scopes.osc2[i], sizeof (float)) == 0);
                }
            }
    // Sync visibly restarts VCO-1 (the picture differs from the unsynced one at a non-integer ratio).
    OscScopeSettings a, b;
    a.config.wave1 = b.config.wave1 = 2;
    a.fineSemis = b.fineSemis = 0.7f;
    b.config.sync = true;
    a.crossModOct = 0.0f;
    a.config.sync = false;
    OscScopes sa, sb;
    a.config.octave1 = b.config.octave1 = 1;
    renderOscScopes (a, sa);
    renderOscScopes (b, sb);
    CHECK (std::memcmp (sa.osc1.data(), sb.osc1.data(), sizeof (float) * OscScopes::kPoints) != 0);
}
