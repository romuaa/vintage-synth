// Rendered audio against the committed reference features (docs/architecture.md 8.9.4, 8.4.5 "Noise"):
// the noise third-octave spectrum within +-1.5 dB of the reference's, 50 Hz - 16 kHz, at 8 frequency
// settings (`a_noise_a`, `a_noise_b`). The trace is rendered through Chip::render at 192 kHz internal (8.9.4),
// then through stand-ins of what S3 adds after the voice (the board filters of 8.15.7: one-pole high-pass
// 1.2 Hz and low-pass 15.4 kHz) and a test decimator to 48 kHz, and analysed with the shared SpecAnalysis
// functions. The absolute output scale belongs to S3 (mixer, volume, output convention), so one gain per
// trace is fitted (the mean difference over every compared band of every segment) and every band must then
// lie within the tolerance: the test checks the spectrum's shape, at every frequency setting, not its level.

#include "../../support/Env.h"
#include "bside/chip/Chip.h"
#include "bside/trace/AnalogMeasure.h"
#include "bside/trace/Compare.h"
#include "bside/trace/ReferenceFormat.h"
#include "bside/trace/SpecAnalysis.h"
#include "bside/trace/TraceFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace tr = vs::bside::trace;
namespace chip = vs::bside::chip;

namespace
{
const fs::path kDir { BS_REFERENCE_DIR };

std::string slurp (const fs::path& p)
{
    std::ifstream f (p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/** A linear-phase low-pass FIR (Kaiser-windowed sinc) for 4:1 decimation: pass to 20 kHz, stop from 24 kHz
    at 192 kHz, about -120 dB. */
std::vector<double> decimatorTaps()
{
    constexpr int kTaps = 767;
    constexpr double pi = 3.14159265358979323846;
    constexpr double fc = 22000.0 / 192000.0; // cycles per sample
    constexpr double beta = 12.0;
    auto i0 = [] (double x) {
        double s = 1.0, t = 1.0;
        for (int k = 1; k < 60; ++k)
        {
            t *= (x / (2.0 * k)) * (x / (2.0 * k));
            s += t;
        }
        return s;
    };
    std::vector<double> h (kTaps);
    double sum = 0.0;
    for (int n = 0; n < kTaps; ++n)
    {
        const double m = n - (kTaps - 1) / 2.0;
        const double sinc = std::abs (m) < 1e-12 ? 2.0 * fc : std::sin (2.0 * pi * fc * m) / (pi * m);
        const double r = 2.0 * n / (kTaps - 1.0) - 1.0;
        h[static_cast<std::size_t> (n)] = sinc * i0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0 (beta);
        sum += h[static_cast<std::size_t> (n)];
    }
    for (double& v : h)
        v /= sum;
    return h;
}

/** The trace rendered by our chip, through the stand-ins described above, as a 48 kHz signal of
    specSampleCount samples (sample i at chip cycle i x clock / 48000). */
std::vector<float> renderTrace (const tr::Trace& t)
{
    constexpr double rate = 192000.0;
    chip::Chip c;
    chip::Config cfg;
    cfg.model = t.model == tr::Model::A ? chip::Model::A : chip::Model::B;
    cfg.clockHz = t.clockHz;
    cfg.internalRate = rate;
    c.prepare (cfg);
    const auto h = decimatorTaps();
    const auto n48 = static_cast<std::size_t> (tr::specSampleCount (t));
    const std::size_t delay = static_cast<std::size_t> (chip::Chip::kLatencySamples) + (h.size() - 1) / 2;
    const std::size_t total = 4 * n48 + delay + h.size() + 8;
    std::vector<float> x (total, 0.0f);
    std::size_t done = 0, next = 0;
    while (done < total)
    {
        const std::uint64_t target = next < t.writes.size() ? t.writes[next].cycle : ~std::uint64_t { 0 };
        done += static_cast<std::size_t> (c.renderUntil (x.data() + done, static_cast<int> (total - done), target));
        while (next < t.writes.size() && c.cycle() == t.writes[next].cycle)
        {
            c.write (t.writes[next].reg, t.writes[next].value);
            ++next;
        }
    }
    // the board filters (8.15.7), one-pole each, at the internal rate
    constexpr double pi = 3.14159265358979323846;
    const double scale = static_cast<double> (t.clockHz) / 985248.0; // the reference's filters scale with its clock
    const double aLp = std::exp (-2.0 * pi * 15400.0 * scale / rate);
    const double aHp = std::exp (-2.0 * pi * 1.2 * scale / rate);
    double lp = 0.0, hpIn = 0.0, hp = 0.0;
    for (float& v : x)
    {
        hp = aHp * (hp + static_cast<double> (v) - hpIn);
        hpIn = static_cast<double> (v);
        lp = (1.0 - aLp) * hp + aLp * lp;
        v = static_cast<float> (lp);
    }
    std::vector<float> y (n48, 0.0f);
    for (std::size_t i = 0; i < n48; ++i)
    {
        // output i at internal sample 4 i (chip time), delayed by the render latency and the FIR
        const std::size_t centre = 4 * i + delay;
        double acc = 0.0;
        for (std::size_t k = 0; k < h.size(); ++k)
        {
            const std::size_t idx = centre + (h.size() - 1) / 2 - k;
            if (idx < x.size())
                acc += h[k] * static_cast<double> (x[idx]);
        }
        y[i] = static_cast<float> (acc);
    }
    return y;
}
} // namespace

TEST_CASE ("rendered noise: third-octave spectrum within 1.5 dB of the reference at 8 frequency settings (8.9.4)", "[trace][reference][render]")
{
    for (const char* name : { "a_noise_a", "a_noise_b" })
    {
        const std::string traceText = slurp (kDir / (std::string (name) + ".bstrace"));
        const std::string specText = slurp (kDir / (std::string (name) + ".bsspec"));
        if (traceText.empty() || specText.empty())
            SKIP ("no reference data committed for " << name);
        const auto t = tr::readTrace (traceText);
        REQUIRE (t.ok);
        const auto s = tr::readSpec (specText);
        REQUIRE (s.ok);
        REQUIRE (s.spec.segments.size() == 8);
        auto y = renderTrace (t.trace);

        // one gain for the trace (the absolute scale is S3's)
        auto spectra = [&] (const std::vector<float>& sig) {
            std::vector<tr::SpecSegment> out;
            for (const auto& seg : s.spec.segments)
            {
                tr::SpecSegment m = seg;
                m.spectrum = tr::twelfthOctaveSpectrum (sig.data(), static_cast<std::size_t> (seg.firstFrame) * tr::kSpecHop,
                                                        static_cast<std::size_t> (seg.endFrame) * tr::kSpecHop);
                out.push_back (m);
            }
            return out;
        };
        auto ours = spectra (y);
        double sum = 0.0;
        int count = 0;
        for (std::size_t i = 0; i < ours.size(); ++i)
        {
            const auto a = tr::thirdOctaveFromTwelfth (ours[i].spectrum);
            const auto b = tr::thirdOctaveFromTwelfth (s.spec.segments[i].spectrum);
            for (std::size_t g = 0; g < a.size(); ++g)
            {
                const double centre = 20.0 * std::pow (2.0, (4.0 * static_cast<double> (g) + 1.5) / 12.0);
                if (centre < 50.0 || centre > 16000.0)
                    continue;
                sum += b[g] - a[g];
                ++count;
            }
        }
        REQUIRE (count > 0);
        const double gainDb = sum / count;
        const auto gain = static_cast<float> (std::pow (10.0, gainDb / 20.0));
        for (float& v : y)
            v *= gain;
        ours = spectra (y);
        double worst = 0.0;
        for (std::size_t i = 0; i < ours.size(); ++i)
        {
            const auto v = tr::checkNoiseSpectrum (ours[i], s.spec.segments[i]);
            INFO (name << " " << s.spec.segments[i].name << ": " << v.what);
            CHECK (v.pass);
            worst = std::max (worst, v.deviation);
        }
        WARN (name << ": fitted gain " << gainDb << " dB; worst third-octave deviation over 8 settings " << worst << " dB (allowed 1.5)");
    }
}

// Diagnostic (hidden): the converter traces against the reference, level per frame (envelope converter)
// and the harmonic structure of the waveform segments (waveform converter). There is no tolerance for
// these in 8.9.4 (the full scripted comparison needs S3's output stage); it shows how close our converter
// model (RenderTables.h) comes.
TEST_CASE ("rendered converters against the reference (diagnostic)", "[.render-dac]")
{
    for (const char* name : { "a_env_dac_a", "a_env_dac_b", "a_dac_a", "a_dac_b" })
    {
        const auto t = tr::readTrace (slurp (kDir / (std::string (name) + ".bstrace")));
        const auto s = tr::readSpec (slurp (kDir / (std::string (name) + ".bsspec")));
        REQUIRE (t.ok);
        REQUIRE (s.ok);
        const auto y = renderTrace (t.trace);
        tr::Spec ours;
        std::string why;
        REQUIRE (tr::analyseRender (t.trace, s.spec.traceSha256, y, ours, &why));
        // frame levels: fit one gain over frames where the reference is above -40 dBFS, then report
        double sum = 0.0;
        int n = 0;
        for (int f = 0; f < s.spec.frames; ++f)
        {
            const int a = s.spec.levelTenthDb[static_cast<std::size_t> (f)], b = ours.levelTenthDb[static_cast<std::size_t> (f)];
            if (a > -400 && b > -400)
            {
                sum += (a - b) / 10.0;
                ++n;
            }
        }
        const double gain = n > 0 ? sum / n : 0.0;
        double worst = 0.0, rms = 0.0;
        int worstFrame = -1;
        for (int f = 0; f < s.spec.frames; ++f)
        {
            const int a = s.spec.levelTenthDb[static_cast<std::size_t> (f)], b = ours.levelTenthDb[static_cast<std::size_t> (f)];
            if (a > -400 && b > -400)
            {
                const double d = std::abs (a / 10.0 - b / 10.0 - gain);
                rms += d * d;
                if (d > worst)
                {
                    worst = d;
                    worstFrame = f;
                }
            }
        }
        rms = n > 0 ? std::sqrt (rms / n) : 0.0;
        std::ostringstream o;
        if (testenv::isSet ("BS_DAC_FRAMES"))
            for (int f = 0; f < s.spec.frames; f += 12)
                o << "[" << f << ": " << s.spec.levelTenthDb[static_cast<std::size_t> (f)] / 10.0 << " " << ours.levelTenthDb[static_cast<std::size_t> (f)] / 10.0 + gain << "] ";
        o << name << ": level per frame after a gain of " << gain << " dB: rms " << rms << " dB, worst " << worst << " dB (frame " << worstFrame << ", " << n << " frames)";
        // harmonics 2..8 relative to the fundamental in the waveform segments (a_dac: s16 saw 110, s17 saw 1000, s18 triangle 110, s19 pulse 110)
        if (s.spec.segments.size() >= 20)
        {
            const double f0s[] { 110.0, 1000.0, 110.0, 110.0 };
            for (int k = 0; k < 4; ++k)
            {
                const auto& rs = s.spec.segments[static_cast<std::size_t> (16 + k)];
                const auto& os = ours.segments[static_cast<std::size_t> (16 + k)];
                const double f0 = static_cast<double> (t.trace.clockHz) * std::round (f0s[k] * 16777216.0 / 985248.0) / 16777216.0;
                double worstH = 0.0;
                for (int h = 2; h <= 8; ++h)
                {
                    const double dr = tr::toneLevelDb (rs, h * f0) - tr::toneLevelDb (rs, f0);
                    const double dm = tr::toneLevelDb (os, h * f0) - tr::toneLevelDb (os, f0);
                    if (dr > -60.0)
                        worstH = std::max (worstH, std::abs (dr - dm));
                }
                o << "; " << rs.name << " harmonics 2-8 (above -60 dB) worst " << worstH << " dB";
                if (k == 2)
                    for (int h = 2; h <= 5; ++h)
                        o << " [h" << h << " ref " << tr::toneLevelDb (rs, h * f0) - tr::toneLevelDb (rs, f0) << " ours " << tr::toneLevelDb (os, h * f0) - tr::toneLevelDb (os, f0) << "]";
            }
        }
        WARN (o.str());
    }
}

namespace
{
struct LevelFit
{
    double gain = 0.0, rms = 0.0, worst = 0.0;
    int worstFrame = -1, frames = 0;
};

/** Frame levels of our render against the reference after one gain, over frames from `first` on where
    both lie above -40 dBFS (the start, power-on and the first gate, carries S3's voice DC and board
    high-pass; it is left out). */
LevelFit fitLevels (const tr::Spec& ref, const tr::Spec& ours, int first)
{
    LevelFit f;
    double sum = 0.0;
    for (int i = first; i < ref.frames; ++i)
    {
        const int a = ref.levelTenthDb[static_cast<std::size_t> (i)], b = ours.levelTenthDb[static_cast<std::size_t> (i)];
        if (a > -400 && b > -400)
        {
            sum += (a - b) / 10.0;
            ++f.frames;
        }
    }
    f.gain = f.frames > 0 ? sum / f.frames : 0.0;
    for (int i = first; i < ref.frames; ++i)
    {
        const int a = ref.levelTenthDb[static_cast<std::size_t> (i)], b = ours.levelTenthDb[static_cast<std::size_t> (i)];
        if (a > -400 && b > -400)
        {
            const double d = std::abs (a / 10.0 - b / 10.0 - f.gain);
            f.rms += d * d;
            if (d > f.worst)
            {
                f.worst = d;
                f.worstFrame = i;
            }
        }
    }
    f.rms = f.frames > 0 ? std::sqrt (f.rms / f.frames) : 0.0;
    return f;
}
} // namespace

TEST_CASE ("rendered converters against the reference features (envelope converter levels, waveform harmonics)", "[trace][reference][render][dac][slow]")
{
    // Tolerances (no number in 8.9.4 for the converters; ours, just above what our converter model
    // achieves, so that a coarser model fails): envelope converter (a_env_dac_*, 256 counter values, every
    // frame from the second on; Model B from frame 23: the reference's first frames carry a power-on step
    // decaying through the board's high-pass, -12.5 dB at frame 0 down to below -40 dB at frame 17, S3's):
    // Model A rms 0.15 dB, worst frame 0.75 dB (measured 0.13 / 0.70); Model B rms 0.10 dB, worst 0.40 dB
    // (0.09 / 0.36). Waveform converter (a_dac_*, s16 saw 110 Hz, s17 saw 1 kHz): harmonics 2..8 above
    // -60 dB relative to the fundamental within 0.35 dB on Model A, 0.15 dB on Model B (measured 0.2 / 0.3
    // and 0.1 / 0.1; the reference gives levels in steps of 0.1 dB). Not compared: the pulse (s19), two
    // levels only, which any converter maps alike (its harmonics follow from the width alone); the
    // triangle (s18), whose reference has even harmonics (-34 dB A / -40 dB B at the second) that a converter
    // linear in its bits cannot make: a nonlinearity after the converters, for S3.
    for (const char* name : { "a_env_dac_a", "a_env_dac_b", "a_dac_a", "a_dac_b" })
    {
        const auto t = tr::readTrace (slurp (kDir / (std::string (name) + ".bstrace")));
        const auto s = tr::readSpec (slurp (kDir / (std::string (name) + ".bsspec")));
        if (! t.ok || ! s.ok)
            SKIP ("no reference data committed for " << name);
        const auto y = renderTrace (t.trace);
        tr::Spec ours;
        std::string why;
        REQUIRE (tr::analyseRender (t.trace, s.spec.traceSha256, y, ours, &why));
        const bool envTrace = std::string (name).find ("env") != std::string::npos;
        const bool modelB = std::string (name).back() == 'b';
        if (envTrace)
        {
            const auto f = fitLevels (s.spec, ours, modelB ? 23 : 1);
            WARN (name << ": envelope converter, " << f.frames << " frames after one gain of " << f.gain << " dB: rms " << f.rms << " dB, worst "
                       << f.worst << " dB (frame " << f.worstFrame << ")");
            CHECK (f.frames > 300);
            CHECK (f.rms <= (modelB ? 0.10 : 0.15));
            CHECK (f.worst <= (modelB ? 0.40 : 0.75));
            continue;
        }
        REQUIRE (s.spec.segments.size() >= 20);
        const double f0s[] { 110.0, 1000.0 };
        for (int k = 0; k < 2; ++k) // the saws (see above)
        {
            const auto& rs = s.spec.segments[static_cast<std::size_t> (16 + k)];
            const auto& os = ours.segments[static_cast<std::size_t> (16 + k)];
            const double f0 = static_cast<double> (t.trace.clockHz) * std::round (f0s[k] * 16777216.0 / 985248.0) / 16777216.0;
            double worst = 0.0;
            for (int h = 2; h <= 8; ++h)
            {
                const double dr = tr::toneLevelDb (rs, h * f0) - tr::toneLevelDb (rs, f0);
                const double dm = tr::toneLevelDb (os, h * f0) - tr::toneLevelDb (os, f0);
                if (dr > -60.0)
                    worst = std::max (worst, std::abs (dr - dm));
            }
            WARN (name << " " << rs.name << ": harmonics 2..8 against the reference, worst " << worst << " dB");
            CHECK (worst <= (modelB ? 0.15 : 0.35));
        }
    }
}
