#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "bside/engine/Part.h"
#include "bside/params/ParameterLayout.h"
#include "bside/params/StateVersion.h"
#include "bside/program/ProgramHandover.h"
#include "common/shell/BiTimbralProcessor.h"

#include <array>

namespace bs
{
class BSideTables;

/** B-Side's parameters in one block's read: both parts' 63 parameters, the instrument's global (chip.clock) and the
    tables claimed in the same seqlock window (docs/architecture.md 8.14, 8.21.1). No field reuses a shared name
    (8.2.5). Trivially copyable. */
struct BSideParams
{
    PartParams lowerParams {}, upperParams {};
    int chipClockIndex = 0;
    const vs::bside::program::Program* claimedTables[2] { nullptr, nullptr };
};

/**
    Exitium B-Side (docs/architecture.md section 8). Everything instrument-independent (block
    chunking, MIDI, key modes, arp/seq, mixer, effects, output stage, bypass, programs, state) is the
    shared skeleton (common/shell/BiTimbralProcessor.h); this class supplies the two chip parts
    (engine/Part.h), the parameter layout, the preset schema and the identity.
*/
class BSideProcessor final : public vs::shell::BiTimbralProcessor<BSideProcessor, Part, BSideParams>
{
    using Base = vs::shell::BiTimbralProcessor<BSideProcessor, Part, BSideParams>;
    friend Base;

public:
    BSideProcessor();
    ~BSideProcessor() override = default;

    /** The plugin's name as hosts list it (the skeleton's getName); equals the build's JucePlugin_Name (tested). */
    static constexpr const char* kName = "Exitium B-Side";

    // State identity (used by the skeleton's get/setStateInformation).
    static constexpr const char* kStateTag = "BSideState";
    static constexpr int kStateVersion = bs::kStateVersion;
    /** Brings a state tree saved by an older version up to the current one. */
    static void migrateState (juce::ValueTree& state);

    /** The compiled patch table of a part (0 Lower, 1 Upper) as the audio thread reads it (S2c): see
        program/ProgramHandover.h. Applying a program or tone, setTable and a state restore publish here
        inside the same ApplyGate hold as the parameters. Valid for the processor's lifetime. */
    vs::bside::program::ProgramHandover& tableHandover (int part) noexcept;

    /** The tempo the parts' Tempo tick rate used in the last block: the host's while it plays, else arp.tempo. */
    double getTickTempo() const noexcept { return tickTempo; }

private:
    // ---- Hooks for the skeleton (once per block or chunk, never per sample) ----
    void readInstrumentParams (BSideParams&) const noexcept;
    void applyInstrumentParams (const BSideParams&) noexcept;
    float partPan (const BSideParams& p, int part) const noexcept { return part == 0 ? p.lowerParams.mixPan : p.upperParams.mixPan; }
    void prepareParts (double hostRate, int maxBlock, int osFactor, int realtimeOsFactor);
    double synthTailSeconds() const noexcept;
    Part& part (int p) noexcept { return parts[static_cast<size_t> (p == 0 ? 0 : 1)]; }
    const Part& part (int p) const noexcept { return parts[static_cast<size_t> (p == 0 ? 0 : 1)]; }

    /** Ends the claims of the last read that no apply committed (a held block): the writer may reuse those buffers. */
    void abortStaleClaims() const noexcept;
    double hostOrArpTempo() const noexcept;

    std::array<Part, 2> parts; // 0 Lower, 1 Upper
    PartParamBinding lowerBinding, upperBinding;
    ChipClockBinding clockBinding;
    BSideTables* tables = nullptr; // owned by the preset manager (initPresets)
    mutable std::array<bool, 2> claimOpen { false, false }; // audio thread: a beginAcquire not yet committed or aborted
    double tickTempo = 120.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BSideProcessor)
};
} // namespace bs
