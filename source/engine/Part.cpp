#include "engine/Part.h"

#include "engine/Oversampler.h"
#include "params/Mappings.h"

#include <cmath>

namespace vs
{
namespace
{
AssignMode assignModeOf (const PartParams& p) noexcept
{
    return static_cast<AssignMode> (juce::jlimit (0, 2, p.assignMode));
}
} // namespace

void Part::prepare (double sampleRate, int maxBlockSize, int oversamplingFactor, std::uint32_t seed)
{
    maxBlock = juce::jmax (1, maxBlockSize);
    osFactor = oversamplingFactor;
    osRate = static_cast<float> (sampleRate) * static_cast<float> (osFactor);
    stealFadeSamples = juce::jmax (1, juce::roundToInt (kStealFadeSeconds * osRate));

    oversampler = makeVoiceOversampler (osFactor);
    oversampler->initProcessing (static_cast<size_t> (maxBlock));
    // Decimator delay + oscillator BLEP look-ahead (in oversampled samples).
    latencySamples = juce::roundToInt (oversampler->getLatencyInSamples()
                                       + static_cast<float> (dsp::Oscillator::kLatency) / static_cast<float> (osFactor));

    silence.setSize (1, maxBlock);
    silence.clear();

    modulator.prepare (osRate, seed ^ 0x5bd1e995u);
    for (size_t i = 0; i < voices.size(); ++i)
    {
        voices[i].prepare (osRate, seed + static_cast<std::uint32_t> (i));
        voicePtrs[i] = &voices[i];
    }
    reset();
}

void Part::reset() noexcept
{
    if (oversampler != nullptr)
        oversampler->reset();
    modulator.reset (target);
    for (auto& v : voices)
        v.reset();
    pending.fill (Pending {});
    allocator.reset();
    allocator.initMode (assignModeOf (target), map::unisonVoices (target.unisonVoices));
    allocator.setLegatoGlideOnly (target.portaMode == 1);
    bendValue = 0.0f;
    bendRangeIndex = target.bendRange;
    bendRangeSemis = map::bendRangeSemis (bendRangeIndex);
    modWheel = 0.0f;
}

int Part::numSoundingVoices() const noexcept
{
    int n = 0;
    for (size_t i = 0; i < voices.size(); ++i)
        n += (voices[i].isActive() || pending[i].active) ? 1 : 0;
    return n;
}

void Part::applyBlockParams() noexcept
{
    // Switches and envelope times apply immediately; continuous values are smoothed per sample.
    modulator.setTarget (target);
    modulator.setVintage (vintage);
    for (auto& v : voices)
        v.setBlockParams (target, vintage);

    allocator.setLegatoGlideOnly (target.portaMode == 1);
    // A mode or unison-count change releases the sounding voices (envelope release, no reset).
    allocator.setMode (assignModeOf (target), map::unisonVoices (target.unisonVoices), sink());

    if (target.bendRange != bendRangeIndex)
    {
        bendRangeIndex = target.bendRange;
        bendRangeSemis = map::bendRangeSemis (bendRangeIndex);
        modulator.setBendTarget (bendValue * bendRangeSemis); // smoothed like any bend move
    }
}

void Part::render (float* out, int numSamples, std::span<const NoteEvent> events) noexcept
{
    jassert (numSamples <= maxBlock);
    numSamples = juce::jmin (numSamples, maxBlock);
    if (numSamples <= 0)
    {
        for (const auto& e : events)
            handleEvent (e); // zero-length host block: state changes still count
        return;
    }

    applyBlockParams();

    // The decimator needs a matching upsampling pass. Feed it silence and overwrite
    // the oversampled block with the voice output.
    auto silentBlock = juce::dsp::AudioBlock<float> (silence).getSubBlock (0, static_cast<size_t> (numSamples));
    auto osBlock = oversampler->processSamplesUp (silentBlock);
    float* os = osBlock.getChannelPointer (0);

    int pos = 0;
    for (const auto& e : events)
    {
        jassert (e.offset >= pos); // offsets must be ascending
        const int at = juce::jlimit (pos, numSamples, e.offset);
        if (at > pos)
        {
            renderOversampled (os + pos * osFactor, (at - pos) * osFactor);
            pos = at;
        }
        handleEvent (e);
    }
    if (numSamples > pos)
        renderOversampled (os + pos * osFactor, (numSamples - pos) * osFactor);

    float* channels[] { out };
    juce::dsp::AudioBlock<float> outBlock (channels, 1, static_cast<size_t> (numSamples));
    oversampler->processSamplesDown (outBlock);
}

void Part::renderOversampled (float* os, int n) noexcept
{
    for (int pos = 0; pos < n;)
    {
        int len = juce::jmin (maxSubBlock, n - pos);
        // A stolen voice starts its new note exactly at the end of its fade: split there.
        for (size_t v = 0; v < voices.size(); ++v)
            if (pending[v].active)
                len = juce::jmin (len, juce::jmax (1, voices[v].getFadeRemaining()));

        modulator.next (len, modBlock);
        Voice::renderBlock (voicePtrs.data(), kMaxVoices, modBlock, os + pos);
        pos += len;
        afterSubBlock();
    }
}

void Part::afterSubBlock() noexcept
{
    for (size_t i = 0; i < voices.size(); ++i)
    {
        auto& v = voices[i];
        auto& p = pending[i];
        if (p.active)
        {
            if (v.getFadeRemaining() > 0)
                continue;
            v.reset();
            p.active = false;
            startVoice (p.action);
            if (p.release)
                v.noteOff();
        }
        else if (! v.isActive() && allocator.isVoiceBusy (static_cast<int> (i)))
        {
            allocator.voiceIdle (static_cast<int> (i));
        }
    }
}

void Part::handleEvent (const NoteEvent& e) noexcept
{
    switch (e.type)
    {
        case EventType::NoteOn:
            // The LFO delay restarts when a note starts with no other key held.
            if (! allocator.anyPhysicalKeyExcept (e.note))
                modulator.triggerLfo();
            allocator.noteOn (e.note, e.value, sink());
            break;

        case EventType::NoteOff:
            allocator.noteOff (e.note, sink());
            break;

        case EventType::PitchBend:
            bendValue = e.value;
            modulator.setBendTarget (bendValue * bendRangeSemis);
            break;

        case EventType::ModWheel:
            modWheel = e.value;
            modulator.setModWheel (modWheel);
            break;

        case EventType::SustainPedal:
            allocator.setSustain (e.value >= 0.5f, sink());
            break;

        case EventType::AllNotesOff:
            allocator.allNotesOff (sink()); // also clears the pedal
            break;

        case EventType::AllSoundOff:
            allSoundOff();
            break;

        case EventType::ResetControllers:
            bendValue = 0.0f;
            modulator.setBendTarget (0.0f);
            modWheel = 0.0f;
            modulator.setModWheel (0.0f);
            allocator.setSustain (false, sink()); // releases the notes only the pedal was holding
            break;
    }
}

void Part::allSoundOff() noexcept
{
    allocator.killAll();
    for (auto& v : voices)
        v.reset();
    pending.fill (Pending {});
}

void Part::execute (const VoiceAction& a) noexcept
{
    const auto i = static_cast<size_t> (a.voice);
    auto& v = voices[i];
    auto& p = pending[i];

    switch (a.type)
    {
        case VoiceAction::Type::Start:
        case VoiceAction::Type::Steal:
            if (p.active)
            {
                // Still fading out for an earlier steal: the newest note starts when the fade ends.
                p.action = a;
                p.release = false;
            }
            else if (a.type == VoiceAction::Type::Steal && v.isActive())
            {
                v.startFade (stealFadeSamples);
                p = Pending { true, false, a };
            }
            else
            {
                if (a.type == VoiceAction::Type::Steal)
                    v.reset(); // finished meanwhile: no fade needed
                startVoice (a);
            }
            break;

        case VoiceAction::Type::Retarget:
            if (p.active)
                p.action.note = a.note;
            else
                v.setNote (a.note, target.portaTime > 0.0f && a.glideFrom == VoiceAction::kGlideFromCurrent);
            break;

        case VoiceAction::Type::Release:
            if (p.active)
                p.release = true;
            else
                v.noteOff();
            break;
    }
}

void Part::startVoice (const VoiceAction& a) noexcept
{
    auto& v = voices[static_cast<size_t> (a.voice)];

    // Glide source: the sounding pitch (mono modes, retrigger) or the previously played note.
    bool glide = false;
    float from = 0.0f;
    if (target.portaTime > 0.0f)
    {
        if (a.glideFrom == VoiceAction::kGlideFromCurrent && v.isActive())
        {
            glide = true;
            from = v.getPitch();
        }
        else if (a.glideFrom >= 0)
        {
            glide = true;
            from = static_cast<float> (a.glideFrom);
        }
    }

    float level = 1.0f, pos = 0.0f;
    if (allocator.getMode() == AssignMode::Unison)
    {
        const int n = allocator.getUnisonCount();
        level = 1.0f / std::sqrt (static_cast<float> (n));
        pos = static_cast<float> (a.voice) / static_cast<float> (n - 1) - 0.5f; // symmetric, -0.5 .. 0.5
    }

    v.start (a.note, a.velocity, level, pos);
    if (glide)
        v.glideFrom (from);
}
} // namespace vs
