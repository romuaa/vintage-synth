#include "bside/PluginProcessor.h"

#include "bside/presets/BSidePresets.h"
#include "bside/presets/BSideTables.h"

#include <algorithm>

namespace bs
{
BSideProcessor::BSideProcessor() : Base (createParameterLayout())
{
    lowerBinding.bind (apvts, kLowerPrefix);
    upperBinding.bind (apvts, kUpperPrefix);
    clockBinding.bind (apvts);

    // Presets (after the parameters exist: the schema's defaults are their values right now).
    auto schema = makeBSideSchema (*this);
    auto factory = makeBSideFactoryBank (schema);
    auto sink = std::make_unique<BSideTables>();
    tables = sink.get(); // the preset manager owns it, and outlives every use
    initPresets (std::move (schema), std::move (factory), kPresetProductFolder, std::move (sink));
}

vs::bside::program::ProgramHandover& BSideProcessor::tableHandover (int part) noexcept
{
    return tables->handover (part);
}

void BSideProcessor::abortStaleClaims() const noexcept
{
    for (int p = 0; p < 2; ++p)
        if (claimOpen[static_cast<size_t> (p)])
        {
            tables->handover (p).abort();
            claimOpen[static_cast<size_t> (p)] = false;
        }
}

void BSideProcessor::readInstrumentParams (BSideParams& out) const noexcept
{
    // Inside the block's seqlock window (8.6.7, 8.14): the parameters and both parts' tables as one set. A claim the
    // last block could not commit (its read failed) ends first.
    lowerBinding.read (out.lowerParams);
    upperBinding.read (out.upperParams);
    out.chipClockIndex = clockBinding.read();
    out.claimedTables[0] = out.claimedTables[1] = nullptr;
    if (tables == nullptr)
        return;
    abortStaleClaims();
    for (int p = 0; p < 2; ++p)
    {
        out.claimedTables[p] = tables->handover (p).beginAcquire();
        claimOpen[static_cast<size_t> (p)] = out.claimedTables[p] != nullptr;
    }
}

double BSideProcessor::hostOrArpTempo() const noexcept
{
    // tick.rate = Tempo (8.21.1): the host's tempo while the host gives one (the shell's rule: playing, with a tempo and
    // a position), else arp.tempo
    if (auto* ph = getPlayHead())
        if (const auto pos = ph->getPosition())
            if (pos->getIsPlaying())
                if (const auto bpm = pos->getBpm(); bpm.hasValue() && pos->getPpqPosition().hasValue())
                    return *bpm;
    return getBlockParams().arp.settings.internalBpm;
}

void BSideProcessor::applyInstrumentParams (const BSideParams& p) noexcept
{
    // The read succeeded (the shell calls this only then). Both tables or neither (ProgramHandover.h): a claim that
    // failed (a publish raced with it, which a correct hold makes the read fail as well) keeps both previous tables.
    if (tables != nullptr)
    {
        const bool both = p.claimedTables[0] != nullptr && p.claimedTables[1] != nullptr;
        for (int i = 0; i < 2; ++i)
        {
            if (claimOpen[static_cast<size_t> (i)])
            {
                if (both)
                    tables->handover (i).commit();
                else
                    tables->handover (i).abort();
                claimOpen[static_cast<size_t> (i)] = false;
            }
        }
    }
    tickTempo = hostOrArpTempo();
    for (int i = 0; i < 2; ++i)
    {
        auto& pt = parts[static_cast<size_t> (i)];
        pt.setParams (i == 0 ? p.lowerParams : p.upperParams);
        pt.setChipClock (p.chipClockIndex);
        pt.setTempo (tickTempo);
        // with the committed table's generation: a new one with changed parameters is a program applied (Part.h "PROGRAM
        // APPLY"); every apply publishes the part's table inside the hold that writes its parameters
        if (tables != nullptr)
            pt.setProgram (&tables->handover (i).inUse(), tables->handover (i).inUseGeneration());
        else
            pt.setProgram (nullptr);
    }
}

void BSideProcessor::prepareParts (double hostRate, int maxBlock, int osFactor, int /*realtimeOsFactor*/)
{
    // prepareToPlay read the parameters just now (outside a seqlock window: not concurrent with an apply in a correct
    // host), its table claims included: they are committed here.
    const auto& bp = getBlockParams();
    if (tables != nullptr)
        for (int i = 0; i < 2; ++i)
            if (claimOpen[static_cast<size_t> (i)])
            {
                tables->handover (i).commit();
                claimOpen[static_cast<size_t> (i)] = false;
            }
    tickTempo = bp.arp.settings.internalBpm;
    for (int i = 0; i < 2; ++i)
    {
        auto& pt = parts[static_cast<size_t> (i)];
        const PartParams& pp = i == 0 ? bp.lowerParams : bp.upperParams;
        pt.setParams (pp);
        pt.setChipClock (bp.chipClockIndex);
        pt.setTempo (tickTempo);
        if (tables != nullptr)
            pt.setProgram (&tables->handover (i).inUse(), tables->handover (i).inUseGeneration());
        else
            pt.setProgram (nullptr);
        pt.prepare (hostRate, maxBlock, osFactor, pp, bp.chipClockIndex);
    }
}

double BSideProcessor::synthTailSeconds() const noexcept
{
    // The longest release among the enabled oscillators of the parts that can sound, 3 x its nominal time (8.7,
    // 8.21.1); the shell caps the total. From the parameter atomics: any thread.
    PartParams p[2];
    lowerBinding.read (p[0]);
    upperBinding.read (p[1]);
    const int soundingParts = currentKeyMode() == vs::KeyMode::Whole ? 1 : 2;
    int longestMs = 0;
    for (int i = 0; i < soundingParts; ++i)
    {
        const PartParams& q = p[i];
        const std::array<int, 3> wave { q.osc1Wave, q.osc2Wave, q.osc3Wave };
        const std::array<int, 3> release { q.osc1Release, q.osc2Release, q.osc3Release };
        for (std::size_t n = 0; n < 3; ++n)
            if (wave[n] != kOscWaveOff)
                longestMs = std::max (longestMs, map::envNominalMs (map::EnvStage::Release, release[n]));
    }
    return 3.0 * static_cast<double> (longestMs) / 1000.0;
}

void BSideProcessor::migrateState (juce::ValueTree& state)
{
    // Before v1.0 the version stays 1 and nothing needs migrating; from v1.0 on, each change adds a
    // step here with a test that loads a state saved by the previous version.
    [[maybe_unused]] const int version = state.getProperty (vs::shell::kStateVersionAttr, 1);
    // if (version < 2) { ... }
}
} // namespace bs

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return vs::shell::createPluginProcessor<bs::BSideProcessor>();
}
