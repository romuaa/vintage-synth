// CPU benchmark: renders one part with one held voice and reports cost as % of one
// core in real time. Run: build/mac/tests/voice_bench
#include "engine/Part.h"

#include <chrono>
#include <cstdio>

int main()
{
    constexpr double fs = 48000.0;
    constexpr int block = 256;
    constexpr int seconds = 20;

    for (const int factor : { 2, 4 })
    {
        vs::Part part;
        vs::PartParams p;
        p.vcoSync = 1;
        p.vco1CrossMod = 0.3f;
        p.vcfResonance = 0.6f;
        p.vcoModLfo = 0.1f;
        p.env2Sustain = 1.0f;
        part.setParams (p);
        part.prepare (fs, block, factor, 1u);
        part.setVintage (0.5f);

        juce::MidiBuffer on;
        on.addEvent (juce::MidiMessage::noteOn (1, 48, 0.8f), 0);
        const juce::MidiBuffer none;
        std::vector<float> out (block);

        const int blocks = static_cast<int> (fs) * seconds / block;
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
            part.render (out.data(), block, b == 0 ? on : none, 0);
        const auto t1 = std::chrono::steady_clock::now();

        const double elapsed = std::chrono::duration<double> (t1 - t0).count();
        std::printf ("%dx oversampling: 1 voice = %.3f%% of one core (%.1f voices per 10%%)\n",
                     factor, 100.0 * elapsed / seconds, 10.0 / (100.0 * elapsed / seconds));
    }
    return 0;
}
