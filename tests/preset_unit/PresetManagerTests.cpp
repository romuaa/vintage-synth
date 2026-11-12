// The preset manager through the real processor (docs/architecture.md 6b.3, 6b.5): applying and
// capturing programs and tones, the modified flag, files and their locations, banks, the factory
// bank, the host program list and the audio hand-over.

#include <catch2/catch_test_macros.hpp>

#include "PresetTestHelpers.h"
#include "vintage/PluginEditor.h"
#include "vintage/presets/VintagePresets.h"

#include "../seq_unit/AllocationCounter.h" // VS_SEQ_TEST_ASAN

#include <thread>

using namespace vs;
using namespace vs::presets;
using namespace vs::ptest;

namespace
{
/** Peak of |x[n] - x[n-1]| over [from, to): the biggest step between neighbouring samples. */
double maxStep (const std::vector<float>& x, size_t from, size_t to)
{
    double m = 0.0;
    to = std::min (to, x.size());
    for (size_t i = std::max<size_t> (from, 1); i < to; ++i)
        m = std::max (m, static_cast<double> (std::abs (x[i] - x[i - 1])));
    return m;
}

std::vector<Midi> chord (int pos, std::initializer_list<int> notes, int vel = 100)
{
    std::vector<Midi> m;
    for (int n : notes)
        m.push_back (noteOn (pos, n, vel));
    return m;
}

Midi programChange (int pos, int program) { return { pos, juce::MidiMessage::programChange (1, program) }; }
} // namespace

// =================================================================================================
// Start-up state and the factory bank
// =================================================================================================

TEST_CASE ("A new instance is on factory program A1 (Init), unmodified, with the defaults sounding", "[presets][manager]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    CHECK (pm.currentIndex() == 0);
    CHECK (pm.currentName() == "Init");
    CHECK (pm.bankName() == "Factory");
    CHECK (pm.isFactoryBank());
    CHECK_FALSE (pm.isModified());
    CHECK (rig.differences (rig.schema().defaults()).isEmpty());
    CHECK (PresetManager::positionName (0) == "A1");
    CHECK (PresetManager::positionName (9) == "B2");
    CHECK (PresetManager::positionName (63) == "H8");
}

TEST_CASE ("Factory bank: placeholders in A1-A8, Init everywhere else, nothing fails to parse", "[presets][factory]")
{
    PresetRig rig;
    juce::StringArray errors;
    const auto bank = makeVintageFactoryBank (rig.schema(), &errors);
    INFO (errors.joinIntoString ("; "));
    CHECK (errors.isEmpty());
    REQUIRE (bank.programs.size() == static_cast<size_t> (kBankSize));
    CHECK (bank.name == "Factory");

    const char* names[] = { "Init", "Placeholder Pad", "Placeholder Bass", "Placeholder Lead", "Placeholder Dual Layer",
                            "Placeholder Split", "Placeholder Arp", "Placeholder Effects" };
    const auto init = rig.schema().defaults();
    for (int i = 0; i < 8; ++i)
    {
        const auto& p = bank.programs[static_cast<size_t> (i)];
        CHECK (p.meta.name == names[i]);
        if (i > 0)
        {
            CHECK (p.meta.category == "Placeholder");
            CHECK (p.meta.author.contains ("placeholder"));
            // Obviously different from Init, and from each other.
            int different = 0;
            for (size_t k = 0; k < p.values.size(); ++k)
                different += sameBitsF (p.values[k], init[k]) ? 0 : 1;
            INFO (names[i]);
            CHECK (different >= 5);
            for (int j = 1; j < i; ++j)
                CHECK_FALSE (sameValues (p.values, bank.programs[static_cast<size_t> (j)].values));
        }
    }
    for (int i = 8; i < kBankSize; ++i)
    {
        const auto& p = bank.programs[static_cast<size_t> (i)];
        CHECK (p.meta.name == "Init");
        CHECK (sameValues (p.values, init));
    }
    // Specific values the files carry (a wrong slot or key mapping would show here).
    const auto at = [&] (int slot, const char* id) { return bank.programs[static_cast<size_t> (slot)].values[static_cast<size_t> (rig.schema().indexOf (id))]; };
    CHECK (feq (at (4, "key.mode"), 1.0f));
    CHECK (feq (at (5, "key.mode"), 2.0f));
    CHECK (feq (at (4, "upper.assign.mode"), 2.0f));
    CHECK (feq (at (4, "lower.assign.mode"), 0.0f));
    CHECK (feq (at (6, "arp.mode"), 1.0f));
    CHECK (feq (at (2, "lower.assign.mode"), 2.0f));
    CHECK (feq (at (2, "upper.assign.mode"), 0.0f));
    CHECK (feq (at (7, "fx.reverb.level"), 0.4f));
}

TEST_CASE ("Every placeholder program loads, sounds, is finite and survives a state round trip", "[presets][factory]")
{
    for (int slot = 0; slot < 8; ++slot)
    {
        PresetRig rig;
        rig.prepare();
        REQUIRE (rig.presets().selectProgram (slot));
        const auto name = rig.presets().currentName();
        INFO ("program " << slot << " " << name);
        const auto out = rig.render (rig.samples (1.6), chord (rig.samples (0.05), { 48, 55, 60, 64 }), 256);
        double peak = 0.0;
        bool finite = true;
        for (size_t i = 0; i < out.l.size(); ++i)
        {
            finite = finite && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
            peak = std::max ({ peak, static_cast<double> (std::abs (out.l[i])), static_cast<double> (std::abs (out.r[i])) });
        }
        CHECK (finite);
        CHECK (peak > 0.01);
        CHECK (peak < 3.9); // below the +12 dBFS safety clamp

        juce::MemoryBlock state;
        rig.proc.getStateInformation (state);
        PresetRig other;
        other.prepare();
        other.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        CHECK (other.presets().currentIndex() == slot);
        CHECK (other.presets().currentName() == name);
        CHECK_FALSE (other.presets().isModified());
        CHECK (other.differences (rig.raw()).isEmpty());
        CHECK (sameValues (other.raw(), rig.raw())); // exactly the same sound, bit for bit
    }
}

// =================================================================================================
// Applying
// =================================================================================================

TEST_CASE ("selectProgram sets every program parameter and nothing else", "[presets][manager]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    rig.set ("master.volume", -20.0f); // not part of a program
    const auto program = randomProgram (rig.schema(), 11, "Mine");
    pm.setBank ([&] {
        Bank b = makeInitBank (rig.schema());
        b.programs[5] = program;
        return b;
    }(), (rig.temp / "b.vsbank").getFullPathName());

    REQUIRE (pm.selectProgram (5));
    CHECK (pm.currentIndex() == 5);
    CHECK (pm.currentName() == "Mine");
    CHECK (pm.currentCategory() == "Test");
    CHECK (pm.currentAuthor() == "Tester");
    CHECK_FALSE (pm.isModified());
    CHECK (rig.differences (program.values).isEmpty());
    CHECK (rig.get ("master.volume") < -19.9f);
    CHECK (rig.get ("master.volume") > -20.1f);
    // Nearly every program parameter is non-default in this program: they were all really set.
    int changedFromDefault = 0;
    for (int i = 0; i < rig.schema().numParams(); ++i)
        if (! rig.schema().isExcluded (i) && ! rig.matches (i, program.values[static_cast<size_t> (i)], rig.schema().spec (i).def))
            ++changedFromDefault;
    CHECK (changedFromDefault > 120); // (a two-way choice lands on its default half of the time)

    CHECK_FALSE (pm.selectProgram (64));
    CHECK_FALSE (pm.selectProgram (-1));
    CHECK (pm.currentIndex() == 5);
}

TEST_CASE ("Parameters changed by hosts follow a program change (the host is notified)", "[presets][manager]")
{
    PresetRig rig;
    struct Counter : juce::AudioProcessorParameter::Listener
    {
        int values = 0, begins = 0, ends = 0;
        void parameterValueChanged (int, float) override { ++values; }
        void parameterGestureChanged (int, bool starting) override { (starting ? begins : ends)++; }
    } counter;
    auto* cutoff = rig.proc.getState().getParameter ("lower.vcf.cutoff");
    cutoff->addListener (&counter);
    rig.presets().selectProgram (2); // Placeholder Bass: cutoff 0.3 (default 0.6)
    cutoff->removeListener (&counter);
    CHECK (counter.values >= 1);
    CHECK (counter.begins == 1);
    CHECK (counter.ends == 1); // a gesture around the change, so hosts record it as one edit
    CHECK (cutoff->getValue() < 0.35f);
}

TEST_CASE ("The modified flag", "[presets][manager]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    pm.selectProgram (2);
    CHECK_FALSE (pm.isModified()); // applying a program does not count as an edit

    SECTION ("a parameter change after a load sets it; a load clears it")
    {
        rig.set ("lower.vcf.resonance", 0.9f);
        CHECK (pm.isModified());
        pm.selectProgram (2);
        CHECK_FALSE (pm.isModified());
    }
    SECTION ("writing the value a parameter already has is not a change")
    {
        rig.set ("lower.vcf.resonance", rig.get ("lower.vcf.resonance"));
        CHECK_FALSE (pm.isModified());
    }
    SECTION ("master volume is not part of a program")
    {
        rig.set ("master.volume", -30.0f);
        CHECK_FALSE (pm.isModified());
    }
    SECTION ("a change made on another thread (host automation) counts")
    {
        std::thread t ([&] {
            auto* p = rig.proc.getState().getParameter ("fx.reverb.level");
            p->setValueNotifyingHost (0.8f);
        });
        t.join();
        CHECK (pm.isModified());
    }
    SECTION ("saving the program clears it; loading a tone sets it")
    {
        rig.set ("lower.vcf.resonance", 0.9f);
        REQUIRE (pm.isModified());
        REQUIRE (pm.saveProgramFile (rig.temp / "p.vsprog").isEmpty());
        CHECK_FALSE (pm.isModified());

        REQUIRE (pm.saveToneFile (rig.temp / "t.vstone", 0).isEmpty());
        CHECK_FALSE (pm.isModified()); // saving a tone does not touch the program
        REQUIRE (pm.loadToneFile (rig.temp / "t.vstone", 1).isEmpty());
        CHECK (pm.isModified());
    }
}

TEST_CASE ("Initialise: every parameter at its default, the slot keeps its place", "[presets][manager]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    pm.selectProgram (6);
    REQUIRE (rig.differences (rig.schema().defaults()).size() > 5);
    rig.set ("master.volume", -12.0f);
    pm.initialise();
    CHECK (rig.differences (rig.schema().defaults()).isEmpty());
    CHECK (pm.currentName() == "Init");
    CHECK (pm.currentIndex() == 6);
    CHECK (pm.isModified()); // not the program stored in slot A7
    CHECK (rig.get ("master.volume") < -11.9f); // master volume untouched
}

TEST_CASE ("Previous and next wrap around the bank", "[presets][manager]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    CHECK (pm.selectPrevious());
    CHECK (pm.currentIndex() == 63);
    CHECK (pm.currentName() == "Init");
    CHECK (pm.selectNext());
    CHECK (pm.currentIndex() == 0);
    CHECK (pm.selectNext());
    CHECK (pm.currentIndex() == 1);
    CHECK (pm.currentName() == "Placeholder Pad");
    CHECK (rig.differences (rig.schema().defaults()).size() > 5);
}

// =================================================================================================
// Files
// =================================================================================================

TEST_CASE ("Program files: save, change, load restores the sound exactly", "[presets][files]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    const auto mine = randomProgram (rig.schema(), 21, "My Sound");
    rig.setAll (mine.values);
    const auto file = rig.temp / "mine.vsprog";
    REQUIRE (pm.saveProgramFile (file, "My Sound").isEmpty());
    CHECK (pm.currentName() == "My Sound");
    const auto soundBefore = rig.raw();

    pm.initialise();
    REQUIRE (rig.differences (mine.values).size() > 100);
    pm.selectProgram (4);

    REQUIRE (pm.loadProgramFile (file).isEmpty());
    CHECK (sameValues (rig.raw(), soundBefore));
    CHECK (pm.currentName() == "My Sound");
    CHECK_FALSE (pm.isModified());
    CHECK (pm.currentIndex() == 4); // loading a file does not change the slot

    // Saving again writes the very same bytes.
    const auto text = readText (file);
    REQUIRE (pm.saveProgramFile (rig.temp / "again.vsprog").isEmpty());
    CHECK (readText (rig.temp / "again.vsprog") == text);
}

TEST_CASE ("Tone files: a tone saved from one part loads into the other and changes only that part", "[presets][files]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    const auto mine = randomProgram (rig.schema(), 22);
    rig.setAll (mine.values);
    REQUIRE (pm.saveToneFile (rig.temp / "t.vstone", 0, "Lower Tone").isEmpty());
    const auto text = readText (rig.temp / "t.vstone");
    CHECK (text.find ("\"lower.") == std::string::npos); // stored without the part prefix
    CHECK (text.find ("\"vcf.cutoff\"") != std::string::npos);
    CHECK (text.find ("\"name\": \"Lower Tone\"") != std::string::npos);

    pm.initialise();
    REQUIRE (pm.loadToneFile (rig.temp / "t.vstone", 1).isEmpty());
    const auto now = rig.raw();
    for (int k = 0; k < rig.schema().toneSize(); ++k)
    {
        const int lower = rig.schema().toneParam (0, k), upper = rig.schema().toneParam (1, k);
        INFO (rig.schema().spec (upper).id);
        CHECK (rig.matches (upper, now[static_cast<size_t> (upper)], mine.values[static_cast<size_t> (lower)]));
        CHECK (rig.matches (lower, now[static_cast<size_t> (lower)], rig.schema().spec (lower).def)); // Lower untouched
    }
    for (int g : rig.schema().globalParams())
        CHECK (rig.matches (g, now[static_cast<size_t> (g)], rig.schema().spec (g).def)); // globals untouched
}

namespace
{
/** Level in dB per octave band (100 Hz .. 12.8 kHz, 8 bands) of x[from, to), Hann-windowed FFT. */
std::vector<double> octaveBandsDb (const std::vector<float>& x, size_t from, size_t to, double fs)
{
    constexpr int order = 14, n = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data (static_cast<size_t> (2 * n), 0.0f);
    for (int i = 0; i < n && from + static_cast<size_t> (i) < to; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (n - 1));
        data[static_cast<size_t> (i)] = static_cast<float> (w) * x[from + static_cast<size_t> (i)];
    }
    fft.performFrequencyOnlyForwardTransform (data.data());
    std::vector<double> power (8, 0.0);
    for (int k = 1; k < n / 2; ++k)
    {
        const double f = k * fs / n;
        const int band = static_cast<int> (std::floor (std::log2 (f / 100.0)));
        if (band >= 0 && band < 8)
            power[static_cast<size_t> (band)] += static_cast<double> (data[static_cast<size_t> (k)]) * static_cast<double> (data[static_cast<size_t> (k)]);
    }
    for (auto& p : power)
        p = 10.0 * std::log10 (std::max (p, 1.0e-20));
    return power;
}
} // namespace

TEST_CASE ("A tone loaded into the other part sounds the same as in its own part", "[presets][files]")
{
    // Dual mode plays every note on both parts. Rig A: tone T in Lower, Upper silent. Rig B: Lower
    // silent, T loaded from a tone file into Upper. The parts' voices start from different seeds
    // (oscillator start phase), so the samples are not identical: the level and the spectrum are.
    // Source Mix 0 is VCO-1 alone, so no beating between two oscillators depends on their phases.
    const auto setup = [] (PresetRig& r) {
        r.set ("key.mode", 1);
        r.set ("lower.mixer.source_mix", 0.0f);
        r.set ("lower.vco1.waveform", 2);
        r.set ("lower.vcf.cutoff", 0.45f);
        r.set ("lower.vcf.resonance", 0.45f);
        r.set ("lower.vcf.env_mod", 0.0f);
        r.set ("lower.vcf.key_follow", 1.0f);
        r.set ("lower.env2.attack", 0.0f);
        r.set ("lower.env2.sustain", 1.0f);
        r.set ("lower.mix.pan", 0.0f);
    };
    const auto notes = chord (100, { 45, 52 }, 110);

    PresetRig a;
    setup (a);
    a.set ("upper.vca.level", 0.0f);
    a.prepare();
    const auto outA = a.render (a.samples (1.2), notes, 256);

    PresetRig b;
    setup (b);
    REQUIRE (b.presets().saveToneFile (b.temp / "t.vstone", 0).isEmpty());
    b.set ("lower.vca.level", 0.0f);
    REQUIRE (b.presets().loadToneFile (b.temp / "t.vstone", 1).isEmpty());
    b.prepare();
    const auto outB = b.render (b.samples (1.2), notes, 256);

    const size_t from = static_cast<size_t> (a.samples (0.3)), to = static_cast<size_t> (a.samples (1.1));
    const double levelA = toDb (rms (outA.l, from, to)), levelB = toDb (rms (outB.l, from, to));
    REQUIRE (levelA > -40.0); // it sounds
    CHECK (std::abs (levelA - levelB) < 0.5);
    CHECK (std::abs (toDb (rms (outA.r, from, to)) - toDb (rms (outB.r, from, to))) < 0.5);
    const auto bandsA = octaveBandsDb (outA.l, from, from + 16384, 48000.0), bandsB = octaveBandsDb (outB.l, from, from + 16384, 48000.0);
    double strongest = -1000.0;
    for (double v : bandsA)
        strongest = std::max (strongest, v);
    int compared = 0;
    for (size_t k = 0; k < bandsA.size(); ++k)
        if (bandsA[k] > strongest - 60.0)
        {
            ++compared;
            INFO ("band " << k << ": " << bandsA[k] << " vs " << bandsB[k]);
            CHECK (std::abs (bandsA[k] - bandsB[k]) < 1.0);
        }
    CHECK (compared >= 3);

    // What it is not: the same tone's neighbours. Loading nothing into Upper (its own default sound)
    // gives a different spectrum, so the comparison above really sees which tone the part has.
    PresetRig c;
    setup (c);
    c.set ("lower.vca.level", 0.0f); // Upper is at its defaults: a plain saw, open filter
    c.prepare();
    const auto outC = c.render (c.samples (1.2), notes, 256);
    const auto bandsC = octaveBandsDb (outC.l, from, from + 16384, 48000.0);
    double worst = 0.0;
    for (size_t k = 0; k < bandsA.size(); ++k)
        if (bandsA[k] > strongest - 60.0)
            worst = std::max (worst, std::abs (bandsA[k] - bandsC[k]));
    CHECK (worst > 3.0);
}

TEST_CASE ("A rejected file changes nothing: parameters, name, slot, flag and bank stay as they were", "[presets][files]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    pm.selectProgram (2);
    rig.set ("lower.vcf.resonance", 0.77f);
    const auto before = rig.raw();
    const auto good = toJson (rig.schema(), randomProgram (rig.schema(), 31));

    struct Bad
    {
        const char* name;
        std::string text;
    };
    std::vector<Bad> bad {
        { "truncated", good.substr (0, good.size() / 2) },
        { "wrong format", [&] { auto s = good; s.replace (s.find ("\"format\": 1"), 11, "\"format\": 7"); return s; }() },
        { "wrong kind", [&] { auto s = good; s.replace (s.find ("\"program\""), 9, "\"banker\""); return s; }() },
        { "empty", "" },
        { "binary junk", std::string ("\x00\x01\x02\xff\xfe", 5) },
        { "list", "[1,2,3]" },
    };
    for (const auto& b : bad)
    {
        INFO (b.name);
        const auto f = rig.temp / "bad.vsprog";
        f.replaceWithData (b.text.data(), b.text.size());
        CHECK (pm.loadProgramFile (f).isNotEmpty());
        CHECK (pm.loadToneFile (f, 0).isNotEmpty());
        CHECK (pm.loadBankFile (f).isNotEmpty());
        CHECK (sameValues (rig.raw(), before));
        CHECK (pm.currentName() == "Placeholder Bass");
        CHECK (pm.currentIndex() == 2);
        CHECK (pm.isModified());
        CHECK (pm.isFactoryBank());
        CHECK (pm.bankName() == "Factory");
    }
    CHECK (pm.loadProgramFile (rig.temp / "missing.vsprog").isNotEmpty());
    CHECK (pm.loadProgramFile (rig.temp.dir).isNotEmpty()); // a folder
    CHECK (sameValues (rig.raw(), before));
}

TEST_CASE ("Banks: save, load, store a program into a slot", "[presets][files]")
{
    PresetRig rig;
    auto& pm = rig.presets();

    // The factory bank is read-only.
    CHECK (pm.storeInBank (3).isNotEmpty());

    const auto bankFile = (rig.temp / "Banks").getChildFile ("mine.vsbank");
    REQUIRE (pm.saveBankFile (bankFile, "My Bank").isEmpty());
    CHECK (bankFile.getParentDirectory().isDirectory()); // created by the first save
    CHECK_FALSE (pm.isFactoryBank());
    CHECK (pm.bankName() == "My Bank");

    // Now a user bank: a sound can be stored into a slot.
    const auto mine = randomProgram (rig.schema(), 41, "Stored One");
    rig.setAll (mine.values);
    pm.renameProgram (pm.currentIndex(), "Stored One"); // the sound is stored under the current name
    REQUIRE (pm.storeInBank (9).isEmpty());
    CHECK (pm.currentIndex() == 9);
    CHECK_FALSE (pm.isModified());
    CHECK (pm.programName (9) == "Stored One");
    REQUIRE (pm.saveBankFile (bankFile).isEmpty());

    PresetRig other;
    REQUIRE (other.presets().loadBankFile (bankFile).isEmpty());
    CHECK (other.presets().bankName() == "My Bank");
    CHECK_FALSE (other.presets().isFactoryBank());
    CHECK (other.presets().currentIndex() == 0); // loading a bank selects its first program
    CHECK (other.presets().programName (9) == "Stored One");
    CHECK (other.presets().programName (1) == "Placeholder Pad"); // the rest was a copy of the factory bank
    REQUIRE (other.presets().selectProgram (9));
    CHECK (other.differences (mine.values).isEmpty());

    // And saving what was loaded writes the very same bytes.
    REQUIRE (other.presets().saveBankFile (other.temp / "copy.vsbank").isEmpty());
    CHECK (readText (other.temp / "copy.vsbank") == readText (bankFile));
}

TEST_CASE ("User file locations: nothing is created until the first save, and only the given files are written", "[presets][files]")
{
    PresetRig rig;
    auto& pm = rig.presets();
    const auto loc = pm.getLocations();
    CHECK (loc.root == rig.temp.dir);
    CHECK (loc.programs() == rig.temp.dir.getChildFile ("Programs"));
    CHECK (loc.tones() == rig.temp.dir.getChildFile ("Tones"));
    CHECK (loc.banks() == rig.temp.dir.getChildFile ("Banks"));

    // Selecting, initialising, applying and capturing write nothing.
    pm.selectProgram (3);
    pm.initialise();
    pm.captureProgram();
    pm.pollRequests();
    CHECK_FALSE (rig.temp.dir.exists());

    // A failed load writes nothing either.
    pm.loadProgramFile (loc.programs().getChildFile ("none.vsprog"));
    CHECK_FALSE (rig.temp.dir.exists());

    // The first save creates exactly the folder it needs and exactly that file (no temp leftovers).
    REQUIRE (pm.saveProgramFile (loc.programs().getChildFile ("First.vsprog")).isEmpty());
    CHECK (loc.programs().isDirectory());
    CHECK_FALSE (loc.tones().exists());
    CHECK_FALSE (loc.banks().exists());
    juce::StringArray names;
    for (const auto& f : juce::RangedDirectoryIterator (rig.temp.dir, true, "*", juce::File::findFilesAndDirectories))
        names.add (f.getFile().getRelativePathFrom (rig.temp.dir).replace ("\\", "/"));
    names.sort (false);
    CHECK (names.joinIntoString ("|") == "Programs|Programs/First.vsprog");

    // The default is under the user's documents area, and building it creates nothing.
    const auto def = Locations::userDefault ("Vintage Synth");
    CHECK (def.root.getFullPathName().endsWith ("Exitium" + juce::File::getSeparatorString() + "Vintage Synth"));
    CHECK (def.root.isAChildOf (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)));

    // A save into an unwritable place fails with a message instead of throwing or crashing.
    CHECK (pm.saveProgramFile (juce::File ("/proc/definitely/not/here/x.vsprog")).isNotEmpty());
}

// =================================================================================================
// MIDI Program Change
// =================================================================================================

TEST_CASE ("MIDI Program Change selects a program of the current bank", "[presets][midi]")
{
    PresetRig rig;
    rig.prepare();
    auto& pm = rig.presets();
    // Rig positions are absolute host samples: offsets are relative to the next block.
    const auto pc = [&] (int offset, int program) { return programChange (static_cast<int> (rig.now) + offset, program); };
    const auto expected = [&] (int slot) { return makeVintageFactoryBank (rig.schema()).programs[static_cast<size_t> (slot)].values; };

    rig.render (512, { pc (10, 3) }, 512);
    CHECK (pm.currentIndex() == 0); // only requested: the message thread applies it
    pm.pollRequests();
    CHECK (pm.currentIndex() == 3);
    CHECK (pm.currentName() == "Placeholder Lead");
    CHECK (rig.differences (expected (3)).isEmpty());
    CHECK_FALSE (pm.isModified());

    SECTION ("out of range is ignored (64..127), 0 and 63 are valid")
    {
        for (int bad : { 64, 65, 100, 127 })
        {
            rig.render (512, { pc (0, bad) }, 512);
            pm.pollRequests();
            CHECK (pm.currentIndex() == 3);
            CHECK (rig.differences (expected (3)).isEmpty());
        }
        rig.render (512, { pc (0, 63) }, 512);
        pm.pollRequests();
        CHECK (pm.currentIndex() == 63);
        rig.render (512, { pc (0, 0) }, 512);
        pm.pollRequests();
        CHECK (pm.currentIndex() == 0);
    }
    SECTION ("rapid successive changes: the latest wins, whether in one block or across blocks")
    {
        rig.render (512, { pc (1, 5), pc (100, 2), pc (200, 6), pc (300, 4) }, 512);
        pm.pollRequests();
        CHECK (pm.currentIndex() == 4);
        CHECK (rig.differences (expected (4)).isEmpty());

        rig.render (1536, { pc (1, 7), pc (600, 1), pc (1100, 2) }, 512);
        pm.pollRequests();
        CHECK (pm.currentIndex() == 2);
        CHECK (rig.differences (expected (2)).isEmpty());
        pm.pollRequests(); // nothing pending any more: no re-apply, no change
        CHECK (pm.currentIndex() == 2);
    }
    SECTION ("a program change does not disturb notes in the same block, and the other channel's too (omni)")
    {
        const auto out = rig.render (4800, { noteOn (static_cast<int> (rig.now), 60, 100), pc (50, 1) }, 512);
        CHECK (peakAbs (out.l, 0, out.l.size()) > 0.01);
        rig.render (512, { Midi { static_cast<int> (rig.now), juce::MidiMessage::programChange (7, 6) } }, 512);
        pm.pollRequests();
        CHECK (pm.currentIndex() == 6);
    }
    SECTION ("while notes sound they are released, and the new program plays the next notes")
    {
        rig.render (rig.samples (0.3), chord (static_cast<int> (rig.now), { 48, 55, 60 }), 256);
        REQUIRE (rig.proc.getLowerPart().isSounding());
        rig.render (256, { pc (0, 2) }, 256);
        pm.pollRequests(); // Placeholder Bass
        const auto tail = rig.render (rig.samples (3.0), {}, 256); // the held keys were released: it decays
        CHECK (peakAbs (tail.l, tail.l.size() - 4800, tail.l.size()) < 1.0e-3);
        CHECK_FALSE (rig.proc.getLowerPart().isSounding());
    }
}

TEST_CASE ("EventParser keeps Program Change apart from note events", "[presets][midi]")
{
    const std::uint8_t pc5[] = { 0xC0, 5 };
    const std::uint8_t pc127ch9[] = { 0xC9, 127 };
    const std::uint8_t pcBad[] = { 0xC0, 0x85 };   // data byte with the high bit set
    const std::uint8_t pcLong[] = { 0xC0, 5, 6 };  // 3 bytes: not a program change
    CHECK (parseProgramChange (pc5, 2) == 5);
    CHECK (parseProgramChange (pc127ch9, 2) == 127);
    CHECK (parseProgramChange (pcBad, 2) == -1);
    CHECK (parseProgramChange (pcLong, 3) == -1);
    CHECK (parseProgramChange (pc5, 1) == -1);
    CHECK (parseProgramChange (nullptr, 2) == -1);
    const std::uint8_t other[] = { 0xD0, 5 }; // channel pressure
    CHECK (parseProgramChange (other, 2) == -1);

    EventParser p;
    p.prepare (8);
    p.add (pc5, 2, 10);
    CHECK (p.events().empty()); // no NoteEvent
    CHECK (p.latestProgramChange() == 5);
    p.add (pc127ch9, 2, 20);
    CHECK (p.latestProgramChange() == 127); // the latest wins
    p.add (pcBad, 2, 30);
    CHECK (p.latestProgramChange() == 127);
    p.clear();
    CHECK (p.latestProgramChange() == -1);

    // It takes no slot: with the event buffer full (overflow) it is still seen, and the overflow rule is unchanged.
    const std::uint8_t on[] = { 0x90, 60, 100 };
    for (int i = 0; i < 20; ++i)
        p.add (on, 3, i);
    p.add (pc5, 2, 25);
    p.finish();
    CHECK (p.didOverflow());
    CHECK (p.latestProgramChange() == 5);
    CHECK (p.events().back().type == EventType::AllNotesOff);
}

// =================================================================================================
// Host program list
// =================================================================================================

TEST_CASE ("Host program list: 64 programs of the current bank", "[presets][host]")
{
    PresetRig rig;
    auto& proc = rig.proc;
    CHECK (proc.getNumPrograms() == 64);
    CHECK (proc.getCurrentProgram() == 0);
    CHECK (proc.getProgramName (0) == "Init");
    CHECK (proc.getProgramName (1) == "Placeholder Pad");
    CHECK (proc.getProgramName (7) == "Placeholder Effects");
    CHECK (proc.getProgramName (8) == "Init");
    CHECK (proc.getProgramName (63) == "Init");
    CHECK (proc.getProgramName (64).isEmpty());
    CHECK (proc.getProgramName (-1).isEmpty());

    proc.setCurrentProgram (4);
    CHECK (proc.getCurrentProgram() == 4);
    CHECK (rig.get ("key.mode") > 0.5f); // Placeholder Dual Layer: Dual
    CHECK (proc.getProgramName (4) == "Placeholder Dual Layer");

    proc.setCurrentProgram (64); // out of range: ignored
    proc.setCurrentProgram (-3);
    CHECK (proc.getCurrentProgram() == 4);

    SECTION ("names follow the bank; the current slot shows the current program's name")
    {
        REQUIRE (rig.presets().saveBankFile (rig.temp / "b.vsbank", "B").isEmpty());
        proc.changeProgramName (4, "Renamed");
        CHECK (proc.getProgramName (4) == "Renamed");
        CHECK (rig.presets().currentName() == "Renamed");
        proc.changeProgramName (5, "Other Slot"); // a user bank: any slot can be renamed
        CHECK (proc.getProgramName (5) == "Other Slot");
        proc.changeProgramName (99, "Nope");
    }
    SECTION ("the factory bank's slot names are read-only")
    {
        proc.changeProgramName (5, "Renamed");
        CHECK (proc.getProgramName (5) == "Placeholder Split");
        proc.changeProgramName (4, "Mine"); // only the current program's displayed name
        CHECK (proc.getProgramName (4) == "Mine");
        proc.setCurrentProgram (5);
        proc.setCurrentProgram (4);
        CHECK (proc.getProgramName (4) == "Placeholder Dual Layer");
    }
}

TEST_CASE ("setCurrentProgram from another thread is queued and applied on the message thread", "[presets][host][threads]")
{
    PresetRig rig;
    const auto before = rig.raw();
    std::thread t ([&] { rig.proc.setCurrentProgram (7); });
    t.join();
    CHECK (rig.proc.getCurrentProgram() == 7); // the host sees its request at once
    CHECK (sameValues (rig.raw(), before));    // but nothing was written from that thread
    rig.presets().pollRequests();
    CHECK (rig.proc.getCurrentProgram() == 7);
    CHECK (rig.differences (before).size() > 3);
    CHECK (rig.presets().currentName() == "Placeholder Effects");
}

TEST_CASE ("The preset bar shows bank, position, name and the modified marker", "[presets][ui]")
{
#if defined(VS_SEQ_TEST_ASAN)
    // Constructing real JUCE components makes macOS's font/graphics libraries parse system files,
    // where AddressSanitizer reports an overflow inside Apple's CoreGraphics (not our code). The
    // bar's logic lives in PresetManager (tested above); this construction check runs in the normal
    // macOS and Linux (xvfb) builds, and is skipped only in the ASan builds.
    SKIP ("not run under AddressSanitizer: system graphics library reports");
#endif
    PresetRig rig;
    std::unique_ptr<juce::AudioProcessorEditor> editor (rig.proc.createEditor());
    auto* ed = dynamic_cast<VintageSynthEditor*> (editor.get());
    REQUIRE (ed != nullptr);
    CHECK (ed->getBar().describe() == "Factory  A1  Init");
    rig.presets().selectProgram (9);
    CHECK (ed->getBar().describe() == "Factory  B2  Init");
    rig.presets().selectProgram (2);
    CHECK (ed->getBar().describe() == "Factory  A3  Placeholder Bass");
    rig.set ("lower.vcf.resonance", 0.9f);
    CHECK (ed->getBar().describe() == "Factory  A3  Placeholder Bass *");
}

// =================================================================================================
// Plugin state
// =================================================================================================

TEST_CASE ("Session state carries bank, program, name and modified flag; the sound is restored exactly", "[presets][state]")
{
    PresetRig rig;
    rig.presets().selectProgram (5);
    rig.set ("lower.vcf.resonance", 0.83f);
    rig.set ("master.volume", -9.0f);
    REQUIRE (rig.presets().isModified());

    juce::MemoryBlock state;
    rig.proc.getStateInformation (state);

    PresetRig other;
    other.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    CHECK (other.presets().currentIndex() == 5);
    CHECK (other.presets().currentName() == "Placeholder Split");
    CHECK (other.presets().isModified());
    CHECK (other.presets().isFactoryBank());
    CHECK (sameValues (other.raw(), rig.raw()));
    CHECK (other.proc.getCurrentProgram() == 5);
}

TEST_CASE ("A session restores the sound exactly whether or not the user bank still exists", "[presets][state]")
{
    PresetRig rig;
    const auto bankFile = rig.temp / "gone.vsbank";
    REQUIRE (rig.presets().saveBankFile (bankFile, "Gone Bank").isEmpty());
    const auto mine = randomProgram (rig.schema(), 51, "Only Here");
    rig.setAll (mine.values);
    rig.presets().renameProgram (rig.presets().currentIndex(), "Only Here");
    REQUIRE (rig.presets().storeInBank (12).isEmpty());
    juce::MemoryBlock state;
    rig.proc.getStateInformation (state);
    const auto sound = rig.raw();

    SECTION ("the bank is still there")
    {
        PresetRig other;
        other.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        CHECK (other.presets().bankName() == "Gone Bank");
        CHECK_FALSE (other.presets().isFactoryBank());
        CHECK (other.presets().currentIndex() == 12);
        CHECK (other.proc.getProgramName (12) == "Only Here");
        CHECK (sameValues (other.raw(), sound));
    }
    SECTION ("the bank file was deleted: back to the factory bank, same sound, same name")
    {
        REQUIRE (bankFile.deleteFile());
        PresetRig other;
        other.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        CHECK (other.presets().isFactoryBank());
        CHECK (other.presets().bankName() == "Factory");
        CHECK (other.presets().currentIndex() == 12);
        CHECK (other.presets().currentName() == "Only Here");
        CHECK_FALSE (other.presets().isModified());
        CHECK (sameValues (other.raw(), sound));
    }
    SECTION ("the bank file was replaced by garbage")
    {
        bankFile.replaceWithText ("this is not a bank");
        PresetRig other;
        other.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        CHECK (other.presets().isFactoryBank());
        CHECK (other.presets().currentName() == "Only Here");
        CHECK (sameValues (other.raw(), sound));
    }
}

TEST_CASE ("A state saved by the previous milestone (no preset information) loads to identical parameters", "[presets][state]")
{
    // tests/fixtures/state_m5.bin was written by the build before presets existed: every one of
    // its 170 parameters at a different, non-default value. The expected values were written next
    // to it by the same capture, as text.
    const juce::File dir (VS_FIXTURE_DIR);
    juce::MemoryBlock blob;
    REQUIRE (dir.getChildFile ("state_m5.bin").loadFileAsData (blob));
    REQUIRE (blob.getSize() > 1000);
    juce::StringArray lines;
    dir.getChildFile ("state_m5_expected.txt").readLines (lines);
    lines.removeEmptyStrings();
    REQUIRE (lines.size() == 170);

    PresetRig rig;
    REQUIRE (rig.differences (rig.schema().defaults()).isEmpty()); // starts at the defaults
    rig.proc.setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));

    int checked = 0, nonDefault = 0;
    for (const auto& line : lines)
    {
        const auto id = line.upToFirstOccurrenceOf ("\t", false, false);
        const auto expected = line.fromFirstOccurrenceOf ("\t", false, false).getFloatValue();
        const int i = rig.schema().indexOf (id);
        REQUIRE (i >= 0);
        const float actual = rig.proc.getState().getRawParameterValue (id)->load();
        INFO (id << ": " << actual << " vs " << expected);
        CHECK (std::abs (actual - expected) <= 1.0e-6f * (rig.schema().spec (i).hi - rig.schema().spec (i).lo));
        ++checked;
        if (std::abs (actual - rig.schema().spec (i).def) > 1.0e-4f)
            ++nonDefault;
    }
    CHECK (checked == 170);
    CHECK (nonDefault > 120); // it really did replace the defaults

    // No preset information in an old session: slot A1, no known program, counted as modified.
    CHECK (rig.presets().currentIndex() == 0);
    CHECK (rig.presets().currentName() == "Untitled");
    CHECK (rig.presets().isModified());
    CHECK (rig.presets().isFactoryBank());

    // And saving it in the new format and loading that again changes nothing.
    juce::MemoryBlock resaved;
    rig.proc.getStateInformation (resaved);
    PresetRig again;
    again.proc.setStateInformation (resaved.getData(), static_cast<int> (resaved.getSize()));
    CHECK (sameValues (again.raw(), rig.raw()));
}

TEST_CASE ("Damaged session blobs are ignored without harm", "[presets][state]")
{
    PresetRig rig;
    rig.presets().selectProgram (3);
    const auto before = rig.raw();
    rig.proc.setStateInformation (nullptr, 0);
    const char junk[] = "not a state";
    rig.proc.setStateInformation (junk, static_cast<int> (sizeof (junk)));
    CHECK (sameValues (rig.raw(), before));
    CHECK (rig.presets().currentIndex() == 3);
    CHECK_FALSE (rig.presets().isModified());

    // A preset child with nonsense in it (hand-edited or from a future version) is cleaned up.
    juce::ValueTree t ("Preset");
    t.setProperty ("program", 9999, nullptr);
    t.setProperty ("name", "A\nBad\tName " + juce::String::repeatedString ("x", 100), nullptr);
    t.setProperty ("bankKind", "user", nullptr);
    t.setProperty ("bankPath", "/definitely/not/there.vsbank", nullptr);
    auto state = rig.proc.getState().copyState();
    state.appendChild (t, nullptr);
    juce::MemoryBlock mb;
    if (auto xml = state.createXml())
        juce::AudioProcessor::copyXmlToBinary (*xml, mb);
    rig.proc.setStateInformation (mb.getData(), static_cast<int> (mb.getSize()));
    CHECK (rig.presets().currentIndex() == 63);
    CHECK (rig.presets().currentName().length() <= kMaxNameChars);
    CHECK_FALSE (rig.presets().currentName().containsAnyOf ("\n\t"));
    CHECK (rig.presets().isFactoryBank());
    CHECK (rig.differences (before).isEmpty());
}

// =================================================================================================
// The audio hand-over
// =================================================================================================

TEST_CASE ("ApplyGate: requests are acknowledged by the audio side; waits are bounded", "[presets][gate]")
{
    ApplyGate gate;
    gate.setTimeouts (0, 0);
    CHECK_FALSE (gate.takeReleaseRequest()); // nothing asked
    CHECK_FALSE (gate.requestReleaseAndWait()); // audio never ran: no waiting, not acknowledged
    CHECK (gate.takeReleaseRequest());
    CHECK_FALSE (gate.takeReleaseRequest()); // once per request
    gate.acknowledgeRelease();
    gate.requestReleaseAndWait();
    gate.requestReleaseAndWait();
    CHECK (gate.takeReleaseRequest()); // two requests before one block: one release serves both
    gate.acknowledgeRelease();
    CHECK_FALSE (gate.takeReleaseRequest());

    CHECK_FALSE (gate.isHolding());
    gate.beginHold();
    CHECK (gate.isHolding());
    gate.endHold();
    CHECK_FALSE (gate.isHolding());

    CHECK (gate.takeMidiProgram() == -1);
    gate.postMidiProgram (3);
    gate.postMidiProgram (9);
    CHECK (gate.takeMidiProgram() == 9);
    CHECK (gate.takeMidiProgram() == -1);

    SECTION ("a running audio thread acknowledges, and the wait ends as soon as it does")
    {
        gate.setTimeouts (1000, 500);
        std::atomic<bool> stop { false };
        std::thread audio ([&] {
            while (! stop)
            {
                gate.blockStarted();
                if (gate.takeReleaseRequest())
                    gate.acknowledgeRelease();
                juce::Thread::sleep (2);
            }
        });
        juce::Thread::sleep (20);
        const auto t0 = juce::Time::getMillisecondCounter();
        CHECK (gate.requestReleaseAndWait());
        CHECK (juce::Time::getMillisecondCounter() - t0 < 200);
        stop = true;
        audio.join();
    }
    SECTION ("an audio thread that stopped is not waited for beyond the idle timeout")
    {
        gate.setTimeouts (2000, 60);
        gate.blockStarted(); // audio ran once, then stopped
        const auto t0 = juce::Time::getMillisecondCounter();
        CHECK_FALSE (gate.requestReleaseAndWait());
        const auto waited = juce::Time::getMillisecondCounter() - t0;
        CHECK (waited >= 50);
        CHECK (waited < 500);
        // The request stays pending: the next block honours it.
        CHECK (gate.takeReleaseRequest());
    }
    SECTION ("a slow audio thread that is alive but never answers is given up on at the overall limit")
    {
        gate.setTimeouts (120, 10000);
        std::atomic<bool> stop { false };
        std::thread audio ([&] {
            while (! stop)
            {
                gate.blockStarted(); // alive, but never takes the request
                juce::Thread::sleep (2);
            }
        });
        juce::Thread::sleep (10);
        const auto t0 = juce::Time::getMillisecondCounter();
        CHECK_FALSE (gate.requestReleaseAndWait());
        const auto waited = juce::Time::getMillisecondCounter() - t0;
        CHECK (waited >= 100);
        CHECK (waited < 600);
        stop = true;
        audio.join();
    }
}

TEST_CASE ("While a preset is being applied the audio thread keeps its parameter values and key mode", "[presets][gate]")
{
    PresetRig rig;
    rig.prepare();
    rig.render (512, {}, 512);
    CHECK (rig.proc.getKeyRouter().getMode() == KeyMode::Whole);
    const float reverbBefore = rig.proc.getAppliedFxSettings().reverb.level;

    rig.presets().getGate().beginHold();
    rig.set ("key.mode", 1);
    rig.set ("fx.reverb.level", 0.7f);
    rig.set ("lower.vcf.cutoff", 0.2f);
    rig.render (512, chord (static_cast<int> (rig.now), { 60 }), 512);
    CHECK (rig.proc.getKeyRouter().getMode() == KeyMode::Whole);       // no half-applied change of mode
    CHECK (feq (rig.proc.getAppliedFxSettings().reverb.level, reverbBefore)); // nor of the effects
    const auto heldOut = rig.render (512, {}, 512);
    CHECK (peakAbs (heldOut.l, 0, heldOut.l.size()) > 0.0); // audio keeps running meanwhile

    rig.presets().getGate().endHold();
    rig.render (512, {}, 512);
    CHECK (rig.proc.getKeyRouter().getMode() == KeyMode::Dual);        // all of it at once afterwards
    CHECK (rig.proc.getAppliedFxSettings().reverb.level > 0.6f);
}

TEST_CASE ("Applying a program while notes sound releases them without a click", "[presets][manager][realtime]")
{
    // A smooth sustained chord (sines, low cutoff), so a hard cut would be the biggest step in the
    // signal by far; the held keys are never released by the player.
    PresetRig rig;
    rig.prepare();
    const int at = rig.samples (0.6);
    for (const int program : { 2, 4, 6, 7 }) // bass, dual layer, arp, effects
    {
        INFO ("program " << program);
        auto& r = rig;
        r.hostReset();
        r.presets().selectProgram (0);
        r.set ("lower.vco1.waveform", 0);
        r.set ("lower.vco2.waveform", 0);
        r.set ("lower.vcf.cutoff", 0.45f);
        r.set ("lower.vcf.env_mod", 0.0f);
        r.set ("lower.env2.attack", 0.0f);
        r.set ("lower.env2.sustain", 1.0f);
        r.set ("lower.env2.release", 0.4f);
        const auto base = static_cast<int> (r.now);
        const auto out = r.render (r.samples (4.0), chord (base + 100, { 48, 55, 64 }, 100),
                                   256, { { base + at, [program] (Rig& rr) { static_cast<PresetRig&> (rr).presets().selectProgram (program); } } });
        const size_t sw = static_cast<size_t> (at);
        const double before = maxStep (out.l, sw - 9600, sw);
        const double after = maxStep (out.l, sw, sw + 4800);
        REQUIRE (peakAbs (out.l, sw - 4800, sw) > 0.05); // it was sounding
        CHECK (after < 3.0 * before + 0.002);            // no step beyond what the sound itself does
        // The keys are still held but the voices were released: the sound decays away.
        CHECK (peakAbs (out.l, out.l.size() - 4800, out.l.size()) < 1.0e-3);
        CHECK (peakAbs (out.r, out.r.size() - 4800, out.r.size()) < 1.0e-3);
    }
}

TEST_CASE ("Program changes, MIDI Program Change and state restores never allocate on the audio thread", "[presets][realtime]")
{
#if defined(__APPLE__) && VS_SEQ_COUNT_ALLOCATIONS
    PresetRig rig (48000.0, 256);
    rig.prepare();
    juce::AudioBuffer<float> buffer (2, 256);
    // The audio side of each step is counted; the message side (which allocates) is not: the
    // counter watches one thread, and it is created around the audio call only.
    const auto countedBlock = [&] (juce::MidiBuffer midi) {
        buffer.clear();
        vs::seq::test::AllocationCounter counter;
        {
            auto* probe = new int (3);
            vs::seq::test::gEscape = probe;
            delete probe;
        }
        const int base = counter.count();
        rig.proc.processBlock (buffer, midi);
        return counter.count() - base;
    };

    juce::MidiBuffer notes;
    for (int n : { 48, 55, 60, 64 })
        notes.addEvent (juce::MidiMessage::noteOn (1, n, static_cast<juce::uint8> (100)), 0);
    CHECK (countedBlock (notes) == 0);
    for (int i = 0; i < 20; ++i)
        CHECK (countedBlock ({}) == 0);

    SECTION ("a program change while notes sound: the release block, the hold, the new parameters")
    {
        auto& gate = rig.presets().getGate();
        gate.beginHold();
        CHECK (countedBlock ({}) == 0); // held
        gate.requestReleaseAndWait();
        CHECK (countedBlock ({}) == 0); // the release is served and acknowledged
        gate.endHold();
        rig.presets().selectProgram (4); // message side
        for (int i = 0; i < 4; ++i)
            CHECK (countedBlock ({}) == 0);
        rig.presets().selectProgram (6); // arp program: arp settings change, events resync
        for (int i = 0; i < 4; ++i)
            CHECK (countedBlock (notes) == 0);
    }
    SECTION ("MIDI Program Change")
    {
        juce::MidiBuffer pc;
        pc.addEvent (juce::MidiMessage::programChange (1, 3), 5);
        pc.addEvent (juce::MidiMessage::programChange (1, 5), 9);
        pc.addEvent (juce::MidiMessage::programChange (1, 120), 11);
        CHECK (countedBlock (pc) == 0);
        rig.presets().pollRequests();
        CHECK (countedBlock ({}) == 0);
    }
    SECTION ("a state restore")
    {
        rig.presets().selectProgram (5);
        juce::MemoryBlock state;
        rig.proc.getStateInformation (state);
        rig.presets().selectProgram (2);
        rig.proc.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        for (int i = 0; i < 4; ++i)
            CHECK (countedBlock ({}) == 0);
        CHECK (rig.presets().currentIndex() == 5);
    }
    SECTION ("bypass during a pending release")
    {
        rig.presets().getGate().requestReleaseAndWait();
        buffer.clear();
        vs::seq::test::AllocationCounter counter;
        auto* probe = new int (3);
        vs::seq::test::gEscape = probe;
        delete probe;
        const int base = counter.count();
        juce::MidiBuffer none;
        for (int i = 0; i < 40; ++i)
            rig.proc.processBlockBypassed (buffer, none);
        CHECK (counter.count() == base);
    }
#else
    SUCCEED ("the allocation counter needs the macOS malloc hook; skipped here");
#endif
}

TEST_CASE ("Applying presets while the audio thread runs: acknowledged, finite, no crash", "[presets][threads]")
{
    PresetRig rig (48000.0, 128);
    rig.prepare();
    auto& pm = rig.presets();
    pm.getGate().setTimeouts (250, 40);

    std::atomic<bool> stop { false }, bad { false };
    std::atomic<int> blocks { 0 };
    std::thread audio ([&] {
        juce::AudioBuffer<float> buf (2, 128);
        for (int n = 0; ! stop; ++n)
        {
            juce::MidiBuffer mb;
            if (n % 7 == 0)
                mb.addEvent (juce::MidiMessage::noteOn (1, 40 + n % 30, static_cast<juce::uint8> (100)), 0);
            if (n % 9 == 0)
                mb.addEvent (juce::MidiMessage::noteOff (1, 40 + (n + 3) % 30), 5);
            if (n % 40 == 0)
                mb.addEvent (juce::MidiMessage::programChange (1, 1 + (n / 40) % 7), 7);
            buf.clear();
            rig.proc.processBlock (buf, mb);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 128; ++i)
                    if (! std::isfinite (buf.getSample (ch, i)) || std::abs (buf.getSample (ch, i)) > 3.99f)
                        bad = true;
            ++blocks;
            juce::Thread::sleep (2); // about real time
        }
    });

    int acknowledged = 0, applies = 0;
    const auto bank = makeVintageFactoryBank (rig.schema());
    for (int i = 0; i < 120; ++i)
    {
        pm.selectProgram (i % 8);
        ++applies;
        acknowledged += pm.lastApplyWasAcknowledged() ? 1 : 0;
        if (i % 5 == 0)
            rig.set ("lower.vcf.cutoff", 0.1f + 0.01f * static_cast<float> (i % 50)); // host automation meanwhile
        pm.pollRequests(); // MIDI program changes
        juce::Thread::sleep (3);
    }
    stop = true;
    audio.join();
    CHECK_FALSE (bad.load());
    CHECK (blocks.load() > 100);
    CHECK (acknowledged >= applies * 9 / 10); // a running audio thread answers (allow a few late ones on a busy machine)
}
