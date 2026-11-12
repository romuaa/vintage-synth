#pragma once

// Preset manager: the current bank, program and "modified" state, file load/save, applying
// programs and tones to the parameters, MIDI Program Change requests and the host program list.
// Generic over the instrument's parameter list (PresetModel.h Schema). Message thread only,
// except where a function says otherwise; nothing here runs on the audio thread. The audio thread
// is reached only through ApplyGate (atomics).
//
// User files live under <documents>/Exitium/<product>/{Programs,Tones,Banks}/. Directories are
// created by the first save into them, never earlier, and nothing is ever written anywhere else
// (a save writes exactly the file it is given). All three directories are overridable (Locations),
// so tests never touch the real user directory.

#include "common/presets/ApplyGate.h"
#include "common/presets/PresetFormat.h"
#include "common/presets/PresetModel.h"

#include <atomic>
#include <functional>
#include <memory>

namespace vs::presets
{
struct Locations
{
    juce::File root; // contains Programs/, Tones/, Banks/
    juce::File programs() const { return root.getChildFile ("Programs"); }
    juce::File tones() const { return root.getChildFile ("Tones"); }
    juce::File banks() const { return root.getChildFile ("Banks"); }
    /** <user documents>/Exitium/<product>: nothing is created. */
    static Locations userDefault (const juce::String& product);
};

inline constexpr const char* kProgramExtension = ".vsprog";
inline constexpr const char* kToneExtension = ".vstone";
inline constexpr const char* kBankExtension = ".vsbank";

class PresetManager final : private juce::Timer, private juce::AudioProcessorParameter::Listener
{
public:
    /** The bank the instrument starts with: read-only; user banks are files. */
    PresetManager (juce::AudioProcessorValueTreeState& state, Schema schema, Bank factoryBank, ApplyGate& gate,
                   Locations locations);
    ~PresetManager() override;

    const Schema& getSchema() const noexcept { return schema; }
    ApplyGate& getGate() noexcept { return gate; }
    void setLocations (Locations l);
    Locations getLocations() const;

    // ---- State (any thread; text values are copies) ----
    int currentIndex() const noexcept;           // 0 .. kBankSize - 1 (the queued one, if a host asked from another thread)
    juce::String currentName() const;
    juce::String currentCategory() const;
    juce::String currentAuthor() const;
    bool isModified() const noexcept { return modified.load (std::memory_order_relaxed); }
    juce::String bankName() const;
    bool isFactoryBank() const;
    /** The slot's name in the current bank (the current slot shows the current program's name). */
    juce::String programName (int index) const;
    static juce::String positionName (int index); // 0 -> "A1", 63 -> "H8"

    // ---- Selecting and applying (message thread) ----
    /** Loads program `index` of the current bank into the instrument. False if out of range. */
    bool selectProgram (int index, bool tellHost = true);
    bool selectNext() { return selectProgram ((currentIndex() + 1) % kBankSize); }
    bool selectPrevious() { return selectProgram ((currentIndex() + kBankSize - 1) % kBankSize); }
    /** All parameters at their defaults, named "Init". The slot keeps its place; the program counts as modified. */
    void initialise();
    /** The host's setCurrentProgram: applied at once on the message thread, queued otherwise (any thread). */
    void requestProgram (int index);

    // ---- Files (message thread). Each returns an empty string on success, else a message. ----
    juce::String loadProgramFile (const juce::File& file);
    juce::String saveProgramFile (const juce::File& file, const juce::String& name = {});
    juce::String loadToneFile (const juce::File& file, int part);
    juce::String saveToneFile (const juce::File& file, int part, const juce::String& name = {});
    juce::String loadBankFile (const juce::File& file);
    /** Writes the current bank (and from now on the current bank is that file's user bank). */
    juce::String saveBankFile (const juce::File& file, const juce::String& name = {});
    /** Stores the current sound into slot `index` of the current bank (user banks only). */
    juce::String storeInBank (int index);
    /** Host: renames a slot (user banks) and/or the current program. */
    void renameProgram (int index, const juce::String& name);

    /** Loads a tone or program from in-memory values (used by the file functions and by tests). */
    void applyProgram (const Program& program, int slot, bool markModified, bool tellHost = true);
    void applyTone (const Tone& tone, int part);
    Program captureProgram() const;
    Tone captureTone (int part) const;
    /** Replaces the current bank (no sound change). */
    void setBank (Bank bank, const juce::String& userFilePathOrEmpty);

    // ---- Requests from other threads; call regularly on the message thread (a timer does) ----
    void pollRequests();

    // ---- Session state (host state blob) ----
    juce::ValueTree createSessionInfo() const;
    /** After the parameters were restored by the caller. `info` may be invalid (state saved before presets existed). */
    void restoreSessionInfo (const juce::ValueTree& info);
    /** Runs `restore` (which replaces the parameter values) with the audio thread holding its parameter values. */
    void withParametersHeld (const std::function<void()>& restore);

    // ---- Observers (message thread) ----
    juce::ChangeBroadcaster& changes() noexcept { return broadcaster; }
    /** Test hook: whether the last apply got the audio thread's acknowledgement. */
    bool lastApplyWasAcknowledged() const noexcept { return lastAcknowledged; }

    static constexpr int kPollMs = 30;

private:
    // juce::AudioProcessorParameter::Listener (any thread: atomics only)
    void parameterValueChanged (int parameterIndex, float newValue) override;
    void parameterGestureChanged (int, bool) override {}
    void timerCallback() override { pollRequests(); }

    void applyIndices (const std::vector<int>& indices, const std::vector<float>& values);
    void resyncLastValues();
    void notifyHost();
    void ensureTimer();
    void setCurrent (int index, const Meta& meta, bool isModified);
    static bool onMessageThread() noexcept { return juce::MessageManager::existsAndIsCurrentThread(); }
    juce::String writeFile (const juce::File& file, const std::string& text) const;

    juce::AudioProcessorValueTreeState& apvts;
    Schema schema;
    Bank factory;
    ApplyGate& gate;

    std::vector<juce::RangedAudioParameter*> params;   // by schema index
    std::vector<std::atomic<float>*> raw;              // by schema index
    std::vector<int> processorIndexToSpec;
    std::unique_ptr<std::atomic<float>[]> lastNormalised; // by schema index
    std::atomic<bool> applying { false };
    std::atomic<bool> modified { false };
    std::atomic<int> index { 0 };
    std::atomic<int> queuedProgram { -1 };
    bool lastAcknowledged = true;
    bool timerStarted = false;

    mutable juce::CriticalSection lock; // names and bank: host threads read them
    Locations locations;
    Bank bank;
    bool bankIsFactory = true;
    juce::File bankFile;
    Meta current;

    juce::ChangeBroadcaster broadcaster;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};
} // namespace vs::presets
