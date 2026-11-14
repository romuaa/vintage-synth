// Hidden: renders the factory bank for the M6b audition (docs/factory-sounds.md) through the real
// preset path, one standard phrase per program (by category), and measures every program.
//   VS_RENDER_DIR=$TMPDIR/renders/m6b build/<dir>/tests/VintageSynthTests "Render m6b_factory*"
// Output in $VS_RENDER_DIR (or the system temp directory; never the repository), 48 kHz / 24 bit:
//   <slot>-<name>.wav       one file per program: the phrase plus up to 4 s of its release
//   m6b_audition.wav        all 64 in slot order with 0.4 s of silence between them
//   m6b_index.txt           start time, slot, name and category of every program in the audition file
//   m6b_metrics.tsv         the measurement table (see FactorySound.h for the definitions)
#include <catch2/catch_test_macros.hpp>

#include "FactorySound.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdio>
#include <map>

using namespace vs;
using namespace vs::ftest;

namespace
{
void writeWav (const juce::File& file, const std::vector<float>& l, const std::vector<float>& r)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (kFs).withNumChannels (2).withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, options);
    REQUIRE (writer != nullptr);
    const float* ch[] { l.data(), r.data() };
    REQUIRE (writer->writeFromFloatArrays (ch, 2, static_cast<int> (l.size())));
}

juce::String fmt (double v, int decimals = 1) { return juce::String (v, decimals); }
} // namespace

TEST_CASE ("Render m6b_factory audition and measurements", "[.render]")
{
    const auto envDir = juce::SystemStats::getEnvironmentVariable ("VS_RENDER_DIR", {});
    auto dir = envDir.isNotEmpty() ? juce::File (envDir)
                                   : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("vs_renders");
    dir.createDirectory();

    const auto& bank = factoryBank();
    constexpr double kAuditionTail = 4.0, kGap = 0.4;
    std::vector<float> allL, allR;
    juce::String index, table;
    table << "slot\tname\tcategory\tgroup\tpeak_dBFS\trms_dB\tdev_dB\tattack_ms\trelease_ms\tcentroid_Hz\twidth_dB\t"
             "chord_peak_dBFS\tchord_rms_dB\treported_tail_s\tlim_on_peak_dBFS\tlim_on_chord_peak_dBFS\n";

    struct Row
    {
        juce::String line;
        LevelGroup group;
        double rms;
    };
    std::vector<Row> rows;
    std::map<LevelGroup, std::vector<double>> byGroup;

    for (int slot = 0; slot < kPresetBankSize; ++slot)
    {
        const auto& prog = bank.programs[static_cast<size_t> (slot)];
        const auto info = categoryInfo (prog.meta.category);
        INFO (prog.meta.name);
        REQUIRE (info.known);
        // Measurements with the output limiter off (levels compare programs, not the limiter) ...
        FactoryRig rig (slot, false);
        const auto perf = phraseFor (prog, rig.schema());
        const double reported = rig.proc.getTailLengthSeconds();
        const auto m = measure (rig.play (perf, std::max (reported, 1.0) + 0.5), perf);

        FactoryRig chordRig (slot, false);
        const auto chord = fullVelocityChord();
        const auto co = chordRig.play (chord, 0.0);
        Performance window = chord;
        window.firstOn = at (0.55); // steady part of the chord
        const auto cm = measure (co, window);

        // ... and the audition as a new instance plays it: limiter on at its defaults.
        FactoryRig onRig (slot);
        const auto out = onRig.play (perf, kAuditionTail + 0.5);
        const auto mOn = measure (out, perf);
        FactoryRig chordOnRig (slot);
        const auto cmOn = measure (chordOnRig.play (chord, 0.0), chord);

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
        const auto pos = rig.presets().positionName (slot);
        writeWav (dir.getChildFile (pos + "-" + prog.meta.name.replaceCharacter (' ', '_') + ".wav"), l, r);

        const double start = static_cast<double> (allL.size()) / kFs;
        index << juce::String::formatted ("%02d:%05.2f", static_cast<int> (start) / 60, std::fmod (start, 60.0)) << "  " << pos << "  "
              << prog.meta.name << "  [" << prog.meta.category << "]\n";
        allL.insert (allL.end(), l.begin(), l.end());
        allR.insert (allR.end(), r.begin(), r.end());
        allL.resize (allL.size() + static_cast<size_t> (at (kGap)), 0.0f);
        allR.resize (allR.size() + static_cast<size_t> (at (kGap)), 0.0f);

        juce::String line;
        line << pos << "\t" << prog.meta.name << "\t" << prog.meta.category << "\t" << groupName (info.group) << "\t"
             << fmt (m.peakDb) << "\t" << fmt (m.rmsDb) << "\t%DEV%\t" << fmt (m.attackMs, 0) << "\t"
             << (m.tailCut ? ">" : "") << fmt (m.releaseMs, 0) << "\t" << fmt (m.centroidHz, 0) << "\t" << fmt (m.widthDb) << "\t"
             << fmt (cm.peakDb) << "\t" << fmt (cm.rmsDb) << "\t" << fmt (reported) << "\t" << fmt (mOn.peakDb) << "\t"
             << fmt (cmOn.peakDb) << "\n";
        rows.push_back ({ line, info.group, m.rmsDb });
        byGroup[info.group].push_back (m.rmsDb);
        CHECK (m.finite);
    }

    std::map<LevelGroup, double> median;
    for (auto& [g, v] : byGroup)
    {
        std::sort (v.begin(), v.end());
        median[g] = v.size() % 2 == 1 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    }
    for (const auto& row : rows)
        table << row.line.replace ("%DEV%", fmt (row.rms - median[row.group]));
    for (const auto& [g, med] : median)
        table << "# median RMS " << groupName (g) << " " << fmt (med, 2) << " dB\n";

    writeWav (dir.getChildFile ("m6b_audition.wav"), allL, allR);
    dir.getChildFile ("m6b_index.txt").replaceWithText (index);
    dir.getChildFile ("m6b_metrics.tsv").replaceWithText (table);
    std::printf ("%s", table.toRawUTF8());
}
