// Black-box tests of the M2b polyphony behaviour, written from docs/architecture.md sections 2 and
// 3.2-3.4 only (not from the implementation). Everything is measured through audio: a vs::Part is
// driven with NoteEvents and the output is analysed (FFT lines, zero-crossing pitch, RMS, onset times,
// largest sample-to-sample step). Patch: one sine oscillator, filter wide open, no modulation,
// Vintage 0, fast ENV-2, so every voice is a pure tone of the same, known amplitude.
//
// Tolerances are stated next to each check. Where the specification is ambiguous the test uses the
// most natural reading and says so in a comment ("SPEC-AMBIGUITY").

#include <catch2/catch_test_macros.hpp>

#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "engine/NoteEvent.h"
#include "engine/Part.h"
#include "params/Mappings.h"
#include "params/PartParamList.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace
{
using vs::EventType;
using vs::NoteEvent;
using Events = std::vector<NoteEvent>;

constexpr double kFs = 48000.0;
constexpr int kBlock = 256;
constexpr int kMaxBlock = 512;
constexpr double kPi = 3.14159265358979323846;

// ---- event builders ----------------------------------------------------------------------------
NoteEvent on (int pos, int note, float vel = 0.8f)
{
    return { pos, EventType::NoteOn, static_cast<std::uint8_t> (note), vel };
}
NoteEvent off (int pos, int note) { return { pos, EventType::NoteOff, static_cast<std::uint8_t> (note), 0.0f }; }
NoteEvent pedal (int pos, bool down) { return { pos, EventType::SustainPedal, 0, down ? 1.0f : 0.0f }; }
NoteEvent wheel (int pos, float v) { return { pos, EventType::ModWheel, 0, v }; }
NoteEvent bend (int pos, float v) { return { pos, EventType::PitchBend, 0, v }; }
NoteEvent ctl (int pos, EventType t) { return { pos, t, 0, 0.0f }; }

double noteHz (int note) { return 440.0 * std::pow (2.0, (note - 69) / 12.0); }
double cents (double f, double ref) { return 1200.0 * std::log2 (f / ref); }
double semis (double f) { return 69.0 + 12.0 * std::log2 (f / 440.0); }

// ---- rig -----------------------------------------------------------------------------------------
struct Rig
{
    vs::Part part;
    vs::PartParams params;

    Rig()
    {
        part.setParams (params);
        part.prepare (kFs, kMaxBlock, 2, 4321u);
    }

    void apply()
    {
        part.setParams (params);
        part.reset();
    }

    int latency() const { return part.getLatencySamples(); }

    /** hook(startSample, params) runs before each host block and may change the parameters. */
    using Hook = std::function<void (int, vs::PartParams&)>;

    std::vector<float> render (int numSamples, Events events = {}, int blockSize = kBlock, const Hook& hook = {})
    {
        REQUIRE (blockSize >= 1);
        REQUIRE (blockSize <= kMaxBlock);
        std::stable_sort (events.begin(), events.end(), [] (const NoteEvent& a, const NoteEvent& b) { return a.offset < b.offset; });
        std::vector<float> out (static_cast<size_t> (numSamples));
        Events chunk;
        for (int start = 0; start < numSamples; start += blockSize)
        {
            const int len = std::min (blockSize, numSamples - start);
            REQUIRE (static_cast<size_t> (start + len) <= out.size());
            chunk.clear();
            for (auto e : events)
                if (e.offset >= start && e.offset < start + len)
                {
                    e.offset -= start;
                    chunk.push_back (e);
                }
            if (hook)
                hook (start, params);
            part.setParams (params);
            part.render (out.data() + start, len, chunk);
        }
        return out;
    }
};

/** One sine oscillator, filter open, no modulation, fast ENV-2, sustain 1, no velocity response. */
void sinePatch (vs::PartParams& p, float releaseSec = 0.05f)
{
    p.vco1Wave = 0; // sine
    p.sourceMix = 0.0f; // VCO-1 only
    p.vcfCutoff = 1.0f;
    p.vcfEnvMod = 0.0f;
    p.vcfResonance = 0.0f;
    p.vcfLfoMod = 0.0f;
    p.vcoModLfo = 0.0f;
    p.vcoModEnv = 0.0f;
    p.env2Attack = 0.0f; // 1 ms
    p.env2Decay = 0.5f;
    p.env2Sustain = 1.0f;
    p.env2Release = vs::map::envTimeFromSec (releaseSec);
    p.velToVca = 0.0f;
    p.velToVcf = 0.0f;
    p.modWheelDepth = 0.0f;
}

std::vector<float> slice (const std::vector<float>& x, int start, int len)
{
    REQUIRE (start >= 0);
    REQUIRE (static_cast<size_t> (start + len) <= x.size());
    return { x.begin() + start, x.begin() + start + len };
}

double rmsOf (const std::vector<float>& x, int start, int len)
{
    REQUIRE (start >= 0);
    REQUIRE (static_cast<size_t> (start + len) <= x.size());
    return vs::test::rms (x.data() + start, static_cast<size_t> (len));
}

double peakOf (const std::vector<float>& x, int start, int len)
{
    REQUIRE (static_cast<size_t> (start + len) <= x.size());
    double m = 0.0;
    for (int i = start; i < start + len; ++i)
        m = std::max (m, static_cast<double> (std::abs (x[static_cast<size_t> (i)])));
    return m;
}

double maxStep (const std::vector<float>& x, int start, int len)
{
    REQUIRE (start >= 1);
    REQUIRE (static_cast<size_t> (start + len) <= x.size());
    double m = 0.0;
    for (int i = start; i < start + len; ++i)
        m = std::max (m, static_cast<double> (std::abs (x[static_cast<size_t> (i)] - x[static_cast<size_t> (i - 1)])));
    return m;
}

bool allFinite (const std::vector<float>& x)
{
    return std::all_of (x.begin(), x.end(), [] (float s) { return std::isfinite (s); });
}

/** Zero-crossing pitch (semitones, MIDI scale) of a window. */
double pitchSemis (const std::vector<float>& x, int start, int len)
{
    return semis (vs::test::measureFrequency (slice (x, start, len), kFs));
}

/** Peak amplitude of a single sustained sine voice for the given patch (the unit of all step bounds). */
double singleVoiceAmp()
{
    Rig rig;
    sinePatch (rig.params);
    rig.apply();
    const auto out = rig.render (24000, { on (0, 69) });
    return peakOf (out, 12000, 12000);
}

/** Largest possible sample-to-sample step of a sum of sines with the given frequencies and amplitude each. */
double stepBound (const std::vector<double>& freqs, double amp)
{
    double s = 0.0;
    for (const double f : freqs)
        s += amp * 2.0 * std::sin (kPi * f / kFs);
    return s;
}

// ---- spectrum ------------------------------------------------------------------------------------
struct Peak
{
    double hz;
    double db; // relative to the strongest line
};

struct Spectrum
{
    std::vector<double> mag;
    double binHz = 0.0;
    double maxMag = 0.0;

    /** Hann window, 2^order samples. */
    Spectrum (const float* x, int order)
    {
        const auto n = static_cast<size_t> (1) << order;
        std::vector<float> buf (2 * n, 0.0f);
        for (size_t i = 0; i < n; ++i)
            buf[i] = x[i] * static_cast<float> (0.5 - 0.5 * std::cos (2.0 * kPi * static_cast<double> (i) / static_cast<double> (n)));
        juce::dsp::FFT fft (order);
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        mag.assign (buf.begin(), buf.begin() + static_cast<std::ptrdiff_t> (n / 2));
        binHz = kFs / static_cast<double> (n);
        for (size_t k = 2; k < mag.size(); ++k)
            maxMag = std::max (maxMag, mag[k]);
    }

    double dbOf (size_t k) const { return 20.0 * std::log10 (std::max (mag[k], 1.0e-12) / maxMag); }

    /** Local maxima within +-radius bins that are within dbBelow of the strongest line (log-parabolic interpolation). */
    std::vector<Peak> peaks (double dbBelow = 15.0, int radius = 4) const
    {
        std::vector<Peak> out;
        const auto r = static_cast<size_t> (radius);
        for (size_t k = 3 + r; k + r + 1 < mag.size(); ++k)
        {
            if (dbOf (k) < -dbBelow)
                continue;
            bool isMax = true;
            for (size_t j = 1; j <= r && isMax; ++j)
                isMax = mag[k] > mag[k - j] && mag[k] >= mag[k + j];
            if (! isMax)
                continue;
            const double a = dbOf (k - 1), b = dbOf (k), c = dbOf (k + 1);
            const double den = a - 2.0 * b + c;
            const double d = std::abs (den) > 1.0e-12 ? 0.5 * (a - c) / den : 0.0;
            out.push_back ({ (static_cast<double> (k) + d) * binHz, b });
        }
        return out;
    }

    /** Level (dB re the strongest line) of the strongest bin within +-2 bins of hz. */
    double levelAt (double hz) const
    {
        const auto k0 = static_cast<long> (std::llround (hz / binHz));
        double m = 0.0;
        for (long k = k0 - 2; k <= k0 + 2; ++k)
            if (k >= 0 && static_cast<size_t> (k) < mag.size())
                m = std::max (m, mag[static_cast<size_t> (k)]);
        return 20.0 * std::log10 (std::max (m, 1.0e-12) / maxMag);
    }
};

bool hasLine (const std::vector<Peak>& peaks, double hz, double tolCents)
{
    return std::any_of (peaks.begin(), peaks.end(), [&] (const Peak& p) { return std::abs (cents (p.hz, hz)) <= tolCents; });
}

/** The spectrum has exactly these notes as lines (and no others) within tolCents. */
void expectExactlyNotes (const Spectrum& sp, const std::vector<int>& notesExpected, double tolCents = 3.0)
{
    const auto peaks = sp.peaks();
    INFO ("lines found: " << peaks.size() << ", expected " << notesExpected.size());
    CHECK (peaks.size() == notesExpected.size());
    for (const int n : notesExpected)
    {
        INFO ("expected note " << n << " (" << noteHz (n) << " Hz)");
        CHECK (hasLine (peaks, noteHz (n), tolCents));
    }
}

/** Largest minus smallest pitch (cents) over sliding windows; also returns the deviation (half of it). */
double pitchDeviationCents (const std::vector<float>& x, int from, int to, int win, double refHz)
{
    double lo = 1.0e9, hi = -1.0e9;
    for (int s = from; s + win <= to; s += win / 2)
    {
        const double f = vs::test::measureFrequency (slice (x, s, win), kFs);
        if (f <= 0.0)
            continue;
        const double c = cents (f, refHz);
        lo = std::min (lo, c);
        hi = std::max (hi, c);
    }
    return 0.5 * (hi - lo);
}

constexpr int kSilentWindow = 4800;
bool isSilent (const std::vector<float>& x, int start, int len = kSilentWindow, double thr = 1.0e-4)
{
    return peakOf (x, start, len) < thr;
}

struct JuceEnv
{
    juce::ScopedJuceInitialiser_GUI init;
};
} // namespace

// =================================================================================================
// Poly
// =================================================================================================
TEST_CASE ("Spec Poly: N held notes sound as N distinct pitches", "[spec][poly]")
{
    // Lines are counted above -15 dB re the strongest: the VCF's nonlinearity adds a 3rd harmonic at about -23 dB.
    // Tolerance: 3 cents (FFT peak interpolation; lines are >= 10 bins apart); level spread 1.5 dB
    // (every voice has the same gain, leakage between lines is far below).
    for (const int n : { 1, 2, 3, 5, 8, 12, 16 })
    {
        INFO ("held notes: " << n);
        Rig rig;
        sinePatch (rig.params);
        rig.apply();
        Events ev;
        std::vector<int> notes;
        for (int i = 0; i < n; ++i)
        {
            notes.push_back (48 + 2 * i);
            ev.push_back (on (i * 200, notes.back()));
        }
        const auto out = rig.render (20000 + 32768, ev);
        REQUIRE (allFinite (out));
        const Spectrum sp (out.data() + 20000, 15);
        expectExactlyNotes (sp, notes);
        const auto peaks = sp.peaks();
        double lo = 0.0, hi = -200.0;
        for (const auto& p : peaks)
        {
            lo = std::min (lo, p.db);
            hi = std::max (hi, p.db);
        }
        CHECK (hi - lo < 1.5);
    }
}

TEST_CASE ("Spec Poly: releasing one key removes exactly that pitch", "[spec][poly]")
{
    Rig rig;
    sinePatch (rig.params);
    rig.apply();
    Events ev;
    std::vector<int> remaining;
    for (int i = 0; i < 6; ++i)
    {
        ev.push_back (on (i * 100, 50 + 3 * i));
        if (i != 2)
            remaining.push_back (50 + 3 * i);
    }
    ev.push_back (off (24000, 50 + 3 * 2));
    const auto out = rig.render (24000 + 12000 + 32768, ev);
    expectExactlyNotes (Spectrum (out.data() + 36000, 15), remaining);
}

TEST_CASE ("Spec Poly: re-pressing a sounding note does not add a second voice", "[spec][poly]")
{
    // Spec: "(1) a voice already sounding the same note (retrigger it)". Level must not double
    // (a second voice would add 6 dB); tolerance 1 dB. The single note-off must then end the note.
    Rig rig;
    sinePatch (rig.params);
    rig.apply();
    Events ev { on (0, 60), on (24000, 60), on (36000, 60), off (60000, 60) };
    const auto out = rig.render (60000 + 24000, ev);
    const double before = rmsOf (out, 12000, 10000);
    const double after1 = rmsOf (out, 30000, 4000);
    const double after2 = rmsOf (out, 44000, 14000);
    INFO ("before " << before << " after1 " << after1 << " after2 " << after2);
    CHECK (std::abs (vs::test::toDb (after1 / before)) < 1.0);
    CHECK (std::abs (vs::test::toDb (after2 / before)) < 1.0);
    expectExactlyNotes (Spectrum (out.data() + 44000, 14), { 60 });
    CHECK (isSilent (out, 60000 + 12000, 10000)); // one note-off silences it: no second voice hung around
}

namespace
{
/** 16 sine notes 48+2i pressed in the given order, one every 2000 samples from 1000. Returns the output. */
struct StealRun
{
    std::vector<float> out;
    int stealPos = 0;
};

StealRun runSteal (const std::vector<int>& pressOrderIdx, const Events& extra, int stealPos, const std::vector<int>& stealNotes,
                   float releaseSec, int stealGap = 0)
{
    Rig rig;
    sinePatch (rig.params, releaseSec);
    rig.apply();
    Events ev;
    int pos = 1000;
    for (const int i : pressOrderIdx)
    {
        ev.push_back (on (pos, 48 + 2 * i));
        pos += 2000;
    }
    for (const auto& e : extra)
        ev.push_back (e);
    int p = stealPos;
    for (const int n : stealNotes)
    {
        ev.push_back (on (p, n));
        p += stealGap;
    }
    StealRun r;
    r.stealPos = stealPos;
    r.out = rig.render (stealPos + stealGap * static_cast<int> (stealNotes.size()) + 6000 + 32768, ev);
    return r;
}

std::vector<int> iota16()
{
    std::vector<int> v;
    for (int i = 0; i < 16; ++i)
        v.push_back (i);
    return v;
}
} // namespace

TEST_CASE ("Spec Poly: 17th note steals the oldest held voice when none is releasing", "[spec][poly][steal]")
{
    // 16 notes pressed in ascending order, then a 17th: the OLDEST held (first pressed, i = 0) goes.
    {
        const auto r = runSteal (iota16(), {}, 50000, { 80 }, 0.05f);
        std::vector<int> expected;
        for (int i = 1; i < 16; ++i)
            expected.push_back (48 + 2 * i);
        expected.push_back (80);
        expectExactlyNotes (Spectrum (r.out.data() + r.stealPos + 6000, 15), expected);
    }
    // Pressed in descending order: the oldest is now i = 15, not the lowest note (age, not pitch).
    {
        std::vector<int> order = iota16();
        std::reverse (order.begin(), order.end());
        const auto r = runSteal (order, {}, 50000, { 80 }, 0.05f);
        std::vector<int> expected;
        for (int i = 0; i < 15; ++i)
            expected.push_back (48 + 2 * i);
        expected.push_back (80);
        expectExactlyNotes (Spectrum (r.out.data() + r.stealPos + 6000, 15), expected);
    }
}

TEST_CASE ("Spec Poly: 17th note steals the oldest voice in release before any held voice", "[spec][poly][steal]")
{
    // Release 4 s so the released voices are still sounding at the 17th note-on.
    // Note 58 (i = 5) is released first and also started earlier than note 66 (i = 9): under either
    // reading of "oldest voice in release" the stolen one is 58, never the oldest held note (48).
    const auto r = runSteal (iota16(), { off (40000, 58), off (45000, 66) }, 50000, { 80 }, 4.0f);
    std::vector<int> expected;
    for (int i = 0; i < 16; ++i)
        if (i != 5)
            expected.push_back (48 + 2 * i);
    expected.push_back (80);
    expectExactlyNotes (Spectrum (r.out.data() + r.stealPos + 6000, 15), expected, 4.0);
}

TEST_CASE ("Spec Poly: with two voices in release exactly one is stolen and no held voice", "[spec][poly][steal]")
{
    // SPEC-AMBIGUITY: "oldest voice in release" may mean longest in release or earliest started.
    // Release 66 (i = 9) first, then 58 (i = 5): the readings disagree, so only the certain part is checked:
    // one of the two releasing voices is gone, the oldest held note (48) survives, the new note sounds.
    const auto r = runSteal (iota16(), { off (40000, 66), off (45000, 58) }, 50000, { 80 }, 4.0f);
    const Spectrum sp (r.out.data() + r.stealPos + 6000, 15);
    const auto peaks = sp.peaks();
    CHECK (peaks.size() == 16);
    CHECK (hasLine (peaks, noteHz (48), 4.0));
    CHECK (hasLine (peaks, noteHz (80), 4.0));
    const bool has58 = hasLine (peaks, noteHz (58), 4.0);
    const bool has66 = hasLine (peaks, noteHz (66), 4.0);
    CHECK ((has58 != has66));
}

TEST_CASE ("Spec Poly: the most recently started note is never stolen", "[spec][poly][steal]")
{
    // 16 held, 17th at 50000, 18th 0.3 s later: the 17th must survive the second steal.
    const auto r = runSteal (iota16(), {}, 50000, { 80, 82 }, 0.05f, 14400);
    std::vector<int> expected;
    for (int i = 2; i < 16; ++i)
        expected.push_back (48 + 2 * i);
    expected.push_back (80);
    expected.push_back (82);
    expectExactlyNotes (Spectrum (r.out.data() + r.stealPos + 14400 + 6000, 15), expected);

    // Two notes pressed back to back (1 ms apart: the first steal fade is still running).
    const auto q = runSteal (iota16(), {}, 50000, { 80, 82 }, 0.05f, 48);
    const Spectrum sp (q.out.data() + q.stealPos + 48 + 6000, 15);
    const auto peaks = sp.peaks();
    CHECK (peaks.size() == 16);
    CHECK (hasLine (peaks, noteHz (80), 4.0));
    CHECK (hasLine (peaks, noteHz (82), 4.0));
}

TEST_CASE ("Spec Poly: a stolen note starts 3 ms later than an unstolen one", "[spec][poly][steal]")
{
    // The new note is 108 (4.2 kHz); the 16 or 15 held notes are 36..51 (< 190 Hz). A second difference
    // (x[n] - 2 x[n-1] + x[n-2]) suppresses the low notes by > 30 dB and isolates the new one. The onset
    // is the first sample where |y| exceeds 30 % of the steady maximum of y. Both runs share the same
    // measurement bias, so only the difference matters: spec 3 ms = 144 samples at 48 kHz.
    // Tolerance +-24 samples (0.5 ms).
    auto onset = [] (int heldCount) {
        Rig rig;
        sinePatch (rig.params);
        rig.apply();
        Events ev;
        for (int i = 0; i < heldCount; ++i)
            ev.push_back (on (i * 500, 36 + i));
        ev.push_back (on (30000, 108));
        const auto x = rig.render (36000, ev);
        std::vector<double> y (x.size(), 0.0);
        for (size_t i = 2; i < x.size(); ++i)
            y[i] = static_cast<double> (x[i]) - 2.0 * static_cast<double> (x[i - 1]) + static_cast<double> (x[i - 2]);
        double steady = 0.0;
        for (size_t i = 33000; i < 35000; ++i)
            steady = std::max (steady, std::abs (y[i]));
        REQUIRE (steady > 0.05);
        for (size_t i = 29900; i < y.size(); ++i)
            if (std::abs (y[i]) > 0.3 * steady)
                return static_cast<int> (i);
        return -1;
    };
    const int free15 = onset (15);
    const int stolen16 = onset (16);
    INFO ("onset with a free voice " << free15 << ", with a steal " << stolen16);
    REQUIRE (free15 > 0);
    REQUIRE (stolen16 > 0);
    const int delay = stolen16 - free15;
    CHECK (std::abs (delay - 144) <= 24);
}

TEST_CASE ("Spec Poly: stealing is click-free", "[spec][poly][steal]")
{
    // Sum of sines: the largest possible step is the sum of the per-tone maxima (computed from the
    // measured single-voice amplitude). A truncated voice would add a step of up to 0.64 x amplitude
    // (here the sines have amplitude ~0.64). Eight steal positions so that no sine sits at a zero
    // crossing in all of them. Bound: 1.3 x analytic sum + 0.02 (new note's 1 ms attack ramp).
    const double amp = singleVoiceAmp();
    std::vector<double> freqs;
    for (int n = 36; n <= 52; ++n)
        freqs.push_back (noteHz (n));
    const double bound = 1.3 * stepBound (freqs, amp) + 0.02;
    for (int k = 0; k < 8; ++k)
    {
        Rig rig;
        sinePatch (rig.params);
        rig.apply();
        Events ev;
        for (int i = 0; i < 16; ++i)
            ev.push_back (on (i * 500, 36 + i));
        const int pos = 30000 + k * 53;
        ev.push_back (on (pos, 52));
        const auto x = rig.render (pos + 8000, ev);
        const double worst = maxStep (x, pos - 100, 4000);
        INFO ("steal at " << pos << ": largest step " << worst << ", bound " << bound);
        CHECK (worst <= bound);
    }
}

// =================================================================================================
// Solo
// =================================================================================================
namespace
{
struct SoloStep
{
    bool isOn;
    int note;
    int sounding;       // note expected afterwards, -1 = silence
    bool continuous;    // the level must not dip (no retrigger)
};

void runSoloOrder (const std::vector<SoloStep>& steps)
{
    constexpr int spacing = 50000;
    Rig rig;
    sinePatch (rig.params);
    rig.params.assignMode = 2; // Solo
    rig.params.env2Attack = vs::map::envTimeFromSec (0.3f); // 0.3 s attack: a retrigger shows as a level dip
    rig.apply();
    Events ev;
    for (size_t i = 0; i < steps.size(); ++i)
    {
        const int pos = 1000 + static_cast<int> (i) * spacing;
        ev.push_back (steps[i].isOn ? on (pos, steps[i].note) : off (pos, steps[i].note));
    }
    const int total = 1000 + static_cast<int> (steps.size()) * spacing;
    const auto out = rig.render (total, ev);
    REQUIRE (allFinite (out));
    for (size_t i = 0; i < steps.size(); ++i)
    {
        const int pos = 1000 + static_cast<int> (i) * spacing;
        INFO ("step " << i << (steps[i].isOn ? " note-on " : " note-off ") << steps[i].note);
        if (steps[i].sounding < 0)
        {
            CHECK (isSilent (out, pos + 12000, 10000));
            continue;
        }
        // 12000 samples of >= 261 Hz: 65+ cycles, so the zero-crossing estimate is good to ~0.01 %.
        const double f = vs::test::measureFrequency (slice (out, pos + 12000, 12000), kFs);
        CHECK (std::abs (f - noteHz (steps[i].sounding)) < 0.3);
        if (steps[i].continuous)
        {
            // rms over 50 ms before and 50 ms (2400..4800 samples) after the change; >= 13 cycles each, ~0.2 dB error.
            const double before = rmsOf (out, pos - 2400, 2400);
            const double after = rmsOf (out, pos + 2400, 2400);
            INFO ("level before " << before << " after " << after);
            CHECK (std::abs (vs::test::toDb (after / before)) < 1.0);
        }
    }
}
} // namespace

TEST_CASE ("Spec Solo: last-note priority and return without retrigger (orders)", "[spec][solo]")
{
    SECTION ("60 64 67 72, release 72 64 67")
    {
        runSoloOrder ({ { true, 60, 60, false }, { true, 64, 64, false }, { true, 67, 67, false }, { true, 72, 72, false },
                        { false, 72, 67, true }, { false, 64, 67, true }, { false, 67, 60, true }, { false, 60, -1, false } });
    }
    SECTION ("60 64 67, release 60 67 64")
    {
        runSoloOrder ({ { true, 60, 60, false }, { true, 64, 64, false }, { true, 67, 67, false },
                        { false, 60, 67, true }, { false, 67, 64, true }, { false, 64, -1, false } });
    }
    SECTION ("most recently pressed remaining key wins, not the highest or lowest: 60 67 64, release 64 67")
    {
        runSoloOrder ({ { true, 60, 60, false }, { true, 67, 67, false }, { true, 64, 64, false },
                        { false, 64, 67, true }, { false, 67, 60, true }, { false, 60, -1, false } });
    }
}

TEST_CASE ("Spec Solo: a new note retriggers the envelopes, a return to a held note does not", "[spec][solo]")
{
    // ENV-2: 1 ms attack, 0.2 s decay to sustain 0.3. After 2 s the level sits at the sustain.
    // A retrigger sends the envelope up again: rms 10..30 ms after the new note is >= 1.5x the sustain
    // level. Returning to a held note (release of the top key) must NOT: level <= 1.1x sustain.
    Rig rig;
    sinePatch (rig.params);
    rig.params.assignMode = 2;
    rig.params.env2Sustain = 0.3f;
    rig.params.env2Decay = vs::map::envTimeFromSec (0.2f); // 0.2 s
    rig.apply();
    const auto out = rig.render (60000 * 3, { on (0, 60), on (96000, 64), off (156000, 64) });
    const double sustain = rmsOf (out, 90000, 4800);
    const double afterNew = rmsOf (out, 96000 + 480, 960);
    const double afterReturn = rmsOf (out, 156000 + 480, 960);
    INFO ("sustain " << sustain << " after new note " << afterNew << " after return " << afterReturn);
    CHECK (afterNew >= 1.5 * sustain);
    CHECK (afterReturn <= 1.1 * sustain);
}

// =================================================================================================
// Unison
// =================================================================================================
namespace
{
std::vector<float> renderUnison (int voicesIdx, float detune, int note, int order = 17, int lead = 20000, float vel = 0.8f)
{
    Rig rig;
    sinePatch (rig.params);
    rig.params.assignMode = 1;
    rig.params.unisonVoices = voicesIdx;
    rig.params.unisonDetune = detune;
    rig.apply();
    return rig.render (lead + (1 << order), { on (0, note, vel) });
}
} // namespace

TEST_CASE ("Spec Unison: 2/4/8/16 voices sound for one key, spread equals the detune setting", "[spec][unison]")
{
    // Note 81 (880 Hz): the lines are at least 4 bins apart in a 2^17 Hann FFT (n = 16, 50 ct: 1.7 Hz = 4.6 bins).
    // Peaks are local maxima within +-2 bins above -20 dB (Hann sidelobes are below -31 dB).
    // SPEC-AMBIGUITY: the spec does not say how voices are distributed inside the spread; the count
    // test assumes distinct, roughly even lines, the spread test uses only the outermost lines.
    const double f0 = noteHz (81);
    struct Case { int idx; int voices; float detune; };
    for (const auto c : { Case { 0, 2, 0.2f }, Case { 0, 2, 0.6f }, Case { 0, 2, 1.0f }, Case { 1, 4, 1.0f }, Case { 2, 8, 1.0f },
                          Case { 3, 16, 1.0f } })
    {
        INFO ("voices " << c.voices << " detune " << c.detune);
        const auto out = renderUnison (c.idx, c.detune, 81);
        REQUIRE (allFinite (out));
        const Spectrum sp (out.data() + 20000, 17);
        const auto peaks = sp.peaks (15.0, 2);
        CHECK (static_cast<int> (peaks.size()) == c.voices);
        REQUIRE (! peaks.empty());
        const double lo = peaks.front().hz, hi = peaks.back().hz;
        const double spread = cents (hi, lo);
        const double centre = 0.5 * (cents (hi, f0) + cents (lo, f0));
        INFO ("spread " << spread << " ct (expected " << 50.0 * static_cast<double> (c.detune) << "), centre offset " << centre << " ct");
        // Tolerance 1.5 ct on the spread (interpolation bias of the outer lines <= 0.2 ct), 1 ct on symmetry.
        CHECK (std::abs (spread - 50.0 * static_cast<double> (c.detune)) < 1.5);
        CHECK (std::abs (centre) < 1.0);
    }
    // Detune 0: all voices at the same pitch -> one line.
    {
        const auto out = renderUnison (3, 0.0f, 81);
        const auto peaks = Spectrum (out.data() + 20000, 17).peaks (15.0, 2);
        CHECK (peaks.size() == 1);
    }
}

TEST_CASE ("Spec Unison: loudness stays roughly constant across voice counts", "[spec][unison]")
{
    // 1/sqrt(n) per voice keeps the POWER constant for uncorrelated (detuned) voices. Reference: one
    // voice (Solo). Detune 1.0 (50 ct) so the voices drift apart in phase quickly.
    // Tolerance +-1.5 dB: the residual coherent cross terms average out only to a few percent, and the
    // spec says "roughly".
    // SPEC-AMBIGUITY: with detune 0 the voices are coherent and the sum is sqrt(n) louder; the spec
    // says nothing about that case, so it is not tested.
    // The detuned voices form a beat pattern (evenly spaced lines make a pulse train) whose period is
    // 1 / line spacing; at detune 1.0 (50 ct) and 16 voices that is ~1.2 s, so rms is taken over 6 s.
    Rig solo;
    sinePatch (solo.params);
    solo.params.assignMode = 2;
    solo.apply();
    const auto ref = solo.render (24000 + 288000, { on (0, 69) });
    const double refRms = rmsOf (ref, 24000, 288000);
    for (const int idx : { 0, 1, 2, 3 })
    {
        const auto out = renderUnison (idx, 1.0f, 69, 19, 24000);
        const double r = rmsOf (out, 24000, 288000);
        INFO ("voices " << (2 << idx) << " rms " << r << " reference " << refRms);
        CHECK (std::abs (vs::test::toDb (r / refRms)) < 1.5);
    }
}

TEST_CASE ("Spec Unison: monophonic, last-note priority", "[spec][unison]")
{
    // Detune 0.3 = 15 ct total, so every line of note N lies within +-7.5 ct (+ interpolation) of N.
    Rig rig;
    sinePatch (rig.params);
    rig.params.assignMode = 1;
    rig.params.unisonVoices = 1; // 4 voices
    rig.apply();
    const auto out = rig.render (24000 + 40000 + 100000 + 40000 + 40000, { on (0, 69), on (24000, 72), off (64000, 72), off (164000, 69) });
    auto allNear = [&] (int start, int note) {
        const Spectrum sp (out.data() + start, 16);
        const auto peaks = sp.peaks (15.0, 2);
        CHECK (! peaks.empty());
        for (const auto& p : peaks)
            CHECK (std::abs (cents (p.hz, noteHz (note))) < 12.0);
    };
    // Window 1: only 72 sounds (60000..) -- no 69 lines
    {
        const Spectrum sp (out.data() + 28000, 15);
        const auto peaks = sp.peaks (15.0, 2);
        CHECK (! peaks.empty());
        for (const auto& p : peaks)
            CHECK (std::abs (cents (p.hz, noteHz (72))) < 12.0);
    }
    // After 72 is released: back to 69 (held), still monophonic.
    allNear (84000, 69);
    // After 69 is released: silent.
    CHECK (isSilent (out, 164000 + 12000, 10000));
}

// =================================================================================================
// Assign mode / unison voice change
// =================================================================================================
TEST_CASE ("Spec: changing assign mode or unison count releases all voices, click-free, and notes keep working", "[spec][modechange]")
{
    const double amp = singleVoiceAmp();
    constexpr int change = 25600; // a multiple of the 256-sample host block
    struct Case
    {
        const char* name;
        int modeBefore, uniIdxBefore, modeAfter, uniIdxAfter;
        int linesAfter;
    };
    const Case cases[] { { "Poly -> Solo", 0, 1, 2, 1, 1 },     { "Poly -> Unison(4)", 0, 1, 1, 1, 4 },
                         { "Solo -> Poly", 2, 1, 0, 1, 1 },     { "Unison 4 -> 8", 1, 1, 1, 2, 8 },
                         { "Unison 8 -> 2", 1, 2, 1, 0, 2 },    { "Unison -> Solo", 1, 1, 2, 1, 1 } };
    for (const auto& c : cases)
    {
        INFO (c.name);
        Rig rig;
        sinePatch (rig.params);
        rig.params.assignMode = c.modeBefore;
        rig.params.unisonVoices = c.uniIdxBefore;
        rig.params.unisonDetune = 0.6f;
        rig.apply();
        Events ev;
        std::vector<double> freqs;
        const bool polyBefore = c.modeBefore == 0;
        const int nHeld = polyBefore ? 5 : 1;
        for (int i = 0; i < nHeld; ++i)
        {
            ev.push_back (on (i * 100, 48 + 2 * i));
            freqs.push_back (noteHz (48 + 2 * i));
        }
        for (int i = 0; i < nHeld; ++i)
            ev.push_back (off (40000, 48 + 2 * i)); // keys come up well after the change
        ev.push_back (on (60000, 81));
        const int total = 60000 + 20000 + (1 << 17);
        const auto out = rig.render (total, ev, kBlock, [&] (int start, vs::PartParams& p) {
            if (start == change)
            {
                p.assignMode = c.modeAfter;
                p.unisonVoices = c.uniIdxAfter;
            }
        });
        REQUIRE (allFinite (out));
        // The keys are still physically down at the change; all voices must have been released: silence
        // from 0.2 s (release 50 ms + decimator latency) after the change up to the note-offs.
        CHECK (isSilent (out, change + 9600, 6000));
        // No click: the largest step in the release is bounded by the sum of the sounding tones.
        const double sounding = polyBefore ? 1.0 : std::sqrt (static_cast<double> (2 << c.uniIdxBefore));
        double bound = 0.0;
        if (polyBefore)
            bound = stepBound (freqs, amp);
        else
            bound = sounding * amp * 2.0 * std::sin (kPi * freqs[0] / kFs) * 1.5; // unison: coherent worst case
        bound = 1.3 * bound + 0.02;
        const double worst = maxStep (out, change - 100, 8000);
        INFO ("largest step " << worst << " bound " << bound);
        CHECK (worst <= bound);
        // Subsequent notes work, with the new mode's voice count.
        const Spectrum sp (out.data() + 60000 + 20000, 17);
        const auto peaks = sp.peaks (15.0, 2);
        CHECK (static_cast<int> (peaks.size()) == c.linesAfter);
        for (const auto& p : peaks)
            CHECK (std::abs (cents (p.hz, noteHz (81))) < 30.0);
    }
}

// =================================================================================================
// Sustain pedal
// =================================================================================================
TEST_CASE ("Spec Pedal: key releases are deferred while down, applied at pedal up (Poly)", "[spec][pedal]")
{
    Rig rig;
    sinePatch (rig.params);
    rig.apply();
    // Notes pressed before and after the pedal goes down; all note-offs while it is down.
    const auto out = rig.render (130000, { on (0, 60), pedal (2000, true), on (3000, 64), off (24000, 60), off (30000, 64), pedal (72000, false) });
    // While the pedal is down both notes keep sounding at full level.
    expectExactlyNotes (Spectrum (out.data() + 36000, 15), { 60, 64 });
    CHECK (rmsOf (out, 60000, 8000) > 0.3);
    // After the pedal-up both are released (50 ms release).
    CHECK (isSilent (out, 72000 + 12000, 20000));
}

TEST_CASE ("Spec Pedal: keys still held at pedal-up keep sounding", "[spec][pedal]")
{
    Rig rig;
    sinePatch (rig.params);
    rig.apply();
    const auto out = rig.render (130000, { pedal (0, true), on (500, 60), on (1000, 64), off (24000, 64), pedal (60000, false) });
    expectExactlyNotes (Spectrum (out.data() + 30000, 15), { 60, 64 });         // 64 held by the pedal
    expectExactlyNotes (Spectrum (out.data() + 70000 + 12000, 15), { 60 });     // 64 gone, 60 still physically held
    CHECK (rmsOf (out, 100000, 20000) > 0.3);
}

TEST_CASE ("Spec Pedal: Solo and Unison hold the sounding note", "[spec][pedal]")
{
    for (const int mode : { 1, 2 })
    {
        INFO ((mode == 1 ? "Unison" : "Solo"));
        Rig rig;
        sinePatch (rig.params);
        rig.params.assignMode = mode;
        rig.apply();
        // Pedal down, 60 then 64 pressed, both released: 64 is the sounding note and must be held.
        // Then pedal up while nothing is held: silence.
        const auto out = rig.render (200000, { pedal (0, true), on (500, 60), on (30000, 64), off (50000, 64), off (51000, 60), pedal (120000, false) });
        {
            const Spectrum sp (out.data() + 70000, 15);
            const auto peaks = sp.peaks (15.0, 2);
            CHECK (! peaks.empty());
            for (const auto& p : peaks)
                CHECK (std::abs (cents (p.hz, noteHz (64))) < 12.0);
        }
        CHECK (rmsOf (out, 100000, 10000) > 0.3);
        CHECK (isSilent (out, 120000 + 12000, 20000));
    }
    // A key still held at pedal up takes over (releases are applied at pedal-up).
    {
        Rig rig;
        sinePatch (rig.params);
        rig.params.assignMode = 2;
        rig.apply();
        const auto out = rig.render (200000, { pedal (0, true), on (500, 60), on (30000, 64), off (50000, 64), pedal (120000, false) });
        CHECK (std::abs (pitchSemis (out, 80000, 20000) - 64.0) < 0.02);        // held by the pedal
        CHECK (std::abs (pitchSemis (out, 140000, 20000) - 60.0) < 0.02);       // 60 still down: back to it
    }
}

TEST_CASE ("Spec Pedal: AllNotesOff releases everything with the pedal down and clears the pedal", "[spec][pedal][panic]")
{
    for (const int mode : { 0, 1, 2 })
    {
        INFO ("assign mode " << mode);
        Rig rig;
        sinePatch (rig.params);
        rig.params.assignMode = mode;
        rig.apply();
        const auto out = rig.render (120000, { pedal (0, true), on (500, 60), on (600, 64), off (10000, 60), ctl (24000, EventType::AllNotesOff),
                                               on (48000, 67), off (60000, 67) });
        CHECK (rmsOf (out, 20000, 3000) > 0.05);
        CHECK (isSilent (out, 24000 + 12000, 10000)); // everything released, key 64 never came up
        // The pedal is cleared: a later note-off is not deferred.
        CHECK (rmsOf (out, 52000, 6000) > 0.05);
        CHECK (isSilent (out, 60000 + 12000, 20000));
    }
}

TEST_CASE ("Spec Pedal: ResetControllers clears bend, wheel and pedal", "[spec][pedal][panic]")
{
    // Bend and pedal.
    {
        Rig rig;
        sinePatch (rig.params);
        rig.params.bendRange = 2;
        rig.apply();
        const auto out = rig.render (150000, { bend (0, 1.0f), pedal (0, true), on (100, 69), off (30000, 69), ctl (60000, EventType::ResetControllers),
                                               on (90000, 69), off (110000, 69) });
        // Bent up 2 semitones and sustained by the pedal before the reset.
        CHECK (std::abs (pitchSemis (out, 40000, 12000) - 71.0) < 0.02);
        // The reset releases the note held only by the pedal.
        CHECK (isSilent (out, 60000 + 12000, 10000));
        // The next note is not bent, and its note-off is not deferred (pedal cleared).
        CHECK (std::abs (pitchSemis (out, 95000, 12000) - 69.0) < 0.02);
        CHECK (isSilent (out, 110000 + 12000, 20000));
    }
    // Wheel: vibrato present before, gone after.
    {
        Rig rig;
        sinePatch (rig.params);
        rig.params.lfoWave = 0;
        rig.params.lfoRate = vs::map::lfoRateFromHz (2.0f);
        rig.params.modWheelDepth = 0.5f;
        rig.apply();
        const auto out = rig.render (200000, { wheel (0, 1.0f), on (100, 81), ctl (100000, EventType::ResetControllers) });
        const double before = pitchDeviationCents (out, 20000, 90000, 480, noteHz (81));
        const double after = pitchDeviationCents (out, 120000, 200000 - 100, 480, noteHz (81));
        INFO ("deviation before " << before << " ct, after " << after << " ct");
        CHECK (before > 100.0);
        CHECK (after < 15.0); // zero-crossing noise floor of a 480-sample window at 880 Hz is ~8 ct
    }
}

TEST_CASE ("Spec: AllSoundOff silences the voices immediately", "[spec][panic]")
{
    Rig rig;
    sinePatch (rig.params, 4.0f); // 4 s release: only an immediate cut can silence it
    rig.apply();
    const auto out = rig.render (60000, { on (0, 60), on (0, 64), ctl (30000, EventType::AllSoundOff) });
    // Within 10 ms (decimator latency plus a short fade) nothing is left.
    CHECK (isSilent (out, 30000 + rig.latency() + 480, 12000));
}

TEST_CASE ("Spec Pedal: event-buffer overflow with the pedal down leaves no note sounding", "[spec][pedal][processor]")
{
    // Spec section 2: on overflow the remaining events of the block are dropped and an AllNotesOff is
    // appended, which also clears the pedal. Here the pedal-up and the note-offs are in the dropped part.
    for (const int mode : { 0, 1, 2 })
    {
        INFO ("assign mode " << mode);
        JuceEnv env;
        vs::VintageSynthProcessor proc;
        constexpr double sr = 48000.0;
        constexpr int block = 512;
        proc.getState().getParameter ("lower.assign.mode")->setValueNotifyingHost (static_cast<float> (mode) / 2.0f);
        proc.setPlayConfigDetails (0, 2, sr, block);
        proc.prepareToPlay (sr, block);

        juce::MidiBuffer flood;
        flood.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
        flood.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 1);
        flood.addEvent (juce::MidiMessage::noteOff (1, 60), 2);
        for (int i = 0; i < 2600; ++i)
            flood.addEvent (juce::MidiMessage::controllerEvent (1, 1, 10), 3 + i % 300); // mod wheel flood
        flood.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 450);             // dropped
        juce::AudioBuffer<float> buffer (2, block);
        proc.processBlock (buffer, flood);

        juce::MidiBuffer none;
        float tailPeak = 0.0f;
        for (int b = 0; b < 96000 / block; ++b)
        {
            proc.processBlock (buffer, none);
            if (b > 40)
                tailPeak = std::max (tailPeak, buffer.getMagnitude (0, 0, block));
        }
        CHECK (tailPeak < 1.0e-5f);
    }
    // Control: the same sequence without the flood keeps the note held by the pedal.
    {
        JuceEnv env;
        vs::VintageSynthProcessor proc;
        constexpr int block = 512;
        proc.setPlayConfigDetails (0, 2, 48000.0, block);
        proc.prepareToPlay (48000.0, block);
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
        m.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 1);
        m.addEvent (juce::MidiMessage::noteOff (1, 60), 2);
        juce::AudioBuffer<float> buffer (2, block);
        proc.processBlock (buffer, m);
        juce::MidiBuffer none;
        float peak = 0.0f;
        for (int b = 0; b < 96000 / block; ++b)
        {
            proc.processBlock (buffer, none);
            if (b > 40)
                peak = std::max (peak, buffer.getMagnitude (0, 0, block));
        }
        CHECK (peak > 0.01f);
    }
}

// =================================================================================================
// Portamento
// =================================================================================================
namespace
{
/** Fraction of the interval from -> to covered at tSec after the second note-on (window 25 ms). */
double glideFraction (const std::vector<float>& x, int onPos, int latency, double tSec, double from, double to)
{
    const int centre = onPos + latency + static_cast<int> (std::lround (tSec * kFs));
    const double st = pitchSemis (x, centre - 600, 1200);
    return (st - from) / (to - from);
}
} // namespace

TEST_CASE ("Spec Portamento: time 0 is off", "[spec][porta]")
{
    for (const int mode : { 0, 2 })
    {
        INFO ("assign mode " << mode);
        Rig rig;
        sinePatch (rig.params);
        rig.params.assignMode = mode;
        rig.params.portaTime = 0.0f;
        rig.apply();
        const auto out = rig.render (60000, { on (0, 60), on (10000, 72) });
        // 10..35 ms after the second note: already at the target (tolerance 0.05 semitone = 0.3 %).
        CHECK (std::abs (pitchSemis (out, 10000 + rig.latency() + 480, 1200) - 72.0) < 0.05);
    }
}

TEST_CASE ("Spec Portamento: 99 % of the interval at the set time, RC shape (up and down)", "[spec][porta]")
{
    // Pitch in semitones over time follows from + (to - from) (1 - 0.01^(t/T)). Measured with a 25 ms
    // zero-crossing window centred on t (after the reported latency). Tolerance: 0.03 of the interval
    // (window averaging over a ~17 % relative change of the remaining distance and zero-crossing noise).
    struct Dir { int from, to; };
    for (const double T : { 0.2, 0.5, 1.0 })
        for (const auto d : { Dir { 69, 81 }, Dir { 81, 69 } })
            for (const int mode : { 0, 2 }) // Poly, Solo
            {
                INFO ("T " << T << " from " << d.from << " to " << d.to << " mode " << mode);
                Rig rig;
                sinePatch (rig.params);
                rig.params.assignMode = mode;
                rig.params.portaTime = vs::map::portaFromSec (static_cast<float> (T));
                rig.apply();
                const int onPos = 30000;
                const auto out = rig.render (onPos + static_cast<int> (3.0 * T * kFs) + 6000, { on (0, d.from), off (20000, d.from), on (onPos, d.to) });
                for (const double r : { 0.25, 0.5, 1.0, 2.0 })
                {
                    const double expected = 1.0 - std::pow (0.01, r);
                    const double got = glideFraction (out, onPos, rig.latency(), r * T, d.from, d.to);
                    INFO ("t = " << r << " T: fraction " << got << ", expected " << expected);
                    CHECK (std::abs (got - expected) < 0.03);
                }
            }
}

TEST_CASE ("Spec Portamento: Solo glides from the held note (mode Always)", "[spec][porta]")
{
    Rig rig;
    sinePatch (rig.params);
    rig.params.assignMode = 2;
    rig.params.portaTime = vs::map::portaFromSec (0.5f);
    rig.apply();
    const auto out = rig.render (70000, { on (0, 60), on (30000, 72) });
    // T = 0.5 s: at t = 0.25 s (0.5 T) the fraction is 1 - 0.01^0.5 = 0.9, at t = 0.5 s it is 0.99.
    CHECK (std::abs (glideFraction (out, 30000, rig.latency(), 0.25, 60, 72) - 0.9) < 0.03);
    CHECK (std::abs (glideFraction (out, 30000, rig.latency(), 0.5, 60, 72) - 0.99) < 0.03);
}

TEST_CASE ("Spec Portamento: Legato glides only when a key is already held", "[spec][porta]")
{
    // Legato mode (portaMode = 1). With a key held the glide follows the RC curve (0.25 T -> 0.684).
    // With no key held the pitch is already at the target within 0.05 semitone at 0.1 T.
    const float T = 1.0f;
    for (const int mode : { 0, 2 })
    {
        INFO ("assign mode " << mode);
        // Key held.
        {
            Rig rig;
            sinePatch (rig.params);
            rig.params.assignMode = mode;
            rig.params.portaMode = 1;
            rig.params.portaTime = vs::map::portaFromSec (T);
            rig.apply();
            // Poly: the held 60 is released 10 ms after 72 starts, so only the gliding voice remains.
            const auto out = rig.render (100000, { on (0, 60), on (30000, 72), off (30500, 60) });
            const double got = glideFraction (out, 30000, rig.latency(), 0.25, 60, 72);
            INFO ("with held key: fraction " << got);
            CHECK (std::abs (got - 0.684) < 0.03);
        }
        // No key held (previous key released and silent).
        {
            Rig rig;
            sinePatch (rig.params);
            rig.params.assignMode = mode;
            rig.params.portaMode = 1;
            rig.params.portaTime = vs::map::portaFromSec (T);
            rig.apply();
            const auto out = rig.render (60000, { on (0, 60), off (10000, 60), on (30000, 72) });
            const double st = pitchSemis (out, 30000 + rig.latency() + 2400 - 600, 1200);
            INFO ("without held key: pitch " << st);
            CHECK (std::abs (st - 72.0) < 0.05);
        }
    }
}

TEST_CASE ("Spec Portamento: in Poly a new voice glides from the previously played note", "[spec][porta]")
{
    // Previous note released and silent; the new voice starts at the previous pitch (60), not its own.
    Rig rig;
    sinePatch (rig.params);
    rig.params.portaTime = vs::map::portaFromSec (0.5f);
    rig.apply();
    const auto out = rig.render (60000, { on (0, 60), off (10000, 60), on (30000, 72) });
    // 10..35 ms after the onset (centre 22 ms = 0.044 T) the pitch is 60 + 12 (1 - 0.01^0.044) = 62.2, far from 72.
    const double early = pitchSemis (out, 30000 + rig.latency() + 480, 1200);
    INFO ("pitch 10..35 ms after onset " << early);
    CHECK (early < 63.5);
    CHECK (early > 60.0);
    CHECK (std::abs (glideFraction (out, 30000, rig.latency(), 0.25, 60, 72) - 0.9) < 0.03); // t = 0.5 T
}

// =================================================================================================
// Velocity
// =================================================================================================
TEST_CASE ("Spec Velocity: VCA gain = 1 - amount x (1 - velocity)", "[spec][velocity]")
{
    // rms over 24000 samples (~220 cycles) is exact to ~0.2 %; tolerance 2 % of the expected ratio.
    auto level = [] (float amount, float vel) {
        Rig rig;
        sinePatch (rig.params);
        rig.params.velToVca = amount;
        rig.apply();
        const auto out = rig.render (48000, { on (0, 69, vel) });
        return rmsOf (out, 20000, 24000);
    };
    const double ref = level (0.0f, 1.0f);
    struct C { float a, v; };
    for (const auto c : { C { 0.0f, 0.2f }, C { 0.0f, 0.7f }, C { 0.5f, 0.2f }, C { 0.5f, 1.0f }, C { 1.0f, 0.25f }, C { 1.0f, 0.5f }, C { 0.3f, 0.6f }, C { 1.0f, 0.8f } })
    {
        const double expected = 1.0 - static_cast<double> (c.a) * (1.0 - static_cast<double> (c.v));
        const double got = level (c.a, c.v) / ref;
        INFO ("amount " << c.a << " velocity " << c.v << ": gain " << got << ", expected " << expected);
        CHECK (std::abs (got / expected - 1.0) < 0.02);
    }
}

TEST_CASE ("Spec Velocity: VCF envelope depth is scaled by 1 - amount x (1 - velocity)", "[spec][velocity]")
{
    // A saw through the 24 dB filter at a low cutoff with a strong ENV-1 sweep (sustain 1): brightness
    // (energy 1..6 kHz) depends monotonically on the envelope depth. Black-box equivalence: a note with
    // velocity v and amount a at ENV depth m must have the same brightness as a note at velocity 1 with
    // depth m x (1 - a (1 - v)) (depth in octaves is linear in the slider). Tolerance 0.5 dB.
    auto brightnessDb = [] (float amount, float vel, float envMod) {
        Rig rig;
        sinePatch (rig.params);
        rig.params.vco1Wave = 2; // saw
        rig.params.vcfCutoff = 0.35f;
        rig.params.vcfKeyFollow = 0.0f;
        rig.params.vcfSlope = 1;
        rig.params.vcfEnvSource = 0;
        rig.params.vcfEnvMod = envMod;
        rig.params.env1Attack = 0.0f;
        rig.params.env1Sustain = 1.0f;
        rig.params.env1Decay = 0.5f;
        rig.params.velToVcf = amount;
        rig.apply();
        const auto out = rig.render (20000 + 32768, { on (0, 45, vel) });
        const Spectrum sp (out.data() + 20000, 15);
        double e = 0.0;
        for (size_t k = static_cast<size_t> (1000.0 / sp.binHz); k < static_cast<size_t> (6000.0 / sp.binHz); ++k)
            e += sp.mag[k] * sp.mag[k];
        return 10.0 * std::log10 (e + 1.0e-12);
    };
    struct C { float a, v; };
    const float m = 0.6f;
    for (const auto c : { C { 1.0f, 0.5f }, C { 0.5f, 0.2f }, C { 0.0f, 0.2f }, C { 1.0f, 0.2f }, C { 0.4f, 0.75f } })
    {
        const float scale = 1.0f - c.a * (1.0f - c.v);
        const double got = brightnessDb (c.a, c.v, m);
        const double ref = brightnessDb (0.0f, 1.0f, m * scale);
        INFO ("amount " << c.a << " velocity " << c.v << " scale " << scale << ": brightness " << got << " dB, reference " << ref << " dB");
        CHECK (std::abs (got - ref) < 0.5);
        if (scale < 0.9f)
            CHECK (std::abs (brightnessDb (0.0f, 1.0f, m) - ref) > 3.0); // the test can tell the difference
    }
    // The VCA is not affected (velToVca = 0): amount 1 at velocity 0.2 keeps the overall level range of the
    // reference render, checked implicitly above through the energy comparison.
}

// =================================================================================================
// Bend, mod wheel
// =================================================================================================
TEST_CASE ("Spec Bend range: 0..12 semitones, full bend gives exactly the range", "[spec][bend]")
{
    // Tolerance 1 cent (the zero-crossing estimate over 0.75 s is good to ~0.05 cent, the exp2 approximation to 0.01).
    for (const int range : { 0, 1, 2, 5, 7, 12 })
        for (const float b : { 1.0f, -1.0f, 0.5f, 0.0f })
        {
            Rig rig;
            sinePatch (rig.params);
            rig.params.bendRange = range;
            rig.apply();
            const auto out = rig.render (72000, { bend (0, b), on (100, 69) });
            const double f = vs::test::measureFrequency (slice (out, 24000, 36000), kFs);
            const double expectedCents = 100.0 * static_cast<double> (range) * static_cast<double> (b);
            INFO ("range " << range << " bend " << b << ": " << cents (f, 440.0) << " ct, expected " << expectedCents);
            CHECK (std::abs (cents (f, 440.0) - expectedCents) < 1.0);
        }
}

TEST_CASE ("Spec Bend: the default range is 2 semitones, and a bend event moves a sounding note", "[spec][bend]")
{
    Rig rig;
    sinePatch (rig.params);
    rig.apply();
    const auto out = rig.render (96000, { on (0, 69), bend (40000, 1.0f), bend (70000, -1.0f) });
    CHECK (std::abs (cents (vs::test::measureFrequency (slice (out, 20000, 18000), kFs), 440.0)) < 1.0);
    CHECK (std::abs (cents (vs::test::measureFrequency (slice (out, 50000, 18000), kFs), 440.0) - 200.0) < 1.0);
    CHECK (std::abs (cents (vs::test::measureFrequency (slice (out, 78000, 18000), kFs), 440.0) + 200.0) < 1.0);
}

namespace
{
/** Vibrato deviation (cents) of note 81 with slider/wheel/target settings; LFO 2 Hz sine, no delay. */
double vibrato (float slider, float wheelDepth, float wheelPos, int target = 1)
{
    Rig rig;
    sinePatch (rig.params);
    rig.params.lfoWave = 0;
    rig.params.lfoRate = vs::map::lfoRateFromHz (2.0f);
    rig.params.lfoDelay = 0.0f;
    rig.params.vcoModLfo = slider;
    rig.params.modWheelDepth = wheelDepth;
    rig.params.vcoModTarget = target;
    rig.apply();
    const auto out = rig.render (130000, { wheel (0, wheelPos), on (100, 81) });
    return pitchDeviationCents (out, 24000, 130000, 480, noteHz (81));
}
} // namespace

TEST_CASE ("Spec Mod wheel: adds wheel x depth to the VCO LFO mod amount", "[spec][wheel]")
{
    // Equivalence with the slider: wheel w at depth d on top of slider s must give the vibrato of a
    // slider at s + w d with the wheel at 0 ("same taper"). Tolerance 3 % of the deviation (both
    // renders are measured identically). SPEC-AMBIGUITY: the cap at the panel maximum is in
    // Mappings.h but not in the spec text, so sums above 1 are not tested.
    struct C { float s, d, w; };
    for (const auto c : { C { 0.0f, 0.3f, 1.0f }, C { 0.2f, 0.4f, 0.5f }, C { 0.0f, 0.6f, 0.5f }, C { 0.3f, 1.0f, 0.0f } })
    {
        const float eff = c.s + c.w * c.d;
        const double viaWheel = vibrato (c.s, c.d, c.w);
        const double viaSlider = vibrato (eff, 0.0f, 0.0f);
        INFO ("slider " << c.s << " depth " << c.d << " wheel " << c.w << ": " << viaWheel << " ct, slider-only " << viaSlider << " ct");
        CHECK (viaSlider > 100.0);
        CHECK (std::abs (viaWheel / viaSlider - 1.0) < 0.03);
    }
    // Wheel at 0 or depth 0: no vibrato.
    CHECK (vibrato (0.0f, 1.0f, 0.0f) < 15.0);
    CHECK (vibrato (0.0f, 0.0f, 1.0f) < 15.0);
}

TEST_CASE ("Spec Mod wheel: respects the VCO mod target switch", "[spec][wheel]")
{
    // Only VCO-1 is audible (source mix 0). Target index: 0 = VCO-1, 1 = Both, 2 = VCO-2.
    CHECK (vibrato (0.0f, 0.5f, 1.0f, 0) > 100.0);
    CHECK (vibrato (0.0f, 0.5f, 1.0f, 1) > 100.0);
    CHECK (vibrato (0.0f, 0.5f, 1.0f, 2) < 15.0);
}

TEST_CASE ("Spec LFO delay: restarts only when a note starts with no other key held", "[spec][lfo]")
{
    // Solo, LFO 5 Hz sine, delay 1 s, vibrato 0.3 from the slider. The first note starts the delay; at
    // 3 s the vibrato is at full depth. A second note pressed while the first is held must NOT restart
    // the delay (full depth immediately). After all keys are up, a new note restarts it (small depth
    // in the first 0.3 s). A 0.3 s window holds >= 1 LFO cycle, so max - min sees the full swing.
    Rig rig;
    sinePatch (rig.params);
    rig.params.assignMode = 2;
    rig.params.lfoWave = 0;
    rig.params.lfoRate = vs::map::lfoRateFromHz (5.0f);
    rig.params.lfoDelay = vs::map::lfoDelayFromSec (1.0f);
    rig.params.vcoModLfo = 0.3f;
    rig.apply();
    const int t1 = 2000, t2 = 2000 + 144000, tUp = t2 + 48000, t3 = tUp + 24000;
    const auto out = rig.render (t3 + 24000, { on (t1, 60), on (t2, 64), off (tUp, 64), off (tUp + 100, 60), on (t3, 67) });
    auto dev = [&] (int from, int len, int note) { return pitchDeviationCents (out, from, from + len, 480, noteHz (note)); };
    const double full = dev (t1 + 96000, 24000, 60);
    const double fresh = dev (t1 + 480, 14400, 60);
    const double held = dev (t2 + 960, 14400, 64);
    const double fresh2 = dev (t3 + 960, 14400, 67);
    INFO ("full " << full << " fresh " << fresh << " second note with key held " << held << " after all keys up " << fresh2);
    REQUIRE (full > 100.0);
    CHECK (fresh < 0.5 * full);
    CHECK (held > 0.8 * full);
    CHECK (fresh2 < 0.5 * full);
}

// =================================================================================================
// Parameters
// =================================================================================================
TEST_CASE ("Spec parameters: the nine new parameters exist for both parts with the specified IDs, names, ranges, defaults", "[spec][params]")
{
    JuceEnv env;
    vs::VintageSynthProcessor proc;
    auto& state = proc.getState();
    CHECK (proc.getParameters().size() == 101); // M3: 96 + lower/upper mix.pan + key.mode, key.split_point, mix.balance

    struct F { const char* id; const char* name; float def; };
    const F floats[] { { "porta.time", "Portamento Time", 0.0f },
                       { "unison.detune", "Unison Detune", 0.3f },
                       { "vel.vca", "Velocity \xe2\x86\x92 VCA", 0.5f },
                       { "vel.vcf", "Velocity \xe2\x86\x92 VCF", 0.0f },
                       { "mod.wheel_vco", "Mod Wheel Vibrato", 0.3f } };
    struct C { const char* id; const char* name; std::vector<const char*> choices; int def; };
    const C choices[] { { "assign.mode", "Assign Mode", { "Poly", "Unison", "Solo" }, 0 },
                        { "assign.unison_voices", "Unison Voices", { "2", "4", "8", "16" }, 1 },
                        { "porta.mode", "Portamento Mode", { "Always", "Legato" }, 0 },
                        { "bend.range", "Bend Range", {}, 2 } };

    auto indexOf = [&] (const juce::String& id) {
        const auto& ps = proc.getParameters();
        for (int i = 0; i < ps.size(); ++i)
            if (auto* p = dynamic_cast<juce::AudioProcessorParameterWithID*> (ps[i]))
                if (p->paramID == id)
                    return i;
        return -1;
    };

    for (const juce::String prefix : { "lower.", "upper." })
    {
        INFO (prefix);
        const juce::String partName = prefix == "lower." ? "Lower" : "Upper";
        int last = -1;
        for (const auto& f : floats)
        {
            INFO (f.id);
            auto* p = dynamic_cast<juce::AudioParameterFloat*> (state.getParameter (prefix + f.id));
            REQUIRE (p != nullptr);
            // SPEC-AMBIGUITY: the spec table lists "Portamento Time"; the existing layout prefixes "Lower "/"Upper ".
            CHECK (p->getName (100) == partName + " " + juce::String (juce::CharPointer_UTF8 (f.name)));
            CHECK (std::abs (p->range.start) < 1.0e-9f);
            CHECK (std::abs (p->range.end - 1.0f) < 1.0e-9f);
            CHECK (std::abs (p->convertFrom0to1 (static_cast<juce::RangedAudioParameter*> (p)->getDefaultValue()) - f.def) < 1.0e-6f);
            const int idx = indexOf (prefix + f.id);
            CHECK (idx > last); // listed order
            last = idx;
        }
        CHECK (indexOf (prefix + "porta.time") == indexOf (prefix + "env2.release") + 1); // appended right after the existing floats
        int lastChoice = -1;
        for (const auto& c : choices)
        {
            INFO (c.id);
            auto* p = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (prefix + c.id));
            REQUIRE (p != nullptr);
            CHECK (p->getName (100) == partName + " " + juce::String (c.name));
            CHECK (p->getIndex() == c.def);
            if (! c.choices.empty())
            {
                REQUIRE (p->choices.size() == static_cast<int> (c.choices.size()));
                for (size_t i = 0; i < c.choices.size(); ++i)
                    CHECK (p->choices[static_cast<int> (i)] == juce::String (c.choices[i]));
            }
            else
            {
                CHECK (p->choices.size() == 13); // 0 .. 12 semitones
            }
            const int idx = indexOf (prefix + c.id);
            CHECK (idx > lastChoice);
            lastChoice = idx;
            CHECK (idx > indexOf (prefix + "mod.wheel_vco") - 100); // sanity: same part block
        }
        CHECK (indexOf (prefix + "assign.mode") == indexOf (prefix + "env2.key_follow") + 1);
    }
    // The upper part's list starts after the whole lower list.
    CHECK (indexOf ("upper.lfo.rate") > indexOf ("lower.bend.range"));
}

TEST_CASE ("Spec parameters: mappings of the new parameters", "[spec][params]")
{
    // porta: 0 = off, else 5 s x v^2; detune: 50 ct x v.
    CHECK (std::abs (vs::map::portaTimeSec (0.0f)) < 1.0e-9f);
    CHECK (std::abs (vs::map::portaTimeSec (0.5f) - 1.25f) < 1.0e-6f);
    CHECK (std::abs (vs::map::portaTimeSec (1.0f) - 5.0f) < 1.0e-6f);
    CHECK (std::abs (vs::map::unisonDetuneCents (0.3f) - 15.0f) < 1.0e-5f);
    CHECK (std::abs (vs::map::unisonDetuneCents (1.0f) - 50.0f) < 1.0e-5f);
    CHECK (vs::map::unisonVoices (0) == 2);
    CHECK (vs::map::unisonVoices (1) == 4);
    CHECK (vs::map::unisonVoices (2) == 8);
    CHECK (vs::map::unisonVoices (3) == 16);
    CHECK (std::abs (vs::map::bendRangeSemis (0)) < 1.0e-9f);
    CHECK (std::abs (vs::map::bendRangeSemis (12) - 12.0f) < 1.0e-6f);
}

TEST_CASE ("Spec parameters: state save and load round-trips the new parameters", "[spec][params][state]")
{
    JuceEnv env;
    juce::MemoryBlock saved;
    const float floatVals[] { 0.37f, 0.81f, 0.22f, 0.64f, 0.93f };
    const char* floatIds[] { "porta.time", "unison.detune", "vel.vca", "vel.vcf", "mod.wheel_vco" };
    const int choiceVals[] { 2, 3, 1, 9 };
    const char* choiceIds[] { "assign.mode", "assign.unison_voices", "porta.mode", "bend.range" };
    {
        vs::VintageSynthProcessor proc;
        for (const juce::String prefix : { "lower.", "upper." })
        {
            for (int i = 0; i < 5; ++i)
                proc.getState().getParameter (prefix + floatIds[i])->setValueNotifyingHost (floatVals[i]);
            for (int i = 0; i < 4; ++i)
            {
                auto* p = proc.getState().getParameter (prefix + choiceIds[i]);
                p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (choiceVals[i])));
            }
        }
        proc.getStateInformation (saved);
    }
    vs::VintageSynthProcessor restored;
    restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    for (const juce::String prefix : { "lower.", "upper." })
    {
        for (int i = 0; i < 5; ++i)
            CHECK (std::abs (restored.getState().getParameter (prefix + floatIds[i])->getValue() - floatVals[i]) < 1.0e-3f);
        for (int i = 0; i < 4; ++i)
        {
            auto* p = dynamic_cast<juce::AudioParameterChoice*> (restored.getState().getParameter (prefix + choiceIds[i]));
            REQUIRE (p != nullptr);
            CHECK (p->getIndex() == choiceVals[i]);
        }
    }
}

// =================================================================================================
// Robustness
// =================================================================================================
namespace
{
struct Stream
{
    Events events;
    std::vector<vs::PartParams> perPeriod; // parameters in force from period k (1920 samples) on
};

constexpr int kPeriod = 1920; // multiple of both 64 and 480

Stream makeStream (std::uint32_t seed, int numPeriods)
{
    std::mt19937 rng (seed);
    auto uni = [&] (int lo, int hi) { return std::uniform_int_distribution<int> (lo, hi) (rng); };
    auto unif = [&] { return std::uniform_real_distribution<float> (0.0f, 1.0f) (rng); };
    Stream s;
    vs::PartParams p;
    p.env2Release = vs::map::envTimeFromSec (0.08f);
    std::vector<int> held;
    for (int k = 0; k < numPeriods; ++k)
    {
        if (uni (0, 9) == 0)
        {
            p.assignMode = uni (0, 2);
            p.unisonVoices = uni (0, 3);
            p.portaMode = uni (0, 1);
            p.portaTime = uni (0, 1) != 0 ? unif() * 0.4f : 0.0f;
            p.bendRange = uni (0, 12);
            p.velToVca = unif();
            p.velToVcf = unif();
            p.unisonDetune = unif();
            p.modWheelDepth = unif();
            p.vcoModLfo = unif() * 0.5f;
        }
        s.perPeriod.push_back (p);
        const int n = uni (0, 3);
        std::vector<int> offsets;
        for (int i = 0; i < n; ++i)
            offsets.push_back (uni (0, kPeriod - 1));
        std::sort (offsets.begin(), offsets.end());
        for (const int o : offsets)
        {
            const int pos = k * kPeriod + o;
            const int r = uni (0, 99);
            if (r < 32)
            {
                const int note = uni (28, 96);
                s.events.push_back (on (pos, note, 0.1f + 0.9f * unif()));
                held.push_back (note);
            }
            else if (r < 60)
                s.events.push_back (off (pos, held.empty() || uni (0, 3) == 0 ? uni (28, 96) : held[static_cast<size_t> (uni (0, static_cast<int> (held.size()) - 1))]));
            else if (r < 70)
                s.events.push_back (pedal (pos, uni (0, 1) != 0));
            else if (r < 80)
                s.events.push_back (wheel (pos, unif()));
            else if (r < 92)
                s.events.push_back (bend (pos, 2.0f * unif() - 1.0f));
            else if (r < 95)
                s.events.push_back (ctl (pos, EventType::AllNotesOff));
            else if (r < 98)
                s.events.push_back (ctl (pos, EventType::ResetControllers));
            else
                s.events.push_back (ctl (pos, EventType::AllSoundOff));
        }
    }
    return s;
}

std::vector<float> renderStream (const Stream& s, int numPeriods, int blockSize, bool finish)
{
    Rig rig;
    rig.params = s.perPeriod.front();
    rig.apply();
    Events ev = s.events;
    const int total = numPeriods * kPeriod;
    int extra = 0;
    if (finish)
    {
        ev.push_back (ctl (total, EventType::AllNotesOff));
        extra = 48000; // 1 s for the release (80 ms) to end
    }
    return rig.render (total + extra, ev, blockSize, [&] (int start, vs::PartParams& p) {
        if (start % kPeriod == 0 && start / kPeriod < numPeriods)
            p = s.perPeriod[static_cast<size_t> (start / kPeriod)];
    });
}
} // namespace

TEST_CASE ("Spec robustness: random valid event streams stay finite and never hang a voice", "[spec][robust]")
{
    constexpr int periods = 125; // 5 s
    for (const std::uint32_t seed : { 1u, 2u, 3u, 4u })
    {
        INFO ("seed " << seed);
        const auto s = makeStream (seed, periods);
        const auto out = renderStream (s, periods, 480, true);
        REQUIRE (allFinite (out));
        CHECK (peakOf (out, 0, static_cast<int> (out.size())) < 100.0);
        // After All Notes Off at the end plus the release time nothing is left (pedal cleared too).
        CHECK (isSilent (out, periods * kPeriod + 24000, 24000, 1.0e-5));
    }
}

TEST_CASE ("Spec robustness: random streams render the same with host block sizes 64 and 480", "[spec][robust]")
{
    // Parameter changes sit on multiples of 1920 samples, which both block sizes hit exactly. The
    // decisions log promises -110 dB between block sizes; the bound here is -90 dB of the rms (3e-5),
    // loose enough for float rounding in the steal/fade splitting (measured: about -108 dB), tight enough to catch a timing or
    // state difference (a one-sample event shift changes a loud tone by > -40 dB).
    constexpr int periods = 100;
    for (const std::uint32_t seed : { 11u, 12u, 13u })
    {
        INFO ("seed " << seed);
        const auto s = makeStream (seed, periods);
        const auto a = renderStream (s, periods, 64, false);
        const auto b = renderStream (s, periods, 480, false);
        REQUIRE (a.size() == b.size());
        REQUIRE (allFinite (a));
        double diff = 0.0, sig = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const double d = static_cast<double> (a[i]) - static_cast<double> (b[i]);
            diff += d * d;
            sig += static_cast<double> (b[i]) * static_cast<double> (b[i]);
        }
        INFO ("signal rms " << std::sqrt (sig / static_cast<double> (b.size())) << ", difference rms " << std::sqrt (diff / static_cast<double> (b.size())));
        REQUIRE (sig > 0.0);
        CHECK (std::sqrt (diff / sig) < 3.0e-5);
    }
}
