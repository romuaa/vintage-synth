#include "bside/engine/TestTonePart.h"

#include <algorithm>
#include <cmath>

namespace bs
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;
} // namespace

void TestTonePart::prepare (double hostSampleRate) noexcept
{
    sampleRate = hostSampleRate > 1000.0 ? hostSampleRate : 48000.0;
    attackStep = static_cast<float> (1.0 / (kAttackSeconds * sampleRate));
    releaseStep = static_cast<float> (1.0 / (kReleaseSeconds * sampleRate));
    allocator.initMode (vs::AssignMode::Poly, 4);
    reset();
}

void TestTonePart::reset() noexcept
{
    for (auto& v : voices)
        v = Voice {};
    allocator.reset();
    bend = 0.0;
}

double TestTonePart::incrementFor (int note) const noexcept
{
    const double semitones = static_cast<double> (note) - 69.0 + bend * kBendRangeSemitones;
    return 440.0 * std::exp2 (semitones / 12.0) / sampleRate;
}

void TestTonePart::updateIncrements() noexcept
{
    for (auto& v : voices)
        if (v.stage != Stage::Idle)
            v.increment = incrementFor (v.note);
}

bool TestTonePart::isSounding() const noexcept
{
    return std::any_of (voices.begin(), voices.end(), [] (const Voice& v) { return v.stage != Stage::Idle; });
}

void TestTonePart::releaseAllNotes() noexcept
{
    allocator.releaseAllKeepPedal ([this] (const vs::VoiceAction& a) { execute (a); });
}

void TestTonePart::execute (const vs::VoiceAction& a) noexcept
{
    auto& v = voices[static_cast<size_t> (a.voice)];
    switch (a.type)
    {
        case vs::VoiceAction::Type::Start:
        case vs::VoiceAction::Type::Steal:
            // A voice that still sounds restarts its attack from its current level and phase (no click).
            v.stage = Stage::Attack;
            v.note = a.note;
            v.velocity = a.velocity;
            v.increment = incrementFor (a.note);
            break;
        case vs::VoiceAction::Type::Retarget:
            v.note = a.note;
            v.increment = incrementFor (a.note);
            break;
        case vs::VoiceAction::Type::Release:
            if (v.stage != Stage::Idle)
                v.stage = Stage::Release;
            break;
    }
}

void TestTonePart::apply (const vs::NoteEvent& e) noexcept
{
    const auto sink = [this] (const vs::VoiceAction& a) { execute (a); };
    switch (e.type)
    {
        case vs::EventType::NoteOn:
            allocator.noteOn (e.note, e.value, sink);
            break;
        case vs::EventType::NoteOff:
            allocator.noteOff (e.note, sink);
            break;
        case vs::EventType::PitchBend:
            bend = static_cast<double> (std::clamp (e.value, -1.0f, 1.0f));
            updateIncrements();
            break;
        case vs::EventType::ModWheel:
            break; // a test tone has no vibrato
        case vs::EventType::SustainPedal:
            allocator.setSustain (e.value >= 0.5f, sink);
            break;
        case vs::EventType::AllNotesOff:
            allocator.allNotesOff (sink);
            break;
        case vs::EventType::AllSoundOff:
            allocator.killAll();
            for (auto& v : voices)
                v = Voice {};
            break;
        case vs::EventType::ResetControllers:
            bend = 0.0;
            updateIncrements();
            allocator.setSustain (false, sink);
            break;
    }
}

void TestTonePart::renderSegment (float* out, int len) noexcept
{
    for (int i = 0; i < len; ++i)
        out[i] = 0.0f;
    for (int vi = 0; vi < kVoices; ++vi)
    {
        auto& v = voices[static_cast<size_t> (vi)];
        if (v.stage == Stage::Idle)
            continue;
        const float gain = kVoiceGain * v.velocity;
        for (int i = 0; i < len; ++i)
        {
            if (v.stage == Stage::Attack)
                v.level = std::min (1.0f, v.level + attackStep);
            else
            {
                v.level -= releaseStep;
                if (v.level <= 0.0f)
                {
                    v = Voice {};
                    allocator.voiceIdle (vi); // the allocator may reuse the voice from the next event on
                    break;
                }
            }
            out[i] += gain * v.level * static_cast<float> (std::sin (kTwoPi * v.phase));
            v.phase += v.increment;
            v.phase -= std::floor (v.phase);
        }
    }
}

void TestTonePart::render (float* out, int n, std::span<const vs::NoteEvent> events) noexcept
{
    int pos = 0;
    size_t next = 0;
    while (next < events.size() && events[next].offset <= n)
    {
        const int at = std::clamp (events[next].offset, pos, n);
        if (at > pos)
        {
            renderSegment (out + pos, at - pos);
            pos = at;
        }
        // Every event at this position (the skeleton hands them over in ascending order).
        while (next < events.size() && std::clamp (events[next].offset, pos, n) == pos)
            apply (events[next++]);
        if (pos >= n)
            break;
    }
    // Events after the end (should not happen: offsets are clamped to n) are applied after the last sample.
    while (next < events.size())
        apply (events[next++]);
    if (pos < n)
        renderSegment (out + pos, n - pos);
}
} // namespace bs
