// Loads the built .vst3 through JUCE's VST3 host, like a DAW would, and checks the basics:
// identity, parameter count, rendering at several rates/block sizes, silence after release,
// and state round-trip between two instances. Usage: host_smoke "<path>/Exitium Vintage Synth.vst3"
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdio>

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    if (! ok)
        ++failures;
}

struct RenderStats
{
    float peak = 0.0f;
    bool finite = true;
};

/** Our own parameters: excludes the JUCE VST3 wrapper's MIDI-CC emulation params and Bypass. */
juce::Array<juce::AudioProcessorParameter*> ownParameters (juce::AudioPluginInstance& p)
{
    juce::Array<juce::AudioProcessorParameter*> result;
    for (auto* param : p.getParameters())
    {
        const auto name = param->getName (100);
        if (! name.startsWith ("MIDI CC") && name != "Bypass")
            result.add (param);
    }
    return result;
}

RenderStats render (juce::AudioPluginInstance& p, int numSamples, int blockSize, juce::MidiBuffer firstBlockMidi)
{
    RenderStats st;
    juce::AudioBuffer<float> buf (2, blockSize);
    for (int done = 0; done < numSamples; done += blockSize)
    {
        buf.clear();
        juce::MidiBuffer midi;
        if (done == 0)
            midi.swapWith (firstBlockMidi);
        p.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < blockSize; ++i)
            {
                const float v = buf.getSample (ch, i);
                st.finite = st.finite && std::isfinite (v);
                st.peak = std::max (st.peak, std::abs (v));
            }
    }
    return st;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: host_smoke <plugin.vst3>\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI init;
    juce::AudioPluginFormatManager formats;
    juce::addHeadlessDefaultFormatsToManager (formats);

    juce::AudioPluginFormat* vst3 = nullptr;
    for (auto* f : formats.getFormats())
        if (f->getName() == "VST3")
            vst3 = f;
    check (vst3 != nullptr, "VST3 hosting available");
    if (vst3 == nullptr)
        return 1;

    juce::OwnedArray<juce::PluginDescription> types;
    vst3->findAllTypesForFile (types, juce::String::fromUTF8 (argv[1]));
    check (types.size() == 1, "exactly one plugin in bundle (" + juce::String (types.size()) + ")");
    if (types.isEmpty())
        return 1;

    const auto& desc = *types[0];
    check (desc.name == "Exitium Vintage Synth", "name: " + desc.name);
    check (desc.manufacturerName == "Exitium", "vendor: " + desc.manufacturerName);
    check (desc.isInstrument, "is instrument");

    juce::String error;
    auto plugin = formats.createPluginInstance (desc, 48000.0, 512, error);
    check (plugin != nullptr, "instantiate " + error);
    if (plugin == nullptr)
        return 1;

    const int numParams = ownParameters (*plugin).size();
    check (numParams == 2 + 2 * 48 + 3, "own parameter count " + juce::String (numParams)
                                        + " (total incl. wrapper params " + juce::String (plugin->getParameters().size()) + ")");

    for (const double sr : { 44100.0, 48000.0, 96000.0 })
        for (const int bs : { 32, 512 })
        {
            plugin->setPlayConfigDetails (0, 2, sr, bs);
            plugin->prepareToPlay (sr, bs);

            juce::MidiBuffer on;
            on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 3);
            const auto playing = render (*plugin, static_cast<int> (sr), bs, on);

            juce::MidiBuffer off;
            off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            render (*plugin, static_cast<int> (sr * 2), bs, off);
            const auto after = render (*plugin, static_cast<int> (sr / 2), bs, {});

            const auto tag = juce::String (sr) + " Hz / " + juce::String (bs) + ": ";
            check (playing.finite && playing.peak > 0.01f && playing.peak < 1.0f, tag + "note renders, peak " + juce::String (playing.peak));
            check (after.finite && after.peak < 1.0e-4f, tag + "silent after release, peak " + juce::String (after.peak));
            plugin->releaseResources();
        }

    // State round-trip: change some parameters, save, load into a fresh instance, compare.
    const auto params = ownParameters (*plugin);
    for (int i = 0; i < params.size(); i += 3)
        params[i]->setValueNotifyingHost (static_cast<float> ((i * 37) % 100) / 100.0f);
    juce::MemoryBlock state;
    plugin->getStateInformation (state);

    auto second = formats.createPluginInstance (desc, 48000.0, 512, error);
    check (second != nullptr, "instantiate second instance");
    if (second == nullptr)
        return 1;
    second->setStateInformation (state.getData(), static_cast<int> (state.getSize()));

    const auto params2 = ownParameters (*second);
    int mismatches = params.size() == params2.size() ? 0 : -1;
    for (int i = 0; mismatches >= 0 && i < params.size(); ++i)
    {
        // Stepped params: the host keeps the raw value it sent, the plugin snaps it; compare step index.
        const int steps = params[i]->getNumSteps();
        const bool discrete = params[i]->isDiscrete() && steps > 1 && steps < 1000;
        const auto stepOf = [steps] (float v) { return juce::roundToInt (v * static_cast<float> (steps - 1)); };
        const bool same = discrete ? stepOf (params[i]->getValue()) == stepOf (params2[i]->getValue())
                                   : std::abs (params[i]->getValue() - params2[i]->getValue()) <= 1.0e-4f;
        if (! same)
        {
            ++mismatches;
            std::printf ("      %s: %f vs %f (%d steps)\n", params[i]->getName (60).toRawUTF8(),
                         static_cast<double> (params[i]->getValue()), static_cast<double> (params2[i]->getValue()),
                         params[i]->getNumSteps());
        }
    }
    check (mismatches == 0, "state round-trip, mismatches: " + juce::String (mismatches));

    second.reset();
    plugin.reset();
    std::printf ("%s\n", failures == 0 ? "HOST SMOKE: PASS" : "HOST SMOKE: FAIL");
    return failures == 0 ? 0 : 1;
}
