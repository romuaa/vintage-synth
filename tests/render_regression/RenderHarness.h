// Golden-render regression harness.
//
// A text "script" (timed note / CC / pitch-bend events and parameter settings by ID) is rendered
// offline through the real vs::VintageSynthProcessor using only the public juce::AudioProcessor API.
// The render is reduced to perceptual features (third-octave band energy per ~25 ms frame plus an
// overall level envelope). References store those features, never audio, and the comparison uses
// dB tolerances with a noise-floor gate, so arm64/x64 and clang/MSVC last-bit differences pass.
//
// Script format (one command per line, '#' starts a comment, times in seconds, default 0):
//   rate 48000                       sample rate (header, optional, default 48000)
//   length 3.0                       rendered length in seconds (header, required)
//   channels 2                       analyse left and right (header, optional; default 1 = left
//                                    only, the format of the M1/M2 references)
//   [t] param <id> <plain value>     set a parameter, e.g.  0.5 param lower.vcf.cutoff 0.62
//                                    (plain value = what the parameter's range uses: 0..1 for
//                                     most sliders, the choice index for choice params)
//   [t] on <note> <velocity 1..127> [channel]
//   [t] off <note> [channel]
//   [t] cc <controller> <value 0..127> [channel]
//   [t] bend <0..16383> [channel]    8192 = centre
//   [t] alloff [channel]
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <string>
#include <vector>

namespace vs::rr
{
struct Event
{
    enum class Kind { param, noteOn, noteOff, cc, bend, allOff };
    double time = 0.0;
    Kind kind = Kind::param;
    std::string id;     // param only
    double value = 0.0; // param value, or note/controller number, or bend
    int data = 0;       // velocity / controller value
    int channel = 1;
};

struct Script
{
    double sampleRate = 48000.0;
    double length = 0.0;
    int channels = 1;          // 1: left channel only; 2: left and right
    std::vector<Event> events; // sorted by time (stable)
};

/** Throws std::runtime_error with a line number on malformed input. */
Script parseScript (const std::string& text);

/** Renders mono (channel 0 of the stereo output). All blocks are `blockSize` long except where a
    parameter event splits a block, so parameter changes land at the same sample for any block size. */
std::vector<float> renderScript (const Script& script, int blockSize);

/** The same render, both output channels: { left, right }. */
std::vector<std::vector<float>> renderScriptStereo (const Script& script, int blockSize);

/** The channels a script asks to analyse: { left } or { left, right }. */
std::vector<std::vector<float>> renderScriptChannels (const Script& script, int blockSize);

// ---- Features ----
constexpr int kFftSize = 4096; // 85 ms window at 48 kHz, centred on each frame
constexpr int kHop = 1200;     // 25 ms at 48 kHz
constexpr int kNumBands = 23;  // [0,111) Hz, then 1/3 octaves 125 Hz .. 16 kHz (the last runs to Nyquist)
constexpr float kFloorDb = -120.0f;

struct Features
{
    double sampleRate = 48000.0;
    int frames = 0;
    std::vector<float> bandDb;  // frames * kNumBands, dBFS of band mean-square (0.1 dB resolution)
    std::vector<float> levelDb; // frames, dBFS RMS of the hop

    float band (int frame, int b) const { return bandDb[static_cast<size_t> (frame * kNumBands + b)]; }
};

Features extractFeatures (const std::vector<float>& mono, double sampleRate);

// ---- Comparison ----
struct Tolerances
{
    float bandGateDb = -80.0f;      // cells below this in both renders are ignored
    float levelGateDb = -70.0f;     // same for the level envelope; also defines "active" for the tail
    float cellDb = 3.0f;            // a band cell counts as deviating beyond this
    double maxCellFraction = 0.01;  // allowed fraction of active cells deviating beyond cellDb
    float meanCellDb = 0.4f;        // allowed mean |delta| over active cells
    float levelDb = 2.0f;           // allowed |delta| of the level envelope, any frame
    int tailFrames = 2;             // allowed shift of the last active frame (25 ms each)
};

struct Comparison
{
    bool pass = true;
    std::string report;
    double cellFraction = 0.0, meanCell = 0.0;
    float maxCell = 0.0f, maxLevel = 0.0f;
    int tailShift = 0;
};

Comparison compare (const Features& reference, const Features& candidate, const Tolerances& tol = {});

// ---- Golden files (text, 0.1 dB integers) ----
// Version 1: one channel (left). Version 2: "channels N" followed by N version-1 bodies (L, R).
// A one-channel reference is always written as version 1, so the M1/M2 files stay byte-identical.
std::string featuresToText (const Features& f);
Features featuresFromText (const std::string& text); // throws std::runtime_error
std::string channelFeaturesToText (const std::vector<Features>& channels);
std::vector<Features> channelFeaturesFromText (const std::string& text); // reads version 1 and 2

/** Optional audition output: writes <dir>/<name>.wav if the environment variable VS_RENDER_DIR is set. */
void maybeWriteWav (const std::vector<float>& mono, double sampleRate, const std::string& name);
void maybeWriteWav (const std::vector<std::vector<float>>& channels, double sampleRate, const std::string& name);
} // namespace vs::rr
