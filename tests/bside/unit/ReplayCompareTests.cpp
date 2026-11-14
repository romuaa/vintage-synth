// The replay driver and the comparer (docs/architecture.md 8.9.4), exercised against a stub chip
// defined here. The real chip arrives in S2; this proves the driver's order of operations, the
// checkpoint schedule, the mismatch report and the tolerance table.

#include "FuzzSupport.h"

#include "bside/trace/Compare.h"
#include "bside/trace/Replay.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace vs::bside::trace;

namespace
{
/** A tiny deterministic "chip": closed-form accumulators advanced by whole cycles, a few registers. */
struct StubChip
{
    std::uint8_t regs[32] {};
    std::uint64_t cyc = 0;
    std::uint64_t acc[3] {};
    std::uint32_t env[3] {};
    bool bug = false;           // an off-by-one in the accumulator advance for long spans
    int writesSeen = 0;

    void advanceCycles (std::uint64_t n) noexcept
    {
        for (int i = 0; i < 3; ++i)
        {
            const std::uint64_t f = regs[7 * i] | (static_cast<std::uint64_t> (regs[7 * i + 1]) << 8);
            acc[i] = (acc[i] + f * n + (bug && n > 1000 ? 1 : 0)) & 0xffffff;
            if ((regs[7 * i + 4] & 1) != 0)
                env[i] = static_cast<std::uint32_t> (std::min<std::uint64_t> (255, env[i] + n / 1000));
        }
        cyc += n;
    }
    void write (std::uint8_t reg, std::uint8_t value) noexcept
    {
        regs[reg] = value;
        ++writesSeen;
        for (int i = 0; i < 3; ++i)
            if (reg == 7 * i + 4 && (value & 8) != 0)
                acc[i] = 0; // test bit
    }
    std::uint64_t cycle() const noexcept { return cyc; }
};

void setup (StubChip& c, const Trace&)
{
    const bool bug = c.bug;
    c = StubChip {};
    c.bug = bug;
}

StateVector snapshot (const StubChip& c)
{
    StateVector s;
    for (int i = 0; i < 3; ++i)
    {
        const auto n = static_cast<std::size_t> (i);
        s.v[n] = static_cast<std::uint32_t> (c.acc[i]);
        s.v[3 + n] = static_cast<std::uint32_t> ((c.acc[i] >> 1) & 0x7fffff);
        s.v[6 + n] = c.env[i];
        s.v[9 + n] = (c.regs[7 * i + 4] & 1) != 0 ? 1u : 2u;
        s.v[12 + n] = static_cast<std::uint32_t> ((c.cyc + n) & 0x7fff);
        s.v[15 + n] = c.regs[7 * i + 5];
    }
    s.v[18] = static_cast<std::uint32_t> ((c.acc[2] >> 16) & 0xff);
    s.v[19] = c.env[2];
    return s;
}

/** What a reference emulator's adapter does, with the stub as the "emulator". */
Reference generate (const Trace& t, bool bug = false)
{
    std::vector<Checkpoint> schedule;
    REQUIRE (checkpointSchedule (t, schedule));
    StubChip chip;
    chip.bug = bug;
    Reference r;
    r.traceSha256 = std::string (64, '1');
    r.adapter = std::string (40, '2');
    r.emulator = std::string (40, '3');
    for (const auto& cp : schedule)
    {
        if (cp.kind == Checkpoint::Kind::afterWrite)
        {
            const auto& w = t.writes[cp.write];
            chip.advanceCycles (w.cycle - chip.cycle());
            chip.write (w.reg, w.value);
        }
        else
            chip.advanceCycles (cp.cycle - chip.cycle());
        r.points.push_back ({ cp.cycle, snapshot (chip) });
    }
    return r;
}

Trace sampleTrace()
{
    Trace t;
    t.clockHz = 985248;
    t.model = Model::A;
    t.check = 5000;
    t.writes = { { 0, 0x00, 0x34 }, { 0, 0x01, 0x12 }, { 0, 0x04, 0x11 }, { 7000, 0x07, 0x00 }, { 7000, 0x08, 0x20 },
                 { 7000, 0x0b, 0x01 }, { 9000, 0x04, 0x09 }, { 20000, 0x0e, 0xff }, { 20000, 0x0f, 0x01 }, { 30000, 0x12, 0x10 } };
    t.endCycle = 40000;
    return t;
}
} // namespace

TEST_CASE ("replay: a faithful chip matches its own reference", "[bside][replay]")
{
    const auto t = sampleTrace();
    const auto ref = generate (t);
    StubChip chip;
    const auto r = replay (chip, t, ref, setup, snapshot);
    INFO (describe (r));
    CHECK (r.ok);
    CHECK (r.error.empty());
    CHECK (r.compared == ref.points.size());
    CHECK (r.mismatchedPoints == 0);
    CHECK (chip.cycle() == t.endCycle);
    CHECK (chip.writesSeen == static_cast<int> (t.writes.size()));
}

TEST_CASE ("replay: a deviating chip is caught at the first differing checkpoint", "[bside][replay]")
{
    const auto t = sampleTrace();
    const auto ref = generate (t);
    StubChip chip;
    chip.bug = true;
    const auto r = replay (chip, t, ref, setup, snapshot);
    REQUIRE_FALSE (r.ok);
    REQUIRE (! r.first.empty());
    // The first span longer than 1000 cycles: from the writes at cycle 0 to the periodic checkpoint at 5000.
    const auto& m = r.first.front();
    CHECK (m.cycle == 5000);
    CHECK (std::string (kStateFieldNames[m.field]) == "acc1");
    CHECK (m.ours != m.expected);
    CHECK (r.mismatchedPoints > 0);
    CHECK (r.first.size() <= kMaxReportedMismatches);
    const auto text = describe (r);
    CHECK (text.find ("(cycle 5000): acc1") != std::string::npos);
    CHECK (text.find ("checkpoints differ") != std::string::npos);
}

TEST_CASE ("replay: at most eight differences are listed", "[bside][replay]")
{
    const auto t = sampleTrace();
    auto ref = generate (t);
    for (auto& p : ref.points)
        for (auto& v : p.state.v)
            v ^= 1; // every field of every checkpoint differs
    StubChip chip;
    const auto r = replay (chip, t, ref, setup, snapshot);
    REQUIRE_FALSE (r.ok);
    CHECK (r.first.size() == kMaxReportedMismatches);
    CHECK (r.mismatchedPoints == ref.points.size());
}

TEST_CASE ("replay: a reference that does not belong to the trace is refused, not compared", "[bside][replay]")
{
    const auto t = sampleTrace();
    auto ref = generate (t);
    ref.points.pop_back();
    StubChip chip;
    const auto r = replay (chip, t, ref, setup, snapshot);
    CHECK_FALSE (r.ok);
    CHECK (r.compared == 0);
    CHECK (r.error.find ("checkpoints") != std::string::npos);
    CHECK (describe (r).find ("replay did not run") == 0);
    CHECK (chip.writesSeen == 0);
}

TEST_CASE ("replay: random traces, with and without periodic checkpoints", "[bside][replay]")
{
    bst::Rng rng (31337);
    for (int i = 0; i < 60; ++i)
    {
        Trace t;
        t.clockHz = 985248;
        t.model = rng.chance (50) ? Model::A : Model::B;
        t.check = rng.chance (50) ? static_cast<std::uint64_t> (rng.range (1, 3000)) : 0;
        std::uint64_t c = 0;
        for (int k = 0, n = rng.range (0, 40); k < n; ++k)
        {
            c += rng.chance (25) ? 0 : static_cast<std::uint64_t> (rng.range (0, 4000));
            t.writes.push_back ({ c, static_cast<std::uint8_t> (rng.range (0, 0x18)), static_cast<std::uint8_t> (rng.range (0, 255)) });
        }
        t.endCycle = c + static_cast<std::uint64_t> (rng.range (0, 6000));
        const auto ref = generate (t);
        StubChip chip;
        const auto r = replay (chip, t, ref, setup, snapshot);
        INFO (describe (r));
        CHECK (r.ok);
        CHECK (r.compared == ref.points.size());
    }
}

// ---- the tolerance table ----

TEST_CASE ("compare: scalar tolerances", "[bside][compare]")
{
    CHECK (checkFilterCorner (1000.0, 1000.0).pass);
    CHECK (checkFilterCorner (1049.0, 1000.0).pass);
    CHECK (checkFilterCorner (951.0, 1000.0).pass);
    CHECK_FALSE (checkFilterCorner (1051.0, 1000.0).pass);
    CHECK_FALSE (checkFilterCorner (940.0, 1000.0).pass);
    CHECK_FALSE (checkFilterCorner (1000.0, 0.0).pass);
    CHECK_FALSE (checkFilterCorner (std::nan (""), 1000.0).pass);
    CHECK (checkFilterCorner (1051.0, 1000.0).what.find ("off by 5.10 %") != std::string::npos);

    CHECK (checkResonancePeak (10.0, 10.9).pass);
    CHECK (checkResonancePeak (10.0, 11.0).pass);
    CHECK_FALSE (checkResonancePeak (10.0, 11.1).pass);
    CHECK (checkVoiceDcStep (-20.0, -20.9).pass);
    CHECK_FALSE (checkVoiceDcStep (-20.0, -21.2).pass);
    CHECK_FALSE (checkVoiceDcStep (INFINITY, 0.0).pass);

    const double ref[4] = { -30, -40, -50, -60 };
    const double ok[4] = { -32, -41, -50, -62.9 };
    const double bad[4] = { -30, -40, -53.5, -60 };
    CHECK (checkDistortion (ok, ref).pass);
    const auto v = checkDistortion (bad, ref);
    CHECK_FALSE (v.pass);
    CHECK (v.what.find ("harmonic 4") != std::string::npos);
    CHECK (std::abs (v.deviation - 3.5) < 1e-9);
}

namespace
{
SpecSegment flatSegment (int tenthDb)
{
    SpecSegment s;
    s.name = "noise";
    s.firstFrame = 0;
    s.endFrame = 1;
    s.spectrum.fill (static_cast<std::int16_t> (tenthDb));
    return s;
}
} // namespace

TEST_CASE ("compare: noise spectrum, third octaves between 50 Hz and 16 kHz", "[bside][compare]")
{
    const auto flat = thirdOctaveFromTwelfth (flatSegment (-600).spectrum);
    REQUIRE (flat.size() == 30);
    for (const auto v : flat)
        CHECK (std::abs (v - -60.0) < 1e-9);

    CHECK (checkNoiseSpectrum (flatSegment (-600), flatSegment (-600)).pass);
    CHECK (checkNoiseSpectrum (flatSegment (-600), flatSegment (-585)).pass);      // 1.5 dB
    CHECK_FALSE (checkNoiseSpectrum (flatSegment (-600), flatSegment (-580)).pass); // 2 dB

    // A deviation only below 50 Hz or above 16 kHz does not count.
    auto m = flatSegment (-600);
    for (int k = 0; k < 12; ++k)
        m.spectrum[static_cast<std::size_t> (k)] = -200; // bins 0..11: 20 Hz .. 40 Hz
    for (int k = 116; k < 120; ++k)
        m.spectrum[static_cast<std::size_t> (k)] = -200; // third-octave 29: about 17.8 kHz
    CHECK (checkNoiseSpectrum (m, flatSegment (-600)).pass);
    m.spectrum[60] = -300; // inside the band: 4 adjacent bins averaged as power
    const auto v = checkNoiseSpectrum (m, flatSegment (-600));
    CHECK_FALSE (v.pass);
    CHECK (v.deviation > 1.5);
    // Power averaging: one bin 30 dB up raises the group by 10 log10 ((3 + 1000) / 4) ~ 24 dB.
    CHECK (v.deviation > 20.0);
}

namespace
{
Spec flatSpec (int frames, int level, int band)
{
    Spec s;
    s.traceSha256 = std::string (64, 'a');
    s.frames = frames;
    for (int f = 0; f < frames; ++f)
    {
        s.levelTenthDb.push_back (static_cast<std::int16_t> (level));
        for (int b = 0; b < kSpecBands; ++b)
            s.bandTenthDb.push_back (static_cast<std::int16_t> (band));
    }
    return s;
}
} // namespace

TEST_CASE ("compare: scripted traces", "[bside][compare]")
{
    const auto ref = flatSpec (100, -300, -400);
    SECTION ("identical")
    {
        const auto r = compareScripted (ref, ref);
        CHECK (r.pass);
        CHECK (std::abs (r.outlierFraction) < 1e-12);
        CHECK (std::abs (r.levelDeltaDb) < 1e-12);
        CHECK (r.report.empty());
    }
    SECTION ("a uniform band shift: mean 1 dB passes, above fails")
    {
        CHECK (compareScripted (ref, flatSpec (100, -300, -390)).pass);
        const auto r = compareScripted (ref, flatSpec (100, -300, -385));
        CHECK_FALSE (r.pass);
        CHECK (r.report.find ("mean band deviation") != std::string::npos);
        CHECK (r.report.find ("band cells off") == std::string::npos); // 1.5 dB is inside the 3 dB cell limit
    }
    SECTION ("outliers: 5 % allowed, more fail")
    {
        auto c = ref;
        const int cells = 100 * kSpecBands; // 2300; 5 % = 115
        for (int i = 0; i < 115; ++i)
            c.bandTenthDb[static_cast<std::size_t> (i * 20)] += 40; // +4 dB
        auto r = compareScripted (ref, c);
        CHECK (r.pass);
        CHECK (std::abs (r.outlierFraction - 115.0 / cells) < 1e-12);
        c.bandTenthDb[1] += 40;
        r = compareScripted (ref, c);
        CHECK_FALSE (r.pass);
        CHECK (r.report.find ("band cells off by more than 3.0 dB") != std::string::npos);
    }
    SECTION ("a cell of exactly 3 dB is not an outlier")
    {
        auto c = ref;
        for (auto& v : c.bandTenthDb)
            v += 30;
        const auto r = compareScripted (ref, c);
        CHECK (std::abs (r.outlierFraction) < 1e-12);
        CHECK_FALSE (r.pass); // but the mean is 3 dB
    }
    SECTION ("overall level within 0.5 dB")
    {
        CHECK (compareScripted (ref, flatSpec (100, -295, -400)).pass);
        const auto r = compareScripted (ref, flatSpec (100, -294, -400));
        CHECK_FALSE (r.pass);
        CHECK (r.report.find ("overall level differs by 0.60 dB") != std::string::npos);
    }
    SECTION ("cells below the gate in both are ignored")
    {
        const auto quiet = flatSpec (10, -1200, -1100);
        auto other = quiet;
        for (auto& v : other.bandTenthDb)
            v = -900; // -90 dB vs -110 dB: both below the -80 dB gate
        CHECK (compareScripted (quiet, other).pass);
        CHECK (std::abs (compareScripted (quiet, other).meanCellDb) < 1e-12);
    }
    SECTION ("frame counts must match")
    {
        const auto r = compareScripted (ref, flatSpec (99, -300, -400));
        CHECK_FALSE (r.pass);
        CHECK (r.report.find ("frame count differs") != std::string::npos);
    }
}

TEST_CASE ("compare: delay estimation", "[bside][compare]")
{
    bst::Rng rng (99);
    std::vector<float> sig (6000);
    for (auto& s : sig)
        s = static_cast<float> (rng.range (-1000, 1000)) / 1000.0f;
    const auto delayed = [&] (int d) {
        std::vector<float> out (sig.size(), 0.0f);
        for (std::size_t i = 0; i < sig.size(); ++i)
        {
            const long j = static_cast<long> (i) - d;
            if (j >= 0 && j < static_cast<long> (sig.size()))
                out[i] = sig[static_cast<std::size_t> (j)];
        }
        return out;
    };
    CHECK (estimateDelay (sig.data(), sig.data(), sig.size()) == 0);
    for (const int d : { 1, 37, 255, 256, -1, -100, -256 })
    {
        const auto m = delayed (d);
        CHECK (estimateDelay (sig.data(), m.data(), sig.size()) == d);
    }
    // Outside the window: no claim except that it stays inside it.
    const auto far = delayed (400);
    const int lag = estimateDelay (sig.data(), far.data(), sig.size());
    CHECK (std::abs (lag) <= 256);
    CHECK (estimateDelay (nullptr, nullptr, 0) == 0);
    CHECK (estimateDelay (sig.data(), sig.data(), 0) == 0);
    CHECK (estimateDelay (sig.data(), sig.data(), 10, 256) == 0); // window longer than the signal
    const std::vector<float> silence (500, 0.0f);
    CHECK (estimateDelay (silence.data(), silence.data(), silence.size()) == 0);
}
