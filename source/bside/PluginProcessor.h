#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "bside/engine/TestTonePart.h"
#include "bside/params/ParameterLayout.h"
#include "bside/params/StateVersion.h"
#include "bside/program/ProgramHandover.h"
#include "common/shell/BiTimbralProcessor.h"

#include <array>

namespace bs
{
class BSideTables;

/** B-Side's parameters in one block's read: both parts and the instrument's globals. Nothing yet:
    the shell has only the shared globals (docs/architecture.md 8.3); S2 adds the per-part parameters. */
struct BSideParams
{
};

/**
    Exitium B-Side (docs/architecture.md section 8). Everything instrument-independent (block
    chunking, MIDI, key modes, arp/seq, mixer, effects, output stage, bypass, programs, state) is the
    shared skeleton (common/shell/BiTimbralProcessor.h); this class supplies the two parts, the
    parameter layout, the preset schema and the identity. Until S2 and S4 the parts are test tones
    (engine/TestTonePart.h).
*/
class BSideProcessor final : public vs::shell::BiTimbralProcessor<BSideProcessor, TestTonePart, BSideParams>
{
    using Base = vs::shell::BiTimbralProcessor<BSideProcessor, TestTonePart, BSideParams>;
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

private:
    // ---- Hooks for the skeleton (once per block or chunk, never per sample) ----
    void readInstrumentParams (BSideParams&) const noexcept {}
    void applyInstrumentParams (const BSideParams&) noexcept {}
    float partPan (const BSideParams&, int) const noexcept { return 0.0f; } // S4: mix.pan per part
    void prepareParts (double hostRate, int maxBlock, int osFactor, int realtimeOsFactor);
    double synthTailSeconds() const noexcept { return TestTonePart::kReleaseSeconds; }
    TestTonePart& part (int p) noexcept { return parts[static_cast<size_t> (p == 0 ? 0 : 1)]; }
    const TestTonePart& part (int p) const noexcept { return parts[static_cast<size_t> (p == 0 ? 0 : 1)]; }

    std::array<TestTonePart, 2> parts; // 0 Lower, 1 Upper
    BSideTables* tables = nullptr; // owned by the preset manager (initPresets)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BSideProcessor)
};
} // namespace bs
