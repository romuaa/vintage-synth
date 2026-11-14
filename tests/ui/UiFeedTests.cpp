// The audio -> editor feed (docs/architecture.md 9.1.4, 9.6 "UiFeed"): the peak and gain-min
// protocols, the held-order seqlock under a writer thread, and what the processor publishes at the
// end of every block (step 9): peaks after the output stage, held and sounding keys per part in
// Whole/Dual/Split, block count, tempo, bypass. That the feed leaves the audio unchanged is proved
// by the render-regression bit dumps and the block-size tests (the feed is write-only for the engine).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../integration/IntegrationRig.h"
#include "common/engine/UiFeed.h"

#include <atomic>
#include <thread>

using namespace vs;
using namespace vs::itest;
using Catch::Approx;

namespace
{
std::vector<int> notesOf (const std::atomic<std::uint64_t> (&mask)[2])
{
    std::vector<int> n;
    for (int k = 0; k < 128; ++k)
        if (UiFeed::hasNote (mask, k))
            n.push_back (k);
    return n;
}

void setPlain (juce::AudioProcessor& proc, const juce::String& id, float plain)
{
    for (auto* p : proc.getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p); r != nullptr && r->paramID == id)
            r->setValueNotifyingHost (r->convertTo0to1 (plain));
}
} // namespace

TEST_CASE ("UiFeed: peaks only rise until taken; the limiter gain only falls until taken", "[ui][feed]")
{
    UiFeed f;
    f.raisePeak (0, 0.5f);
    f.raisePeak (0, 0.25f); // smaller: kept 0.5
    f.raisePeak (1, 0.75f);
    CHECK (f.takePeak (0) == Approx (0.5f));
    CHECK (f.takePeak (0) == Approx (0.0f)); // taken: reset
    CHECK (f.takePeak (1) == Approx (0.75f));
    f.lowerLimiterGain (0.9f);
    f.lowerLimiterGain (0.95f);
    CHECK (f.takeLimiterGainMin() == Approx (0.9f));
    CHECK (f.takeLimiterGainMin() == Approx (1.0f));
}

TEST_CASE ("UiFeed: the held-order seqlock never hands out a torn copy (writer thread, 10^6 writes)", "[ui][feed]")
{
    UiFeed f;
    std::atomic<bool> stop { false };
    constexpr std::uint32_t writes = 1000000;
    std::thread writer ([&] {
        UiFeed::Order o {};
        for (std::uint32_t i = 1; i <= writes; ++i)
        {
            // Invariant: bytes 0..14 = (i + k) & 0x7F, byte 15 = their sum & 0xFF.
            unsigned sum = 0;
            for (int k = 0; k < 15; ++k)
            {
                o[static_cast<size_t> (k)] = static_cast<std::uint8_t> ((i + static_cast<std::uint32_t> (k)) & 0x7Fu);
                sum += o[static_cast<size_t> (k)];
            }
            o[15] = static_cast<std::uint8_t> (sum & 0xFFu);
            f.writeOrder (0, o);
        }
        stop = true;
    });
    long reads = 0, consistent = 0, refused = 0;
    while (! stop.load())
    {
        UiFeed::Order got {};
        ++reads;
        if (! f.readOrder (0, got))
        {
            ++refused;
            continue;
        }
        if (got[0] == 0xFF) // nothing written yet
            continue;
        unsigned sum = 0;
        bool sequence = true;
        for (int k = 0; k < 15; ++k)
        {
            sum += got[static_cast<size_t> (k)];
            sequence = sequence && got[static_cast<size_t> (k)] == static_cast<std::uint8_t> ((got[0] + k) & 0x7F);
        }
        REQUIRE (sequence);
        REQUIRE (got[15] == static_cast<std::uint8_t> (sum & 0xFFu));
        ++consistent;
    }
    writer.join();
    UiFeed::Order last {};
    REQUIRE (f.readOrder (0, last));
    CHECK (last[0] == static_cast<std::uint8_t> (writes & 0x7Fu));
    CHECK (consistent > 0);
    WARN ("seqlock: " << reads << " reads, " << consistent << " consistent, " << refused << " refused (kept the previous order)");
}

TEST_CASE ("UiFeed from the processor: block count, peaks after the output stage, tempo and transport", "[ui][feed]")
{
    Rig rig;
    rig.prepare();
    auto& feed = rig.proc.getUiFeed();
    CHECK (feed.internalRate.load() == Approx (96000.0f)); // 48 kHz x 2 (real time)
    const auto before = feed.blockCount.load();
    const auto out = rig.render (rig.samples (0.3), { { 0, juce::MidiMessage::noteOn (1, 60, 1.0f) } }, 256);
    CHECK (feed.blockCount.load() - before == static_cast<std::uint32_t> ((rig.samples (0.3) + 255) / 256));
    float peakL = 0.0f, peakR = 0.0f;
    for (size_t i = 0; i < out.l.size(); ++i)
    {
        peakL = std::max (peakL, std::abs (out.l[i]));
        peakR = std::max (peakR, std::abs (out.r[i]));
    }
    REQUIRE (peakL > 0.01f);
    CHECK (feed.takePeak (0) == Approx (peakL)); // the largest block peak since the last take
    CHECK (feed.takePeak (1) == Approx (peakR));
    CHECK (feed.takeLimiterGainMin() <= 1.0f);
    CHECK_FALSE (feed.hostPlaying.load());
    CHECK (feed.hostBpm.load() == Approx (120.0f)); // the arp/seq's own tempo while stopped

    rig.play (97.0, 0.0);
    rig.render (512, {}, 256);
    CHECK (feed.hostPlaying.load());
    CHECK (feed.hostBpm.load() == Approx (97.0f));
}

TEST_CASE ("UiFeed from the processor: held and sounding keys per part in Whole, Dual and Split", "[ui][feed]")
{
    Rig rig;
    rig.prepare();
    auto& feed = rig.proc.getUiFeed();
    auto& lower = feed.part[0];
    auto& upper = feed.part[1];

    SECTION ("Whole: everything is Lower")
    {
        rig.render (256, { { 0, juce::MidiMessage::noteOn (1, 48, 0.8f) }, { 0, juce::MidiMessage::noteOn (1, 72, 0.8f) } }, 256);
        CHECK (notesOf (lower.held) == std::vector<int> { 48, 72 });
        CHECK (notesOf (lower.sounding) == std::vector<int> { 48, 72 });
        CHECK (notesOf (upper.held).empty());
        CHECK (notesOf (upper.sounding).empty());
        rig.render (256, { { static_cast<int> (rig.now), juce::MidiMessage::noteOff (1, 48) } }, 256);
        CHECK (notesOf (lower.held) == std::vector<int> { 72 });
        CHECK (notesOf (lower.sounding) == std::vector<int> { 48, 72 }); // 48 is in its release
    }
    SECTION ("Dual: both parts")
    {
        setPlain (rig.proc, "key.mode", 1.0f);
        rig.render (256, {}, 256);
        rig.render (256, { { static_cast<int> (rig.now), juce::MidiMessage::noteOn (1, 60, 0.8f) } }, 256);
        CHECK (notesOf (lower.held) == std::vector<int> { 60 });
        CHECK (notesOf (upper.held) == std::vector<int> { 60 });
        CHECK (notesOf (lower.sounding) == std::vector<int> { 60 });
        CHECK (notesOf (upper.sounding) == std::vector<int> { 60 });
    }
    SECTION ("Split at C4: below Lower, the split key and above Upper")
    {
        setPlain (rig.proc, "key.mode", 2.0f);
        setPlain (rig.proc, "key.split_point", 60.0f);
        rig.render (256, {}, 256);
        const int t = static_cast<int> (rig.now);
        rig.render (256, { { t, juce::MidiMessage::noteOn (1, 59, 0.8f) }, { t, juce::MidiMessage::noteOn (1, 60, 0.8f) },
                           { t, juce::MidiMessage::noteOn (1, 100, 0.8f) } },
                    256);
        CHECK (notesOf (lower.held) == std::vector<int> { 59 });
        CHECK (notesOf (upper.held) == std::vector<int> { 60, 100 });
        CHECK (notesOf (lower.sounding) == std::vector<int> { 59 });
        CHECK (notesOf (upper.sounding) == std::vector<int> { 60, 100 });
    }
}

TEST_CASE ("UiFeed from the processor: bypassed blocks count, publish no peaks and no keys", "[ui][feed]")
{
    Rig rig;
    rig.prepare();
    auto& feed = rig.proc.getUiFeed();
    rig.render (rig.samples (0.1), { { 0, juce::MidiMessage::noteOn (1, 64, 1.0f) } }, 256);
    feed.takePeak (0);
    feed.takePeak (1);
    rig.bypass = true;
    const auto before = feed.blockCount.load();
    rig.render (rig.samples (0.05), {}, 256); // fade, then silence
    CHECK (feed.blockCount.load() > before);
    CHECK (feed.takePeak (0) <= 0.0f);
    CHECK (feed.takePeak (1) <= 0.0f);
    CHECK (notesOf (feed.part[0].held).empty());
    CHECK (feed.part[0].seqStep.load() == -1);
}
