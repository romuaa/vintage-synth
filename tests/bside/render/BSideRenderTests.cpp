// B-Side render-regression tests (ctest label "render"): six factory programs rendered through the real processor and the
// shared harness (tests/render_regression/RenderHarness, instrument-independent; BSideInstance.cpp says which instrument),
// compared with the references in golden/ by the harness's own features and tolerances (docs/architecture.md 8.21.2).
//
// Updating references after an INTENDED sound change (the same option as Vintage's executable):
//   cmake -S . -B build/mac -DUPDATE_GOLDENS=ON && ctest --test-dir build/mac -L render
//   (or run bside_render with the environment variable VS_UPDATE_GOLDENS=1), review the diff of
//   tests/bside/render/golden/, and switch the option off again.
#include <catch2/catch_test_macros.hpp>

#include "RenderHarness.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace vs::rr;

namespace
{
constexpr int kGoldenBlock = 512;

std::string readFile (const juce::File& f)
{
    std::ifstream in (f.getFullPathName().toStdString(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool updateMode()
{
    const auto v = juce::SystemStats::getEnvironmentVariable ("VS_UPDATE_GOLDENS", {});
    return v.isNotEmpty() && v != "0";
}

juce::File scriptDir() { return juce::File (BS_RR_SCRIPT_DIR); }
juce::File goldenDir() { return juce::File (BS_RR_GOLDEN_DIR); }

std::vector<Features> featuresOfChannels (const std::vector<std::vector<float>>& chans, double rate)
{
    std::vector<Features> f;
    for (const auto& c : chans)
        f.push_back (extractFeatures (c, rate));
    return f;
}

/** How much of each tolerance a comparison uses (100 % = at the limit). */
void printMargins (const char* label, const char* channel, const Comparison& c)
{
    const Tolerances tol;
    std::printf ("%-24s %-6s %s: cells>3dB %.3f %% (%3.0f %% of limit), mean %.3f dB (%3.0f %%), level %.2f dB (%3.0f %%), tail %d (%3.0f %%)\n",
                 label, channel, c.pass ? "pass" : "FAIL", 100.0 * c.cellFraction, 100.0 * c.cellFraction / tol.maxCellFraction,
                 c.meanCell, 100.0 * c.meanCell / static_cast<double> (tol.meanCellDb), static_cast<double> (c.maxLevel),
                 100.0 * static_cast<double> (c.maxLevel) / static_cast<double> (tol.levelDb), c.tailShift,
                 100.0 * c.tailShift / tol.tailFrames);
}
} // namespace

TEST_CASE ("B-Side render regression: every script matches its golden reference", "[render]")
{
    const auto files = scriptDir().findChildFiles (juce::File::findFiles, false, "*.txt");
    REQUIRE (files.size() == 6);

    for (const auto& file : files)
    {
        const auto name = file.getFileNameWithoutExtension();
        INFO ("script: " << name);
        const auto script = parseScript (readFile (file));
        REQUIRE (script.channels == 2);
        const auto chans = renderScriptChannels (script, kGoldenBlock);
        maybeWriteWav (chans, script.sampleRate, name.toStdString());
        const auto feat = featuresOfChannels (chans, script.sampleRate);
        const auto goldenFile = goldenDir().getChildFile (name + ".vsgolden");

        if (updateMode())
        {
            goldenDir().createDirectory();
            REQUIRE (goldenFile.replaceWithText (juce::String (channelFeaturesToText (feat)), false, false, "\n"));
            std::printf ("updated golden: %s\n", goldenFile.getFullPathName().toRawUTF8());
            continue;
        }

        INFO ("missing reference " << goldenFile.getFullPathName() << " (create it with VS_UPDATE_GOLDENS=1)");
        REQUIRE (goldenFile.existsAsFile());
        const auto golden = channelFeaturesFromText (readFile (goldenFile));
        REQUIRE (static_cast<int> (golden.size()) == script.channels);

        // The host may use any block size: the reference must hold for odd and small ones too.
        const auto odd = featuresOfChannels (renderScriptChannels (script, 97), script.sampleRate);
        const auto tiny = featuresOfChannels (renderScriptChannels (script, 16), script.sampleRate);
        for (size_t ch = 0; ch < golden.size(); ++ch)
        {
            INFO ("channel " << (ch == 0 ? "left" : "right"));
            const auto c = compare (golden[ch], feat[ch]);
            INFO (c.report);
            CHECK (c.pass);
            printMargins (name.toRawUTF8(), ch == 0 ? "L" : "R", c);

            const auto c2 = compare (golden[ch], odd[ch]);
            INFO ("block size 97: " << c2.report);
            CHECK (c2.pass);
            printMargins (name.toRawUTF8(), ch == 0 ? "L/b97" : "R/b97", c2);
            const auto c3 = compare (golden[ch], tiny[ch]);
            INFO ("block size 16: " << c3.report);
            CHECK (c3.pass);
            printMargins (name.toRawUTF8(), ch == 0 ? "L/b16" : "R/b16", c3);
        }
    }
}

TEST_CASE ("B-Side render regression: the program command loads B-Side's own bank", "[render]")
{
    const auto bySlot = renderScript (parseScript ("length 1.5\n0 program A2\n0.1 on 60 100\n"), kGoldenBlock);
    const auto byName = renderScript (parseScript ("length 1.5\n0 program Moving Pulse\n0.1 on 60 100\n"), kGoldenBlock);
    const auto other = renderScript (parseScript ("length 1.5\n0 program A5\n0.1 on 60 100\n"), kGoldenBlock);
    const auto none = renderScript (parseScript ("length 1.5\n0.1 on 60 100\n"), kGoldenBlock);
    CHECK (bySlot == byName); // bit-identical: same program
    CHECK (bySlot != other);
    CHECK (bySlot != none);
    CHECK_THROWS (renderScript (parseScript ("length 1\n0 program Z9\n"), kGoldenBlock));
}
