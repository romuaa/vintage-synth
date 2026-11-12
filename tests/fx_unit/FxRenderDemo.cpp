// Hidden: renders an audition WAV of the M5 effect blocks.
//   build/mac-arm64/tests/VintageSynthTests "[.render]"
// Output: $VS_RENDER_DIR (if set) or <working dir>/renders: m5_fx_demo.wav (stereo, 48 kHz, 24 bit)
//
// The same dry pad phrase six times: dry, chorus I, chorus II, delay, reverb, all three.
// The pad is synthesised here (detuned PolyBLEP saws, 2-pole low-pass, slow envelope) so the
// demo does not depend on the synth engine.
#include <catch2/catch_test_macros.hpp>

#include "fx/Chorus.h"
#include "fx/Delay.h"
#include "fx/Reverb.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <vector>

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 256;

struct Note
{
    double start, length;
    int midi;
    double attack, release;
};

float polyBlep (double t, double dt)
{
    if (t < dt)
    {
        t /= dt;
        return static_cast<float> (t + t - t * t - 1.0);
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return static_cast<float> (t * t + t + t + 1.0);
    }
    return 0.0f;
}

/** Mono pad phrase: two held chords, then three short notes (to hear the echoes and the tail). */
std::vector<float> padPhrase (double& lengthOut)
{
    std::vector<Note> notes;
    for (const int m : { 48, 55, 59, 64 }) // C maj7 voicing
        notes.push_back ({ 0.05, 1.7, m, 0.35, 0.9 });
    for (const int m : { 45, 52, 55, 60, 62 }) // A min9 voicing
        notes.push_back ({ 1.8, 1.6, m, 0.35, 0.9 });
    notes.push_back ({ 3.6, 0.15, 72, 0.005, 0.15 });
    notes.push_back ({ 3.9, 0.15, 76, 0.005, 0.15 });
    notes.push_back ({ 4.2, 0.25, 79, 0.005, 0.25 });

    lengthOut = 4.8;
    std::vector<float> out (static_cast<size_t> (lengthOut * kFs), 0.0f);
    const double detune[3] { -0.09, 0.0, 0.11 }; // semitones
    for (const auto& n : notes)
    {
        double phase[3] { 0.0, 0.31, 0.67 };
        const auto s0 = static_cast<size_t> (n.start * kFs);
        const auto len = static_cast<size_t> ((n.length + n.release) * kFs);
        // 2-pole low-pass (two one-poles) at 2.2 kHz.
        const float a = static_cast<float> (1.0 - std::exp (-2.0 * 3.14159265358979 * 2200.0 / kFs));
        float lp1 = 0.0f, lp2 = 0.0f;
        for (size_t i = 0; i < len && s0 + i < out.size(); ++i)
        {
            const double t = static_cast<double> (i) / kFs;
            double env = std::min (1.0, t / n.attack);
            if (t > n.length)
                env *= std::max (0.0, 1.0 - (t - n.length) / n.release);
            float x = 0.0f;
            for (int v = 0; v < 3; ++v)
            {
                const double hz = 440.0 * std::exp2 ((n.midi + detune[v] - 69.0) / 12.0);
                const double dt = hz / kFs;
                x += static_cast<float> (2.0 * phase[v] - 1.0) - polyBlep (phase[v], dt);
                phase[v] += dt;
                if (phase[v] >= 1.0)
                    phase[v] -= 1.0;
            }
            lp1 += a * (x - lp1);
            lp2 += a * (lp1 - lp2);
            out[s0 + i] += 0.06f * static_cast<float> (env * env) * lp2;
        }
    }
    return out;
}

enum Fx { kChorus = 1, kDelay = 2, kReverb = 4 };

void renderScene (const std::vector<float>& phrase, int which, vs::fx::ChorusMode mode, std::vector<float>& outL, std::vector<float>& outR)
{
    vs::fx::Chorus chorus;
    vs::fx::Delay delay;
    vs::fx::Reverb reverb;
    chorus.prepare (kFs, kBlock);
    delay.prepare (kFs, kBlock);
    reverb.prepare (kFs, kBlock);

    const vs::fx::ChorusParams cp { (which & kChorus) != 0 ? mode : vs::fx::ChorusMode::Off, 1.0f };
    vs::fx::DelayParams dp;
    dp.timeSeconds = 0.375f;
    dp.feedback = 0.45f;
    dp.damping = 0.35f;
    dp.pingPong = true;
    dp.level = (which & kDelay) != 0 ? 0.45f : 0.0f;
    vs::fx::ReverbParams rp;
    rp.decaySeconds = 3.5f;
    rp.damping = 0.5f;
    rp.preDelaySeconds = 0.025f;
    rp.level = (which & kReverb) != 0 ? 0.5f : 0.0f;

    const size_t total = phrase.size() + static_cast<size_t> (3.0 * kFs); // room for the tail
    std::vector<float> l (total, 0.0f), r (total, 0.0f);
    std::copy (phrase.begin(), phrase.end(), l.begin());
    std::copy (phrase.begin(), phrase.end(), r.begin());
    for (size_t s = 0; s < total; s += kBlock)
    {
        const int len = static_cast<int> (std::min<size_t> (kBlock, total - s));
        chorus.setParams (cp);
        delay.setParams (dp);
        reverb.setParams (rp);
        chorus.process (l.data() + s, r.data() + s, len);
        delay.process (l.data() + s, r.data() + s, len);
        reverb.process (l.data() + s, r.data() + s, len);
    }
    outL.insert (outL.end(), l.begin(), l.end());
    outR.insert (outR.end(), r.begin(), r.end());
    outL.insert (outL.end(), static_cast<size_t> (0.4 * kFs), 0.0f); // gap
    outR.insert (outR.end(), static_cast<size_t> (0.4 * kFs), 0.0f);
}
} // namespace

TEST_CASE ("Render M5 effects demo WAV", "[.render]")
{
    double len = 0.0;
    const auto phrase = padPhrase (len);

    std::vector<float> l, r;
    using M = vs::fx::ChorusMode;
    renderScene (phrase, 0, M::Off, l, r);                            // 1 dry
    renderScene (phrase, kChorus, M::I, l, r);                        // 2 chorus I
    renderScene (phrase, kChorus, M::II, l, r);                       // 3 chorus II
    renderScene (phrase, kDelay, M::Off, l, r);                       // 4 delay (ping-pong 375 ms)
    renderScene (phrase, kReverb, M::Off, l, r);                      // 5 reverb (3.5 s hall)
    renderScene (phrase, kChorus | kDelay | kReverb, M::I, l, r);     // 6 all three

    float peak = 0.0f;
    for (size_t i = 0; i < l.size(); ++i)
        peak = std::max ({ peak, std::abs (l[i]), std::abs (r[i]) });
    WARN ("rendered " << static_cast<double> (l.size()) / kFs << " s, peak " << juce::Decibels::gainToDecibels (peak) << " dBFS");
    REQUIRE (peak < 1.0f);

    const auto envDir = juce::SystemStats::getEnvironmentVariable ("VS_RENDER_DIR", {});
    auto dir = envDir.isNotEmpty() ? juce::File (envDir) : juce::File::getCurrentWorkingDirectory().getChildFile ("renders");
    dir.createDirectory();
    auto file = dir.getChildFile ("m5_fx_demo.wav");
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (kFs).withNumChannels (2).withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, options);
    REQUIRE (writer != nullptr);
    const float* ch[] { l.data(), r.data() };
    writer->writeFromFloatArrays (ch, 2, static_cast<int> (l.size()));
    WARN ("wrote " << file.getFullPathName());
}
