// Hidden: renders B-Side's factory bank for the audition (docs/bside-factory-sounds.md) through the real preset path,
// one standard phrase per program (by category), and measures every program.
//   VS_RENDER_DIR=<dir> build/<dir>/tests/bside/BSideTests "Render bside_bank*"
// Output in $VS_RENDER_DIR (nothing is written without it; never the repository), 48 kHz / 24 bit:
//   <slot>-<name>.wav       one file per program: the phrase plus up to 4 s of its release
//   bside_audition.wav      all programs in slot order with 0.4 s of silence between them
//   bside_index.txt         start time, slot, name and category of every program in the audition file
//   bside_metrics.tsv       the measurement table (BankSound.h has the definitions)
// The table is also printed to the console whether or not VS_RENDER_DIR is set.
// With VS_BANK_DIR=<folder of program files named like the factory files> the programs are read from that folder and
// applied with PresetManager::applyProgram instead of coming from the embedded bank: the developer's loop for measuring a
// draft without rebuilding (the committed bank is always the embedded one).
#include <catch2/catch_test_macros.hpp>

#include "BankSound.h"

#include <cstdio>
#include <map>

using namespace bsbank;

namespace
{
juce::String fmt (double v, int decimals = 2) { return juce::String (v, decimals); }
double medianOf (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    return v.size() % 2 == 1 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}
} // namespace

TEST_CASE ("Render bside_bank audition and measurements", "[.render]")
{
    const auto dir = renderDir();
    if (dir != juce::File())
        dir.createDirectory();

    const auto folder = juce::SystemStats::getEnvironmentVariable ("VS_BANK_DIR", {});
    vs::presets::Bank folderBank;
    if (folder.isNotEmpty())
    {
        Rig schemaRig (-1);
        std::vector<juce::MemoryBlock> blocks;
        std::vector<vs::presets::FactoryFile> files;
        for (const auto& f : juce::File (folder).findChildFiles (juce::File::findFiles, false, "*.json"))
        {
            blocks.emplace_back();
            REQUIRE (f.loadFileAsData (blocks.back()));
        }
        size_t i = 0;
        for (const auto& f : juce::File (folder).findChildFiles (juce::File::findFiles, false, "*.json"))
        {
            files.push_back ({ f.getFileName(), static_cast<const char*> (blocks[i].getData()), blocks[i].getSize() });
            ++i;
        }
        juce::StringArray errors;
        folderBank = vs::presets::makeBankFromFiles (schemaRig.schema(), "Folder", files, &errors);
        INFO (errors.joinIntoString ("; "));
        REQUIRE (errors.isEmpty());
    }
    const auto& bank = folder.isNotEmpty() ? folderBank : factoryBank();
    const auto rigFor = [&] (int slot, bool limiter) {
        return std::make_unique<Rig> (slot, limiter, folder.isNotEmpty() ? &bank.programs[static_cast<size_t> (slot)] : nullptr);
    };
    constexpr double kAuditionTail = 4.0, kGap = 0.4;
    std::vector<float> allL, allR;
    juce::String index, table;
    table << "slot\tname\tcategory\tgroup\tmix_level_dB\tpeak_dBFS\trms_dB\tdev_dB\tcentroid_Hz\tchord_peak_dBFS\tchord_rms_dB\t"
             "reported_tail_s\tlim_on_peak_dBFS\tlim_on_chord_peak_dBFS";
    for (int b = 0; b < kBands; ++b)
        table << "\tband" << static_cast<int> (kBandEdges[b]) << "_dB";
    table << "\n";

    struct Row
    {
        juce::String line;
        Group group;
        double rms;
    };
    std::vector<Row> rows;
    std::map<Group, std::vector<double>> byGroup;

    for (int slot = 0; slot < kBankSize; ++slot)
    {
        const auto& prog = bank.programs[static_cast<size_t> (slot)];
        const auto info = categoryInfo (prog.meta.category);
        INFO (prog.meta.name);
        REQUIRE (info.known);
        const auto perf = phraseFor (prog);
        const auto chord = fullVelocityChord();

        auto rigP = rigFor (slot, false); // levels with the limiter off: the rule compares programs, not the limiter
        auto& rig = *rigP;
        const double reported = rig.proc.getTailLengthSeconds();
        const auto m = measure (rig.play (perf, std::max (reported, 1.0) + 0.5), perf);
        auto chordRig = rigFor (slot, false);
        Performance window = chord;
        window.firstOn = at (0.55); // steady part of the chord
        const auto cm = measure (chordRig->play (chord, 0.0), window);

        // ... and the audition as a new instance plays it: limiter on at its defaults.
        auto onRig = rigFor (slot, true);
        const auto out = onRig->play (perf, kAuditionTail + 0.5);
        const auto mOn = measure (out, perf);
        auto chordOnRig = rigFor (slot, true);
        const auto cmOn = measure (chordOnRig->play (chord, 0.0), chord);

        const auto pos = rig.presets().positionName (slot);
        const double level = std::max (rig.get ("lower.mix.level"), rig.get ("upper.mix.level"));
        juce::String line;
        line << pos << "\t" << prog.meta.name << "\t" << prog.meta.category << "\t" << groupName (info.group) << "\t"
             << fmt (rig.get ("lower.mix.level"), 1) << "/" << fmt (rig.get ("upper.mix.level"), 1) << "\t" << fmt (m.peakDb) << "\t"
             << fmt (m.rmsDb) << "\t%DEV%\t" << fmt (m.centroidHz, 0) << "\t" << fmt (cm.peakDb) << "\t" << fmt (cm.rmsDb) << "\t"
             << fmt (reported) << "\t" << fmt (mOn.peakDb) << "\t" << fmt (cmOn.peakDb);
        for (int b = 0; b < kBands; ++b)
            line << "\t" << fmt (m.bandDb[b], 1);
        line << "\n";
        (void) level;
        rows.push_back ({ line, info.group, m.rmsDb });
        if (info.group != Group::Reference)
            byGroup[info.group].push_back (m.rmsDb);
        CHECK (m.finite);

        if (dir != juce::File())
        {
            // Audition clip: the phrase plus up to 4 s of release, with a 50 ms fade if cut.
            const size_t len = std::min (out.l.size(), static_cast<size_t> (perf.lastOff + at (kAuditionTail)));
            std::vector<float> l (out.l.begin(), out.l.begin() + static_cast<std::ptrdiff_t> (len));
            std::vector<float> r (out.r.begin(), out.r.begin() + static_cast<std::ptrdiff_t> (len));
            if (len < out.l.size())
            {
                const size_t fade = static_cast<size_t> (at (0.05));
                for (size_t i = 0; i < fade; ++i)
                {
                    const float g = static_cast<float> (i) / static_cast<float> (fade);
                    l[len - 1 - i] *= g;
                    r[len - 1 - i] *= g;
                }
            }
            writeWav (dir.getChildFile (pos + "-" + prog.meta.name.replaceCharacter (' ', '_') + ".wav"), l, r);
            const double start = static_cast<double> (allL.size()) / kFs;
            index << juce::String::formatted ("%02d:%05.2f", static_cast<int> (start) / 60, std::fmod (start, 60.0)) << "  " << pos << "  "
                  << prog.meta.name << "  [" << prog.meta.category << "]\n";
            allL.insert (allL.end(), l.begin(), l.end());
            allR.insert (allR.end(), r.begin(), r.end());
            allL.resize (allL.size() + static_cast<size_t> (at (kGap)), 0.0f);
            allR.resize (allR.size() + static_cast<size_t> (at (kGap)), 0.0f);
        }
    }

    std::map<Group, double> median;
    for (const auto& [g, v] : byGroup)
        median[g] = medianOf (v);
    for (const auto& row : rows)
        table << row.line.replace ("%DEV%", fmt (row.group == Group::Reference ? 0.0 : row.rms - median[row.group]));
    for (const auto& [g, med] : median)
        table << "# median RMS " << groupName (g) << " " << fmt (med, 2) << " dB\n";

    if (dir != juce::File())
    {
        writeWav (dir.getChildFile ("bside_audition.wav"), allL, allR);
        dir.getChildFile ("bside_index.txt").replaceWithText (index);
        dir.getChildFile ("bside_metrics.tsv").replaceWithText (table);
    }
    std::printf ("%s", table.toRawUTF8());
}
