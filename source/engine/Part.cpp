#include "engine/Part.h"

#include "engine/Oversampler.h"

namespace vs
{
void Part::prepare (double sampleRate, int maxBlockSize, int oversamplingFactor, std::uint32_t seed)
{
    maxBlock = juce::jmax (1, maxBlockSize);
    osFactor = oversamplingFactor;
    osRate = static_cast<float> (sampleRate) * static_cast<float> (osFactor);

    oversampler = makeVoiceOversampler (osFactor);
    oversampler->initProcessing (static_cast<size_t> (maxBlock));
    // Decimator delay + oscillator BLEP look-ahead (in oversampled samples).
    latencySamples = juce::roundToInt (oversampler->getLatencyInSamples()
                                       + static_cast<float> (dsp::Oscillator::kLatency) / static_cast<float> (osFactor));

    silence.setSize (1, maxBlock);
    silence.clear();

    modulator.prepare (osRate, seed ^ 0x5bd1e995u);
    voice.prepare (osRate, seed);
    reset();
}

void Part::reset() noexcept
{
    if (oversampler != nullptr)
        oversampler->reset();
    modulator.reset (target);
    voice.reset();
    numHeld = 0;
    modWheel = 0.0f;
    sustainDown = false;
}

void Part::applyBlockParams() noexcept
{
    // Switches and envelope times apply immediately; continuous values are smoothed per sample.
    modulator.setTarget (target);
    modulator.setVintage (vintage);
    voice.setBlockParams (target, vintage);
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
    Voice* voices[] { &voice };
    for (int pos = 0; pos < n;)
    {
        const int len = juce::jmin (maxSubBlock, n - pos);
        modulator.next (len, modBlock);
        Voice::renderBlock (voices, 1, modBlock, os + pos);
        pos += len;
    }
}

void Part::handleEvent (const NoteEvent& e) noexcept
{
    switch (e.type)
    {
        case EventType::NoteOn:           noteOn (e.note, e.value); break;
        case EventType::NoteOff:          noteOff (e.note); break;
        case EventType::PitchBend:        modulator.setBendTarget (e.value * bendRangeSemis); break;
        case EventType::AllNotesOff:      allNotesOff(); break;
        case EventType::AllSoundOff:      allNotesOff(); voice.reset(); break;
        case EventType::ResetControllers: resetControllers(); break;
        case EventType::ModWheel:         modWheel = e.value; break;               // vibrato: M2b
        case EventType::SustainPedal:     sustainDown = e.value >= 0.5f; break;    // deferred releases: M2b
    }
}

void Part::noteOn (int note, float velocity) noexcept
{
    noteOff (note); // re-pressed key moves to the top of the stack

    if (numHeld == 0)
        modulator.triggerLfo();

    if (numHeld < static_cast<int> (held.size()))
        held[static_cast<size_t> (numHeld++)] = note;

    voice.noteOn (note, velocity);
}

void Part::noteOff (int note) noexcept
{
    int i = 0;
    while (i < numHeld && held[static_cast<size_t> (i)] != note)
        ++i;
    if (i == numHeld)
        return;

    for (; i < numHeld - 1; ++i)
        held[static_cast<size_t> (i)] = held[static_cast<size_t> (i + 1)];
    --numHeld;

    if (note != voice.getNote())
        return;

    if (numHeld > 0)
        voice.setNote (held[static_cast<size_t> (numHeld - 1)]);
    else
        voice.noteOff();
}

void Part::allNotesOff() noexcept
{
    // Also clears the pedal: once M2b defers releases while it is down, an All Notes Off (or
    // the overflow marker, whose pedal-up may have been dropped) must still release everything.
    sustainDown = false;
    numHeld = 0;
    voice.noteOff();
}

void Part::resetControllers() noexcept
{
    modulator.setBendTarget (0.0f);
    modWheel = 0.0f;
    sustainDown = false; // M2b: releasing the pedal releases the notes it was holding
}
} // namespace vs
