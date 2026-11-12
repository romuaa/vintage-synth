# Architecture contracts

The interfaces that let milestones be built in parallel. Each section is the contract for one work
stream: what it must provide, what it may touch, and the gates it must pass. The *reasons* for the
choices are in `docs/decisions.md`. Internals not specified here are the implementer's call.

Change a contract only in its own commit, and only when no agent is working against it.

## 1. Signal and event flow (target after M5)

```
host MIDI ─► EventParser ─► KeyRouter ─► [Arp/Seq] ─► Part "lower" ─┐ mono
 (raw bytes)  (NoteEvent)   (M3: whole/   (M4, per     Part "upper" ─┤ mono
                             dual/split)   part)                     ▼
                                                    PartMixer (level, balance, pan) ─► stereo bus
                                                    ─► Chorus ─► Delay ─► Reverb ─► master gain
                                                    ─► NaN guard + clamp ─► host
```

- A **Part** is mono from its voices through its decimator (one decimator per part). Stereo starts at
  the PartMixer. Voices run at the oversampled rate; everything after the decimator runs at the host rate.
- FX add **no latency**. Reported latency stays decimator + BLEP delay.
- Everything in this diagram runs on the audio thread: all `CLAUDE.md` real-time rules apply.

## 2. Note events (introduced in M2)

`source/engine/NoteEvent.h`, JUCE-free, plain data:

```cpp
namespace vs
{
enum class EventType : std::uint8_t { NoteOn, NoteOff, PitchBend, ModWheel, SustainPedal,
                                      AllNotesOff, AllSoundOff, ResetControllers };

struct NoteEvent
{
    int offset;          // sample position inside the current render call, host rate, ascending
    EventType type;
    std::uint8_t note;   // NoteOn / NoteOff
    float value;         // NoteOn: velocity 0..1; PitchBend: -1..1; ModWheel: 0..1; SustainPedal: 0 or 1
};
}
```

- The processor parses raw MIDI bytes **once** per block into a preallocated `NoteEvent` buffer
  (never build a `juce::MidiMessage`). NoteOn with velocity 0 becomes NoteOff. Channel is ignored
  (omni) until M3 decides otherwise. Unknown messages are dropped.
- Buffer capacity is fixed at prepare time (2048 events per render call). On overflow the remaining
  events of that block are dropped and an `AllNotesOff` is appended at the last offset, so a note
  can never hang.
- **Panic semantics:** `AllNotesOff` (CC 123 and the overflow marker) releases all keys **and clears
  the sustain pedal**, so a held pedal can never keep notes hanging. `AllSoundOff` (CC 120) also
  silences the voices immediately. `ResetControllers` (CC 121) resets pitch bend, mod wheel and
  the sustain pedal (notes held only by the pedal are released).
- Malformed channel messages (too short, or a data byte with the high bit set) are dropped, not
  interpreted. Events positioned before the block are applied at offset 0; events at or after its
  end are applied after the last sample; a zero-length block still applies its events.
- `Part::render` takes `std::span<const NoteEvent>` instead of a `juce::MidiBuffer`. Events are applied
  sample-accurately by splitting the block, as today.
- The same type is the input and output of the arpeggiator/sequencer (section 6), which is why it
  must stay JUCE-free and trivially copyable.

## 3. M2 — polyphony (serial track)

### 3.1 Files

| File | Content |
|---|---|
| `engine/NoteEvent.h` (new) | Section 2 |
| `engine/VoiceAllocator.h` (new, JUCE-free) | Pure note → voice decision logic, no DSP, unit-testable without audio |
| `engine/Voice.*`, `engine/Part.*` | 16 voices, steal fade, portamento, velocity, optimisation |
| `PluginProcessor.*` | Event parsing, `Part::render` call |
| `params/PartParamList.h`, `Mappings.h`, `ParameterLayout.cpp` | New parameters (3.4) |
| `tests/dsp_unit/` | `VoiceAllocatorTests.cpp` (new), `PartTests.cpp` (extended) |

### 3.2 Behaviour

`kMaxVoices = 16` per part. Voice `i` of a part is seeded `partSeed + i` (deterministic renders).
One LFO per part, shared by all voices. The LFO delay restarts when a note starts with no other key held.

**Assign modes** (per part):

| Mode | Behaviour |
|---|---|
| Poly | One voice per note. Choice order for a new note: (1) a voice already sounding the same note (retrigger it), (2) the voice that has been idle longest, (3) steal. |
| Unison | Monophonic, last-note priority. `unison voices` (2/4/8/16) voices play the note, detuned symmetrically across `unison detune` (0–50 ct total spread). Output is scaled by 1/√n so loudness stays roughly constant. |
| Solo | One voice, last-note priority: a new note retriggers the envelopes; releasing a key while another is held returns to that note **without** retrigger (as in M1). |

Changing the assign mode or the unison voice count releases all sounding voices (no reset click).

**Stealing** (Poly, when all 16 are busy): steal the oldest voice in release; if none is in release,
the oldest held voice, but never the most recently started one. The stolen voice fades out over
**3 ms** (linear), is reset, and then starts the new note, so a stolen note starts 3 ms late.
No extra voices are allocated for the fade.

**Sustain pedal (CC 64):** key releases are deferred while the pedal is down and applied when it
comes up. In Solo/Unison the pedal holds the sounding note. Notes held only by the pedal count as
"a key held" for the LFO-delay restart, but not for Legato portamento.

**Unison** follows the Solo rules for retrigger and legato return. Its voices sit evenly across the
detune spread and start with a fixed phase pattern, so the level is the same on every note and
equal to one voice at any detune.

**Parameter names** in the host carry the part prefix ("Lower Portamento Time").

**Portamento** (per part): RC glide of the pitch (in semitones); `porta time` is the time to cover
99 % of the interval, 0 = off. Mode `Always` or `Legato` (glide only when a key is already held).
In Poly, a new voice glides from the pitch of the previously played note.

**Velocity** (per part):
- `vel → VCA`: gain = 1 − amount × (1 − velocity).
- `vel → VCF`: the VCF envelope-mod depth is scaled by 1 − amount × (1 − velocity).

**Pitch bend range** (per part): 0–12 semitones, default 2. **Mod wheel (CC 1)** adds vibrato: it
adds `wheel × mod wheel depth` to the VCO LFO mod amount (same taper and target switch as the slider).

### 3.3 Allocator requirements

- No allocation, no JUCE, no DSP state. It tracks key state, pedal state and voice ages, and emits
  actions for the Part to execute: *start(voice, note, velocity, glideFrom)*, *retarget(voice, note)*
  (legato pitch change), *release(voice)*, *steal(voice, note, velocity, glideFrom)*.
- The Part reports back when a voice has gone idle (ENV-2 finished).
- Deterministic: the same event sequence gives the same voice assignment.
- Exact signatures are the implementer's choice; the unit tests are written against behaviour in 3.2.

### 3.4 New parameters (all per part, appended to the lists in this order)

Float, appended to `VS_PART_FLOAT_PARAMS`:

| Member | ID suffix | Name | Range (default) | Mapping |
|---|---|---|---|---|
| `portaTime` | `porta.time` | Portamento Time | 0..1 (0) | 0 = off, else 5 s × v² |
| `unisonDetune` | `unison.detune` | Unison Detune | 0..1 (0.3) | 50 ct × v total spread |
| `velToVca` | `vel.vca` | Velocity → VCA | 0..1 (0.5) | linear amount |
| `velToVcf` | `vel.vcf` | Velocity → VCF | 0..1 (0) | linear amount |
| `modWheelDepth` | `mod.wheel_vco` | Mod Wheel Vibrato | 0..1 (0.3) | added to VCO LFO mod |

Choice, appended to `VS_PART_CHOICE_PARAMS`:

| Member | ID suffix | Name | Choices (default) |
|---|---|---|---|
| `assignMode` | `assign.mode` | Assign Mode | Poly, Unison, Solo (Poly) |
| `unisonVoices` | `assign.unison_voices` | Unison Voices | 2, 4, 8, 16 (4) |
| `portaMode` | `porta.mode` | Portamento Mode | Always, Legato (Always) |
| `bendRange` | `bend.range` | Bend Range | 0 … 12 semitones (2) |

Follow the `add-parameter` skill. `stateVersion` stays 1 (pre-release). The host parameter count
goes from 78 to 96; update `host_smoke` and `ProcessorTests`.

### 3.5 CPU optimisation (part of M2, done **before** the voice count goes up)

Budget: 32 voices at 2× oversampling, 48 kHz, under **15 % of one core** on the reference Mac,
*including* part overhead and leaving room for the effects (≤ 1.5 %, section 5).

Baseline (`voice_bench`, non-LTO, best of 3, 2026-11-12): **0.35 % per voice** at 2× with 32 voices
running, so 32 voices ≈ 11.2–11.5 %. Per-part overhead (smoothing, LFO, decimator) is about 0.04 %.
That fits, but M2 adds per-voice work (glide, velocity, steal fade, unison detune) and the effects
need their share. Target for M2: **≤ 0.30 % per voice** with all 32 running, measured the same way.
Where the time goes inside a voice has not been profiled yet; profile before optimising.

Permitted means (implementer's choice):
- Control-rate coefficient updates (pitch → phase increment, cutoff → `g`) with interpolation
  between updates. Audio-rate accuracy is required only where the modulator is audio-rate
  (cross-mod, sync).
- Parameter smoothing at control rate instead of 23 one-poles per oversampled sample.
- Voice-by-voice block rendering into an accumulation buffer; skip idle voices entirely.
- Bounded-error fast approximations of `exp2`/`tan`, with the error stated and tested.
- SIMD only as portable code (plain loops the compiler vectorises, or `juce::dsp::SIMDRegister`);
  no x86-only intrinsics without a NEON path.

Must stay true (existing tests, do not relax): alias ≤ −100 dB up to a 5 kHz fundamental; VCF
−12.04/−6.02 dB at fc; self-oscillation at fc ± 3 %; static pitch error ≤ 0.1 cent; envelope
timing; sample-accurate note-on; deterministic renders; no zipper noise on parameter moves
(add a test if smoothing changes). **Renders must not depend on the host block size or on where
events split the block, at any Vintage amount** (tested with Vintage at maximum). Approximations
return NaN for NaN input, so the processor's non-finite output guard still fires.

### 3.6 Gates for M2

1. All existing tests pass unchanged, except where the contract changes behaviour (state that per test).
2. New tests: allocator (every row of 3.2), 16-note chord renders 16 distinct pitches, 17th note
   steals without a click (bounded sample-to-sample step during the fade), sustain pedal, portamento
   timing, velocity scaling, bend range, mod wheel, unison detune spread and loudness, mode change
   without click, event-buffer overflow never hangs a note.
3. `voice_bench`: 16 and 32 voices inside the budget; per-voice target met or the gap explained.
4. pluginval strictness 10; `realtime-review` by a fresh reviewer; CI green on all three targets.
5. Audition render (`[.render]`) updated with a polyphonic demo for the owner's ear.

## 4. M3 — bi-timbral (serial track of Wave 2)

### 4.1 Structure
- Two `Part` instances, **Lower** and **Upper**, with different seeds, each fed by its own parameter
  binding (`lower.*`, `upper.*`; the parameters already exist). One LFO per part. Vintage is global.
- `engine/KeyRouter.h` (new, JUCE-free, no allocation): takes the parsed `NoteEvent`s of a render
  chunk and writes one event list per part.
- `engine/PartMixer.h` (new, JUCE-free): mono part outputs → stereo bus.
- The processor output becomes true stereo. With a mono output bus the result is (L + R) / 2.

### 4.2 Key modes (global choice parameter `key.mode`)
| Mode | Routing |
|---|---|
| Whole (default) | All notes to Lower. Upper is silent and costs no CPU. |
| Dual | All notes to both parts (up to 32 voices). |
| Split | Notes below the split point to Lower, notes at or above it to Upper. |

- Pitch bend, mod wheel, sustain pedal, All Notes Off, All Sound Off and Reset Controllers go to
  both parts in every mode. MIDI channel is ignored (omni).
- **A note-off always goes to the part(s) that received the note-on.** The router remembers the
  owner of each of the 128 keys, so moving the split point while keys are held can never hang a note.
- **Changing the key mode releases all voices of both parts** (as All Notes Off, without clearing
  controllers) and forgets held keys; no click.
- The split point (`key.split_point`, MIDI note 0–127, default 60, shown as a note name) only
  affects notes pressed after the change.

### 4.3 Mixer
- `mix.balance` (global, −1…+1, default 0): constant-power balance between Lower and Upper; at 0
  both are at unity. It has no effect in Whole mode (Lower at unity).
- `mix.pan` (per part, −1…+1, default 0, appended to the per-part float list): constant-power pan,
  unity at centre.
- All three are smoothed (no zipper noise).

### 4.4 Oversampling by host rate (closes the open CPU item)
| Host rate | Real time | Offline render |
|---|---|---|
| below 88.2 kHz | 2× | 4× |
| 88.2 kHz and above | 1× | 2× |

The internal rate is then at least 88.2 kHz at every host rate. Latency is reported per
configuration (decimator + BLEP; BLEP only at 1×). The alias gate (≤ −100 dB, 0–20 kHz) and all
existing DSP tests must pass in every configuration; `voice_bench 192000` must show 32 voices
inside the 15 % budget.

### 4.5 New parameters
Appended after all existing ones, in this order: per-part float `mix.pan` (at the end of
`VS_PART_FLOAT_PARAMS`, so it exists as `lower.mix.pan` and `upper.mix.pan`); then global
`key.mode` (Whole, Dual, Split), `key.split_point`, `mix.balance`. Host parameter count 96 → 101.
`stateVersion` stays 1 (pre-release). Follow the `add-parameter` skill.

### 4.6 Gates for M3
1. All existing tests pass; changed expectations are listed and justified (parameter count, stereo).
2. New tests: router (every row of 4.2, split-point moves with held keys, mode change, controllers
   to both parts, random event streams never hang a note); Dual = sum of both parts within float
   tolerance when rendered separately; Split boundaries; balance and pan laws (constant power
   ±0.1 dB); Whole mode: Upper costs no CPU and is silent; stereo output and mono fold-down;
   block-size independence with both parts sounding at full Vintage; oversampling table 4.4 incl.
   latency and the alias gate at a 96 kHz host rate; state round-trip of the new parameters.
3. `voice_bench`: Dual with 32 voices at 48 kHz and at 192 kHz inside the budget.
4. Sanitizer run, pluginval strictness 10, independent review, CI green on all four targets.
5. Audition render `m3_demo`: a Dual layer (two different patches), a Split (bass below, lead
   above), a split-point move and a mode change while playing.

## 5. M5 — effects (parallel track, new files only)

### 5.1 Common interface

Header-only, JUCE-free, in `source/fx/` (`Chorus.h`, `Delay.h`, `Reverb.h`), namespace `vs::fx`:

```cpp
struct XParams { /* plain floats/ints in physical units (Hz, seconds, 0..1) */ };

class X
{
public:
    void prepare (double sampleRate, int maxBlockSize);   // the only place that allocates
    void reset() noexcept;                                // clear all state, no allocation
    void setParams (const XParams&) noexcept;             // once per block; the block smooths internally
    void process (float* left, float* right, int numSamples) noexcept;  // in place, dry + wet
    static double tailSeconds (const XParams&) noexcept;  // pure function, callable from any thread
};
```

- Sample rates 44.1–192 kHz; `numSamples` from 1 to `maxBlockSize`; results must not depend on
  the block size (same output for 1-sample and 512-sample blocks, within float tolerance).
- No latency. No allocation, locks or exceptions after `prepare`. No denormal stalls with silent input.
- **Bounded work per call:** `process`, `setParams` and `reset` never clear or touch more memory
  than a small multiple of `numSamples` (no bulk clearing of delay lines on the audio thread; stale
  content is hidden by gating reads). `reset()` is therefore safe on the audio thread.
- A non-finite or absurdly large input sample is replaced by 0 at the effect's input, so one bad
  sample cannot poison a feedback path.
- `tailSeconds` is computed from the parameters only (no object state), so the processor can call
  it on the message thread. The reported tail is capped at 30 s.
- With the effect at level/mix 0 or mode Off the output equals the input **bit-exactly**, and
  switching it on or off never clicks (short crossfade).
- Own implementations only (AGPLv3). Published algorithms are fine; copied code needs a compatible
  licence and a note in `docs/decisions.md`.
- Parameter IDs, host mappings and wiring into the processor are **not** part of this track; they
  happen in the Wave 3 integration step. Do not touch `source/params/` or `PluginProcessor.*`.

### 5.2 The three effects

| Effect | Character and parameters | Must-pass tests |
|---|---|---|
| **Chorus** | Classic bucket-brigade style stereo chorus: mono sum in, L/R modulated in anti-phase, band-limited wet path, clean (no added noise). Params: `mode` (Off, I, II; I slow and deep, II faster), `mix` (full mix = wet about as loud as dry). | Mono-compatible at the default mix (worst dip of L+R below dry ≤ 6 dB for sines across 900–1100 Hz) and never a full cancellation at maximum mix, delay modulation stays inside the delay line, interpolation error ≤ −80 dB against the ideal delayed sine, mode switch click-free. |
| **Delay** | Stereo delay, `time` 1 ms–2 s, `feedback` 0–95 %, `damping` (low-pass in the feedback path), `pingPong` on/off, `level`. Time changes glide (no zipper, no clicks). | Echo spacing exact to ±1 sample, feedback decay per repeat matches the setting ±0.5 dB (also at fractional delay times), stable at max feedback for 60 s of noise, time sweep without discontinuities (tested at feedback 0, because repeats re-pitch during a glide). |
| **Reverb** | Algorithmic hall/plate (FDN or plate topology), lush and smooth to suit pads. Params: `decay` 0.3–20 s (RT60), `damping`, `preDelay` 0–200 ms, `level`. | RT60 at 1 kHz within ±15 % of the setting and independent of sample rate, bounded energy for any parameter combination, L/R tail correlation < 0.5, tail falls below −120 dBFS after silence, no metallic ringing: with impulse excitation, no tail-spectrum peak > 6 dB above the third-octave mean, 150 Hz–16 kHz, across decay, damping and sample-rate extremes. |

### 5.3 Gates for M5 blocks

- Tests in `tests/fx_unit/` (Catch2, added to `VintageSynthTests`).
- `tests/benchmarks/FxBench.cpp` (`fx_bench` target): all three effects together **≤ 1.5 % of one
  core** at 48 kHz stereo.
- A hidden `[.render]` test writes an audition WAV (dry pad phrase, then each effect, then all three).
- `realtime-review` by a fresh reviewer.

## 6. M4 — arpeggiator and sequencer (parallel track of Wave 2: module and tests only)

Pure, JUCE-free event transformers in `source/seq/`, tested on event lists. Host parameters and
wiring into the processor happen in Wave 3. One instance sits in front of a part.

```cpp
struct Transport { double sampleRate; double bpm; double ppqPosition; bool playing; };

// Returns the number of events written to `out` (ascending offsets). Never allocates.
int process (std::span<const NoteEvent> in, const Transport&, int numSamples,
             std::span<NoteEvent> out) noexcept;
```

### 6.1 Common rules
- **Clock:** when the host transport is playing, steps are locked to the host's musical position
  (`ppqPosition`, `bpm`), sample-accurately, including tempo changes, loop jumps and playback
  starting mid-bar. When the transport is stopped, an internal clock runs at the module's own tempo
  setting and starts on the first key.
- Rate as a note division: 1/4, 1/8, 1/8 triplet, 1/16, 1/16 triplet, 1/32. Swing 50–75 %
  (delays every second step). Gate length 5–100 % of the step.
- **Every note-on it emits gets a note-off**, also on bypass, mode or setting changes, All Notes
  Off, transport stop/start and reset. A random stream of events and setting changes must never
  leave a note sounding.
- Pitch bend, mod wheel, sustain pedal and panic events pass through unchanged, in order.
- Output velocity: the pressed key's velocity (arp) or the step's velocity (sequencer).
- Deterministic: same input, transport and seed → same output, for any block size.
- When off, events pass through bit-exactly.
- Output capacity is fixed; on overflow it stops emitting new notes but still emits the note-offs
  it owes.

### 6.2 Arpeggiator
- Modes: Up, Down, Up & Down (end notes not repeated), Random (seeded), As Played.
- Octave range 1–4. Hold (latch): notes stay in the pattern after the keys are released; the next
  key pressed after all keys were up starts a new pattern.
- Keys added or released change the pattern from the next step. With the sustain pedal down,
  released keys stay in the pattern.

### 6.3 Step sequencer
- 1–16 steps. Each step: semitone offset (−24…+24), type (Note, Rest, Tie) and velocity.
  A Tie extends the previous note without retriggering.
- It plays while a key is held: the lowest held key sets the transposition (relative to C4) and
  the sequence restarts from step 1 when the first key is pressed. With Hold on it keeps playing
  after release, and a new key re-transposes without restarting.
- Step data is plain data in the module's settings struct (host parameters are defined in Wave 3).

### 6.4 Gates for M4
- Tests in `tests/seq_unit/`: every rule above; timing exact to the sample against an independently
  computed expectation at several tempi, rates, swing values, sample rates and block sizes
  (including 1-sample blocks and tempo changes mid-block); loop jumps; long random streams with the
  "no hanging note" and "note-offs match note-ons" invariants.
- No allocation (the existing allocation counter test pattern), sanitizer-clean, independent review.
- Does not touch `source/engine/`, `source/params/` or `PluginProcessor.*`.

## 7. Test and validation infrastructure (parallel track in Wave 1)

- **VST3 validator** wired into `ctest` and CI (macOS and Windows x64) next to pluginval.
- **Golden-render harness** (`tests/render_regression/`): renders a fixed event script offline through
  the processor and compares against a stored reference. The comparison must tolerate cross-platform
  float differences (arm64 / x64, clang / MSVC math libraries): compare perceptual features
  (band energies and envelope over time), not samples. Goldens themselves are created in M6.

## 8. Second instrument: "B-Side" chip synth (outline; refined before its first milestone)

A second plugin in this repository, named **B-Side** (shown as "Exitium B-Side"): a polyphonic synthesizer modelled on a classic 3-voice sound
chip from early-1980s home computers. It has its own name, panel, plugin identity and parameter
list. **No third-party brand names, part numbers or nicknames anywhere in the repository**; the two
chip revisions are called **Model A** (early: warm, distorting filter) and **Model B** (later: clean
filter).

### 8.1 Structure
- **Separate plugin target, shared code.** Not a second engine inside Vintage Synth: hosts identify a
  plugin by its codes and parameter list, and the two instruments share neither parameters nor presets.
- **Step S0 extracts a common library** (`source/common/`) from Vintage Synth, with no behaviour
  change: note events and MIDI parsing, voice allocator, parameter plumbing and smoothing, fast math,
  BLEP table, oversampler, effect blocks, test helpers, render-regression harness, benchmarks.
  S0 runs only when no other agent is editing the engine (after M3 is merged).
- Each plugin then has its own directory (`source/vintage/`, `source/chip/`), CMake target, identity,
  parameter list and voice code. CI builds and validates both.

### 8.2 Sound engine: own model, not an embedded emulator
- **One note = one chip voice:** three oscillators with the chip's sync and ring-modulation routing,
  three envelopes, one filter with per-oscillator routing, and the output stage. 16 voices per part.
  An optional paraphonic mode (three notes share one filter, as on the hardware) is a later decision.
- **Digital parts are reproduced bit-exactly** (phase accumulators, noise shift register, envelope
  counters with their stepped exponential curve, sync, ring modulation), computed per output sample
  in closed form rather than per chip clock cycle.
- **Waveforms are band-limited** at our oversampled rate, with the chip's waveform shapes
  (12-bit steps, combined waveforms from a parametric model, Model A converter kinks).
- **Filter:** a non-linear two-pole state-variable model per voice, with Model A and Model B curves
  and a "chip variation" control. Cutoff, pitch and pulse width are continuous; an optional
  "register resolution" switch restores the original stepping.
- **Quirks that hurt playing are options, not defaults** (for example the envelope retrigger delay:
  "Authentic" or "Clean").
- CPU target: 16 voices ≤ 5 % of one core at 48 kHz; 32 voices within the shared 15 % budget.

### 8.3 Proving authenticity
- **Reference oracle, offline only:** an established cycle-accurate emulator (GPL v2-or-later) is
  built and run in a separate CI workflow, never vendored, never shipped and never linked. It
  produces reference data (envelope and oscillator sequences, waveform periods, filter responses and
  distortion curves) that is committed under neutral names; the product's tests compare against it.
- Gates: digital sequences bit-exact; waveform periods within ±1 step; filter cutoff within ±5 % and
  resonance peak within ±1 dB of the oracle per revision; spectral distance on scripted patches;
  determinism and block-size independence as for Vintage Synth.
- **The alias gate is redefined for chip waveforms:** components folding at our internal rate stay
  ≤ −100 dB; harmonics that belong to the chip's waveform shape are exempt.
- **By ear:** the owner compares audition renders with public recordings of the original hardware.
  Recordings are never downloaded into the repository or used as test data.

### 8.4 Milestones
| Step | Scope |
|---|---|
| S0 | Extract the common library (no behaviour change; all existing gates stay green) |
| S1 | Contract, plugin shell, parameter list, oracle workflow and reference data |
| S2 | Oscillators, noise, envelopes: bit-exact against the oracle |
| S3 | Model B filter, then Model A filter with chip variation; output stage |
| S4 | Polyphony, modulation, effects from the common library; audition by the owner |
| S5 | Presets, custom UI and release together with Vintage Synth's M6–M8 |
