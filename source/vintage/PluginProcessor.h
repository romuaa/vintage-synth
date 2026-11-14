#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "common/shell/BiTimbralProcessor.h"
#include "vintage/engine/Part.h"
#include "vintage/params/ParameterLayout.h"
#include "vintage/params/StateVersion.h"

namespace vs
{
/** Vintage's parameters in one block's read: both parts and the Vintage amount. */
struct VintageParams
{
    PartParams lower, upper;
    float vintage = 0.0f;
};

/**
    Exitium Vintage Synth. Everything instrument-independent (block chunking, MIDI, key modes,
    arp/seq, mixer, effects, output stage, bypass, programs, state) is the shared skeleton
    (common/shell/BiTimbralProcessor.h, docs/architecture.md 8.2); this class supplies the two
    Parts, the per-part parameters and Vintage amount, the synth tail and the preset schema.
*/
class VintageSynthProcessor final : public shell::BiTimbralProcessor<VintageSynthProcessor, Part, VintageParams>
{
    using Base = shell::BiTimbralProcessor<VintageSynthProcessor, Part, VintageParams>;
    friend Base;

public:
    VintageSynthProcessor();
    ~VintageSynthProcessor() override = default;

    /** The synth's own part of the tail: the longest time a released voice can ring (ENV-2). */
    double getSynthTailSeconds() const noexcept;

    int getOversamplingFactor() const noexcept { return lowerPart.getOversamplingFactor(); }

    /** The custom editor (docs/architecture.md 9; vintage/ui/VintageEditor.cpp). */
    juce::AudioProcessorEditor* createEditor() override;

    static constexpr std::uint32_t kLowerSeed = 0x1001u, kUpperSeed = 0x2001u;

    // State identity (used by the skeleton's get/setStateInformation).
    static constexpr const char* kStateTag = "VintageSynthState";
    static constexpr int kStateVersion = vs::kStateVersion;
    /** Brings a state tree saved by an older version up to the current one. */
    static void migrateState (juce::ValueTree& state);

private:
    // ---- Hooks for the skeleton (once per block or chunk, never per sample) ----
    void readInstrumentParams (VintageParams& out) const noexcept;
    void applyInstrumentParams (const VintageParams& p) noexcept;
    float partPan (const VintageParams& p, int index) const noexcept { return index == 0 ? p.lower.pan : p.upper.pan; }
    void prepareParts (double hostRate, int maxBlock, int osFactor, int realtimeOsFactor);
    double synthTailSeconds() const noexcept { return getSynthTailSeconds(); }
    Part& part (int p) noexcept { return p == 0 ? lowerPart : upperPart; }
    const Part& part (int p) const noexcept { return p == 0 ? lowerPart : upperPart; }

    PartParamBinding lowerBinding, upperBinding;
    PartParams lowerParams, upperParams;
    Part lowerPart, upperPart;
    BoundParam vintage; // sanitised read (ParamFactory.h)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VintageSynthProcessor)
};
} // namespace vs
