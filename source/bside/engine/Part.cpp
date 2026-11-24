#include "bside/engine/Part.h"

#include "bside/chip/OutputStage.h"
#include "bside/engine/TickClock.h"
#include "bside/params/PartMappings.h"
#include "common/engine/Oversampler.h"
#include "common/params/Mappings.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bs
{
namespace chip = vs::bside::chip;
namespace engine = vs::bside::engine;
namespace program = vs::bside::program;

namespace
{
/** oscN.wave (kOscWaveLabels' order) -> control bits 4 to 7 (8.21.1). */
constexpr std::array<std::uint8_t, 9> kWaveBits { 0x00, 0x10, 0x20, 0x40, 0x80, 0x30, 0x50, 0x60, 0x70 };
/** filter.mode (kFilterModeLabels' order) -> the mode bits (1 low, 2 band, 4 high; register 0x18 bits 4 to 6). */
constexpr std::array<std::uint8_t, 8> kModeBits { 0, 1, 2, 4, 3, 6, 5, 7 };

chip::Model modelOf (int index) noexcept { return index == 0 ? chip::Model::A : chip::Model::B; }

/** The board filters' delay in internal samples (OutputStage::board: the 15.4 kHz low-pass's; the 1.2 Hz high-pass's
    phase lead is negligible there): the phase delay of a sine of 48 samples' period (2 kHz at 96 kHz), measured like
    the decimator's (coherent projection over whole periods after a settling time). ChipVoice::latencySamples does not count it; a stage that
    adds delay is in the reported latency (8.21.1). Prepare time only. */
double boardDelaySamples (double rate)
{
    chip::OutputStage st;
    st.prepare (chip::Model::B, rate, false);
    constexpr int period = 48;
    const double w = 2.0 * 3.14159265358979323846 / period;
    const int settle = 64 * period, measure = 64 * period;
    double s = 0.0, c = 0.0;
    for (int n = 0; n < settle + measure; ++n)
    {
        const double y = st.board (std::sin (w * n), 1.0);
        if (n >= settle)
        {
            s += y * std::sin (w * n);
            c += y * std::cos (w * n);
        }
    }
    return std::atan2 (-c, s) / w; // y = A sin (w n - phi): s ~ cos (phi), c ~ -sin (phi)
}

int clampIndex (int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

double finiteOr (double v, double fallback) noexcept { return std::isfinite (v) ? v : fallback; }

/** Unison voice k of n starts with its accumulators at k^2 / (2 n) of a cycle (3.2's phase pattern; no random base: a
    part's output is deterministic, 8.6.2). */
std::uint32_t unisonAccumulator (int k, int n) noexcept
{
    const double kk = static_cast<double> (k);
    const double x = kk * kk / (2.0 * static_cast<double> (n));
    const double f = x - std::floor (x);
    return static_cast<std::uint32_t> (std::llround (f * 16777216.0)) & 0xffffffu;
}

const program::Program& emptyTable() noexcept
{
    static const program::Program empty = program::emptyProgram(); // built before the first block (Part's constructor)
    return empty;
}
} // namespace

void Part::Smooth::setGoal (double v, int steps) noexcept
{
    if (! (v < goal || v > goal))
        return;
    goal = v;
    left = steps > 0 ? steps : 1;
    step = (goal - value) / static_cast<double> (left);
}

Part::Part()
    : protoChain (std::make_unique<std::array<chip::ChipVoice, 2>>()),
      voices (std::make_unique<Voices>())
{
    (void) emptyTable();
    for (int i = 0; i < kVoices; ++i)
        (*voices)[static_cast<std::size_t> (i)].sink.setTap (&Part::sharedTap, this, i);
}

Part::~Part() = default;

std::size_t Part::memoryBytes() noexcept
{
    return sizeof (Part) + sizeof (Voices) + sizeof (std::array<chip::ChipVoice, 2>);
}

// ---- prepare, structure ----------------------------------------------------------------------------------------

Part::Structure Part::structureOf (const PartParams& p, int clock) const noexcept
{
    Structure s;
    s.model = clampIndex (p.chipModel, 0, 1);
    s.shared = p.filterTopology == 1;
    s.clock = clock != 0 ? 1 : 0;
    s.authentic = p.chipResolution == 1;
    s.volumeDc = p.chipVolumeDc == 1;
    return s;
}

void Part::prepare (double sampleRate, int maxBlockSize, int oversamplingFactor, const PartParams& params, int chipClock)
{
    hostRate = sampleRate > 1000.0 && std::isfinite (sampleRate) ? sampleRate : 48000.0;
    maxBlock = std::max (1, maxBlockSize);
    osFactor = oversamplingFactor >= 4 ? 4 : (oversamplingFactor >= 2 ? 2 : 1);
    internalRate = hostRate * static_cast<double> (osFactor);
    controlInterval = std::max (1, static_cast<int> (std::lround (internalRate / 1000.0)));
    fadeSamples = std::max (1, static_cast<int> (std::lround (kFadeSeconds * internalRate)));
    rampSamples = std::max (1, static_cast<int> (std::lround (kSmoothSeconds * internalRate)));
    smoothSteps = std::max (1, static_cast<int> (std::lround (kSmoothSeconds * internalRate / static_cast<double> (controlInterval))));

    target = applied = params;
    clockTarget = chipClock != 0 ? 1 : 0;
    current = wanted = structureOf (params, clockTarget);
    switchPending = false;

    // Everything a structural switch can select, built here (8.21.1): both models' chains and filters at this rate.
    const float variation = map::percentToFraction (params.chipVariation);
    for (int m = 0; m < 2; ++m)
    {
        chip::Config c;
        c.model = modelOf (m);
        c.clockHz = kClockHz[current.clock];
        c.internalRate = internalRate;
        c.variation = variation;
        c.volumeDc = current.volumeDc;
        c.authentic = current.authentic;
        (*protoChain)[static_cast<std::size_t> (m)].prepare (c);
        protoFilter[static_cast<std::size_t> (m)].prepare (c.model, internalRate, variation);
        chainLatency[static_cast<std::size_t> (m)] = (*protoChain)[static_cast<std::size_t> (m)].latencySamples();
    }
    maxChainLatency = std::max (chainLatency[0], chainLatency[1]);
    // what the pad needs: the difference of the two chains (the longer one is not delayed), with margin
    jassert (2 * (maxChainLatency - std::min (chainLatency[0], chainLatency[1])) <= kPadRing);

    // The decimator (Vintage's, unchanged) and the one latency of both models (8.21.1).
    oversampler.reset();
    double decimatorDelay = 0.0;
    if (osFactor > 1)
    {
        oversampler = vs::makeVoiceOversampler (osFactor);
        oversampler->initProcessing (static_cast<size_t> (maxBlock));
        decimatorDelay = vs::measureDecimatorDelay (osFactor);
    }
    exactLatency = static_cast<double> (maxChainLatency) / static_cast<double> (osFactor) + decimatorDelay
                   + boardDelaySamples (internalRate) / static_cast<double> (osFactor);
    latencySamples = static_cast<int> (std::lround (exactLatency));
    silence.setSize (1, maxBlock);
    silence.clear();

    configureVoices();
    reset();
}

void Part::configureVoices() noexcept
{
    const auto m = static_cast<std::size_t> (current.model);
    const chip::Config& pc = (*protoChain)[m].config();
    const std::uint32_t clockHz = kClockHz[current.clock];
    const bool sameConfig = pc.clockHz == clockHz && pc.authentic == current.authentic && pc.volumeDc == current.volumeDc;
    const float variation = map::percentToFraction (static_cast<float> (sm[sVariation].value));
    for (auto& v : *voices)
    {
        v.chain = (*protoChain)[m]; // a copy: no design work
        if (! sameConfig)
            v.chain.reconfigure (clockHz, current.authentic, current.volumeDc);
        v.filter = protoFilter[m];
        v.filter.setVariation (variation);
    }
    sharedFilter = protoFilter[m];
    sharedFilter.setVariation (variation);
    padDelay = maxChainLatency - chainLatency[m];
    for (auto& v : *voices)
    {
        v.chain.reset();
        v.filter.reset();
        v.chain.setFilter (current.shared ? nullptr : &v.filter);
        engine::EngineConfig ec;
        ec.clockHz = clockHz;
        ec.authentic = current.authentic;
        v.engine.prepare (ec);
        v.engine.setTickPeriod (period);
        v.sink.setAuthentic (current.authentic);
        v.sink.forgetInputs();
        v.sink.tapOn (false);
        v.chain.suspend(); // the first note starts settled
        v.active = v.held = v.idleSent = false;
        v.sinceNoteOff = v.sinceIdle = v.quiet = v.skipPending = 0;
        v.glide = 0.0;
        v.gain = v.gainTarget = v.gainStep = 0.0;
        v.gainLeft = 0;
        v.fade = 1.0;
        v.fadeStep = 0.0;
        v.fadeLeft = 0;
        v.fadeKind = NoteVoice::Fade::none;
        v.pending = false;
        v.pendingRelease = -1;
        v.releaseCountdown = -1;
        v.startDelay = 0;
        v.late = false;
        v.share = 1.0;
        v.shareUp = true;
        v.startedAt = 0;
    }
    refClock.prepare (clockHz, internalRate);
    refCycle = 0;
    pad.fill (0.0);
    padPos = 0;
    sharedHead = sharedCount = 0;
    mostRecent = -1;
    sharedCutoff = 0.0f;
    sharedLastChange = 0;
    sharedChanged = sharedMoving = false;
    sharedSettle = true;
}

std::uint32_t Part::tickPeriod() const noexcept
{
    engine::TickParams t;
    t.rate = static_cast<engine::TickRate> (clampIndex (applied.tickRate, 0, 2));
    t.clockHz = kClockHz[current.clock];
    t.mult = applied.tickMult;
    t.freeHz = static_cast<double> (applied.tickFreeHz);
    t.bpm = tempo;
    t.sync = map::kTickSyncPerBeat[static_cast<std::size_t> (clampIndex (applied.tickSync, 0, 3))];
    return engine::tickPeriodCycles (t);
}

engine::RestartPolicy Part::policy() const noexcept
{
    switch (clampIndex (applied.restartPolicy, 0, 2))
    {
        case 1: return engine::RestartPolicy::precondition;
        case 2: return engine::RestartPolicy::authentic;
        default: return engine::RestartPolicy::clean;
    }
}

double Part::noteGainFor (float velocity, const CtlState& c) const noexcept
{
    const double v = std::clamp (finiteOr (static_cast<double> (velocity), 1.0), 0.0, 1.0);
    return 1.0 - c.v[sVelVolume] * (1.0 - v);
}

Part::CtlState Part::liveState() const noexcept
{
    CtlState c;
    for (std::size_t k = 0; k < kNumSmoothedValues; ++k)
        c.v[k] = sm[k].value;
    c.vibratoPhase = vibratoPhase;
    return c;
}

void Part::applyBlockParams() noexcept
{
    if (std::memcmp (&applied, &target, sizeof (PartParams)) != 0)
        hostDirty = true;
    applied = target;
    const PartParams& p = applied;

    // structural switches (8.21.1): fade, then every voice into the new configuration
    const Structure s = structureOf (p, clockTarget);
    if (! (s == current) || switchPending)
    {
        wanted = s;
        if (! switchPending && ! (s == current))
        {
            if (isSounding())
                beginSwitch();
            else
            {
                current = wanted;
                configureVoices();
                ++switches;
            }
        }
    }

    // the allocator (a mode or unison-count change releases the sounding voices)
    allocator.setLegatoGlideOnly (p.portaMode == 1);
    allocator.setMode (static_cast<vs::AssignMode> (clampIndex (p.assignMode, 0, 2)),
                       map::kUnisonVoiceCounts[static_cast<std::size_t> (clampIndex (p.unisonVoices, 0, 3))],
                       [this] (const vs::VoiceAction& a) { execute (a); });

    // the continuous parameters' targets (smoothed over 5 ms at the control points)
    const std::array<double, kNumSmoothed - 2> goals {
        static_cast<double> (p.osc1Fine), static_cast<double> (p.osc2Fine), static_cast<double> (p.osc3Fine),
        static_cast<double> (p.osc1Pw), static_cast<double> (p.osc2Pw), static_cast<double> (p.osc3Pw),
        static_cast<double> (p.filterCutoff), static_cast<double> (p.filterKeyFollow), static_cast<double> (p.chipVariation),
        static_cast<double> (p.macroPwOffset), static_cast<double> (p.macroCutoffOffset), static_cast<double> (p.macroSweepDepth),
        static_cast<double> (p.velVolume), static_cast<double> (p.velCutoff), static_cast<double> (p.vibratoRate),
        static_cast<double> (p.modWheelVibrato), static_cast<double> (p.unisonDetune)
    };
    for (std::size_t k = 0; k < goals.size(); ++k)
        sm[k].setGoal (finiteOr (goals[k], 0.0), smoothSteps);

    // glide: Vintage's time mapping (99 % of the interval in 5 s x v^2), per control interval
    const double portaSec = static_cast<double> (vs::map::portaTimeSec (std::clamp (p.portaTime, 0.0f, 1.0f)));
    const double ctlRate = internalRate / static_cast<double> (controlInterval);
    glideFactor = portaSec > 0.0 ? std::exp (-4.605170185988091 / (portaSec * ctlRate)) : 0.0;

    // the tick period, to every engine (it applies from the tick after the one already scheduled, 8.16)
    period = tickPeriod();
    for (auto& v : *voices)
        v.engine.setTickPeriod (period);
}

void Part::beginSwitch() noexcept
{
    switchPending = true;
    switchLeft = fadeSamples;
}

void Part::finishSwitch() noexcept
{
    switchPending = false;
    current = wanted;
    // the voices that sounded become idle; a note that waits starts in the new configuration (late)
    std::array<bool, kVoices> waiting {};
    std::array<vs::VoiceAction, kVoices> actions {};
    std::array<std::int64_t, kVoices> since {}, released {};
    for (int i = 0; i < kVoices; ++i)
    {
        auto& v = (*voices)[static_cast<std::size_t> (i)];
        waiting[static_cast<std::size_t> (i)] = v.pending;
        actions[static_cast<std::size_t> (i)] = v.pendingAction;
        since[static_cast<std::size_t> (i)] = v.pendingAt;
        released[static_cast<std::size_t> (i)] = v.pendingRelease;
    }
    configureVoices();
    ++switches;
    for (int i = 0; i < kVoices; ++i)
        if (! waiting[static_cast<std::size_t> (i)] && allocator.isVoiceBusy (i))
            allocator.voiceIdle (i); // held keys do not retrigger
    const CtlState now = liveState();
    for (int i = 0; i < kVoices; ++i)
        if (waiting[static_cast<std::size_t> (i)])
        {
            auto& v = (*voices)[static_cast<std::size_t> (i)];
            v.pendingAction = actions[static_cast<std::size_t> (i)];
            v.pendingAt = since[static_cast<std::size_t> (i)];
            v.pendingRelease = released[static_cast<std::size_t> (i)];
            startWaiting (v, i, false, now, partSample); // the fade ended at this sample
        }
}

void Part::startWaiting (NoteVoice& v, int index, bool stolen, const CtlState& c, std::int64_t at) noexcept
{
    const vs::VoiceAction a = v.pendingAction;
    const std::int64_t waited = std::clamp<std::int64_t> (at - v.pendingAt, 0, 1 << 30);
    const std::int64_t released = v.pendingRelease;
    startNote (v, index, a, stolen, c); // clears the waiting state and the delay of the note before
    v.startDelay = static_cast<int> (waited);
    v.late = waited > 0;
    if (released == 0)
    {
        v.late = false; // note-on and note-off in one sample: released at once
        noteOffVoice (v);
    }
    else if (released > 0)
    {
        // released while it waited: as long after its start as the release came after its note-on
        v.late = false;
        v.releaseCountdown = static_cast<int> (std::min<std::int64_t> (released, 1 << 30));
    }
}

// ---- reset, resume, release ------------------------------------------------------------------------------------

void Part::reset() noexcept
{
    current = structureOf (target, clockTarget);
    wanted = current;
    applied = target;
    for (std::size_t k = 0; k < kNumSmoothed; ++k)
        sm[k].jump (0.0);
    const std::array<double, kNumSmoothed - 2> now {
        static_cast<double> (target.osc1Fine), static_cast<double> (target.osc2Fine), static_cast<double> (target.osc3Fine),
        static_cast<double> (target.osc1Pw), static_cast<double> (target.osc2Pw), static_cast<double> (target.osc3Pw),
        static_cast<double> (target.filterCutoff), static_cast<double> (target.filterKeyFollow), static_cast<double> (target.chipVariation),
        static_cast<double> (target.macroPwOffset), static_cast<double> (target.macroCutoffOffset), static_cast<double> (target.macroSweepDepth),
        static_cast<double> (target.velVolume), static_cast<double> (target.velCutoff), static_cast<double> (target.vibratoRate),
        static_cast<double> (target.modWheelVibrato), static_cast<double> (target.unisonDetune)
    };
    for (std::size_t k = 0; k < now.size(); ++k)
        sm[k].jump (finiteOr (now[k], 0.0));
    bendValue = wheelValue = 0.0f;
    vibratoPhase = 0.0;
    switchPending = false;
    switchLeft = 0;
    period = tickPeriod();
    configureVoices();
    allocator.reset();
    allocator.initMode (static_cast<vs::AssignMode> (clampIndex (target.assignMode, 0, 2)),
                        map::kUnisonVoiceCounts[static_cast<std::size_t> (clampIndex (target.unisonVoices, 0, 3))]);
    allocator.setLegatoGlideOnly (target.portaMode == 1);
    partSample = 0;
    controlDoneAt = -1;
    startCounter = 0;
    rounding = chip::render::OutputRounding {};
    if (oversampler != nullptr)
        oversampler->reset();
    numLeft = 0;
    hostDirty = true;
    silentRun = 0;
    quietChain = false;
}

void Part::resume() noexcept
{
    // Upper back from Whole mode, silent (the shell's rule): clean, as after prepare, keeping the controllers (bend,
    // wheel, pedal) and the allocator's voice ages. Its chips restart from power-on: how long it was not rendered
    // depends on the host's blocks, so its chips' time cannot carry over (8.4.2).
    current = structureOf (target, clockTarget);
    wanted = current;
    applied = target;
    const std::array<double, kNumSmoothed - 2> now {
        static_cast<double> (target.osc1Fine), static_cast<double> (target.osc2Fine), static_cast<double> (target.osc3Fine),
        static_cast<double> (target.osc1Pw), static_cast<double> (target.osc2Pw), static_cast<double> (target.osc3Pw),
        static_cast<double> (target.filterCutoff), static_cast<double> (target.filterKeyFollow), static_cast<double> (target.chipVariation),
        static_cast<double> (target.macroPwOffset), static_cast<double> (target.macroCutoffOffset), static_cast<double> (target.macroSweepDepth),
        static_cast<double> (target.velVolume), static_cast<double> (target.velCutoff), static_cast<double> (target.vibratoRate),
        static_cast<double> (target.modWheelVibrato), static_cast<double> (target.unisonDetune)
    };
    for (std::size_t k = 0; k < now.size(); ++k)
        sm[k].jump (finiteOr (now[k], 0.0));
    sm[sBend].jump (static_cast<double> (bendValue));
    sm[sWheel].jump (static_cast<double> (wheelValue));
    vibratoPhase = 0.0;
    switchPending = false;
    switchLeft = 0;
    period = tickPeriod();
    configureVoices();
    allocator.initMode (static_cast<vs::AssignMode> (clampIndex (target.assignMode, 0, 2)),
                        map::kUnisonVoiceCounts[static_cast<std::size_t> (clampIndex (target.unisonVoices, 0, 3))]);
    allocator.setLegatoGlideOnly (target.portaMode == 1);
    for (int i = 0; i < kVoices; ++i)
        if (allocator.isVoiceBusy (i))
            allocator.voiceIdle (i);
    partSample = 0;
    controlDoneAt = -1;
    rounding = chip::render::OutputRounding {};
    if (oversampler != nullptr)
        oversampler->reset();
    numLeft = 0;
    hostDirty = true;
    silentRun = 0;
    quietChain = false;
}

void Part::releaseAllNotes() noexcept
{
    allocator.releaseAllKeepPedal ([this] (const vs::VoiceAction& a) { execute (a); });
}

bool Part::isSounding() const noexcept
{
    if (switchPending)
        return true;
    for (const auto& v : *voices)
        if (v.active || v.pending)
            return true;
    return false;
}

int Part::numActiveVoices() const noexcept
{
    int n = 0;
    for (const auto& v : *voices)
        n += v.active ? 1 : 0;
    return n;
}

// ---- events and voices -----------------------------------------------------------------------------------------

void Part::handleEvent (const vs::NoteEvent& e) noexcept
{
    tapPartSample = partSample;
    const auto sink = [this] (const vs::VoiceAction& a) { execute (a); };
    switch (e.type)
    {
        case vs::EventType::NoteOn: allocator.noteOn (e.note, e.value, sink); break;
        case vs::EventType::NoteOff: allocator.noteOff (e.note, sink); break;
        case vs::EventType::PitchBend:
            bendValue = std::isfinite (e.value) ? std::clamp (e.value, -1.0f, 1.0f) : 0.0f;
            sm[sBend].setGoal (static_cast<double> (bendValue), smoothSteps);
            break;
        case vs::EventType::ModWheel:
            wheelValue = std::isfinite (e.value) ? std::clamp (e.value, 0.0f, 1.0f) : 0.0f;
            sm[sWheel].setGoal (static_cast<double> (wheelValue), smoothSteps);
            break;
        case vs::EventType::SustainPedal: allocator.setSustain (e.value >= 0.5f, sink); break;
        case vs::EventType::AllNotesOff: allocator.allNotesOff (sink); break;
        case vs::EventType::AllSoundOff:
            allocator.killAll();
            for (auto& v : *voices)
                killVoice (v);
            break;
        case vs::EventType::ResetControllers:
            bendValue = wheelValue = 0.0f;
            sm[sBend].setGoal (0.0, smoothSteps);
            sm[sWheel].setGoal (0.0, smoothSteps);
            allocator.setSustain (false, sink);
            break;
    }
}

void Part::execute (const vs::VoiceAction& a) noexcept
{
    const int index = clampIndex (a.voice, 0, kVoices - 1);
    auto& v = (*voices)[static_cast<std::size_t> (index)];
    switch (a.type)
    {
        case vs::VoiceAction::Type::Start:
        case vs::VoiceAction::Type::Steal:
            v.releaseCountdown = -1; // a new note cancels a delayed release of the old one
            v.pendingRelease = -1;
            if (switchPending || v.pending)
            {
                v.pending = true; // starts after the switch or the steal fade
                v.pendingAction = a;
                v.pendingAt = partSample;
            }
            else if (a.type == vs::VoiceAction::Type::Steal && v.active)
            {
                if (current.shared && carriedByOthers (v))
                    v.shareUp = false; // Shared: its share goes to the others as it fades (its new note joins from 0)
                v.fadeKind = NoteVoice::Fade::steal;
                v.fadeLeft = fadeSamples;
                v.fadeStep = v.fade / static_cast<double> (fadeSamples);
                v.pending = true;
                v.pendingAction = a;
                v.pendingAt = partSample;
            }
            else
                startNote (v, index, a, false);
            break;

        case vs::VoiceAction::Type::Retarget:
            if (v.pending)
            {
                v.pendingAction.note = a.note; // the waiting note is this one now: its delay counts from here
                v.pendingAt = partSample;
            }
            else if (v.active)
            {
                // the note that started late is over: the one retargeted to came on time, and so does its release
                v.late = false;
                v.startDelay = 0;
                const bool glide = glideFactor > 0.0 && a.glideFrom == vs::VoiceAction::kGlideFromCurrent;
                v.glide = glide ? static_cast<double> (v.note) + v.glide - static_cast<double> (a.note) : 0.0;
                v.note = a.note;
                if (v.engine.isRunning())
                {
                    v.engine.setHost (hostFor (v, liveState()));
                    v.engine.sync (v.chain.chip().cycle(), v.sink); // the legato pitch at the event's cycle
                }
            }
            break;

        case vs::VoiceAction::Type::Release:
            if (v.pending)
            {
                // released while it waits: remembered as a length, scheduled when the note starts (startWaiting)
                if (v.pendingRelease < 0)
                    v.pendingRelease = std::max<std::int64_t> (0, partSample - v.pendingAt);
            }
            else if (v.late && v.active)
            {
                // a late note is released as much later as it started (not a fixed fade length later)
                v.late = false;
                if (v.releaseCountdown < 0)
                    v.releaseCountdown = v.startDelay;
            }
            else if (v.active)
                noteOffVoice (v);
            break;
    }
}

void Part::catchUp (NoteVoice& v) noexcept
{
    if (v.skipPending > 0 && v.chain.isSuspended())
    {
        std::int64_t left = v.skipPending;
        while (left > 0)
        {
            const int n = static_cast<int> (std::min<std::int64_t> (left, 1 << 30));
            v.chain.skipSamples (n);
            left -= n;
        }
    }
    v.skipPending = 0;
}

void Part::startNote (NoteVoice& v, int index, const vs::VoiceAction& a, bool stolen, const CtlState& c) noexcept
{
    const bool fromIdle = ! v.active;
    if (fromIdle)
        catchUp (v); // the chip's state is what it would be had it run through the idle time (8.21.1)

    // glide (3.2: Poly from the previously played note, mono modes from the sounding pitch)
    double glideFrom = std::numeric_limits<double>::quiet_NaN();
    if (glideFactor > 0.0)
    {
        if (a.glideFrom == vs::VoiceAction::kGlideFromCurrent && v.active)
            glideFrom = static_cast<double> (v.note) + v.glide;
        else if (a.glideFrom >= 0)
            glideFrom = static_cast<double> (a.glideFrom);
    }
    v.glide = std::isfinite (glideFrom) ? glideFrom - static_cast<double> (a.note) : 0.0;
    v.note = clampIndex (a.note, 0, 127);
    v.velocity = a.velocity;

    // unison: the detune spread and the level 1 / sqrt (n)
    double level = 1.0;
    v.unisonPos = 0.0;
    const bool unison = allocator.getMode() == vs::AssignMode::Unison;
    if (unison)
    {
        const int n = allocator.getUnisonCount();
        level = 1.0 / std::sqrt (static_cast<double> (n));
        v.unisonPos = static_cast<double> (a.voice) / static_cast<double> (n - 1) - 0.5;
    }

    // the note gain: set on a start from idle, smoothed over 5 ms on a retrigger
    const double g = noteGainFor (a.velocity, c) * level;
    if (fromIdle || stolen)
    {
        v.gain = v.gainTarget = g;
        v.gainLeft = 0;
        v.gainStep = 0.0;
    }
    else
    {
        v.gain *= v.fade; // a retrigger inside a leave fade continues from what is heard
        v.gainTarget = g;
        v.gainLeft = rampSamples;
        v.gainStep = (g - v.gain) / static_cast<double> (rampSamples);
    }
    v.fade = 1.0;
    v.fadeLeft = 0;
    v.fadeStep = 0.0;
    v.fadeKind = NoteVoice::Fade::none;
    v.pending = false;
    v.pendingRelease = -1;
    v.startDelay = 0;
    v.late = false; // a delay belongs to one note: the callers that start a note late set it after this
    v.active = true;
    v.held = true;
    v.idleSent = false;
    v.sinceNoteOff = v.sinceIdle = v.quiet = 0;
    v.startOrder = ++startCounter;
    if (current.model == 0)
        v.filter.setVariation (map::percentToFraction (static_cast<float> (c.v[sVariation])));
    if (current.shared)
    {
        // its share of the shared filter's output (Part.h "SHARES"): from 0 when other chips carry it already (it joins
        // over the 3 ms fade), in full when none does; every idle chip hands its share on to it
        bool carried = false;
        for (auto& u : *voices)
            if (&u != &v && u.active)
            {
                carried = carried || (u.share > 0.0 && u.startedAt < partSample);
                if (u.idleSent)
                    u.shareUp = false;
            }
        if (fromIdle || stolen)
            v.share = carried ? 0.0 : 1.0;
        v.shareUp = true;
        v.startedAt = partSample;
        setMostRecent (index);
    }

    const program::Program& table = program != nullptr ? *program : emptyTable();
    v.restart = table.restart;
    v.engine.setTickPeriod (period);
    v.engine.setHost (hostFor (v, c));
    v.engine.noteOn (v.chain.chip().cycle(), table, policy(), stolen, v.sink);
    if (unison && fromIdle)
        v.chain.chip().setAccumulators (unisonAccumulator (a.voice, allocator.getUnisonCount()));
    if (current.shared)
        sharedFollowVoice (index);
}

void Part::noteOffVoice (NoteVoice& v) noexcept
{
    if (v.engine.isRunning())
        v.engine.noteOff (v.chain.chip().cycle(), v.sink);
    v.held = false;
    v.sinceNoteOff = 0;
}

void Part::killVoice (NoteVoice& v) noexcept
{
    // All Sound Off: silent now; the chip's gates are cleared and the idle writes made, so a later note starts clean
    if (v.active)
    {
        if (v.engine.isRunning())
        {
            v.engine.noteOff (v.chain.chip().cycle(), v.sink);
            v.engine.enterIdle (v.chain.chip().cycle(), v.restart, policy(), v.sink);
        }
        v.chain.suspend();
    }
    v.active = v.held = v.idleSent = false;
    v.pending = false;
    v.pendingRelease = -1;
    v.releaseCountdown = -1;
    v.startDelay = 0;
    v.late = false;
    v.fade = 1.0;
    v.fadeLeft = 0;
    v.fadeKind = NoteVoice::Fade::none;
}

void Part::leaveSum (NoteVoice& v) noexcept
{
    v.chain.suspend();
    v.active = false;
    v.fade = 1.0;
    v.fadeLeft = 0;
    v.fadeKind = NoteVoice::Fade::none;
    v.skipPending = 0;
}

engine::HostInputs Part::hostFor (const NoteVoice& v, const CtlState& c) const noexcept
{
    const PartParams& p = applied;
    engine::HostInputs h;
    const double bend = c.v[static_cast<std::size_t> (sBend)] * static_cast<double> (clampIndex (p.bendRange, 0, 12));
    const double depthCents = c.v[static_cast<std::size_t> (sWheel)] * static_cast<double> (map::wheelVibratoCents (static_cast<float> (c.v[static_cast<std::size_t> (sWheelDepth)])));
    const double vibrato = depthCents * 0.01 * std::sin (6.283185307179586 * c.vibratoPhase);
    const double detune = v.unisonPos * static_cast<double> (map::unisonDetuneCents (static_cast<float> (c.v[static_cast<std::size_t> (sUnisonDetune)]))) * 0.01;
    const double base = static_cast<double> (v.note) + v.glide + bend + vibrato + detune + testDetune;

    const std::array<int, 3> wave { p.osc1Wave, p.osc2Wave, p.osc3Wave };
    const std::array<int, 3> coarse { p.osc1Coarse, p.osc2Coarse, p.osc3Coarse };
    const std::array<int, 3> sync { p.osc1Sync, p.osc2Sync, p.osc3Sync };
    const std::array<int, 3> ring { p.osc1Ring, p.osc2Ring, p.osc3Ring };
    const std::array<int, 3> attack { p.osc1Attack, p.osc2Attack, p.osc3Attack };
    const std::array<int, 3> decay { p.osc1Decay, p.osc2Decay, p.osc3Decay };
    const std::array<int, 3> sustain { p.osc1Sustain, p.osc2Sustain, p.osc3Sustain };
    const std::array<int, 3> release { p.osc1Release, p.osc2Release, p.osc3Release };
    const std::array<int, 3> filt { p.osc1Filter, p.osc2Filter, p.osc3Filter };
    std::uint8_t routing = 0;
    for (std::size_t n = 0; n < 3; ++n)
    {
        auto& o = h.osc[n];
        const int w = clampIndex (wave[n], 0, 8);
        o.enabled = w != 0;
        o.waveBits = kWaveBits[static_cast<std::size_t> (w)];
        o.sync = sync[n] == 1;
        o.ring = ring[n] == 1;
        o.semitones = base + static_cast<double> (clampIndex (coarse[n], -24, 24)) + map::fineSemitones (static_cast<float> (c.v[static_cast<std::size_t> (sFine1 + n)]));
        o.pulseWidth = map::pulseWidthRegister (static_cast<float> (c.v[static_cast<std::size_t> (sPw1 + n)]));
        o.attackDecay = static_cast<std::uint8_t> ((clampIndex (attack[n], 0, 15) << 4) | clampIndex (decay[n], 0, 15));
        o.sustainRelease = static_cast<std::uint8_t> ((clampIndex (sustain[n], 0, 15) << 4) | clampIndex (release[n], 0, 15));
        if (filt[n] == 1)
            routing = static_cast<std::uint8_t> (routing | (1u << n));
    }
    // cutoff + key follow (16 register units per semitone at 100 %, around note 60; review) - velocity, then the
    // macro offset (the engine adds it after the lane value) and the clamp
    const double velocity = std::clamp (finiteOr (static_cast<double> (v.velocity), 1.0), 0.0, 1.0);
    const double cutoff = c.v[static_cast<std::size_t> (sCutoff)] + static_cast<double> (map::percentToFraction (static_cast<float> (c.v[static_cast<std::size_t> (sKeyFollow)]))) * 16.0 * (static_cast<double> (v.note) - 60.0)
                          - c.v[static_cast<std::size_t> (sVelCutoff)] * (1.0 - velocity) * 1024.0;
    h.filter.cutoff = static_cast<float> (std::clamp (cutoff, 0.0, 2047.0));
    h.filter.resonance = static_cast<std::uint8_t> (clampIndex (p.filterResonance, 0, 15));
    h.filter.routing = routing;
    h.filter.mode = kModeBits[static_cast<std::size_t> (clampIndex (p.filterMode, 0, 7))];
    h.filter.osc3Off = p.osc3Mute == 1;
    h.filter.volume = static_cast<std::uint8_t> (clampIndex (p.chipVolume, 0, 15));
    h.macro.pulseWidthOffset = map::pwOffsetRegister (static_cast<float> (c.v[static_cast<std::size_t> (sPwOffset)]));
    h.macro.cutoffOffset = map::cutoffOffsetRegister (static_cast<float> (c.v[static_cast<std::size_t> (sCutoffOffset)]));
    h.macro.sweepDepth = map::sweepDepthFactor (static_cast<float> (c.v[static_cast<std::size_t> (sSweep)]));
    return h;
}

// ---- rendering -------------------------------------------------------------------------------------------------

void Part::render (float* out, int numSamples, std::span<const vs::NoteEvent> events) noexcept
{
    jassert (numSamples <= maxBlock);
    numSamples = std::min (numSamples, maxBlock);
    if (numSamples <= 0)
    {
        for (const auto& e : events)
            handleEvent (e); // a zero-length render applies the events only
        return;
    }

    applyBlockParams();

    // Quiet: nothing sounds and the chain has been silent long enough; a block without a note-on renders nothing
    // (time, control points and the idle chips' time move on). The chain after the sum is reset once on the way in, so
    // it holds exactly what a silent chain holds; what it drops is the decimator's tail after kQuietAfterSeconds of
    // silence.
    const auto quietAfter = static_cast<std::int64_t> (kQuietAfterSeconds * internalRate);
    bool noteOn = false;
    for (const auto& e : events)
        noteOn = noteOn || e.type == vs::EventType::NoteOn;
    if (! noteOn && ! isSounding() && silentRun >= quietAfter)
    {
        if (! quietChain)
        {
            if (oversampler != nullptr)
                oversampler->reset();
            pad.fill (0.0);
            rounding = chip::render::OutputRounding {};
            quietChain = true;
        }
        int at0 = 0;
        for (const auto& e : events)
        {
            const int at = std::clamp (e.offset, at0, numSamples);
            if (at > at0)
            {
                renderQuiet ((at - at0) * osFactor);
                at0 = at;
            }
            handleEvent (e);
        }
        if (at0 < numSamples)
            renderQuiet ((numSamples - at0) * osFactor);
        for (auto& v : *voices)
            if (! v.active)
                catchUp (v);
        std::fill (out, out + numSamples, 0.0f);
        return;
    }
    quietChain = false;

    // The decimator needs a matching upsampling pass: it is fed silence and its oversampled block is overwritten.
    float* os = out;
    if (oversampler != nullptr)
    {
        auto silentBlock = juce::dsp::AudioBlock<float> (silence).getSubBlock (0, static_cast<size_t> (numSamples));
        auto osBlock = oversampler->processSamplesUp (silentBlock);
        os = osBlock.getChannelPointer (0);
    }

    int pos = 0;
    for (const auto& e : events)
    {
        const int at = std::clamp (e.offset, pos, numSamples);
        if (at > pos)
        {
            renderInternal (os + pos * osFactor, (at - pos) * osFactor);
            pos = at;
        }
        handleEvent (e);
    }
    if (pos < numSamples)
        renderInternal (os + pos * osFactor, (numSamples - pos) * osFactor);

    // an idle voice's chip time catches up once per block (bounded; 8.21.1)
    for (auto& v : *voices)
        if (! v.active)
            catchUp (v);

    if (oversampler != nullptr)
    {
        float* channels[] { out };
        juce::dsp::AudioBlock<float> outBlock (channels, 1, static_cast<size_t> (numSamples));
        oversampler->processSamplesDown (outBlock);
    }
}

int Part::segmentLimit (int len) const noexcept
{
    if (switchPending)
        len = std::min (len, std::max (1, switchLeft));
    for (const auto& v : *voices)
    {
        if (v.fadeLeft > 0)
            len = std::min (len, v.fadeLeft);
        if (v.releaseCountdown > 0)
            len = std::min (len, v.releaseCountdown);
    }
    return std::max (1, len);
}

void Part::renderInternal (float* dst, int count) noexcept
{
    int done = 0;
    while (done < count)
    {
        if (! current.shared)
        {
            // Per Note: spans, each voice on its own over the whole span
            int len = std::min (count - done, std::min (kSpan, controlInterval * (kMaxControls - 2)));
            if (switchPending)
                len = std::min (len, std::max (1, switchLeft));
            renderSpanPerNote (dst + done, len);
            done += len;
            continue;
        }
        // Shared: lockstep segments, split at every control point, fade end and delayed release
        if (partSample % controlInterval == 0 && controlDoneAt != partSample)
        {
            controlDoneAt = partSample;
            controlPoint();
        }
        int len = std::min (count - done, kSegment);
        len = std::min (len, controlInterval - static_cast<int> (partSample % controlInterval));
        len = segmentLimit (len);
        renderShared (len);
        postProcess (dst + done, len);
        partSample += len;
        done += len;
        afterSegment (len);
    }
}

void Part::postProcess (float* dst, int len) noexcept
{
    // the latency pad, the switch fade, the gain, the one conversion to float (8.20.1)
    for (int i = 0; i < len; ++i)
    {
        const double s = sum[static_cast<std::size_t> (i)];
        pad[static_cast<std::size_t> (padPos)] = s;
        double delayed = pad[static_cast<std::size_t> ((padPos - padDelay + kPadRing) & (kPadRing - 1))];
        padPos = (padPos + 1) & (kPadRing - 1);
        if (switchPending)
        {
            // the switch's fade AFTER the pad (8.21.1 after the review): it reaches the output in full, and what the
            // pad still holds when the fade ends (the old configuration's last samples) is behind a gain of 0
            delayed *= static_cast<double> (switchLeft) / static_cast<double> (fadeSamples);
            switchLeft = std::max (0, switchLeft - 1);
        }
        dst[i] = rounding.next (delayed * kChipGain);
        silentRun = s < 0.0 || s > 0.0 ? 0 : silentRun + 1;
    }
}

void Part::renderQuiet (int count) noexcept
{
    int done = 0;
    while (done < count)
    {
        if (partSample % controlInterval == 0 && controlDoneAt != partSample)
        {
            controlDoneAt = partSample;
            controlPoint();
        }
        const int len = std::min (count - done, controlInterval - static_cast<int> (partSample % controlInterval));
        for (auto& v : *voices)
            v.skipPending += len;
        refCycle += refClock.nextSamples (static_cast<std::uint64_t> (len));
        partSample += len;
        silentRun += len;
        done += len;
    }
}

void Part::renderVoicePerNote (NoteVoice& v, double* buf, int len) noexcept
{
    // render up to the next tick (the sample containing it stays in progress), run it, go on (8.16); a tick period is
    // at least 256 cycles, so a span holds a bounded number of ticks
    int pos = 0;
    for (int guard = 0; guard < 4 * kSpan + 8; ++guard)
    {
        const std::uint64_t t = v.engine.nextTickCycle();
        pos += v.chain.renderUntil (buf + pos, len - pos, t);
        if (v.engine.isRunning() && v.chain.chip().cycle() >= t)
        {
            v.engine.runTick (v.sink);
            continue;
        }
        if (pos >= len)
            break;
    }
}

void Part::mixVoice (NoteVoice& v, const double* buf, double* dst, int len) noexcept
{
    // the note gain (its 5 ms ramp on a retrigger) and the 3 ms fade, per sample; the quiet count of an idle voice
    // at the part's output
    if (v.gainLeft == 0 && v.fadeLeft == 0 && ! v.idleSent)
    {
        const double g = v.gain * v.fade;
        for (int i = 0; i < len; ++i)
            dst[i] += buf[i] * g;
    }
    else
        for (int i = 0; i < len; ++i)
        {
            const double g = v.gain;
            if (v.gainLeft > 0)
                v.gain = --v.gainLeft == 0 ? v.gainTarget : v.gain + v.gainStep;
            const double f = v.fade;
            if (v.fadeLeft > 0)
                v.fade = --v.fadeLeft == 0 ? 0.0 : std::max (0.0, v.fade - v.fadeStep);
            const double y = buf[i] * g * f;
            dst[i] += y;
            if (v.idleSent)
                v.quiet = std::abs (y * kChipGain) < kQuietLevel ? v.quiet + 1 : 0;
        }
    if (! v.held)
        v.sinceNoteOff += len;
    if (v.idleSent)
        v.sinceIdle += len;
}

void Part::renderSpanPerNote (float* dst, int count) noexcept
{
    // 1. the part's control points in the span, in order (the smoothed parameters, the vibrato); each voice then reads
    //    the state in force at its position
    const CtlState start = liveState();
    int nCtl = 0;
    for (int off = 0; off < count;)
    {
        const std::int64_t s = partSample + off;
        if (s % controlInterval == 0 && controlDoneAt != s && nCtl < kMaxControls)
        {
            controlDoneAt = s;
            ctlPos[static_cast<std::size_t> (nCtl)] = off;
            ctlStates[static_cast<std::size_t> (nCtl)] = partControl();
            ++nCtl;
        }
        off += controlInterval - static_cast<int> (s % controlInterval);
    }

    // 2. every voice over the whole span
    std::fill (sum.begin(), sum.begin() + count, 0.0);
    numLeft = 0;
    for (int i = 0; i < kVoices; ++i)
    {
        auto& v = (*voices)[static_cast<std::size_t> (i)];
        if (v.active)
            renderVoiceSpan (v, i, count, nCtl, start);
        else
            v.skipPending += count;
    }
    refCycle += refClock.nextSamples (static_cast<std::uint64_t> (count));

    // 3. the voices that left, to the allocator in the order they left (the sample, then the index)
    for (int a = 1; a < numLeft; ++a)
        for (int b = a; b > 0; --b)
        {
            const auto x = static_cast<std::size_t> (b - 1), y = static_cast<std::size_t> (b);
            if (leftAt[x] < leftAt[y] || (leftAt[x] == leftAt[y] && leftVoices[x] < leftVoices[y]))
                break;
            std::swap (leftAt[x], leftAt[y]);
            std::swap (leftVoices[x], leftVoices[y]);
        }
    for (int k = 0; k < numLeft; ++k)
        allocator.voiceIdle (leftVoices[static_cast<std::size_t> (k)]);
    numLeft = 0;

    postProcess (dst, count);
    partSample += count;
    tapPartSample = partSample;
    if (switchPending && switchLeft <= 0)
        finishSwitch();
}

void Part::renderVoiceSpan (NoteVoice& v, int index, int count, int nCtl, const CtlState& start) noexcept
{
    int pos = 0, k = 0;
    while (pos < count)
    {
        const CtlState& now = k > 0 ? ctlStates[static_cast<std::size_t> (k - 1)] : start;
        if (k < nCtl && ctlPos[static_cast<std::size_t> (k)] == pos)
        {
            const bool left = voiceControl (v, ctlStates[static_cast<std::size_t> (k)]);
            ++k;
            if (left)
            {
                leftVoices[static_cast<std::size_t> (numLeft)] = index;
                leftAt[static_cast<std::size_t> (numLeft)] = pos;
                ++numLeft;
                v.skipPending += count - pos;
                return;
            }
            continue;
        }
        int sub = count - pos;
        if (k < nCtl)
            sub = std::min (sub, ctlPos[static_cast<std::size_t> (k)] - pos);
        if (v.fadeLeft > 0)
            sub = std::min (sub, v.fadeLeft);
        if (v.releaseCountdown > 0)
            sub = std::min (sub, v.releaseCountdown);
        std::fill (scratch.begin(), scratch.begin() + sub, 0.0);
        renderVoicePerNote (v, scratch.data(), sub);
        mixVoice (v, scratch.data(), sum.data() + pos, sub);
        pos += sub;
        if (v.releaseCountdown > 0)
            v.releaseCountdown -= sub;

        // a fade that ended here: a stolen voice starts its note (settled), a voice that did not fall silent leaves
        if (v.fadeKind != NoteVoice::Fade::none && v.fadeLeft == 0)
        {
            if (v.fadeKind == NoteVoice::Fade::steal)
            {
                v.chain.suspend();
                v.active = false;
                v.skipPending = 0;
                startWaiting (v, index, true, now, partSample + pos);
            }
            else
            {
                leaveSum (v);
                leftVoices[static_cast<std::size_t> (numLeft)] = index;
                leftAt[static_cast<std::size_t> (numLeft)] = pos;
                ++numLeft;
                v.skipPending += count - pos;
                return;
            }
        }
        // a delayed release of a late note
        if (v.releaseCountdown == 0)
        {
            v.releaseCountdown = -1;
            if (v.active && ! v.pending)
                noteOffVoice (v);
        }
    }
    // a control point at the span's end belongs to the next span
}

int Part::mostRecentActive() const noexcept
{
    int best = -1;
    std::uint64_t order = 0;
    for (int i = 0; i < kVoices; ++i)
    {
        const auto& v = (*voices)[static_cast<std::size_t> (i)];
        if (v.active && (best < 0 || v.startOrder > order))
        {
            best = i;
            order = v.startOrder;
        }
    }
    return best;
}

void Part::renderShared (int len) noexcept
{
    std::fill (sum.begin(), sum.begin() + len, 0.0);
    std::array<int, kVoices> act {};
    int nAct = 0;
    for (int i = 0; i < kVoices; ++i)
    {
        auto& v = (*voices)[static_cast<std::size_t> (i)];
        if (v.active)
            act[static_cast<std::size_t> (nAct++)] = i;
        else
            v.skipPending += len;
    }
    // the cycle at each sample start of the segment: every chip of the part has it (one sample clock, TIME)
    cycAt[0] = refCycle;
    for (int k = 0; k < len; ++k)
        cycAt[static_cast<std::size_t> (k + 1)] = cycAt[static_cast<std::size_t> (k)] + refClock.nextSample();
    refCycle = cycAt[static_cast<std::size_t> (len)];
    if (nAct == 0)
    {
        // the shared filter rests: its next note starts it settled, its cutoff rules start afresh
        sharedSettle = true;
        sharedChanged = sharedMoving = false;
        return;
    }
    std::array<bool, kVoices> shedding {}; // chips that still hand a share on in this segment
    for (int a = 0; a < nAct; ++a)
    {
        const auto vi = static_cast<std::size_t> (act[static_cast<std::size_t> (a)]);
        std::fill (voiceOut[vi].begin(), voiceOut[vi].begin() + len, 0.0);
        shedding[vi] = ! (*voices)[vi].shareUp && (*voices)[vi].share > 0.0;
    }

    int pos = 0;
    while (pos < len)
    {
        tapPartSample = partSample + pos;
        // ticks inside sample pos (or due): the sample is rendered up to the tick (in progress), the tick runs
        int m = len - pos;
        for (int a = 0; a < nAct; ++a)
        {
            auto& v = (*voices)[static_cast<std::size_t> (act[static_cast<std::size_t> (a)])];
            for (int guard = 0; guard < 8 && v.engine.isRunning() && v.engine.nextTickCycle() <= cycAt[static_cast<std::size_t> (pos + 1)]; ++guard)
            {
                (void) v.chain.renderBusesUntil (1, v.engine.nextTickCycle());
                v.engine.runTick (v.sink);
            }
            if (v.engine.isRunning())
            {
                const std::uint64_t t = v.engine.nextTickCycle();
                int k = pos;
                while (k < len && cycAt[static_cast<std::size_t> (k + 1)] < t)
                    ++k;
                m = std::min (m, k - pos);
            }
        }
        m = std::max (1, m);
        // every chip renders the same m samples (no tick inside them)
        for (int a = 0; a < nAct; ++a)
        {
            auto& v = (*voices)[static_cast<std::size_t> (act[static_cast<std::size_t> (a)])];
            const int got = v.chain.renderBusesUntil (m, v.engine.nextTickCycle());
            jassert (got == m);
            juce::ignoreUnused (got);
        }
        // the shared filter, per bus sample: the sum of the chips' filterInputAc and the most recent note's static DC
        const int dcVoice = mostRecent >= 0 && (*voices)[static_cast<std::size_t> (mostRecent)].active ? mostRecent : mostRecentActive();
        const auto& dcChain = (*voices)[static_cast<std::size_t> (dcVoice)].chain;
        // a chip that starts settled gets the shared filter's output on its static level (the filter's fixed point for
        // its static DC, on a copy: the shared filter keeps its state)
        for (int a = 0; a < nAct; ++a)
        {
            auto& v = (*voices)[static_cast<std::size_t> (act[static_cast<std::size_t> (a)])];
            if (v.chain.settling())
            {
                const double dcS = v.chain.staticFilterBusDc (0);
                settleCopy = sharedFilter;
                if (sharedSettle)
                    settleCopy.reset();
                settleCopy.primeSteady (dcS);
                double lpS = 0.0;
                const double linS = settleCopy.peekLinearDouble (dcS, settleCopy, lpS);
                v.chain.settleShared (linS, lpS);
            }
        }
        // the chips' shares of the filter's output over these samples, and a fading chip's input factor
        const bool inputFades = sharedWeights (act, nAct, pos, m);
        for (int i = 0; i < m; ++i)
        {
            const std::int64_t p = partSample + pos + i;
            while (sharedCount > 0 && sharedQueue[static_cast<std::size_t> (sharedHead)].at <= p)
            {
                applySharedWrite (sharedQueue[static_cast<std::size_t> (sharedHead)], p);
                sharedHead = (sharedHead + 1) % kSharedQueue;
                --sharedCount;
            }
            if (sharedMoving && p - sharedLastChange >= chip::ChipVoice::kCutoffSettle)
            {
                sharedMoving = false;
                sharedFilter.setCutoff (sharedCutoff);
            }
            const double dc = dcChain.staticFilterBusDc (i);
            double in = dc;
            if (! inputFades)
                for (int a = 0; a < nAct; ++a)
                    in += (*voices)[static_cast<std::size_t> (act[static_cast<std::size_t> (a)])].chain.filterInputAc (i);
            else
                for (int a = 0; a < nAct; ++a)
                {
                    const auto vi = static_cast<std::size_t> (act[static_cast<std::size_t> (a)]);
                    in += shareIn[vi][static_cast<std::size_t> (i)] * (*voices)[vi].chain.filterInputAc (i);
                }
            if (sharedSettle)
            {
                sharedFilter.primeSteady (dc); // the static level: the voices' DC, no audio, envelopes 0
                sharedSettle = false;
            }
            const auto j = static_cast<std::size_t> (i);
            shLinear[j] = sharedMoving ? sharedFilter.processWithCutoffLinearDouble (in, sharedCutoff, shLowPass[j])
                                       : sharedFilter.processLinearDouble (in, shLowPass[j]);
            shStatic[j] = sharedFilter.staticOutput (dc);
            shShape[j] = sharedFilter.outputShapeCoefficients();
        }
        for (int a = 0; a < nAct; ++a)
        {
            const auto vi = static_cast<std::size_t> (act[static_cast<std::size_t> (a)]);
            (*voices)[vi].chain.finishSharedWeighted (shShape.data(), shLinear.data(), shLowPass.data(), shStatic.data(), shareW[vi].data(),
                                                      voiceOut[vi].data() + pos, m);
        }
        pos += m;
    }
    for (int a = 0; a < nAct; ++a)
    {
        const auto vi = static_cast<std::size_t> (act[static_cast<std::size_t> (a)]);
        mixVoice ((*voices)[vi], voiceOut[vi].data(), sum.data(), len);
        // the quiet criterion is judged on the chip's own contribution (8.21.1 after the review): not while it still
        // carried a share that it is handing on
        if (shedding[vi])
            (*voices)[vi].quiet = 0;
    }
}

bool Part::carriedByOthers (const NoteVoice& v) const noexcept
{
    for (const auto& u : *voices)
        if (&u != &v && u.active && u.shareUp && u.fadeLeft == 0)
            return true;
    return false;
}

bool Part::sharedWeights (const std::array<int, kVoices>& act, int nAct, int pos, int m) noexcept
{
    const double step = 1.0 / static_cast<double> (fadeSamples);
    bool moving = false;
    double total = 0.0;
    for (int a = 0; a < nAct; ++a)
    {
        const auto& v = (*voices)[static_cast<std::size_t> (act[static_cast<std::size_t> (a)])];
        moving = moving || (v.shareUp ? v.share < 1.0 : (v.share > 0.0 || v.fadeLeft > 0));
        total += v.share;
    }
    if (! moving)
    {
        // nothing moves: each chip's share of the sum (one chip, or equal chips: 1 / n; an idle chip that handed on: 0)
        for (int a = 0; a < nAct; ++a)
        {
            const auto vi = static_cast<std::size_t> (act[static_cast<std::size_t> (a)]);
            const double w = total > 0.0 ? (*voices)[vi].share / total : 0.0;
            std::fill (shareW[vi].begin(), shareW[vi].begin() + m, w);
        }
        return false;
    }
    bool inputFades = false;
    std::array<double, kVoices> share {};
    for (int i = 0; i < m; ++i)
    {
        // sample i: every share i steps on its way to its target; the weights are the shares over their sum
        double all = 0.0;
        for (int a = 0; a < nAct; ++a)
        {
            const auto& v = (*voices)[static_cast<std::size_t> (act[static_cast<std::size_t> (a)])];
            const double moved = static_cast<double> (i) * step;
            const double s = v.shareUp ? std::min (1.0, v.share + moved) : std::max (0.0, v.share - moved);
            share[static_cast<std::size_t> (a)] = s;
            all += s;
        }
        for (int a = 0; a < nAct; ++a)
        {
            const auto vi = static_cast<std::size_t> (act[static_cast<std::size_t> (a)]);
            const auto& v = (*voices)[vi];
            shareW[vi][static_cast<std::size_t> (i)] = all > 0.0 ? share[static_cast<std::size_t> (a)] / all : 0.0;
            // a chip that fades out while others carry the filter's output: its input to the filter fades with it (the
            // output gain mixVoice applies at this sample)
            const bool fading = v.fadeLeft > 0 && ! v.shareUp;
            shareIn[vi][static_cast<std::size_t> (i)] = fading ? std::max (0.0, v.fade - static_cast<double> (pos + i) * v.fadeStep) : 1.0;
            inputFades = inputFades || fading;
        }
    }
    for (int a = 0; a < nAct; ++a)
    {
        auto& v = (*voices)[static_cast<std::size_t> (act[static_cast<std::size_t> (a)])];
        const double moved = static_cast<double> (m) * step;
        v.share = v.shareUp ? std::min (1.0, v.share + moved) : std::max (0.0, v.share - moved);
    }
    return inputFades;
}

Part::CtlState Part::partControl() noexcept
{
    // the smoothed parameters and the vibrato oscillator advance (8.21.1); whether a host input may have moved
    CtlState c;
    const double variationBefore = sm[sVariation].value;
    bool moved = hostDirty;
    for (auto& s : sm)
    {
        moved = moved || s.left > 0;
        s.advance();
    }
    const double ctlRate = internalRate / static_cast<double> (controlInterval);
    vibratoPhase += std::clamp (sm[sVibratoRate].value, 0.0, 100.0) / ctlRate;
    vibratoPhase -= std::floor (vibratoPhase);
    // the vibrato moves the pitch at every control point while it has depth
    moved = moved || std::abs (sm[sWheel].value * sm[sWheelDepth].value) > 0.0;
    hostDirty = false;
    for (std::size_t k = 0; k < kNumSmoothedValues; ++k)
        c.v[k] = sm[k].value;
    c.vibratoPhase = vibratoPhase;
    c.moved = moved;
    c.variationMoved = sm[sVariation].value < variationBefore || sm[sVariation].value > variationBefore;
    if (c.variationMoved && current.model == 0)
        sharedFilter.setVariation (map::percentToFraction (static_cast<float> (c.v[sVariation])));
    return c;
}

bool Part::voiceControl (NoteVoice& v, const CtlState& c) noexcept
{
    if (! v.active)
        return false;
    // glide (per control interval), chip.variation on Model A while it moves
    bool glideMoved = false;
    if (v.glide < 0.0 || v.glide > 0.0)
    {
        v.glide *= glideFactor;
        if (std::abs (v.glide) < 1.0e-9)
            v.glide = 0.0;
        glideMoved = true;
    }
    if (current.model == 0 && c.variationMoved)
        v.filter.setVariation (map::percentToFraction (static_cast<float> (c.v[sVariation])));

    // the host inputs and a sync at the voice's current cycle, only when a host input can have changed (the sync writes
    // changed values only, so it would write nothing otherwise)
    if (v.engine.isRunning() && (c.moved || glideMoved))
    {
        v.engine.setHost (hostFor (v, c));
        v.engine.sync (v.chain.chip().cycle(), v.sink);
    }

    // the idle rule (8.5, 8.21.1)
    if (v.held || v.pending)
        return false;
    if (! v.idleSent)
    {
        const auto& chipNow = v.chain.chip();
        bool silent = ! v.engine.anyWaveLaneRunning();
        for (int n = 0; n < 3 && silent; ++n)
            silent = chipNow.envelope (n) == 0 && chipNow.envelopePhase (n) == chip::EnvPhase::release;
        const auto idleAfterNoteOff = static_cast<std::int64_t> (std::llround (kIdleAfterNoteOffSeconds * internalRate));
        if (silent || v.sinceNoteOff >= idleAfterNoteOff)
        {
            v.engine.enterIdle (chipNow.cycle(), v.restart, policy(), v.sink);
            v.idleSent = true;
            v.sinceIdle = 0;
            v.quiet = 0;
            if (current.shared)
            {
                // Shared: it hands its share of the filter's output on to the chips that carry it (Part.h "SHARES"),
                // so that its own contribution is what the quiet criterion below sees
                if (carriedByOthers (v))
                    v.shareUp = false;
            }
        }
        return false;
    }
    if (v.fadeKind != NoteVoice::Fade::none)
        return false;
    if (v.quiet >= controlInterval)
    {
        leaveSum (v);
        return true;
    }
    const auto leaveAfter = static_cast<std::int64_t> (std::llround (kLeaveSeconds * internalRate));
    if (v.sinceIdle >= leaveAfter)
    {
        if (current.shared && carriedByOthers (v))
            v.shareUp = false; // Shared: what it still carries of the filter's output goes to the others as it fades
        v.fadeKind = NoteVoice::Fade::leave;
        v.fadeLeft = fadeSamples;
        v.fadeStep = v.fade / static_cast<double> (fadeSamples);
    }
    return false;
}

void Part::controlPoint() noexcept
{
    // Shared and quiet: the part's control point, then every voice at its boundary (in index order)
    const CtlState c = partControl();
    tapPartSample = partSample;
    numLeft = 0;
    for (int i = 0; i < kVoices; ++i)
        if (voiceControl ((*voices)[static_cast<std::size_t> (i)], c))
            leftVoices[static_cast<std::size_t> (numLeft++)] = i;
    for (int k = 0; k < numLeft; ++k)
        allocator.voiceIdle (leftVoices[static_cast<std::size_t> (k)]);
    numLeft = 0;
}

void Part::afterSegment (int len) noexcept
{
    tapPartSample = partSample;

    // delayed releases of late notes, first: the segment's samples count for the notes that sounded in it, not for a
    // note that starts below (its countdown is its whole length)
    for (auto& v : *voices)
        if (v.releaseCountdown > 0)
        {
            v.releaseCountdown -= len;
            if (v.releaseCountdown <= 0)
            {
                v.releaseCountdown = -1;
                if (v.active && ! v.pending)
                    noteOffVoice (v);
            }
        }

    if (switchPending && switchLeft <= 0)
        finishSwitch();

    // fades that ended: a stolen voice starts its note, a voice that did not fall silent leaves (in index order: they
    // all end at this sample)
    numLeft = 0;
    for (int i = 0; i < kVoices; ++i)
    {
        auto& v = (*voices)[static_cast<std::size_t> (i)];
        if (v.fadeKind == NoteVoice::Fade::none || v.fadeLeft > 0)
            continue;
        if (v.fadeKind == NoteVoice::Fade::steal)
        {
            v.chain.suspend(); // the new note starts settled
            v.active = false;
            v.skipPending = 0;
            startWaiting (v, i, true, liveState(), partSample);
        }
        else
        {
            leaveSum (v);
            leftVoices[static_cast<std::size_t> (numLeft++)] = i;
        }
    }
    for (int k = 0; k < numLeft; ++k)
        allocator.voiceIdle (leftVoices[static_cast<std::size_t> (k)]);
    numLeft = 0;
}

// ---- Shared ----------------------------------------------------------------------------------------------------

void Part::sharedTap (void* context, int voice, NoteVoiceSink::FilterWrite what) noexcept
{
    static_cast<Part*> (context)->sharedWrite (voice, what);
}

void Part::setMostRecent (int voice) noexcept
{
    if (mostRecent >= 0)
        (*voices)[static_cast<std::size_t> (mostRecent)].sink.tapOn (false);
    mostRecent = voice;
    (*voices)[static_cast<std::size_t> (voice)].sink.tapOn (true);
}

void Part::sharedFollowVoice (int voice) noexcept
{
    // the shared filter takes the new note's cutoff, resonance and mode (its engine may not write them at tick 0 when
    // they equal what it wrote before)
    sharedWrite (voice, NoteVoiceSink::FilterWrite::cutoff);
    sharedWrite (voice, NoteVoiceSink::FilterWrite::resonance);
    sharedWrite (voice, NoteVoiceSink::FilterWrite::mode);
}

void Part::sharedWrite (int voice, NoteVoiceSink::FilterWrite what) noexcept
{
    auto& v = (*voices)[static_cast<std::size_t> (voice)];
    auto& c = v.chain.chip();
    SharedWrite w;
    w.what = what;
    if (what == NoteVoiceSink::FilterWrite::cutoff)
        w.cutoff = current.authentic ? static_cast<float> ((c.registerValue (chip::reg::cutoffLo) & 7) | (c.registerValue (chip::reg::cutoffHi) << 3))
                                     : c.cutoffSmooth();
    else if (what == NoteVoiceSink::FilterWrite::resonance)
        w.value = static_cast<std::uint8_t> (c.registerValue (chip::reg::resonanceRouting) & 0xf0);
    else
        w.value = c.registerValue (chip::reg::modeVolume);

    if (v.chain.isSuspended())
    {
        // before the voice's first render: part of the state it starts on (as ChipVoice applies such a write at once)
        if (what == NoteVoiceSink::FilterWrite::cutoff)
        {
            sharedCutoff = w.cutoff;
            if (! sharedMoving)
                sharedFilter.setCutoff (sharedCutoff);
        }
        else
            applySharedWrite (w, tapPartSample);
        return;
    }
    if (what == NoteVoiceSink::FilterWrite::cutoff)
    {
        (void) c.renderEventTime(); // the sample the write lands in (started here if none is in progress)
        w.at = tapPartSample + v.chain.filterLagSamples();
    }
    else
    {
        const std::int64_t s = v.chain.sharedSwitchSample();
        if (s < 0)
        {
            applySharedWrite (w, tapPartSample);
            return;
        }
        w.at = s - static_cast<std::int64_t> (c.renderSampleIndex()) + tapPartSample;
    }
    if (sharedCount == kSharedQueue)
    {
        applySharedWrite (sharedQueue[static_cast<std::size_t> (sharedHead)], sharedQueue[static_cast<std::size_t> (sharedHead)].at);
        sharedHead = (sharedHead + 1) % kSharedQueue;
        --sharedCount;
    }
    sharedQueue[static_cast<std::size_t> ((sharedHead + sharedCount) % kSharedQueue)] = w;
    ++sharedCount;
}

void Part::applySharedWrite (const SharedWrite& w, std::int64_t at) noexcept
{
    switch (w.what)
    {
        case NoteVoiceSink::FilterWrite::cutoff:
            // ChipVoice's rule for its own filter (applyParam): a change within kCutoffSettle samples of the previous
            // one makes the cutoff move (run per sample); otherwise it is set
            if (! (w.cutoff < sharedCutoff || w.cutoff > sharedCutoff))
                return;
            sharedCutoff = w.cutoff;
            if (sharedChanged && at - sharedLastChange < chip::ChipVoice::kCutoffSettle)
                sharedMoving = true;
            sharedLastChange = at;
            sharedChanged = true;
            if (! sharedMoving)
                sharedFilter.setCutoff (sharedCutoff);
            break;
        case NoteVoiceSink::FilterWrite::resonance: sharedFilter.setResonance (w.value >> 4); break;
        case NoteVoiceSink::FilterWrite::mode: sharedFilter.setMode (w.value); break;
    }
}
} // namespace bs
