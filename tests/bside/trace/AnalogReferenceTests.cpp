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
        // named strings, not temporaries: GCC 13's -Wdangling-reference would flag a temporary argument
        const std::string modesName = "a_modes_" + sm, bypassName = "a_modes_bypass_" + sm;
        const auto& r = renderedTrace (kDir, modesName);
        const auto& b = renderedTrace (kDir, bypassName);
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
    // Through the real chain (AnalogChain.h renderChain: ChipVoice, the renderer's two buses), every routing of the
    // three voices and oscillator 3 off, each routing write at its cycle: the scripted tolerance over every frame.
    // Model B is gated; Model A is reported, NOT inside the tolerance (decisions "B-Side S3a" round 2: its
    // filter's dependence on the level and the routed DC is not modelled; cutoff 300 here).
    for (const char* name : { "a_routing_a", "a_routing_b" })
    {
        const std::string traceName (name);
        const auto& r = renderedTrace (kDir, traceName);
        if (! r.ok)
            SKIP (r.why);
        const auto res = tr::compareScripted (r.reference, r.ours);
        const bool gate = std::string (name).back() == 'b';
        std::printf ("[analog] %s: outliers %.2f %% (allowed 5), mean %.2f dB (allowed 1), level %.2f dB (allowed 0.5), largest cell %.1f dB%s\n", name,
                     100.0 * res.outlierFraction, res.meanCellDb, res.levelDeltaDb, res.maxCellDb, gate ? "" : " (reported, outside)");
        INFO (res.report);
        if (gate)
            CHECK (res.pass);
        // Model A's evidence (8.19.4: a trace that fails only by the filter's known dynamics, 8.19.3): the frames where
        // no voice is routed to the filter against those where one is (the routing at every cycle of the frame's
        // analysis window), each with the scripted tolerance; reported
        std::vector<bool> direct (static_cast<std::size_t> (r.reference.frames), true);
        {
            std::uint8_t routing = 0;
            std::size_t next = 0;
            for (int f = 0; f < r.reference.frames; ++f)
            {
                const long centre = static_cast<long> (f) * tr::kSpecHop + tr::kSpecHop / 2;
                const auto from = static_cast<std::uint64_t> (std::max (0L, centre - tr::kFeatureFftSize / 2)) * r.trace.clockHz / 48000u;
                const auto to = static_cast<std::uint64_t> (centre + tr::kFeatureFftSize / 2) * r.trace.clockHz / 48000u;
                while (next < r.trace.writes.size() && r.trace.writes[next].cycle < from)
                {
                    if (r.trace.writes[next].reg == 0x17)
                        routing = static_cast<std::uint8_t> (r.trace.writes[next].value & 7);
                    ++next;
                }
                bool none = routing == 0;
                for (std::size_t k = next; k < r.trace.writes.size() && r.trace.writes[k].cycle <= to; ++k)
                    if (r.trace.writes[k].reg == 0x17 && (r.trace.writes[k].value & 7) != 0)
                        none = false;
                direct[static_cast<std::size_t> (f)] = none;
            }
        }
        std::vector<bool> routed (direct.size());
        int nDirect = 0;
        for (std::size_t f = 0; f < direct.size(); ++f)
        {
            routed[f] = ! direct[f];
            nDirect += direct[f] ? 1 : 0;
        }
        const auto rd = tr::compareScripted (keepFrames (r.reference, direct), keepFrames (r.ours, direct));
        const auto rf = tr::compareScripted (keepFrames (r.reference, routed), keepFrames (r.ours, routed));
        std::printf ("[analog] %s, the %d frames with no voice routed to the filter: outliers %.2f %%, mean %.2f dB, level %.2f dB%s; the %d with one: "
                     "outliers %.2f %%, mean %.2f dB, level %.2f dB%s\n",
                     name, nDirect, 100.0 * rd.outlierFraction, rd.meanCellDb, rd.levelDeltaDb, rd.pass ? " (inside)" : " (OUTSIDE)",
                     r.reference.frames - nDirect, 100.0 * rf.outlierFraction, rf.meanCellDb, rf.levelDeltaDb, rf.pass ? " (inside)" : " (OUTSIDE)");
    }
}

TEST_CASE ("analog: the triangle segment of a_dac_* through the chain: harmonics 2..8 within 3 dB (8.19.4)", "[trace][reference][chip][analog]")
{
    // a_dac_* s18: a 110 Hz triangle at full level on the direct path (RenderReferenceTests.cpp leaves it out: its
    // second harmonic, -34 dB on Model A and -40 dB on Model B, comes from after the converters). Through the real
    // chain (Model B's mixer curve, Model A's voice bend and mixer) its harmonics 2..8 above -60 dB re the fundamental
    // within the distortion tolerance of 8.9.4 (+-3 dB). The saws s16 and s17 are printed through the chain as well.
    for (const char* name : { "a_dac_a", "a_dac_b" })
    {
        const std::string traceName (name);
        const auto& r = renderedTrace (kDir, traceName);
        if (! r.ok)
            SKIP (r.why);
        REQUIRE (r.reference.segments.size() >= 20);
        const double f0s[] { 110.0, 1000.0, 110.0 };
        for (int k = 0; k < 3; ++k)
        {
            const auto& rs = r.reference.segments[static_cast<std::size_t> (16 + k)];
            const auto& os = r.ours.segments[static_cast<std::size_t> (16 + k)];
            const double f0 = static_cast<double> (r.trace.clockHz) * std::round (f0s[k] * 16777216.0 / 985248.0) / 16777216.0;
            double worst = 0.0;
            std::string line;
            for (int h = 2; h <= 8; ++h)
            {
                const double dr = tr::toneLevelDb (rs, h * f0) - tr::toneLevelDb (rs, f0);
                const double dm = tr::toneLevelDb (os, h * f0) - tr::toneLevelDb (os, f0);
                char buf[48];
                std::snprintf (buf, sizeof buf, " %6.1f/%6.1f", dm, dr);
                line += buf;
                if (dr > -60.0)
                    worst = std::max (worst, std::abs (dr - dm));
            }
            const bool triangle = k == 2;
            std::printf ("[analog] %s %s harmonics 2..8 (ours/reference dB):%s; worst above -60 dB %.2f dB%s\n", name, rs.name.c_str(), line.c_str(), worst,
                         triangle ? " (allowed 3)" : " (reported)");
            if (triangle)
            {
                INFO (name << " triangle: worst harmonic " << worst << " dB");
                CHECK (worst <= tr::tolerance::distortionDb);
            }
        }
    }
}

namespace
{
/** Corners (+-5 %) and peaks (+-1 dB) of every segment of a filtered/bypass pair against the reference. */
void cornersAndPeaks (const char* filtered, const char* bypass, bool gate)
{
    const std::string filteredName (filtered), bypassName (bypass);
    const auto& f = renderedTrace (kDir, filteredName);
    const auto& b = renderedTrace (kDir, bypassName);
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
        {
            CHECK (vc.pass);
            CHECK (vp.pass);
        }
    }
    std::printf ("[analog] %s: %d segments, worst corner %.2f %% (allowed 5), worst peak %.2f dB (allowed 1)%s\n", filtered, compared, 100.0 * worstC, worstP,
                 gate ? "" : " (reported)");
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

TEST_CASE ("analog: Model A held-tone distortion and two-tone products at held-out drives (a_s3a_dist_a)", "[trace][reference][chip][analog]")
{
    // Harmonics 2..5 against the fundamental, and for two tones (160 and 211 Hz) the products 2 f1, 2 f2, f2 - f1,
    // f1 + f2 against f1, from the segment spectra (toneLevelDb), +-3 dB where the reference's line lies above -60 dB.
    // Gated: the held tones through the low-pass at 2047 and 1024 and on the direct path (s00..s07, s10..s12).
    // Reported, outside: the band-pass at 1500 (s08, s09) and the two-tone products (s13..s16: up to 4.5 dB).
    const std::string distName ("a_s3a_dist_a");
    const auto& d = renderedTrace (kDir, distName);
    if (! d.ok)
        SKIP (d.why);
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
                CHECK (std::abs (o - r) <= tr::tolerance::distortionDb);
                worst = std::max (worst, std::abs (o - r));
            }
        }
        std::printf ("[analog] a_s3a_dist_a %s %s (ours/reference dB):%s%s\n", rs.name.c_str(), twoTone ? "2f1 2f2 f2-f1 f1+f2" : "h2 h3 h4 h5", line.c_str(),
                     twoTone || i == 8 || i == 9 ? " (reported)" : "");
    }
    std::printf ("[analog] a_s3a_dist_a: worst gated line %.2f dB (allowed 3)\n", worst);
}

TEST_CASE ("analog: the modes at cutoff 200 and 1500 within the scripted-trace tolerance (a_s3a_modes_a)", "[trace][reference][chip][analog]")
{
    // The bypass twin is gated; the filtered trace is reported, outside (finding 5 of the review stays open).
    for (const char* name : { "a_s3a_modes_a", "a_s3a_modes_bypass_a" })
    {
        const std::string traceName (name);
        const auto& r = renderedTrace (kDir, traceName);
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
    for (const char m : { 'a', 'b' })
    {
        const std::string stepsName = std::string ("a_s3a_steps_") + m;
        const auto& r = renderedTrace (kDir, stepsName);
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
            std::printf ("[analog] a_s3a_steps_%c write at cycle %llu (frame %zu): ours %6.1f dBFS, reference %6.1f dBFS (%+.1f dB)%s\n", m,
                         static_cast<unsigned long long> (w.cycle), fr, lo, lr, lo - lr, m == 'a' ? (pass ? "" : "  OUTSIDE") : " (reported)");
            if (m == 'a' && volumeWrite)
            {
                CHECK (pass);
                worst = std::max (worst, std::abs (lo - lr));
            }
        }
        if (m == 'a')
            std::printf ("[analog] a_s3a_steps_a: worst volume step %.2f dB (allowed 1)\n", worst);
    }
}
