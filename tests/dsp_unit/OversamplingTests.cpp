// Oversampling by host rate (docs/architecture.md 4.4): the factor table, the reported latency
// in every configuration (checked against the real onset of a note), the alias gate in every
// configuration (including 96 kHz at 1x, where there is no decimator), and the M2 Part invariants
// (pitch, release, steal fade, block-size independence) at 1x.
#include <catch2/catch_test_macros.hpp>

#include "vintage/PluginProcessor.h"
#include "TestHelpers.h"
#include "vintage/dsp/Envelope.h"
#include "common/dsp/FastMath.h"
#include "vintage/dsp/Filters.h"
#include "vintage/dsp/Lfo.h"
#include "vintage/dsp/Oscillator.h"
#include "common/engine/Oversampler.h"
#include "vintage/engine/Part.h"
#include "vintage/params/PartMappings.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
struct Config
{
    double rate;
    bool offline;
    int factor; // expected
};

// The table of 4.4, at every common host rate.
constexpr Config kConfigs[] {
    { 22050.0, false, 4 }, { 22050.0, true, 4 },  { 32000.0, false, 4 },  { 32000.0, true, 4 },
    { 44100.0, false, 2 }, { 44100.0, true, 4 },  { 48000.0, false, 2 },  { 48000.0, true, 4 },
    { 88200.0, false, 1 }, { 88200.0, true, 2 },  { 96000.0, false, 1 },  { 96000.0, true, 2 },
    { 176400.0, false, 1 }, { 176400.0, true, 2 }, { 192000.0, false, 1 }, { 192000.0, true, 2 },
};

/**
    Exact latency of a configuration: decimator phase delay (down path only) + BLEP delay. Offline
    at twice the real-time factor, Cross Mod notes render at the real-time rate and every note of
    the part is aligned to them (Voice::kRtPathExtraDelay internal samples more).
*/
double exactLatency (int factor, bool offlineAtTwiceRealTime = false)
{
    const int align = offlineAtTwiceRealTime ? vs::Voice::kRtPathExtraDelay : 0;
    return vs::measureDecimatorDelay (factor) + static_cast<double> (vs::dsp::Oscillator::kLatency + align) / static_cast<double> (factor);
}
} // namespace

TEST_CASE ("Oversampling by host rate: the factor table (2x/4x below 88.2 kHz, 1x/2x from 88.2 kHz)", "[os][m3]")
{
    for (const auto& c : kConfigs)
    {
        INFO (c.rate << " Hz, offline " << c.offline);
        CHECK (vs::voiceOversamplingFactor (c.rate, c.offline) == c.factor);
        CHECK (c.rate * c.factor >= 88200.0); // the internal rate never drops below 88.2 kHz
    }
    CHECK (vs::voiceOversamplingFactor (88199.0, false) == 2);
    CHECK (vs::voiceOversamplingFactor (88199.9, false) == 1); // a host reporting 88.2 kHz slightly low
    CHECK (vs::voiceOversamplingFactor (44099.0, false) == 4);
    CHECK (vs::voiceOversamplingFactor (44099.9, false) == 2);
    CHECK (vs::voiceOversamplingFactor (16000.0, false) == 4); // below 22.05 kHz: the largest factor (64 kHz internal)
    CHECK (vs::voiceOversamplingFactor (16000.0, true) == 4);
}

TEST_CASE ("Oversampling by host rate: the processor uses the table and reports the latency of each configuration", "[os][m3][latency]")
{
    for (const auto& c : kConfigs)
    {
        juce::ScopedJuceInitialiser_GUI init;
        vs::VintageSynthProcessor proc;
        // A 1 ms attack on a bright saw: the onset is at the reported latency.
        for (const char* id : { "lower.env2.attack", "lower.vcf.env_mod" })
            proc.getState().getParameter (id)->setValueNotifyingHost (0.0f);
        proc.getState().getParameter ("lower.vcf.cutoff")->setValueNotifyingHost (1.0f);
        proc.setNonRealtime (c.offline);
        proc.setPlayConfigDetails (0, 2, c.rate, 512);
        proc.prepareToPlay (c.rate, 512);

        INFO (c.rate << " Hz, offline " << c.offline);
        CHECK (proc.getOversamplingFactor() == c.factor);
        CHECK (proc.getLowerPart().getOversamplingFactor() == c.factor);
        CHECK (proc.getUpperPart().getOversamplingFactor() == c.factor);
        const int latency = proc.getLatencySamples();
        const bool twice = c.offline && c.factor == 2 * vs::voiceOversamplingFactor (c.rate, false);
        CHECK (latency == juce::roundToInt (exactLatency (c.factor, twice)));
        CHECK (latency == proc.getLowerPart().getLatencySamples());
        CHECK (std::abs (proc.getLowerPart().getExactLatency() - exactLatency (c.factor, twice)) < 1.0e-9);
        WARN (c.rate << " Hz " << (c.offline ? "offline" : "real time") << " (" << c.factor << "x): latency "
                     << proc.getLowerPart().getExactLatency() << " samples, reported " << latency);
    }
}

TEST_CASE ("Offline: the processor renders notes that start with Cross Mod up at the real-time rate (and only offline)", "[os][xmod][offline]")
{
    for (const auto& c : kConfigs)
        for (const float crossMod : { 0.0f, 0.4f })
        {
            juce::ScopedJuceInitialiser_GUI init;
            vs::VintageSynthProcessor proc;
            proc.getState().getParameter ("lower.vco1.cross_mod")->setValueNotifyingHost (crossMod);
            proc.setNonRealtime (c.offline);
            proc.setPlayConfigDetails (0, 2, c.rate, 512);
            proc.prepareToPlay (c.rate, 512);
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 10);
            proc.processBlock (buffer, midi);

            const bool twice = c.offline && c.factor == 2 * vs::voiceOversamplingFactor (c.rate, false);
            int onRtPath = 0, sounding = 0;
            for (int v = 0; v < vs::kMaxVoices; ++v)
            {
                sounding += proc.getLowerPart().getVoice (v).isActive() ? 1 : 0;
                onRtPath += proc.getLowerPart().getVoice (v).usesRealTimePair() ? 1 : 0;
            }
            INFO (c.rate << " Hz, offline " << c.offline << ", Cross Mod " << crossMod);
            CHECK (sounding == 1);
            CHECK (onRtPath == (twice && crossMod > 0.0f ? 1 : 0));
            CHECK (buffer.getMagnitude (0, 512) > 0.0f);
        }
}

TEST_CASE ("Latency: the reported latency is the true delay of the voice path, measured independently (+-0.5 sample)", "[os][m3][latency]")
{
    // Method (independent of Part's own measurement): a Part at host rate r with factor f and a
    // Part at host rate r f with factor 1 run the voices at the same internal rate with the same
    // seed, so they compute the same oversampled signal; the 1x one outputs it directly (delayed
    // by the BLEP look-ahead, kLatency internal samples, by construction), the other decimates
    // it. The delay of the decimated output against every f-th sample of the 1x output is
    // measured as a phase delay at the note's fundamental (a low A, 110 Hz: coherent projection
    // on sin/cos over 0.5 s of steady state). True latency = that delay + kLatency / f host
    // samples. The plugin reports it rounded to the nearest sample, so it must be within 0.5.
    struct Pair
    {
        double rate;
        int factor;
    };
    for (const auto pair : { Pair { 22050.0, 4 }, Pair { 44100.0, 2 }, Pair { 48000.0, 2 }, Pair { 48000.0, 4 }, Pair { 96000.0, 2 } })
    {
        auto render = [] (double rate, int factor, int noteAt, int length) {
            vs::Part part;
            vs::PartParams p;
            p.vco1Wave = 0; // sine
            p.sourceMix = 0.0f;
            p.vcfCutoff = 1.0f;
            p.vcfEnvMod = 0.0f;
            p.vcfKeyFollow = 0.0f;
            p.vcfResonance = 0.0f;
            p.env2Attack = 0.0f;
            p.env2Sustain = 1.0f;
            part.setParams (p);
            part.prepare (rate, 512, factor, 777u);
            std::vector<float> out (static_cast<size_t> (length));
            for (int start = 0; start < length; start += 512)
            {
                const int len = std::min (512, length - start);
                std::vector<vs::NoteEvent> ev;
                if (noteAt >= start && noteAt < start + len)
                    ev.push_back ({ noteAt - start, vs::EventType::NoteOn, 45, 0.8f });
                part.setParams (p);
                part.render (out.data() + start, len, ev);
            }
            return std::pair { out, part.getLatencySamples() };
        };
        const int f = pair.factor;
        const int len = static_cast<int> (pair.rate); // 1 s
        const auto [decimated, reported] = render (pair.rate, f, 100, len);
        const auto [direct, latency1x] = render (pair.rate * f, 1, 100 * f, len * f);
        REQUIRE (latency1x == vs::dsp::Oscillator::kLatency);

        const double w = 2.0 * juce::MathConstants<double>::pi * 110.0 / pair.rate; // per host sample
        double sd = 0, cd = 0, sx = 0, cx = 0;
        for (int n = len / 2; n < len; ++n)
        {
            const double t = w * n;
            sd += decimated[static_cast<size_t> (n)] * std::sin (t);
            cd += decimated[static_cast<size_t> (n)] * std::cos (t);
            sx += direct[static_cast<size_t> (n * f)] * std::sin (t);
            cx += direct[static_cast<size_t> (n * f)] * std::cos (t);
        }
        double dphi = std::atan2 (cx, sx) - std::atan2 (cd, sd); // phase lag of the decimated signal
        while (dphi > juce::MathConstants<double>::pi)
            dphi -= 2.0 * juce::MathConstants<double>::pi;
        while (dphi < -juce::MathConstants<double>::pi)
            dphi += 2.0 * juce::MathConstants<double>::pi;
        const double decimatorDelay = dphi / w;
        const double trueLatency = decimatorDelay + static_cast<double> (vs::dsp::Oscillator::kLatency) / f;
        INFO (pair.rate << " Hz " << f << "x: decimator " << decimatorDelay << ", true latency " << trueLatency << ", reported " << reported);
        WARN (pair.rate << " Hz " << f << "x: decimator delay " << decimatorDelay << " samples, true latency " << trueLatency
                        << ", reported " << reported << " (round-trip value JUCE reports: "
                        << vs::makeVoiceOversampler (f)->getLatencyInSamples() << ")");
        CHECK (std::abs (static_cast<double> (reported) - trueLatency) <= 0.5);
    }
}

namespace
{
struct AliasResult
{
    double worstDb;
    double worstHz;
};

/**
    Worst non-harmonic component in 0..20 kHz relative to the fundamental, for an oscillator
    running at rate x factor and taken back to `rate` through the production path: the voice
    decimator for factor > 1, nothing at 1x (the Part renders straight into the output).
    Bin-centred, coherent (rectangular window), as in OscillatorTests.
*/
AliasResult measureAliasing (vs::dsp::Wave wave, double hz, float width, double rate, int factor)
{
    constexpr int order = 15, n = 1 << order, block = 512;
    int k = static_cast<int> (std::lround (hz * n / rate));
    k = (k % 2 == 0) ? k + 1 : k;

    auto os = factor > 1 ? vs::makeVoiceOversampler (factor) : nullptr;
    if (os != nullptr)
        os->initProcessing (block);
    juce::AudioBuffer<float> silence (1, block), out (1, block);
    silence.clear();

    vs::dsp::Oscillator osc;
    osc.setWave (wave);
    osc.reset();
    const float dt = static_cast<float> (k) / static_cast<float> (factor * n); // exact in float

    std::vector<float> captured;
    const int warmup = 16384;
    std::vector<float> raw (static_cast<size_t> (block * factor));
    for (int done = 0; done < warmup + n; done += block)
    {
        float wrap = 0.0f;
        if (os != nullptr)
        {
            auto up = os->processSamplesUp (juce::dsp::AudioBlock<float> (silence));
            float* p = up.getChannelPointer (0);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
                p[i] = osc.process (dt, width, -1.0f, wrap);
            juce::dsp::AudioBlock<float> ob (out);
            os->processSamplesDown (ob);
        }
        else
        {
            float* p = out.getWritePointer (0);
            for (int i = 0; i < block; ++i)
                p[i] = osc.process (dt, width, -1.0f, wrap);
        }
        if (done >= warmup)
            captured.insert (captured.end(), out.getReadPointer (0), out.getReadPointer (0) + block);
    }

    const auto mag = vs::test::magnitudeSpectrum (captured, order);
    const double fundamental = mag[static_cast<size_t> (k)];
    AliasResult r { -300.0, 0.0 };
    for (size_t b = 1; b < mag.size(); ++b)
    {
        const double f = static_cast<double> (b) * rate / n;
        // The audio band: 0..20 kHz, and never more than 20/48 of the host rate. At 48 kHz and
        // above that is 20 kHz; at 22.05 / 32 kHz hosts (4x) it is 9.19 / 13.3 kHz, the same
        // fraction of Nyquist: above it lies the decimator's transition band at every rate.
        if (f > std::min (20000.0, rate * 20.0 / 48.0))
            break;
        if (b % static_cast<size_t> (k) == 0)
            continue;
        const double db = vs::test::toDb (mag[b] / fundamental);
        if (db > r.worstDb)
            r = { db, f };
    }
    return r;
}
} // namespace

TEST_CASE ("Alias gate (<= -100 dB in 0..20 kHz up to a 5 kHz fundamental) in every oversampling configuration", "[os][m3][alias]")
{
    struct Wave
    {
        vs::dsp::Wave wave;
        float width;
        const char* name;
    };
    const Wave waves[] { { vs::dsp::Wave::Saw, 0.5f, "saw" }, { vs::dsp::Wave::Square, 0.5f, "square" },
                         { vs::dsp::Wave::Pulse, 0.2f, "pulse20" }, { vs::dsp::Wave::Triangle, 0.5f, "triangle" } };
    for (const auto& c : kConfigs)
    {
        if (c.rate > 44000.0 && c.rate < 44200.0 && ! c.offline)
            continue; // 44.1 kHz 2x is the long-standing case in OscillatorTests (at 48 kHz); keep the run short
        double worst = -300.0;
        for (const auto& w : waves)
            for (const double hz : { 110.0, 1000.0, 2000.0, 5000.0 })
            {
                const auto r = measureAliasing (w.wave, hz, w.width, c.rate, c.factor);
                INFO (c.rate << " Hz " << c.factor << "x, " << w.name << " @ " << hz << " Hz: worst alias " << r.worstDb << " dB at " << r.worstHz << " Hz");
                CHECK (r.worstDb <= -100.0);
                worst = std::max (worst, r.worstDb);
            }
        WARN ("alias floor " << c.rate << " Hz " << c.factor << "x (" << (c.offline ? "offline" : "real time") << "): worst " << worst << " dB");
    }
}

// Hard sync and Cross Mod alias checks (every VCO-1 waveform, every configuration) and the real-time
// vs offline equivalence of Cross Mod patches: SyncCrossModTests.cpp.

TEST_CASE ("Ladder filter at the new internal rates: -12.04 / -6.02 dB at fc, self-oscillation at fc +-3 %", "[os][m3][vcf]")
{
    // The internal rate is now 88.2 .. 384 kHz (1x at 88.2 kHz up to 2x at 192 kHz offline).
    for (const float fs : { 88200.0f, 176400.0f, 384000.0f })
    {
        auto gainDb = [fs] (float sineHz, float fc, bool slope24) {
            vs::dsp::OtaLadder f;
            const float g = vs::dsp::tptG (fc, fs);
            const float amp = 0.01f;
            const int n = static_cast<int> (fs);
            std::vector<float> y (static_cast<size_t> (n));
            for (int i = 0; i < n; ++i)
                y[static_cast<size_t> (i)] = f.process (amp * std::sin (2.0f * vs::dsp::kPi * sineHz * static_cast<float> (i) / fs), g, 0.0f, slope24);
            const auto half = static_cast<size_t> (n / 2);
            return vs::test::toDb (vs::test::rms (y.data() + half, half) * std::sqrt (2.0) / amp);
        };
        for (const float fc : { 200.0f, 1000.0f, 8000.0f })
        {
            INFO ("fs " << fs << ", fc " << fc);
            CHECK (std::abs (gainDb (fc, fc, true) - (-12.04)) < 0.2);
            CHECK (std::abs (gainDb (fc, fc, false) - (-6.02)) < 0.2);
        }
        for (const bool slope24 : { true, false })
        {
            vs::dsp::OtaLadder f;
            const float g = vs::dsp::tptG (1000.0f, fs);
            std::vector<float> y (static_cast<size_t> (fs));
            for (size_t i = 0; i < y.size(); ++i)
                y[i] = f.process (i == 0 ? 0.1f : 0.0f, g, 4.15f, slope24);
            const std::vector<float> tail (y.begin() + static_cast<std::ptrdiff_t> (fs / 2), y.end());
            const double hz = vs::test::measureFrequency (tail, static_cast<double> (fs));
            INFO ("fs " << fs << ", self-oscillation at " << hz << " Hz, slope24 " << slope24);
            CHECK (std::abs (hz / 1000.0 - 1.0) < 0.03);
        }
    }
}

namespace
{
/** A Part at host rate `rate` with oversampling `factor`, rendered in host blocks. */
struct RatedRig
{
    vs::Part part;
    vs::PartParams params;
    double rate;
    int maxBlock = 512;

    RatedRig (double r, int factor) : rate (r)
    {
        part.setParams (params);
        part.prepare (rate, maxBlock, factor, 1234u);
    }
    void apply()
    {
        part.setParams (params);
        part.reset();
    }
    std::vector<float> render (int numSamples, const std::vector<vs::NoteEvent>& events, int blockSize = 256)
    {
        REQUIRE (blockSize >= 1);
        REQUIRE (blockSize <= maxBlock);
        std::vector<float> out (static_cast<size_t> (numSamples));
        std::vector<vs::NoteEvent> chunk;
        for (int start = 0; start < numSamples; start += blockSize)
        {
            const int len = std::min (blockSize, numSamples - start);
            REQUIRE (static_cast<size_t> (start + len) <= out.size());
            chunk.clear();
            for (auto e : events)
                if (e.offset >= start && e.offset < start + len)
                {
                    e.offset -= start;
                    chunk.push_back (e);
                }
            part.setParams (params);
            part.render (out.data() + start, len, chunk);
        }
        return out;
    }
};

vs::NoteEvent on (int pos, int note, float vel = 0.8f) { return { pos, vs::EventType::NoteOn, static_cast<std::uint8_t> (note), vel }; }
vs::NoteEvent off (int pos, int note) { return { pos, vs::EventType::NoteOff, static_cast<std::uint8_t> (note), 0.0f }; }

void sinePatch (vs::PartParams& p)
{
    p.vco1Wave = 0;
    p.sourceMix = 0.0f;
    p.vcfCutoff = 1.0f;
    p.vcfEnvMod = 0.0f;
    p.vcfResonance = 0.0f;
    p.vcfKeyFollow = 0.0f;
    p.env2Attack = 0.0f;
    p.env2Sustain = 1.0f;
    p.velToVca = 0.0f;
    p.vcaLevel = 0.5f;
}

double maxStep (const std::vector<float>& x, int from, int to)
{
    double m = 0.0;
    for (int i = std::max (1, from); i < std::min (to, static_cast<int> (x.size())); ++i)
        m = std::max (m, static_cast<double> (std::abs (x[static_cast<size_t> (i)] - x[static_cast<size_t> (i - 1)])));
    return m;
}

constexpr struct
{
    double rate;
    int factor;
} kOneX[] { { 88200.0, 1 }, { 96000.0, 1 }, { 192000.0, 1 }, { 192000.0, 2 } };
} // namespace

TEST_CASE ("Part at 1x (and 192 kHz 2x): static pitch error <= 0.1 cent, note-on sample-accurate, release to silence", "[os][m3][part]")
{
    for (const auto& c : kOneX)
    {
        for (const int note : { 33, 69, 105 })
        {
            RatedRig rig (c.rate, c.factor);
            sinePatch (rig.params);
            rig.apply();
            const int len = static_cast<int> (c.rate);
            const auto out = rig.render (len * 2, { on (0, note) });
            const double hz = vs::test::measureFrequency (std::vector<float> (out.begin() + len, out.end()), c.rate);
            const double cents = 1200.0 * std::log2 (hz / (440.0 * std::pow (2.0, (note - 69) / 12.0)));
            INFO (c.rate << " Hz " << c.factor << "x, note " << note << ": " << cents << " cent");
            CHECK (std::abs (cents) <= 0.1);
        }
        for (const int pos : { 0, 257, 1001 })
        {
            RatedRig rig (c.rate, c.factor);
            rig.params.env2Attack = 0.0f; // 1 ms
            rig.params.vcfCutoff = 1.0f;  // a bright onset: the threshold is crossed at once at any rate
            rig.params.vcfEnvMod = 0.0f;
            rig.apply();
            const auto out = rig.render (4096, { on (pos, 60) });
            int first = -1;
            for (size_t i = 0; i < out.size() && first < 0; ++i)
                if (std::abs (out[i]) > 1.0e-5f)
                    first = static_cast<int> (i);
            INFO (c.rate << " Hz " << c.factor << "x: note at " << pos << ", first sound at " << first << ", latency " << rig.part.getLatencySamples());
            CHECK (first >= pos);
            CHECK (first <= pos + rig.part.getLatencySamples() + 4);
        }
        {
            RatedRig rig (c.rate, c.factor);
            const int len = static_cast<int> (c.rate);
            const auto out = rig.render (len * 2, { on (0, 60), off (len / 4, 60) });
            CHECK (vs::test::rms (out.data() + len / 8, static_cast<size_t> (len / 16)) > 1.0e-3);
            CHECK (vs::test::rms (out.data() + len, static_cast<size_t> (len)) < 1.0e-6);
            CHECK_FALSE (rig.part.isSounding());
        }
    }
}

TEST_CASE ("Part at 1x: the steal fade is click-free and renders do not depend on the block size (Vintage 1)", "[os][m3][part][control]")
{
    for (const auto& c : kOneX)
    {
        // Steal fade: the largest step during the fade stays within the signal's own steps.
        {
            RatedRig rig (c.rate, c.factor);
            sinePatch (rig.params);
            rig.apply();
            const int stealAt = static_cast<int> (c.rate / 2);
            std::vector<vs::NoteEvent> e;
            for (int n = 0; n < vs::kMaxVoices; ++n)
                e.push_back (on (0, 24 + n));
            e.push_back (on (stealAt, 26 + vs::kMaxVoices));
            const auto out = rig.render (stealAt + static_cast<int> (c.rate / 5), e);
            const int fadeHost = juce::roundToInt (vs::Part::kStealFadeSeconds * c.rate), latency = rig.part.getLatencySamples();
            const int w = static_cast<int> (c.rate / 20);
            const double before = maxStep (out, stealAt - w, stealAt);
            const double after = maxStep (out, stealAt + fadeHost + latency + 1, stealAt + fadeHost + latency + w);
            const double fade = maxStep (out, stealAt, stealAt + fadeHost + latency);
            INFO (c.rate << " Hz " << c.factor << "x: steps before " << before << ", fade " << fade << ", after " << after);
            CHECK (fade <= 1.1 * std::max (before, after));
        }
        // Block-size independence with voice reuse at full Vintage (Solo reuses one voice).
        for (const int mode : { 0, 2 })
        {
            auto run = [&] (int block) {
                RatedRig rig (c.rate, c.factor);
                rig.part.setVintage (1.0f);
                rig.params.assignMode = mode;
                rig.params.vcfResonance = 0.6f;
                rig.params.vcoSync = 1;
                rig.params.vco1CrossMod = 0.3f;
                rig.params.vcoModLfo = 0.1f;
                rig.params.portaTime = 0.2f;
                rig.apply();
                const int s = static_cast<int> (c.rate / 48000.0);
                return rig.render (72000 * s, { on (0, 88), off (2000 * s, 88), on (24001 * s, 57), off (30011 * s + 3, 57), on (48003 * s, 64),
                                                { 50001 * s, vs::EventType::PitchBend, 0, 0.4f } }, block);
            };
            const auto a = run (512), b = run (97);
            double signal = 0.0, diff = 0.0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                signal += static_cast<double> (a[i]) * a[i];
                diff += (static_cast<double> (b[i]) - a[i]) * (static_cast<double> (b[i]) - a[i]);
            }
            REQUIRE (signal > 1.0);
            const double db = 10.0 * std::log10 (std::max (diff, 1.0e-30) / signal);
            INFO (c.rate << " Hz " << c.factor << "x, mode " << mode << ": " << db << " dB");
            CHECK (db < -100.0);
        }
    }
}

// =================================================================================================
// Precision at the internal rates of table 4.4 (88.2 .. 384 kHz): long envelopes, LFO rate and
// delay, one-pole coefficients. At 384 kHz a 10 s segment moves ~1e-6 per sample, below a float
// ulp near 1: float math stalled or mis-timed these (measured +7.7 % attack, 0.42 dB sustain).
// =================================================================================================
namespace
{
constexpr double kInternalRates[] { 88200.0, 96000.0, 176400.0, 192000.0, 352800.0, 384000.0 };
}

TEST_CASE ("Precision: one-pole coefficients are exact at every internal rate", "[os][m3][precision]")
{
    for (const double fs : kInternalRates)
        for (const double tau : { 0.0005, 0.005, 1.2, 10.0 / 4.605170185988091 })
        {
            const double exact = -std::expm1 (-1.0 / (tau * fs));
            const double naive = 1.0 - std::exp (-1.0 / (tau * fs));
            const double d = vs::dsp::onePoleCoefD (tau, fs);
            const double f = static_cast<double> (vs::dsp::onePoleCoef (static_cast<float> (tau), static_cast<float> (fs)));
            INFO ("fs " << fs << ", tau " << tau);
            CHECK (std::abs (d / exact - 1.0) < 1.0e-12);
            CHECK (std::abs (naive / exact - 1.0) < 1.0e-8); // the double formula agrees: exact enough
            CHECK (std::abs (f / exact - 1.0) < 2.0e-7);     // float rounding of the result only
        }
}

TEST_CASE ("Precision: 10 s envelope segments keep their time and reach the sustain level at every internal rate", "[os][m3][precision][env]")
{
    for (const double fs : kInternalRates)
    {
        INFO ("internal rate " << fs);
        vs::dsp::Envelope env;
        env.setSampleRate (static_cast<float> (fs));
        env.setTimes (10.0f, 10.0f, 10.0f);
        env.noteOn();

        // Attack: reaches the peak (1.0) after 10 s.
        std::int64_t n = 0;
        while (env.getStage() == vs::dsp::Envelope::Stage::Attack && n < static_cast<std::int64_t> (30.0 * fs))
        {
            env.process (0.5f);
            ++n;
        }
        const double attackSec = static_cast<double> (n) / fs;
        CHECK (std::abs (attackSec / 10.0 - 1.0) < 1.0e-4);

        // Decay toward 0.5: 99 % of the way after 10 s, and on the target (no stall) after 60 s.
        const auto tenSeconds = static_cast<std::int64_t> (std::llround (10.0 * fs));
        float level = 1.0f;
        for (std::int64_t i = 0; i < tenSeconds; ++i)
            level = env.process (0.5f);
        CHECK (std::abs ((static_cast<double> (level) - 0.5) / 0.5 - 0.01) < 1.0e-4);
        for (std::int64_t i = 0; i < 5 * tenSeconds; ++i)
            level = env.process (0.5f);
        INFO ("sustain after 60 s: " << level << " (" << 20.0 * std::log10 (static_cast<double> (level) / 0.5) << " dB)");
        CHECK (std::abs (static_cast<double> (level) - 0.5) < 1.0e-6);

        // Release: 1 % of the start level after 10 s.
        env.noteOff();
        for (std::int64_t i = 0; i < tenSeconds; ++i)
            level = env.process (0.5f);
        CHECK (std::abs (static_cast<double> (level) / 0.5 - 0.01) < 1.0e-5);
        WARN ("envelope at " << fs << " Hz: attack " << attackSec << " s (set 10 s), release end " << level / 0.5f << " (expected 0.01)");
    }
}

TEST_CASE ("Precision: LFO rate and delay are exact at every internal rate (0.05 Hz and 40 Hz, 5 s delay)", "[os][m3][precision][lfo]")
{
    for (const double fs : kInternalRates)
    {
        for (const float hz : { 0.05f, 40.0f })
        {
            vs::dsp::Lfo lfo;
            lfo.prepare (static_cast<float> (fs), 7u);
            lfo.setWave (vs::dsp::LfoWave::Square);
            lfo.setRate (hz);
            // Rising zero crossings of the (slightly smoothed) square, one per period.
            const double seconds = hz < 1.0f ? 60.0 : 1.0;
            const auto total = static_cast<std::int64_t> (std::llround (seconds * fs));
            float prev = lfo.process();
            std::int64_t first = -1, last = -1;
            int periods = 0;
            for (std::int64_t i = 1; i < total; ++i)
            {
                const float v = lfo.process();
                if (prev < 0.0f && v >= 0.0f)
                {
                    if (first < 0)
                        first = i;
                    else
                        ++periods;
                    last = i;
                }
                prev = v;
            }
            REQUIRE (periods >= 1);
            const double measured = static_cast<double> (periods) * fs / static_cast<double> (last - first);
            INFO ("fs " << fs << ", LFO " << hz << " Hz: measured " << measured << " Hz");
            CHECK (std::abs (measured / static_cast<double> (hz) - 1.0) < 2.0e-4);
        }

        vs::dsp::Lfo lfo;
        lfo.prepare (static_cast<float> (fs), 7u);
        lfo.setDelay (5.0f);
        lfo.trigger();
        std::int64_t done = 0;
        auto runTo = [&] (double sec) {
            for (const auto target = static_cast<std::int64_t> (std::llround (sec * fs)); done < target; ++done)
                lfo.process();
            return lfo.delayAmount();
        };
        INFO ("fs " << fs);
        CHECK (runTo (2.4) <= 0.0f);                      // held for the first half
        CHECK (std::abs (runTo (3.75) - 0.5f) < 1.0e-4f); // half faded in at 3/4
        CHECK (std::abs (runTo (5.0) - 1.0f) < 1.0e-6f);  // fully in at 5 s
    }
}
