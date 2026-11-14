#include "bside/trace/Compare.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vs::bside::trace
{
namespace
{
std::string fixed (double v, int digits = 1)
{
    char buf[48];
    std::snprintf (buf, sizeof buf, "%.*f", digits, v);
    return buf;
}

Verdict make (bool pass, double dev, double allowed, std::string what)
{
    Verdict v;
    v.pass = pass;
    v.deviation = dev;
    v.allowed = allowed;
    v.what = std::move (what);
    return v;
}
} // namespace

Verdict checkFilterCorner (double measuredHz, double referenceHz)
{
    if (! (referenceHz > 0.0) || ! std::isfinite (measuredHz) || ! std::isfinite (referenceHz))
        return make (false, 0.0, tolerance::filterCornerRelative, "filter corner: reference or measurement is not a positive finite number");
    const double dev = std::abs (measuredHz / referenceHz - 1.0);
    return make (dev <= tolerance::filterCornerRelative, dev, tolerance::filterCornerRelative,
                 "filter corner: " + fixed (measuredHz, 1) + " Hz against " + fixed (referenceHz, 1) + " Hz, off by " + fixed (100.0 * dev, 2)
                     + " % (allowed " + fixed (100.0 * tolerance::filterCornerRelative, 1) + " %)");
}

Verdict checkWithinDb (const char* quantity, double measuredDb, double referenceDb, double allowedDb)
{
    if (! std::isfinite (measuredDb) || ! std::isfinite (referenceDb))
        return make (false, 0.0, allowedDb, std::string (quantity) + ": not a finite number");
    const double dev = std::abs (measuredDb - referenceDb);
    return make (dev <= allowedDb, dev, allowedDb,
                 std::string (quantity) + ": " + fixed (measuredDb, 2) + " dB against " + fixed (referenceDb, 2) + " dB, off by " + fixed (dev, 2)
                     + " dB (allowed " + fixed (allowedDb, 1) + " dB)");
}

Verdict checkResonancePeak (double measuredDb, double referenceDb)
{
    return checkWithinDb ("resonance peak", measuredDb, referenceDb, tolerance::resonancePeakDb);
}

Verdict checkVoiceDcStep (double measuredDb, double referenceDb)
{
    return checkWithinDb ("voice DC step", measuredDb, referenceDb, tolerance::voiceDcDb);
}

Verdict checkDistortion (const double measuredDb[4], const double referenceDb[4])
{
    Verdict worst;
    for (int h = 0; h < 4; ++h)
    {
        auto v = checkWithinDb (("harmonic " + std::to_string (h + 2)).c_str(), measuredDb[h], referenceDb[h], tolerance::distortionDb);
        if (h == 0 || (! v.pass && worst.pass) || (v.pass == worst.pass && v.deviation > worst.deviation))
            worst = v;
    }
    return worst;
}

std::vector<double> thirdOctaveFromTwelfth (const std::array<std::int16_t, kSpecBins>& spectrum)
{
    std::vector<double> out;
    out.reserve (kSpecBins / 4);
    for (int g = 0; g < kSpecBins / 4; ++g)
    {
        double power = 0.0;
        for (int k = 0; k < 4; ++k)
            power += std::pow (10.0, spectrum[static_cast<std::size_t> (g * 4 + k)] / 100.0); // 0.1 dB -> power
        out.push_back (10.0 * std::log10 (power / 4.0));
    }
    return out;
}

Verdict checkNoiseSpectrum (const SpecSegment& measured, const SpecSegment& reference)
{
    const auto a = thirdOctaveFromTwelfth (measured.spectrum);
    const auto b = thirdOctaveFromTwelfth (reference.spectrum);
    double worst = 0.0;
    int worstBand = -1, compared = 0;
    for (std::size_t g = 0; g < a.size(); ++g)
    {
        const double centre = 20.0 * std::pow (2.0, (4.0 * static_cast<double> (g) + 1.5) / 12.0);
        if (centre < 50.0 || centre > 16000.0)
            continue;
        ++compared;
        const double d = std::abs (a[g] - b[g]);
        if (d > worst)
        {
            worst = d;
            worstBand = static_cast<int> (g);
        }
    }
    const bool pass = worst <= tolerance::noiseThirdOctaveDb;
    return make (pass, worst, tolerance::noiseThirdOctaveDb,
                 "noise spectrum: worst third-octave band off by " + fixed (worst, 2) + " dB over " + std::to_string (compared) + " bands"
                     + (worstBand >= 0 ? " (band " + std::to_string (worstBand) + ")" : std::string()) + " (allowed "
                     + fixed (tolerance::noiseThirdOctaveDb, 1) + " dB)");
}

ScriptedResult compareScripted (const Spec& ref, const Spec& cand)
{
    ScriptedResult r;
    if (ref.frames != cand.frames || ref.frames < 1)
    {
        r.report = "FAIL frame count differs: reference " + std::to_string (ref.frames) + ", measured " + std::to_string (cand.frames) + "\n";
        return r;
    }
    long active = 0, over = 0;
    double sum = 0.0, maxCell = 0.0;
    const double gate = tolerance::scriptedBandGateDb;
    for (int f = 0; f < ref.frames; ++f)
        for (int b = 0; b < kSpecBands; ++b)
        {
            const double a = ref.band (f, b) / 10.0, d = cand.band (f, b) / 10.0;
            if (std::max (a, d) <= gate)
                continue;
            const double delta = std::abs (std::max (a, gate) - std::max (d, gate));
            ++active;
            sum += delta;
            over += delta > tolerance::scriptedCellDb ? 1 : 0;
            maxCell = std::max (maxCell, delta);
        }
    r.outlierFraction = active > 0 ? static_cast<double> (over) / static_cast<double> (active) : 0.0;
    r.meanCellDb = active > 0 ? sum / static_cast<double> (active) : 0.0;
    r.maxCellDb = maxCell;

    const auto overall = [] (const Spec& s) {
        double p = 0.0;
        for (const auto l : s.levelTenthDb)
            p += std::pow (10.0, l / 100.0);
        return 10.0 * std::log10 (p / static_cast<double> (s.frames));
    };
    r.levelDeltaDb = std::abs (overall (ref) - overall (cand));

    r.pass = true;
    const auto check = [&] (bool ok, const std::string& what) {
        if (! ok)
        {
            r.pass = false;
            r.report += "FAIL " + what + "\n";
        }
    };
    check (r.outlierFraction <= tolerance::scriptedOutlierFraction,
           "band cells off by more than " + fixed (tolerance::scriptedCellDb, 1) + " dB: " + fixed (100.0 * r.outlierFraction, 2) + " % (allowed "
               + fixed (100.0 * tolerance::scriptedOutlierFraction, 1) + " %)");
    check (r.meanCellDb <= tolerance::scriptedMeanCellDb,
           "mean band deviation " + fixed (r.meanCellDb, 2) + " dB (allowed " + fixed (tolerance::scriptedMeanCellDb, 1) + " dB)");
    check (r.levelDeltaDb <= tolerance::scriptedLevelDb,
           "overall level differs by " + fixed (r.levelDeltaDb, 2) + " dB (allowed " + fixed (tolerance::scriptedLevelDb, 1) + " dB)");
    return r;
}

int estimateDelay (const float* ref, const float* meas, std::size_t n, int maxLag)
{
    if (ref == nullptr || meas == nullptr || n == 0 || maxLag < 0)
        return 0;
    int best = 0;
    double bestScore = -2.0;
    const std::size_t minOverlap = std::max<std::size_t> (1, n / 2); // tiny overlaps correlate perfectly by chance
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        const std::size_t shift = static_cast<std::size_t> (lag < 0 ? -lag : lag);
        if (shift >= n || n - shift < minOverlap)
            continue;
        double xy = 0.0, xx = 0.0, yy = 0.0;
        for (std::size_t i = 0; i + shift < n; ++i)
        {
            const double x = lag >= 0 ? ref[i] : ref[i + shift];
            const double y = lag >= 0 ? meas[i + shift] : meas[i];
            xy += x * y;
            xx += x * x;
            yy += y * y;
        }
        const double score = (xx > 0.0 && yy > 0.0) ? xy / std::sqrt (xx * yy) : 0.0;
        // Best score wins; on a tie (silence, a constant) the smallest lag.
        if (score > bestScore + 1e-12 || (score > bestScore - 1e-12 && std::abs (lag) < std::abs (best)))
        {
            bestScore = std::max (score, bestScore);
            best = lag;
        }
    }
    return best;
}
} // namespace vs::bside::trace
