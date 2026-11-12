#include "vintage/engine/Part.h"

#include "common/engine/Oversampler.h"
#include "vintage/params/PartMappings.h"

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

void Part::prepare (double sampleRate, int maxBlockSize, int oversamplingFactor, std::uint32_t seed, int realTimeFactor)
{
    maxBlock = juce::jmax (1, maxBlockSize);
    osFactor = oversamplingFactor;
    osRate = static_cast<float> (sampleRate) * static_cast<float> (osFactor);
    stealFadeSamples = juce::jmax (1, juce::roundToInt (kStealFadeSeconds * osRate));

    // At 1x there is no decimator: the voices render straight into the output.
    oversampler.reset();
    double decimatorLatency = 0.0;
    if (osFactor > 1)
    {
        oversampler = makeVoiceOversampler (osFactor);
        oversampler->initProcessing (static_cast<size_t> (maxBlock));
        decimatorLatency = measureDecimatorDelay (osFactor);
    }
    // Offline at twice the real-time rate, notes that start with Cross Mod up render at the
    // real-time rate (Voice::setRealTimePairRatio) and sound Voice::kRtPathExtraDelay samples
    // later; the other notes are delayed by as much, so all stay aligned (and it is latency).
    rtPathOn = realTimeFactor > 0 && oversamplingFactor == 2 * realTimeFactor;
    const int alignDelay = rtPathOn ? Voice::kRtPathExtraDelay : 0;

    // Decimator delay (low-frequency phase delay of the down path only) + oscillator BLEP
    // look-ahead (kLatency oversampled samples) + that alignment, rounded to the nearest host sample.
    exactLatency = decimatorLatency + static_cast<double> (dsp::Oscillator::kLatency + alignDelay) / static_cast<double> (osFactor);
    latencySamples = juce::roundToInt (exactLatency);
    renderedSamples = 0;

    silence.setSize (1, maxBlock);
    silence.clear();

    partSeed = seed;
    modulator.prepare (osRate, seed ^ 0x5bd1e995u);
    for (size_t i = 0; i < voices.size(); ++i)
    {
        voices[i].prepare (osRate, seed + static_cast<std::uint32_t> (i));
        voices[i].setRealTimePairRatio (rtPathOn ? 2 : 1);
        voicePtrs[i] = &voices[i];
    }
    reset();
}

void Part::reset() noexcept
{
    if (oversampler != nullptr)
        oversampler->reset();
    alignRing.fill (0.0f);
    modulator.reset (target);
    for (auto& v : voices)
        v.reset();
    pending.fill (Pending {});
    releaseCountdown.fill (-1);
    late.fill (false);
    unisonRng.setSeed (partSeed * 2654435761u + 0x7f4a7c15u);
    allocator.reset();
    allocator.initMode (assignModeOf (target), map::unisonVoices (target.unisonVoices));
    allocator.setLegatoGlideOnly (target.portaMode == 1);
    bendValue = 0.0f;
    bendRangeIndex = target.bendRange;
    bendRangeSemis = map::bendRangeSemis (bendRangeIndex);
    modWheel = 0.0f;
}

void Part::releaseAllNotes() noexcept
{
    allocator.releaseAllKeepPedal (sink());
}

void Part::resume() noexcept
{
    if (oversampler != nullptr)
        oversampler->reset();
    alignRing.fill (0.0f);
    modulator.reset (target); // parameters jump to the targets, LFO restarts
    bendRangeIndex = target.bendRange;
    bendRangeSemis = map::bendRangeSemis (bendRangeIndex);
    modulator.setControllersNow (bendValue * bendRangeSemis, modWheel);
    allocator.initMode (assignModeOf (target), map::unisonVoices (target.unisonVoices));
    allocator.setLegatoGlideOnly (target.portaMode == 1);
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
    renderedSamples += numSamples;

    // The decimator needs a matching upsampling pass. Feed it silence and overwrite
    // the oversampled block with the voice output. At 1x the voices render into `out`.
    float* os = out;
    if (oversampler != nullptr)
    {
        auto silentBlock = juce::dsp::AudioBlock<float> (silence).getSubBlock (0, static_cast<size_t> (numSamples));
        auto osBlock = oversampler->processSamplesUp (silentBlock);
        os = osBlock.getChannelPointer (0);
    }

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

    if (oversampler != nullptr)
    {
        float* channels[] { out };
        juce::dsp::AudioBlock<float> outBlock (channels, 1, static_cast<size_t> (numSamples));
        oversampler->processSamplesDown (outBlock);
    }
}

void Part::renderOversampled (float* os, int n) noexcept
{
    for (int pos = 0; pos < n;)
    {
        int len = juce::jmin (maxSubBlock, n - pos);
        // A stolen voice starts its new note exactly at the end of its fade, and a delayed
        // release lands on its exact sample: split there.
        for (size_t v = 0; v < voices.size(); ++v)
        {
            if (pending[v].active)
                len = juce::jmin (len, juce::jmax (1, voices[v].getFadeRemaining()));
            if (releaseCountdown[v] > 0)
                len = juce::jmin (len, releaseCountdown[v]);
            // A drift retarget lands on a sub-block boundary (block-size independence).
            len = juce::jmin (len, juce::jmax (1, voices[v].samplesToDriftRetarget()));
        }

        modulator.next (len, modBlock);
        if (rtPathOn)
        {
            // Offline: ordinary notes are delayed to meet the real-time-rate ones (see prepare).
            float* y = os + pos;
            Voice::renderBlock (voicePtrs.data(), kMaxVoices, modBlock, y, rtPathOut.data());
            constexpr unsigned mask = static_cast<unsigned> (kAlignRingSize - 1);
            for (int i = 0; i < len; ++i)
            {
                alignRing[alignPos & mask] = y[i];
                y[i] = alignRing[(alignPos - static_cast<unsigned> (Voice::kRtPathExtraDelay)) & mask] + rtPathOut[static_cast<size_t> (i)];
                ++alignPos;
            }
        }
        else
        {
            Voice::renderBlock (voicePtrs.data(), kMaxVoices, modBlock, os + pos);
        }
        pos += len;
        for (auto& c : releaseCountdown)
            if (c > 0)
                c -= len;
        afterSubBlock();
    }
}

void Part::afterSubBlock() noexcept
{
    int finished[kMaxVoices];
    int numFinished = 0;
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
            late[i] = true;
        }
        if (releaseCountdown[i] == 0)
        {
            releaseCountdown[i] = -1;
            v.noteOff();
        }
        if (! p.active && ! v.isActive() && allocator.isVoiceBusy (static_cast<int> (i)))
            finished[numFinished++] = static_cast<int> (i);
    }

    // Report in the order the voices finished (exact sample inside the sub-block, then index):
    // the allocator's "idle longest" choice must not depend on how the render is split.
    for (int a = 1; a < numFinished; ++a)
    {
        const int v = finished[a];
        int b = a;
        for (; b > 0; --b)
        {
            const int u = finished[b - 1];
            const int tu = voices[static_cast<size_t> (u)].getIdleAt(), tv = voices[static_cast<size_t> (v)].getIdleAt();
            if (tu < tv || (tu == tv && u < v))
                break;
            finished[b] = u;
        }
        finished[b] = v;
    }
    for (int a = 0; a < numFinished; ++a)
        allocator.voiceIdle (finished[a]);
}

void Part::handleEvent (const NoteEvent& e) noexcept
{
    switch (e.type)
    {
        case EventType::NoteOn:
            // The LFO delay restarts when a note starts with no other note held (by a key or by
            // the sustain pedal).
            if (! allocator.anyNoteHeldExcept (e.note))
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
    releaseCountdown.fill (-1);
    late.fill (false);
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
            releaseCountdown[i] = -1; // a new note cancels a delayed release of the old one
            if (p.active)
            {
                // Still fading out for an earlier steal: the newest note starts when the fade ends.
                p.action = a;
            }
            else if (a.type == VoiceAction::Type::Steal && v.isActive())
            {
                v.startFade (stealFadeSamples);
                p = Pending { true, a };
            }
            else
            {
                if (a.type == VoiceAction::Type::Steal)
                    v.reset(); // finished meanwhile: no fade needed
                startVoice (a);
                late[i] = false;
            }
            break;

        case VoiceAction::Type::Retarget:
            if (p.active)
                p.action.note = a.note;
            else
                v.setNote (a.note, target.portaTime > 0.0f && a.glideFrom == VoiceAction::kGlideFromCurrent);
            break;

        case VoiceAction::Type::Release:
            // A stolen note (fading in, or started late) is released as late as it started.
            if (p.active || late[i])
            {
                if (releaseCountdown[i] < 0)
                    releaseCountdown[i] = stealFadeSamples;
            }
            else
            {
                v.noteOff();
            }
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

        // Voices starting from idle get a fixed phase pattern on a random base: quadratic phases
        // k^2 / (2n) cycles have a phasor sum of magnitude exactly sqrt(n) for even n, so the
        // 1/sqrt(n) level holds also at detune 0, the same on every note (random independent
        // phases gave -4.8 .. -26.7 dB). The allocator starts the group with voice 0.
        if (a.voice == 0)
        {
            unisonBase1 = unisonRng.unipolar();
            unisonBase2 = unisonRng.unipolar();
        }
        const double k = static_cast<double> (a.voice);
        const double offset = k * k / (2.0 * static_cast<double> (n));
        auto wrap = [] (double x) { return static_cast<float> (x - std::floor (x)); };
        v.setRestartPhases (wrap (unisonBase1 + offset), wrap (unisonBase2 + offset));
    }

    v.start (a.note, a.velocity, level, pos);
    if (glide)
        v.glideFrom (from);
}
} // namespace vs
