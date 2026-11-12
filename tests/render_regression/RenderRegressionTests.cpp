// Render-regression tests (ctest label "render"). See RenderHarness.h for the script format.
//
// Updating references after an INTENDED sound change:
//   cmake -S . -B build/mac -DUPDATE_GOLDENS=ON && ctest --test-dir build/mac -L render
//   (or run the test binary with the environment variable VS_UPDATE_GOLDENS=1), then review the
//   diff of tests/render_regression/golden/ and switch the option off again.
#include <catch2/catch_test_macros.hpp>

#include "RenderHarness.h"

#include <cmath>
#include <cstdio>
#include <functional>
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

juce::File scriptDir() { return juce::File (VS_RR_SCRIPT_DIR); }
juce::File goldenDir() { return juce::File (VS_RR_GOLDEN_DIR); }

Script loadScript (const char* name)
{
    return parseScript (readFile (scriptDir().getChildFile (juce::String (name) + ".txt")));
}

Features featuresOf (const Script& s, int block) { return extractFeatures (renderScript (s, block), s.sampleRate); }

bool isNote (const Event& e, int note) { return std::abs (e.value - note) < 0.5; }

Event* findParam (Script& s, const char* id)
{
    for (auto& e : s.events)
        if (e.kind == Event::Kind::param && e.id == id)
            return &e;
    return nullptr;
}

/** Applies a deliberate change to the self-test script and tells whether the comparison fails. */
bool detected (const Features& ref, const std::function<void (Script&)>& change, Comparison* out = nullptr)
{
    auto s = loadScript ("selftest_basic");
    change (s);
    const auto c = compare (ref, featuresOf (s, kGoldenBlock));
    if (out != nullptr)
        *out = c;
    return ! c.pass;
}
} // namespace

TEST_CASE ("Render regression: every script matches its golden reference", "[render]")
{
    const auto files = scriptDir().findChildFiles (juce::File::findFiles, false, "*.txt");
    REQUIRE (files.size() > 0);

    const auto featuresOfChannels = [] (const std::vector<std::vector<float>>& chans, double rate) {
        std::vector<Features> f;
        for (const auto& c : chans)
            f.push_back (extractFeatures (c, rate));
        return f;
    };

    for (const auto& file : files)
    {
        const auto name = file.getFileNameWithoutExtension();
        INFO ("script: " << name);
        const auto script = parseScript (readFile (file));
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
        REQUIRE (static_cast<int> (golden.size()) == script.channels); // a stereo script needs a stereo reference

        const auto odd = featuresOfChannels (renderScriptChannels (script, 97), script.sampleRate);
        for (size_t ch = 0; ch < golden.size(); ++ch)
        {
            INFO ("channel " << (ch == 0 ? "left" : "right"));
            const auto c = compare (golden[ch], feat[ch]);
            INFO (c.report);
            CHECK (c.pass);
            std::printf ("%-18s %s: cells>3dB %.3f %%, mean %.3f dB, worst %.2f dB, level %.2f dB, tail %d\n",
                         name.toRawUTF8(), ch == 0 ? "L" : "R", 100.0 * c.cellFraction, c.meanCell,
                         static_cast<double> (c.maxCell), static_cast<double> (c.maxLevel), c.tailShift);

            // The host may use any block size: the reference must hold for an odd, small one too.
            const auto c2 = compare (golden[ch], odd[ch]);
            INFO ("block size 97: " << c2.report);
            CHECK (c2.pass);
        }
    }
}

TEST_CASE ("Render regression: stereo golden format round-trips and keeps version 1 for one channel", "[render]")
{
    const auto s = parseScript ("channels 2\nlength 0.5\n0 on 60 100\n");
    CHECK (s.channels == 2);
    const auto chans = renderScriptChannels (s, kGoldenBlock);
    REQUIRE (chans.size() == 2);
    std::vector<Features> f { extractFeatures (chans[0], s.sampleRate), extractFeatures (chans[1], s.sampleRate) };
    const auto text = channelFeaturesToText (f);
    CHECK (text.rfind ("vsgolden 2\nchannels 2\n", 0) == 0);
    const auto back = channelFeaturesFromText (text);
    REQUIRE (back.size() == 2);
    CHECK (back[1].bandDb == f[1].bandDb);
    CHECK (back[0].levelDb == f[0].levelDb);
    // One channel: byte-identical to the version-1 writer.
    CHECK (channelFeaturesToText ({ f[0] }) == featuresToText (f[0]));
    CHECK (channelFeaturesFromText (featuresToText (f[0])).size() == 1);
    CHECK_THROWS (parseScript ("channels 3\nlength 1\n"));
    CHECK_THROWS (channelFeaturesFromText ("vsgolden 2\nchannels 2\n" + featuresToText (f[0])));
}

TEST_CASE ("Render regression: golden text format round-trips", "[render]")
{
    const auto f = featuresOf (loadScript ("selftest_basic"), kGoldenBlock);
    const auto back = featuresFromText (featuresToText (f));
    CHECK (back.frames == f.frames);
    CHECK (back.bandDb == f.bandDb);
    CHECK (back.levelDb == f.levelDb);
    CHECK_THROWS (featuresFromText ("vsgolden 1\nrate 48000\nbands 23\nframes 3\nlevel 1 2\n"));
    CHECK_THROWS (featuresFromText ("nonsense"));
}

TEST_CASE ("Render regression: script parser", "[render]")
{
    const auto s = parseScript ("rate 44100\nlength 1\n0.5 on 60 100 # comment\n0.1 param lower.vcf.cutoff 0.3\n");
    REQUIRE (s.events.size() == 2);
    CHECK (s.events[0].kind == Event::Kind::param); // sorted by time
    CHECK (s.events[1].kind == Event::Kind::noteOn);
    CHECK_THROWS (parseScript ("length 1\n0.5 dance 3\n"));
    CHECK_THROWS (parseScript ("0.5 on 60 100\n")); // no length
    CHECK_THROWS (parseScript ("length 1\non sixty 100\n"));
    CHECK_THROWS (renderScript (parseScript ("length 0.1\nparam no.such.param 1\n"), 256));
}

TEST_CASE ("Render regression: a render matches itself and other block sizes", "[render]")
{
    const auto script = loadScript ("selftest_basic");
    const auto ref = featuresOf (script, kGoldenBlock);

    SECTION ("identical render")
    {
        const auto c = compare (ref, featuresOf (script, kGoldenBlock));
        CHECK (c.pass);
        CHECK (c.maxCell < 1.0e-6f);
        CHECK (c.maxLevel < 1.0e-6f);
    }

    for (const int block : { 16, 37, 64, 256, 480, 1024, 4096 })
    {
        DYNAMIC_SECTION ("block size " << block)
        {
            const auto c = compare (ref, featuresOf (script, block));
            INFO (c.report);
            std::printf ("block %5d: cells>3dB %.3f %%, mean %.3f dB, worst %.2f dB, level %.2f dB, tail %d\n",
                        block, 100.0 * c.cellFraction, c.meanCell, static_cast<double> (c.maxCell),
                        static_cast<double> (c.maxLevel), c.tailShift);
            CHECK (c.pass);
        }
    }
}

TEST_CASE ("Render regression: numeric noise is tolerated", "[render]")
{
    // Stand-in for arm64/x64 and clang/MSVC differences: nudges that are far below anything audible
    // (cutoff +0.0001 = 0.001 octave = 0.07 %) must pass, even through the resonant filter.
    const auto ref = featuresOf (loadScript ("selftest_basic"), kGoldenBlock);
    for (const double nudge : { 1.0e-6, 1.0e-5, 1.0e-4 })
    {
        auto s = loadScript ("selftest_basic");
        findParam (s, "lower.vcf.cutoff")->value += nudge;
        findParam (s, "lower.vcf.resonance")->value += nudge;
        const auto c = compare (ref, featuresOf (s, kGoldenBlock));
        std::printf ("nudge %-8g: cells %.3f %%, mean %.3f dB, worst %.2f dB, level %.2f dB\n", nudge,
                     100.0 * c.cellFraction, c.meanCell, static_cast<double> (c.maxCell), static_cast<double> (c.maxLevel));
        INFO (c.report);
        CHECK (c.pass);
    }
}

TEST_CASE ("Render regression: deliberate sound changes are detected", "[render]")
{
    const auto ref = featuresOf (loadScript ("selftest_basic"), kGoldenBlock);

    const auto report = [] (const char* what, double amount, const Comparison& c) {
        std::printf ("%-28s %-8g -> %s (cells %.2f %%, mean %.2f dB, worst %.1f dB, level %.1f dB, tail %d)\n",
                     what, amount, c.pass ? "NOT detected" : "detected", 100.0 * c.cellFraction, c.meanCell,
                     static_cast<double> (c.maxCell), static_cast<double> (c.maxLevel), c.tailShift);
    };

    SECTION ("cutoff moved (initial value 0.5 = 640 Hz; 0.01 = 0.1 octave = 7 %)")
    {
        for (const double delta : { 0.002, 0.005, 0.01, 0.02, 0.05 })
        {
            Comparison c;
            detected (ref, [&] (Script& s) { findParam (s, "lower.vcf.cutoff")->value += delta; }, &c);
            report ("cutoff +", delta, c);
        }
        CHECK (detected (ref, [] (Script& s) { findParam (s, "lower.vcf.cutoff")->value += 0.005; })); // 0.05 octave = 3.5 %
    }

    SECTION ("env-1 decay changed")
    {
        for (const double delta : { 0.01, 0.02, 0.05, 0.1 })
        {
            Comparison c;
            detected (ref, [&] (Script& s) { findParam (s, "lower.env1.decay")->value += delta; }, &c);
            report ("env1 decay +", delta, c);
        }
        CHECK (detected (ref, [] (Script& s) { findParam (s, "lower.env1.decay")->value += 0.05; }));
    }

    SECTION ("resonance changed")
    {
        for (const double delta : { 0.02, 0.05, 0.1, 0.2 })
        {
            Comparison c;
            detected (ref, [&] (Script& s) { findParam (s, "lower.vcf.resonance")->value += delta; }, &c);
            report ("resonance +", delta, c);
        }
        CHECK (detected (ref, [] (Script& s) { findParam (s, "lower.vcf.resonance")->value += 0.1; }));
    }

    SECTION ("note missing")
    {
        CHECK (detected (ref, [] (Script& s) {
            std::erase_if (s.events, [] (const Event& e) { return e.kind != Event::Kind::param && e.time > 0.7 && e.time < 0.9; });
        }));
    }

    SECTION ("note released later (tail / duration)")
    {
        for (const double shift : { 0.025, 0.05, 0.1, 0.25 })
        {
            Comparison c;
            detected (ref, [&] (Script& s) {
                for (auto& e : s.events)
                    if (e.kind == Event::Kind::noteOff && isNote (e, 60))
                        e.time += shift;
            }, &c);
            report ("last note-off delayed s", shift, c);
        }
        CHECK (detected (ref, [] (Script& s) {
            for (auto& e : s.events)
                if (e.kind == Event::Kind::noteOff && isNote (e, 60))
                    e.time += 0.025; // one analysis frame
        }));
    }

    SECTION ("pitch changed")
    {
        CHECK (detected (ref, [] (Script& s) {
            for (auto& e : s.events)
                if (e.kind == Event::Kind::noteOn && isNote (e, 60))
                    e.value = 61.0; // one semitone
        }));
    }

    SECTION ("pitch bend ignored")
    {
        CHECK (detected (ref, [] (Script& s) { std::erase_if (s.events, [] (const Event& e) { return e.kind == Event::Kind::bend; }); }));
    }
}
