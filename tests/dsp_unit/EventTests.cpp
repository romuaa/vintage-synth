#include <catch2/catch_test_macros.hpp>

#include "vintage/PluginProcessor.h"
#include "TestHelpers.h"
#include "common/engine/EventParser.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

// ---- Allocation counter -------------------------------------------------------------------
// macOS only: libmalloc calls malloc_logger (if set) for every allocation, whatever the entry
// point (malloc, calloc, realloc, operator new, juce::HeapBlock). Nothing is replaced, so the
// allocator itself is untouched (an earlier version replaced the global operator new/delete
// in this test executable). Only allocations on the thread that starts the counter are
// counted. AddressSanitizer replaces malloc and bypasses the hook, so the test is skipped there.
#if defined(__has_feature)
 #if __has_feature(address_sanitizer)
  #define VS_TEST_ASAN 1
 #endif
#endif
#if defined(__SANITIZE_ADDRESS__)
 #define VS_TEST_ASAN 1
#endif

#if defined(__APPLE__) && ! defined(VS_TEST_ASAN)
 #define VS_TEST_COUNT_ALLOCATIONS 1
 #include <pthread.h>

using MallocLogger = void (std::uint32_t type, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uint32_t);
extern "C" MallocLogger* malloc_logger; // libmalloc debug hook (used by MallocStackLogging)

namespace
{
std::atomic<int> gAllocations { 0 };
std::atomic<pthread_t> gCountingThread { nullptr };
void* volatile gEscape = nullptr; // keeps probe allocations from being optimised away

void countAllocation (std::uint32_t type, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uint32_t)
{
    constexpr std::uint32_t kAllocate = 2; // MALLOC_LOG_TYPE_ALLOCATE
    if ((type & kAllocate) != 0 && pthread_equal (pthread_self(), gCountingThread.load()) != 0)
        gAllocations.fetch_add (1, std::memory_order_relaxed);
}

struct AllocationCounter
{
    AllocationCounter()
    {
        gAllocations = 0;
        gCountingThread = pthread_self();
        malloc_logger = countAllocation;
    }
    ~AllocationCounter()
    {
        malloc_logger = nullptr;
        gCountingThread = nullptr;
    }
    int count() const { return gAllocations.load(); }
};
} // namespace
#else
 #define VS_TEST_COUNT_ALLOCATIONS 0
#endif

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

TEST_CASE ("Event parsing: malformed channel messages are rejected, not reinterpreted", "[events]")
{
    vs::NoteEvent e {};
    CHECK_FALSE (parseOne ({ 0xB0, 0xF8, 0x10 }, e)); // would mask to CC 120 (all sound off)
    CHECK_FALSE (parseOne ({ 0xB0, 123, 0x80 }, e));  // status byte as the value
    CHECK_FALSE (parseOne ({ 0xE0, 0x00 }, e));       // 2-byte bend (would be a full down-bend)
    CHECK_FALSE (parseOne ({ 0x90, 60 }, e));         // 2-byte note-on (would be a note-off)
    CHECK_FALSE (parseOne ({ 0x80, 60 }, e));         // 2-byte note-off
    CHECK_FALSE (parseOne ({ 0xB0, 64 }, e));         // 2-byte controller
    CHECK_FALSE (parseOne ({ 0x90, 0xBC, 100 }, e));  // note number with the high bit set
    CHECK (parseOne ({ 0x90, 60, 100, 0x00 }, e));    // trailing bytes are ignored
    CHECK (e.type == EventType::NoteOn);
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
        // The marker is AllNotesOff, which in this synth releases every key AND clears the
        // sustain pedal (docs/architecture.md section 2): the dropped tail may have held the
        // pedal-up. The Part side is tested in PartTests ("AllNotesOff ... clears the pedal").
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

TEST_CASE ("Processor: event-buffer overflow with the sustain pedal down never hangs a note", "[events][processor][pedal]")
{
    // M2b defers key releases while the pedal is down. Here the pedal-up and the note-offs are
    // dropped with the overflow; the appended All Notes Off must clear the pedal and release.
    for (const int mode : { 0, 1, 2 }) // Poly, Unison, Solo
    {
        JuceEnv env;
        vs::VintageSynthProcessor proc;
        constexpr double sr = 48000.0;
        constexpr int block = 512;
        proc.getState().getParameter ("lower.assign.mode")->setValueNotifyingHost (static_cast<float> (mode) / 2.0f);
        proc.setPlayConfigDetails (0, 2, sr, block);
        proc.prepareToPlay (sr, block);

        juce::MidiBuffer flood;
        flood.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 0);
        flood.addEvent (juce::MidiMessage::noteOn (1, 64, static_cast<juce::uint8> (100)), 0);
        flood.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 1); // pedal down
        flood.addEvent (juce::MidiMessage::noteOff (1, 60), 2);              // held by the pedal
        for (int i = 0; i < 3000; ++i)
            flood.addEvent (juce::MidiMessage::pitchWheel (1, 8192), 3 + i % 400);
        flood.addEvent (juce::MidiMessage::noteOff (1, 64), 500);            // dropped
        flood.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 501); // dropped

        juce::AudioBuffer<float> buffer (2, block);
        proc.processBlock (buffer, flood);
        CHECK (buffer.getMagnitude (0, 0, block) > 0.01f);

        // The pedal must be up now: a later note is released by its own note-off.
        juce::MidiBuffer later;
        later.addEvent (juce::MidiMessage::noteOn (1, 67, static_cast<juce::uint8> (100)), 10);
        later.addEvent (juce::MidiMessage::noteOff (1, 67), 200);
        proc.processBlock (buffer, later);

        juce::MidiBuffer none;
        float tailPeak = 0.0f;
        for (int b = 0; b < static_cast<int> (sr) / block; ++b)
        {
            proc.processBlock (buffer, none);
            if (b > 40)
                tailPeak = std::max (tailPeak, buffer.getMagnitude (0, 0, block));
        }
        INFO ("assign mode " << mode);
        CHECK (tailPeak < 1.0e-5f);
    }
}

TEST_CASE ("Processor: processBlock does not allocate (malloc or new), even with SysEx in the MIDI buffer", "[events][processor][realtime]")
{
#if ! VS_TEST_COUNT_ALLOCATIONS
    SKIP ("allocation counting uses the macOS malloc_logger hook (not available here or under ASan)");
#else
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
    midi.addEvent (juce::MidiMessage::noteOn (1, 64, static_cast<juce::uint8> (90)), 2000); // past the end
    juce::AudioBuffer<float> buffer (2, block * 3); // larger than announced: chunked rendering
    juce::AudioBuffer<float> empty (2, 0);
    juce::MidiBuffer none;

    {
        // The hook sees both entry points (guards against a silently disabled counter).
        AllocationCounter counter;
        void* raw = std::malloc (64);
        gEscape = raw;
        std::free (raw);
        std::vector<float> probe (static_cast<size_t> (buffer.getNumSamples()));
        gEscape = probe.data();
        CHECK (counter.count() >= 2);
    }

    int allocations = 0;
    {
        AllocationCounter counter;
        proc.processBlock (buffer, midi);
        proc.processBlock (empty, midi); // zero-length block with events
        proc.processBlock (buffer, none);
        allocations = counter.count();
    }
    CHECK (allocations == 0);
    CHECK (buffer.getMagnitude (0, 0, buffer.getNumSamples()) > 0.01f);

    // M3: both parts, key-mode changes (release, Upper resume), split moves, pan and balance.
    // Parameters are set outside the counted region (the host does that on its own thread).
    auto set = [&proc] (const char* id, float plain) {
        auto* p = proc.getState().getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
    };
    const std::pair<const char*, float> steps[] { { "key.mode", 1.0f },  { "mix.balance", 0.5f },     { "key.mode", 2.0f },
                                                  { "key.split_point", 40.0f }, { "upper.mix.pan", -0.7f }, { "key.mode", 0.0f },
                                                  { "key.mode", 1.0f } };
    int m3Allocations = 0;
    for (const auto& [id, v] : steps)
    {
        set (id, v);
        AllocationCounter counter;
        proc.processBlock (buffer, midi);
        proc.processBlock (buffer, none);
        m3Allocations += counter.count();
    }
    CHECK (m3Allocations == 0);
    CHECK (proc.isUpperRendering());
#endif
}

namespace
{
/** A prepared processor at 48 kHz with 256-sample blocks. */
struct ProcRig
{
    JuceEnv env;
    vs::VintageSynthProcessor proc;
    juce::AudioBuffer<float> buffer { 2, 256 };

    ProcRig()
    {
        proc.setPlayConfigDetails (0, 2, 48000.0, 256);
        proc.prepareToPlay (48000.0, 256);
    }

    /** Peak of the next numBlocks blocks without MIDI, ignoring the first skipBlocks. */
    float peakAfter (int numBlocks, int skipBlocks = 0)
    {
        juce::MidiBuffer none;
        float peak = 0.0f;
        for (int b = 0; b < numBlocks; ++b)
        {
            proc.processBlock (buffer, none);
            if (b >= skipBlocks)
                peak = std::max (peak, buffer.getMagnitude (0, 0, buffer.getNumSamples()));
        }
        return peak;
    }
};

juce::MidiBuffer single (const juce::MidiMessage& m, int position)
{
    juce::MidiBuffer b;
    b.addEvent (m, position);
    return b;
}
} // namespace

TEST_CASE ("Processor: an event at a negative position is applied at the block start", "[events][processor]")
{
    ProcRig rig;
    auto on = single (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), -5);
    rig.proc.processBlock (rig.buffer, on);
    CHECK (rig.peakAfter (20) > 0.01f);
}

TEST_CASE ("Processor: a note-off at or past the block end is not lost", "[events][processor]")
{
    for (const int position : { 256, 300 })
    {
        ProcRig rig;
        auto on = single (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 0);
        rig.proc.processBlock (rig.buffer, on);
        auto off = single (juce::MidiMessage::noteOff (1, 60), position);
        rig.proc.processBlock (rig.buffer, off);
        INFO ("note-off at " << position);
        CHECK (rig.peakAfter (100, 60) < 1.0e-5f); // released (63 ms) well before 0.32 s
    }
}

TEST_CASE ("Processor: a zero-length block still applies its events", "[events][processor]")
{
    ProcRig rig;
    juce::AudioBuffer<float> empty (2, 0);

    auto on = single (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 0);
    rig.proc.processBlock (empty, on);
    CHECK (rig.peakAfter (20) > 0.01f);

    auto off = single (juce::MidiMessage::noteOff (1, 60), 0);
    rig.proc.processBlock (empty, off);
    CHECK (rig.peakAfter (100, 60) < 1.0e-5f);
}
