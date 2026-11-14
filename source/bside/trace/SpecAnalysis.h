#pragma once

// The measurement side of the .bsspec format (docs/architecture.md 8.9.3, docs/bside-oracle-adapter.md):
// how a 48 kHz mono render of a trace is reduced to the spec's frames and segment spectra. The
// reference adapter and our own replay tests both call these functions, so both sides measure
// alike. JUCE-free; allocates (offline analysis, never on the audio thread).
//
// Time base: output sample i lies at i / 48000 s, i.e. at chip cycle i * clock / 48000. A trace
// renders specSampleCount() samples (cycles 0 .. end). Frames are 1200 samples (25 ms).
//
// Frames: the render-regression feature layout (tests/render_regression/RenderHarness.cpp,
// extractFeatures): per frame a Hann-windowed 4096-point FFT centred on the frame centre, power summed
// into 23 bands ([0, 111) Hz, then third octaves from 125 Hz), and the RMS level of the frame's own
// 1200 samples; dB values floored at -120 and rounded to 0.1 dB. The band limit: FFT bins at or
// above 20 kHz are not counted, so the last band (band 22) runs from 16 kHz to 20 kHz, not to
// Nyquist. 20 .. 24 kHz is where the reference's resampler and our decimator have their (different)
// transition bands; both sides measure through this one function, so neither filters separately.
//
// Segments (the analysis plan): a trace format has no plan of its own, so the plan is derived from
// the trace alone. The write cycles (and cycle 0) split the render into gaps; a gap yields a segment
// from the 8th whole frame after the frame holding its first cycle (settle time 200 ms) up to the
// frame holding the next write (exclusive) or the last whole frame, if that leaves at least 14 frames
// (350 ms, enough for one 16384-point window). Segments are named s00, s01, ... in time order; at
// most 64 (later gaps are not marked).
//
// Segment spectrum: Welch average of Hann-windowed 16384-point FFTs (hop 8192) over the segment's
// samples, as mean-square power; bin k of the 120 is the power in the 1/12-octave band centred on
// 20 Hz * 2^(k/12) (edges at +-1/24 octave), counting each FFT bin by the fraction of its width
// inside the band; 0.1 dB, floored at -120 dB.

#include "bside/trace/ReferenceFormat.h"
#include "bside/trace/TraceFormat.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vs::bside::trace
{
constexpr int kSpecRate = 48000;
constexpr int kSpecHop = 1200;          // 25 ms
constexpr int kFeatureFftSize = 4096;   // as the render-regression harness
constexpr int kSegmentFftSize = 16384;  // segment spectra
constexpr std::uint32_t kSegmentSettleFrames = 8;
constexpr std::uint32_t kSegmentMinFrames = 14;
/** FFT bins at or above this frequency are not counted in the frame bands (see above). */
constexpr double kSpecBandLimitHz = 20000.0;

/** Number of 48 kHz output samples a trace renders: ceil (end * 48000 / clock). */
std::uint64_t specSampleCount (const Trace& t);

/** Number of frames of the trace's spec: max (1, ceil (samples / 1200)). */
std::uint64_t specFrameCount (const Trace& t);

/** The frame that holds chip cycle `cycle`: floor (cycle * 40 / clock). */
std::uint64_t frameOfCycle (const Trace& t, std::uint64_t cycle);

struct PlannedSegment
{
    std::string name;
    std::uint32_t firstFrame = 0, endFrame = 0;
};

/** The marked segments of a trace (see the header comment). Empty for a trace that does not validate. */
std::vector<PlannedSegment> segmentPlan (const Trace& t);

/** Frame features of a 48 kHz mono signal: `level` gets one value per frame, `bands` frames * 23,
    both in 0.1 dB. `frames` frames are produced; samples beyond the signal count as silence. Bins at
    or above `bandLimitHz` are left out of the bands (the spec uses kSpecBandLimitHz; 24000 = no limit,
    the render-regression harness's own layout). The level is not band-limited. */
void featureFrames (const float* mono, std::size_t n, int frames, std::vector<std::int16_t>& level, std::vector<std::int16_t>& bands,
                    double bandLimitHz = kSpecBandLimitHz);

/** The 1/12-octave spectrum of samples [first, end) of a 48 kHz signal (end - first >= 16384). */
std::array<std::int16_t, kSpecBins> twelfthOctaveSpectrum (const float* mono, std::size_t first, std::size_t end);

/** The complete spec of a trace's render (`mono` holds specSampleCount (t) samples). False (and `why`)
    when the trace does not validate, the sample count is wrong, or the spec would exceed the format. */
bool analyseRender (const Trace& t, const std::string& traceSha256, const std::vector<float>& mono, Spec& out, std::string* why = nullptr);
} // namespace vs::bside::trace
