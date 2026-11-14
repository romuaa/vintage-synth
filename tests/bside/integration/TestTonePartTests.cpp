// The placeholder part of the B-Side shell (source/bside/engine/TestTonePart.h): pitch, envelope,
// sample accuracy, polyphony, and independence of how a block is chopped.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "bside/engine/TestTonePart.h"
#include "common/engine/NoteEvent.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
using vs::EventType;
using vs::NoteEvent;

constexpr double kRate = 48000.0;

NoteEvent noteOn (int offset, int note, float velocity = 1.0f) { return { offset, EventType::NoteOn, static_cast<std::uint8_t> (note), velocity }; }
NoteEvent noteOff (int offset, int note) { return { offset, EventType::NoteOff, static_cast<std::uint8_t> (note), 0.0f }; }
NoteEvent control (int offset, EventType type, float value = 0.0f) { return { offset, type, 0, value }; }

std::vector<float> renderAll (bs::TestTonePart& part, int total, int block, const std::vector<NoteEvent>& events)
{
    std::vector<float> out (static_cast<size_t> (total));
    std::vector<NoteEvent> chunk;
    for (int start = 0; start < total; start += block)
    {
        const int len = std::min (block, total - start);
        chunk.clear();
        for (auto e : events)
            if (e.offset >= start && (e.offset < start + len || (start + len == total && e.offset == total)))
            {
                e.offset -= start;
                chunk.push_back (e);
            }
        part.render (out.data() + start, len, chunk);
    }
    return out;
}

float peakOf (const std::vector<float>& v, size_t from, size_t to)
{
    float peak = 0.0f;
    for (size_t i = from; i < to && i < v.size(); ++i)
        peak = std::max (peak, std::abs (v[i]));
    return peak;
}
} // namespace

TEST_CASE ("TestTonePart: latency 0, idle part is silent", "[bside][part]")
{
    bs::TestTonePart part;
    part.prepare (kRate);
    CHECK (part.getLatencySamples() == 0);
    CHECK_FALSE (part.isSounding());
    CHECK_FALSE (part.isSustainDown());
    const auto out = renderAll (part, 512, 512, {});
    CHECK (peakOf (out, 0, out.size()) < 1.0e-9f);
}

TEST_CASE ("TestTonePart: a note is a sine at the right pitch with the right level", "[bside][part]")
{
    bs::TestTonePart part;
    part.prepare (kRate);
    const auto out = renderAll (part, 48000, 512, { noteOn (0, 69, 0.5f) }); // A4 = 440 Hz
    CHECK (part.isSounding());
    CHECK (peakOf (out, 4800, 48000) == Catch::Approx (bs::TestTonePart::kVoiceGain * 0.5f).margin (0.002));
    int crossings = 0; // upward zero crossings in one second = the frequency
    for (size_t i = 1; i < out.size(); ++i)
        if (out[i - 1] < 0.0f && out[i] >= 0.0f)
            ++crossings;
    CHECK (crossings >= 439);
    CHECK (crossings <= 441);
}

TEST_CASE ("TestTonePart: 5 ms linear attack and release, then silence and not sounding", "[bside][part]")
{
    bs::TestTonePart part;
    part.prepare (kRate);
    const int attack = static_cast<int> (bs::TestTonePart::kAttackSeconds * kRate); // 240 samples
    const auto out = renderAll (part, 4800, 480, { noteOn (0, 81), noteOff (2400, 81) });
    CHECK (peakOf (out, 0, 24) < 0.12f * bs::TestTonePart::kVoiceGain); // 24 of 240 attack samples: at most 10 %
    CHECK (peakOf (out, static_cast<size_t> (attack) + 48, 2400) > 0.9f * bs::TestTonePart::kVoiceGain);
    CHECK (peakOf (out, 2400 + attack + 24, 4800) < 1.0e-9f); // released within 5 ms
    CHECK_FALSE (part.isSounding());
}

TEST_CASE ("TestTonePart: events apply at their sample", "[bside][part]")
{
    bs::TestTonePart part;
    part.prepare (kRate);
    const auto out = renderAll (part, 1000, 1000, { noteOn (300, 72) });
    CHECK (peakOf (out, 0, 301) < 1.0e-9f); // sine phase 0 and level 0 at the event's own sample
    CHECK (peakOf (out, 301, 400) > 0.0f);
}

TEST_CASE ("TestTonePart: the output does not depend on how the block is chopped", "[bside][part]")
{
    const std::vector<NoteEvent> events { noteOn (10, 60, 0.9f), noteOn (700, 64), noteOn (701, 67, 0.4f), noteOff (3000, 60), control (3500, EventType::PitchBend, 0.5f),
                                          control (4000, EventType::SustainPedal, 1.0f), noteOff (4100, 64), control (6000, EventType::SustainPedal, 0.0f),
                                          control (7000, EventType::AllNotesOff) };
    bs::TestTonePart a, b;
    a.prepare (kRate);
    b.prepare (kRate);
    const auto whole = renderAll (a, 9000, 9000, events);
    for (const int block : { 1, 7, 61, 512, 1000 })
    {
        INFO ("block " << block);
        b.prepare (kRate);
        CHECK (renderAll (b, 9000, block, events) == whole);
    }
}

TEST_CASE ("TestTonePart: 16 voices, the 17th note steals, nothing hangs after All Notes Off", "[bside][part]")
{
    bs::TestTonePart part;
    part.prepare (kRate);
    std::vector<NoteEvent> events;
    for (int n = 0; n < 17; ++n)
        events.push_back (noteOn (n * 10, 40 + n));
    events.push_back (control (3000, EventType::AllNotesOff));
    const auto out = renderAll (part, 6000, 256, events);
    for (const float v : out)
        REQUIRE (std::isfinite (v));
    CHECK (peakOf (out, 1000, 3000) > 0.5f);
    CHECK (peakOf (out, 3000 + 240 + 24, 6000) < 1.0e-9f);
    CHECK_FALSE (part.isSounding());
}

TEST_CASE ("TestTonePart: the sustain pedal holds a released note; reset silences at once", "[bside][part]")
{
    bs::TestTonePart part;
    part.prepare (kRate);
    auto out = renderAll (part, 2000, 500, { control (0, EventType::SustainPedal, 1.0f), noteOn (10, 69), noteOff (500, 69) });
    CHECK (part.isSustainDown());
    CHECK (peakOf (out, 1500, 2000) > 0.2f); // still sounding long after the key went up
    part.reset();
    CHECK_FALSE (part.isSounding());
    CHECK_FALSE (part.isSustainDown());
    out = renderAll (part, 480, 480, {});
    CHECK (peakOf (out, 0, out.size()) < 1.0e-9f);
}

TEST_CASE ("TestTonePart: a zero-length render only applies the events", "[bside][part]")
{
    bs::TestTonePart part;
    part.prepare (kRate);
    const NoteEvent on = noteOn (0, 60);
    part.render (nullptr, 0, { &on, 1 });
    CHECK (part.isSounding());
}

TEST_CASE ("TestTonePart: an offset at or beyond the block applies after its last sample", "[bside][part]")
{
    bs::TestTonePart part;
    part.prepare (kRate);
    std::vector<float> out (100, 1.0f);
    const NoteEvent late[] { noteOn (100, 60), noteOn (5000, 64) }; // n == 100: at the end; and far beyond
    part.render (out.data(), 100, late);
    CHECK (peakOf (out, 0, out.size()) < 1.0e-9f); // both notes start after the block's last sample
    CHECK (part.isSounding());
    part.render (out.data(), 100, {});
    CHECK (peakOf (out, 0, out.size()) > 0.0f);
}

TEST_CASE ("TestTonePart: a non-finite pitch bend is no bend", "[bside][part]")
{
    bs::TestTonePart part;
    part.prepare (kRate);
    const auto out = renderAll (part, 4800, 480, { noteOn (0, 69), control (100, EventType::PitchBend, std::numeric_limits<float>::quiet_NaN()) });
    for (const float v : out)
        REQUIRE (std::isfinite (v));
    CHECK (peakOf (out, 1000, 4800) > 0.2f);
}
