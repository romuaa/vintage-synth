#pragma once

// The comparer of docs/architecture.md 8.9.4: the tolerance table as named constants and the
// functions that apply it. Digital state and read registers are compared exactly (Replay.h); this
// file holds the audio-side comparisons, which work on measurements and on .bsspec features (the
// repository never holds the reference's audio). JUCE-free.

#include "bside/trace/ReferenceFormat.h"

#include <cstddef>
#include <string>
#include <vector>

namespace vs::bside::trace
{
namespace tolerance
{
constexpr double filterCornerRelative = 0.05; // -3 dB point at 16 cutoff values, resonance 0 (Model A: the reference's default curve)
constexpr double resonancePeakDb = 1.0;       // resonance 0..15
constexpr double distortionDb = 3.0;          // Model A harmonics 2..5 at three drive levels
constexpr double voiceDcDb = 1.0;             // voice DC step on a volume write (Model A)
constexpr double noiseThirdOctaveDb = 1.5;    // noise third-octave spectrum, 50 Hz .. 16 kHz
constexpr double scriptedCellDb = 3.0;        // scripted traces: each band cell
constexpr double scriptedOutlierFraction = 0.05;
constexpr double scriptedMeanCellDb = 1.0;
constexpr double scriptedLevelDb = 0.5;       // overall level
constexpr double scriptedBandGateDb = -80.0;  // cells at or below this in both are ignored
constexpr int maxDelaySamples = 256;          // delay estimation window (+- samples at 48 kHz)
} // namespace tolerance

struct Verdict
{
    bool pass = false;
    double deviation = 0.0; // |measured - reference|, in the quantity's unit (relative for the corner)
    double allowed = 0.0;
    std::string what;       // one line, e.g. "resonance peak: 0.7 dB (allowed 1.0 dB)"
};

/** |measuredHz / referenceHz - 1| <= 5 %. */
Verdict checkFilterCorner (double measuredHz, double referenceHz);
/** |measuredDb - referenceDb| <= allowedDb, named for the report. */
Verdict checkWithinDb (const char* quantity, double measuredDb, double referenceDb, double allowedDb);
Verdict checkResonancePeak (double measuredDb, double referenceDb);
Verdict checkVoiceDcStep (double measuredDb, double referenceDb);
/** Harmonics 2..5 (four values) at one drive level, each within 3 dB. */
Verdict checkDistortion (const double measuredDb[4], const double referenceDb[4]);

/** The 1/12-octave spectrum (0.1 dB values) as 30 third-octave levels in dB: groups of four
    adjacent bins averaged as power; level k has its centre at 20 Hz * 2^((4k + 1.5) / 12). */
std::vector<double> thirdOctaveFromTwelfth (const std::array<std::int16_t, kSpecBins>& spectrum);
/** Noise spectrum: every third-octave level whose centre lies in 50 Hz .. 16 kHz within 1.5 dB. */
Verdict checkNoiseSpectrum (const SpecSegment& measured, const SpecSegment& reference);

struct ScriptedResult
{
    bool pass = false;
    double outlierFraction = 0.0; // active cells deviating by more than 3 dB
    double meanCellDb = 0.0;      // mean |delta| over active cells
    double levelDeltaDb = 0.0;    // overall level difference
    double maxCellDb = 0.0;
    std::string report;
};

/** Scripted traces: band energy per third-octave per 25 ms frame within 3 dB per cell with at most
    5 % outliers, mean deviation at most 1 dB, overall level within 0.5 dB. */
ScriptedResult compareScripted (const Spec& reference, const Spec& measured);

/** The lag (in samples, within +-maxLag) at which `measured` best lines up with `reference`: positive
    when `measured` is delayed. Normalised cross-correlation over the overlap; 0 for empty input. */
int estimateDelay (const float* reference, const float* measured, std::size_t n, int maxLag = tolerance::maxDelaySamples);
} // namespace vs::bside::trace
