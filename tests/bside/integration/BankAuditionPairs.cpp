// Hidden: renders the three comparison pairs of the S4b audition (docs/architecture.md 8.21.2, "Audition") through the real
// processor, for the owner's listening. Each pair is two files that differ in exactly one thing and have the same length, so
// that they can be compared by switching between them at the same position.
//   VS_RENDER_DIR=<dir> build/<dir>/tests/bside/BSideTests "Render bside_audition pairs"
// Output in $VS_RENDER_DIR (nothing is written without it; never the repository), 48 kHz / 24 bit stereo:
//   pair1_steal_clean_a/b, pair1_steal_authentic_a/b, pair1_steal_clean_soft_a   a voice steal against none
//   pair2_shared_modelB, pair2_pernote_modelB, pair2_shared_modelA, pair2_pernote_modelA   a soft note joining a loud one
//   pair3_init_now, pair3_init_before   Init at the current kChipGain against the previous one
//   PAIRS.md   what differs in every file, what to do when listening, and the printed measurements
// One line of measurements per file is also printed to the console whether or not VS_RENDER_DIR is set. The test makes no
// claim about how anything sounds: it prints numbers and writes instructions for the listener.
#include <catch2/catch_test_macros.hpp>

#include "BankSound.h"

#include <cstdio>

using namespace bsbank;

namespace
{
juce::String fmt (double v, int decimals = 2) { return juce::String (v, decimals); }

/** Both channels' largest sample-to-sample step over [from, to). */
double stepOf (const Stereo& s, int from, int to) { return std::max (maxStep (s.l, static_cast<size_t> (from), static_cast<size_t> (to)),
                                                                      maxStep (s.r, static_cast<size_t> (from), static_cast<size_t> (to))); }

double peakDbOf (const Stereo& s) { return dbOf (std::max (peakAbs (s.l, 0, s.l.size()), peakAbs (s.r, 0, s.r.size()))); }

/** Level of the strongest sinusoid within 1 % of `hz` in the mid signal over [from, to) (Hann window, a fine scan of the
    frequency): the amplitude of a sine of that frequency, in dBFS. */
double toneDb (const Stereo& s, int from, int to, double hz)
{
    const int n = to - from;
    std::vector<double> x (static_cast<size_t> (n));
    double wsum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (n - 1));
        x[static_cast<size_t> (i)] = 0.5 * (static_cast<double> (s.l[static_cast<size_t> (from + i)]) + s.r[static_cast<size_t> (from + i)]) * w;
        wsum += w;
    }
    double best = 0.0;
    for (double f = hz * 0.99; f <= hz * 1.01; f += 0.05)
    {
        double re = 0.0, im = 0.0;
        const double step = 2.0 * juce::MathConstants<double>::pi * f / kFs;
        for (int i = 0; i < n; ++i)
        {
            re += x[static_cast<size_t> (i)] * std::cos (step * i);
            im -= x[static_cast<size_t> (i)] * std::sin (step * i);
        }
        best = std::max (best, 2.0 * std::sqrt (re * re + im * im) / wsum);
    }
    return dbOf (best);
}

double noteHz (int note) { return 440.0 * std::pow (2.0, (note - 69) / 12.0); }

/** What is written and printed for the files of the run. */
struct Output
{
    juce::File dir; // empty: nothing is written
    juce::String md;
    void file (const juce::String& name, const juce::String& differs, const juce::String& listen, const Stereo& audio, const juce::String& measured)
    {
        if (dir != juce::File())
            writeWav (dir.getChildFile (name + ".wav"), audio.l, audio.r);
        const auto line = name + ".wav: " + measured;
        std::printf ("%s\n", line.toRawUTF8());
        md << "## " << name << ".wav\n\n- Differs: " << differs << "\n- Listen: " << listen << "\n- Measured: " << measured << "\n\n";
    }
};

// ---------------------------------------------------------------------------------------------------------------- pair 1

constexpr double kFirstOn = 0.1;                       // s, the chord (lets the program's smoothed values settle)
constexpr double kStealAt[4] { 1.1, 1.6, 2.1, 2.6 };   // s, one more key every 0.5 s
constexpr double kAllOff = 3.6;                        // s
constexpr double kPair1Length = 6.0;                   // s
constexpr int kStealNotes[4] { 40, 45, 50, 55 };       // none of them is in the chord
constexpr int kReleaseSetting = 12;                    // osc1.release: Init has 6

/** Init with the release lengthened (osc1.release 12 instead of 6, so that released voices ring on), Lower part, Poly
    (assign.mode 0, Init's own setting), the chord of 16 keys from C2 upward in fourths (36, 41, ... 111) held from 0.1 s
    to 3.6 s, and, if `steals`, one more key every 0.5 s from 1.1 s on (four keys, each takes a voice). `policy`:
    restart.policy 0 Clean, 2 Authentic. `stealVelocity` is the velocity of the stealing keys. */
Stereo stealRender (int policy, bool steals, int stealVelocity)
{
    Rig rig (0);
    rig.set ("lower.osc1.release", static_cast<float> (kReleaseSetting));
    rig.set ("lower.assign.mode", 0.0f);
    rig.set ("lower.restart.policy", static_cast<float> (policy));
    std::vector<Midi> midi;
    for (int k = 0; k < 16; ++k)
    {
        midi.push_back (noteOn (at (kFirstOn), 36 + 5 * k, kVelocity));
        midi.push_back (noteOff (at (kAllOff), 36 + 5 * k));
    }
    if (steals)
        for (int k = 0; k < 4; ++k)
        {
            midi.push_back (noteOn (at (kStealAt[k]), kStealNotes[k], stealVelocity));
            midi.push_back (noteOff (at (kAllOff), kStealNotes[k]));
        }
    return rig.render (at (kPair1Length), midi);
}

/** "step after / step before" for each steal: the largest sample-to-sample step within 10 ms after the steal's note-on and
    the largest step in the second before it (both channels), with the stealing keys' note-ons. */
juce::String stepsAt (const Stereo& s)
{
    juce::String out;
    for (int k = 0; k < 4; ++k)
    {
        const int t = at (kStealAt[k]);
        out << (k > 0 ? "; " : "") << "steal " << (k + 1) << " at " << fmt (kStealAt[k], 1) << " s: " << fmt (stepOf (s, t, t + at (0.010)), 5) << " / "
            << fmt (stepOf (s, t - at (1.0), t), 5);
    }
    return out;
}

void pair1 (Output& out)
{
    struct Variant
    {
        const char* name;
        int policy;
        bool steals;
        int velocity;
        const char* policyName;
    };
    const Variant variants[] {
        { "pair1_steal_clean_a", 0, true, kVelocity, "Clean" },           { "pair1_steal_clean_b", 0, false, kVelocity, "Clean" },
        { "pair1_steal_authentic_a", 2, true, kVelocity, "Authentic" },   { "pair1_steal_authentic_b", 2, false, kVelocity, "Authentic" },
        { "pair1_steal_clean_soft_a", 0, true, 1, "Clean" },
    };
    for (const auto& v : variants)
    {
        const auto audio = stealRender (v.policy, v.steals, v.velocity);
        REQUIRE (audio.l.size() == static_cast<size_t> (at (kPair1Length)));
        juce::String differs, listen;
        const juce::String common = juce::String ("Init, Lower part, Poly, lower.osc1.release ") + juce::String (kReleaseSetting) + " (Init: 6), restart.policy "
                                    + v.policyName + "; 16 keys from C2 upward in fourths (36, 41, ... 111) at velocity 100 held from 0.1 s to 3.6 s";
        if (v.steals)
            differs = common + "; WITH four more keys (40, 45, 50, 55) at 1.1, 1.6, 2.1 and 2.6 s at velocity " + juce::String (v.velocity)
                      + ", each of which takes a voice (all 16 are busy) and is released at 3.6 s";
        else
            differs = common + "; WITHOUT the four stealing keys (so no steal happens)";
        if (v.steals && v.velocity == 1)
            listen = "Switch between this file and pair1_steal_clean_b.wav at the same position, at about 1.1, 1.6, 2.1 and 2.6 s. The stealing keys are played at velocity 1, so that the voice that is taken is the main difference at those positions.";
        else if (v.steals)
            listen = juce::String ("Switch between this file and pair1_steal_") + (v.policy == 0 ? "clean" : "authentic")
                     + "_b.wav at the same position, at about 1.1, 1.6, 2.1 and 2.6 s, and say whether the moment of a steal can be told from the same moment without one.";
        else
            listen = juce::String ("This is the reference without a steal: compare it with pair1_steal_") + (v.policy == 0 ? "clean" : "authentic")
                     + "_a.wav at the positions 1.1, 1.6, 2.1 and 2.6 s.";
        juce::String measured = "peak " + fmt (peakDbOf (audio), 2) + " dBFS; length " + fmt (kPair1Length, 1) + " s";
        measured << "; largest sample-to-sample step within 10 ms after each steal time / in the second before it (full-scale 1.0): " << stepsAt (audio);
        out.file (v.name, differs, listen, audio, measured);
    }
}

// ---------------------------------------------------------------------------------------------------------------- pair 2

constexpr int kHeldNote = 48, kSoftNote = 55; // C3 and a fifth above
constexpr double kHeldOn = 0.1, kSoftOn = 1.1, kSoftOff = 2.6, kHeldOff = 4.1, kPair2Length = 4.6;

Stereo sharedRender (int model, int topology)
{
    Rig rig (0);
    rig.set ("lower.chip.model", static_cast<float> (model));
    rig.set ("lower.osc1.wave", 2.0f); // Saw
    rig.set ("lower.filter.mode", 1.0f); // Low
    rig.set ("lower.filter.cutoff", 1000.0f);
    rig.set ("lower.filter.resonance", 8.0f);
    rig.set ("lower.vel.volume", 1.0f);
    rig.set ("lower.filter.topology", static_cast<float> (topology));
    std::vector<Midi> midi { noteOn (at (kHeldOn), kHeldNote, 127), noteOff (at (kHeldOff), kHeldNote), noteOn (at (kSoftOn), kSoftNote, 8),
                             noteOff (at (kSoftOff), kSoftNote) };
    return rig.render (at (kPair2Length), midi);
}

void pair2 (Output& out)
{
    struct Variant
    {
        const char* name;
        int model, topology;
    };
    const Variant variants[] { { "pair2_shared_modelB", 1, 1 }, { "pair2_pernote_modelB", 1, 0 }, { "pair2_shared_modelA", 0, 1 }, { "pair2_pernote_modelA", 0, 0 } };
    for (const auto& v : variants)
    {
        const auto audio = sharedRender (v.model, v.topology);
        const double f0 = noteHz (kHeldNote);
        const double before = toneDb (audio, at (0.5), at (1.0), f0), during = toneDb (audio, at (1.5), at (2.5), f0), after = toneDb (audio, at (3.0), at (4.0), f0);
        const juce::String model = v.model == 1 ? "Model B" : "Model A";
        const juce::String topo = v.topology == 1 ? "Shared" : "Per Note";
        const auto other = juce::String (v.topology == 1 ? "pair2_pernote_model" : "pair2_shared_model") + (v.model == 1 ? "B" : "A");
        const auto differs = model + " (chip.model " + juce::String (v.model) + "), filter.topology " + topo + "; both files: Init, Lower part, osc1.wave Saw, filter.mode Low, filter.cutoff 1000, filter.resonance 8, vel.volume 1.0 (100 %); "
                             "key C3 (note 48) at velocity 127 from 0.1 s to 4.1 s, key G3 (note 55) at velocity 8 from 1.1 s to 2.6 s";
        const auto listen = "Switch between this file and " + other + ".wav at the same position, before 1.1 s, between 1.1 s and 2.6 s, and after 2.6 s, and say whether the held note changes when the soft key joins and leaves.";
        const auto measured = "peak " + fmt (peakDbOf (audio), 2) + " dBFS; level of the held note's fundamental (" + fmt (f0, 2) + " Hz, strongest line within 1 %, mid signal) before the second note (0.5 to 1.0 s) "
                              + fmt (before, 2) + " dBFS, during it (1.5 to 2.5 s) " + fmt (during, 2) + " dBFS, after it (3.0 to 4.0 s) " + fmt (after, 2)
                              + " dBFS; during minus before " + fmt (during - before, 2) + " dB, after minus before " + fmt (after - before, 2) + " dB";
        out.file (v.name, differs, listen, audio, measured);
    }
}

// ---------------------------------------------------------------------------------------------------------------- pair 3

constexpr double kGainBefore = 0.9739; // kChipGain before S4b

Stereo initPhrases (double gainFactor)
{
    // Init's Chord phrase (C4 E4 G4 C5 held 3 s, velocity 100) with 1 s of release, 0.5 s of silence, then a single held C4 (3 s, velocity 100)
    // with 1 s of release: two fresh rigs, so that the second phrase starts from the same state as the first.
    Stereo all;
    {
        Rig rig (0);
        const auto chord = phrase (Phrase::Chord);
        const auto s = rig.play (chord, 1.0);
        all.l.insert (all.l.end(), s.l.begin(), s.l.end());
        all.r.insert (all.r.end(), s.r.begin(), s.r.end());
    }
    all.l.resize (all.l.size() + static_cast<size_t> (at (0.5)), 0.0f);
    all.r.resize (all.r.size() + static_cast<size_t> (at (0.5)), 0.0f);
    {
        Rig rig (0);
        Performance single;
        addNote (single, 0.05, 3.05, 60, kVelocity);
        finish (single);
        const auto s = rig.play (single, 1.0);
        all.l.insert (all.l.end(), s.l.begin(), s.l.end());
        all.r.insert (all.r.end(), s.r.begin(), s.r.end());
    }
    for (auto& v : all.l)
        v = static_cast<float> (v * gainFactor);
    for (auto& v : all.r)
        v = static_cast<float> (v * gainFactor);
    return all;
}

void pair3 (Output& out)
{
    const auto now = initPhrases (1.0);
    const double factor = kGainBefore / bs::Part::kChipGain;
    const auto before = initPhrases (factor);
    REQUIRE (now.l.size() == before.l.size());
    const auto rmsOf = [&] (const Stereo& s, size_t a, size_t b) {
        double acc = 0.0;
        for (size_t i = a; i < b; ++i)
            acc += 0.5 * (static_cast<double> (s.l[i]) * s.l[i] + static_cast<double> (s.r[i]) * s.r[i]);
        return 10.0 * std::log10 (std::max (acc / static_cast<double> (b - a), 1.0e-20));
    };
    const auto differs = [&] (bool isNow) {
        return juce::String ("Init's Chord phrase (C4 E4 G4 C5 held 3 s, velocity 100, then 1 s of release), 0.5 s of silence, a single held C4 (3 s, velocity 100, then 1 s of release); ")
               + (isNow ? juce::String ("kChipGain as it is now (") + fmt (bs::Part::kChipGain, 4) + "); samples as rendered"
                        : "the same samples multiplied by " + fmt (kGainBefore, 4) + " / " + fmt (bs::Part::kChipGain, 4) + " = " + fmt (factor, 6) + " (the previous kChipGain "
                              + fmt (kGainBefore, 4) + " over the current one), applied after the processor's output, with the limiter at its default settings in both");
    };
    const auto listen = juce::String ("Switch between pair3_init_now.wav and pair3_init_before.wav at the same position, in the chord (0 to 3 s) and in the single note (4.55 to 7.6 s), and judge the loudness of the two against each other.");
    const auto measured = [&] (const Stereo& s) {
        const size_t singleFrom = static_cast<size_t> (at (4.55)) + static_cast<size_t> (at (0.05));
        return "peak " + fmt (peakDbOf (s), 2) + " dBFS; RMS of the chord phrase (0.05 to 3.05 s) " + fmt (rmsOf (s, static_cast<size_t> (at (0.05)), static_cast<size_t> (at (3.05))), 2)
               + " dBFS; RMS of the single note (held 3 s) " + fmt (rmsOf (s, singleFrom, singleFrom + static_cast<size_t> (at (3.0))), 2) + " dBFS";
    };
    out.file ("pair3_init_now", differs (true), listen, now, measured (now));
    out.file ("pair3_init_before", differs (false), listen, before, measured (before) + "; difference of the two files " + fmt (20.0 * std::log10 (factor), 3) + " dB");
}
} // namespace

TEST_CASE ("Render bside_audition pairs", "[.render]")
{
    Output out;
    out.dir = renderDir();
    if (out.dir != juce::File())
        out.dir.createDirectory();
    out.md << "# B-Side S4b audition: three comparison pairs\n\n"
              "Every pair is two or more files of the same length that differ in exactly one thing (stated per file). Compare them by switching between them at the same position. "
              "The text of this file states what differs and what to do; it does not say how anything sounds. All files are 48 kHz, 24 bit, stereo, rendered through the real processor "
              "(Init, output limiter at its defaults).\n\n"
              "Pair 1 (steal): the measured numbers are the largest sample-to-sample step of the output (either channel, full scale = 1.0) within 10 ms after each stealing key's note-on "
              "against the largest step in the second before it; the same two numbers are printed for the file without the stealing keys at the same times.\n\n";
    SECTION ("all")
    {
        out.md << "# Pair 1 - a voice steal against none\n\n";
        pair1 (out);
        out.md << "# Pair 2 - a soft note joining a held loud note, Shared against Per Note\n\n";
        pair2 (out);
        out.md << "# Pair 3 - Init before and after the kChipGain move\n\n";
        pair3 (out);
        if (out.dir != juce::File())
            out.dir.getChildFile ("PAIRS.md").replaceWithText (out.md);
    }
}
