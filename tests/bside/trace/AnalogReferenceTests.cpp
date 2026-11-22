// S3a (docs/architecture.md 8.19.1): the filter modes against the committed reference data, and the traces that
// wait for the renderer's two buses (S3b). Our chain: AnalogChain.h; the corners, peaks, distortion and voice
// DC are in ReferenceDataTests.cpp ("analog tolerances against our chip").

#include "AnalogGates.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
namespace tr = vs::bside::trace;
using namespace vs::bside::test;

namespace
{
const fs::path kDir { BS_REFERENCE_DIR };
// Whether the signal-dependent form's figures on the distortion and step traces are assertions (they are when met:
// decisions "B-Side S3a round 3").
constexpr bool kGateSignalDependentDistortion = true;
constexpr bool kGateSignalDependentSteps = true;
}

TEST_CASE ("analog: the filter modes and their sums within the scripted-trace tolerance (a_modes_*)", "[trace][reference][chip][analog]")
{
    // 8.9.4 "scripted traces": band energy per third octave per 25 ms frame within 3 dB per cell with at most 5 %
    // outliers, mean deviation at most 1 dB, overall level within 0.5 dB (Compare.h compareScripted), over every
    // frame our single-sum chain can render: the trace's last segments sound oscillator 1 on the direct path beside
    // the filtered noise, which needs the two buses of S3b; those frames are left out and counted. The mode
    // responses against the bypass twin are printed per segment as well (filterResponse at 1, 3 and 10 kHz).
    for (const char m : { 'a', 'b' })
    {
        const std::string sm (1, m);
        const auto& r = renderedTrace (kDir, "a_modes_" + sm);
        const auto& b = renderedTrace (kDir, "a_modes_bypass_" + sm);
        if (! r.ok || ! b.ok)
            SKIP (r.why << " " << b.why);
        INFO ("model " << sm);
        for (const auto* t : { &r, &b })
        {
            const auto keep = clearFrames (*t);
            std::size_t kept = 0;
            for (const bool k : keep)
                kept += k ? 1u : 0u;
            const auto res = tr::compareScripted (keepFrames (t->reference, keep), keepFrames (t->ours, keep));
            std::printf ("[analog] model %s %s: %zu of %d frames compared (%d need two buses): outliers %.2f %% (allowed 5), mean %.2f dB (allowed 1), "
                         "level %.2f dB (allowed 0.5), largest cell %.1f dB\n",
                         sm.c_str(), t == &r ? "a_modes" : "a_modes_bypass", kept, t->reference.frames, t->reference.frames - static_cast<int> (kept),
                         100.0 * res.outlierFraction, res.meanCellDb, res.levelDeltaDb, res.maxCellDb);
            INFO (res.report);
            CHECK (res.pass);
            CHECK (kept > static_cast<std::size_t> (t->reference.frames) / 2);
        }
        for (std::size_t i = 0; i < r.reference.segments.size(); ++i)
        {
            if (r.segmentAmbiguous[i] || b.segmentAmbiguous[i])
            {
                std::printf ("[analog] model %s a_modes %s: needs two buses (S3b), not compared\n", sm.c_str(), r.reference.segments[i].name.c_str());
                continue;
            }
            const auto ref = tr::filterResponse (r.reference.segments[i], b.reference.segments[i]);
            const auto ours = tr::filterResponse (r.ours.segments[i], b.ours.segments[i]);
            auto at = [] (const tr::Response& x, double hz) { return x[static_cast<std::size_t> (std::lround (12.0 * std::log2 (hz / 20.0)))]; };
            std::printf ("[analog] model %s a_modes %s response at 1 / 3 / 10 kHz: ours %6.1f %6.1f %6.1f, reference %6.1f %6.1f %6.1f dB\n", sm.c_str(),
                         r.reference.segments[i].name.c_str(), at (ours, 1000.0), at (ours, 3000.0), at (ours, 10000.0), at (ref, 1000.0),
                         at (ref, 3000.0), at (ref, 10000.0));
        }
    }
}

TEST_CASE ("analog: routing and oscillator 3 off within the scripted-trace tolerance (a_routing_*)", "[trace][reference][chip][analog]")
{
    // The per-voice harness (AnalogChain.h) renders each voice alone (no sync or ring in these traces), so the two
    // buses are exact: every routing of the three voices and oscillator 3 off, the scripted tolerance over every
    // frame.
    // Model B is gated; Model A is reported, NOT inside the tolerance (decisions "B-Side S3a" round 2: its
    // filter's dependence on the level and the routed DC is not modelled; cutoff 300 here).
    for (const char* name : { "a_routing_a", "a_routing_b" })
    {
        const auto& r = renderedTrace (kDir, name);
        if (! r.ok)
            SKIP (r.why);
        REQUIRE (r.chain.ambiguousSamples == 0);
        const auto res = tr::compareScripted (r.reference, r.ours);
        const bool gate = std::string (name).back() == 'b';
        std::printf ("[analog] %s: outliers %.2f %% (allowed 5), mean %.2f dB (allowed 1), level %.2f dB (allowed 0.5), largest cell %.1f dB%s\n", name,
                     100.0 * res.outlierFraction, res.meanCellDb, res.levelDeltaDb, res.maxCellDb, gate ? "" : " (reported, outside)");
        INFO (res.report);
        if (gate)
            CHECK (res.pass);
    }
}

namespace
{
/** Corners (+-5 %) and peaks (+-1 dB) of every segment of a filtered/bypass pair against the reference. `gate`:
    both are assertions; `gatePeaks`: the peaks are. `signalDependent`: Model A's signal-dependent dynamics
    (Filter::processSignalDependent) instead of Filter::process; its lines carry the tag "signal-dependent". */
void cornersAndPeaks (const char* filtered, const char* bypass, bool gate, bool signalDependent = false, bool gatePeaks = false)
{
    const auto& f = renderedTrace (kDir, filtered, signalDependent);
    const auto& b = renderedTrace (kDir, bypass, signalDependent);
    const std::string tagged = std::string (signalDependent ? "signal-dependent " : "") + filtered;
    filtered = tagged.c_str();
    if (! f.ok || ! b.ok)
        SKIP (f.why << " " << b.why);
    REQUIRE (f.reference.segments.size() == f.ours.segments.size());
    double worstC = 0.0, worstP = 0.0;
    int compared = 0;
    for (std::size_t i = 0; i < f.reference.segments.size(); ++i)
    {
        const auto ref = tr::filterResponse (f.reference.segments[i], b.reference.segments[i]);
        const auto ours = tr::filterResponse (f.ours.segments[i], b.ours.segments[i]);
        const auto cr = tr::lowpassCorner (ref), co = tr::lowpassCorner (ours);
        const auto pr = tr::resonancePeak (ref), po = tr::resonancePeak (ours);
        INFO (filtered << " " << f.reference.segments[i].name);
        if (! cr.found)
        {
            std::printf ("[analog] %s %s: the reference has no corner the estimator resolves; not compared\n", filtered, f.reference.segments[i].name.c_str());
            continue;
        }
        if (! co.found || ! pr.found || ! po.found)
        {
            std::printf ("[analog] %s %s: ours has no corner or peak the estimator resolves (reference %.1f Hz)  OUTSIDE\n", filtered,
                         f.reference.segments[i].name.c_str(), cr.hz);
            if (gate)
                FAIL_CHECK ("no corner or peak in ours");
            continue;
        }
        const auto vc = tr::checkFilterCorner (co.hz, cr.hz);
        const auto vp = tr::checkResonancePeak (po.db, pr.db);
        std::printf ("[analog] %s %s: corner ours %8.1f Hz, reference %8.1f Hz (%+5.1f %%); peak ours %+6.2f dB, reference %+6.2f dB (%+5.2f)%s\n", filtered,
                     f.reference.segments[i].name.c_str(), co.hz, cr.hz, 100.0 * (co.hz / cr.hz - 1.0), po.db, pr.db, po.db - pr.db,
                     vc.pass && vp.pass ? "" : "  OUTSIDE");
        ++compared;
        worstC = std::max (worstC, vc.deviation);
        worstP = std::max (worstP, vp.deviation);
        if (gate)
            CHECK (vc.pass);
        if (gate || gatePeaks)
            CHECK (vp.pass);
    }
    std::printf ("[analog] %s: %d segments, worst corner %.2f %% (allowed 5), worst peak %.2f dB (allowed 1)%s\n", filtered, compared, 100.0 * worstC, worstP,
                 gate ? "" : (gatePeaks ? " (corners reported, peaks gated)" : " (reported)"));
    CHECK (compared > 0);
}
} // namespace

TEST_CASE ("analog: Model A corners and peaks across cutoff, resonance, level and voices (a_s3a_*)", "[trace][reference][chip][analog]")
{
    // Round 2's corner and peak gates (8.19.1; our own traces, never used for fitting): resonance 0..15 at cutoffs
    // 300 .. 1900 (a_s3a_res), sustain 3 and 15 (a_s3a_level), three voices routed together (a_s3a_voices), corners
    // around the cutoff curve's steep part and FC 380 repeated (a_s3a_cutoff). REPORTED, NOT GATED: Model A's filter
    // does not follow the level, the routed DC or the resonance's cutoff-dependent shift of the reference (findings 1,
    // 2, 4 of the S3a review stay open; decisions "B-Side S3a" round 2). The figures are printed per segment.
    cornersAndPeaks ("a_s3a_res_a", "a_s3a_res_bypass_a", false);
    cornersAndPeaks ("a_s3a_level_a", "a_s3a_level_bypass_a", false);
    cornersAndPeaks ("a_s3a_voices_a", "a_s3a_voices_bypass_a", false);
    cornersAndPeaks ("a_s3a_cutoff_a", "a_s3a_cutoff_bypass_a", false);
}

TEST_CASE ("analog: Model A's signal-dependent dynamics against the same reference data (reported; gated where met)", "[trace][reference][chip][analog]")
{
    // Filter::processSignalDependent (the caller's choice; decisions "B-Side S3a round 3"): the same traces and
    // tolerances as the default form above and in ReferenceDataTests.cpp, printed with the tag "signal-dependent".
    // GATED, because met: the resonance peaks of a_filter_a and a_s3a_cutoff_a (+-1 dB), the modes at cutoff 200 and
    // 1500 (a_s3a_modes_a, the scripted tolerance), and in their own test cases the held-tone distortion (a_s3a_dist_a)
    // and the volume steps (a_s3a_steps_a). REPORTED, NOT GATED: every corner (not all within 5 % on any of the five
    // traces), the peaks of a_s3a_res_a, a_s3a_level_a and a_s3a_voices_a, a_modes_a and a_routing_a.
    cornersAndPeaks ("a_filter_a", "a_filter_bypass_a", false, true, true);
    cornersAndPeaks ("a_s3a_res_a", "a_s3a_res_bypass_a", false, true, false);
    cornersAndPeaks ("a_s3a_level_a", "a_s3a_level_bypass_a", false, true, false);
    cornersAndPeaks ("a_s3a_voices_a", "a_s3a_voices_bypass_a", false, true, false);
    cornersAndPeaks ("a_s3a_cutoff_a", "a_s3a_cutoff_bypass_a", false, true, true);
    for (const char* name : { "a_modes_a", "a_s3a_modes_a", "a_routing_a" })
    {
        const auto& r = renderedTrace (kDir, name, true);
        if (! r.ok)
            SKIP (r.why);
        const auto res = tr::compareScripted (r.reference, r.ours);
        const bool gate = std::string (name) == "a_s3a_modes_a";
        std::printf ("[analog] signal-dependent %s: outliers %.2f %% (allowed 5), mean %.2f dB (allowed 1), level %.2f dB (allowed 0.5), largest cell %.1f dB%s\n",
                     name, 100.0 * res.outlierFraction, res.meanCellDb, res.levelDeltaDb, res.maxCellDb, gate ? "" : (res.pass ? " (reported)" : " (reported, outside)"));
        INFO (res.report);
        if (gate)
            CHECK (res.pass);
    }
}

TEST_CASE ("analog: Model A held-tone distortion and two-tone products at held-out drives (a_s3a_dist_a)", "[trace][reference][chip][analog]")
{
    // Harmonics 2..5 against the fundamental, and for two tones (160 and 211 Hz) the products 2 f1, 2 f2, f2 - f1,
    // f1 + f2 against f1, from the segment spectra (toneLevelDb), +-3 dB where the reference's line lies above -60 dB.
    // Gated: the held tones through the low-pass at 2047 and 1024 and on the direct path (s00..s07, s10..s12).
    // Reported, outside: the band-pass at 1500 (s08, s09) and the two-tone products (s13..s16: up to 4.5 dB).
    // Both forms of Model A's filter: Filter::process, then Filter::processSignalDependent (tag "signal-dependent").
    auto run = [&] (bool sd) {
        const auto& d = renderedTrace (kDir, "a_s3a_dist_a", sd);
        const char* tag = sd ? "signal-dependent " : "";
        if (! d.ok)
            SKIP (d.why);
        REQUIRE (d.chain.ambiguousSamples == 0);
        const double f1 = 985248.0 * std::round (160.0 * 16777216.0 / 985248.0) / 16777216.0;
        const double f2 = 985248.0 * std::round (211.0 * 16777216.0 / 985248.0) / 16777216.0;
        const std::size_t segs = d.reference.segments.size();
        REQUIRE (segs == 17);
        double worst = 0.0;
        for (std::size_t i = 0; i < segs; ++i)
        {
            const bool twoTone = i >= 13;
            const auto& rs = d.reference.segments[i];
            const auto& os = d.ours.segments[i];
            std::array<double, 4> fr {};
            if (twoTone)
                fr = { 2 * f1, 2 * f2, f2 - f1, f1 + f2 };
            else
                fr = { 2 * f1, 3 * f1, 4 * f1, 5 * f1 };
            std::string line;
            for (std::size_t k = 0; k < 4; ++k)
            {
                const double r = tr::toneLevelDb (rs, fr[k]) - tr::toneLevelDb (rs, f1);
                const double o = tr::toneLevelDb (os, fr[k]) - tr::toneLevelDb (os, f1);
                char buf[64];
                std::snprintf (buf, sizeof buf, " %6.1f/%6.1f", o, r);
                line += buf;
                const bool gated = ! twoTone && i != 8 && i != 9;
                if (r > -60.0 && gated)
                {
                    INFO (rs.name << " line " << k << ": ours " << o << " dB, reference " << r << " dB");
                    if (! sd || kGateSignalDependentDistortion)
                        CHECK (std::abs (o - r) <= tr::tolerance::distortionDb);
                    worst = std::max (worst, std::abs (o - r));
                }
            }
            std::printf ("[analog] %sa_s3a_dist_a %s %s (ours/reference dB):%s%s\n", tag, rs.name.c_str(), twoTone ? "2f1 2f2 f2-f1 f1+f2" : "h2 h3 h4 h5", line.c_str(),
                         twoTone || i == 8 || i == 9 ? " (reported)" : "");
        }
        std::printf ("[analog] %sa_s3a_dist_a: worst gated line %.2f dB (allowed 3)%s\n", tag, worst, sd && ! kGateSignalDependentDistortion ? " (reported)" : "");
    };
    run (false);
    run (true);
}

TEST_CASE ("analog: the modes at cutoff 200 and 1500 within the scripted-trace tolerance (a_s3a_modes_a)", "[trace][reference][chip][analog]")
{
    // The bypass twin is gated; the filtered trace is reported, outside (finding 5 of the review stays open).
    for (const char* name : { "a_s3a_modes_a", "a_s3a_modes_bypass_a" })
    {
        const auto& r = renderedTrace (kDir, name);
        if (! r.ok)
            SKIP (r.why);
        const auto res = tr::compareScripted (r.reference, r.ours);
        const bool gate = std::string (name).find ("bypass") != std::string::npos;
        std::printf ("[analog] %s: outliers %.2f %% (allowed 5), mean %.2f dB (allowed 1), level %.2f dB (allowed 0.5), largest cell %.1f dB%s\n", name,
                     100.0 * res.outlierFraction, res.meanCellDb, res.levelDeltaDb, res.maxCellDb, gate ? "" : " (reported)");
        INFO (res.report);
        if (gate)
            CHECK (res.pass);
    }
}

TEST_CASE ("analog: steps after volume, routing and resonance writes (a_s3a_steps_*)", "[trace][reference][chip][analog]")
{
    // The level of the first whole frame after each write against +-1 dB (the voice-DC tolerance of 8.9.4). Gated:
    // the volume writes (Model A). Reported: the routing and resonance writes on Model A (up to 2 dB outside,
    // finding 9 stays open) and every Model B step (they lie near the reference's board high-pass residual).
    for (const char m : { 'a', 'b', 'A' }) // 'A': Model A through Filter::processSignalDependent (tag "signal-dependent")
    {
        const bool sd = m == 'A';
        const char* tag = sd ? "signal-dependent " : "";
        const auto& r = renderedTrace (kDir, std::string ("a_s3a_steps_") + (sd ? 'a' : m), sd);
        if (! r.ok)
            SKIP (r.why);
        double worst = 0.0;
        std::uint64_t last = ~std::uint64_t { 0 };
        for (const auto& w : r.trace.writes)
        {
            if (w.cycle < 1000 || w.cycle == last)
                continue;
            last = w.cycle;
            const auto fr = static_cast<std::size_t> (tr::frameOfCycle (r.trace, w.cycle) + 1);
            const double lr = r.reference.levelTenthDb[fr] / 10.0, lo = r.ours.levelTenthDb[fr] / 10.0;
            if (lr < -60.0 && lo < -60.0)
                continue; // both quiet (a write that changes nothing audible)
            const bool pass = std::abs (lo - lr) <= tr::tolerance::voiceDcDb;
            const bool volumeWrite = w.reg == 0x18;
            std::printf ("[analog] %sa_s3a_steps_%c write at cycle %llu (frame %zu): ours %6.1f dBFS, reference %6.1f dBFS (%+.1f dB)%s\n", tag, sd ? 'a' : m,
                         static_cast<unsigned long long> (w.cycle), fr, lo, lr, lo - lr, m != 'b' ? (pass ? "" : "  OUTSIDE") : " (reported)");
            if (m != 'b' && volumeWrite)
            {
                if (! sd || kGateSignalDependentSteps)
                    CHECK (pass);
                worst = std::max (worst, std::abs (lo - lr));
            }
        }
        if (m != 'b')
            std::printf ("[analog] %sa_s3a_steps_a: worst volume step %.2f dB (allowed 1)\n", tag, worst);
    }
}
