#pragma once

// Preset manager: the current bank, program and "modified" state, file load/save, applying
// programs and tones to the parameters, MIDI Program Change requests and the host program list.
// Generic over the instrument (PresetModel.h Schema and Instrument). Nothing here runs on the
// audio thread; the audio thread is reached only through ApplyGate (atomics).
//
// Threads:
//   - Every write of parameters (applying a program or tone, a state restore) runs under one
//     mutex, writerLock(), and inside a hold of the ApplyGate. The mutex is never taken on the
//     audio thread. Holding it across a host's get/setStateInformation makes a saved state never
//     half an apply, and a restore never interleave with an apply on another thread.
//   - Files are read and written on the message thread only, never from setStateInformation
//     (hosts call it on any thread): a session names its user bank, and the bank is looked up and
//     read later by pollRequests() on the message thread (a timer calls it). The sound restored
//     from a session never depends on the bank file.
//   - Names, the bank and the flags can be read from any thread (copies, under a small lock).
//
// Patch tables (an instrument whose Instrument has validateTable; docs/architecture.md 8.8): each
// tone has an optional table, held here as validated, canonical text lines. Applying a program or
// tone, setTable and a state restore replace tables inside the same ApplyGate hold as the
// parameters, and hand each changed table to the TableSink the instrument supplied (which compiles
// and publishes it for the audio thread). A table that does not validate never gets that far: a file
// is rejected whole, a session gives that part an empty table and an error (tableError) for the
// editor. Instruments without tables: all of this is inert (no state child, no sink call).
//
// User files live under <documents>/Exitium/<product>/{Programs,Tones,Banks}/. Directories are
// created by the first save into them, never earlier, and nothing is ever written anywhere else
// (a save writes exactly the file it is given, through a temporary file next to it). All three
// directories are overridable (Locations), so tests never touch the real user directory.
// A session stores its user bank as a plain file name inside Banks/ (never a path); a bank
// loaded from elsewhere is used, but a session saved with it reports it as not found later.

#include "common/presets/ApplyGate.h"
#include "common/presets/PresetFormat.h"
#include "common/presets/PresetModel.h"

#include <atomic>
#include <functional>
#include <memory>

namespace vs::presets
{
/** The child of a session's state tree that holds the tables (createTablesInfo). */
inline constexpr const char* kTablesInfoType = "Tables";

struct Locations
{
    juce::File root; // contains Programs/, Tones/, Banks/
    juce::File programs() const { return root.getChildFile ("Programs"); }
    juce::File tones() const { return root.getChildFile ("Tones"); }
    juce::File banks() const { return root.getChildFile ("Banks"); }
    /** <user documents>/Exitium/<product>: nothing is created. */
    static Locations userDefault (const juce::String& product);
};

/** A table made ready for the audio thread by TableSink::prepareTable (the sink's own type). */
struct PreparedTable
{
    virtual ~PreparedTable() = default;
};

/** The instrument's side of a table change, in two steps so that nothing that can fail or allocate runs
    inside the ApplyGate hold.
      prepareTable   BEFORE the hold, on the message thread or a host's state thread: builds whatever the
                     audio thread will use (B-Side: the compiled program) from validated lines (empty =
                     no table). May allocate; may throw or return nullptr when the table cannot be used,
                     in which case the table counts as rejected (an empty table and an error).
      publishTable   INSIDE the hold and under writerLock(): hands the prepared table over. Must not
                     allocate, wait, take the manager's locks or fail. The audio thread never calls either. */
class TableSink
{
public:
    virtual ~TableSink() = default;
    virtual std::unique_ptr<PreparedTable> prepareTable (int tone, const std::vector<std::string>& lines) = 0;
    virtual void publishTable (int tone, PreparedTable& prepared) noexcept = 0;
    /** The prepared "no table", ready in advance (used when a table is rejected). */
    virtual PreparedTable& emptyTable (int tone) noexcept = 0;
};

enum class BankStatus
{
    Factory, // the built-in bank
    User,    // a user bank file, loaded
    Pending, // a session named a user bank; it is looked up on the message thread soon
    Missing  // a session named a user bank that could not be loaded: the factory bank is shown
};

class PresetManager final : private juce::Timer, private juce::AudioProcessorParameter::Listener
{
public:
    /** The bank the instrument starts with is `factoryBank`: read-only; user banks are files. */
    PresetManager (juce::AudioProcessorValueTreeState& state, Schema schema, Bank factoryBank, ApplyGate& gate,
                   Locations locations, std::unique_ptr<TableSink> tableSink = nullptr);
    ~PresetManager() override;

    const Schema& getSchema() const noexcept { return schema; }
    int bankSize() const noexcept { return schema.bankSize(); }
    ApplyGate& getGate() noexcept { return gate; }
    void setLocations (Locations l);
    Locations getLocations() const;
    /** Serialises every parameter write (applies and state restores) and state saves. Never take it
        on the audio thread, and never while holding anything that the message thread waits for. */
    juce::CriticalSection& writerLock() noexcept { return writer; }

    // ---- State (any thread; text values are copies) ----
    /** The current slot (the queued one, if a host asked from another thread). A program that is in
        no slot (detached, see isDetached) reports slot 0: the host program list needs an index. */
    int currentIndex() const noexcept;
    /** True when the sound belongs to no slot of the current bank (its session's bank was not found). */
    bool isDetached() const noexcept { return detached.load (std::memory_order_relaxed); }
    juce::String currentName() const;
    juce::String currentCategory() const;
    juce::String currentAuthor() const;
    bool isModified() const noexcept { return modified.load (std::memory_order_relaxed); }
    juce::String bankName() const;
    bool isFactoryBank() const;
    BankStatus bankStatus() const;
    /** The slot's name in the current bank (the current slot shows the current program's name). */
    juce::String programName (int index) const;
    juce::String positionName (int slot) const { return schema.instrument().positionName (slot); }

    // ---- Selecting and applying (message thread) ----
    /** Loads program `index` of the current bank into the instrument. False if out of range. */
    bool selectProgram (int index, bool tellHost = true);
    bool selectNext() { return selectProgram ((currentIndex() + 1) % bankSize()); }
    bool selectPrevious() { return selectProgram ((currentIndex() + bankSize() - 1) % bankSize()); }
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
    /** Stores the current sound into slot `index` of the current user bank and writes the bank file
        at once. If the file cannot be written nothing changes (and the message says why). */
    juce::String storeInBank (int index);
    /** Renames the current program's displayed name if `index` is its slot, and the slot of a user
        bank (written to the bank file at once; if that fails nothing changes). Any thread. */
    juce::String renameProgram (int index, const juce::String& name);

    /** Loads a program from in-memory values. slot < 0: the program belongs to no slot (detached). */
    void applyProgram (const Program& program, int slot, bool markModified, bool tellHost = true);
    void applyTone (const Tone& tone, int part);
    Program captureProgram() const;
    Tone captureTone (int part) const;
    /** Replaces the current bank (no sound change). userFile: the bank's file, or juce::File() for the factory bank. */
    void setBank (Bank bank, const juce::File& userFile);

    // ---- Requests from other threads; call regularly on the message thread (a timer does) ----
    /** Applies a queued host or MIDI program request and resolves a session's user bank. */
    void pollRequests();

    // ---- Session state (host state blob; call both under writerLock()) ----
    juce::ValueTree createSessionInfo() const;
    /** The session's tables: a child "Tables" with one text property per tone (the tone key, e.g.
        "lower"): the lines, verbatim, joined by LF. An invalid tree for an instrument without tables. */
    juce::ValueTree createTablesInfo() const;
    /** The tables of a session, checked and prepared, ready to commit. */
    struct PreparedTables
    {
        struct Item
        {
            std::vector<std::string> lines;
            juce::String error;
            std::shared_ptr<PreparedTable> prepared;
        };
        std::vector<Item> items; // by tone; empty for an instrument without tables
    };
    /** Does the checking and preparing of a session's tables (any thread, no locks, no I/O).
        `info` may be invalid (a session without tables: every tone gets no table). Never throws; an
        unusable table becomes "no table" plus an error. Do this before withParametersHeld. */
    PreparedTables prepareTablesInfo (const juce::ValueTree& info) const;
    /** Replaces every tone's table with the prepared ones. Only inside withParametersHeld's `restore`. */
    void restoreTables (PreparedTables&& prepared);
    /** After the parameters were restored by the caller. `info` may be invalid (state saved before
        presets existed). Any thread; does no file I/O (the bank is resolved by pollRequests()). */
    void restoreSessionInfo (const juce::ValueTree& info);
    /** Tells the host and the bar about a restore; call after releasing writerLock() (a host
        callback must not run while another thread may be waiting for that lock). */
    void sessionRestored();
    /** Runs `restore` (which replaces the parameter values) under writerLock() with the audio
        thread holding its parameter values. */
    void withParametersHeld (const std::function<void()>& restore);

    // ---- Patch tables (any thread; only for an instrument with tables, else empty and inert) ----
    bool hasTables() const noexcept { return schema.instrument().hasTables(); }
    /** The table of tone `part`: validated lines exactly as given (empty = no table). */
    std::vector<std::string> currentTable (int part) const;
    /** Why the part has no table although the session saved one ("" = nothing wrong). Set by a state
        restore, cleared by the next apply or setTable that replaces that part's table. */
    juce::String tableError (int part) const;
    /** Replaces tone `part`'s table (message thread; the editor's way in). Returns "" on success;
        otherwise the message, and nothing changed. The lines are kept verbatim. A table is program
        content: the program counts as modified unless the canonical form is unchanged. The host is told
        that non-parameter state changed when the text changed. */
    juce::String setTable (int part, const std::vector<std::string>& lines);

    // ---- Observers (message thread) ----
    juce::ChangeBroadcaster& changes() noexcept { return broadcaster; }
    /** Test hook: whether the last apply got the audio thread's acknowledgement. */
    bool lastApplyWasAcknowledged() const noexcept { return lastAcknowledged.load (std::memory_order_relaxed); }

    /** Test hook: whether the poll timer runs (it is started by the constructor, on any thread). */
    bool isPolling() const noexcept { return isTimerRunning(); }

    static constexpr int kPollMs = 30;

private:
    // juce::AudioProcessorParameter::Listener (any thread: atomics only)
    void parameterValueChanged (int parameterIndex, float newValue) override;
    void parameterGestureChanged (int, bool) override {}
    void timerCallback() override { pollRequests(); }

    struct TableUpdate
    {
        int tone = 0;
        std::vector<std::string> lines;
        juce::String error;
        std::shared_ptr<PreparedTable> prepared; // from the sink; null for a rejected table or without a sink
    };
    /** Validates `lines` and has the sink prepare them; on failure the lines are empty and `error` says why (prefixed by `context`). */
    TableUpdate prepareTable (int tone, const std::vector<std::string>& lines, const juce::String& context) const;
    void applyIndices (const std::vector<int>& indices, const std::vector<float>& values, const std::vector<TableUpdate>& tableUpdates = {});
    /** Stores and publishes the tables. Inside a gate hold and under writer. */
    void commitTables (const std::vector<TableUpdate>& updates);
    juce::String partLabel (int tone) const;
    /** True if the two tables are the same (identical lines, or the same canonical form). */
    bool sameTable (const std::vector<std::string>& a, const std::vector<std::string>& b) const;
    /** Tells the host that state that is not a parameter changed (message thread only: a call from another thread is skipped). */
    void notifyNonParameterState();
    void resyncLastValues();
    void notifyHost();
    void broadcastIfMessageThread();
    void setCurrent (int slot, const Meta& meta, bool nowModified, bool nowDetached);
    void resolvePendingBank();
    static bool onMessageThread() noexcept { return juce::MessageManager::existsAndIsCurrentThread(); }
    juce::String writeFile (const juce::File& file, const std::string& text) const;
    /** Writes `b` to the current user bank's file; empty string on success. */
    juce::String writeUserBank (const Bank& b, juce::File& fileOut) const;

    juce::AudioProcessorValueTreeState& apvts;
    Schema schema;
    Bank factory;
    ApplyGate& gate;

    std::vector<juce::RangedAudioParameter*> params;   // by schema index
    std::vector<std::atomic<float>*> raw;              // by schema index
    std::vector<int> processorIndexToSpec;
    std::unique_ptr<std::atomic<float>[]> lastNormalised; // by schema index
    std::atomic<int> applying { 0 };                   // writes in progress (nested): not user edits
    std::atomic<bool> modified { false }, detached { false }, resolvePending { false };
    std::atomic<int> index { 0 };
    std::atomic<int> queuedProgram { -1 };
    std::atomic<bool> lastAcknowledged { true };

    juce::CriticalSection writer;       // serialises parameter writers (see writerLock())
    mutable juce::CriticalSection lock; // names, bank, locations and tables: host threads read them
    Locations locations;
    Bank bank;
    BankStatus status = BankStatus::Factory;
    juce::File bankFile;         // status User
    juce::String bankReference;  // status Pending / Missing: the file name the session named
    juce::String bankDisplayName; // status Pending / Missing: the bank's name as the session had it
    std::uint32_t generation = 0; // bumped by every bank change: a stale lookup result is dropped
    Meta current;

    std::unique_ptr<TableSink> sink;
    std::vector<std::vector<std::string>> tables; // by tone (lock); empty vectors for an instrument without tables
    std::vector<juce::String> tableErrors;        // by tone (lock)

    juce::ChangeBroadcaster broadcaster;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};
} // namespace vs::presets
