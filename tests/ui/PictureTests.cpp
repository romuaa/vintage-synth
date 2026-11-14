// Derived pictures of the Sound page (docs/architecture.md 9.4, 9.6): the filter curve is the voice
// filter's real response, and the oscilloscopes are the voice's own oscillators.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vintage/dsp/FilterResponse.h"
#include "vintage/dsp/Filters.h"
#include "vintage/dsp/OscScope.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace vs;
using Catch::Approx;

namespace
{
/** |H(f)| in dB of the real filter chain (OnePoleHighPass -> OtaLadder, as Voice::renderSample),
    driven by a sine at -60 dBFS (the linear region), measured by I/Q demodulation over whole
    cycles after the transient. */
double measuredDb (const dsp::FilterCurveSettings& s, double hz)
{
    const float fs = static_cast<float> (s.sampleRate);
    dsp::OnePoleHighPass hpf;
    dsp::OtaLadder vcf;
    const float hpfG = dsp::tptOnePoleG (dsp::tptG (static_cast<float> (s.hpfHz), fs));
    const auto c = dsp::OtaLadder::makeCoefs (dsp::tptG (static_cast<float> (s.cutoffHz), fs), static_cast<float> (s.k));
    constexpr double amp = 0.001; // -60 dBFS
    const double w = 2.0 * 3.14159265358979323846 * hz / s.sampleRate;
    // Settle for the slowest pole (the high-pass, or the cutoff with resonance), then measure.
    const double slowest = std::min ({ s.hpfHz, s.cutoffHz, hz });
    const int settle = static_cast<int> (std::min (12.0 * s.sampleRate / slowest + 8000.0, 4.0 * s.sampleRate));
    const int cycles = std::max (20, static_cast<int> (std::ceil (0.2 * hz)));
    const int measure = static_cast<int> (std::round (cycles * s.sampleRate / hz));
    double re = 0.0, im = 0.0;
    for (int n = 0; n < settle + measure; ++n)
    {
        const double x = amp * std::sin (w * n);
        const float hp = hpf.processG (static_cast<float> (x), hpfG);
        const float y = vcf.tick (hp, c.G, c.c1, c.c2, c.c3, c.c4, c.k, c.inv, s.slope24) * c.comp;
        if (n >= settle)
        {
            re += y * std::sin (w * n);
            im += y * std::cos (w * n);
        }
    }
    const double a = 2.0 * std::sqrt (re * re + im * im) / measure;
    return 20.0 * std::log10 (a / amp);
}
} // namespace

TEST_CASE ("Filter curve: FilterResponse equals the real filter (linear region), 30 Hz ... fs/4", "[ui][pictures]")
{
    // 3 cutoffs x 3 resonances (k < 3.8) x 2 slopes x high-pass off/on, at the 48 kHz real-time voice rate.
    constexpr double fs = 96000.0;
    int compared = 0, belowFloor = 0;
    for (float cutoff : { 0.35f, 0.6f, 0.85f })
        for (float resonance : { 0.0f, 0.5f, 0.9f })
            for (int slope : { 0, 1 })
                for (float hpf : { 0.0f, 0.45f })
                {
                    const auto s = dsp::filterCurveSettings (cutoff, resonance, slope, hpf, fs);
                    REQUIRE (s.k < 3.8);
                    for (double hz : { 30.0, 60.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0, 24000.0 })
                    {
                        const double predicted = dsp::filterMagnitudeDb (hz, s);
                        // Below -100 dB the float filter's own rounding dominates (measure above the floor).
                        if (predicted < -100.0)
                        {
                            ++belowFloor;
                            continue;
                        }
                        const double measured = measuredDb (s, hz);
                        INFO ("cutoff " << cutoff << " res " << resonance << " slope " << slope << " hpf " << hpf << " at " << hz << " Hz");
                        CHECK (measured == Approx (predicted).margin (0.2));
                        ++compared;
                    }
                }
    CHECK (compared > 300);
    WARN ("filter curve: " << compared << " points compared, " << belowFloor << " below -100 dB skipped");
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
    // The compensation makes the passband 0 dB at k = 0 and keeps it near 0 dB with resonance.
    CHECK (dsp::filterMagnitudeDb (100.0, dsp::filterCurveSettings (0.9f, 0.0f, 1, 0.0f, 96000.0)) == Approx (0.0).margin (0.05));
}

TEST_CASE ("Oscillator scopes: the picture is a direct run of the voice's own oscillators", "[ui][pictures]")
{
    for (int wave = 0; wave < 6; ++wave)
        for (bool sync : { false, true })
        {
            dsp::OscScopeSettings s;
            s.wave1 = wave;
            s.wave2 = (wave + 2) % 6;
            s.octave1 = 1;
            s.octave2 = 0;
            s.sync = sync;
            s.width = 0.3f;
            dsp::OscScopes scopes;
            dsp::renderOscScopes (s, scopes);

            // The same objects, run directly as Voice::renderSample does (VCO-2 first, VCO-1 synced).
            dsp::Oscillator v1, v2;
            v1.setWave (static_cast<dsp::Wave> (s.wave1));
            v2.setWave (static_cast<dsp::Wave> (s.wave2));
            v1.setSeed (0x51u);
            v2.setSeed (0x52u);
            v1.reset();
            v2.reset();
            const float dt2 = 2.0f / dsp::OscScopes::kPoints;
            const float dt1 = sync ? std::min (dt2 * 2.0f, 0.25f) : dt2;
            std::vector<float> o1, o2;
            for (int i = -dsp::Oscillator::kLatency; i < dsp::OscScopes::kPoints; ++i)
            {
                float wrap2 = -1.0f, unused = -1.0f;
                const float a2 = v2.process (dt2, 0.3f, -1.0f, wrap2);
                const float a1 = v1.process (dt1, 0.3f, sync ? wrap2 : -1.0f, unused);
                if (i >= 0)
                {
                    o1.push_back (a1);
                    o2.push_back (a2);
                }
            }
            INFO ("wave " << wave << " sync " << sync);
            for (size_t i = 0; i < o1.size(); ++i)
            {
                REQUIRE (std::memcmp (&o1[i], &scopes.osc1[i], sizeof (float)) == 0);
                REQUIRE (std::memcmp (&o2[i], &scopes.osc2[i], sizeof (float)) == 0);
            }
        }
    CHECK (dsp::oscScopeRatio ({ 2, 2, 1, 0, false, 0.0f, false, 0.5f }) == Approx (2.0));
    CHECK (dsp::oscScopeRatio ({ 2, 2, 0, 0, false, 12.0f, false, 0.5f }) == Approx (0.5));
}
