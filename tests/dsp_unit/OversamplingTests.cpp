// Oversampling by host rate (docs/architecture.md 4.4): the factor table, the reported latency
// in every configuration (checked against the real onset of a note), the alias gate in every
// configuration (including 96 kHz at 1x, where there is no decimator), and the M2 Part invariants
// (pitch, release, steal fade, block-size independence) at 1x.
#include <catch2/catch_test_macros.hpp>

#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "dsp/Filters.h"
#include "dsp/Oscillator.h"
#include "engine/Oversampler.h"
#include "engine/Part.h"
#include "params/Mappings.h"

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
    { 44100.0, false, 2 }, { 44100.0, true, 4 },  { 48000.0, false, 2 },  { 48000.0, true, 4 },
    { 88200.0, false, 1 }, { 88200.0, true, 2 },  { 96000.0, false, 1 },  { 96000.0, true, 2 },
    { 176400.0, false, 1 }, { 176400.0, true, 2 }, { 192000.0, false, 1 }, { 192000.0, true, 2 },
};

int expectedLatency (int factor)
{
    if (factor == 1)
        return vs::dsp::Oscillator::kLatency; // BLEP only
    return juce::roundToInt (vs::makeVoiceOversampler (factor)->getLatencyInSamples()
                             + static_cast<float> (vs::dsp::Oscillator::kLatency) / static_cast<float> (factor));
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
    CHECK (vs::voiceOversamplingFactor (22050.0, true) == 4);
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
        CHECK (latency == expectedLatency (c.factor));
        CHECK (latency == proc.getLowerPart().getLatencySamples());

        constexpr int noteAt = 700;
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 69, static_cast<juce::uint8> (127)), noteAt - 512);
        std::vector<float> out;
        for (int b = 0; b < 4; ++b)
        {
            juce::MidiBuffer m;
            if (b == 1)
                m.swapWith (midi);
            buffer.clear();
            proc.processBlock (buffer, m);
            out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + 512);
        }
        int first = -1;
        for (size_t i = 0; i < out.size() && first < 0; ++i)
            if (std::abs (out[i]) > 1.0e-5f)
                first = static_cast<int> (i);
        const int delay = first - noteAt;
        WARN (c.rate << " Hz " << (c.offline ? "offline" : "real time") << " (" << c.factor << "x): reported latency "
                     << latency << " samples, first output " << delay << " samples after the note-on");
        // Nothing before the note; the onset within a few samples of the reported latency (the
        // half-band decimator's group delay is frequency dependent, so the very first audible
        // sample can lead its nominal delay slightly).
        CHECK (delay >= 0);
        CHECK (delay >= latency - 3);
        CHECK (delay <= latency + 2);
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
        if (f > 20000.0)
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
        if (c.rate < 48000.0 && ! c.offline)
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
