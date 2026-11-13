#include "common/presets/PresetManager.h"

#include "common/presets/PresetFiles.h"
#include "common/presets/StrictJson.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vs::presets
{
namespace
{
constexpr const char* kInfoType = "Preset";

juce::String errorFor (const juce::File& f, const juce::String& what) { return what + " (" + f.getFileName() + ")"; }

bool sameBits (float a, float b) noexcept { return std::memcmp (&a, &b, sizeof (float)) == 0; }

/** A session flag written as "1"/"0" (or "true"/"false"); anything else is `fallback`. */
bool parseFlag (const juce::var& v, bool fallback)
{
    if (v.isBool())
        return static_cast<bool> (v);
    const auto s = v.toString();
    if (s == "1" || s == "true")
        return true;
    if (s == "0" || s == "false")
        return false;
    return fallback;
}
} // namespace

Locations Locations::userDefault (const juce::String& product)
{
    Locations l;
    l.root = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Exitium").getChildFile (product);
    return l;
}

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& state, Schema s, Bank factoryBank, ApplyGate& g, Locations loc)
    : apvts (state), schema (std::move (s)), factory (std::move (factoryBank)), gate (g), locations (std::move (loc))
{
    jassert (schema.isValid());
    jassert (static_cast<int> (factory.programs.size()) == schema.bankSize());

    const auto n = static_cast<size_t> (schema.numParams());
    params.assign (n, nullptr);
    raw.assign (n, nullptr);
    lastNormalised = std::make_unique<std::atomic<float>[]> (n);
    int maxProcessorIndex = 0;
    for (int i = 0; i < schema.numParams(); ++i)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (schema.spec (i).id));
        jassert (p != nullptr);
        params[static_cast<size_t> (i)] = p;
        raw[static_cast<size_t> (i)] = apvts.getRawParameterValue (schema.spec (i).id);
        maxProcessorIndex = std::max (maxProcessorIndex, p->getParameterIndex());
    }
    processorIndexToSpec.assign (static_cast<size_t> (maxProcessorIndex) + 1u, -1);
    for (int i = 0; i < schema.numParams(); ++i)
        processorIndexToSpec[static_cast<size_t> (params[static_cast<size_t> (i)]->getParameterIndex())] = i;
    resyncLastValues();
    for (auto* p : params)
        p->addListener (this);

    bank = factory;
    current = bank.programs[0].meta;
    // JUCE timers may be started from any thread (a host can construct plugins off the message
    // thread); the callback always runs on the message thread.
    startTimer (kPollMs);
}

PresetManager::~PresetManager()
{
    stopTimer();
    for (auto* p : params)
        p->removeListener (this);
}

void PresetManager::setLocations (Locations l)
{
    const juce::ScopedLock sl (lock);
    locations = std::move (l);
}

Locations PresetManager::getLocations() const
{
    const juce::ScopedLock sl (lock);
    return locations;
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
int PresetManager::currentIndex() const noexcept
{
    const int q = queuedProgram.load (std::memory_order_acquire);
    return q >= 0 ? q : index.load (std::memory_order_relaxed);
}

juce::String PresetManager::currentName() const { const juce::ScopedLock sl (lock); return current.name; }
juce::String PresetManager::currentCategory() const { const juce::ScopedLock sl (lock); return current.category; }
juce::String PresetManager::currentAuthor() const { const juce::ScopedLock sl (lock); return current.author; }
bool PresetManager::isFactoryBank() const { const juce::ScopedLock sl (lock); return status == BankStatus::Factory; }
BankStatus PresetManager::bankStatus() const { const juce::ScopedLock sl (lock); return status; }

juce::String PresetManager::bankName() const
{
    const juce::ScopedLock sl (lock);
    return status == BankStatus::Pending || status == BankStatus::Missing ? bankDisplayName : bank.name;
}

juce::String PresetManager::programName (int i) const
{
    if (i < 0 || i >= bankSize())
        return {};
    const juce::ScopedLock sl (lock);
    const bool isCurrent = ! detached.load (std::memory_order_relaxed) && i == index.load (std::memory_order_relaxed);
    return isCurrent ? current.name : bank.programs[static_cast<size_t> (i)].meta.name;
}

void PresetManager::setCurrent (int i, const Meta& meta, bool nowModified, bool nowDetached)
{
    {
        const juce::ScopedLock sl (lock);
        index.store (nowDetached ? 0 : std::clamp (i, 0, bankSize() - 1), std::memory_order_relaxed);
        detached.store (nowDetached, std::memory_order_relaxed);
        current = meta;
    }
    modified.store (nowModified, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// Applying
// ---------------------------------------------------------------------------------------------
void PresetManager::resyncLastValues()
{
    for (size_t i = 0; i < params.size(); ++i)
        lastNormalised[i].store (params[i]->getValue(), std::memory_order_relaxed);
}

void PresetManager::parameterValueChanged (int parameterIndex, float newValue)
{
    // Any thread (the audio thread when a host automates): atomics only.
    if (parameterIndex < 0 || static_cast<size_t> (parameterIndex) >= processorIndexToSpec.size())
        return;
    const int i = processorIndexToSpec[static_cast<size_t> (parameterIndex)];
    if (i < 0)
        return;
    const float previous = lastNormalised[static_cast<size_t> (i)].exchange (newValue, std::memory_order_relaxed);
    if (applying.load (std::memory_order_acquire) > 0 || schema.isExcluded (i))
        return;
    if (previous < newValue || previous > newValue)
        modified.store (true, std::memory_order_relaxed);
}

void PresetManager::applyIndices (const std::vector<int>& indices, const std::vector<float>& values)
{
    jassert (onMessageThread());
    // Under the writer lock: 1. hold the audio thread's parameter values  2. have it release every
    // note (it acknowledges at the start of its next block, or we give up when no audio runs)
    // 3. write all the parameters, then let the audio thread read them at once. See ApplyGate.h.
    const juce::ScopedLock w (writer);
    applying.fetch_add (1, std::memory_order_acq_rel);
    gate.beginHold();
    lastAcknowledged.store (gate.requestReleaseAndWait(), std::memory_order_relaxed);
    for (const int i : indices)
    {
        auto* p = params[static_cast<size_t> (i)];
        const float target = values[static_cast<size_t> (i)];
        // Written unless the parameter holds exactly this value already (no needless host event;
        // never "close enough": a loaded program is the file's values, as far as the host's
        // normalised representation can carry them).
        if (sameBits (raw[static_cast<size_t> (i)]->load (std::memory_order_relaxed), target))
            continue;
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (target));
        p->endChangeGesture();
    }
    resyncLastValues();
    gate.endHold();
    applying.fetch_sub (1, std::memory_order_acq_rel);
}

void PresetManager::withParametersHeld (const std::function<void()>& restore)
{
    const juce::ScopedLock w (writer);
    applying.fetch_add (1, std::memory_order_acq_rel);
    gate.beginHold();
    restore();
    resyncLastValues();
    gate.endHold();
    applying.fetch_sub (1, std::memory_order_acq_rel);
}

void PresetManager::notifyHost()
{
    apvts.processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
}

void PresetManager::broadcastIfMessageThread()
{
    if (onMessageThread())
        broadcaster.sendSynchronousChangeMessage();
}

void PresetManager::applyProgram (const Program& program, int slot, bool markModified, bool tellHost)
{
    jassert (static_cast<int> (program.values.size()) == schema.numParams());
    std::vector<int> indices = schema.globalParams();
    for (int t = 0; t < schema.numTones(); ++t)
        for (int k = 0; k < schema.toneSize(); ++k)
            indices.push_back (schema.toneParam (t, k));
    {
        // Parameters and the program's name change together for anyone saving state meanwhile.
        const juce::ScopedLock w (writer);
        applyIndices (indices, program.values);
        setCurrent (slot, program.meta, markModified, slot < 0);
    }
    if (tellHost)
        notifyHost();
    broadcaster.sendSynchronousChangeMessage();
}

void PresetManager::applyTone (const Tone& tone, int part)
{
    jassert (part >= 0 && part < schema.numTones());
    jassert (static_cast<int> (tone.values.size()) == schema.toneSize());
    std::vector<int> indices;
    std::vector<float> values (static_cast<size_t> (schema.numParams()), 0.0f);
    for (int k = 0; k < schema.toneSize(); ++k)
    {
        const int i = schema.toneParam (part, k);
        indices.push_back (i);
        values[static_cast<size_t> (i)] = tone.values[static_cast<size_t> (k)];
    }
    {
        const juce::ScopedLock w (writer);
        applyIndices (indices, values);
        modified.store (true, std::memory_order_relaxed); // the sound no longer is the loaded program
    }
    broadcaster.sendSynchronousChangeMessage();
}

bool PresetManager::selectProgram (int i, bool tellHost)
{
    if (i < 0 || i >= bankSize())
        return false;
    Program p;
    {
        const juce::ScopedLock sl (lock);
        if (status == BankStatus::Pending || status == BankStatus::Missing)
        {
            // Choosing a program of the bank that is shown (the factory bank) ends the wait for,
            // or the report of, the session's user bank.
            status = BankStatus::Factory;
            bank = factory;
            bankReference.clear();
            ++generation;
        }
        p = bank.programs[static_cast<size_t> (i)];
    }
    applyProgram (p, i, false, tellHost);
    return true;
}

void PresetManager::initialise()
{
    auto init = makeInitProgram (schema);
    applyProgram (init, isDetached() ? -1 : currentIndex(), true);
}

void PresetManager::requestProgram (int i)
{
    if (i < 0 || i >= bankSize())
        return;
    if (onMessageThread())
        selectProgram (i, false); // the host asked: it knows already, and an edit of the same parameter must not be
                                  // answered from inside its own call
    else
        queuedProgram.store (i, std::memory_order_release);
}

void PresetManager::pollRequests()
{
    jassert (onMessageThread());
    if (resolvePending.exchange (false, std::memory_order_acq_rel))
        resolvePendingBank();
    const int queued = queuedProgram.exchange (-1, std::memory_order_acq_rel);
    if (queued >= 0)
        selectProgram (queued);
    // MIDI Program Change: applied here, some time after the block that carried it (see ApplyGate.h).
    // Program Changes outside the bank (64..127) are ignored; there is no Bank Select.
    const int midi = gate.takeMidiProgram();
    if (midi >= 0 && midi < bankSize())
        selectProgram (midi);
}

Program PresetManager::captureProgram() const
{
    Program p;
    p.values.resize (static_cast<size_t> (schema.numParams()));
    for (int i = 0; i < schema.numParams(); ++i)
        p.values[static_cast<size_t> (i)] = schema.sanitise (i, static_cast<double> (raw[static_cast<size_t> (i)]->load (std::memory_order_relaxed)));
    const juce::ScopedLock sl (lock);
    p.meta = current;
    return p;
}

Tone PresetManager::captureTone (int part) const
{
    return toneOf (schema, captureProgram(), part);
}

void PresetManager::setBank (Bank b, const juce::File& userFile)
{
    {
        const juce::ScopedLock sl (lock);
        ++generation;
        bankReference.clear();
        if (userFile == juce::File())
        {
            status = BankStatus::Factory;
            bankFile = juce::File();
            bank = factory;
        }
        else
        {
            status = BankStatus::User;
            bankFile = userFile;
            bank = std::move (b);
            bank.programs.resize (static_cast<size_t> (bankSize()), makeInitProgram (schema));
        }
    }
    notifyHost();
    broadcaster.sendSynchronousChangeMessage();
}

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------
juce::String PresetManager::writeFile (const juce::File& file, const std::string& text) const
{
    if (! file.getParentDirectory().createDirectory())
        return errorFor (file, "The folder could not be created");
    // Written to a uniquely named temporary file next to the target and moved over it, so a failed
    // write never damages an existing file.
    juce::TemporaryFile tmp (file, juce::TemporaryFile::useHiddenFile);
    if (! tmp.getFile().replaceWithData (text.data(), text.size()))
        return errorFor (file, "The file could not be written");
    if (! tmp.overwriteTargetFileWithTemporary())
        return errorFor (file, "The file could not be replaced");
    return {};
}

juce::String PresetManager::loadProgramFile (const juce::File& file)
{
    std::string data;
    if (auto e = readPresetFile (file, kMaxFileBytes, true, data); e.isNotEmpty())
        return e;
    const auto parsed = parseProgram (schema, data);
    if (! parsed.ok)
        return errorFor (file, parsed.error);
    applyProgram (parsed.value, isDetached() ? -1 : currentIndex(), false);
    return {};
}

juce::String PresetManager::saveProgramFile (const juce::File& file, const juce::String& name)
{
    auto p = captureProgram();
    if (name.isNotEmpty())
        p.meta.name = sanitiseName (name);
    if (auto e = writeFile (file, toJson (schema, p)); e.isNotEmpty())
        return e;
    {
        const juce::ScopedLock sl (lock);
        current.name = sanitiseName (p.meta.name);
    }
    modified.store (false, std::memory_order_relaxed);
    broadcaster.sendSynchronousChangeMessage();
    return {};
}

juce::String PresetManager::loadToneFile (const juce::File& file, int part)
{
    if (part < 0 || part >= schema.numTones())
        return "No such part.";
    std::string data;
    if (auto e = readPresetFile (file, kMaxFileBytes, true, data); e.isNotEmpty())
        return e;
    const auto parsed = parseTone (schema, data);
    if (! parsed.ok)
        return errorFor (file, parsed.error);
    applyTone (parsed.value, part);
    return {};
}

juce::String PresetManager::saveToneFile (const juce::File& file, int part, const juce::String& name)
{
    if (part < 0 || part >= schema.numTones())
        return "No such part.";
    auto t = captureTone (part);
    if (name.isNotEmpty())
        t.meta.name = sanitiseName (name);
    return writeFile (file, toJson (schema, t));
}

juce::String PresetManager::loadBankFile (const juce::File& file)
{
    std::string data;
    if (auto e = readPresetFile (file, kMaxFileBytes, true, data); e.isNotEmpty())
        return e;
    const auto parsed = parseBank (schema, data);
    if (! parsed.ok)
        return errorFor (file, parsed.error);
    setBank (parsed.value, file);
    selectProgram (0);
    return {};
}

juce::String PresetManager::saveBankFile (const juce::File& file, const juce::String& name)
{
    Bank b;
    {
        const juce::ScopedLock sl (lock);
        b = bank;
    }
    if (name.isNotEmpty())
        b.name = sanitiseName (name);
    if (auto e = writeFile (file, toJson (schema, b)); e.isNotEmpty())
        return e;
    {
        const juce::ScopedLock sl (lock);
        ++generation;
        bank = b;
        status = BankStatus::User;
        bankFile = file;
        bankReference.clear();
    }
    notifyHost();
    broadcaster.sendSynchronousChangeMessage();
    return {};
}

juce::String PresetManager::writeUserBank (const Bank& b, juce::File& fileOut) const
{
    {
        const juce::ScopedLock sl (lock);
        fileOut = bankFile;
    }
    if (fileOut == juce::File())
        return "The bank has no file.";
    return writeFile (fileOut, toJson (schema, b));
}

juce::String PresetManager::storeInBank (int i)
{
    if (i < 0 || i >= bankSize())
        return "No such slot.";
    auto p = captureProgram();
    p.meta.name = sanitiseName (p.meta.name);
    Bank updated;
    std::uint32_t gen = 0;
    {
        const juce::ScopedLock sl (lock);
        if (status != BankStatus::User)
            return "The factory bank is read-only. Save the bank to a file first; then the copy can be edited.";
        updated = bank;
        gen = generation;
    }
    updated.programs[static_cast<size_t> (i)] = p;
    juce::File written;
    if (auto e = writeUserBank (updated, written); e.isNotEmpty())
        return e; // memory unchanged: the slot still holds what the file holds
    {
        const juce::ScopedLock sl (lock);
        if (gen != generation || bankFile != written)
            return "The bank changed while it was being written.";
        bank = std::move (updated);
    }
    setCurrent (i, p.meta, false, false);
    notifyHost();
    broadcastIfMessageThread();
    return {};
}

juce::String PresetManager::renameProgram (int i, const juce::String& name)
{
    if (i < 0 || i >= bankSize())
        return "No such slot.";
    const auto n = sanitiseName (name);
    bool userBank = false;
    Bank updated;
    std::uint32_t gen = 0;
    {
        const juce::ScopedLock sl (lock);
        userBank = status == BankStatus::User;
        if (userBank)
        {
            updated = bank;
            gen = generation;
        }
    }
    if (userBank)
    {
        updated.programs[static_cast<size_t> (i)].meta.name = n;
        juce::File written;
        if (auto e = writeUserBank (updated, written); e.isNotEmpty())
            return e; // nothing renamed
        const juce::ScopedLock sl (lock);
        if (gen != generation || bankFile != written)
            return "The bank changed while it was being written.";
        bank = std::move (updated);
    }
    {
        const juce::ScopedLock sl (lock);
        if (! detached.load (std::memory_order_relaxed) && i == index.load (std::memory_order_relaxed))
            current.name = n;
    }
    notifyHost();
    broadcastIfMessageThread();
    return {};
}

// ---------------------------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------------------------
juce::ValueTree PresetManager::createSessionInfo() const
{
    juce::ValueTree t (kInfoType);
    const juce::ScopedLock sl (lock);
    juce::String reference;
    if (status == BankStatus::User)
        reference = bankFile.getParentDirectory() == locations.banks() ? bankFile.getFileName() : juce::String(); // a plain name, never a path
    else if (status == BankStatus::Pending || status == BankStatus::Missing)
        reference = bankReference;
    const bool isUser = status != BankStatus::Factory;
    t.setProperty ("bankKind", isUser ? "user" : "factory", nullptr);
    t.setProperty ("bank", reference, nullptr);
    t.setProperty ("bankName", isUser && status != BankStatus::User ? bankDisplayName : bank.name, nullptr);
    t.setProperty ("program", detached.load (std::memory_order_relaxed) ? -1 : index.load (std::memory_order_relaxed), nullptr);
    t.setProperty ("name", current.name, nullptr);
    t.setProperty ("category", current.category, nullptr);
    t.setProperty ("author", current.author, nullptr);
    t.setProperty ("modified", modified.load (std::memory_order_relaxed), nullptr);
    return t;
}

void PresetManager::restoreSessionInfo (const juce::ValueTree& info)
{
    // The sound itself came with the parameters; this only restores what the preset bar and the
    // host program list show. No file is touched here: a user bank is looked up later, on the
    // message thread (resolvePendingBank). Every value is untrusted and parsed with bounds.
    const juce::ScopedLock w (writer);
    Meta meta;
    int slot = 0;
    bool wasModified = true;
    bool userBank = false;
    juce::String reference, displayName;

    if (info.isValid() && info.hasType (kInfoType))
    {
        slot = json::parseBoundedInt (info.getProperty ("program", 0).toString().toStdString(), -1, bankSize() - 1, 0);
        meta.name = sanitiseName (info.getProperty ("name", "Untitled").toString());
        meta.category = sanitiseText (info.getProperty ("category", "").toString(), kMaxCategoryChars);
        meta.author = sanitiseText (info.getProperty ("author", "").toString(), kMaxAuthorChars);
        wasModified = parseFlag (info.getProperty ("modified", true), true);
        userBank = info.getProperty ("bankKind", "factory").toString() == "user";
        // Only a plain file name is ever used (checked again when it is resolved); an absolute
        // "bankPath" from an earlier development build is ignored.
        reference = info.getProperty ("bank", "").toString().substring (0, 256);
        displayName = sanitiseName (info.getProperty ("bankName", "").toString());
    }
    else
    {
        meta.name = "Untitled"; // a session saved before presets existed: the sound is not any known program
    }

    {
        const juce::ScopedLock sl (lock);
        ++generation;
        bank = factory;
        bankFile = juce::File();
        status = userBank ? BankStatus::Pending : BankStatus::Factory;
        bankReference = userBank ? reference : juce::String();
        bankDisplayName = userBank ? displayName : juce::String();
    }
    setCurrent (slot, meta, wasModified, slot < 0);
    queuedProgram.store (-1, std::memory_order_relaxed);
    if (userBank)
        resolvePending.store (true, std::memory_order_release);
    // The host and the bar are told by sessionRestored(), after the caller released the writer lock.
}

void PresetManager::sessionRestored()
{
    notifyHost();
    broadcastIfMessageThread();
}

void PresetManager::resolvePendingBank()
{
    jassert (onMessageThread());
    juce::String reference;
    std::uint32_t gen = 0;
    juce::File folder;
    {
        const juce::ScopedLock sl (lock);
        if (status != BankStatus::Pending)
            return;
        reference = bankReference;
        gen = generation;
        folder = locations.banks();
    }
    const auto& inst = schema.instrument();
    Bank loaded;
    bool ok = false;
    const auto file = resolveBankFile (folder, reference, inst.bankExtension);
    if (file != juce::File())
    {
        std::string data;
        if (readPresetFile (file, kMaxFileBytes, false, data).isEmpty())
        {
            auto parsed = parseBank (schema, data);
            if (parsed.ok)
            {
                loaded = std::move (parsed.value);
                ok = true;
            }
        }
    }
    {
        const juce::ScopedLock sl (lock);
        if (gen != generation || status != BankStatus::Pending)
            return; // something else changed the bank meanwhile
        if (ok)
        {
            status = BankStatus::User;
            bank = std::move (loaded);
            bank.programs.resize (static_cast<size_t> (bankSize()), makeInitProgram (schema));
            bankFile = file;
            bankReference.clear();
        }
        else
        {
            // Not found (or unreadable, or refused): the restored sound and its name stay, but it
            // belongs to no slot; the factory bank is shown, and the bar says the bank is missing.
            status = BankStatus::Missing;
            bank = factory;
            index.store (0, std::memory_order_relaxed);
            detached.store (true, std::memory_order_relaxed);
        }
    }
    if (! ok)
        modified.store (true, std::memory_order_relaxed);
    notifyHost();
    broadcaster.sendSynchronousChangeMessage();
}
} // namespace vs::presets
