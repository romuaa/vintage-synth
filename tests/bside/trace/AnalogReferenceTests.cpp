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

TEST_CASE ("analog: traces that need the renderer's two buses (S3b)", "[trace][reference][chip][analog]")
{
    // Until Chip::render has a filter bus and a direct bus (8.19.2), a trace whose sounding voices are routed
    // differently cannot be rendered through our chain; it is reported skipped here, never passed. The rest of
    // a_modes_* is compared above. a_dac_*: its triangle segment is S3b's (8.19.2); its envelope and saw segments
    // are compared by RenderReferenceTests.cpp through stand-ins.
    for (const char* name : { "a_routing_a", "a_routing_b" })
    {
        const auto& r = renderedTrace (kDir, name);
        if (! r.ok)
            continue;
        std::size_t amb = 0;
        for (const bool a : r.segmentAmbiguous)
            amb += a ? 1u : 0u;
        std::printf ("[analog] %s: %zu of %zu segments sound voices on both paths\n", name, amb, r.segmentAmbiguous.size());
    }
    SKIP ("a_routing_a, a_routing_b (routing bits and oscillator 3 off with voices on both paths) need the two buses of S3b");
}
