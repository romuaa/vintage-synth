#include <catch2/catch_test_macros.hpp>

#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "engine/EventParser.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <vector>

// ---- Allocation counter -------------------------------------------------------------------
// Replaces the global allocation functions for this test executable. Only allocations made
// on a thread that has counting switched on are counted, so Catch2/JUCE background work does
// not interfere.
namespace
{
thread_local bool tCountAllocations = false;
std::atomic<int> gAllocations { 0 };
void* volatile gEscape = nullptr; // keeps the probe allocation from being optimised away

void* countedAlloc (std::size_t size)
{
    if (tCountAllocations)
        gAllocations.fetch_add (1, std::memory_order_relaxed);
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

struct AllocationCounter
{
    AllocationCounter() { gAllocations = 0; tCountAllocations = true; }
    ~AllocationCounter() { tCountAllocations = false; }
    int count() const { return gAllocations.load(); }
};
} // namespace

void* operator new (std::size_t size) { return countedAlloc (size); }
void* operator new[] (std::size_t size) { return countedAlloc (size); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

// ---- Helpers ------------------------------------------------------------------------------
namespace
{
using vs::EventType;

bool parseOne (std::initializer_list<int> bytes, vs::NoteEvent& e, int offset = 0)
{
    std::vector<std::uint8_t> raw;
    for (const int b : bytes)
        raw.push_back (static_cast<std::uint8_t> (b));
    return vs::parseMidiMessage (raw.data(), static_cast<int> (raw.size()), offset, e);
}

struct JuceEnv
{
    juce::ScopedJuceInitialiser_GUI init;
};
} // namespace

TEST_CASE ("Event parsing: every event type", "[events]")
{
    vs::NoteEvent e {};

    REQUIRE (parseOne ({ 0x90, 60, 127 }, e, 17));
    CHECK (e.type == EventType::NoteOn);
    CHECK (e.note == 60);
    CHECK (std::abs (e.value - (1.0f)) < 1.0e-7f);
    CHECK (e.offset == 17);

    REQUIRE (parseOne ({ 0x9F, 61, 64 }, e)); // channel 16: omni
    CHECK (e.type == EventType::NoteOn);
    CHECK (e.note == 61);
    CHECK (std::abs (e.value - 64.0f / 127.0f) < 1.0e-7f);

    REQUIRE (parseOne ({ 0x83, 62, 40 }, e));
    CHECK (e.type == EventType::NoteOff);
    CHECK (e.note == 62);

    REQUIRE (parseOne ({ 0xE0, 0, 0 }, e));
    CHECK (e.type == EventType::PitchBend);
    CHECK (std::abs (e.value - (-1.0f)) < 1.0e-7f);
    REQUIRE (parseOne ({ 0xE5, 0, 0x40 }, e));
    CHECK (std::abs (e.value - (0.0f)) < 1.0e-7f);
    REQUIRE (parseOne ({ 0xE0, 0x7F, 0x7F }, e));
    CHECK (std::abs (e.value - (8191.0f / 8192.0f)) < 1.0e-7f);

    REQUIRE (parseOne ({ 0xB0, 1, 127 }, e));
    CHECK (e.type == EventType::ModWheel);
    CHECK (std::abs (e.value - (1.0f)) < 1.0e-7f);
    REQUIRE (parseOne ({ 0xB0, 1, 0 }, e));
    CHECK (std::abs (e.value - (0.0f)) < 1.0e-7f);

    REQUIRE (parseOne ({ 0xB0, 64, 63 }, e));
    CHECK (e.type == EventType::SustainPedal);
    CHECK (std::abs (e.value - (0.0f)) < 1.0e-7f);
    REQUIRE (parseOne ({ 0xB2, 64, 64 }, e));
    CHECK (std::abs (e.value - (1.0f)) < 1.0e-7f);

    REQUIRE (parseOne ({ 0xB0, 123, 0 }, e));
    CHECK (e.type == EventType::AllNotesOff);
    REQUIRE (parseOne ({ 0xB0, 120, 0 }, e));
    CHECK (e.type == EventType::AllSoundOff);
    REQUIRE (parseOne ({ 0xB0, 121, 0 }, e));
    CHECK (e.type == EventType::ResetControllers);
}

TEST_CASE ("Event parsing: note-on with velocity 0 is a note-off", "[events]")
{
    vs::NoteEvent e {};
    REQUIRE (parseOne ({ 0x90, 69, 0 }, e));
    CHECK (e.type == EventType::NoteOff);
    CHECK (e.note == 69);
}

TEST_CASE ("Event parsing: unused, system and malformed messages are dropped", "[events]")
{
    vs::NoteEvent e {};
    CHECK_FALSE (parseOne ({ 0xB0, 7, 100 }, e));  // volume CC: not used by a Part
    CHECK_FALSE (parseOne ({ 0xB0, 33, 5 }, e));   // mod wheel LSB
    CHECK_FALSE (parseOne ({ 0xC0, 5 }, e));       // program change
    CHECK_FALSE (parseOne ({ 0xD0, 50 }, e));      // channel pressure
    CHECK_FALSE (parseOne ({ 0xA0, 60, 10 }, e));  // poly pressure
    CHECK_FALSE (parseOne ({ 0xF0, 0x7D, 1, 2, 3, 0xF7 }, e)); // SysEx
    CHECK_FALSE (parseOne ({ 0xF8 }, e));          // clock
    CHECK_FALSE (parseOne ({ 0xFF }, e));          // reset
    CHECK_FALSE (parseOne ({ 0xF2, 0, 0 }, e));    // song position
    CHECK_FALSE (parseOne ({ 60, 100 }, e));       // running status data bytes
    CHECK_FALSE (parseOne ({ 0x90 }, e));          // truncated
    CHECK_FALSE (vs::parseMidiMessage (nullptr, 3, 0, e));
}

TEST_CASE ("Event buffer: overflow drops the rest and appends AllNotesOff", "[events]")
{
    vs::EventParser parser;
    parser.prepare (16);
    const std::uint8_t on[] { 0x90, 60, 100 }, off[] { 0x80, 60, 0 }, bend[] { 0xE0, 0, 0x40 };

    SECTION ("exactly capacity - 1 events fit without overflow")
    {
        for (int i = 0; i < 15; ++i)
            parser.add (bend, 3, i);
        parser.finish();
        CHECK_FALSE (parser.didOverflow());
        CHECK (parser.events().size() == 15);
    }

    SECTION ("overflow")
    {
        parser.add (on, 3, 0);
        for (int i = 0; i < 40; ++i)
            parser.add (bend, 3, 1 + i);
        parser.add (off, 3, 100); // dropped
        parser.finish();

        const auto ev = parser.events();
        REQUIRE (parser.didOverflow());
        REQUIRE (ev.size() == 16);
        CHECK (ev[0].type == EventType::NoteOn);
        for (size_t i = 1; i < 15; ++i)
            CHECK (ev[i].type == EventType::PitchBend);
        CHECK (ev[15].type == EventType::AllNotesOff);
        CHECK (ev[15].offset == 100); // the last offset of the chunk
        for (size_t i = 1; i < ev.size(); ++i)
            CHECK (ev[i].offset >= ev[i - 1].offset);
    }

    SECTION ("clear starts a new chunk")
    {
        for (int i = 0; i < 40; ++i)
            parser.add (bend, 3, i);
        parser.finish();
        parser.clear();
        parser.add (on, 3, 3);
        parser.finish();
        CHECK_FALSE (parser.didOverflow());
        REQUIRE (parser.events().size() == 1);
        CHECK (parser.events()[0].offset == 3);
    }
}

TEST_CASE ("Processor: event-buffer overflow never hangs a note", "[events][processor]")
{
    JuceEnv env;
    vs::VintageSynthProcessor proc;
    constexpr double sr = 48000.0;
    constexpr int block = 512;
    proc.setPlayConfigDetails (0, 2, sr, block);
    proc.prepareToPlay (sr, block);

    // One block with a note-on, 3000 pitch-bend messages (more than the 2048-event buffer)
    // and the note-off at the end: the note-off is dropped with the overflow.
    juce::MidiBuffer flood;
    flood.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 0);
    for (int i = 0; i < 3000; ++i)
        flood.addEvent (juce::MidiMessage::pitchWheel (1, 8192), 1 + i % 400);
    flood.addEvent (juce::MidiMessage::noteOff (1, 60), 500);

    juce::AudioBuffer<float> buffer (2, block);
    proc.processBlock (buffer, flood);

    // Without the appended AllNotesOff the voice would sustain forever (ENV-2 sustain 0.8).
    juce::MidiBuffer none;
    float tailPeak = 0.0f;
    for (int b = 0; b < static_cast<int> (sr) / block; ++b)
    {
        proc.processBlock (buffer, none);
        if (b > 40) // after ~0.45 s the release (63 ms default) is long over
            tailPeak = std::max (tailPeak, buffer.getMagnitude (0, 0, block));
    }
    CHECK (tailPeak < 1.0e-5f);
}

TEST_CASE ("Processor: processBlock does not allocate, even with SysEx in the MIDI buffer", "[events][processor][realtime]")
{
    JuceEnv env;
    vs::VintageSynthProcessor proc;
    constexpr double sr = 48000.0;
    constexpr int block = 256;
    proc.setPlayConfigDetails (0, 2, sr, block);
    proc.prepareToPlay (sr, block);

    std::vector<juce::uint8> sysex (300, 0x11); // longer than MidiMessage's inline storage
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::createSysExMessage (sysex.data(), static_cast<int> (sysex.size())), 0);
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 10);
    midi.addEvent (juce::MidiMessage::pitchWheel (1, 12000), 20);
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, 90), 30);
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 40);
    midi.addEvent (juce::MidiMessage::midiClock(), 50);
    juce::AudioBuffer<float> buffer (2, block * 3); // larger than announced: chunked rendering
    juce::MidiBuffer empty;

    {
        // The counter itself works (guards against a silently disabled replacement).
        AllocationCounter counter;
        std::vector<float> probe (static_cast<size_t> (buffer.getNumSamples()));
        gEscape = probe.data();
        CHECK (counter.count() >= 1);
    }

    int allocations = 0;
    {
        AllocationCounter counter;
        proc.processBlock (buffer, midi);
        proc.processBlock (buffer, empty);
        allocations = counter.count();
    }
    CHECK (allocations == 0);
    CHECK (buffer.getMagnitude (0, 0, buffer.getNumSamples()) > 0.01f);
}
