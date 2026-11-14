// The committed reference data of the trace replay (tests/bside/reference/, docs/bside-oracle-adapter.md):
// structure only, no chip needed. Every trace reads; its .bsref and .bsspec read, are canonical (the
// writer reproduces them byte for byte), belong to the trace (SHA-256 and checkpoint schedule), and the
// spec has the frame count and segment plan of SpecAnalysis.h; the MANIFEST lists every file with its
// SHA-256 and names the generator commits the files carry. The replay against our chip and the analog
// tolerances need the chip (S2/S3): those tests report themselves skipped until it exists.

#include "bside/trace/ReferenceFormat.h"
#include "bside/trace/Sha256.h"
#include "bside/trace/SpecAnalysis.h"
#include "bside/trace/TraceFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

TEST_CASE ("reference data: digital replay against our chip", "[trace][reference][chip]")
{
    // Replay.h, against vs::bside::chip::Chip once S2 builds it: every checkpoint exact.
    SKIP ("needs the chip (S2): no chip implementation exists yet");
}

TEST_CASE ("reference data: analog tolerances against our chip", "[trace][reference][chip]")
{
    // Compare.h: filter corners, resonance peaks, distortion, voice DC steps, noise spectra (8.9.4).
    SKIP ("needs the chip's output path (S3): no chip implementation exists yet");
}
