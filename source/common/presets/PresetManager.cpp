#include "common/presets/PresetManager.h"

#include <algorithm>
#include <cmath>

namespace vs::presets
{
namespace
{
constexpr const char* kInfoType = "Preset";

juce::String errorFor (const juce::File& f, const juce::String& what) { return what + " (" + f.getFileName() + ")"; }
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
    jassert (static_cast<int> (factory.programs.size()) == kBankSize);

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
    ensureTimer();
}

PresetManager::~PresetManager()
{
    stopTimer();
    for (auto* p : params)
        p->removeListener (this);
}

void PresetManager::ensureTimer()
{
    if (! timerStarted && onMessageThread())
    {
        timerStarted = true;
        startTimer (kPollMs);
    }
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
juce::String PresetManager::bankName() const { const juce::ScopedLock sl (lock); return bank.name; }
bool PresetManager::isFactoryBank() const { const juce::ScopedLock sl (lock); return bankIsFactory; }

juce::String PresetManager::programName (int i) const
{
    if (i < 0 || i >= kBankSize)
        return {};
    const juce::ScopedLock sl (lock);
    return i == index.load (std::memory_order_relaxed) ? current.name : bank.programs[static_cast<size_t> (i)].meta.name;
}

juce::String PresetManager::positionName (int i)
{
    const int group = std::clamp (i, 0, kBankSize - 1) / 8, slot = std::clamp (i, 0, kBankSize - 1) % 8;
    return juce::String::charToString (static_cast<juce::juce_wchar> ('A' + group)) + juce::String (slot + 1);
}

void PresetManager::setCurrent (int i, const Meta& meta, bool isModified)
{
    {
        const juce::ScopedLock sl (lock);
        index.store (i, std::memory_order_relaxed);
        current = meta;
    }
    modified.store (isModified, std::memory_order_relaxed);
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
    if (applying.load (std::memory_order_relaxed) || schema.isExcluded (i))
        return;
    if (previous < newValue || previous > newValue)
        modified.store (true, std::memory_order_relaxed);
}

void PresetManager::applyIndices (const std::vector<int>& indices, const std::vector<float>& values)
{
    jassert (onMessageThread());
    // 1. hold the audio thread's parameter values  2. have it release every note (it acknowledges
    // at the start of its next block, or we give up when no audio runs)  3. write all the
    // parameters, then let the audio thread read them at once. See ApplyGate.h.
    applying.store (true, std::memory_order_release);
    gate.beginHold();
    lastAcknowledged = gate.requestReleaseAndWait();
    for (const int i : indices)
    {
        auto* p = params[static_cast<size_t> (i)];
        const float target = values[static_cast<size_t> (i)];
        const auto& s = schema.spec (i);
        const float now = raw[static_cast<size_t> (i)]->load (std::memory_order_relaxed);
        if (std::abs (now - target) <= 1.0e-6f * (s.hi - s.lo))
            continue; // already there: no needless host event
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (target));
        p->endChangeGesture();
    }
    resyncLastValues();
    gate.endHold();
    applying.store (false, std::memory_order_release);
}

void PresetManager::withParametersHeld (const std::function<void()>& restore)
{
    applying.store (true, std::memory_order_release);
    gate.beginHold();
    restore();
    resyncLastValues();
    gate.endHold();
    applying.store (false, std::memory_order_release);
}

void PresetManager::notifyHost()
{
    apvts.processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
}

void PresetManager::applyProgram (const Program& program, int slot, bool markModified, bool tellHost)
{
    jassert (static_cast<int> (program.values.size()) == schema.numParams());
    std::vector<int> indices = schema.globalParams();
    for (int t = 0; t < schema.numTones(); ++t)
        for (int k = 0; k < schema.toneSize(); ++k)
            indices.push_back (schema.toneParam (t, k));
    applyIndices (indices, program.values);
    setCurrent (std::clamp (slot, 0, kBankSize - 1), program.meta, markModified);
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
    applyIndices (indices, values);
    modified.store (true, std::memory_order_relaxed); // the sound no longer is the loaded program
    broadcaster.sendSynchronousChangeMessage();
}

bool PresetManager::selectProgram (int i, bool tellHost)
{
    if (i < 0 || i >= kBankSize)
        return false;
    Program p;
    {
        const juce::ScopedLock sl (lock);
        p = bank.programs[static_cast<size_t> (i)];
    }
    applyProgram (p, i, false, tellHost);
    return true;
}

void PresetManager::initialise()
{
    auto init = makeInitProgram (schema);
    applyProgram (init, currentIndex(), true);
}

void PresetManager::requestProgram (int i)
{
    if (i < 0 || i >= kBankSize)
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
    const int queued = queuedProgram.exchange (-1, std::memory_order_acq_rel);
    if (queued >= 0)
        selectProgram (queued);
    const int midi = gate.takeMidiProgram();
    if (midi >= 0 && midi < kBankSize) // 64..127 is outside the bank: ignored
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

void PresetManager::setBank (Bank b, const juce::String& userFilePathOrEmpty)
{
    {
        const juce::ScopedLock sl (lock);
        bankIsFactory = userFilePathOrEmpty.isEmpty();
        bankFile = bankIsFactory ? juce::File() : juce::File (userFilePathOrEmpty);
        if (bankIsFactory)
            bank = factory;
        else
        {
            bank = std::move (b);
            bank.programs.resize (static_cast<size_t> (kBankSize), makeInitProgram (schema));
        }
    }
    notifyHost();
    broadcaster.sendSynchronousChangeMessage();
}

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------
namespace
{
juce::String readFile (const juce::File& file, juce::MemoryBlock& data)
{
    if (! file.existsAsFile())
        return errorFor (file, "The file does not exist");
    if (static_cast<juce::uint64> (file.getSize()) > kMaxFileBytes)
        return errorFor (file, "The file is too large to be a preset");
    if (! file.loadFileAsData (data))
        return errorFor (file, "The file could not be read");
    return {};
}
} // namespace

juce::String PresetManager::writeFile (const juce::File& file, const std::string& text) const
{
    if (! file.getParentDirectory().createDirectory())
        return errorFor (file, "The folder could not be created");
    // Written next to the target and moved over it, so a failed write never damages an existing file.
    const auto tmp = file.getSiblingFile (file.getFileName() + ".tmp");
    if (! tmp.replaceWithData (text.data(), text.size()))
    {
        tmp.deleteFile();
        return errorFor (file, "The file could not be written");
    }
    if (! tmp.moveFileTo (file))
    {
        tmp.deleteFile();
        return errorFor (file, "The file could not be replaced");
    }
    return {};
}

juce::String PresetManager::loadProgramFile (const juce::File& file)
{
    juce::MemoryBlock data;
    if (auto e = readFile (file, data); e.isNotEmpty())
        return e;
    const auto parsed = parseProgram (schema, static_cast<const char*> (data.getData()), data.getSize());
    if (! parsed.ok)
        return errorFor (file, parsed.error);
    applyProgram (parsed.value, currentIndex(), false);
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
    juce::MemoryBlock data;
    if (auto e = readFile (file, data); e.isNotEmpty())
        return e;
    const auto parsed = parseTone (schema, static_cast<const char*> (data.getData()), data.getSize());
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
    juce::MemoryBlock data;
    if (auto e = readFile (file, data); e.isNotEmpty())
        return e;
    const auto parsed = parseBank (schema, static_cast<const char*> (data.getData()), data.getSize());
    if (! parsed.ok)
        return errorFor (file, parsed.error);
    setBank (parsed.value, file.getFullPathName());
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
        bank = b;
        bankIsFactory = false;
        bankFile = file;
    }
    notifyHost();
    broadcaster.sendSynchronousChangeMessage();
    return {};
}

juce::String PresetManager::storeInBank (int i)
{
    if (i < 0 || i >= kBankSize)
        return "No such slot.";
    auto p = captureProgram();
    {
        const juce::ScopedLock sl (lock);
        if (bankIsFactory)
            return "The factory bank is read-only. Save the bank to a file first; then the copy can be edited.";
        bank.programs[static_cast<size_t> (i)] = p;
    }
    setCurrent (i, p.meta, false);
    notifyHost();
    broadcaster.sendSynchronousChangeMessage();
    return {};
}

void PresetManager::renameProgram (int i, const juce::String& name)
{
    if (i < 0 || i >= kBankSize)
        return;
    const auto n = sanitiseName (name);
    {
        const juce::ScopedLock sl (lock);
        if (i == index.load (std::memory_order_relaxed))
            current.name = n;
        if (! bankIsFactory)
            bank.programs[static_cast<size_t> (i)].meta.name = n;
    }
    notifyHost();
    if (onMessageThread())
        broadcaster.sendSynchronousChangeMessage();
}

// ---------------------------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------------------------
juce::ValueTree PresetManager::createSessionInfo() const
{
    juce::ValueTree t (kInfoType);
    const juce::ScopedLock sl (lock);
    t.setProperty ("bankName", bank.name, nullptr);
    t.setProperty ("bankKind", bankIsFactory ? "factory" : "user", nullptr);
    t.setProperty ("bankPath", bankIsFactory ? juce::String() : bankFile.getFullPathName(), nullptr);
    t.setProperty ("program", index.load (std::memory_order_relaxed), nullptr);
    t.setProperty ("name", current.name, nullptr);
    t.setProperty ("category", current.category, nullptr);
    t.setProperty ("author", current.author, nullptr);
    t.setProperty ("modified", modified.load (std::memory_order_relaxed), nullptr);
    return t;
}

void PresetManager::restoreSessionInfo (const juce::ValueTree& info)
{
    // The sound itself came with the parameters; this only restores what the preset bar and the
    // host program list show. Whatever the bank's fate, the sound is exactly the saved one.
    Meta meta;
    int slot = 0;
    bool wasModified = true;
    Bank restoredBank = factory;
    juce::String restoredPath;

    if (info.isValid() && info.hasType (kInfoType))
    {
        slot = std::clamp (static_cast<int> (info.getProperty ("program", 0)), 0, kBankSize - 1);
        meta.name = sanitiseName (info.getProperty ("name", "Untitled").toString());
        meta.category = sanitiseText (info.getProperty ("category", "").toString(), kMaxCategoryChars);
        meta.author = sanitiseText (info.getProperty ("author", "").toString(), kMaxAuthorChars);
        wasModified = static_cast<bool> (info.getProperty ("modified", true));
        if (info.getProperty ("bankKind", "factory").toString() == "user")
        {
            const juce::File f (info.getProperty ("bankPath", "").toString());
            juce::MemoryBlock data;
            if (f != juce::File() && readFile (f, data).isEmpty())
            {
                auto parsed = parseBank (schema, static_cast<const char*> (data.getData()), data.getSize());
                if (parsed.ok)
                {
                    restoredBank = std::move (parsed.value);
                    restoredPath = f.getFullPathName();
                }
            }
        }
    }
    else
    {
        meta.name = "Untitled"; // a session saved before presets existed: the sound is not any known program
    }

    {
        const juce::ScopedLock sl (lock);
        bank = std::move (restoredBank);
        bankIsFactory = restoredPath.isEmpty();
        bankFile = bankIsFactory ? juce::File() : juce::File (restoredPath);
        index.store (slot, std::memory_order_relaxed);
        current = meta;
    }
    modified.store (wasModified, std::memory_order_relaxed);
    queuedProgram.store (-1, std::memory_order_relaxed);
    notifyHost();
    if (onMessageThread())
        broadcaster.sendSynchronousChangeMessage();
}
} // namespace vs::presets
