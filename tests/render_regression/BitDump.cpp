// Hidden "[.bitdump]" test: writes the raw float output of every golden script (and an extra
// stress script) to $VS_DUMP_DIR, so two builds on the same machine can be compared bit for bit
// (`cmp`). Used to prove that a change leaves the default sound bit-identical (Wave 3: effects
// and arpeggiator off). Never writes inside the repository; does nothing without VS_DUMP_DIR.
//
//   VS_DUMP_DIR=$TMPDIR/dump-before render_regression "[.bitdump]"   (old build)
//   VS_DUMP_DIR=$TMPDIR/dump-after  render_regression "[.bitdump]"   (new build)
//   for f in $TMPDIR/dump-before/*; do cmp "$f" "$TMPDIR/dump-after/${f##*/}"; done
#include <catch2/catch_test_macros.hpp>

#include "RenderHarness.h"

#include <fstream>
#include <sstream>

using namespace vs::rr;

namespace
{
// Key-mode changes with the pedal down, split moves, controllers, panic and full Vintage: the
// event paths the Wave 3 arpeggiator sits in, with the arpeggiator off.
constexpr const char* kStressScript = R"(
rate 48000
channels 2
length 4.0
param global.vintage 1
param lower.assign.mode 2
param lower.porta.time 0.2
param upper.vco1.waveform 3
param upper.mix.pan 0.4
0.00 on 48 90
0.05 cc 64 127
0.10 on 60 100
0.20 off 48
0.30 param key.mode 1
0.31 on 64 80
0.40 bend 12000
0.50 cc 1 90
0.60 off 64
0.70 param key.mode 2
0.70 param key.split_point 62
0.75 on 50 100
0.80 on 70 100
0.90 cc 64 0
1.00 param key.split_point 52
1.05 off 70
1.10 on 55 70
1.20 alloff
1.30 on 72 110
1.30 on 40 100
1.60 param key.mode 0
1.70 off 72
1.80 on 45 90
2.10 cc 121 0
2.20 off 45
2.30 off 40
2.40 param key.mode 1
2.50 on 67 100
2.50 on 43 100
3.00 off 67
3.00 off 43
)";

void dump (const std::vector<std::vector<float>>& chans, const juce::File& file)
{
    std::ofstream out (file.getFullPathName().toStdString(), std::ios::binary);
    for (const auto& c : chans)
        out.write (reinterpret_cast<const char*> (c.data()), static_cast<std::streamsize> (c.size() * sizeof (float)));
}

std::string readFile (const juce::File& f)
{
    std::ifstream in (f.getFullPathName().toStdString(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE ("Bit dump: raw output of every golden script for build-to-build comparison", "[.bitdump]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VS_DUMP_DIR", {});
    if (dir.isEmpty())
        SKIP ("VS_DUMP_DIR not set");
    const juce::File out (dir);
    REQUIRE (out.createDirectory());

    std::vector<std::pair<juce::String, Script>> scripts;
    for (const auto& f : juce::File (VS_RR_SCRIPT_DIR).findChildFiles (juce::File::findFiles, false, "*.txt"))
        scripts.emplace_back (f.getFileNameWithoutExtension(), parseScript (readFile (f)));
    scripts.emplace_back ("stress", parseScript (kStressScript));

    for (const auto& [name, script] : scripts)
        for (const int block : { 512, 61 })
            dump (renderScriptStereo (script, block), out.getChildFile (name + "_" + juce::String (block) + ".f32"));
}
