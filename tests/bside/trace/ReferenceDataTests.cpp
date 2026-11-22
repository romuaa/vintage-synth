// The committed reference data of the trace replay (tests/bside/reference/, docs/bside-oracle-adapter.md):
// structure only, no chip needed. Every trace reads; its .bsref and .bsspec read, are canonical (the
// writer reproduces them byte for byte), belong to the trace (SHA-256 and checkpoint schedule), and the
// spec has the frame count and segment plan of SpecAnalysis.h; the MANIFEST lists every file with its
// SHA-256 and names the generator commits the files carry. The digital replay runs every trace through
// our chip (docs/architecture.md 8.9.4, 8.15, ChipReplay.h): exact at every checkpoint, with the
// shape exemption of 8.15.8 counted and reported per trace (`[replay]` lines on stdout); a `d_*` trace
// may have no exemption. The analog tolerances run our chain with the filter and output stage of S3a
// (AnalogGates.h); the rendered noise spectra (S2b) are in RenderReferenceTests.cpp, the modes and the traces
// that need two buses (S3b) in AnalogReferenceTests.cpp.

#include "AnalogGates.h"

#include "../../support/Env.h"
#include "bside/trace/ChipReplay.h"
#include "bside/trace/ReferenceFormat.h"
#include "bside/trace/Sha256.h"
#include "bside/trace/SpecAnalysis.h"
#include "bside/trace/TraceFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace vs::bside::trace;
namespace fs = std::filesystem;

namespace
{
const fs::path kDir { BS_REFERENCE_DIR };

std::string slurp (const fs::path& p)
{
    std::ifstream f (p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool isHex (const std::string& s, std::size_t n)
{
    return s.size() == n && std::all_of (s.begin(), s.end(), [] (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

struct Manifest
{
    std::map<std::string, std::string> keys;  // the five generator hashes (kKeys)
    std::map<std::string, std::string> files; // name -> sha256
    std::vector<std::string> problems;
};

Manifest readManifest()
{
    Manifest m;
    std::istringstream in (slurp (kDir / "MANIFEST"));
    std::string line;
    int n = 0;
    while (std::getline (in, line))
    {
        ++n;
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls (line);
        std::string a, b, c, extra;
        ls >> a >> b;
        if (a == "file")
        {
            ls >> c;
            if (! isHex (b, 64) || c.empty() || (ls >> extra))
                m.problems.push_back ("line " + std::to_string (n) + ": expected 'file <sha256> <name>'");
            else if (! m.files.emplace (c, b).second)
                m.problems.push_back ("line " + std::to_string (n) + ": " + c + " listed twice");
        }
        else if (a == "emulator-commit" || a == "adapter-commit" || a == "sources-commit" || a == "emulator-archive-sha256" || a == "settings-sha256")
        {
            const std::size_t len = (a == "emulator-archive-sha256" || a == "settings-sha256") ? 64 : 40;
            if (! isHex (b, len) || (ls >> extra))
                m.problems.push_back ("line " + std::to_string (n) + ": bad " + a);
            m.keys[a] = b;
        }
        else
            m.problems.push_back ("line " + std::to_string (n) + ": unknown entry '" + a + "'");
    }
    return m;
}

std::vector<fs::path> traces()
{
    std::vector<fs::path> out;
    if (fs::is_directory (kDir))
        for (const auto& e : fs::directory_iterator (kDir))
            if (e.path().extension() == ".bstrace")
                out.push_back (e.path());
    std::sort (out.begin(), out.end());
    return out;
}
} // namespace

TEST_CASE ("reference data: the MANIFEST lists every file with its SHA-256", "[trace][reference]")
{
    if (traces().empty())
        SKIP ("no reference data committed");
    const auto m = readManifest();
    for (const auto& p : m.problems)
        FAIL_CHECK (p);
    for (const char* k : { "emulator-commit", "emulator-archive-sha256", "adapter-commit", "sources-commit", "settings-sha256" })
        CHECK (m.keys.count (k) == 1);

    std::set<std::string> onDisk;
    for (const auto& e : fs::directory_iterator (kDir))
        if (e.is_regular_file() && e.path().filename() != "MANIFEST")
            onDisk.insert (e.path().filename().string());
    for (const auto& name : onDisk)
    {
        INFO (name);
        const auto it = m.files.find (name);
        REQUIRE (it != m.files.end());
        CHECK (sha256Hex (slurp (kDir / name)) == it->second);
    }
    for (const auto& [name, sha] : m.files)
    {
        INFO (name);
        CHECK (onDisk.count (name) == 1);
    }
}

TEST_CASE ("reference data: every trace has a canonical reference and spec that belong to it", "[trace][reference]")
{
    const auto all = traces();
    if (all.empty())
        SKIP ("no reference data committed");
    const auto m = readManifest();
    for (const auto& path : all)
    {
        const auto name = path.stem().string();
        INFO (name);
        const auto bytes = slurp (path);
        const auto tr = readTrace (bytes);
        INFO (tr.message());
        REQUIRE (tr.ok);

        // .bsref
        const auto refText = slurp (kDir / (name + ".bsref"));
        const auto rr = readReference (refText);
        INFO (rr.message());
        REQUIRE (rr.ok);
        std::string why;
        const std::string_view tb (bytes);
        CHECK (referenceMatchesTrace (rr.reference, tr.trace, &tb, &why));
        INFO (why);
        std::string canonical;
        REQUIRE (writeReference (rr.reference, canonical));
        CHECK (canonical == refText);
        CHECK (rr.reference.adapter == m.keys.at ("adapter-commit"));
        CHECK (rr.reference.emulator == m.keys.at ("emulator-commit"));

        // .bsspec
        const auto specText = slurp (kDir / (name + ".bsspec"));
        const auto sr = readSpec (specText);
        INFO (sr.message());
        REQUIRE (sr.ok);
        CHECK (sr.spec.traceSha256 == sha256Hex (bytes));
        CHECK (static_cast<std::uint64_t> (sr.spec.frames) == specFrameCount (tr.trace));
        const auto plan = segmentPlan (tr.trace);
        REQUIRE (sr.spec.segments.size() == plan.size());
        for (std::size_t i = 0; i < plan.size(); ++i)
        {
            CHECK (sr.spec.segments[i].name == plan[i].name);
            CHECK (sr.spec.segments[i].firstFrame == plan[i].firstFrame);
            CHECK (sr.spec.segments[i].endFrame == plan[i].endFrame);
        }
        REQUIRE (writeSpec (sr.spec, canonical));
        CHECK (canonical == specText);
    }
}

namespace
{
struct FamilyTotals
{
    std::size_t traces = 0, replays = 0, checkpoints = 0, exact = 0, exempt = 0, mismatched = 0;
};

std::string familyOf (const std::string& name)
{
    if (name.rfind ("d_", 0) == 0)
        return "d";
    if (name.rfind ("c_", 0) == 0)
        return "c";
    if (name.rfind ("a_", 0) == 0)
        return "a";
    return "other";
}
} // namespace

TEST_CASE ("reference data: digital replay against our chip, exact at every checkpoint (8.15.8 exemption)", "[trace][reference][chip]")
{
    const auto all = traces();
    if (all.empty())
        SKIP ("no reference data committed");
    std::map<std::string, FamilyTotals> families;
    for (const auto& path : all)
    {
        const auto name = path.stem().string();
        const auto family = familyOf (name);
        const auto bytes = slurp (path);
        const auto tr = readTrace (bytes);
        REQUIRE (tr.ok);
        const auto rr = readReference (slurp (kDir / (name + ".bsref")));
        REQUIRE (rr.ok);
        // The envelope traces set no waveform bit and hold for both models (docs/bside-oracle-adapter.md):
        // they are replayed as committed and once more with the chip configured as the other model.
        std::vector<vs::bside::chip::Model> models { chipModel (tr.trace.model) };
        if (name.rfind ("d_env_", 0) == 0)
            models.push_back (tr.trace.model == Model::A ? vs::bside::chip::Model::B : vs::bside::chip::Model::A);
        auto& f = families[family];
        ++f.traces;
        for (const auto m : models)
        {
            const auto r = replayChipExact (tr.trace, bytes, rr.reference, m);
            const char* modelName = m == vs::bside::chip::Model::A ? "A" : "B";
            std::printf ("[replay] %-24s model %s: %6zu checkpoints, %6zu exact, %6zu exempt (%zu of them equal anyway), %zu mismatched;"
                         " exempt fields (checkpoints exempt/differing): %s\n",
                         name.c_str(), modelName, r.checkpoints, r.exact, r.exempt, r.exemptButEqual, r.mismatched, exemptFieldsText (r).c_str());
            INFO (name << " as model " << modelName);
            REQUIRE (r.error.empty());
            std::string first;
            for (const auto& mm : r.first)
                first += "\n  checkpoint " + std::to_string (mm.checkpoint) + " (cycle " + std::to_string (mm.cycle) + "): " + kStateFieldNames[mm.field]
                         + " ours " + hexText (mm.ours) + ", reference " + hexText (mm.expected);
            for (const auto& w : r.writesBefore)
                first += "\n  write before: " + std::to_string (w.cycle) + " " + hexText (w.reg) + " " + hexText (w.value);
            INFO ("first differences:" << first);
            CHECK (r.checkpoints == rr.reference.points.size());
            CHECK (r.mismatched == 0);
            CHECK (r.fastPathDiffs == 0); // advanceCycles equals stepCycle in the full state at every checkpoint
            // The digital and analog traces use no combined-waveform table, and noise with only triangle or
            // only saw (c_9_*, c_a_*) uses none either (8.15.2 (c)): there nothing may be exempt, so those two
            // are exact tests of the noise write-back path.
            if (family == "d" || family == "a" || name.rfind ("c_9_", 0) == 0 || name.rfind ("c_a_", 0) == 0)
                CHECK (r.exempt == 0);
            ++f.replays;
            f.checkpoints += r.checkpoints;
            f.exact += r.exact;
            f.exempt += r.exempt;
            f.mismatched += r.mismatched;
        }
    }
    for (const auto& [family, f] : families)
        std::printf ("[replay] family %-5s %3zu traces, %3zu replays: %7zu checkpoints, %7zu exact, %6zu exempt, %zu mismatched\n", family.c_str(),
                     f.traces, f.replays, f.checkpoints, f.exact, f.exempt, f.mismatched);
    CHECK (families["d"].traces > 0);
    CHECK (families["c"].traces > 0);
}

TEST_CASE ("reference data: our fitted shapes against the S2a placeholder in the combined-waveform traces", "[trace][reference][chip]")
{
    // The exempt fields (8.15.8) of every c_* trace, replayed once with the placeholder shapes (before S2b)
    // and once with our fitted tables: per trace and field, how many exempt checkpoints differ from the
    // reference ([shapes-replay] lines). The fitted tables must differ less often over all c_* traces,
    // and (contract decision B on 8.15.8) exactly as often in every trace and field as the committed
    // budget, tests/bside/trace/exemption-budget.txt, says: the counts of the committed fit, a golden
    // regenerated with every refit. More fails (a regression), fewer fails too (the budget can be
    // tightened: rebless); a key that matches no exempt trace and field, or a duplicate key, fails.
    // BS_BLESS_EXEMPTION_BUDGET=<path> writes the file and then FAILS ("blessed; rerun without the
    // variable"), so a bless can never pass for a check. A field without a budget line has budget 0.
    // Exemptions are marked from the trace and the chip's events, so both runs exempt the same checkpoints.
    const auto all = traces();
    if (all.empty())
        SKIP ("no reference data committed");
    std::map<std::string, std::size_t> budget;
    {
        std::istringstream in (slurp (fs::path (BS_EXEMPTION_BUDGET)));
        std::string line;
        while (std::getline (in, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream ls (line);
            std::string trace, field;
            std::size_t n = 0;
            REQUIRE (static_cast<bool> (ls >> trace >> field >> n));
            INFO ("budget line: " << line);
            CHECK (budget.count (trace + " " + field) == 0); // a duplicate key
            budget[trace + " " + field] = n;
        }
    }
    const auto bless = testenv::get ("BS_BLESS_EXEMPTION_BUDGET");
    std::string blessed = "# Exempt checkpoints (8.15.8) that differ from the reference, per c_* trace and field, with our\n"
                          "# committed fit: the budget the replay may not exceed (contract decision B). Generated by bside_trace\n"
                          "# with BS_BLESS_EXEMPTION_BUDGET; regenerate with every refit and review like a golden.\n"
                          "# <trace> <field> <differing exempt checkpoints>\n";
    std::set<std::string> used;
    std::size_t before = 0, after = 0, exempt = 0;
    for (const auto& path : all)
    {
        const auto name = path.stem().string();
        if (familyOf (name) != "c")
            continue;
        const auto bytes = slurp (path);
        const auto tr = readTrace (bytes);
        const auto rr = readReference (slurp (kDir / (name + ".bsref")));
        REQUIRE (tr.ok);
        REQUIRE (rr.ok);
        const auto old = replayChipExact (tr.trace, bytes, rr.reference, std::nullopt, true, vs::bside::chip::ShapeSet::placeholder);
        const auto now = replayChipExact (tr.trace, bytes, rr.reference, std::nullopt, true, vs::bside::chip::ShapeSet::fitted);
        INFO (name);
        REQUIRE (old.error.empty());
        REQUIRE (now.error.empty());
        CHECK (now.mismatched == 0);
        for (int k = 0; k < kStateFields; ++k)
        {
            const auto i = static_cast<std::size_t> (k);
            CHECK (old.exemptPerField[i] == now.exemptPerField[i]);
            if (now.exemptPerField[i] == 0)
                continue;
            std::printf ("[shapes-replay] %-16s %-5s exempt %4zu: differing placeholder %4zu, fitted %4zu (now agreeing %4zu)\n", name.c_str(),
                         kStateFieldNames[k], now.exemptPerField[i], old.differPerField[i], now.differPerField[i],
                         now.exemptPerField[i] - now.differPerField[i]);
            INFO (kStateFieldNames[k]);
            const std::string key = name + " " + kStateFieldNames[k];
            const auto it = budget.find (key);
            const std::size_t allowed = it == budget.end() ? 0 : it->second;
            used.insert (key);
            if (! bless)
            {
                if (now.differPerField[i] > allowed)
                    FAIL_CHECK (key << ": " << now.differPerField[i] << " exempt checkpoints differ, budget " << allowed << " (a regression)");
                else if (now.differPerField[i] < allowed)
                    FAIL_CHECK (key << ": " << now.differPerField[i] << " exempt checkpoints differ, budget " << allowed
                                    << ": budget can be tightened: rebless");
            }
            if (now.differPerField[i] != 0)
                blessed += key + " " + std::to_string (now.differPerField[i]) + "\n";
            before += old.differPerField[i];
            after += now.differPerField[i];
            exempt += now.exemptPerField[i];
        }
    }
    std::printf ("[shapes-replay] all c_* traces: exempt field checkpoints %zu, differing placeholder %zu, fitted %zu\n", exempt, before, after);
    CHECK (after < before);
    for (const auto& [key, n] : budget)
        if (used.count (key) == 0)
            FAIL_CHECK ("budget key matches no exempt c_* trace and field: " << key << " " << n);
    if (bless)
    {
        std::ofstream f (*bless, std::ios::binary);
        f << blessed;
        REQUIRE (static_cast<bool> (f));
        std::printf ("[shapes-replay] wrote the exemption budget to %s\n", bless->c_str());
        FAIL ("blessed " << *bless << "; rerun without the variable BS_BLESS_EXEMPTION_BUDGET");
    }
}

TEST_CASE ("reference data: analog tolerances against our chip", "[trace][reference][chip]")
{
    // Compare.h: filter corners, resonance peaks, distortion, voice DC steps (8.9.4), measured with
    // AnalogMeasure.h on our chain (Chip::render -> Filter -> OutputStage, tests/bside/trace/AnalogChain.h) at
    // 192 kHz against the committed a_* data; the noise spectra are compared in RenderReferenceTests.cpp, the
    // modes (scripted tolerance) and what stays skipped in AnalogReferenceTests.cpp. Every figure is printed
    // ([analog] lines on stdout).
    using namespace vs::bside::test;
    for (const char m : { 'a', 'b' })
    {
        const std::string sm (1, m);
        // named strings, not temporaries: GCC 13's -Wdangling-reference would flag a temporary argument
        const std::string filterName = "a_filter_" + sm, bypassName = "a_filter_bypass_" + sm, distName = "a_distortion_" + sm,
                          dcName = "a_voice_dc_" + sm;
        const auto& f = renderedTrace (kDir, filterName);
        const auto& b = renderedTrace (kDir, bypassName);
        if (! f.ok || ! b.ok)
            SKIP (f.why << " " << b.why);
        INFO ("model " << sm);
        REQUIRE (f.reference.segments.size() == 32);
        REQUIRE (f.ours.segments.size() == 32);
        double worstCorner = 0.0, worstPeak = 0.0;
        for (std::size_t i = 0; i < 32; ++i)
        {
            INFO (f.reference.segments[i].name);
            REQUIRE_FALSE (f.segmentAmbiguous[i]); // one routing: the noise voice alone sounds
            REQUIRE_FALSE (b.segmentAmbiguous[i]);
            const auto ref = filterResponse (f.reference.segments[i], b.reference.segments[i]);
            const auto ours = filterResponse (f.ours.segments[i], b.ours.segments[i]);
            if (i < 16)
            {
                const auto cr = lowpassCorner (ref), co = lowpassCorner (ours);
                if (! cr.found)
                {
                    std::printf ("[analog] model %s %s corner: the reference has none the estimator resolves (below 40 Hz); ours %s %.1f Hz: not compared\n",
                                 sm.c_str(), f.reference.segments[i].name.c_str(), co.found ? "at" : "none,", co.hz);
                    continue;
                }
                REQUIRE (co.found);
                const auto v = checkFilterCorner (co.hz, cr.hz);
                std::printf ("[analog] model %s %s corner ours %8.1f Hz, reference %8.1f Hz: %+.2f %% (allowed 5 %%)\n", sm.c_str(),
                             f.reference.segments[i].name.c_str(), co.hz, cr.hz, 100.0 * (co.hz / cr.hz - 1.0));
                CHECK (v.pass);
                worstCorner = std::max (worstCorner, v.deviation);
            }
            else
            {
                const auto pr = resonancePeak (ref), po = resonancePeak (ours);
                REQUIRE (pr.found);
                REQUIRE (po.found);
                const auto v = checkResonancePeak (po.db, pr.db);
                std::printf ("[analog] model %s %s (resonance %2zu) peak ours %+6.2f dB at %7.1f Hz, reference %+6.2f dB at %7.1f Hz: %+.2f dB (allowed 1)\n",
                             sm.c_str(), f.reference.segments[i].name.c_str(), i - 16, po.db, po.hz, pr.db, pr.hz, po.db - pr.db);
                CHECK (v.pass);
                worstPeak = std::max (worstPeak, v.deviation);
            }
        }
        std::printf ("[analog] model %s worst corner %.2f %% (allowed 5), worst peak %.2f dB (allowed 1)\n", sm.c_str(), 100.0 * worstCorner, worstPeak);

        // distortion: harmonics 2..5 against the fundamental, triangle 160.1 Hz through the low-pass at 2047, drive
        // 1, 2, 3 voices (s00..s02, the gate, 8.9.4: Model A) and 3 voices at sustain 8 (s03, reported)
        const auto& d = renderedTrace (kDir, distName);
        REQUIRE (d.ok);
        REQUIRE (d.reference.segments.size() >= 4);
        const double f0 = 985248.0 * 0x0aa6 / 16777216.0;
        for (std::size_t i = 0; i < 4; ++i)
        {
            REQUIRE_FALSE (d.segmentAmbiguous[i]);
            const auto hr = harmonics (d.reference.segments[i], f0), ho = harmonics (d.ours.segments[i], f0);
            const auto v = checkDistortion (ho.data(), hr.data());
            std::printf ("[analog] model %s distortion %s: ours %6.1f %6.1f %6.1f %6.1f, reference %6.1f %6.1f %6.1f %6.1f dB (h2..h5; allowed +-3)%s\n",
                         sm.c_str(), d.reference.segments[i].name.c_str(), ho[0], ho[1], ho[2], ho[3], hr[0], hr[1], hr[2], hr[3],
                         i < 3 ? "" : " (reported)");
            if (m == 'a' && i < 3)
            {
                INFO (v.what);
                CHECK (v.pass);
            }
        }

        // voice DC: the level of the first whole frame after each write that changes the volume (8.15.7). The
        // gate is Model A's (8.9.4); Model B's steps (-27 to -42 dBFS) lie within 13 dB of the reference's board
        // high-pass residual (-46.4 dBFS, 8.15.6, a reference artefact we do not reproduce), so they are printed only.
        const auto& vd = renderedTrace (kDir, dcName);
        REQUIRE (vd.ok);
        double worstDc = 0.0;
        int steps = 0;
        std::uint8_t volume = 0;
        for (const auto& w : vd.trace.writes)
        {
            if (w.reg != 0x18)
                continue;
            const auto before = volume;
            volume = static_cast<std::uint8_t> (w.value & 0x0f);
            if (w.cycle < 1000 || volume == before)
                continue;
            const auto fr = static_cast<std::size_t> (frameOfCycle (vd.trace, w.cycle) + 1);
            const double lr = vd.reference.levelTenthDb[fr] / 10.0, lo = vd.ours.levelTenthDb[fr] / 10.0;
            const auto v = checkVoiceDcStep (lo, lr);
            std::printf ("[analog] model %s voice DC: write 18 %02x at cycle %llu, frame %zu: ours %6.1f dBFS, reference %6.1f dBFS: %+.1f dB (allowed 1)%s\n",
                         sm.c_str(), w.value, static_cast<unsigned long long> (w.cycle), fr, lo, lr, lo - lr, m == 'a' ? "" : " (reported)");
            ++steps;
            if (m == 'a')
            {
                CHECK (v.pass);
                worstDc = std::max (worstDc, v.deviation);
            }
        }
        CHECK (steps == 9);
        if (m == 'a')
            std::printf ("[analog] model a voice DC worst %.2f dB over %d volume steps (allowed 1)\n", worstDc, steps);
    }
}
