// Exitium B-Side's part (source/bside/engine/Part.h; docs/architecture.md 8.21.1): the level, the latency, the
// mapping of every S3 and S4 parameter to the chip, block-length independence, the idle rule, the settled start,
// the Shared topology, structural switches and random event streams. The alias gate after the decimator is in
// PartAliasTests.cpp. Long scans are hidden ("[.part-full]") and run before a hand-over.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "PartTestSupport.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace bs_part_test;
namespace chip = vs::bside::chip;

namespace
{
bs::PartParams initParams()
{
    return bs::PartParams {};
}

/** The first host sample from `from` at which y crosses the middle of its levels before (lo) and after (hi) an edge
    near `expected`, interpolated; NaN if none. */
double edgeAt (const std::vector<float>& y, double expected)
{
    const auto e = static_cast<int> (std::lround (expected));
    auto mean = [&] (int a, int b) {
        double s = 0.0;
        for (int i = a; i < b; ++i)
            s += static_cast<double> (y[static_cast<std::size_t> (i)]);
        return s / (b - a);
    };
    const double lo = mean (e - 60, e - 20), hi = mean (e + 20, e + 60);
    const double mid = 0.5 * (lo + hi);
    for (int i = e - 20; i < e + 20; ++i)
    {
        const double a = static_cast<double> (y[static_cast<std::size_t> (i)]) - mid;
        const double b = static_cast<double> (y[static_cast<std::size_t> (i + 1)]) - mid;
        if ((a <= 0.0 && b > 0.0) || (a >= 0.0 && b < 0.0))
            return i + a / (a - b);
    }
    return std::numeric_limits<double>::quiet_NaN();
}

/** A table that starts every note's accumulators at 0 (phase restart): the first pulse edge lies at a known cycle. */
const char* kPhaseTable = "bside-table 1\n[restart]\nmode phase\n";

/** Renders a C2 pulse note (attack 0, direct) started at host sample n; returns where its first rising pulse edge
    appears, in host samples, minus where the chip makes it (n + the edge's chip time): the part's delay. */
double measuredDelay (HostConfig h, int model, bool shared, int n)
{
    bs::PartParams p;
    p.chipModel = model;
    p.filterTopology = shared ? 1 : 0;
    p.osc1Wave = 3;
    p.osc1Pw = 50.0f;
    p.osc1Attack = 0;
    p.osc1Decay = 0;
    p.osc1Sustain = 15;
    p.osc1Filter = 0;
    const auto prog = compile (kPhaseTable);
    Rig rig (h, p, 512);
    rig.part->setProgram (&prog);
    const int note = 36;
    const auto y = rig.run (n + static_cast<int> (0.03 * h.rate), { on (n, note) });
    // the accumulator starts at 0 at the note-on cycle; the pulse rises when bits 23..12 reach 2048: after 2^23 / F cycles
    const double f = 440.0 * std::exp2 ((note - 69) / 12.0) * 16777216.0 / 985248.0;
    const double edgeCycles = 8388608.0 / f;
    const double edgeHost = static_cast<double> (n) + edgeCycles / 985248.0 * h.rate;
    const double at = edgeAt (y, edgeHost + rig.part->getLatencySamples());
    return at - edgeHost;
}
} // namespace

// ---- level ---------------------------------------------------------------------------------------------------

TEST_CASE ("Part: one note of the Init program at velocity 1 peaks at -12 dBFS +-0.5 dB on Model B (kChipGain)", "[bside][part][level]")
{
    for (const int note : { 48, 60, 72 })
    {
        Rig rig ({ 48000.0, 2 }, initParams());
        const auto y = rig.run (48000, { on (0, note, 1.0f) });
        const double peak = db (peakOf (y));
        INFO ("note " << note << ": peak " << peak << " dBFS");
        CHECK (peak > -12.5);
        CHECK (peak < -11.5);
    }
}

// ---- latency -------------------------------------------------------------------------------------------------

TEST_CASE ("Part: one latency for both models, fixed at prepare; a note's first edge lands there at every factor of 4.4", "[bside][part][latency]")
{
    for (const auto& h : hostConfigs())
    {
        Rig a (h, [] { bs::PartParams p; p.chipModel = 0; return p; }());
        Rig b (h, [] { bs::PartParams p; p.chipModel = 1; return p; }());
        INFO (h.rate << " Hz x" << h.factor << ": latency " << b.part->getLatencySamples() << " (exact " << b.part->getExactLatency() << ")");
        REQUIRE (a.part->getLatencySamples() == b.part->getLatencySamples());
        // the longer chain (Model A's) at the internal rate, plus the decimator's measured delay, rounded
        CHECK (b.part->getMaxChainLatency() == b.part->getChainLatency (chip::Model::A));
        CHECK (b.part->getChainLatency (chip::Model::A) > b.part->getChainLatency (chip::Model::B));
        CHECK (std::abs (b.part->getExactLatency() - b.part->getLatencySamples()) <= 0.5);

        for (const int n : { 100, 333 })
        {
            for (const bool shared : { false, true })
            {
                double byModel[2] {};
                for (const int model : { 0, 1 })
                {
                    const double d = measuredDelay (h, model, shared, n);
                    INFO ("model " << (model == 0 ? "A" : "B") << (shared ? " Shared" : " Per Note") << ", note at " << n << ": edge delay " << d
                                   << " host samples, reported " << b.part->getLatencySamples());
                    REQUIRE (std::isfinite (d));
                    CHECK (std::abs (d - b.part->getLatencySamples()) <= 1.0);
                    byModel[model] = d;
                }
                // the two models within half a host sample of each other, Per Note and Shared (8.21.1 after the review)
                INFO ((shared ? "Shared" : "Per Note") << ", note at " << n << ": Model A " << byModel[0] << ", Model B " << byModel[1]);
                CHECK (std::abs (byModel[0] - byModel[1]) <= 0.5);
            }
        }
    }
}

TEST_CASE ("Part: the latency pad holds the difference of the two chains with margin at 8, 11.025 and 16 kHz; both models' edges agree there", "[bside][part][latency]")
{
    // 8.21.1 after the review: the pad delays the shorter chain by the difference of the two chains (63 internal samples
    // at 8 and 11.025 kHz, where the chains are 134 and 71 samples long: longer than the old ring of 64 themselves)
    for (const double rate : { 8000.0, 11025.0, 16000.0 })
        for (const int factor : { 4, 2, 1 })
        {
            const HostConfig h { rate, factor };
            Rig a (h, [] { bs::PartParams p; p.chipModel = 0; return p; }());
            Rig b (h, [] { bs::PartParams p; p.chipModel = 1; return p; }());
            const int difference = b.part->getChainLatency (chip::Model::A) - b.part->getChainLatency (chip::Model::B);
            INFO (rate << " Hz x" << factor << ": chains " << b.part->getChainLatency (chip::Model::A) << " and " << b.part->getChainLatency (chip::Model::B)
                       << ", pad " << b.part->getPadDelay() << " of " << bs::Part::padCapacity());
            CHECK (a.part->getLatencySamples() == b.part->getLatencySamples());
            CHECK (a.part->getPadDelay() == 0);
            CHECK (b.part->getPadDelay() == difference);
            CHECK (difference > 0);
            CHECK (2 * difference <= bs::Part::padCapacity()); // margin: twice what is needed
            // the pad does its work: the first edge of a note lands at the same place on both models, at the reported latency
            double byModel[2] {};
            for (const int model : { 0, 1 })
            {
                byModel[model] = measuredDelay (h, model, false, 150);
                INFO ("model " << (model == 0 ? "A" : "B") << ": edge delay " << byModel[model] << " host samples, reported " << b.part->getLatencySamples());
                REQUIRE (std::isfinite (byModel[model]));
                CHECK (std::abs (byModel[model] - b.part->getLatencySamples()) <= 1.0);
            }
            CHECK (std::abs (byModel[0] - byModel[1]) <= 0.5);
        }
}

// ---- block lengths ---------------------------------------------------------------------------------------------

namespace
{
const char* kArpTable = "bside-table 1\n[wave1]\n    wave pulse+gate +0 ticks 0\nloop:\n    pitch +0\n    pitch +4\n    pitch +7\n    jump loop\n"
                        "release:\n    gate off ticks 0\n    jump loop\n[pulse1]\n    set 1024\n    pingpong 512 3584 48\n[filter]\n    cutoff 1800\n    sweep -40 x16\n";

std::vector<vs::NoteEvent> scenario()
{
    return { on (10, 60, 0.9f), on (37, 64, 0.6f), control (500, vs::EventType::PitchBend, 0.4f), on (1200, 67, 1.0f),
             control (2000, vs::EventType::ModWheel, 0.8f), off (3000, 64), control (3500, vs::EventType::SustainPedal, 1.0f),
             off (4100, 60), on (5000, 72, 0.7f), control (6000, vs::EventType::SustainPedal, 0.0f), off (7000, 67),
             off (9000, 72), on (9100, 48, 1.0f), off (14000, 48) };
}

struct Run
{
    std::vector<float> y;
    std::vector<chip::DigitalState> states;
    std::vector<std::uint64_t> ticks;
};

Run runScenario (int model, bool shared, int block, int total = 24000)
{
    bs::PartParams p;
    p.chipModel = model;
    p.filterTopology = shared ? 1 : 0;
    p.osc2Wave = 2;
    p.osc2Coarse = 7;
    p.filterResonance = 6;
    const auto prog = compile (kArpTable);
    Rig rig ({ 48000.0, 2 }, p, 512);
    rig.part->setProgram (&prog);
    Run r;
    r.y = rig.run (total, scenario(), block);
    for (int i = 0; i < bs::Part::kVoices; ++i)
    {
        r.states.push_back (rig.part->getVoice (i).chain.chip().state());
        r.ticks.push_back (rig.part->getVoice (i).engine.tickIndex());
    }
    return r;
}
} // namespace

TEST_CASE ("Part: block lengths 1, 7, 61 and 512 give the same digital state and ticks and the same audio; two instances are bit-identical", "[bside][part][blocks]")
{
    for (const int model : { 1, 0 })
        for (const bool shared : { false, true })
        {
            const auto ref = runScenario (model, shared, 512);
            const auto twin = runScenario (model, shared, 512);
            INFO ("model " << (model == 0 ? "A" : "B") << (shared ? " Shared" : " Per Note"));
            CHECK (twin.y == ref.y); // two instances, the same input: bit-identical
            REQUIRE (peakOf (ref.y) > 0.01);
            for (const int block : { 1, 7, 61 })
            {
                const auto r = runScenario (model, shared, block);
                double worst = 0.0;
                for (std::size_t i = 0; i < r.y.size(); ++i)
                    worst = std::max (worst, std::abs (static_cast<double> (r.y[i]) - static_cast<double> (ref.y[i])));
                INFO ("block " << block << ": largest difference " << worst << " (" << db (worst) << " dB re full scale)");
                CHECK (worst <= 1.0e-6); // -120 dB
                for (int v = 0; v < bs::Part::kVoices; ++v)
                {
                    INFO ("voice " << v);
                    CHECK (r.states[static_cast<std::size_t> (v)] == ref.states[static_cast<std::size_t> (v)]);
                    CHECK (r.ticks[static_cast<std::size_t> (v)] == ref.ticks[static_cast<std::size_t> (v)]);
                }
            }
        }
}

// ---- the mapping of the host parameters (8.21.1, "Host parameters to chip values"; 8.7 rows S3 and S4) -----------

namespace
{
/** A part with `p`, one note (60 at velocity `velocity`) started, `ms` of audio rendered. */
struct NoteRig
{
    Rig rig;
    NoteRig (const bs::PartParams& p, double ms = 20.0, float velocity = 1.0f, int note = 60, HostConfig h = { 48000.0, 2 })
        : rig (h, p)
    {
        rig.run (static_cast<int> (ms * 0.001 * h.rate), { on (0, note, velocity) });
    }
    const chip::Chip& c (int voice = 0) const { return rig.part->getVoice (voice).chain.chip(); }
    std::uint8_t reg (std::uint8_t r, int voice = 0) const { return c (voice).registerValue (r); }
    /** Oscillator n's frequency in Hz (Smooth: the increment's register value x clock / 2^24). */
    double hz (int n, int voice = 0) const
    {
        return static_cast<double> (c (voice).increment (n)) / 4294967296.0 * 985248.0 / 16777216.0;
    }
};

double cents (double a, double b) { return 1200.0 * std::log2 (a / b); }
} // namespace

TEST_CASE ("Part mapping: oscN.wave is the control register's waveform bits, Off leaves the oscillator off", "[bside][part][mapping]")
{
    const std::array<std::uint8_t, 9> bits { 0x00, 0x10, 0x20, 0x40, 0x80, 0x30, 0x50, 0x60, 0x70 };
    for (int w = 0; w < 9; ++w)
        for (int n = 0; n < 3; ++n)
        {
            bs::PartParams p;
            p.osc1Wave = n == 0 ? w : 0;
            p.osc2Wave = n == 1 ? w : 0;
            p.osc3Wave = n == 2 ? w : 0;
            NoteRig r (p);
            const auto ctl = r.reg (static_cast<std::uint8_t> (7 * n + 4));
            INFO ("osc " << n + 1 << " wave " << w << ": control 0x" << std::hex << static_cast<int> (ctl));
            CHECK ((ctl & 0xf0) == bits[static_cast<std::size_t> (w)]);
            CHECK ((ctl & 0x01) == (w == 0 ? 0 : 1)); // the gate follows the note only when the oscillator is not Off
        }
}

TEST_CASE ("Part mapping: sync, ring and the envelope settings are control bits 1 and 2 and the two envelope registers", "[bside][part][mapping]")
{
    bs::PartParams p;
    p.osc1Wave = p.osc2Wave = p.osc3Wave = 1;
    p.osc2Sync = 1;
    p.osc3Ring = 1;
    p.osc1Attack = 3, p.osc1Decay = 4, p.osc1Sustain = 5, p.osc1Release = 6;
    p.osc2Attack = 7, p.osc2Decay = 8, p.osc2Sustain = 9, p.osc2Release = 10;
    p.osc3Attack = 11, p.osc3Decay = 12, p.osc3Sustain = 13, p.osc3Release = 14;
    NoteRig r (p);
    CHECK ((r.reg (4) & 0x06) == 0x00);
    CHECK ((r.reg (11) & 0x06) == 0x02);
    CHECK ((r.reg (18) & 0x06) == 0x04);
    CHECK (r.reg (5) == 0x34);
    CHECK (r.reg (6) == 0x56);
    CHECK (r.reg (12) == 0x78);
    CHECK (r.reg (13) == 0x9a);
    CHECK (r.reg (19) == 0xbc);
    CHECK (r.reg (20) == 0xde);
}

TEST_CASE ("Part mapping: oscN.pw + macro.pw_offset is the pulse width 0 .. 4095, clamped", "[bside][part][mapping]")
{
    for (const auto& c : std::vector<std::array<float, 3>> { { 50.0f, 0.0f, 2047.5f }, { 25.0f, 10.0f, 1023.75f + 409.6f }, { 90.0f, 50.0f, 4095.0f },
                                                            { 5.0f, -50.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 100.0f, 0.0f, 4095.0f } })
    {
        bs::PartParams p;
        p.osc1Pw = c[0];
        p.osc2Wave = 3;
        p.osc2Pw = c[0];
        p.macroPwOffset = c[1];
        NoteRig r (p);
        INFO ("pw " << c[0] << " %, offset " << c[1] << " %");
        CHECK (static_cast<double> (r.c().pulseWidthSmooth (0)) == Catch::Approx (c[2]).margin (0.01));
        CHECK (static_cast<double> (r.c().pulseWidthSmooth (1)) == Catch::Approx (c[2]).margin (0.01));
    }
}

TEST_CASE ("Part mapping: oscN.filter, osc3.mute, filter.resonance and chip.volume are the routing bits, bit 7 of 0x18 and the two nibbles", "[bside][part][mapping]")
{
    for (int mask = 0; mask < 8; ++mask)
    {
        bs::PartParams p;
        p.osc1Filter = mask & 1;
        p.osc2Filter = (mask >> 1) & 1;
        p.osc3Filter = (mask >> 2) & 1;
        p.osc3Mute = mask & 1;
        p.filterResonance = 2 * mask;
        p.chipVolume = 15 - mask;
        NoteRig r (p);
        INFO ("mask " << mask);
        CHECK ((r.reg (0x17) & 7) == mask);
        CHECK ((r.reg (0x17) >> 4) == 2 * mask);
        CHECK ((r.reg (0x18) >> 7) == (mask & 1));
        CHECK ((r.reg (0x18) & 15) == 15 - mask);
    }
}

TEST_CASE ("Part mapping: filter.mode is Off 0, Low 1, Band 2, High 4, Low+Band 3, Band+High 6, Low+High 5, All 7 in bits 4 to 6", "[bside][part][mapping]")
{
    const std::array<int, 8> want { 0, 1, 2, 4, 3, 6, 5, 7 };
    for (int m = 0; m < 8; ++m)
    {
        bs::PartParams p;
        p.filterMode = m;
        NoteRig r (p);
        INFO ("mode " << m);
        CHECK (((r.reg (0x18) >> 4) & 7) == want[static_cast<std::size_t> (m)]);
    }
}

TEST_CASE ("Part mapping: the cutoff is filter.cutoff + key follow x 16 x (note - 60) - vel.cutoff x (1 - velocity) x 1024, then the macro offset, clamped", "[bside][part][mapping]")
{
    struct Case
    {
        float cutoff, follow, velCut, offset, velocity;
        int note;
    };
    for (const auto& k : std::vector<Case> { { 1024.0f, 0.0f, 0.0f, 0.0f, 1.0f, 60 }, { 1000.0f, 100.0f, 0.0f, 0.0f, 1.0f, 72 }, { 1000.0f, 50.0f, 0.0f, 0.0f, 1.0f, 48 },
                                             { 1500.0f, 0.0f, 1.0f, 0.0f, 0.5f, 60 }, { 1500.0f, 0.0f, 0.5f, 0.0f, 0.25f, 60 }, { 800.0f, 0.0f, 0.0f, 25.0f, 1.0f, 60 },
                                             { 800.0f, 0.0f, 0.0f, -100.0f, 1.0f, 60 }, { 2000.0f, 100.0f, 0.0f, 100.0f, 1.0f, 96 } })
    {
        bs::PartParams p;
        p.filterCutoff = k.cutoff;
        p.filterKeyFollow = k.follow;
        p.velCutoff = k.velCut;
        p.macroCutoffOffset = k.offset;
        NoteRig r (p, 20.0, k.velocity, k.note);
        const double base = std::clamp (static_cast<double> (k.cutoff) + k.follow / 100.0 * 16.0 * (k.note - 60) - k.velCut * (1.0 - k.velocity) * 1024.0, 0.0, 2047.0);
        const double want = std::clamp (base + k.offset * 2047.0 / 100.0, 0.0, 2047.0);
        INFO ("cutoff " << k.cutoff << ", follow " << k.follow << " %, vel.cutoff " << k.velCut << ", offset " << k.offset << " %, velocity " << k.velocity << ", note " << k.note);
        CHECK (static_cast<double> (r.c().cutoffSmooth()) == Catch::Approx (want).margin (0.01));
    }
}

TEST_CASE ("Part mapping: macro.sweep_depth scales every sweep step (1 at 100 %)", "[bside][part][mapping]")
{
    const auto prog = compile ("bside-table 1\n[pulse1]\n    set 1000\n    sweep +100 x10\n");
    double width[2] {};
    for (int k = 0; k < 2; ++k)
    {
        bs::PartParams p;
        p.macroSweepDepth = k == 0 ? 100.0f : 50.0f;
        Rig rig ({ 48000.0, 2 }, p);
        rig.part->setProgram (&prog);
        rig.run (static_cast<int> (0.25 * 48000.0), { on (0, 60) }); // more than 11 ticks
        width[k] = static_cast<double> (rig.part->getVoice (0).chain.chip().pulseWidthSmooth (0));
    }
    CHECK (width[0] == Catch::Approx (2000.0).margin (0.01));
    CHECK (width[1] == Catch::Approx (1500.0).margin (0.01));
}

TEST_CASE ("Part mapping: chip.variation moves Model A's cutoff curve (smoothed), not Model B's", "[bside][part][mapping]")
{
    for (const int model : { 0, 1 })
    {
        double hz[2] {};
        for (int k = 0; k < 2; ++k)
        {
            bs::PartParams p;
            p.chipModel = model;
            p.chipVariation = k == 0 ? 50.0f : 90.0f;
            NoteRig r (p);
            hz[k] = r.rig.part->getVoice (0).filter.cutoffHz();
        }
        INFO ("model " << (model == 0 ? "A" : "B") << ": corner " << hz[0] << " -> " << hz[1] << " Hz");
        if (model == 0)
            CHECK (std::abs (hz[1] - hz[0]) > 0.01 * hz[0]);
        else
            CHECK (std::abs (hz[1] - hz[0]) <= 1e-9 * hz[0]);
    }
    // smoothed: a change reaches the filter over 5 ms of control points, not at once
    bs::PartParams p;
    p.chipModel = 0;
    Rig rig ({ 48000.0, 2 }, p);
    rig.run (480, { on (0, 60) });
    const double before = rig.part->getVoice (0).filter.cutoffHz();
    p.chipVariation = 90.0f;
    rig.set (p);
    rig.run (96); // 2 ms: part of the way
    const double during = rig.part->getVoice (0).filter.cutoffHz();
    rig.run (480);
    const double after = rig.part->getVoice (0).filter.cutoffHz();
    INFO ("corner " << before << " -> " << during << " -> " << after);
    CHECK (std::abs (during - before) > 0.05 * std::abs (after - before));
    CHECK (std::abs (after - during) > 0.05 * std::abs (after - before));
}

TEST_CASE ("Part mapping: vel.volume is the note gain 1 - amount x (1 - velocity), smoothed over 5 ms on a retrigger", "[bside][part][mapping]")
{
    for (const float amount : { 0.0f, 0.5f, 1.0f })
        for (const float velocity : { 1.0f, 0.5f, 0.2f })
        {
            bs::PartParams p;
            p.velVolume = amount;
            NoteRig r (p, 5.0, velocity);
            INFO ("amount " << amount << ", velocity " << velocity);
            CHECK (r.rig.part->getVoice (0).gain == Catch::Approx (1.0 - amount * (1.0 - velocity)).margin (1e-12));
        }
    // a retrigger moves the gain over 5 ms
    bs::PartParams p;
    p.velVolume = 1.0f;
    Rig rig ({ 48000.0, 2 }, p);
    rig.run (480, { on (0, 60, 1.0f) });
    rig.run (48, { on (0, 60, 0.2f) }); // 1 ms after the retrigger
    const double during = rig.part->getVoice (0).gain;
    rig.run (480);
    const double after = rig.part->getVoice (0).gain;
    CHECK (during < 0.95);
    CHECK (during > 0.3);
    CHECK (after == Catch::Approx (0.2).margin (1e-12));
}

TEST_CASE ("Part mapping: vibrato at vibrato.rate with depth mod wheel x mod.wheel_vibrato (0 .. 100 ct)", "[bside][part][mapping]")
{
    bs::PartParams p;
    p.vibratoRate = 5.0f;
    p.modWheelVibrato = 0.5f; // 50 ct at a full wheel
    Rig rig ({ 48000.0, 2 }, p);
    rig.run (480, { on (0, 69), control (0, vs::EventType::ModWheel, 1.0f) });
    const double centre = 440.0;
    double lo = 1e9, hi = -1e9;
    int crossings = 0;
    double last = 0.0;
    for (int b = 0; b < 100; ++b) // 1 s in 10 ms steps
    {
        rig.run (480);
        const double c = cents (static_cast<double> (rig.part->getVoice (0).chain.chip().increment (0)) / 4294967296.0 * 985248.0 / 16777216.0, centre);
        lo = std::min (lo, c);
        hi = std::max (hi, c);
        if (b > 0 && ((last < 0.0 && c >= 0.0)))
            ++crossings;
        last = c;
    }
    INFO ("vibrato " << lo << " .. " << hi << " ct, " << crossings << " upward crossings in 1 s");
    CHECK (hi == Catch::Approx (50.0).margin (1.5));
    CHECK (lo == Catch::Approx (-50.0).margin (1.5));
    CHECK (crossings >= 4);
    CHECK (crossings <= 6);
    // no wheel, no vibrato
    Rig flat ({ 48000.0, 2 }, p);
    flat.run (4800, { on (0, 69) });
    CHECK (cents (static_cast<double> (flat.part->getVoice (0).chain.chip().increment (0)) / 4294967296.0 * 985248.0 / 16777216.0, centre) == Catch::Approx (0.0).margin (0.01));
}

TEST_CASE ("Part mapping: bend.range x the bend, coarse and fine are the oscillator's pitch", "[bside][part][mapping]")
{
    for (const int range : { 0, 2, 7, 12 })
    {
        bs::PartParams p;
        p.bendRange = range;
        p.osc1Coarse = 5;
        p.osc1Fine = -30.0f;
        Rig rig ({ 48000.0, 2 }, p);
        rig.run (4800, { on (0, 60), control (0, vs::EventType::PitchBend, 1.0f) });
        const double f = static_cast<double> (rig.part->getVoice (0).chain.chip().increment (0)) / 4294967296.0 * 985248.0 / 16777216.0;
        const double want = 440.0 * std::exp2 ((60 + 5 - 0.3 + range - 69) / 12.0);
        INFO ("range " << range << ": " << f << " Hz, expected " << want);
        CHECK (cents (f, want) == Catch::Approx (0.0).margin (0.01));
    }
}

TEST_CASE ("Part mapping: porta.time glides 99 % of the interval in 5 s x v^2 (Always), only legato in Legato", "[bside][part][mapping]")
{
    bs::PartParams p;
    p.portaTime = std::sqrt (0.2f / 5.0f); // 200 ms
    p.assignMode = 2;                    // Solo
    Rig rig ({ 48000.0, 2 }, p);
    auto hzOf = [&] { return static_cast<double> (rig.part->getVoice (0).chain.chip().increment (0)) / 4294967296.0 * 985248.0 / 16777216.0; };
    rig.run (4800, { on (0, 60) });
    rig.run (4800, { off (0, 60), on (0, 72) }); // 100 ms into a glide from 60 to 72 (Always: no key held, still glides)
    const double mid = cents (hzOf(), 440.0 * std::exp2 ((72 - 69) / 12.0));
    rig.run (static_cast<int> (0.1 * 48000.0)); // at 200 ms: 99 % done, 12 ct left at most
    const double end = cents (hzOf(), 440.0 * std::exp2 ((72 - 69) / 12.0));
    INFO ("100 ms: " << mid << " ct, 200 ms: " << end << " ct from the target");
    CHECK (mid < -60.0);  // exp: 10 % of the way left would be -120 ct; still far
    CHECK (mid > -1200.0);
    CHECK (std::abs (end) <= 13.0);
    CHECK (std::abs (end) >= 11.0);

    // Legato: a note with no key held does not glide
    p.portaMode = 1;
    Rig leg ({ 48000.0, 2 }, p);
    auto legHz = [&] { return static_cast<double> (leg.part->getVoice (0).chain.chip().increment (0)) / 4294967296.0 * 985248.0 / 16777216.0; };
    leg.run (4800, { on (0, 60) });
    leg.run (480, { off (0, 60), on (0, 72) });
    CHECK (cents (legHz(), 440.0 * std::exp2 ((72 - 69) / 12.0)) == Catch::Approx (0.0).margin (0.01));
    leg.run (480, { on (0, 76) }); // a key is held: glides
    CHECK (std::abs (cents (legHz(), 440.0 * std::exp2 ((76 - 69) / 12.0))) > 50.0);
}

TEST_CASE ("Part mapping: assign.mode, assign.unison_voices and unison.detune: voices, spread and level 1 / sqrt (n)", "[bside][part][mapping]")
{
    for (const int voicesIndex : { 0, 1, 2, 3 })
    {
        const int n = std::array<int, 4> { 2, 4, 8, 16 }[static_cast<std::size_t> (voicesIndex)];
        bs::PartParams p;
        p.assignMode = 1;
        p.unisonVoices = voicesIndex;
        p.unisonDetune = 1.0f; // 50 ct spread
        p.velVolume = 0.0f;
        NoteRig r (p);
        CHECK (r.rig.part->numActiveVoices() == n);
        double lo = 1e9, hi = -1e9;
        for (int v = 0; v < n; ++v)
        {
            const double c = cents (r.hz (0, v), 440.0 * std::exp2 ((60 - 69) / 12.0));
            lo = std::min (lo, c);
            hi = std::max (hi, c);
            CHECK (r.rig.part->getVoice (v).gain == Catch::Approx (1.0 / std::sqrt (static_cast<double> (n))).margin (1e-12));
        }
        INFO (n << " voices: " << lo << " .. " << hi << " ct");
        CHECK (lo == Catch::Approx (-25.0).margin (0.01));
        CHECK (hi == Catch::Approx (25.0).margin (0.01));
    }
    bs::PartParams solo;
    solo.assignMode = 2;
    NoteRig s (solo);
    s.rig.run (480, { on (0, 64) });
    CHECK (s.rig.part->numActiveVoices() == 1);
    CHECK (s.rig.part->getVoice (0).note == 64);
}

TEST_CASE ("Part mapping: unison voices start at the phase pattern k^2 / (2 n) of a cycle", "[bside][part][mapping]")
{
    bs::PartParams p;
    p.assignMode = 1;
    p.unisonVoices = 1; // 4
    p.unisonDetune = 0.0f;
    Rig rig ({ 48000.0, 2 }, p);
    rig.run (0, { on (0, 60) }); // applied, nothing rendered: the voices sit at their note-on cycle
    rig.run (1);
    // after the same number of cycles, the accumulators differ by the pattern (equal frequencies)
    const auto a0 = rig.part->getVoice (0).chain.chip().accumulator (0);
    for (int k = 1; k < 4; ++k)
    {
        const auto ak = rig.part->getVoice (k).chain.chip().accumulator (0);
        const double frac = static_cast<double> ((ak - a0) & 0xffffffu) / 16777216.0;
        const double want = std::fmod (k * k / 8.0, 1.0);
        INFO ("voice " << k << ": " << frac << " of a cycle ahead, expected " << want);
        CHECK (std::abs (frac - want) < 1e-6);
    }
}

TEST_CASE ("Part mapping: chip.model, chip.resolution, chip.volume_dc and chip.clock configure the voices' chips; restart.policy and tick.* the engines", "[bside][part][mapping]")
{
    bs::PartParams p;
    p.chipModel = 0;
    p.chipResolution = 1;
    p.chipVolumeDc = 1;
    Rig rig ({ 48000.0, 2 }, p, 512, 1);
    rig.run (480, { on (0, 60) });
    const auto& cfg = rig.part->getVoice (0).chain.config();
    CHECK (cfg.model == chip::Model::A);
    CHECK (cfg.authentic);
    CHECK (cfg.volumeDc);
    CHECK (cfg.clockHz == 1022727u);
    // Authentic: the frequency register is an integer
    CHECK ((rig.part->getVoice (0).chain.chip().increment (0) & 0xffffffffu) == 0u);
    // the tick period (8.6.1): Frame at 60 Hz system / mult; Free; Tempo
    struct T
    {
        int rate, mult, sync;
        float freeHz;
        double tempo;
        std::uint32_t want;
    };
    for (const auto& t : std::vector<T> { { 0, 1, 2, 50.0f, 120.0, 17095u }, { 0, 4, 2, 50.0f, 120.0, 4274u }, { 1, 1, 2, 100.0f, 120.0, 10227u },
                                          { 1, 2, 2, 400.0f, 120.0, 1278u }, { 2, 1, 2, 50.0f, 120.0, 21307u }, { 2, 2, 3, 50.0f, 90.0, 7102u } })
    {
        bs::PartParams q = p;
        q.tickRate = t.rate;
        q.tickMult = t.mult;
        q.tickSync = t.sync;
        q.tickFreeHz = t.freeHz;
        rig.set (q);
        rig.part->setTempo (t.tempo);
        rig.run (48);
        INFO ("tick.rate " << t.rate << " mult " << t.mult << " sync " << t.sync << " free " << t.freeHz << " tempo " << t.tempo);
        CHECK (rig.part->getVoice (0).engine.tickPeriod() == t.want);
    }
}

// ---- idle (8.5, 8.21.1) -----------------------------------------------------------------------------------------

TEST_CASE ("Part idle: after its note-off a voice gets enterIdle, falls silent, leaves the sum and is idle for the allocator; idle voices are not rendered", "[bside][part][idle]")
{
    bs::PartParams p;
    p.osc1Release = 0;
    Rig rig ({ 48000.0, 2 }, p);
    rig.run (4800, { on (0, 60) });
    CHECK (rig.part->numActiveVoices() == 1);
    CHECK (rig.part->getAllocator().isVoiceBusy (0));
    rig.run (480, { off (0, 60) });
    CHECK (rig.part->numActiveVoices() == 1); // the release still sounds
    int idleAfter = -1, leftAfter = -1;
    for (int b = 0; b < 1000 && leftAfter < 0; ++b)
    {
        rig.run (48);
        if (idleAfter < 0 && rig.part->getVoice (0).idleSent)
            idleAfter = b + 1;
        if (rig.part->numActiveVoices() == 0)
            leftAfter = b + 1;
    }
    INFO ("enterIdle " << idleAfter << " ms and left the sum " << leftAfter << " ms after 10 ms of release");
    REQUIRE (idleAfter > 0);
    REQUIRE (leftAfter > 0);
    CHECK (idleAfter < 60); // release 0 (6 ms nominal; the envelopes at 0)
    CHECK (leftAfter - idleAfter <= 505); // silent for a control interval, or 500 ms and the 3 ms fade
    CHECK_FALSE (rig.part->getAllocator().isVoiceBusy (0));
    CHECK_FALSE (rig.part->isSounding());
    CHECK (rig.part->getVoice (0).chain.isSuspended());
    // silent from here on: exactly, once the decimator's and the shaped rounding's tails are over
    rig.run (9600);
    const auto y = rig.run (48000);
    CHECK (peakOf (y) <= 0.0); // exactly 0
}

TEST_CASE ("Part idle: a suspended chain's chip skipped through time has the state the rendered chip has", "[bside][part][idle]")
{
    // The entry points the idle rule uses (ChipVoice::suspend, skipSamples): a chip rendered for n samples and the same
    // chip rendered for k, suspended and skipped for n - k have the same digital state, noise register included.
    for (const auto model : { chip::Model::B, chip::Model::A })
    {
        auto run = [&] (int k, int n) {
            auto v = std::make_unique<chip::ChipVoice>();
            chip::Config cfg;
            cfg.model = model;
            cfg.internalRate = 96000.0;
            v->prepare (cfg);
            chip::Filter f;
            f.prepare (model, 96000.0, 0.5f);
            v->setFilter (&f);
            chip::OscInput in;
            in.frequency = 7000.0;
            in.pulseWidth = 1000.0f;
            in.control = 0x41;
            in.sustainRelease = 0xf0;
            v->setOscillator (0, in);
            in.frequency = 3000.0;
            in.control = 0x81; // noise
            v->setOscillator (1, in);
            v->write (0x18, 0x1f);
            std::vector<double> out (static_cast<std::size_t> (n), 0.0);
            v->renderUntil (out.data(), k, ~std::uint64_t { 0 });
            if (k < n)
            {
                v->suspend();
                v->skipSamples (n - k);
            }
            return std::make_pair (v->chip().state(), v->chip().cycle());
        };
        const auto rendered = run (200000, 200000);
        const auto skipped = run (777, 200000);
        CHECK (rendered.second == skipped.second);
        CHECK (rendered.first == skipped.first);
    }
}

TEST_CASE ("Part idle: an idle voice's chip time keeps running with the part's", "[bside][part][idle]")
{
    bs::PartParams p;
    p.osc1Release = 0;
    Rig rig ({ 48000.0, 2 }, p);
    rig.run (4800, { on (0, 60) });
    rig.run (72000, { off (0, 60) });
    REQUIRE (rig.part->numActiveVoices() == 0);
    rig.run (0, { on (0, 60) });
    chip::ChipClock clk;
    clk.prepare (985248u, 96000.0);
    const auto cycles = clk.nextSamples (static_cast<std::uint64_t> ((4800 + 72000) * 2));
    CHECK (rig.part->getVoice (0).chain.chip().cycle() == cycles);
    // an idle voice that never played keeps it too
    CHECK (rig.part->getVoice (5).chain.chip().cycle() == cycles);
}

TEST_CASE ("Part idle: 30 s after its note-off a voice with a running wave lane goes idle", "[bside][part][idle]")
{
    // a wave lane that loops for ever: the envelope rule cannot make the voice idle (8.5)
    const auto prog = compile ("bside-table 1\n[wave1]\n    wave pulse+gate +0\nloop:\n    pitch +0\n    pitch +12\n    jump loop\n");
    bs::PartParams p;
    p.osc1Release = 0;
    Rig rig ({ 48000.0, 2 }, p);
    rig.part->setProgram (&prog);
    rig.run (4800, { on (0, 60) });
    rig.run (480, { off (0, 60) });
    rig.run (static_cast<int> (29.9 * 48000.0));
    CHECK (rig.part->numActiveVoices() == 1);
    CHECK_FALSE (rig.part->getVoice (0).idleSent);
    rig.run (static_cast<int> (0.2 * 48000.0));
    CHECK (rig.part->getVoice (0).idleSent);
    rig.run (static_cast<int> (0.6 * 48000.0));
    CHECK (rig.part->numActiveVoices() == 0);
}

TEST_CASE ("Part idle: a voice that does not fall silent leaves after 500 ms with a 3 ms fade", "[bside][part][idle]")
{
    // Model A with volume DC on: after the envelopes reach 0 the output keeps the board high-pass's slow tail of the
    // voice's DC change, above -120 dB for longer than 500 ms
    bs::PartParams p;
    p.chipModel = 0;
    p.chipVolumeDc = 1;
    p.osc1Release = 0;
    Rig rig ({ 48000.0, 2 }, p);
    rig.run (4800, { on (0, 60) });
    rig.run (48, { off (0, 60) });
    int idleAt = -1, leftAt = -1;
    for (int ms = 0; ms < 1000 && leftAt < 0; ++ms)
    {
        rig.run (48);
        if (idleAt < 0 && rig.part->getVoice (0).idleSent)
            idleAt = ms;
        if (rig.part->numActiveVoices() == 0)
            leftAt = ms;
    }
    INFO ("idle " << idleAt << " ms after the note-off, left " << leftAt << " ms after it");
    REQUIRE (idleAt >= 0);
    REQUIRE (leftAt > 0);
    CHECK (leftAt - idleAt >= 500);
    CHECK (leftAt - idleAt <= 505);
}

// ---- the settled start (8.21.1) ----------------------------------------------------------------------------------

namespace
{
/** One chip's chain with osc 1 pulse at C4, gate off, the given routing; volume DC on; mode low-pass, volume 15. */
std::unique_ptr<chip::ChipVoice> settledChain (chip::Model m, bool routed, std::unique_ptr<chip::Filter>& f)
{
    auto v = std::make_unique<chip::ChipVoice>();
    chip::Config cfg;
    cfg.model = m;
    cfg.internalRate = 96000.0;
    cfg.volumeDc = true;
    v->prepare (cfg);
    f = std::make_unique<chip::Filter>();
    f->prepare (m, 96000.0, 0.5f);
    v->setFilter (f.get());
    chip::OscInput in;
    in.frequency = 4389.0; // about C4
    in.pulseWidth = 2048.0f;
    in.control = 0x40; // pulse, gate off
    in.attackDecay = 0xf0;
    in.sustainRelease = 0xf0; // release 0: the power-on envelope (0xaa) is at 0 within milliseconds
    v->setOscillator (0, in);
    v->write (0x15, 0);
    v->write (0x16, 0x80);
    v->write (0x17, routed ? 0x01 : 0x00);
    v->write (0x18, 0x1f);
    return v;
}
} // namespace

TEST_CASE ("Part: a voice that starts from idle starts settled: the chain primed on its static level equals a chain run long at it", "[bside][part][settled]")
{
    // ChipVoice::suspend (the part's idle) against a chain that rendered the same static state for 3 s: the same chip,
    // the same note at the same cycle (attack 15, volume DC on). Model A's case of 8.21.1 ("nothing above -80 dB re one
    // full-level voice in its first 50 ms beyond its own envelope"): the difference is what the priming leaves.
    for (const auto m : { chip::Model::A, chip::Model::B })
        for (const bool routed : { false, true })
        {
            const int idle = 3 * 96000, note = 4800; // 50 ms
            std::unique_ptr<chip::Filter> fa, fb, ff;
            auto a = settledChain (m, routed, fa); // the part's way: suspended, skipped, primed at its first sample
            a->suspend();
            a->skipSamples (idle);
            auto b = settledChain (m, routed, fb); // run long at the static level
            std::vector<double> warm (static_cast<std::size_t> (idle), 0.0);
            b->renderUntil (warm.data(), idle, ~std::uint64_t { 0 });
            REQUIRE (a->chip().cycle() == b->chip().cycle());
            UNSCOPED_INFO ("env a " << static_cast<int> (a->chip().envelope (0)) << " b " << static_cast<int> (b->chip().envelope (0)) << ", states equal "
                                    << (a->chip().state() == b->chip().state() ? 1 : 0));
            for (auto* v : { a.get(), b.get() })
            {
                chip::OscInput in;
                in.frequency = 4389.0;
                in.pulseWidth = 2048.0f;
                in.control = 0x41; // the gate: attack 15
                in.attackDecay = 0xf0;
                in.sustainRelease = 0xff;
                v->setOscillator (0, in);
            }
            std::vector<double> ya (static_cast<std::size_t> (note), 0.0), yb (static_cast<std::size_t> (note), 0.0);
            a->renderUntil (ya.data(), note, ~std::uint64_t { 0 });
            b->renderUntil (yb.data(), note, ~std::uint64_t { 0 });
            // one full-level voice: the same chain with attack 0, its peak over 50 ms after 50 ms
            auto full = settledChain (m, routed, ff);
            chip::OscInput in;
            in.frequency = 4389.0;
            in.pulseWidth = 2048.0f;
            in.control = 0x41;
            in.attackDecay = 0x00;
            in.sustainRelease = 0xf0;
            full->setOscillator (0, in);
            std::vector<double> yf (static_cast<std::size_t> (2 * note), 0.0);
            full->renderUntil (yf.data(), 2 * note, ~std::uint64_t { 0 });
            double peak = 0.0, diff = 0.0;
            for (std::size_t i = static_cast<std::size_t> (note); i < yf.size(); ++i)
                peak = std::max (peak, std::abs (yf[i]));
            std::size_t worstAt = 0;
            for (std::size_t i = 0; i < ya.size(); ++i)
                if (std::abs (ya[i] - yb[i]) > diff)
                {
                    diff = std::abs (ya[i] - yb[i]);
                    worstAt = i;
                }
            UNSCOPED_INFO ("worst at " << worstAt << ": " << ya[worstAt] << " vs " << yb[worstAt] << "; at 0: " << ya[0] << " vs " << yb[0] << "; at 100: " << ya[100] << " vs " << yb[100]);
            INFO ("Model " << (m == chip::Model::A ? "A" : "B") << (routed ? ", through the filter" : ", direct") << ": full-level peak " << peak
                           << ", primed against settled: " << db (diff / peak) << " dB");
            REQUIRE (peak > 0.0);
            CHECK (diff <= 1.0e-4 * peak);
        }
}

TEST_CASE ("Part: a voice that starts from idle holds nothing above -80 dB while its envelope is at 0 (Model A, volume DC on)", "[bside][part][settled]")
{
    // Through the part: the first note on a voice and a note after it went idle, Per Note and Shared, direct and through
    // the filter. Host sample by host sample, as long as the voice's envelope counter is still 0 (a latency later), the
    // output must stay below -80 dB re one full-level voice (attack 15: the first step comes up to 31.7 ms later).
    for (const bool shared : { false, true })
        for (const int routed : { 0, 1 })
        {
            bs::PartParams full;
            full.chipModel = 0;
            full.chipVolumeDc = 1;
            full.osc1Filter = routed;
            full.filterTopology = shared ? 1 : 0;
            full.osc1Release = 15;
            full.velVolume = 0.0f;
            Rig ref ({ 48000.0, 2 }, full);
            const double fullPeak = peakOf (ref.run (24000, { on (0, 60) }), 4800);
            REQUIRE (fullPeak > 0.05);

            bs::PartParams slow = full;
            slow.osc1Attack = 15;
            Rig rig ({ 48000.0, 2 }, slow, 1);
            const int lat = rig.part->getLatencySamples();
            auto window = [&] (int voice) {
                // renders until the voice's envelope moves (or 50 ms), then a latency more; the samples before that
                std::vector<float> y;
                int moved = -1;
                for (int i = 0; i < 2400; ++i)
                {
                    y.push_back (rig.run (1)[0]);
                    if (moved < 0 && rig.part->getVoice (voice).chain.chip().envelope (0) > 0)
                        moved = i;
                }
                const int end = moved < 0 ? 2400 : std::max (0, moved + lat - 2);
                return std::make_pair (peakOf (y, 0, static_cast<std::size_t> (end)), end);
            };
            rig.run (1000);
            rig.run (0, { on (0, 60) });
            const auto first = window (0);
            rig.run (0, { off (0, 60) });
            rig.run (static_cast<int> (3.0 * 48000.0));
            REQUIRE (rig.part->numActiveVoices() == 0);
            rig.run (0, { on (0, 60) });
            const int second = rig.part->getVoice (0).active ? 0 : 1;
            const auto after = window (second);
            INFO ((shared ? "Shared" : "Per Note") << (routed ? ", through the filter" : ", direct") << ": first note " << db (first.first / fullPeak) << " dB over "
                                                   << first.second << " samples; after idle " << db (after.first / fullPeak) << " dB over " << after.second << " samples");
            CHECK (first.first <= 1.0e-4 * fullPeak);
            CHECK (after.first <= 1.0e-4 * fullPeak);
            CHECK (first.second > 200); // the window holds more than the latency
            CHECK (after.second > 200);
        }
}

// ---- Shared (8.21.1) ----------------------------------------------------------------------------------------------

TEST_CASE ("Part Shared: one voice in Shared equals Per Note bit for bit at the part's output, the cutoff moving", "[bside][part][shared]")
{
    // a filter lane sweeps the cutoff every tick (moving: run per sample) and the host moves it too
    const auto prog = compile ("bside-table 1\n[filter]\n    cutoff 1500\n    sweep -50 x20\n    sweep +30 x20\n");
    for (const int model : { 1, 0 })
        for (const int resolution : { 0, 1 })
        {
            std::vector<float> y[2];
            for (const int topology : { 0, 1 })
            {
                bs::PartParams p;
                p.chipModel = model;
                p.chipResolution = resolution;
                p.filterTopology = topology;
                p.osc2Wave = 2;
                p.osc2Filter = 1;
                p.filterResonance = 9;
                p.osc1Release = p.osc2Release = 0;
                Rig rig ({ 48000.0, 2 }, p, 512);
                rig.part->setProgram (&prog);
                // one voice at a time: the first note goes idle before the second starts (on another voice)
                auto a = rig.run (60000, { on (100, 57), off (9000, 57) });
                REQUIRE (rig.part->numActiveVoices() == 0);
                p.filterCutoff = 700.0f; // a host move: smoothed, written at the control points
                rig.set (p);
                auto b = rig.run (24000, { on (2000, 64), off (15000, 64) });
                a.insert (a.end(), b.begin(), b.end());
                y[topology] = a;
            }
            INFO ("model " << (model == 0 ? "A" : "B") << ", resolution " << resolution);
            REQUIRE (peakOf (y[0]) > 0.01);
            CHECK (y[1] == y[0]);
        }
}

TEST_CASE ("Part Shared: several voices share one filter (their output differs from Per Note), finite, and the filter follows the most recent note", "[bside][part][shared]")
{
    for (const int model : { 1, 0 })
    {
        std::vector<float> y[2];
        for (const int topology : { 0, 1 })
        {
            bs::PartParams p;
            p.chipModel = model;
            p.filterTopology = topology;
            p.osc1Wave = 2;
            p.filterResonance = 10;
            p.filterCutoff = 600.0f;
            p.filterKeyFollow = 100.0f; // each note its own cutoff: the shared one is the last note's
            Rig rig ({ 48000.0, 2 }, p);
            y[topology] = rig.run (24000, { on (0, 48), on (4000, 60), on (8000, 72), off (16000, 48), off (16000, 60), off (16000, 72) });
        }
        double diff = 0.0, peak = 0.0;
        for (std::size_t i = 0; i < y[0].size(); ++i)
        {
            REQUIRE (std::isfinite (y[1][i]));
            diff = std::max (diff, std::abs (static_cast<double> (y[1][i]) - static_cast<double> (y[0][i])));
            peak = std::max (peak, std::abs (static_cast<double> (y[1][i])));
        }
        INFO ("model " << (model == 0 ? "A" : "B") << ": Shared differs from Per Note by " << db (diff / peak) << " dB re its peak");
        CHECK (peak > 0.01);
        CHECK (diff > 1e-3 * peak);
    }
}

TEST_CASE ("Part Shared: the filter takes the most recent note's cutoff even when its engine writes nothing new", "[bside][part][shared]")
{
    // Key follow gives notes 48 and 72 their own cutoffs. Every voice plays once (All Sound Off makes each idle at
    // once, so the next note takes the next voice); the 17th note lands on voice 0 again with the note it had: its
    // engine finds its cutoff unchanged and writes none, while the shared filter still holds the 16th note's.
    bs::PartParams p;
    p.filterTopology = 1;
    p.filterKeyFollow = 100.0f;
    p.filterCutoff = 1000.0f;
    Rig rig ({ 48000.0, 2 }, p);
    for (int k = 0; k < 17; ++k)
    {
        rig.run (960, { on (0, k % 2 == 0 ? 48 : 72) });
        if (k < 16)
            rig.run (0, { control (0, vs::EventType::AllSoundOff) });
    }
    REQUIRE (rig.part->getVoice (0).active);
    const double want = 1000.0 + 16.0 * (48 - 60);
    CHECK (static_cast<double> (rig.part->getVoice (0).chain.chip().cutoffSmooth()) == Catch::Approx (want).margin (0.01));
    CHECK (static_cast<double> (rig.part->getSharedFilter().cutoff()) == Catch::Approx (want).margin (0.01));
}

// ---- structural switches (8.21.1) --------------------------------------------------------------------------------

TEST_CASE ("Part: a structural switch fades the sounding voices over 3 ms, resets them into the new configuration; held keys do not retrigger", "[bside][part][switch]")
{
    struct Sw
    {
        const char* what;
        void (*apply) (bs::PartParams&, int&);
    };
    const std::vector<Sw> switches {
        { "chip.model", [] (bs::PartParams& p, int&) { p.chipModel = 0; } },
        { "filter.topology", [] (bs::PartParams& p, int&) { p.filterTopology = 1; } },
        { "chip.clock", [] (bs::PartParams&, int& c) { c = 1; } },
        { "chip.resolution", [] (bs::PartParams& p, int&) { p.chipResolution = 1; } },
        { "chip.volume_dc", [] (bs::PartParams& p, int&) { p.chipVolumeDc = 1; } },
    };
    for (const auto& s : switches)
    {
        bs::PartParams p;
        int clock = 0;
        Rig rig ({ 48000.0, 2 }, p);
        rig.run (4800, { on (0, 60), on (0, 64) });
        REQUIRE (rig.part->numActiveVoices() == 2);
        const auto before = rig.run (480);
        s.apply (p, clock);
        rig.set (p);
        rig.part->setChipClock (clock);
        const auto during = rig.run (480); // 10 ms: the 3 ms fade (288 internal samples = 144 host samples), then silence
        INFO (s.what);
        CHECK (rig.part->switchesDone() == 1);
        CHECK (rig.part->numActiveVoices() == 0); // held keys do not retrigger
        CHECK_FALSE (rig.part->getAllocator().isVoiceBusy (0));
        // no click: the fade is smooth (the largest step between samples no larger than the note's own)
        double ownStep = 0.0, fadeStep = 0.0;
        for (std::size_t i = 1; i < before.size(); ++i)
            ownStep = std::max (ownStep, std::abs (static_cast<double> (before[i]) - static_cast<double> (before[i - 1])));
        for (std::size_t i = 1; i < during.size(); ++i)
            fadeStep = std::max (fadeStep, std::abs (static_cast<double> (during[i]) - static_cast<double> (during[i - 1])));
        CHECK (fadeStep <= ownStep * 1.1);
        CHECK (peakOf (during, 200) < 1e-3 * peakOf (before)); // after the fade and the latency
        // the new configuration plays
        const auto after = rig.run (9600, { on (0, 67) });
        CHECK (peakOf (after) > 0.01);
        const auto& cfg = rig.part->getVoice (0).chain.config();
        if (std::string (s.what) == "chip.model")
            CHECK (cfg.model == chip::Model::A);
        if (std::string (s.what) == "chip.clock")
            CHECK (cfg.clockHz == 1022727u);
        if (std::string (s.what) == "chip.resolution")
            CHECK (cfg.authentic);
        if (std::string (s.what) == "chip.volume_dc")
            CHECK (cfg.volumeDc);
    }
}

TEST_CASE ("Part: a note played during a switch's fade starts after it, in the new configuration, and keeps its length", "[bside][part][switch]")
{
    bs::PartParams p;
    p.osc1Release = 0;
    Rig rig ({ 48000.0, 2 }, p);
    rig.run (4800, { on (0, 60) });
    p.chipModel = 0;
    rig.set (p);
    rig.run (48, { on (10, 67) }); // the switch fades for 3 ms; the note waits for it
    CHECK (rig.part->getVoice (1).pending);
    rig.run (480);
    CHECK (rig.part->switchesDone() == 1);
    CHECK (rig.part->numActiveVoices() == 1);
    CHECK (rig.part->getVoice (1).note == 67);
    CHECK (rig.part->getVoice (1).chain.config().model == chip::Model::A);
    rig.run (4800, { off (0, 67) });
    rig.run (48000);
    CHECK (rig.part->numActiveVoices() == 0);
}

TEST_CASE ("Part: a note that starts late after a steal's fade keeps its length, released while it waits or after its start", "[bside][part][switch]")
{
    // Poly at 96 kHz x1 (one host sample is one internal sample; the fade is 288): 16 held notes, the 17th steals a
    // voice: the old note fades for 3 ms, then the new one starts. Its release comes as much later as it started (8.21.1
    // after the review), whether the key went up during the fade or after it.
    for (const int length : { 0, 1, 20, 287, 288, 400, 2000 })
    {
        bs::PartParams p;
        p.osc1Release = 9;
        Rig rig ({ 96000.0, 1 }, p, 1);
        std::vector<vs::NoteEvent> chord;
        for (int k = 0; k < 16; ++k)
            chord.push_back (on (k, 40 + k));
        rig.run (2000, chord, 1);
        REQUIRE (rig.part->numActiveVoices() == 16);
        int started = -1, released = -1, voice = -1;
        for (int i = 0; i < 3000; ++i)
        {
            std::vector<vs::NoteEvent> ev;
            if (i == 0)
                ev.push_back (on (0, 90));
            if (i == length)
                ev.push_back (off (0, 90));
            rig.run (1, ev, 1);
            for (int k = 0; k < bs::Part::kVoices && voice < 0; ++k)
                if (rig.part->getVoice (k).pending && rig.part->getVoice (k).pendingAction.note == 90)
                    voice = k;
            REQUIRE (voice >= 0); // a steal: the note waits for the fade
            const auto& v = rig.part->getVoice (voice);
            if (started < 0 && ! v.pending && v.note == 90)
                started = i;
            if (started >= 0 && released < 0 && ! v.held)
                released = i;
        }
        INFO ("played " << length << " samples: started at +" << started << ", released at +" << released);
        REQUIRE (started >= 0);
        REQUIRE (released >= 0);
        CHECK (started == 287); // the steal's fade: the 288th sample after the note-on is the new note's first
        CHECK (std::abs ((released - started) - length) <= 1);
        CHECK_FALSE (rig.part->getVoice (voice).late);
    }
}

TEST_CASE ("Part: a note played inside a switch's fade and released after its start keeps its length", "[bside][part][switch]")
{
    // the reviewer's test releases the note while it still waits; here the key goes up after the note has started, so
    // the release is delayed by the start's own delay (288 - into), not by a whole fade
    for (const int into : { 0, 100, 250, 287 })
    {
        bs::PartParams p;
        p.osc1Release = 9;
        Rig rig ({ 96000.0, 1 }, p, 1);
        rig.run (2000, { on (0, 45) }, 1);
        p.chipModel = 0;
        rig.set (p);
        rig.run (into, {}, 1);
        const int length = 1000;
        int started = -1, released = -1, voice = -1;
        for (int i = 0; i < 3000; ++i)
        {
            std::vector<vs::NoteEvent> ev;
            if (i == 0)
                ev.push_back (on (0, 69));
            if (i == length)
                ev.push_back (off (0, 69));
            rig.run (1, ev, 1);
            for (int k = 0; k < bs::Part::kVoices && voice < 0; ++k)
                if ((rig.part->getVoice (k).pending && rig.part->getVoice (k).pendingAction.note == 69)
                    || (rig.part->getVoice (k).active && rig.part->getVoice (k).note == 69))
                    voice = k;
            REQUIRE (voice >= 0);
            const auto& v = rig.part->getVoice (voice);
            if (started < 0 && v.active && v.note == 69)
                started = i;
            if (started >= 0 && released < 0 && ! v.held)
                released = i;
        }
        INFO ("note-on " << into << " samples into the fade: started at +" << started << ", released at +" << released);
        REQUIRE (started >= 0);
        REQUIRE (released >= 0);
        CHECK (started == 288 - into - 1);
        CHECK (std::abs ((released - started) - length) <= 1);
    }
}

TEST_CASE ("Part: a delay does not outlive its note: a late start, then a legato return in Solo, and the release is on time", "[bside][part][switch]")
{
    // Solo: a key held, a structural switch, and a second key played 100 samples into its fade: that note starts 188
    // samples late. Its key goes up while the first is still down (a legato return to the first key, no release); the
    // first key's release then comes at its event, not 188 samples later (8.21.1 after the review).
    bs::PartParams p;
    p.assignMode = 2;
    p.osc1Release = 9;
    Rig rig ({ 96000.0, 1 }, p, 1);
    rig.run (2000, { on (0, 48) }, 1);
    p.chipModel = 0;
    rig.set (p);
    rig.run (100, {}, 1);
    rig.run (1000, { on (0, 60) }, 1);
    const auto& v = rig.part->getVoice (0);
    REQUIRE (v.active);
    REQUIRE (v.note == 60);
    CHECK (v.late);
    CHECK (v.startDelay == 188);
    rig.run (1000, { off (0, 60) }, 1); // key 48 is still down: a legato return
    REQUIRE (v.note == 48);
    REQUIRE (v.held);
    CHECK_FALSE (v.late); // the late note is over
    rig.run (1, { off (0, 48) }, 1);
    CHECK_FALSE (v.held); // released at its event
}

// ---- random event streams (8.21.1, behaviour gate 4) ---------------------------------------------------------------

namespace
{
void randomStream (std::uint32_t seed, int blocks)
{
    std::mt19937 rng (seed);
    const auto tables = std::vector<vs::bside::program::Program> { compile (kArpTable), compile (kPhaseTable), vs::bside::program::emptyProgram(),
                                                                  compile ("bside-table 1\n[restart]\nmode classic\nlead 2\n[wave1]\n    adsr 0 9 0 9, wave noise+gate =C#7\n    wave pulse+gate =A3\n    end\n") };
    bs::PartParams p;
    p.osc1Release = 3;
    Rig rig ({ 48000.0, 2 }, p, 256);
    rig.part->setProgram (&tables[0]);
    for (int b = 0; b < blocks; ++b)
    {
        std::vector<vs::NoteEvent> ev;
        const int n = static_cast<int> (rng() % 6);
        for (int k = 0; k < n; ++k)
        {
            const int at = static_cast<int> (rng() % 256);
            const auto kind = rng() % 10;
            const int note = 36 + static_cast<int> (rng() % 48);
            if (kind < 4)
                ev.push_back (on (at, note, static_cast<float> (rng() % 100) / 100.0f));
            else if (kind < 8)
                ev.push_back (off (at, note));
            else if (kind == 8)
                ev.push_back (control (at, vs::EventType::SustainPedal, (rng() % 2) == 0 ? 1.0f : 0.0f));
            else
                ev.push_back (control (at, vs::EventType::PitchBend, static_cast<float> (rng() % 200) / 100.0f - 1.0f));
        }
        std::sort (ev.begin(), ev.end(), [] (const vs::NoteEvent& a, const vs::NoteEvent& c) { return a.offset < c.offset; });
        if (rng() % 40 == 0)
            p.assignMode = static_cast<int> (rng() % 3);
        if (rng() % 40 == 0)
            p.unisonVoices = static_cast<int> (rng() % 4);
        if (rng() % 60 == 0)
            p.chipModel = static_cast<int> (rng() % 2);
        if (rng() % 60 == 0)
            p.filterTopology = static_cast<int> (rng() % 2);
        if (rng() % 80 == 0)
            p.chipResolution = static_cast<int> (rng() % 2);
        if (rng() % 80 == 0)
            p.chipVolumeDc = static_cast<int> (rng() % 2);
        if (rng() % 80 == 0)
            rig.part->setChipClock (static_cast<int> (rng() % 2));
        if (rng() % 30 == 0)
            p.restartPolicy = static_cast<int> (rng() % 3);
        if (rng() % 50 == 0)
            rig.part->setProgram (&tables[rng() % tables.size()]);
        rig.set (p);
        const auto y = rig.run (256, ev, 256);
        for (const float s : y)
            REQUIRE (std::isfinite (s));
    }
    // everything off: the part reaches silence and every voice goes idle
    rig.run (256, { control (0, vs::EventType::SustainPedal, 0.0f), control (0, vs::EventType::AllNotesOff) });
    rig.run (static_cast<int> (31.0 * 48000.0)); // the 30 s rule bounds a looping lane
    INFO ("seed " << seed);
    CHECK (rig.part->numActiveVoices() == 0);
    CHECK_FALSE (rig.part->isSounding());
    for (int v = 0; v < bs::Part::kVoices; ++v)
        CHECK_FALSE (rig.part->getAllocator().isVoiceBusy (v));
    CHECK (peakOf (rig.run (4800)) <= 0.0); // exactly 0
}
} // namespace

TEST_CASE ("Part: random event streams (notes, pedal, modes, switches, tables) never hang a note; afterwards every voice is idle", "[bside][part][random]")
{
    randomStream (1u, 400);
}

TEST_CASE ("Part: random event streams, more seeds and longer", "[.part-full][bside][part][random]")
{
    for (std::uint32_t seed = 2; seed < 12; ++seed)
        randomStream (seed, 2000);
}

