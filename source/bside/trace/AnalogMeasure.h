#pragma once

// The analog measurements of docs/architecture.md 8.9.4, computed from .bsspec segment spectra
// (SpecAnalysis.h). One estimator for both sides: the reference numbers (adapter tools) and our
// replay tests call the same functions on spectra measured the same way. JUCE-free.
//
// A filter response is the difference, per 1/12-octave bin, between a segment of a trace with the
// voice routed through the filter and the same segment of its bypass twin (same writes, voice not
// routed), so the excitation cancels. Rules:
// - Bins where the bypass is at or below -100 dB carry no information (NaN).
// - Smoothing: running mean over 5 bins (2 either side, NaN skipped, fewer at the ends).
// - Plateau: median of the unsmoothed response over the bins from 0 up to one octave below the corner
//   (at least bins 0..5, 20..26.7 Hz); found by iterating with the corner estimate until the
//   corner's bin no longer changes (at most 8 rounds; not converged = not found, `converged` false).
// - Corner (-3 dB): the first bin k where the smoothed response and the next bin's are both below
//   plateau - 3 dB; then a least-squares parabola through the unsmoothed response over bins
//   k-4 .. k+4 (log frequency) and its falling crossing of plateau - 3 dB nearest k. Not found when
//   there is none, or when it lies below bin 12 (40 Hz: a corner needs an octave of plateau).
// - Resonance peak: the largest smoothed value from one octave below the corner up to half an octave
//   above it, minus the plateau (dB). (Searching from bin 0 would report plateau noise as the peak
//   at low resonance.)
// - Tone level: the bin nearest the tone's frequency (round (12 log2 (f / 20 Hz))).

#include "bside/trace/ReferenceFormat.h"

#include <array>

namespace vs::bside::trace
{
using Response = std::array<double, kSpecBins>;

/** filtered - bypass in dB per bin; NaN where the bypass is at or below -100 dB. */
Response filterResponse (const SpecSegment& filtered, const SpecSegment& bypass);

/** 5-bin running mean (NaN skipped). */
Response smoothResponse (const Response& r);

struct Corner
{
    bool found = false;
    bool converged = true; // false: the plateau/corner iteration did not settle (found is false then)
    double hz = 0.0;
    double plateauDb = 0.0;
    int rounds = 0;
};

/** The -3 dB point of a low-pass response (rules above). */
Corner lowpassCorner (const Response& r);

struct Peak
{
    bool found = false;
    double db = 0.0;      // peak above the plateau
    double hz = 0.0;      // centre of the peak bin
    double plateauDb = 0.0;
};

/** The resonance peak of a low-pass response (rules above); needs the corner. */
Peak resonancePeak (const Response& r);

/** Centre frequency of bin k: 20 Hz * 2^(k/12). */
double binHz (double k);

/** Level in dB of the bin nearest `hz` (0.1 dB resolution of the spec); -120 outside the bins. */
double toneLevelDb (const SpecSegment& s, double hz);
} // namespace vs::bside::trace
