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
                                                    ─► Chorus ─► Delay ─► Reverb ─► master gain × trim
                                                    ─► NaN guard ─► soft limiter ─► clamp ─► host
```

- A **Part** is mono from its voices through its decimator (one decimator per part). Stereo starts at
  the PartMixer. Voices run at the oversampled rate; everything after the decimator runs at the host rate.
- FX add **no latency**. Reported latency stays decimator + BLEP delay.
- Everything in this diagram runs on the audio thread: all `CLAUDE.md` real-time rules apply.

## 2. Note events (introduced in M2)

`source/common/engine/NoteEvent.h`, JUCE-free, plain data:

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
- `mix.balance` (global, −1…+1, default 0) between Lower and Upper, and `mix.pan` (per part,
  −1…+1, default 0, appended to the per-part float list) use the same **no-boost law**: unity on
  both sides at centre; toward either side the near side stays at exactly unity and the far side
  falls (√2·cos θ, capped at 1) to exactly 0 at the extreme. Balance has no effect in Whole mode.
- All gains are smoothed (no zipper noise).
- **Latch after a key-mode change:** a part that is still sounding keeps its mixer gains until it
  starts its first new note or has gone silent, so release tails fade at the level they were heard at.
- Key mode and split point are read once per host block: a change takes effect at that block's
  first sample (accepted; automation of these two is block-accurate, not sample-accurate).

### 4.4 Oversampling by host rate (closes the open CPU item)
| Host rate | Real time | Offline render |
|---|---|---|
| below 44.1 kHz | 4× | 4× |
| 44.1 kHz to below 88.2 kHz | 2× | 4× |
| 88.2 kHz and above | 1× | 2× |

The internal rate is then at least 88.2 kHz at every host rate from 22.05 kHz up. Latency is the
measured delay of the decimator alone plus the BLEP delay, rounded to the nearest sample (6 at 2×,
5 at 4×, 8 at 1×). The alias gate's band is 0 to min(20 kHz, 20/48 × host rate). The alias gate (≤ −100 dB, 0–20 kHz) and all
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

Header-only, JUCE-free, in `source/common/fx/` (`Chorus.h`, `Delay.h`, `Reverb.h`), namespace `vs::fx`:

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

Pure, JUCE-free event transformers in `source/common/seq/`, tested on event lists. Host parameters and
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
- Pitch bend, mod wheel and panic events pass through unchanged, in order. **While the module is
  on it consumes the sustain pedal** (it keeps released keys in the pattern) and does not pass it
  to the part; switching on or off hands the pedal state over.
- At 100 % gate, when the next note has a different pitch, its note-on comes before the previous
  note-off (legato: a Solo part can glide); same pitch: off, then on.
- A small deviation of the host position (up to about one step, e.g. during tempo ramps) is drift:
  re-anchor without cutting or repeating notes. Only a larger discontinuity or a transport
  start/stop releases and resynchronises. Swing and rate changes never repeat or skip a step.
- Output velocity: the pressed key's velocity (arp) or the step's velocity (sequencer).
- Deterministic: same input, transport and seed → same output, for any block size.
- When off, events pass through bit-exactly.
- Output capacity is fixed; on overflow it stops emitting new notes, and a note-off that does not
  fit stays owed and is retried, so no note can hang. `ArpSeq::minimumCapacity` gives the size at
  which nothing is ever dropped.

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

## 6a. Wave 3 integration: effects and arpeggiator/sequencer in the plugin (serial track)

Wires the standalone modules of sections 5 and 6 into the processor and defines their host
parameters. No change to the modules' DSP or logic unless a defect is found (then fix it there,
with a test).

### 6a.1 Signal and event flow
```
host MIDI → EventParser → KeyRouter → ArpSeq (one per part) → Part lower / Part upper
          → PartMixer → Chorus → Delay → Reverb → master gain → non-finite guard + clamp → host
```
(Since Wave 4 the output stage is master gain × trim → non-finite guard → soft limiter → clamp; see 6c.)
- The effects run on the stereo bus at the host rate, add no latency, and are skipped bit-exactly
  when off (chorus mode Off, delay and reverb level 0).
- `getTailLengthSeconds()` is the largest of the effects' static `tailSeconds (params)` (each
  capped at 30 s), computed from the parameter values, callable from any thread.
- When the non-finite guard fires it resets both parts, both arp/seq instances and all three effects.
- `reset()` and `prepareToPlay` reset everything together (parts, router, arp/seq, effects).
- **Transport** comes from the host play head on the audio thread: tempo, musical position and the
  playing flag. If any of them is missing, the transport counts as stopped (internal clock). When
  the host block is larger than the prepared size and is rendered in chunks, the position for each
  chunk is advanced from the block's start position.
- Arp/seq settings are built from the parameter atomics once per host block on the audio thread.
- Event buffers behind each arp/seq are sized with `ArpSeq::minimumCapacity` for the prepared block.

### 6a.2 New host parameters (global; appended after all existing ones, in this order)
Effects:

| ID | Name | Range (default) | Mapping |
|---|---|---|---|
| `fx.chorus.mode` | Chorus | Off, I, II (Off) | |
| `fx.chorus.mix` | Chorus Mix | 0..1 (0.5) | the block's mix curve |
| `fx.delay.time` | Delay Time | 0..1 (700 ms) | 1 ms … 2 s, exponential |
| `fx.delay.feedback` | Delay Feedback | 0..1 (0.35) | 0 … 95 % |
| `fx.delay.damping` | Delay Damping | 0..1 (0.3) | |
| `fx.delay.ping_pong` | Delay Ping-Pong | Off, On (On) | |
| `fx.delay.level` | Delay Level | 0..1 (0) | |
| `fx.reverb.decay` | Reverb Decay | 0..1 (2.5 s) | 0.3 … 20 s, exponential |
| `fx.reverb.damping` | Reverb Damping | 0..1 (0.5) | |
| `fx.reverb.predelay` | Reverb Pre-Delay | 0..1 (20 ms) | 0 … 200 ms |
| `fx.reverb.level` | Reverb Level | 0..1 (0) | |

Arpeggiator and sequencer (one settings set; both instances use it):

| ID | Name | Range (default) |
|---|---|---|
| `arp.mode` | Arp/Seq Mode | Off, Arpeggiator, Sequencer (Off) |
| `arp.target` | Arp/Seq Part | Lower, Upper, Both (Both) |
| `arp.rate` | Arp/Seq Rate | 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 (1/16) |
| `arp.swing` | Arp/Seq Swing | 50 … 75 % (50) |
| `arp.gate` | Arp/Seq Gate | 5 … 100 % (50) |
| `arp.hold` | Arp/Seq Hold | Off, On (Off) |
| `arp.tempo` | Internal Tempo | 20 … 300 bpm (120) |
| `arp.pattern` | Arp Pattern | Up, Down, Up & Down, Random, As Played (Up) |
| `arp.octaves` | Arp Octaves | 1 … 4 (1) |
| `seq.length` | Seq Length | 1 … 16 (16) |
| `seq.stepNN.note` (NN = 01…16) | Seq Step NN Note | −24 … +24 semitones (0) |
| `seq.stepNN.type` | Seq Step NN Type | Note, Rest, Tie (Note) |
| `seq.stepNN.velocity` | Seq Step NN Velocity | 0..1 (0.8) |

The step parameters are appended step by step (note, type, velocity for step 01, then step 02, …).
69 new parameters: host parameter count 101 → 170. Display text and DSP share the mappings in
`params/Mappings.h`. Continuous effect parameters are smoothed by the blocks themselves; switches
are not smoothed. `stateVersion` stays 1 (pre-release). A part not selected by `arp.target` gets
its events unchanged. The random seed is fixed, so renders are reproducible.

### 6a.3 Gates
1. All existing tests pass; changed expectations (parameter count, tail length) are listed.
2. New tests: every parameter reaches its module with the documented mapping (display ↔ value
   round-trip); effects off = bit-identical to the M3 output; each effect audible and stereo when
   on; tail length matches the parameters; the guard resets effects and arp/seq; arp/seq through
   the real parts — pedal consumed while on, no pile-up; legato glide in Solo at 100 % gate;
   Off ↔ on with held keys; `arp.target` routing in Whole, Dual and Split; host transport
   (start, stop, loop, tempo change, missing play head) through the processor; chunked rendering
   keeps step timing; random streams never hang a note; block-size independence with arp and
   effects on; state round-trip of all 170 parameters (172 since Wave 4, section 6c); no allocation in `processBlock`.
3. `voice_bench`/`fx_bench`: Dual with 32 voices, arp on and all three effects on, inside 15 % of a
   core at 48 kHz, and reported at 192 kHz.
4. Golden scripts with effects and with the arpeggiator (host-synced).
5. Sanitizer run, pluginval strictness 10, VST3 validator and all targets in CI, independent review.
6. **Audition render `m5_demo` for the owner (the M5 audition gate):** pads with chorus I and II,
   a lead with the 700 ms ping-pong delay, the hall reverb at several decays, all three together,
   an arpeggiated chord over a Split bass, and a short step-sequencer line.

## 6b. M6 — presets and banks (after S0)

### 6b.1 Terms
- **Tone:** the sound of one part: all of that part's parameters (the `lower.*` or `upper.*` set).
- **Program:** the whole instrument: Lower tone, Upper tone and the global parameters (key mode,
  split point, balance, arp/seq incl. the 16 steps, effects, Vintage). Master volume and the output
  limiter (section 6c) are not part of a program. A program has a name, a category and an author.
- **Bank:** 64 programs in 8 groups of 8 (A1 … H8). One **factory bank** is compiled into the
  plugin and is read-only; **user banks** are files.

### 6b.2 File format
- One text file per program and one per bank, JSON, UTF-8, with a `format` version number.
  Parameters are stored **by ID with their plain value** (the value the parameter's range uses);
  a tone inside a program is stored without the part prefix, so a tone can be loaded into either part.
- Loading is tolerant: unknown IDs are ignored, missing IDs take the parameter's default, values
  are clamped to the range, non-finite values become the default. A malformed file is rejected as
  a whole with an error message; it never crashes and never half-applies.
- User files live in the platform's user documents area under `Exitium/Vintage Synth/`
  (`Programs/`, `Tones/`, `Banks/`). The plugin never writes anywhere else.
- Factory programs are authored as the same JSON files under `presets/factory/` in the repository
  and embedded at build time.

### 6b.3 Behaviour
- **Applying a program or tone** happens on the message thread: all sounding notes are released
  first (as a key-mode change does), then the parameters are set through the host-notifying API so
  hosts, automation and the editor follow. Nothing in the audio thread allocates, locks or parses.
- **MIDI Program Change** selects a program of the current bank: the audio thread only stores the
  requested index; the message thread applies it.
- **Host program list:** the 64 programs of the current bank, with their names.
- **Plugin state** (host session): all parameters as today, plus the current bank reference, the
  current program index and name, and a "modified" flag. Loading a session restores exactly the
  saved sound, whether or not the bank it came from still exists.
- `stateVersion` stays 1 before the first release; the migration hook and a test that loads a
  state saved by the previous milestone are part of M6.
- **Until the custom UI (M7):** a minimal preset bar above the generic editor (previous / next,
  program name, bank, load, save as, initialise) so presets are usable now.

### 6b.4 Factory bank
- 64 programs covering what this kind of instrument is known for: brass, strings and pads, basses,
  leads (incl. sync and unison leads), bells and plucks (cross-mod), organ and keys, effects and
  noises, arpeggiated and sequenced programs, Dual layers and Splits. Own names; no third-party
  names of any kind (the hygiene check covers `presets/`).
- **Level:** every program is trimmed (part VCA levels only) so that, measured with the output
  limiter off at the default master volume: a full-velocity eight-note chord stays below 0 dBFS and
  at most 3 dB above Init's (RMS and peak); and the category's standard phrase at velocity 100 is
  within ±2 dB (unweighted RMS) of the median of its level group (sustained, percussive, bass).
  With the limiter on at its defaults, no standard phrase reaches the limiter's knee. Phrases,
  groups and measurements: `docs/factory-sounds.md`. This is tested for every program.
- Programs are drafted by an agent from a written sound specification and **auditioned by the
  owner** (the M6 audition gate): one short rendered phrase per program, with a contact sheet
  (name, category, what to listen for).

### 6b.5 Gates
1. Tests: save → load round-trips of tones, programs and banks; tolerant loading (unknown,
   missing, out-of-range, non-finite, malformed, truncated, wrong `format`); a tone loaded into
   the other part sounds the same; every factory program loads, sounds, is finite, meets the level
   rule and survives a state round-trip; applying a program releases notes without a click and
   without audio-thread allocation; MIDI Program Change; host program list; a session saved by the
   previous milestone loads unchanged.
2. Golden renders: a fixed phrase for Init and seven designed factory programs (`ctest -L render`);
   the load, level and release tests cover all 64 (owner decision: eight references are enough).
3. `scripts/preflight.sh` passes; independent review; the owner's audition.

## 6c. Wave 4 — output stage: headroom and soft limiter (owner decision, 2026-11-13)

### 6c.1 Signal path
```
… → Reverb → × master gain × kOutputTrim → non-finite guard → SoftLimiter (if on) → ±4.0 clamp → host
```
- **Trim:** `VintageSynthProcessor::kOutputTrim` = 0.5 (−6.02 dB), fixed, multiplied into the
  master gain. Per-voice, per-part and effect gains are unchanged, so relative program levels and
  the effect send balance stay as they were. A power of two: a render differs from the untrimmed
  one by exactly that factor (except values that become subnormal and are flushed to zero).
- **Limiter** (`common/fx/SoftLimiter.h`): stereo-linked, zero latency (the reported latency does
  not change), soft knee 3 dB below the ceiling, gain exactly 1 below the knee (samples untouched,
  bit-identical to limiter off), output never above the current (smoothed) ceiling (float
  rounding aside; the ceiling ramps over 20 ms), program-dependent release. Block-size
  independent, deterministic, allocation-free, denormal-safe.
- **Switch:** crossfaded over 5 ms (applied gain g^m, m ramping 0 ↔ 1, linear in dB), a deliberate
  exception to "never smooth switches" (docs/decisions.md). The detectors keep running during a
  fade-out; fully off, the limiter does nothing and costs nothing (bit-identical to the path ×
  trim); switched on from fully off it starts clean (detectors cleared). Below the knee the
  samples stay untouched, also during a fade. Reset with everything else (`reset()`, the
  non-finite guard, the end of a host bypass), at the current parameter values without a ramp.
- The clamp at ±4.0 (+12 dBFS) stays as a safety net behind the limiter.
- The limiter adds no tail: the reported tail length is unchanged.

### 6c.2 New host parameters (global; appended after `seq.step16.velocity`, in this order)

| ID | Name | Range (default) |
|---|---|---|
| `master.limiter` | Limiter | Off, On (On) |
| `master.limiter.ceiling` | Limiter Ceiling | −12 … 0 dB, continuous (−1 dB) |

Host parameter count 170 → **172**. Like `master.volume`, both are **session settings**: saved in
the host session, not part of a program, tone or bank; loading or initialising a program does not
change them and changing them does not mark the program modified. Non-finite host values become
the default (sanitised binding). The ceiling is smoothed (20 ms linear ramp in dB); the switch is
crossfaded (5 ms). The ceiling is shown in 0.1 dB steps ("0.0", never "-0.0").

**Sessions saved before a parameter existed:** every parameter missing from a loaded state is set
to its default (the processor adds an empty entry for it before the state is applied; JUCE 9.0.5
also does this by itself, the processor makes it explicit). A session saved before Wave 4 so loads
with the limiter on at −1 dB, and plays 6.02 dB quieter than before (the trim).

### 6c.3 Gates
1. Headroom: the 32-voice full-velocity Dual chord, dry, default master, limiter off, peaks at or
   below −1 dBFS.
2. Limiter block: below the knee bit-identical; the gain returns to exactly 1 after a burst; no
   sample above the ceiling on sustained material (+6/+12/+24 dB over) or a hard transient, at
   44.1/48/192 kHz; **bit-identical** for block sizes 1, 7, 61, 512 and 4096 with a moving (and
   NaN) ceiling; switching below the knee leaves the samples untouched; fully off does nothing.
3. Through the processor: ceiling held for the loudest chord dry and with extreme effects; limiter
   off = the plain path plus trim (bit-identical to the M3 reference at a level far above the
   ceiling); distortion of a chord 6 dB over the ceiling below −38 dB (20 ms fits); block-size
   independent with ceiling automation and switch changes (whole processor, blocks 512 / 61 / 7:
   difference **below −120 dB**, not bit-identical, as for the rest of the engine); a ceiling
   sweep in 0.5 dB steps changes the gain by less than 0.01 dB per sample (measured against the
   unswept render, so the detectors' own moves cancel); switching off or on while reducing the
   gain changes it by less than 0.1 dB per sample during the fade; non-finite values are safe;
   program loads leave both parameters alone; a session without the limiter entries loads them
   at their defaults.
4. Golden renders re-blessed only after showing old × 0.5 = new for every sample of the raw
   renders. Golden scene `limiter_drive` drives the limiter (master +6 dB, ceiling moves, switch
   off and on), so limiter changes show up in render regression.

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
*S0 is done: `source/common/` and `source/vintage/` exist. Test helpers, the render-regression harness
and the benchmarks stay under `tests/`. The processor skeleton and the display formatter are not
shared yet; B-Side's first milestone designs that abstraction.*

- **Separate plugin target, shared code.** Not a second engine inside Exitium Vintage Synth: hosts identify a
  plugin by its codes and parameter list, and the two instruments share neither parameters nor presets.
- **Step S0 extracts a common library** (`source/common/`) from Exitium Vintage Synth, with no behaviour
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

### 8.3 Patch programs: how the classic sounds are made
Most of the chip's signature sounds came from the player software rewriting its registers about
fifty times a second: a noise burst then a pulse wave for a snare, notes cycled so fast they read
as a chord, pulse width and cutoff moved in audible steps, and a deliberate envelope reset before
each note. B-Side therefore has a **program sequencer in every patch**, at the core of the voice,
not in the modulation section.

- **Tick clock:** a "frame rate" in Hz (presets 50.12 and 59.83, or free), a multiplier 1–8, and
  optional tempo sync. It starts on the note-on sample and is derived from the sample counter, so
  output never depends on the host block size; blocks are split at ticks exactly as at MIDI events.
- **Lanes,** each a short table (up to 32–64 steps) with delay counts, jump, end, loop and a
  release entry point:
  - *wave/note lane* per oscillator: waveform, gate, sync, ring and test bits, and a pitch that is
    relative (arpeggio), absolute (drums), unchanged, or a held-key index;
  - *pulse lane:* set, sweep, ping-pong between limits;
  - *filter lane:* set cutoff, sweep, switch mode, resonance and oscillator routing.
- **Program-driven values change in steps at the tick, unsmoothed,** and at register resolution in
  Authentic mode. The stepped vibrato, the per-tick cutoff steps and the attack snap are part of
  the sound. Host automation and ordinary modulation stay smooth. Band-limiting must keep the
  discontinuities a program creates (a waveform switch or phase reset mid-cycle is real content,
  placed at its exact sub-sample position).
- **Hard restart is a patch feature:** mode (off, classic, phase-reset), its envelope values, lead
  time in ticks, first-tick waveform, gate-off timer. Live playing gives no warning of the next
  note, so there are policies: idle voices are pre-conditioned (default, no latency); a stolen
  voice is reset directly in Clean mode; an optional look-ahead mode delays notes and reports the
  latency; Authentic does nothing and lets the envelope quirk happen.
- **Chip arpeggio is not the shared arpeggiator.** It cycles pitch inside one voice at tick rate
  with the gate held. A "chord into one voice" assign mode feeds the held keys to the note lane.
- **Filter topology per part:** per note (default), or shared (one filter for the whole part, as on
  the hardware; needed for paraphonic patches and for the early revision's overdrive when several
  notes are summed into it). The filter is a part-owned resource from the first milestone on.
- **Tables are patch data, not host parameters.** They live in the plugin state and preset format
  and are handed to the audio thread as immutable, preallocated objects. Host parameters are the
  macros (tick rate, multiplier, lane offsets and depths, restart mode). Until the table editor UI
  exists, patches are written in a **human-readable text format** kept in the repository.
- Drums are ordinary patches with absolute-note programs first; a kit mode (key ranges mapped to
  sub-patches) comes later.

### 8.4 Proving authenticity
- **Reference oracle, offline only:** an established cycle-accurate emulator (GPL v2-or-later) is
  built and run in a separate CI workflow, never vendored, never shipped and never linked. Reference
  data is committed under neutral names.
- **Register-trace replay is the main test:** the program engine emits time-stamped register
  writes; the same trace drives the oracle and our chip model (shared-filter, three-voice mode).
  Digital state must match bit for bit, including the envelope-quirk cases, phase resets, the noise
  generator's lock-up and recovery, sync and ring per step. Analog output is compared within
  tolerances (filter cutoff ±5 %, resonance peak ±1 dB, spectral distance on scripted patches).
- Golden traces for canonical tables (snare, kick, three-note arpeggio, ping-pong pulse sweep,
  filter sweep, each restart mode) at several tick multipliers.
- Block-size independence including tick positions; determinism; restart-policy tests.
- **The alias gate is redefined for chip waveforms:** components folding at our internal rate stay
  ≤ −100 dB; harmonics and steps that belong to the chip's waveform shape or to a program are
  exempt. Noise and combined waveforms get their own, documented budget.
- **By ear:** the owner compares audition renders with public recordings of the original hardware.
  Recordings are never downloaded into the repository or used as test data.

### 8.5 Milestones
| Step | Scope |
|---|---|
| S0 | Extract the common library (no behaviour change; all existing gates stay green) |
| S1 | Contract, plugin shell, oracle workflow, register-trace harness, text table format |
| S2 | Chip voice digital parts (bit-exact), tick clock, program engine (wave/note, pulse, filter lanes), restart modes and policies, stepped register-resolution path |
| S3 | Model B filter, then Model A filter with chip variation; output stage; filter as a part-owned resource |
| S4 | Polyphony, macros, effects from the common library; 10–20 reference patches (drums, arpeggio chord, pulse lead, filter bass); audition by the owner |
| S5 | Chord-into-one-voice mode, shared-filter mode, look-ahead restart, kit mode |
| S6 | Table editor UI, presets and release together with Exitium Vintage Synth's M6–M8 |
