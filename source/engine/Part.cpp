#include "engine/Part.h"

#include "dsp/Filters.h"
#include "engine/Oversampler.h"
#include "params/Mappings.h"

namespace vs
{
static_assert (kLfoWaveLabels.size() == dsp::kNumLfoWaves, "LFO wave labels must match dsp::LfoWave");

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

    smoothCoef = dsp::onePoleCoef (0.005f, osRate);
    lfo.prepare (osRate, seed ^ 0x5bd1e995u);
    voice.prepare (osRate, seed);
    reset();
}

void Part::reset() noexcept
{
    if (oversampler != nullptr)
        oversampler->reset();
    lfo.reset();
    voice.reset();
    smoothed = target;
    bend = bendTarget = 0.0f;
    numHeld = 0;
}

void Part::applyBlockParams() noexcept
{
    // Switches and envelope times apply immediately; continuous values are smoothed per sample.
#define VS_COPY_CHOICE(member, ...) smoothed.member = target.member;
    VS_PART_CHOICE_PARAMS (VS_COPY_CHOICE)
#undef VS_COPY_CHOICE

    lfo.setWave (static_cast<dsp::LfoWave> (target.lfoWave));
    lfo.setRate (map::lfoRateHz (target.lfoRate));
    lfo.setDelay (map::lfoDelaySec (target.lfoDelay));
    voice.setBlockParams (target, vintage);
}

void Part::render (float* out, int numSamples, const juce::MidiBuffer& midi, int startSample) noexcept
{
    jassert (numSamples <= maxBlock);
    numSamples = juce::jmin (numSamples, maxBlock);
    if (numSamples <= 0)
        return;

    applyBlockParams();

    // The decimator needs a matching upsampling pass. Feed it silence and overwrite
    // the oversampled block with the voice output.
    auto silentBlock = juce::dsp::AudioBlock<float> (silence).getSubBlock (0, static_cast<size_t> (numSamples));
    auto osBlock = oversampler->processSamplesUp (silentBlock);
    float* os = osBlock.getChannelPointer (0);

    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = meta.samplePosition - startSample;
        if (at < 0)
            continue;
        if (at >= numSamples)
            break;

        if (at > pos)
        {
            renderOversampled (os + pos * osFactor, (at - pos) * osFactor);
            pos = at;
        }
        handleMidi (meta.data, meta.numBytes);
    }
    renderOversampled (os + pos * osFactor, (numSamples - pos) * osFactor);

    float* channels[] { out };
    juce::dsp::AudioBlock<float> outBlock (channels, 1, static_cast<size_t> (numSamples));
    oversampler->processSamplesDown (outBlock);
}

void Part::renderOversampled (float* os, int n) noexcept
{
    const float a = smoothCoef;
    for (int i = 0; i < n; ++i)
    {
#define VS_SMOOTH(member, ...) smoothed.member += (target.member - smoothed.member) * a;
        VS_PART_FLOAT_PARAMS (VS_SMOOTH)
#undef VS_SMOOTH
        bend += (bendTarget - bend) * a;

        const float lfoValue = lfo.process();
        const float tremolo = 1.0f - map::vcaLfoDepth (smoothed.vcaLfoMod) * 0.5f * (1.0f - lfoValue);

        const ModFrame frame { smoothed,
                               lfoValue,
                               bend,
                               dsp::tptG (map::hpfHz (smoothed.hpfCutoff), osRate),
                               map::vcaGain (smoothed.vcaLevel) * tremolo,
                               vintage };

        os[i] = voice.process (frame);
    }
}

void Part::handleMidi (const juce::uint8* data, int numBytes) noexcept
{
    if (numBytes < 2 || data[0] < 0x80 || data[0] >= 0xF0)
        return; // running status is resolved by the host; system messages are ignored

    const int status = data[0] & 0xF0;
    const int d1 = data[1] & 0x7F;
    const int d2 = numBytes > 2 ? (data[2] & 0x7F) : 0;

    switch (status)
    {
        case 0x90:
            if (d2 > 0)
                noteOn (d1, static_cast<float> (d2) / 127.0f);
            else
                noteOff (d1);
            break;

        case 0x80:
            noteOff (d1);
            break;

        case 0xE0:
            bendTarget = static_cast<float> ((d2 << 7 | d1) - 8192) / 8192.0f * bendRangeSemis;
            break;

        case 0xB0:
            if (d1 == 123) // all notes off
                allNotesOff();
            else if (d1 == 120) // all sound off
            {
                allNotesOff();
                voice.reset();
            }
            else if (d1 == 121) // reset all controllers
                bendTarget = 0.0f;
            break;

        default:
            break;
    }
}

void Part::noteOn (int note, float velocity) noexcept
{
    noteOff (note); // re-pressed key moves to the top of the stack

    if (numHeld == 0)
        lfo.trigger();

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
    numHeld = 0;
    voice.noteOff();
}
} // namespace vs
