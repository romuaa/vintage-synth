// The replay driver (Replay.h) with the real chip (ChipReplay.h). The references here are generated
// by the chip's own per-cycle definition (stepCycle): this proves the plumbing (setup, schedule,
// snapshot field order and ranges, the fast path under the driver), not authenticity. The committed
// reference data of the reference emulator is replayed by bside_trace (tests/bside/trace/).

#include "FuzzSupport.h"

#include "bside/chip/Chip.h"
#include "bside/trace/ChipReplay.h"
#include "bside/trace/Sha256.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace vs::bside;
using namespace vs::bside::trace;

namespace
{
std::string bytesOf (const Trace& t)
{
    std::string text;
    REQUIRE (writeTrace (t, text));
    return text;
}

/** A reference written by stepping the chip one cycle at a time (an independent path through the chip). */
Reference stepperReference (const Trace& t, std::string_view bytes)
{
    std::vector<Checkpoint> schedule;
    REQUIRE (checkpointSchedule (t, schedule));
    chip::Chip c;
    setupChip (c, t);
    Reference r;
    r.traceSha256 = sha256Hex (bytes);
    r.adapter = std::string (40, 'a');
    r.emulator = std::string (64, 'b');
    for (const auto& cp : schedule)
    {
        const std::uint64_t target = cp.kind == Checkpoint::Kind::afterWrite ? t.writes[cp.write].cycle : cp.cycle;
        while (c.cycle() < target)
            c.stepCycle();
        if (cp.kind == Checkpoint::Kind::afterWrite)
            c.write (t.writes[cp.write].reg, t.writes[cp.write].value);
        r.points.push_back ({ cp.cycle, snapshotChip (c) });
    }
    return r;
}

Trace randomTrace (std::uint64_t seed, Model model)
{
    bst::Rng r (seed);
    Trace t;
    t.clockHz = r.chance (50) ? 985248 : 1022727;
    t.model = model;
    t.check = 20000;
    std::uint64_t at = 0;
    for (int i = 0; i < 400; ++i)
    {
        at += r.chance (30) ? 0 : r.below (6000);
        const auto reg = static_cast<std::uint8_t> (r.below (0x19));
        std::uint8_t v = static_cast<std::uint8_t> (r.below (256));
        if (reg % 7 == 4 && reg < 21 && r.chance (80))
            v &= static_cast<std::uint8_t> (~0x08u); // mostly without the test bit
        t.writes.push_back ({ at, reg, v });
    }
    t.endCycle = at + 300000;
    return t;
}
} // namespace

TEST_CASE ("chip replay: the contract's example trace replays against a per-cycle reference", "[bside][chip][replay]")
{
    const std::string text = "bside-trace 1\nclock 985248\nmodel A\ncheck 4096\n0 18 0f\n0 05 09\n40 04 11\nend 2000000\n";
    const auto parsed = readTrace (text);
    REQUIRE (parsed.ok);
    const auto ref = stepperReference (parsed.trace, text);
    std::string why;
    REQUIRE (validateReference (ref, &why));
    chip::Chip c;
    const auto r = replayChip (c, parsed.trace, text, ref);
    INFO (describe (r));
    CHECK (r.ok);
    CHECK (r.compared == ref.points.size());
    CHECK (c.cycle() == 2000000);
    // Gate on with attack 0 (9 cycles) and decay 9: by the end the envelope has risen and decayed to sustain 0.
    CHECK (ref.points.back().state.v[8] == 0); // env3 is untouched
    CHECK (ref.points.back().state.v[6] == 0); // env1: attack then decay to sustain 0
}

TEST_CASE ("chip replay: random traces, both models, every field in range and the reference round-trips", "[bside][chip][replay]")
{
    for (std::uint64_t seed = 1; seed <= 6; ++seed)
    {
        const auto model = seed % 2 == 0 ? Model::A : Model::B;
        const auto t = randomTrace (seed, model);
        const auto bytes = bytesOf (t);
        const auto ref = stepperReference (t, bytes);
        std::string why;
        REQUIRE (validateReference (ref, &why)); // every field within kStateFieldMax
        std::string text;
        REQUIRE (writeReference (ref, text));
        const auto back = readReference (text);
        REQUIRE (back.ok);

        chip::Chip c;
        const auto r = replayChip (c, t, bytes, back.reference);
        INFO ("seed " << seed << ": " << describe (r));
        CHECK (r.ok);
        CHECK (r.mismatchedPoints == 0);
        CHECK (r.compared == ref.points.size());
        CHECK (c.config().clockHz == t.clockHz);
        CHECK (c.config().model == chipModel (model));
    }
}

TEST_CASE ("chip replay: a differing field is reported by name", "[bside][chip][replay]")
{
    const auto t = randomTrace (11, Model::B);
    const auto bytes = bytesOf (t);
    auto ref = stepperReference (t, bytes);
    const std::size_t at = ref.points.size() / 2;
    ref.points[at].state.v[13] ^= 1; // rate2
    chip::Chip c;
    const auto r = replayChip (c, t, bytes, ref);
    REQUIRE_FALSE (r.ok);
    REQUIRE (r.first.size() == 1);
    CHECK (r.first[0].checkpoint == at);
    CHECK (std::string (kStateFieldNames[r.first[0].field]) == "rate2");
    CHECK (r.mismatchedPoints == 1);
}

TEST_CASE ("chip replay: the snapshot puts each field where the reference format says", "[bside][chip][replay]")
{
    chip::Chip c;
    chip::Config cfg;
    cfg.authentic = true;
    c.prepare (cfg);
    c.write (0x0e, 0x00);
    c.write (0x0f, 0x10); // osc3 F = 0x1000
    c.write (0x13, 0x20); // osc3 attack 63
    c.write (0x14, 0xf0);
    c.write (0x12, chip::ctrl::saw | chip::ctrl::gate);
    c.write (0x00, 0x00);
    c.write (0x01, 0x01); // osc1 F = 0x100
    c.advanceCycles (1000);
    const auto v = snapshotChip (c);
    const auto s = c.state();
    CHECK (v.v[0] == 0x555555u + 1000u * 0x100u); // power-on accumulator 0x555555 (8.15.1)
    CHECK (v.v[2] == (0x555555u + 1000u * 0x1000u) % 0x1000000u);
    CHECK (v.v[3] == s.osc[0].noise);
    CHECK (v.v[5] == s.osc[2].noise);
    CHECK (v.v[8] == s.env[2].counter);
    CHECK (v.v[11] == 0); // attack
    CHECK (v.v[9] == 2);  // osc1 envelope: release
    CHECK (v.v[14] == s.env[2].position);
    CHECK (v.v[17] == (s.env[2].divider | (s.env[2].resetPending ? 0x80u : 0u)));
    CHECK (v.v[18] == static_cast<std::uint32_t> (s.osc[2].readValue >> 4));
    CHECK (v.v[19] == s.env[2].readSample);
}

namespace
{
/** The 20 fields as the reference format names them, taken from DigitalState by name. */
std::array<std::uint32_t, kStateFields> fieldsByName (const chip::DigitalState& s)
{
    const auto& o = s.osc;
    const auto& e = s.env;
    auto div = [] (const chip::EnvState& x) { return static_cast<std::uint32_t> (x.divider) | (x.resetPending ? 0x80u : 0u); };
    return { o[0].acc,      o[1].acc,      o[2].acc,     o[0].noise,  o[1].noise,  o[2].noise,   e[0].counter,
             e[1].counter,  e[2].counter,  e[0].phase,   e[1].phase,  e[2].phase,  e[0].position,
             e[1].position, e[2].position, div (e[0]),   div (e[1]),  div (e[2]),  s.osc3Output, s.env3Output };
}

/** Every pair of fields differs and no field is 0, except the fields `exempt` names (and env3 = rd1C,
    the same counter by definition). */
bool allDistinct (const std::array<std::uint32_t, kStateFields>& v, bool (*exempt) (int))
{
    for (int i = 0; i < kStateFields; ++i)
    {
        if (exempt (i))
            continue;
        const bool isPhase = i >= 9 && i <= 11; // 0 is a phase value like the others (attack)
        if (v[static_cast<std::size_t> (i)] == 0 && ! isPhase)
            return false;
        for (int j = i + 1; j < kStateFields; ++j)
        {
            const bool sameByDefinition = i == 8 && j == 19;
            if (! exempt (j) && ! sameByDefinition && v[static_cast<std::size_t> (i)] == v[static_cast<std::size_t> (j)])
                return false;
        }
    }
    return true;
}

/** Steps `c` until every field has its own value (within the exemptions), then checks the snapshot. */
void searchAndCheck (chip::Chip& c, bool (*exempt) (int), std::uint64_t maxCycles, bool (*extra) (const chip::DigitalState&) = nullptr)
{
    bool found = false;
    for (std::uint64_t i = 0; i < maxCycles && ! found; ++i)
    {
        c.stepCycle();
        const auto s = c.state();
        found = allDistinct (fieldsByName (s), exempt) && (extra == nullptr || extra (s));
    }
    REQUIRE (found);
    const auto expected = fieldsByName (c.state());
    const auto v = snapshotChip (c);
    for (int k = 0; k < kStateFields; ++k)
    {
        INFO ("field " << kStateFieldNames[k] << " at cycle " << c.cycle());
        CHECK (v.v[static_cast<std::size_t> (k)] == expected[static_cast<std::size_t> (k)]);
        CHECK (v.v[static_cast<std::size_t> (k)] <= kStateFieldMax[k]);
    }
}

chip::Chip snapshotChipUnderTest()
{
    chip::Chip c;
    chip::Config cfg;
    cfg.authentic = true;
    c.prepare (cfg);
    const int freq[3] = { 0x1234, 0x2b67, 0x3a09 };
    for (int n = 0; n < 3; ++n)
    {
        c.write (static_cast<std::uint8_t> (7 * n), static_cast<std::uint8_t> (freq[n] & 0xff));
        c.write (static_cast<std::uint8_t> (7 * n + 1), static_cast<std::uint8_t> (freq[n] >> 8));
    }
    return c;
}
} // namespace

TEST_CASE ("chip replay: the snapshot puts every field where the reference format says (all three phases)", "[bside][chip][replay]")
{
    // Envelope 1 in release, 2 in decay, 3 in a slow attack; every other field differs too. The divider
    // fields are exempt here (attack keeps its divider at 0); the next test covers them.
    auto c = snapshotChipUnderTest();
    c.write (0x05, 0x00);
    c.write (0x06, 0x02); // osc1: release 63
    c.write (0x0c, 0x0b); // osc2: attack 9, decay 3126
    c.write (0x0d, 0x10);
    c.write (0x13, 0xa0); // osc3: attack 1954 (stays in attack for about 500 000 cycles)
    c.write (0x14, 0xf0);
    c.write (0x04, chip::ctrl::gate);
    c.write (0x0b, chip::ctrl::gate);
    c.write (0x12, chip::ctrl::saw | chip::ctrl::gate);
    c.advanceCycles (255 * 9 + 600);
    c.write (0x04, 0); // osc1 into release
    searchAndCheck (c, [] (int k) { return k >= 15 && k <= 17; }, 3000000);
    const auto s = c.state();
    CHECK (s.env[0].phase == 2);
    CHECK (s.env[1].phase == 1);
    CHECK (s.env[2].phase == 0);
}

TEST_CASE ("chip replay: the snapshot puts every field where the reference format says (all three dividers)", "[bside][chip][replay]")
{
    // All three envelopes release from 0xff at different rates; low counters have divider periods of 2
    // to 30, so the three divider counters can all be non-zero and different. The phases (all release)
    // are exempt here; the previous test covers them.
    auto c = snapshotChipUnderTest();
    const int release[3] = { 3, 4, 5 };
    for (int n = 0; n < 3; ++n)
    {
        c.write (static_cast<std::uint8_t> (7 * n + 5), 0x00);
        c.write (static_cast<std::uint8_t> (7 * n + 6), static_cast<std::uint8_t> (release[n]));
        c.write (static_cast<std::uint8_t> (7 * n + 4), static_cast<std::uint8_t> (n == 2 ? chip::ctrl::saw | chip::ctrl::gate : chip::ctrl::gate));
    }
    c.advanceCycles (255 * 9);
    for (int n = 0; n < 3; ++n)
        c.write (static_cast<std::uint8_t> (7 * n + 4), static_cast<std::uint8_t> (n == 2 ? chip::ctrl::saw : 0));
    searchAndCheck (c, [] (int k) { return k >= 9 && k <= 11; }, 3000000, [] (const chip::DigitalState& st) {
        return st.env[0].divider != 0 && st.env[1].divider != 0 && st.env[2].divider != 0;
    });
    const auto s = c.state();
    for (std::size_t n = 0; n < 3; ++n)
        CHECK (s.env[n].divider != 0);
}

// ---- The shape exemption of 8.15.8 (ShapeExemption.h): where it starts and every way it ends ----
// No committed trace ends an exemption inside a trace, so these unit tests are the only evidence.

namespace
{
/** A chip stepped cycle by cycle with the exemption tracker attached, as replayChipExact runs it. */
struct ExemptionRig
{
    explicit ExemptionRig (chip::Model m) : tracker (m)
    {
        chip::Config cfg;
        cfg.model = m;
        cfg.authentic = true;
        c.prepare (cfg);
    }
    ExemptionRig (const ExemptionRig&) = delete;
    void write (int osc, int r, int v)
    {
        const auto a = static_cast<std::uint8_t> (osc * chip::reg::oscStride + r);
        c.write (a, static_cast<std::uint8_t> (v));
        tracker.write (a, static_cast<std::uint8_t> (v));
    }
    void freq (int osc, int f)
    {
        write (osc, chip::reg::freqLo, f & 0xff);
        write (osc, chip::reg::freqHi, f >> 8);
    }
    void step (std::uint64_t n = 1)
    {
        for (std::uint64_t i = 0; i < n; ++i)
            c.stepCycle (sink);
    }
    chip::Chip c;
    ShapeExemption tracker;
    ShapeExemptionSink sink { &tracker };
};
} // namespace

TEST_CASE ("exemption: a table is used for two of triangle, saw, pulse, or pulse with noise; noise with one of triangle or saw uses none", "[bside][chip][exemption]")
{
    using namespace chip::wave;
    for (int w = 0; w < 16; ++w)
    {
        const int tsp = (w & 1) + ((w >> 1) & 1) + ((w >> 2) & 1);
        const bool expected = tsp >= 2 || ((w & 4) != 0 && (w & 8) != 0);
        INFO ("waveform bits " << w);
        CHECK (usesTable (static_cast<std::uint8_t> (w)) == expected);
    }
    CHECK_FALSE (usesTable (kNoise | kTri));
    CHECK_FALSE (usesTable (kNoise | kSaw));
    // In the chip, noise + triangle and noise + saw output that shape AND the noise output.
    for (auto m : { chip::Model::A, chip::Model::B })
        for (int w : { kNoise | kTri, kNoise | kSaw })
        {
            ExemptionRig r (m);
            r.freq (2, 0x1d3b);
            r.write (2, chip::reg::control, (w << 4) | chip::ctrl::test);
            r.write (2, chip::reg::control, w << 4);
            for (int i = 0; i < 3000; ++i)
            {
                r.step();
                const auto acc = r.c.accumulator (2);
                const auto ix = index (acc, static_cast<std::uint8_t> (w << 4), 0);
                const std::uint16_t sh = (w & kTri) != 0 ? triangle (ix) : saw (ix);
                REQUIRE (r.c.output (2) == (sh & r.c.state().osc[2].noiseOutput));
                REQUIRE_FALSE (r.tracker.readExempt()); // no table: the read value is exact
                REQUIRE_FALSE (r.tracker.noiseExempt (2)); // the write-back of an exact output is exact
                REQUIRE_FALSE (r.tracker.accExempt (2)); // and on Model A no uncertain pull-down
            }
        }
}

TEST_CASE ("exemption: a test-bit reset ends an accumulator exemption", "[bside][chip][exemption]")
{
    // Model A, saw + triangle (a table): the pull-down is uncertain, so the accumulator is exempt.
    ExemptionRig r (chip::Model::A);
    r.freq (0, 0x1234);
    r.write (0, chip::reg::control, chip::ctrl::saw | chip::ctrl::triangle);
    r.step (100);
    REQUIRE (r.tracker.accExempt (0));
    r.write (0, chip::reg::control, chip::ctrl::saw | chip::ctrl::triangle | chip::ctrl::test);
    CHECK_FALSE (r.tracker.accExempt (0)); // acc := 0 at the write
    r.write (0, chip::reg::control, chip::ctrl::triangle);
    r.step (5000);
    CHECK_FALSE (r.tracker.accExempt (0)); // triangle alone: no table, no pull-down
    // Model B never pulls down: the same waveform exempts no accumulator.
    ExemptionRig b (chip::Model::B);
    b.freq (0, 0x1234);
    b.write (0, chip::reg::control, chip::ctrl::saw | chip::ctrl::triangle);
    b.step (100);
    CHECK_FALSE (b.tracker.accExempt (0));
}

TEST_CASE ("exemption: a sync reset from a certain source ends an accumulator exemption; an uncertain one starts it", "[bside][chip][exemption]")
{
    // Oscillator 2 (index 1) becomes exempt through its own uncertain pull-down, then plays triangle with
    // the sync bit; its source, oscillator 1, plays nothing and is exact, so its rise resets 2 for certain.
    ExemptionRig r (chip::Model::A);
    r.freq (1, 0x1234);
    r.write (1, chip::reg::control, chip::ctrl::saw | chip::ctrl::triangle);
    r.step (100);
    REQUIRE (r.tracker.accExempt (1));
    r.write (1, chip::reg::control, chip::ctrl::triangle | chip::ctrl::sync);
    r.freq (0, 0x3000);
    bool reset = false;
    for (int i = 0; i < 20000 && ! reset; ++i)
    {
        const bool before = r.tracker.accExempt (1);
        r.step();
        if (r.c.accumulator (1) == 0)
        {
            reset = true;
            CHECK (before);
            CHECK_FALSE (r.tracker.accExempt (1));
        }
    }
    CHECK (reset);
    // The same with an uncertain source (oscillator 1 itself pulls down through a table): the target stays
    // exempt, and a target that was exact becomes exempt.
    ExemptionRig u (chip::Model::A);
    u.freq (0, 0x3000);
    u.write (0, chip::reg::control, chip::ctrl::saw | chip::ctrl::triangle);
    u.freq (1, 0x0100);
    u.write (1, chip::reg::control, chip::ctrl::sync);
    REQUIRE_FALSE (u.tracker.accExempt (1));
    u.step (2);
    CHECK (u.tracker.accExempt (1));
    u.step (20000);
    CHECK (u.tracker.accExempt (1));
}

TEST_CASE ("exemption: a fill to all ones ends a noise-register exemption", "[bside][chip][exemption]")
{
    // Noise + pulse uses a table; its write-back makes the register exempt.
    for (int keepCombined = 0; keepCombined < 2; ++keepCombined)
    {
        ExemptionRig r (chip::Model::A);
        r.freq (0, 0x2345);
        r.write (0, chip::reg::control, chip::ctrl::noise | chip::ctrl::pulse);
        r.step (1000);
        REQUIRE (r.tracker.noiseExempt (0));
        // Under the test bit: with noise alone, or still combined (then the noise output takes the uncertain
        // output, but the register fills on its own and becomes exact).
        const int ctl = keepCombined != 0 ? chip::ctrl::noise | chip::ctrl::pulse : chip::ctrl::noise;
        r.write (0, chip::reg::control, ctl | chip::ctrl::test);
        const auto t = chip::timingFor (chip::Model::A);
        r.step (t.testHold);
        INFO ((keepCombined != 0 ? "noise + pulse" : "noise alone") << " under the test bit");
        CHECK (r.tracker.noiseExempt (0)); // one fill step is not enough
        while (r.c.noiseRegister (0) != chip::kNoiseMask)
        {
            REQUIRE (r.tracker.noiseExempt (0));
            r.step (t.fillInterval);
        }
        CHECK_FALSE (r.tracker.noiseExempt (0));
    }
}

TEST_CASE ("exemption: the read value's exemption ends with the next exact output (Model B: after the latch)", "[bside][chip][exemption]")
{
    for (auto m : { chip::Model::A, chip::Model::B })
    {
        ExemptionRig r (m);
        r.freq (2, 0x1234);
        r.write (2, chip::reg::control, chip::ctrl::saw | chip::ctrl::triangle);
        r.step (10);
        REQUIRE (r.tracker.readExempt());
        r.write (2, chip::reg::control, chip::ctrl::test); // acc := 0 ends the pull-down exemption (Model A)
        r.write (2, chip::reg::control, chip::ctrl::saw);
        r.step();
        // Model A: the read value is this step's exact output. Model B: it is the latch, which still holds
        // the table shape of the last saw + triangle step; the next step reads this step's saw.
        CHECK (r.tracker.readExempt() == (m == chip::Model::B));
        r.step();
        CHECK_FALSE (r.tracker.readExempt());
    }
}
