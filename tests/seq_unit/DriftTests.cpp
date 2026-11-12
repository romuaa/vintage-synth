// Host position drift (tempo ramps reported once per block, rounding) versus real jumps; swing and
// rate changes while playing; unusable host tempi.
//
// Spec stated once here (the module's contract): the drift limit is
//   min(max(half a step, duration of the block just rendered), one step)  [quarter notes].
// A deviation up to that is drift: nothing is released, no step doubled or skipped. Above it is a jump.
#include <catch2/catch_test_macros.hpp>

#include "SeqTestUtil.h"

#include <cmath>
#include <functional>

using namespace vs::seq;
using namespace vs::seq::test;
using vs::EventType;

namespace
{
constexpr double kSr = 48000.0;
constexpr double kBpm = 120.0;
constexpr double kSpp = 60.0 * kSr / kBpm; // 24000 samples per quarter

constexpr int kD[] = { 1, 2, 3, 4, 6, 8 };
constexpr Rate kRates[] = { Rate::Quarter, Rate::Eighth, Rate::EighthTriplet, Rate::Sixteenth, Rate::SixteenthTriplet, Rate::ThirtySecond };

Rate rateFor (int d)
{
    for (int i = 0; i < 6; ++i)
        if (kD[i] == d)
            return kRates[i];
    return Rate::Sixteenth;
}

Settings arp (int d, double gate = 0.5, double swing = 0.5)
{
    Settings s;
    s.mode = Mode::Arp;
    s.rate = rateFor (d);
    s.gate = gate;
    s.swing = swing;
    s.internalBpm = 120.0;
    return s;
}

struct Result
{
    std::vector<NoteEvent> events; // absolute offsets
    std::vector<std::int64_t> onAt;
    std::vector<int> onNote;
    std::vector<std::int64_t> offAt;
    bool wellFormed = true;
    std::string checkerError;
};

using ReportFn = std::function<Transport (int blockIndex, std::int64_t startSample)>;
using ChangeFn = std::function<bool (int blockIndex, std::int64_t startSample, Settings&)>;

/** Host at 120 bpm, 48 kHz; `report` gives the transport each block claims, `change` may alter the settings. */
Result drive (const Settings& initial, const std::vector<int>& keys, int block, std::int64_t total, const ReportFn& report,
              const ChangeFn& change = nullptr)
{
    ArpSeq m;
    Settings s = initial;
    m.setSettings (s);
    Result r;
    std::vector<NoteEvent> out (4096);
    StreamChecker chk;
    int b = 0;
    for (std::int64_t pos = 0; pos < total; pos += block, ++b)
    {
        const int n = static_cast<int> (std::min<std::int64_t> (block, total - pos));
        if (change && change (b, pos, s))
            m.setSettings (s);
        std::vector<NoteEvent> in;
        if (b == 0)
            for (int k : keys)
                in.push_back (noteOn (0, k));
        const int count = m.process (in, report (b, pos), n, out);
        int prev = 0;
        for (int i = 0; i < count; ++i)
        {
            NoteEvent e = out[static_cast<std::size_t> (i)];
            if (e.offset < prev || e.offset >= n)
                r.wellFormed = false;
            prev = e.offset;
            chk.feed (e, Mode::Arp, e.offset == 0);
            e.offset = static_cast<int> (e.offset + pos);
            r.events.push_back (e);
            if (e.type == EventType::NoteOn)
            {
                r.onAt.push_back (e.offset);
                r.onNote.push_back (e.note);
            }
            else if (e.type == EventType::NoteOff)
                r.offAt.push_back (e.offset);
        }
    }
    r.checkerError = chk.firstError;
    return r;
}

Transport steady (std::int64_t pos, double deviation = 0.0)
{
    return Transport { kSr, kBpm, 8.0 + static_cast<double> (pos) / kSpp + deviation, true };
}

double driftLimit (int d, int block)
{
    const double step = 1.0 / static_cast<double> (d);
    return std::min (std::max (0.5 * step, static_cast<double> (block) / kSpp), step);
}

struct Gen
{
    std::uint64_t s;
    explicit Gen (std::uint64_t seed) : s (seed * 6364136223846793005ull + 99) {}
    double unit() // [-1, 1]
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double> ((s >> 33) % 20001) / 10000.0 - 1.0;
    }
};
} // namespace

TEST_CASE ("Drift: one block reporting its position 0.02 quarter late changes nothing (480-sample blocks, 1/16)", "[seq][drift]")
{
    // The reviewer's repro: the note used to be cut, the step fired twice, and the next (correct) report cut it again.
    const Settings s = arp (4);
    const std::int64_t total = 48000 * 2;
    const auto base = drive (s, { 60, 64 }, 480, total, [] (int, std::int64_t pos) { return steady (pos); });
    for (const double dev : { -0.02, 0.02 })
    {
        const auto r = drive (s, { 60, 64 }, 480, total, [dev] (int b, std::int64_t pos) { return steady (pos, b == 50 ? dev : 0.0); });
        INFO ("deviation " << dev);
        CHECK (r.wellFormed);
        CHECK (r.checkerError.empty());
        CHECK (r.onNote == base.onNote);                 // same notes in the same order: none doubled, none dropped
        CHECK (r.onAt.size() == base.onAt.size());
        CHECK (r.offAt.size() == base.offAt.size());
        for (std::size_t i = 0; i < std::min (r.onAt.size(), base.onAt.size()); ++i)
            CHECK (std::llabs (r.onAt[i] - base.onAt[i]) <= 481);
        // Every note keeps (about) its length: nothing is cut at the block that reported the odd position.
        for (std::size_t i = 0; i < std::min (r.onAt.size(), r.offAt.size()); ++i)
            CHECK (std::llabs ((r.offAt[i] - r.onAt[i]) - 3000) <= 482);
    }
}

TEST_CASE ("Drift: jitter up to 90 % of the limit at block sizes 480, 4096 and 8192 equals the undisturbed note sequence", "[seq][drift]")
{
    for (const int d : { 1, 4, 8 })
        for (const int block : { 480, 4096, 8192 })
        {
            const double limit = driftLimit (d, block);
            const std::int64_t total = 48000 * 8;
            const Settings s = arp (d);
            const auto base = drive (s, { 60, 64, 67 }, block, total, [] (int, std::int64_t pos) { return steady (pos); });
            for (const std::uint64_t seed : { 1u, 2u, 3u })
            {
                // Successive deviations differ by at most 0.9 * limit (each in +-0.45 limit).
                Gen g (seed + 10 * static_cast<std::uint64_t> (d));
                std::vector<double> dev;
                for (std::int64_t i = 0; i < total / block + 2; ++i)
                    dev.push_back (0.45 * limit * g.unit());
                const auto r = drive (s, { 60, 64, 67 }, block, total,
                                      [&dev] (int b, std::int64_t pos) { return steady (pos, dev[static_cast<std::size_t> (b)]); });
                INFO ("d " << d << " block " << block << " seed " << seed << " limit " << limit);
                CHECK (r.wellFormed);
                CHECK (r.checkerError.empty());
                // The last step may fall just inside or outside the run: compare the common prefix.
                REQUIRE (! r.onNote.empty());
                CHECK (std::abs (static_cast<int> (r.onNote.size()) - static_cast<int> (base.onNote.size())) <= 1);
                CHECK (std::equal (r.onNote.begin(), r.onNote.begin() + static_cast<std::ptrdiff_t> (std::min (r.onNote.size(), base.onNote.size())), base.onNote.begin()));
                const double step = kSpp / static_cast<double>(d);
                for (std::size_t i = 1; i < r.onAt.size(); ++i)
                    CHECK (static_cast<double> (r.onAt[i] - r.onAt[i - 1]) > 0.0); // strictly increasing
                for (std::size_t i = 0; i < std::min (r.onAt.size(), r.offAt.size()); ++i)
                {
                    const double dur = static_cast<double> (r.offAt[i] - r.onAt[i]);
                    // The gate (50 % of a step) is not cut: it may only stretch or shrink by the drift itself.
                    CHECK (std::fabs (dur - 0.5 * step) <= 0.9 * limit * kSpp + 2.0);
                }
            }
        }
}

TEST_CASE ("Drift: tempo ramps reported once per block (480 / 4096 / 8192) neither double nor drop steps", "[seq][drift]")
{
    for (const int block : { 480, 4096, 8192 })
        for (const bool up : { true, false })
        {
            // 2 s ramp 60 <-> 240 bpm (steep: about 10 % per 8192-sample block), then steady.
            const double bpm0 = up ? 60.0 : 240.0, bpm1 = up ? 240.0 : 60.0, ramp = 2.0 * kSr;
            const auto ppqAt = [&] (double sample) {
                const double t = std::min (sample, ramp);
                double ppq = (bpm0 * t + (bpm1 - bpm0) * t * t / (2.0 * ramp)) / (60.0 * kSr);
                if (sample > ramp)
                    ppq += (sample - ramp) * bpm1 / (60.0 * kSr);
                return ppq;
            };
            const auto bpmAt = [&] (double sample) { return bpm0 + (bpm1 - bpm0) * std::min (sample, ramp) / ramp; };
            const std::int64_t total = 48000 * 6;
            const auto r = drive (arp (4), { 60, 64 }, block, total,
                                  [&] (int, std::int64_t pos) { return Transport { kSr, bpmAt (static_cast<double> (pos)), 8.0 + ppqAt (static_cast<double> (pos)), true }; });
            // Musical time: steps every 0.25 quarter from 8.0; how many lie before the end?
            const double endPpq = 8.0 + ppqAt (static_cast<double> (total));
            const int expected = static_cast<int> (std::ceil ((endPpq - 8.0) / 0.25 - 1.0e-9));
            INFO ("block " << block << (up ? " accelerando" : " ritardando") << " end ppq " << endPpq);
            CHECK (r.wellFormed);
            CHECK (r.checkerError.empty());
            CHECK (std::abs (static_cast<int> (r.onAt.size()) - expected) <= 1);
            for (std::size_t i = 0; i < r.onNote.size(); ++i)
                CHECK (r.onNote[i] == (i % 2 == 0 ? 60 : 64)); // each step plays the next note: none doubled or lost
            // No note is cut: every off is at least a third of the nominal gate after its on, and offs match ons.
            for (std::size_t i = 0; i < std::min (r.onAt.size(), r.offAt.size()); ++i)
                CHECK (r.offAt[i] - r.onAt[i] > 300);
            CHECK (r.offAt.size() + 1 >= r.onAt.size());
        }
}

TEST_CASE ("Drift: a real jump just above the limit releases at offset 0 and resynchronises, just below it does not", "[seq][drift]")
{
    constexpr int block = 480;
    const double limit = driftLimit (4, block); // 0.125
    for (const double sign : { 1.0, -1.0 })
        for (const double factor : { 0.8, 1.2 })
        {
            // Gate 100 %: a note is sounding all the time. Block 20 (samples 10080..10560) lies inside a step.
            const auto r = drive (arp (4, 1.0), { 60, 64 }, block, 48000, [&] (int b, std::int64_t pos) {
                return steady (pos, b >= 21 ? sign * factor * limit : 0.0);
            });
            // Look at what happens in the block that starts at sample 10080 (no step boundary inside it, with or without the shift).
            std::vector<NoteEvent> at;
            for (const auto& e : r.events)
                if (e.offset >= 10080 && e.offset < 10560)
                    at.push_back (e);
            INFO ("sign " << sign << " factor " << factor);
            CHECK (r.wellFormed);
            CHECK (r.checkerError.empty());
            if (factor > 1.0)
            {
                REQUIRE (! at.empty());
                CHECK (at[0].type == EventType::NoteOff);
                CHECK (at[0].offset == 10080); // released at offset 0 of the block that reported the jump
            }
            else if (sign > 0.0)
            {
                // Drift forward over a step position: that step is overdue and fires once, at offset 0, by itself
                // (its legato note-off follows); nothing is released first.
                REQUIRE (at.size() == 2);
                CHECK (at[0].type == EventType::NoteOn);
                CHECK (at[0].offset == 10080);
                CHECK (at[1].type == EventType::NoteOff);
            }
            else
                CHECK (at.empty()); // drift backwards: no event at all inside this block
        }
}

TEST_CASE ("Swing: a swing change at any block boundary around a step neither doubles nor skips a step", "[seq][drift]")
{
    for (const bool increase : { true, false })
    {
        const double from = increase ? 0.5 : 0.6, to = increase ? 0.6 : 0.5;
        int runs = 0;
        for (int boundary = 1; boundary * 64 <= 14000; ++boundary)
        {
            const Settings s = arp (4, 0.5, from);
            const std::int64_t changeAt = static_cast<std::int64_t> (boundary) * 64;
            const auto r = drive (s, { 60, 64, 67 }, 64, 30000, [] (int, std::int64_t pos) { return steady (pos); },
                                  [&] (int, std::int64_t pos, Settings& cur) {
                                      if (pos == changeAt)
                                      {
                                          cur.swing = to;
                                          return true;
                                      }
                                      return false;
                                  });
            INFO ("change at " << changeAt << (increase ? " 0.5 -> 0.6" : " 0.6 -> 0.5"));
            // Steps at 0, 6000 (+1200 if swung), 12000, 18000 (+1200), 24000 (30000 is outside): five steps, once each, in order.
            REQUIRE (r.onAt.size() == 5);
            CHECK (r.onNote == std::vector<int> { 60, 64, 67, 60, 64 });
            for (std::size_t i = 1; i < r.onAt.size(); ++i)
                CHECK (r.onAt[i] - r.onAt[i - 1] >= 4200); // never closer than 0.7 step
            ++runs;
        }
        CHECK (runs > 200);
    }
}

TEST_CASE ("Swing: sweeping the swing continuously across many blocks plays exactly the steps that elapse", "[seq][drift]")
{
    // The reviewer's probe: swing 0.5 -> 0.75 -> 0.5 over 8 seconds (about 3 updates per step) in 64-sample blocks.
    constexpr int block = 64;
    const std::int64_t total = static_cast<std::int64_t> (kSr * 8.0);
    const auto swingAt = [&] (std::int64_t pos) { return 0.5 + 0.25 * std::sin (3.14159265358979323846 * static_cast<double> (pos) / static_cast<double> (total)); };
    const auto r = drive (arp (4, 0.5, 0.5), { 60, 64, 67 }, block, total, [] (int, std::int64_t pos) { return steady (pos); },
                          [&] (int, std::int64_t pos, Settings& cur) {
                              cur.swing = swingAt (pos);
                              return true;
                          });
    CHECK (r.wellFormed);
    CHECK (r.checkerError.empty());
    REQUIRE (r.onAt.size() == 64); // 16 quarters at 1/16: one note per step, none doubled, none skipped
    for (std::size_t i = 0; i < r.onNote.size(); ++i)
        CHECK (r.onNote[i] == (i % 3 == 0 ? 60 : (i % 3 == 1 ? 64 : 67)));
    for (std::size_t i = 1; i < r.onAt.size(); ++i)
        CHECK (r.onAt[i] - r.onAt[i - 1] >= 2400); // at 75 % swing the short step is half a step
}

TEST_CASE ("Rate: a rate change at any block boundary continues with the first step of the new grid after the last one that fired", "[seq][drift]")
{
    struct Change
    {
        int from, to;
    };
    for (const Change ch : { Change { 1, 2 }, Change { 4, 1 }, Change { 1, 8 }, Change { 8, 1 }, Change { 4, 6 }, Change { 3, 4 }, Change { 6, 3 }, Change { 2, 3 } })
    {
        int runs = 0;
        for (std::int64_t boundary = 1; boundary * 64 <= 30000; boundary += 7)
        {
            const std::int64_t T = boundary * 64;
            const std::int64_t total = 90000;
            const auto r = drive (arp (ch.from), { 60 }, 64, total, [] (int, std::int64_t pos) { return steady (pos); },
                                  [&] (int, std::int64_t pos, Settings& cur) {
                                      if (pos == T)
                                      {
                                          cur.rate = rateFor (ch.to);
                                          return true;
                                      }
                                      return false;
                                  });
            // Exact expectation in integer ticks (anchor: ppq 8.0 at sample 0).
            MCfg oldCfg, newCfg;
            oldCfg.d = ch.from;
            newCfg.d = ch.to;
            const std::int64_t anchor = 8 * kTick;
            const MSeg seg { 0, total, 12000, anchor, false };
            // Grid positions are absolute ticks from ppq 0; the host reports ppq 8.0 at sample 0.
            const auto sampleOld = [&] (std::int64_t i) { return relSample (oldCfg, seg, oldCfg.pos (i)); };
            const auto sampleNew = [&] (std::int64_t i) { return relSample (newCfg, seg, newCfg.pos (i)); };
            std::vector<std::int64_t> expected;
            std::int64_t k = 8 * ch.from; // ppq 8.0 is step 8 * from
            for (; sampleOld (k) < T; ++k)
                expected.push_back (sampleOld (k));
            // Spec: the first step of the new grid after the last one that fired, at most one of them overdue.
            const std::int64_t lastPos = oldCfg.pos (k - 1);
            std::int64_t newFirst = 8 * ch.to - 2, firstAtOrAfter = 8 * ch.to - 2;
            while (newCfg.pos (newFirst) <= lastPos)
                ++newFirst;
            while (sampleNew (firstAtOrAfter) < T)
                ++firstAtOrAfter;
            for (std::int64_t j = std::max (newFirst, firstAtOrAfter - 1);; ++j)
            {
                const std::int64_t at = std::max (sampleNew (j), T);
                if (at >= total)
                    break;
                expected.push_back (at);
            }
            INFO ("rate " << ch.from << " -> " << ch.to << " at sample " << T);
            CHECK (r.wellFormed);
            CHECK (r.onAt == expected);
            ++runs;
        }
        CHECK (runs > 60);
    }
}

TEST_CASE ("Unusable host tempo or sample rate falls back to the internal clock (no step per sample)", "[seq][drift]")
{
    const double bpms[] = { 1.0e-310, 1.0e-30, 0.5, 1001.0, 1.0e9, 1.0e300, HUGE_VAL, -5.0, std::nan ("") };
    for (const double bpm : bpms)
    {
        ArpSeq m;
        m.setSettings (arp (8));
        std::vector<NoteEvent> out (4096);
        int ons = 0;
        for (int b = 0; b < 8; ++b)
        {
            std::vector<NoteEvent> in;
            if (b == 0)
                in.push_back (noteOn (0, 60));
            const int count = m.process (in, Transport { kSr, bpm, 8.0, true }, 512, out);
            for (int i = 0; i < count; ++i)
                ons += out[static_cast<std::size_t> (i)].type == EventType::NoteOn ? 1 : 0;
        }
        INFO ("host bpm " << bpm);
        // Internal clock at 120 bpm, 1/32 = 3000 samples: 8 * 512 = 4096 samples hold 2 steps.
        CHECK (ons == 2);
    }
}

TEST_CASE ("Host tempo at the accepted limits (1 and 1000 bpm) is used", "[seq][drift]")
{
    for (const double bpm : { 1.0, 1000.0 })
    {
        ArpSeq m;
        m.setSettings (arp (4));
        std::vector<NoteEvent> out (4096);
        std::vector<std::int64_t> ons;
        for (int b = 0; b < 40; ++b)
        {
            std::vector<NoteEvent> in;
            if (b == 0)
                in.push_back (noteOn (0, 60));
            const double ppq = 4.0 + static_cast<double> (b) * 512.0 * bpm / (60.0 * kSr);
            const int count = m.process (in, Transport { kSr, bpm, ppq, true }, 512, out);
            for (int i = 0; i < count; ++i)
                if (out[static_cast<std::size_t> (i)].type == EventType::NoteOn)
                    ons.push_back (b * 512 + out[static_cast<std::size_t> (i)].offset);
        }
        INFO ("bpm " << bpm);
        if (bpm > 500.0)
        {
            // 1/16 at 1000 bpm = 720 samples (exact: 60 * 48000 / 1000 / 4).
            REQUIRE (ons.size() >= 20);
            for (std::size_t i = 1; i < ons.size(); ++i)
                CHECK (ons[i] - ons[i - 1] == 720);
        }
        else
            CHECK (ons.size() == 1); // 1 bpm: one 1/16 step is 12 seconds long; only the first (aligned) step is inside 20480 samples
    }
}

TEST_CASE ("Unusable sample rates fall back to a safe value", "[seq][drift]")
{
    for (const double sr : { 1.0e-9, 1.0e12, HUGE_VAL, -48000.0, 0.0 })
    {
        ArpSeq m;
        m.setSettings (arp (4));
        std::vector<NoteEvent> out (256);
        const std::vector<NoteEvent> in { noteOn (0, 60) };
        const int count = m.process (in, Transport { sr, 120.0, 0.0, true }, 256, out);
        INFO ("sample rate " << sr);
        REQUIRE (count >= 1);
        CHECK (out[0].type == EventType::NoteOn);
    }
}
