// Hidden "[.uishots]" test (docs/architecture.md 9.6): renders every page x part at 100 % and
// 200 % to PNG for human review. Never compared (image snapshots are no gate). Writes only to
// $VS_UISHOT_DIR (outside the repository); does nothing without it.
//
//   VS_UISHOT_DIR=$TMPDIR/shots VintageSynthTests "[.uishots]"

#include <catch2/catch_test_macros.hpp>

#include "EditorRig.h"

using namespace vs;
using namespace vs::itest;

TEST_CASE ("UI shots: every page and part at 100 % and 200 % as PNG", "[.uishots]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VS_UISHOT_DIR", {});
    if (dir.isEmpty())
        SKIP ("VS_UISHOT_DIR not set");
    const juce::File out (dir);
    REQUIRE (out.createDirectory());

    EditorRig rig;
    // A sound with something to see: some resonance, a high-pass, sync on Upper.
    rig.set ("lower.vcf.resonance", 0.55f);
    rig.set ("lower.hpf.cutoff", 0.25f);
    rig.set ("upper.vco2.sync", 1.0f);
    rig.set ("upper.vco1.range", 4.0f);
    rig.set ("upper.vcf.slope", 0.0f);
    rig.set ("upper.vcf.cutoff", 0.45f);
    rig.tick();

    for (int scale : { 100, 200 })
    {
        rig.editor->setSizePercent (scale);
        for (int page = 0; page < static_cast<int> (ui::kVintagePageNames.size()); ++page)
            for (int part = 0; part < 2; ++part)
            {
                if (page >= 3 && part == 1)
                    continue; // global pages look the same for both parts
                rig.editor->selectPage (page, part);
                rig.tick();
                const auto image = rig.editor->createComponentSnapshot (rig.editor->getLocalBounds(), true, 1.0f);
                const auto file = out.getChildFile (juce::String (ui::kVintagePageNames[static_cast<size_t> (page)]) + (page < 3 ? (part == 0 ? "_lower" : "_upper") : "")
                                                    + "_" + juce::String (scale) + ".png");
                file.deleteFile();
                juce::FileOutputStream stream (file);
                REQUIRE (stream.openedOk());
                REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
            }
    }
}
