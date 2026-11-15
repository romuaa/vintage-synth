// The committed reference data of the trace replay (tests/bside/reference/, docs/bside-oracle-adapter.md):
// structure only, no chip needed. Every trace reads; its .bsref and .bsspec read, are canonical (the
// writer reproduces them byte for byte), belong to the trace (SHA-256 and checkpoint schedule), and the
// spec has the frame count and segment plan of SpecAnalysis.h; the MANIFEST lists every file with its
// SHA-256 and names the generator commits the files carry. The digital replay runs every trace through
// our chip (docs/architecture.md 8.9.4, 8.15, ChipReplay.h): exact at every checkpoint, with the
// shape exemption of 8.15.8 counted and reported per trace (`[replay]` lines on stdout); a `d_*` trace
// may have no exemption. The analog tolerances need the chip's output path (S2b/S3) and report
// themselves skipped until it exists.

#include "bside/trace/ChipReplay.h"
#include "bside/trace/ReferenceFormat.h"
#include "bside/trace/Sha256.h"
#include "bside/trace/SpecAnalysis.h"
#include "bside/trace/TraceFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
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

TEST_CASE ("reference data: the exemption covers real differences in the combined-waveform traces", "[trace][reference][chip]")
{
    // Our combined-waveform tables are placeholders (8.15.8), so some exempt checkpoints must actually
    // differ from the reference; otherwise the exemption would be untested.
    const auto all = traces();
    if (all.empty())
        SKIP ("no reference data committed");
    std::size_t exemptAndDifferent = 0;
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
        const auto r = replayChipExact (tr.trace, bytes, rr.reference);
        exemptAndDifferent += r.exempt - r.exemptButEqual;
    }
    CHECK (exemptAndDifferent > 0);
}

TEST_CASE ("reference data: analog tolerances against our chip", "[trace][reference][chip]")
{
    // Compare.h: filter corners, resonance peaks, distortion, voice DC steps, noise spectra (8.9.4).
    SKIP ("needs the chip's output path (S2b/S3): the digital core exists and is replayed above, but there is no audio path yet");
}
