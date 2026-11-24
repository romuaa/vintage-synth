#pragma once

// One note voice of Exitium B-Side's part (docs/architecture.md 8.5, 8.21.1): a whole chip with its chain
// (chip::ChipVoice: renderer, filter, output stage), the filter it lends to the chain Per Note, the patch program
// engine that drives it, and the sink between them (writes go to the ChipVoice, never to the Chip inside it).
// Plain state only; the part (Part.h) runs it. JUCE-free, allocation-free, not copyable (the sink and the chain's
// filter pointer point into the voice).

#include "bside/chip/ChipVoice.h"
#include "bside/chip/Filter.h"
#include "bside/engine/ChipProgramSink.h"
#include "bside/engine/ProgramEngine.h"
#include "bside/program/Program.h"
#include "common/engine/VoiceAllocator.h"

#include <cmath>
#include <cstdint>

namespace bs
{
/** The sink the engine of a note voice writes to: the voice's chain, and in the Shared topology a tap that tells the
    part about the filter writes of the most recently started note (8.21.1, "Shared topology"). */
class NoteVoiceSink final : public vs::bside::engine::ProgramSink
{
public:
    enum class FilterWrite : std::uint8_t { cutoff, resonance, mode };
    using Tap = void (*) (void* context, int voice, FilterWrite what) noexcept;

    explicit NoteVoiceSink (vs::bside::chip::ChipVoice& chain) noexcept : inner (chain, false), voiceChain (&chain) {}

    void setAuthentic (bool on) noexcept
    {
        inner.setAuthentic (on);
        authentic = on;
    }
    void forgetInputs() noexcept { inner.forgetInputs(); }
    void setTap (Tap t, void* context, int voiceIndex) noexcept
    {
        tap = t;
        tapContext = context;
        voice = voiceIndex;
    }
    /** The tap is called only while this is on (the part's most recent note in Shared). */
    void tapOn (bool on) noexcept { tapping = on; }

    void frequency (std::uint64_t c, int osc, double v) noexcept override { inner.frequency (c, osc, v); }
    void pulseWidth (std::uint64_t c, int osc, double v) noexcept override { inner.pulseWidth (c, osc, v); }
    void control (std::uint64_t c, int osc, std::uint8_t v) noexcept override { inner.control (c, osc, v); }
    void envelope (std::uint64_t c, int osc, std::uint8_t ad, std::uint8_t sr) noexcept override { inner.envelope (c, osc, ad, sr); }
    void cutoff (std::uint64_t c, double v) noexcept override
    {
        if (authentic && tapping)
        {
            // the two register writes one by one, each seen by the tap, exactly as the chain queues them (ChipVoice::write:
            // a cutoff entry per register), so the shared filter takes the same values at the same places
            const auto fc = static_cast<unsigned> (std::fmin (std::fmax (v, 0.0), 2047.0));
            voiceChain->write (vs::bside::chip::reg::cutoffLo, static_cast<std::uint8_t> (fc & 7u));
            notify (FilterWrite::cutoff);
            voiceChain->write (vs::bside::chip::reg::cutoffHi, static_cast<std::uint8_t> (fc >> 3));
            notify (FilterWrite::cutoff);
            return;
        }
        inner.cutoff (c, v);
        notify (FilterWrite::cutoff);
    }
    void resonanceRouting (std::uint64_t c, std::uint8_t v) noexcept override
    {
        inner.resonanceRouting (c, v);
        notify (FilterWrite::resonance);
    }
    void modeVolume (std::uint64_t c, std::uint8_t v) noexcept override
    {
        inner.modeVolume (c, v);
        notify (FilterWrite::mode);
    }
    void clearEnvelopeTimers (std::uint64_t c) noexcept override { inner.clearEnvelopeTimers (c); }
    void resetChip (std::uint64_t c) noexcept override
    {
        inner.resetChip (c);
        notify (FilterWrite::cutoff); // power-on registers: the shared filter follows them too
        notify (FilterWrite::resonance);
        notify (FilterWrite::mode);
    }

private:
    void notify (FilterWrite w) noexcept
    {
        if (tapping && tap != nullptr)
            tap (tapContext, voice, w);
    }

    vs::bside::engine::ChipVoiceProgramSink inner;
    vs::bside::chip::ChipVoice* voiceChain;
    bool authentic = false;
    Tap tap = nullptr;
    void* tapContext = nullptr;
    int voice = 0;
    bool tapping = false;
};

struct NoteVoice
{
    NoteVoice() noexcept : sink (chain) {}
    NoteVoice (const NoteVoice&) = delete;
    NoteVoice& operator= (const NoteVoice&) = delete;

    enum class Fade : std::uint8_t
    {
        none,
        steal, // a stolen voice: its pending note starts when the fade ends
        leave  // an idle voice that did not fall silent in time: it leaves the sum when the fade ends
    };

    vs::bside::chip::ChipVoice chain;
    vs::bside::chip::Filter filter; // Per Note: lent to the chain
    vs::bside::engine::ProgramEngine engine;
    NoteVoiceSink sink;

    // ---- note state ----
    bool active = false;   // in the part's sum (rendering); false: idle, the chain suspended, chip time skipped
    bool held = false;     // between its note-on and note-off
    bool idleSent = false; // enterIdle done (8.5): renders on until quiet, then leaves
    int note = 60;
    float velocity = 0.0f;
    std::int64_t sinceNoteOff = 0; // internal samples since the note-off (the 30 s rule)
    std::int64_t sinceIdle = 0;    // internal samples since enterIdle (the 500 ms rule)
    std::int64_t quiet = 0;        // consecutive internal samples below -120 dB re full scale at the part's output
    std::int64_t skipPending = 0;  // idle: internal samples of chip time not yet advanced (caught up per block)
    std::uint64_t startOrder = 0;  // which note started most recently (Shared)
    vs::bside::program::Restart restart {}; // the note's restart section, for enterIdle

    // ---- pitch ----
    double glide = 0.0;        // semitones still to glide (decays at the control points)
    double unisonPos = 0.0;    // -0.5 .. 0.5 across the unison spread (0 outside Unison)

    // ---- gains (per internal sample) ----
    double gain = 0.0, gainTarget = 0.0, gainStep = 0.0; // note gain x unison level; 5 ms ramp on a retrigger
    int gainLeft = 0;
    double fade = 1.0, fadeStep = 0.0; // the 3 ms fade (steal, leave)
    int fadeLeft = 0;
    Fade fadeKind = Fade::none;

    // ---- a note waiting for a steal fade or a structural switch ----
    bool pending = false;
    vs::VoiceAction pendingAction {};
    int releaseCountdown = -1; // internal samples until a delayed release (a late-started note keeps its length)
    bool late = false;         // started late (after a steal fade or a switch)
};
} // namespace bs
