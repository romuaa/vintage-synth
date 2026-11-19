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

## 8. Second instrument: "B-Side" chip synth

A second plugin in this repository, **Exitium B-Side**: a polyphonic synthesizer modelled on a
classic three-voice sound chip of early-1980s home computers. Own name, panel, plugin identity,
parameter list and presets. **No third-party brand, company, chip, product, game, song or composer
names anywhere in the repository, briefs included** (the hashed name check covers every tracked
file). The chip is described functionally; its two revisions are **Model A** (early: warm,
distorting filter, large part-to-part variation) and **Model B** (later: clean filter). The
emulator used as a test oracle is "the reference emulator", never named.

Status: S0 is done (`source/common/`). Sections 8.1–8.10 are the contract for S1–S4. S5 and S6 are
outlined in 8.11 and get their own contract before they start. Items marked **(Qn)** depend on a
pending owner decision (8.12); until it is made, build against the default stated there.

| Term | Meaning |
|---|---|
| chip | one instance of the modelled sound chip: 3 oscillators, 3 envelope generators, 1 filter, mixer, volume |
| oscillator n | one of the chip's three voices, n = 1..3 ("voice" in this section always means a note voice of a part) |
| register | the chip's write and read registers (8.4.1) |
| cycle | one period of the chip clock (about 1 MHz) |
| tick | one step of the patch program (8.6.1), typically 50 or 60 per second |
| table | the patch program of one tone: lanes of rows (8.6) |
| Smooth / Authentic | the register-resolution setting (8.4.6) |

### 8.1 Repository and build structure

#### 8.1.1 Directories
```
source/common/shell/     (S1a) shared bi-timbral processor skeleton and editor (JUCE)
source/bside/
  PluginProcessor.*      BSideProcessor, built on common/shell
  PluginEditor.*         preset bar + generic editor (until S6)
  chip/                  JUCE-free: chip model (8.4): clock, oscillator, noise, envelope, waveform shapes, DAC, filter, mixer, Chip
  program/               JUCE-free: table text reader/writer, compiled program, lane runner, tick clock, restart (8.6)
  trace/                 JUCE-free: trace and reference file formats, replay driver, comparer (8.9)
  engine/                Part (note voices = chips, allocator, part filter, decimator), NoteVoice, ProgramSlots
  params/                PartParamList.h (X-macro), ParameterLayout.*, PartMappings.h, StateVersion.h
  presets/               BSidePresets.* (instrument identity, schema, factory bank)
presets/bside/factory/   factory programs (JSON, embedded at build time)
tests/bside/             unit/ (JUCE-free), integration/, trace/ (traces/, reference/), golden/ (write traces),
                         render/ (scripts/, golden/), benchmarks/
```
- `source/common` includes nothing from an instrument directory; `source/vintage` and
  `source/bside` never include each other. JUCE-free by design: `bside/chip`, `bside/program`,
  `bside/trace` (the docs check enforces all of this from S1b).
- Vintage's factory presets stay in `presets/factory/` (no move).
- Nothing from the reference emulator is ever stored in this repository (8.9.5).

#### 8.1.2 CMake
| Item | Contract |
|---|---|
| Option | `VS_BUILD_BSIDE`, default ON from S1b. OFF builds exactly what is built today. |
| Source lists | `BS_SOURCES` (the `.cpp` files of `source/bside/`), `BS_PLUGIN_SOURCES = VS_COMMON_SOURCES + BS_SOURCES`. No static library of our code shared between the plugins: JUCE modules are compiled into each plugin target and the `JucePlugin_*` definitions differ per plugin. |
| Plugin target | `juce_add_plugin (BSide …)`: `PRODUCT_NAME "Exitium B-Side"`, `COMPANY_NAME "Exitium"`, `BUNDLE_ID com.exitium.bside`, `PLUGIN_MANUFACTURER_CODE Exit`, `PLUGIN_CODE Bsy1`, `FORMATS VST3`, `IS_SYNTH TRUE`, `NEEDS_MIDI_INPUT TRUE`, `VST3_CATEGORIES Instrument Synth`, `HARDENED_RUNTIME_ENABLED TRUE`; same compile definitions, warnings as errors, plugin-only LTO and Linux static-runtime link options as `VintageSynth`. **Identity is a proposal (Q1); frozen at the first release.** |
| Factory data | `BSideFactoryData` (`juce_add_binary_data`, namespace `bs_factory`) from `presets/bside/factory/*.json`. |
| Test core | `bs_test_core`: `BS_PLUGIN_SOURCES` without LTO, include directories and definitions taken from `BSide` (the `vs_test_core` pattern). |
| Test executables | `bside_unit`: JUCE-free code only (chip, program, trace, common dsp/engine/seq); links Catch2, not JUCE, so it builds fast. `BSideTests`: links `bs_test_core` (processor, parameters, state, presets, integration). `bside_render`: render regression (ctest label `render`). `bside_trace`: trace replay (label `trace`). `chip_bench`: benchmark, not a test. |
| host_smoke | One executable, a profile argument (`vintage`, `bside`) selecting the expected name, parameter count and program names. Tests `host_smoke` (unchanged behaviour) and `host_smoke_bside`. |
| VST3 validator | `vst3_validator` and `vst3_validator_bside`, same pass expression. |
| Optional (S1b measures) | Compile the JUCE modules for both test cores once, in one static library, if that saves ≥ 20 % of a cold build and changes no test result. |

#### 8.1.3 Gates for two plugins
| Gate | Extension (S1b) |
|---|---|
| `preflight mac` | Builds both plugins; `lipo` check of both binaries; ctest runs everything; pluginval strictness 10 on each bundle, one after the other. |
| `validator` | Runs on each bundle the mac step built. |
| `mac-sanitize`, `linux`, `linux-sanitize` | Sanitizer target lists add `bside_unit BSideTests bside_render bside_trace`; `inner.sh` builds all. |
| `docs` | Include rule for both instruments, the three new JUCE-free directories; the name check already covers new files. |
| CI `build.yml` | Every row builds both; pluginval, validator and bundle merge (Windows, Linux) per bundle; one artifact per plugin. Unverified until CI returns (listed in `CLAUDE.md`). |
| Iteration | `VS_PREFLIGHT_PLUGINS` = `vintage`, `bside` or `all` (default `all`) limits the mac step's pluginval/validator while iterating. The hand-over gate always runs `all`. |

S1b reports the gate time before and after (cold and warm). Target: the full default gate grows by
at most 50 % cold and 25 % warm; `bside_unit` does not link JUCE so its edit-build-test loop stays short.

### 8.2 S1a: shared processor skeleton (refactor, no behaviour change)

#### 8.2.1 What moves into `source/common/shell/`
From `source/vintage/PluginProcessor.*` (line references as of 5bc0710):

| Responsibility | Today | Shared as |
|---|---|---|
| Chunking at the prepared block size; MIDI parsed once per chunk; the exactly-once rule (negative offsets → 0, at/after the end → after the last sample, zero-length blocks); Program Change posted to the ApplyGate; player pedal tracking | `processInternal`, `parseChunk` | `BiTimbralProcessor::processInternal` |
| Key routing and the two arp/seq instances incl. pass-through detection, part selection, resync after a key-mode change, output capacities | `arpChunk`, `isPassThrough`, `partSelected`, `resyncArpAfterKeyModeChange`, `prepareToPlay` | same, unchanged logic |
| Host transport read per block and per chunk | `readTransport`, `transportAt` | same |
| Seqlock parameter read per block, held blocks, release request of a preset apply | `processInternal`, `releaseForPresetChange` | same; instrument part via hooks |
| Key-mode change: release, resync, mixer latch; Upper rendered only while needed (`kUpperIdleSamples`), resume from Whole | `processInternal`, `renderChunk` | same |
| Mixer with latch cuts, non-finite guard, chorus → delay → reverb, master gain × `kOutputTrim`, limiter, clamp, copy/fold to the host bus, guard reset | `renderChunk` | same |
| Host bypass (fade, panic, reset) | `processBlockBypassed` | same |
| Tail = synth tail (hook) + effect tails, capped | `getTailLengthSeconds` | same |
| Programs (host list), PresetManager ownership | header | same |
| State: tag, `stateVersion`, preset info child, migration hook, missing-parameter entries, writer lock | `get/setStateInformation`, `migrateState`, `addMissingParameters` | same; tag, version and migration from the instrument |
| Editor: preset bar above the generic editor | `PluginEditor.*` | `shell::InstrumentEditor` |
| Shared global parameters | `ParameterLayout.cpp`, `FxArpParams.*` | appenders in `common/params`: `addMasterVolume`, `addKeyMixParameters` (`key.mode`, `key.split_point`, `mix.balance`), `addFxParameters`, `addArpSeqParameters`, `addLimiterParameters`. Vintage calls them in today's order (master volume, parts, `global.vintage`, key/mix, effects, arp/seq, limiter), so its list is unchanged |
| Render-regression harness, `voice_bench` setup | `RenderHarness.cpp` (constructs `VintageSynthProcessor`) | harness takes a processor factory; Vintage scripts unchanged |

Instrument-specific (stays in each instrument): the `Part` type and its prepare/parameters, seeds,
the per-part parameter list and binding, instrument globals (Vintage amount), the synth tail, the
parameter layout, preset schema and factory bank, state tag and migrations, editor contents.

#### 8.2.2 Interface
```cpp
namespace vs::shell
{
/** What the skeleton needs from one part. Both instruments' parts satisfy it. */
template <class P>
concept PartEngine = requires (P& p, const P& c, float* out, int n, std::span<const NoteEvent> ev) {
    p.render (out, n, ev);        // host rate, mono, overwrites out[0..n); n == 0 applies the events only
    p.releaseAllNotes();          // key-mode change, preset apply, bypass: release, keep controllers
    p.reset();                    // silence now, forget everything
    p.resume();                   // Upper back from Whole: parameters at target, modulators restarted
    { c.isSounding() } -> std::same_as<bool>;
    { c.isSustainDown() } -> std::same_as<bool>;
    { c.getLatencySamples() } -> std::same_as<int>;
};

/** Every global the skeleton itself uses, read in the same seqlock read as the instrument's. */
struct SharedParams
{
    float masterDb, balance, limiterCeilingDb;
    bool limiterOn;
    KeyMode mode;
    int splitPoint;
    FxSettings fx;
    ArpSettings arp;
};

/** CRTP base. Derived provides (all noexcept except prepareParts):
      using Part = …;                                    // PartEngine
      using InstrumentParams = …;                        // trivially copyable: both parts' params + instrument globals
      static constexpr const char* kStateTag;  static constexpr int kStateVersion;
      static void migrateState (juce::ValueTree&);
      void readInstrumentParams (InstrumentParams&) const;   // atomic loads only
      void applyInstrumentParams (const InstrumentParams&);  // after a consistent read (setParams, setVintage, …)
      float partPan (const InstrumentParams&, int part) const;
      void prepareParts (double hostRate, int maxBlock, int osFactor, int realtimeOsFactor);  // may allocate
      double synthTailSeconds() const;                    // any thread, from parameter atomics
      Part& part (int p);  const Part& part (int p) const;  // 0 Lower, 1 Upper
*/
template <class Derived>
class BiTimbralProcessor : public juce::AudioProcessor { /* owns apvts, ApplyGate, EventParser, KeyRouter,
    2 × ArpSeq, PartMixer, Chorus, Delay, Reverb, SoftLimiter, master gain, bypass, transport, Upper activity,
    PresetManager; implements every AudioProcessor override listed in 8.2.1; keeps today's test hooks */ };
}
```
Hooks are called at most once per block or chunk, never per sample. `VintageSynthProcessor`
becomes `final : shell::BiTimbralProcessor<VintageSynthProcessor>` and keeps its public test API
(`getLowerPart`, `getBlockParams`, `kOutputTrim`, …) as thin forwards, so tests change at most in
includes and type names.

#### 8.2.3 Block algorithm (normative order; S1a may not reorder)
1. `ScopedNoDenormals`; back from bypass → `resetEngine`.
2. `blockStarted`; seqlock read of `SharedParams` + `InstrumentParams`; held → keep the previous set.
3. If consistent: `applyInstrumentParams`, master gain target, limiter ceiling and switch, effect
   settings, arp/seq settings per part (`partSelected`).
4. Transport for the block.
5. Release request of a preset apply → `releaseForPresetChange`, acknowledge.
6. Key-mode change → release both parts, resync both arp/seq, latch sounding parts; split point.
7. Upper resume rule; mixer targets.
8. Per chunk: parse, pedal tracking, route, arp/seq, render Lower (and Upper if active, else a
   zero-length render), mixer with latch cuts, input guard, chorus, delay, reverb, gain × trim with
   guard, limiter, clamp, copy or mono fold, guard reset, unlatch, Upper idle count.
9. (M7a) Publish the UI feed (9.1.4): reads only, after the last chunk of `processBlock` and at
   every exit of `processBlockBypassed` (peaks 0 there). Writes nothing the engine reads, so the
   audio is unchanged.

#### 8.2.4 Proof obligations (S1a gates)
1. **Bit-identical audio:** the raw `[.bitdump]` output of every render-regression script and the
   stress script, built before and after S1a on the same machine, at block sizes 512 and 61:
   identical bytes (`cmp`).
2. **Identical parameter list:** a dump of every parameter (index, ID, name, range, default, step
   count, display text at 0, 0.25, 0.5, 0.75, 1) before and after: identical text.
3. **Identical state and files:** `getStateInformation` for the defaults and for a seeded random
   parameter set: identical bytes; `tests/fixtures/state_m5.bin` loads to identical values; a
   program, tone and bank saved from a factory program: identical bytes.
4. All existing tests pass; edits to tests limited to includes and type names, listed per file;
   no test removed or weakened.
5. `host_smoke`, pluginval 10, VST3 validator, linux, both sanitizer runs; `voice_bench` and
   `fx_bench` within ±3 % of before (best of 3); identity, latency and tail unchanged.
6. Docs check: `source/common/shell` includes nothing from an instrument directory.

#### 8.2.5 As built (2026-11-14; where the code differs from 8.2.1–8.2.2, this subsection wins)
| Point | As built |
|---|---|
| Template | `shell::BiTimbralProcessor<Derived, PartT, InstrumentParamsT>` (the part and parameter types are template arguments: `Derived` is incomplete where the base declares its members) |
| Block parameters | `BlockParams : SharedParams, InstrumentParams`; an instrument's parameter struct must not reuse a shared field name (`masterDb`, `balance`, `limiterOn`, `limiterCeilingDb`, `mode`, `splitPoint`, `fx`, `arp`) |
| Construction | The base constructor takes the complete parameter layout, which **must** contain every shared parameter (master volume, key/mix, effects, arp/seq, limiter: use the shared appenders). `Derived` **must** call `initPresets (schema, bank, productFolder)` at the end of its constructor |
| Hooks in `Derived` (private, `friend Base`) | `kStateTag`, `kStateVersion`, `static migrateState`, `readInstrumentParams`, `applyInstrumentParams`, `partPan` (noexcept, once per block), `prepareParts (hostRate, maxBlock, osFactor, realtimeOsFactor)` (reads the parameters through `getBlockParams()`), `synthTailSeconds()`, `part (int)` |
| Editor | The base's `createEditor()` returns `shell::InstrumentEditor`; an instrument with its own editor overrides it (M7) |
| Render harness | Each render executable defines `makeDefaultInstance()`; `voice_bench` stays Vintage-specific |
| **Tightened in S1b (done)** | The plugin name comes from the instrument (`Derived::kName`). A missing shared parameter or a missing `initPresets` call throws `vs::ConfigError` naming what is missing, in every build type; the plugin entry point `shell::createPluginProcessor<T>()` reports it and aborts (a programmer error that fails on the first instantiation, so it cannot ship). The per-block hooks are `static_assert`ed `noexcept` |
| Build (S1b) | `VS_BUILD_BSIDE` (default ON); one function configures both plugin targets; the JUCE modules of both test cores are compiled once in `vs_juce_modules` with Vintage's definitions (a core that ever needs plugin-specific module behaviour must leave it); `scripts/sanitize-targets.sh` is the one list of sanitizer test targets, and a defined test target that is not listed fails the gate |

### 8.3 S1b: B-Side plugin shell
- `BSideProcessor : shell::BiTimbralProcessor<BSideProcessor>` with a placeholder part
  `TestTonePart`: one sine per note (VoiceAllocator, Poly, 16 voices), 5 ms linear attack and
  release, host rate, latency 0. Replaced in S2/S4; it exists so the whole chain (router, arp/seq,
  effects, limiter, presets, state) is tested from the first day.
- Parameters: the shared globals only, in this order: `master.volume`, `key.mode`,
  `key.split_point`, `mix.balance`, the 11 effect parameters, the 58 arp/seq parameters,
  `master.limiter`, `master.limiter.ceiling`. Same IDs, ranges and defaults as Vintage. **75 parameters.**
- Presets: instrument id `b-side`, 8 × 8 slots, extensions `.bsprog`, `.bstone`, `.bsbank`, user
  folder `Exitium/B-Side/`, master volume and limiter excluded from programs; factory bank: A1
  "Init" (`presets/bside/factory/A1-init.json`), every other slot Init.
- State tag `BSideState`, `stateVersion` 1.
- Gates: everything in 8.1.3 green; `host_smoke_bside` checks name, vendor, instrument flag, 75
  parameters, 64 programs, a rendered note; pluginval 10 and the VST3 validator on the B-Side
  bundle; `BSideTests`: state round-trip, bypass, guard, arp through the test tone, no allocation in
  `processBlock`; Vintage gates unchanged; gate time reported.

### 8.4 Chip model (contract for S2 and S3)

#### 8.4.1 Registers
| Address | Bits | Meaning |
|---|---|---|
| 0x00 + 7(n−1) | 8 | oscillator n frequency, low byte |
| 0x01 + 7(n−1) | 8 | frequency, high byte (16-bit value F) |
| 0x02 + 7(n−1) | 8 | pulse width, low byte |
| 0x03 + 7(n−1) | 4 (low) | pulse width, high bits (12-bit value PW) |
| 0x04 + 7(n−1) | 8 | control: bit 0 gate, 1 sync, 2 ring, 3 test, 4 triangle, 5 saw, 6 pulse, 7 noise |
| 0x05 + 7(n−1) | 8 | attack (high nibble), decay (low nibble) |
| 0x06 + 7(n−1) | 8 | sustain (high nibble), release (low nibble) |
| 0x15 | 3 (low) | cutoff, low bits (11-bit value FC) |
| 0x16 | 8 | cutoff, high bits |
| 0x17 | 8 | resonance (high nibble); bits 0–2: oscillator 1–3 through the filter; bit 3: external input (not modelled, 0) |
| 0x18 | 8 | bit 7: oscillator 3 off; bits 6/5/4: high-/band-/low-pass; low nibble: volume |
| 0x1B (read) | 8 | oscillator 3 waveform output, bits 11..4 |
| 0x1C (read) | 8 | oscillator 3 envelope counter |

The two analog input registers (0x19, 0x1A) are not modelled.

#### 8.4.2 Clocks, time and latency
| Clock | Value | Use |
|---|---|---|
| Chip clock (`chip.clock`, global) | 985 248 Hz ("50 Hz system") or 1 022 727 Hz ("60 Hz system") | oscillators, noise, envelopes |
| Frame | 19 656 or 17 095 cycles (50.1245 / 59.8261 Hz) | tick rate `Frame` |
| Internal rate | host rate × the oversampling factor of 4.4 (unchanged) | waveform rendering, filter, mixer |
| Tick | an integer number of cycles (8.6.1) | program engine |

- **Chip time is an integer.** Each part keeps a 64-bit cycle counter with a 32-bit fraction; per
  internal sample it advances by `round (clock × 2^32 / internal rate)`, computed once at prepare.
  No floating-point time in the digital path. A host-sample position (note-on, tick, block split)
  maps to the cycle at that sample. So the digital state and the ticks never depend on the block
  size, and the digital state at a given cycle never depends on the oversampling factor.
- **Closed form, not per cycle.** Per internal sample the digital state is advanced over the whole
  cycles that sample covers (about 10 at 96 kHz), computing in closed form every event inside the
  span (accumulator wrap, rising edges of the accumulator's top bit and bit 19, sync resets,
  envelope steps) and its exact cycle. Writes split spans at their cycle.
- **Latency:** the chip adds none. The voice path's latency is decimator + BLEP delay, measured as
  for Vintage (4.4). Look-ahead restart (S5) adds its own, reported.

#### 8.4.3 Digital state and the bit-exact rule
State per oscillator: 24-bit accumulator; 23-bit noise shift register; the edge-detection bits;
8-bit envelope counter; envelope phase (attack, decay/sustain, release); 15-bit rate counter;
exponential divider counter and period; written register values. Per chip: the two read registers.

- **Bit-exact rule:** fed the same register writes at the same cycles, the digital state at every
  cycle equals the reference emulator's, for both models, including the documented delays with
  which a write takes effect. A deviation is allowed only when listed in `docs/decisions.md` with
  the owner's approval.
- In musical use the same machinery runs on continuous inputs (Smooth: a frequency with a fraction
  below the accumulator's last bit, pulse width and cutoff as floats; the integer part follows the same
  rules). Nothing is compared to the chip then. With Authentic resolution
  every input is register-exact and the rule applies; this is proven by replaying the program
  engine's own register trace (8.9).

Reference behaviours (unit gates, from public technical documentation; where a detail is marked
"confirm", S2 takes it from the reference emulator and records it in `docs/decisions.md`):

| # | Behaviour |
|---|---|
| 1 | Accumulator: + F every cycle, wraps at 2^24. Output frequency = F × clock / 2^24. |
| 2 | Saw output = accumulator bits 23..12. |
| 3 | Triangle output = bits 22..11 of (accumulator XOR (top bit ? all ones : 0)). With ring on, the top bit used for the fold is the own top bit XOR the ring source's top bit. Superseded by 8.15 (8.15.2 (c): XOR NOT the source's top bit). |
| 4 | Pulse output = 0xFFF when accumulator bits 23..12 ≥ PW, else 0. The test bit forces it high (confirm). |
| 5 | Noise: 23-bit shift register clocked on every rising edge of accumulator bit 19; new bit 0 = bit 22 XOR bit 17; output bits 11..4 = register bits 20, 18, 14, 11, 9, 5, 2, 0; output bits 3..0 = 0. Timing (shift 2 cycles after the rise) and feedback: superseded by 8.15 (8.15.2 (a), 8.15.3). |
| 6 | Sync: a rising edge of the source's top bit resets the target's accumulator to 0 in the same cycle. Sources (also for ring): 1 ← 3, 2 ← 1, 3 ← 2. Chains (a source reset in the same cycle): confirm. Superseded by 8.15 (8.15.2 (d)). |
| 7 | Test bit: accumulator held at 0 while set; the noise register is reset (value and timing: confirm). Superseded by 8.15 (8.15.2 (a), 8.15.5: the register is not reset; it fills). |
| 8 | Combined waveforms (several waveform bits): the chip's combined output per model (8.4.5). A combination with noise also writes the combined output back into the noise register's output bits, so noise can lock up (silent) until the test bit resets it. |
| 9 | Envelope rate periods in cycles, nibble 0..15: 9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251. At about 1 MHz: attack 2 ms … 8 s; decay and release 3 × those. |
| 10 | Attack: counter + 1 per period up to 0xFF, then decay. Decay and release: − 1 per (period × divider); divider 1 above 0x5D, 2 from 0x5D, 4 from 0x36, 8 from 0x1A, 16 from 0x0E, 30 from 0x06 (exact threshold timing: confirm; the divider period is a state: superseded by 8.15.4); the counter stops at 0 and does not wrap. Sustain level = nibble × 0x11; lowering it during sustain lets the counter decay to the new level, raising it does not raise the counter. |
| 11 | Rate counter: 15 bits, compared for equality with the period and reset on a match. After a change to a period below the current count the envelope waits until the counter wraps (up to 32 768 cycles, about 33 ms): the **envelope delay quirk**. Superseded by 8.15.4 (32 767 states). |
| 12 | Gate: a rising edge starts attack, a falling edge starts release, from the current counter. |
| 13 | Read registers: 0x1B = oscillator 3 output bits 11..4; 0x1C = oscillator 3 envelope counter. Timing and the Model B latch: superseded by 8.15 (8.15.2 (c), 8.15.4). |
| 14 | Oscillator 3 off: oscillator 3 is not mixed to the output unless it is routed through the filter. |

#### 8.4.4 Output path (modelled, compared within tolerances)
```
oscillator output → waveform DAC → × envelope (envelope DAC) → filter or direct (routing bits)
→ mixer (+ voice DC) → × volume → board filter → note gain (velocity, unison) → part sum → decimator
```
| Stage | Model A | Model B |
|---|---|---|
| Waveform and envelope DACs | non-ideal bit weights (an unterminated ladder with a ratio of about 2.2): the converter kinks | near-ideal |
| Voice DC (output level at envelope 0) | large: a volume write steps the output (4-bit sample playback works) | small |
| Filter | 2-pole state-variable; non-linear integrators (distortion, stronger with several signals summed); cutoff curve non-linear in FC, shifted by `chip.variation` | 2-pole state-variable, cutoff close to linear in FC; clean |
| Resonance | 16 steps, per-model damping law | same |
| No waveform selected | the DAC input holds its last value and fades (the reference emulator's model) | same |
| Board filter | 1-pole high-pass about 16 Hz and 1-pole low-pass about 16 kHz (typical coupling and output network of a host board); superseded by 8.15.7 | same |

- `chip.volume_dc` Off: the voice DC is removed before the volume stage (volume writes do not
  click). On: as modelled. Never affects the digital state.
- All analog constants are fitted to the reference emulator's output (8.9), not copied from its
  source code (Q5).

#### 8.4.5 Waveform rendering and alias budgets
- Rendered at the internal rate as the band-limited form of the chip's analog waveform. Permitted
  means: BLEP/BLAMP from `common/dsp/BlepTable.h` at edges and resets; mip-mapped band-limited
  tables (one per octave, built at prepare time from the 4096-entry shape after the DAC) for
  shapes the DAC bends and for combined waveforms; closed-form integration.
- The 12-bit amplitude staircase is not rendered: its steps lie above 20 kHz for every frequency
  register value above about 50 (about 3 Hz).
- Discontinuities caused by writes (waveform switch, test bit, sync reset) are placed at their exact
  cycle as a sub-sample position and band-limited.
- Envelope steps are applied per internal sample and not band-limited: 8-bit steps at up to about
  110 kHz are the chip's own content (the attack snap). Exempt.
- Combined waveform shapes come from a parametric model per chip model, fitted to the reference
  emulator (Q5).

**Alias budgets** (they redefine the alias gate for B-Side only; Vintage's gate is unchanged;
owner approval Q6). Aliasing is measured as for Vintage: non-harmonic energy relative to the
fundamental, band 0 to min(20 kHz, 20/48 × host rate), every rate configuration of 4.4.

| Signal | Budget |
|---|---|
| Triangle, saw, pulse, incl. sync and ring, Model B DAC | ≤ −100 dB up to a 3.85 kHz fundamental (the register maximum at 985 248 Hz) |
| Model A DAC, combined waveforms | ≤ −96 dB up to 2 kHz, ≤ −80 dB up to 3.85 kHz |
| Write discontinuities (a waveform switched every tick, a test-bit reset every tick) | ≤ −90 dB up to 2 kHz |
| Noise | no alias gate; third-octave spectrum within ±1.5 dB of the reference emulator's, 50 Hz–16 kHz, at 8 frequency settings |
| Envelope steps | exempt (8.4.5) |

Configurations the gate does not measure must be listed in the S2 report (engineering rule).

#### 8.4.6 Resolution (`chip.resolution`, per part)
| Input | Smooth (default) | Authentic |
|---|---|---|
| Pitch | continuous (accumulator with fraction); up to 12 kHz fundamental (alias budgets measured to 3.85 kHz) | 16-bit register `round (f × 2^24 / clock)`, clamped to 1 … 65 535 (notes above the maximum play at it) |
| Pulse width | continuous 0 … 4095 | integer |
| Cutoff | continuous 0 … 2047 | integer |
| Host modulation (bend, vibrato, glide, automation, macros) | smooth | smooth, then rounded to the register |

Program-driven values step at the tick in both modes (8.6).

#### 8.4.7 Interfaces (built in S2; names may differ, semantics may not)
```cpp
namespace vs::bside::chip
{
enum class Model : std::uint8_t { A, B };

struct Config
{
    Model model = Model::B;
    std::uint32_t clockHz = 985248;
    double internalRate = 96000.0;
    float variation = 0.5f;      // Model A filter curve offset, 0..1
    bool volumeDc = false;
    bool authentic = false;      // register resolution
};

/** One oscillator's input as the program engine sets it. Authentic: integer values only. */
struct OscInput { double frequency; float pulseWidth; std::uint8_t control, attackDecay, sustainRelease; };
struct FilterInput { float cutoff; std::uint8_t resonance, routing, modeVolume; };

struct DigitalState { /* 8.4.3; operator== compares every field */ };

class Chip   // JUCE-free; allocates only in prepare
{
public:
    void prepare (const Config&);
    void reset() noexcept;                                          // power-on state, cycle 0
    void write (std::uint8_t reg, std::uint8_t value) noexcept;     // at the current cycle (trace replay)
    void setOscillator (int n, const OscInput&) noexcept;           // at the current cycle (program engine)
    void setFilter (const FilterInput&) noexcept;
    std::uint8_t read (std::uint8_t reg) const noexcept;            // 0x1B, 0x1C
    void advanceCycles (std::uint64_t cycles) noexcept;             // digital state only (tests)
    void render (float* out, int numInternalSamples) noexcept;      // adds the output after volume, before the board filter
    DigitalState state() const noexcept;
    std::uint64_t cycle() const noexcept;
};
}
```
The filter object is a separate class (`chip/Filter.h`) that the chip uses by reference, so a
`Part` can own the filter instances and lend one to several chips (shared topology, 8.5).

### 8.5 Polyphony mapping
- **One note = one chip** (default layout): each note voice of a part owns a whole chip. Its three
  oscillators are set by the tone's oscillator parameters and lanes (8.6), so one note can be three
  layered, synced or ring-modulated oscillators through one filter. 16 note voices per part
  (`kMaxVoices`): up to 16 chips per part, 32 in Dual.
- **Allocation:** `common/engine/VoiceAllocator` as is: Poly, Unison, Solo, pedal, 3 ms steal fade,
  idle-longest. Unison: n chips run the same program with the detune spread, 1/√n level, and the
  phase pattern of 3.2 applied as accumulator start offsets (both resolutions).
- **Idle:** a note voice is idle when, after its note-off, all three envelope counters are 0 in
  release and no wave lane can gate again (every wave lane ended); or 30 s after its note-off
  (then faded out over 3 ms). Then the allocator gets `voiceIdle`. Idle voices and oscillators
  with waveform Off, envelope 0 and no lane activity cost no CPU.
- **Filter topology** (`filter.topology`, S3): Per Note (default): each chip's own filter. Shared:
  one filter per part; every note's filter-routed signal is summed into it (Model A's summed
  distortion); cutoff, resonance and mode come from the most recently started note's lanes plus the
  host parameters; direct signals and volume stay per note. `Part` owns the filters from S2 on.
- **Velocity:** a per-note gain after the chip, 1 − amount × (1 − velocity) (`vel.volume`),
  smoothed over 5 ms on a retrigger; `vel.cutoff` lowers the cutoff by
  amount × (1 − velocity) × 1024 register units.
- **Pitch of oscillator n** = note + glide + bend + vibrato + `oscN.coarse`/`oscN.fine` + unison
  detune + the wave lane's relative pitch; an absolute lane pitch (8.6.3) replaces all of these.
- **CPU (S4 gate):** 16 note voices, all three oscillators sounding (pulse, saw, a combined
  waveform), filter on, Model A, 48 kHz (2×): **≤ 5 % of one core**; 32 (Dual) ≤ 10 %; with the
  effects ≤ 15 %; at 192 kHz (1×) within 15 %. `chip_bench`, non-LTO, best of 3.
- S5 adds the hardware layouts (8.11): Shared Chip and chord-into-one-voice.

### 8.6 Patch program engine

#### 8.6.1 Tick clock
| `tick.rate` | Tick period P in cycles (integer) |
|---|---|
| Frame (default) | round (frame cycles / `tick.mult`); frame = 19 656 or 17 095 by `chip.clock` |
| Free | round (clock / (`tick.free_hz` × `tick.mult`)) |
| Tempo | round (clock × 60 / (bpm × `tick.sync` × `tick.mult`)); host tempo, or `arp.tempo` when the transport is stopped |

- P ≥ 256 cycles (rates above about 3.8 kHz are clamped).
- Every note voice has its own tick clock: tick 0 at its note-on cycle (after the steal fade for a
  stolen voice), each following tick P cycles later. P is read at block start; a change applies
  from each voice's next tick.
- Ticks split rendering at their exact cycle like MIDI events: output never depends on block size.

#### 8.6.2 Lanes and rows
| Lane | Count | Writes |
|---|---|---|
| `wave1`–`wave3` | one per oscillator | control bits, pitch, envelope registers |
| `pulse1`–`pulse3` | one per oscillator | pulse width |
| `filter` | one | cutoff, resonance, mode, routing, volume |

- A lane is up to **64 rows**. A row is one or more commands and a delay (ticks before the next row,
  0–255, default 1). At each tick a lane executes rows from its position until it has executed a
  row with delay ≥ 1, a `sweep`, a `pingpong` or an `end`. A lane executes at most 64 rows in one
  tick: the reader rejects zero-delay loops, and the runtime cap stops the lane as a backstop.
- Order within a tick: restart writes, `wave1`–`wave3`, `pulse1`–`pulse3`, `filter`. All writes of
  a tick are applied at the tick's cycle in that order (a register trace lists them with equal
  cycles, in that order).
- **Ownership:** a register a lane has not written for this note follows the host parameters
  (smoothly). After a lane writes it, the lane's value holds for the rest of the note; the macros
  (8.7) still apply on top.
- **Note-off:** at the note-off sample (not the next tick), each lane with a `release` label jumps
  there and executes its rows. An oscillator whose wave lane has no `release` label has its gate
  cleared at that sample. Ticks continue during the release.
- No table, or an empty lane: the note follows the host parameters (gate on at note-on for each
  oscillator whose `oscN.wave` is not Off, off at note-off).
- Program values step at the tick, unsmoothed; in Authentic they are register values.
- Deterministic: no randomness in lanes; same events, parameters and tables → same output.

#### 8.6.3 Commands
| Lane | Command | Effect |
|---|---|---|
| wave | `wave <bits> [<pitch>]` | sets the control register (gate only if `gate` is in `<bits>`) and optionally the pitch |
| wave | `pitch <pitch>` | sets the pitch, control bits unchanged |
| wave | `gate on` / `gate off` | sets or clears the gate bit only |
| wave | `adsr <a> <d> <s> <r>` | four hex digits 0–f: the envelope registers |
| pulse | `set <0..4095 or n%>` | pulse width |
| pulse | `sweep <±d> x<n>` | n ticks (1–255), adds d at each (the first at the row's own tick), clamped 0–4095 |
| pulse | `pingpong <lo> <hi> <d>` | moves by d per tick between lo and hi, reversing at the limits, until a release jump |
| filter | `cutoff <0..2047>`, `sweep <±d> x<n>` | cutoff; sweep as above, clamped 0–2047 |
| filter | `res <0..15>`, `volume <0..15>` | resonance, volume |
| filter | `mode <m>` | filter mode bits; m = `off`, `lp`, `bp`, `hp`, `lp+bp`, `bp+hp`, `lp+hp`, `lp+bp+hp` |
| filter | `route <r>` | oscillators through the filter; r = `none` or digits 1–3, e.g. `route 13` |
| all | `<label>:` | names the next row; `release` is the note-off entry |
| all | `jump <label>` | continues at the label (same tick) |
| all | `repeat <label> <n>` | jumps to the label n times (1–255), then falls through; the count restarts after falling through |
| all | `end` | the lane stops; its last values hold |

- `<bits>`: `none`, or tokens joined by `+`: `tri`, `saw`, `pulse`, `noise`, `gate`, `sync`, `ring`, `test`.
- `<pitch>`: relative semitones with up to two decimals (`+7`, `-12`, `+0.15`); absolute `=<note>`
  (`=C#6`, `=61`; ignores key, glide, bend, coarse/fine and unison detune); `~` (unchanged);
  `key1`–`key3` (the held keys, lowest first; S5, rejected before).
- Several commands in one row are separated by `,` and execute in order; a flow command (`jump`,
  `repeat`, `end`), `sweep` or `pingpong` must be the last in its row; `sweep` and `pingpong` rows
  take no `ticks`.

#### 8.6.4 Hard restart
`[restart]` section keys: `mode off|classic|phase` (default off), `lead <1–8>` ticks (default 2),
`ad <hh>`, `sr <hh>` (hex, default 00 00), `gateoff <1–8>` ticks (default = lead), `firstwave <bits>` (optional).

- **Restart writes:** gate off and envelope registers := `ad`/`sr` on all three oscillators
  (`phase`: test bit set as well). At the note's tick 0: if `firstwave` is set, control :=
  `firstwave` plus gate, the tone's own envelope values, and each wave lane's row 0 runs at tick 1;
  otherwise the envelope values and row 0 at tick 0. `phase` clears the test bit at tick 0, so
  accumulators and noise start from their reset state.
- **Policies** (`restart.policy`, per part):

| Policy | Behaviour |
|---|---|
| Clean (default) | As Precondition; and when a note starts on a voice that did not get the full lead (retrigger, steal, no restart section), the rate counter and divider are set to 0 directly (not a chip operation): the envelope never waits. |
| Precondition | An idle voice gets the restart writes as soon as it is idle and stays in that state; a note on it then starts normally. A voice re-used sooner may show the envelope delay quirk. |
| Authentic | No preconditioning and no reset: only what the table's rows do. |
| Look-ahead (S5) | Notes are delayed by `gateoff` ticks so the restart runs before each note; the latency is reported. |

- A stolen voice is faded over 3 ms (3.2), then: Clean and Precondition reset the chip to its
  power-on state and apply the restart writes; Authentic does not reset it.

#### 8.6.5 Text format
A table is UTF-8 text (as a file: extension `.bstable`; in presets: one JSON string per line, 8.8).
```
table     = header *line
header    = "bside-table 1" EOL                       ; first line that is not blank or a comment
line      = [ws] [statement] [ws] [comment] EOL
comment   = "#" *(any character except EOL)
statement = section / setting / label / row
section   = "[" ("wave1" / "wave2" / "wave3" / "pulse1" / "pulse2" / "pulse3" / "filter" / "restart") "]"
setting   = key ws value                               ; only inside [restart]
label     = name ":"                                   ; name = [a-z][a-z0-9_]{0,15}; "release" is reserved
row       = command *([ws] "," [ws] command) [ws "ticks" ws 1*3DIGIT]
```
- Keywords are lower case; note names are case-insensitive (`c#6`, `Db4`, middle C = C4 = 60).
  Numbers are decimal, or hex where stated (`adsr`, `ad`, `sr`).
- **Limits** (the reader is a bounded parser of untrusted input, like the strict preset reader):
  text ≤ 32 KB, ≤ 1024 lines, ≤ 200 bytes per line, ≤ 64 rows and ≤ 16 labels per lane, each
  section at most once, every number within its stated range (out of range is an error, not
  clamped), linear time, no recursion.
- **Errors:** the table is rejected as a whole; up to 8 messages are reported, each
  `line L, column C: <what> (expected <what>)`, then "and N more". Checked: syntax, unknown
  command or command in the wrong lane, ranges, undefined or duplicate label, a jump target in
  another lane, a loop of rows that all have delay 0, a command after a flow command in one row.
- **Writer:** canonical form (fixed spacing, lower case, `ticks` only when ≠ 1, hex digits lower
  case, comments not kept). `write (read (t))` reads back to the same compiled program, and writing
  twice gives the same text.

#### 8.6.6 Example tables
Snare (oscillator 1; envelope set by the table):
```
bside-table 1
[restart]
mode classic
lead 2
[wave1]
    adsr 0 9 0 9, wave noise+gate =C#7
    wave pulse+gate =A3
    wave pulse+gate =F3
    wave noise+gate =D6 ticks 4
    wave noise =D6        # gate off: the release rings out
    end
[pulse1]
    set 2048
```
Arpeggiated chord in one oscillator (major triad, one note per tick; keeps cycling in the release):
```
bside-table 1
[wave1]
    wave pulse+gate +0 ticks 0
loop:
    pitch +0
    pitch +4
    pitch +7
    jump loop
release:
    gate off ticks 0
    jump loop
[pulse1]
    set 25%
```
Pulse-width lead (ping-pong width, stepped vibrato after 12 ticks; oscillator 2 from the host
parameters, e.g. a detuned saw):
```
bside-table 1
[restart]
mode phase
lead 2
[wave1]
    wave pulse+gate +0 ticks 12
vib:
    pitch +0.12 ticks 2
    pitch -0.12 ticks 2
    jump vib
[pulse1]
    set 512
    pingpong 512 3584 48
```
Filter bass (saw through the low-pass, resonant sweep down per tick):
```
bside-table 1
[restart]
mode phase
lead 2
ad 00
sr 00
[wave1]
    adsr 0 9 a 9, wave saw+gate +0
    end
[filter]
    route 1, mode lp, res 12, volume 15, cutoff 1800
    sweep -96 x16
    end
```

#### 8.6.7 Real time and threads
- Reading and compiling run on the message thread or the host's state thread, never on the audio
  thread. The compiled program is a fixed-size, trivially copyable struct of at most 8 KB (rows
  pre-resolved: jump targets as indices, numbers in register units).
- Per part, three program buffers. The writer fills a buffer that is neither the published one nor
  the one the audio thread reports in use (an atomic it stores at block start), then publishes its
  index **inside the ApplyGate hold**, so a block reads parameters and table as one consistent set
  (the seqlock read includes the index).
- At note-on a voice copies the program into its own storage (≤ 8 KB memcpy), so a later apply never
  changes a sounding note and no lifetime tracking is needed.
- Lane state per voice is plain data (row index, delay counter, repeat counters, sweep and
  ping-pong state). Per tick the work is bounded by the row cap. No allocation, locks or I/O.

### 8.7 Parameters
Per-part parameters exist as `lower.<suffix>` and `upper.<suffix>`; they are added in one serial
step at the start of S2, after the 75 globals of S1b, in this order: all of `lower.*`, all of
`upper.*`, then `chip.clock`. Behaviour not yet built in S2 is inert until its milestone
(listed). Display text and DSP share `bside/params/PartMappings.h`. Continuous parameters are
smoothed; switches are not.

Per part (63), N = 1, 2, 3, the three `oscN.*` blocks in order:

| ID suffix | Name | Range (default) | Built in |
|---|---|---|---|
| `oscN.wave` | Osc N Wave | Off, Triangle, Saw, Pulse, Noise, Tri+Saw, Tri+Pulse, Saw+Pulse, Tri+Saw+Pulse (osc 1 Pulse, osc 2–3 Off) | S2 |
| `oscN.coarse` | Osc N Coarse | −24 … +24 st (0) | S2 |
| `oscN.fine` | Osc N Fine | −100 … +100 ct, continuous (0) | S2 |
| `oscN.pw` | Osc N Pulse Width | 0 … 100 % → 0 … 4095 (50 %) | S2 |
| `oscN.attack` | Osc N Attack | 0 … 15, shown as the nominal time (0) | S2 |
| `oscN.decay` | Osc N Decay | 0 … 15 (0) | S2 |
| `oscN.sustain` | Osc N Sustain | 0 … 15 (15) | S2 |
| `oscN.release` | Osc N Release | 0 … 15 (6) | S2 |
| `oscN.sync` | Osc N Sync | Off, On (Off) | S2 |
| `oscN.ring` | Osc N Ring Mod | Off, On (Off) | S2 |
| `oscN.filter` | Osc N Filter | Off, On (On) | S3 |
| `osc3.mute` | Osc 3 Mute | Off, On (Off) | S3 |
| `filter.cutoff` | Filter Cutoff | 0 … 2047 register units, continuous (1024) | S3 |
| `filter.resonance` | Filter Resonance | 0 … 15 (0) | S3 |
| `filter.mode` | Filter Mode | Off, Low, Band, High, Low+Band, Band+High, Low+High, All (Low) | S3 |
| `filter.key_follow` | Filter Key Follow | 0 … 100 % (0) | S3 |
| `filter.topology` | Filter Topology | Per Note, Shared (Per Note) | S3 |
| `chip.model` | Chip Model | Model A, Model B (Model B; Q3) | S2 (digital), S3 (analog) |
| `chip.variation` | Chip Variation | 0 … 100 % (50 %); Model A filter curve | S3 |
| `chip.volume` | Chip Volume | 0 … 15 (15) | S3 |
| `chip.resolution` | Resolution | Smooth, Authentic (Smooth) | S2 |
| `chip.volume_dc` | Volume DC | Off, On (Off) | S3 |
| `restart.policy` | Restart Policy | Clean, Precondition, Authentic (Clean) | S2 |
| `tick.rate` | Tick Rate | Frame, Free, Tempo (Frame) | S2 |
| `tick.free_hz` | Tick Free Rate | 10 … 400 Hz, exponential (50) | S2 |
| `tick.mult` | Tick Multiplier | 1 … 8 (1) | S2 |
| `tick.sync` | Tick Sync | 6, 12, 24, 48 per beat (24) | S2 |
| `macro.pw_offset` | PW Offset | −50 … +50 % → ±2048 (0) | S4 |
| `macro.cutoff_offset` | Cutoff Offset | −100 … +100 % → ±2047 (0) | S4 |
| `macro.sweep_depth` | Sweep Depth | 0 … 200 % (100 %) | S4 |
| `vel.volume` | Velocity → Volume | 0 … 1 (0.5) | S4 |
| `vel.cutoff` | Velocity → Cutoff | 0 … 1 (0) | S4 |
| `vibrato.rate` | Vibrato Rate | 0.5 … 12 Hz (5.5) | S4 |
| `mod.wheel_vibrato` | Mod Wheel Vibrato | 0 … 1 → 0 … 100 ct (0.3) | S4 |
| `assign.mode` | Assign Mode | Poly, Unison, Solo (Poly) | S4 |
| `assign.unison_voices` | Unison Voices | 2, 4, 8, 16 (4) | S4 |
| `unison.detune` | Unison Detune | 0 … 1 → 0 … 50 ct (0.3) | S4 |
| `porta.time` | Portamento Time | 0 … 1 (0), as Vintage | S4 |
| `porta.mode` | Portamento Mode | Always, Legato (Always) | S4 |
| `bend.range` | Bend Range | 0 … 12 st (2) | S4 |
| `mix.pan` | Pan | −1 … +1 (0), as Vintage | S4 |

Global, appended last: `chip.clock` (Chip Clock: 50 Hz system, 60 Hz system; default 50 Hz system), S2.

**Host parameter count after S2: 75 + 2 × 63 + 1 = 202.**

- **Automatable:** every host parameter. **Macros over table data:** `tick.*` (speed of every
  lane), `macro.pw_offset`, `macro.cutoff_offset` (added after the lane value, smoothed),
  `macro.sweep_depth` (scales every `sweep` and `pingpong` step), `restart.policy` (how the table's
  restart section is used).
- **Patch data, not automatable:** the table (lanes and restart section). Saved in the state and
  in presets (8.8).
- Envelope displays: attack 2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800 ms, 1, 3, 5, 8 s;
  decay and release 3 × those. Cutoff is displayed in register units until S6 shows Hz.
- Before v1.0 `stateVersion` stays 1; follow the `add-parameter` skill.

### 8.8 Presets and state
- **Program file:** as 6b.2, plus `"tables": { "lower": [ "<line>", … ], "upper": [ … ] }`. **Tone
  file:** plus `"table": [ … ]`. Missing or empty → no table. A table that fails the reader rejects
  the whole file, with the table's message prefixed by part and program name.
- **Common change (S1c):** `presets::Tone` gets `std::vector<std::string> data` (lines) and
  `Program` one such vector per tone; `Instrument` gets an optional validator
  (`std::function<juce::String (const std::vector<std::string>&)>`) and the file/JSON limits.
  Vintage sets none: its files stay byte-identical (tested).
- **Limits:** the instrument's file cap and JSON value cap are set so the largest legitimate bank
  (64 programs, two maximal tables each) fits; tested like Vintage's "largest legitimate bank".
- **Session state:** the parameters plus a child `Tables` with text properties `lower` and `upper`
  (canonical form). Read in `setStateInformation` without file I/O; a malformed table gives that
  part an empty table and an error the editor shows; never a crash.
- Applying a program or tone publishes its tables inside the same ApplyGate hold as its parameters.
  A table counts as program content (editing it marks the program modified, S6).

### 8.9 Tests and authenticity

#### 8.9.1 Layers
| Layer | What | Needs reference data |
|---|---|---|
| Digital unit gates | every row of 8.4.3; closed form = a naive **per-cycle stepper** written by the test agent from 8.4.3 alone (superseded by 8.15: from 8.4.3 and 8.15), for 10^8 random cycles with random writes, both models | no |
| Program engine | every command and rule of 8.6; tick timing exact to the cycle; golden write traces (8.9.6) | no |
| Trace replay | our chip against the reference emulator: digital state exact at checkpoints, read registers exact, output within 8.9.4 | yes |
| Analog | filter curves, resonance, Model A distortion, DAC kinks, voice DC, noise spectrum | yes |
| Alias | budgets of 8.4.5 | no |
| Golden renders | render-regression features for B-Side scripts (shared harness) | no |
| Robustness | block sizes 1/7/61/512 (digital state and ticks exact, audio difference below −120 dB), determinism, no allocation, sanitizers, state round-trip, table reader fuzz (10^6 mutated tables: no crash, bounded time) | no |
| CPU | `chip_bench` (8.5) | no |
| By ear | the owner, against public recordings of the original hardware (never downloaded into the repository) | — |

#### 8.9.2 Trace file (`.bstrace`)
```
bside-trace 1
clock 985248                # chip clock, Hz
model A                     # A or B
check 4096                  # optional: compare the state every N cycles (and always after each write)
# <cycle> <register hex> <value hex>; cycles ascending, equal cycles apply in file order
0 18 0f
0 05 09
40 04 11
end 2000000                 # last cycle simulated
```
Bounded reader: ≤ 16 MB, ≤ 10^6 writes, ≤ 2^40 cycles, ≤ 64 bytes per line, strict syntax.

#### 8.9.3 Reference files
`.bsref` (text): the trace's SHA-256, the generator's identity as commit hashes only (adapter,
reference emulator), then one line per checkpoint:
```
state <cycle> <acc1> <acc2> <acc3> <noise1> <noise2> <noise3> <env1> <env2> <env3>
      <phase1> <phase2> <phase3> <rate1> <rate2> <rate3> <div1> <div2> <div3> <rd1B> <rd1C>
```
(hex; one line in the file; field meanings and power-on values: 8.15.1). `.bsspec`: the trace's
output at 48 kHz reduced to the render-regression feature format, plus a 1/12-octave averaged
spectrum per marked segment for the analog measurements. Never audio.

#### 8.9.4 Replay and tolerances
The driver resets our chip (model and clock from the trace), advances to each write's cycle,
writes, and compares the state at every checkpoint. For audio it renders at 192 kHz internal,
decimates to 48 kHz, estimates one constant delay against the reference by cross-correlation
(within ±256 samples), then measures both signals the same way.

| Quantity | Tolerance |
|---|---|
| Digital state, read registers | exact |
| Filter −3 dB point at 16 cutoff values, resonance 0 | ±5 % (Model A at the reference's default curve) |
| Resonance peak height, resonance 0–15 | ±1 dB |
| Model A distortion: harmonics 2–5 at three drive levels | ±3 dB |
| Voice DC step on a volume write (Model A) | ±1 dB |
| Noise third-octave spectrum | ±1.5 dB |
| Scripted traces: band energy per third-octave per 25 ms frame | ±3 dB per cell with ≤ 5 % outliers, mean deviation ≤ 1 dB; level ±0.5 dB (quiet cells and band 22: superseded by 8.15.7) |

#### 8.9.5 Reference emulator (oracle) workflow — needs owner approval (Q4)
- Never linked into, vendored in or shipped with the plugin; no CMake target of this repository
  builds or downloads it.
- A small **adapter** program (reads `.bstrace`, drives the reference emulator cycle by cycle,
  writes `.bsref` and `.bsspec`) lives outside this repository, under the reference emulator's
  licence. This repository holds only the neutral formats, our replay driver and comparer, and the
  generated reference data.
- Generation is a deliberate developer step on the development Mac (a dispatch-only CI workflow
  can come later): the reference emulator pinned by commit and archive SHA-256, the adapter by
  commit; `tests/bside/reference/MANIFEST` records hashes only (no names, no URLs). The location
  is configured outside the repository (`BS_ORACLE` = path to the built adapter).
- Regenerating is reviewed like a golden re-bless. `bside_trace` uses committed data only; a test
  without data reports itself skipped, never passed.

#### 8.9.6 Golden write traces
The program engine's register trace (Authentic, 50 Hz system) for canonical tables — snare, kick,
hi-hat, three-note chip arpeggio, pulse ping-pong, filter sweep, each restart mode under each
policy — at `tick.mult` 1, 2 and 4, compared as exact text (`tests/bside/golden/*.bstrace`). The
same traces are replay inputs, which proves the program engine and the chip together.

### 8.10 Milestones S1–S4
| Step | Scope | Acceptance gates | Agents (model) | Depends on |
|---|---|---|---|---|
| **S1a** | Shared skeleton `common/shell/` (8.2); Vintage on it; render harness and `voice_bench` take a processor factory | 8.2.4 in full; `realtime-review` | implementer **opus**; proof scripts and verifier **sonnet**; fresh reviewer **opus** | no other agent editing Vintage's processor or editor |
| **S1b** | B-Side plugin shell (8.1, 8.3); gates and CI for two plugins | 8.3 gates; 8.1.3; gate time reported | implementer **sonnet**; reviewer **sonnet** | S1a |
| **S1c** | Table reader/writer and compiler (8.6.5, without runtime), trace/reference formats and comparer (8.9.2–8.9.4) against a stub chip, preset model extension (8.8), docs of the adapter interface | reader spec tests incl. every error class and the 4 examples; 10^6-case fuzz under sanitizers; round-trip; Vintage preset files byte-identical; trace reader bounds; reviewer's fuzz ported in | implementer **sonnet**; spec test agent **sonnet**; reviewer **opus** (untrusted input) | JUCE-free parts: none (parallel to S1a). Preset extension: after S1a |
| **S2** | Parameter step (8.7); chip digital core (8.4.2–8.4.3); band-limited waveforms (8.4.5); tick clock, lanes, restart and policies (8.6); Authentic resolution; replay of golden write traces | 8.4.3 rows and stepper equivalence; trace replay digital = exact (when Q4 data exists); alias budgets; golden write traces; block-size/oversampling independence of the digital state; no allocation; `realtime-review` | S2a digital core **opus**; S2b waveform rendering **opus**; S2c program engine **sonnet** + review **opus**; stepper and spec tests **sonnet** (not the implementers); parameter step **sonnet** | S1b, S1c. S2a ∥ S2c (S2c writes to a recording chip stub); S2b after S2a's interface |
| **S3** | Model B filter, then Model A with variation; DACs, voice DC, mixer, volume, board filter; filter topology | 8.9.4 analog tolerances; filter stable at every setting and rate; block-size independent below −120 dB | filters **opus**; output stage **sonnet** | S2a (Model B filter can start alone, parallel to S2) |
| **S4** | Polyphony with the allocator (8.5); macros; effects/arp from the shell; 10–20 reference patches (drums, chip arpeggio, pulse lead, filter bass, sync/ring leads, Model A pads); factory level rule as 6b.4; audition render with a contact sheet | `chip_bench` budget; render-regression goldens for 6 patches; level rule; pluginval, validator, sanitizers, linux; **owner audition** | integration **sonnet**, patch drafting **sonnet**, review **opus** | S2, S3 |

- Owner points: Q1–Q8 before S1b/S2 (8.12); Q4 before any reference data exists; an **optional
  first listen at the end of S2** (raw chip, no filter: does it sound like the chip?); a listen to
  Model A and B filter sweeps at the end of S3; the S4 audition gate; UI direction before S6.
- At most one agent edits a given processor; the parameter step is one serial step.

### 8.11 Later: S5 and S6 (outline; own contract before they start)
| Step | Scope |
|---|---|
| S5 | Shared Chip layout (one chip per part, up to three notes, one per oscillator, shared filter; an allocator voice limit ≤ 16); chord-into-one-voice (`key1`–`key3`); Look-ahead restart (latency = `gateoff` ticks, changed only at prepare); kit mode (key ranges → sub-tables); optional global tick phase |
| S6 | Table editor UI, Hz display of the cutoff, presets and factory bank, release together with Exitium Vintage Synth's M7–M8 |

### 8.12 Owner decisions (2026-11-14)
| Q | Decision |
|---|---|
| Q1 | Plugin identity: code `Bsy1`, bundle ID `com.exitium.bside` — approved |
| Q2 | Two parts (Lower/Upper, Whole/Dual/Split) like Vintage |
| Q3 | Default chip model for Init: Model B |
| Q4 | Reference emulator approved as a developer tool: downloaded and built outside the repository, never linked or shipped; the adapter lives outside the repository; generated reference data (state checkpoints and spectral features, no audio) may be committed. **Choose the one regarded as the most faithful replica of the chip.** |
| Q5 | Analog constants and combined waveforms fitted by our own model, never copied |
| Q6 | B-Side alias budgets of 8.4.5 approved, with the instruction to mimic the original chip as closely as possible: authenticity outranks cleanliness wherever the two conflict in Authentic settings |
| Q7 | Note-off handled at its sample |
| Q8 | Default restart policy: Clean |


### 8.13 Amendments from S1c (as built, 2026-11-14; these win over 8.6.5 and 8.9.2–8.9.4 where they differ)
| Section | Amendment |
|---|---|
| 8.6.5 | A row ending in `jump` or `repeat` costs no tick (default delay 0; `ticks N` allowed). **No cycle of delay-0 rows is accepted, `repeat` edges included.** Guarantee: an accepted table, and any `Program` that `validate()` accepts, runs **at most 64 rows per tick per lane** from any start row and any repeat-counter state. A note-off on the same sample as a tick can run two such bursts back to back, so the runtime budgets 2 × 64 rows; its own cap stays as a backstop |
| 8.6.5 | At most 512 commands per table. **The canonical text of everything accepted must itself be acceptable:** the reader rejects a row whose canonical form exceeds 200 bytes and a table whose canonical text exceeds 32 768 bytes; `validate()` enforces the same plus at most 16 jump targets and `release` per lane. Property under test: accept ⇒ write ⇒ read ⇒ identical program bytes |
| 8.6.5 | Text rules: UTF-8 only; C0 and C1 control characters and a byte order mark are rejected with their own messages; `#` belongs to a note name only directly after `=` and a note letter, otherwise it starts a comment; leading zeros do not count toward digit limits (`ticks` keeps 1–3 digits). The compiled program stores no label names: the writer generates them, so an editor must keep the user's own text |
| 8.9.2 | Trace limits: **at most 100 000 writes and 49 999 periodic checkpoints** (150 000 checkpoints in all), replacing "10^6 writes". 64 bytes per statement (the bytes before `#`), 256 per line including a comment. Header: `clock` 1 000–100 000 000 Hz; `model` exactly `A` or `B`; header words in any order before the first write, each at most once; `end` last. Register and value are exactly two hex digits; writable registers 0x00–0x18; cycles decimal, non-decreasing, ≤ 2^40 |
| 8.9.3 | The reference and spec grammars are those of `source/bside/trace/ReferenceFormat.h` (20 state fields with ranges; hashes of 64 hex digits for the trace and 40 or 64 for commits; `frame` and `segment` lines; 16 MB file limit). **Every writer refuses output its reader would reject** |
| 8.9.4 | Checkpoint order: after every write; periodic checkpoints strictly before `end`, taken before the writes of that cycle; one at `end`. "Level ±0.5 dB" is the overall level of the render. The replay driver verifies the trace's SHA-256 and the schedule. Delay estimation is a helper for the side that holds audio (the repository holds none) |

### 8.14 Amendments from S1d (as built, 2026-11-14; these win over 8.6.7 and 8.8 where they differ)
| Section | Amendment |
|---|---|
| 8.6.7 | Per part, **four** program buffers (published, committed, candidate, one free), not three; three cannot give a wait-free reader that keeps its table after a failed read (model-checked). The audio thread claims the published buffer as a candidate inside the seqlock window, re-checks that it is still published, and commits the claim only after `endRead` succeeds; the writer reads the claim before the committed index. Unchanged table: one atomic load per block. With two parts in one window: commit both or abort both (`source/bside/program/ProgramHandover.h`). |
| 8.8 | **Tables are stored as the user wrote them** (owner-facing decision by the orchestrator, 2026-11-14): the manager, program, tone and bank files and the session's `Tables` child keep the validated lines verbatim (comments, spacing, label names). The canonical form is used only to compare tables (the modified marker) and to compile. A text change with the same canonical form tells the host that the state changed but does not mark the program modified. |
| 8.8 | `Instrument` also carries an optional canonicaliser and the limits `maxTableLines`, `maxTableBytes`, `maxFileBytes`, `maxJsonValues` (0 = the common default). A table line holds no LF, CR or NUL. The manager owns a table sink: `prepareTable` (before the hold; may allocate or fail, a failure is treated like an invalid table), `publishTable` and `emptyTable` (inside the hold; copy or swap only, `noexcept`). |
| 8.8 | B-Side limits: 1024 lines and 32 768 bytes per table, file cap 12 MiB, 160 000 JSON values. The worst legitimate bank (64 programs, two maximal tables, every byte costing two written bytes) is 9.9 MB and 136 327 values. The cap is checked before parsing, and **no save function writes a file its reader would refuse**. |
| 8.8 | Session: of several `Tables` (or `Preset`) children the first is used and all are removed. A malformed table gives that part an empty table and an error available from `PresetManager::tableError(part)` until the next apply or `setTable` for that part. `setTable` is the editor's entry point (message thread; tells the host that non-parameter state changed). |

### 8.15 Amendments from the reference data
Reference behaviour of the digital core, measured from the reference emulator through the trace
interface (`tests/bside/reference/`). **These rules win over 8.4.3, 8.4.4, 8.9.3 and 8.9.4 where
they differ.** Values marked † are constants of the reference, tuned to one chip revision per
model: approximate on real hardware, exact for the bit-exact rule. Validated by an independent
per-cycle model written from this text: 0 mismatches at every checkpoint of every committed `d_*`
trace (both models); the `c_*` traces agree wherever no combined-waveform shape enters (8.15.8).

#### 8.15.1 Time, fields, power-on
- The state at cycle c is the state after c steps. A write at cycle c applies between step c and
  step c+1; writes with equal cycles apply in file order. A periodic checkpoint at c is taken before
  that cycle's writes; an after-write checkpoint is taken right after its write, with no step in
  between.
- Fields (reference conventions):
  - `acc`: 24-bit accumulator.
  - `noise`: 23-bit register, bit k as in 8.4.3 row 5.
  - `env`: counter.
  - `phase`: 0 attack, 1 decay/sustain, 2 release.
  - `rate`: position 0 … 32 766 of the rate counter (one to one).
  - `div`: divider counter in bits 0–6, pending rate reset in bit 7.
  - `rd1B`: oscillator 3's read value, bits 11..4.
  - `rd1C`: oscillator 3's envelope sample.
- Power-on, which is also `reset()`:
  - Oscillators: accumulators 0x555555; noise register 0x7ffffe; noise latch = register;
    first-shift flag set; noise pipeline 0; rise flags clear; fill and fade timers stopped; output
    and read value 0; pulse comparison high; Model B triangle/saw latch 0x555. Noise output = the
    output of the power-on register (0xfe0); unproven by data (no trace selects noise before the
    first shift; wanted at the next regeneration).
  - Registers: F, PW and control all 0.
  - Envelopes: counter 0xaa; phase release; target phase release; all pipelines 0; position 0 with
    a reset pending; divider counter 0, period 1, no period change pending; not frozen; gate 0;
    attack, decay, sustain and release all 0; read sample 0.
  - Analog states 0.

#### 8.15.2 One step
One step runs these four parts in order: (a) oscillator advance for 1, 2, 3; (b) envelope step for
1, 2, 3; (c) outputs for 1, then 2, then 3; (d) sync for 1, 2, 3.

**(a) Oscillator advance**
- If the test bit is set:
  - If the fill timer is running, decrement it. When it reaches 0: register := register OR
    (register << 1) OR 1 (23 bits); latch := register; update the noise output; if the register is
    not all ones, restart the timer with the fill interval †.
  - Set the first-shift flag. The pulse comparison reads high.
  - The accumulator, rise flag and noise pipeline do not change. Our chip clears the rise flag
    here; see the artefact in 8.15.6.
- Otherwise:
  - acc := (acc + F) mod 2^24.
  - The rise flag := bit 23 went from 0 to 1 in this addition.
  - If bit 19 went from 0 to 1: noise pipeline := 2. Otherwise, if the noise pipeline is above 0,
    decrement it:
    - reaching 1 (phase 1): latch := register; clear the first-shift flag;
    - reaching 0 (phase 2): a noise shift (8.15.3), with old waveform = new waveform = the current
      one.

**(b) Envelope step:** 8.15.4.

**(c) Output of oscillator n** (source s = 3, 1, 2 for n = 1, 2, 3)
- If any waveform bit is set:
  1. Index := acc bits 23..12. If ring = 1 and saw = 0, index bit 11 := acc bit 23 XOR NOT (source
     acc bit 23). The source's accumulator is read as it is at this point of the step, so for n = 1
     it comes before oscillator 3's output (and any pull-down), and for n = 2 and 3 after their
     sources' outputs.
  2. Shape: triangle = (index < 0x800 ? index : index XOR 0xfff) << 1 (12 bits); saw = index; pulse
     or noise alone = 0xfff. A combined-waveform table (per model; our own fits, 8.15.8) is used
     when at least two of triangle, saw and pulse are selected, or pulse together with noise.
     Noise with only triangle or only saw uses no table: the shape is that of the triangle or saw
     (the traces `c_9_*` and `c_a_*` replay exactly this way).
     **Pulse together with noise and neither triangle nor saw** (S2b, measured): the shape is not
     indexed by the accumulator: shape := Tpn(noise output bits 11..4), a per-model table of the
     8 noise output bits (our own fit, the same network form with the noise output driving the
     bit lines in place of a waveform). Step 3's AND with the noise output still applies.
     **Structural rules of our tables.** For all 12 bits: a bit is set only on a line where the
     selected triangle or saw input is high (it holds in every measured sample and is part of
     the fitted model, not only a cosmetic rule). Bits 3..0 cannot be observed through the
     read-back; they are set only as the continuation of a run of set bits from bit 4 down (so
     no entry has low bits alone). A reader of a shape must go through the chip's shape function:
     the table row for pulse + noise is unused.
  3. out := shape AND (pulse selected ? the pulse comparison of the previous step, high under
     test : 0xfff) AND (noise selected ? noise output : 0xfff).
  4. Model A, saw bit set: if out bit 11 is 0, clear acc bit 23 and the rise flag.
  5. Read value. Model A: out. Model B with a triangle or saw bit set: read value := latch AND
     this step's pulse mask AND noise mask, where the latch is the step-2 shape (after any
     combined table) of the previous step that had a triangle or saw bit set; then latch := this
     step's shape. The latch is updated only in such steps. Model B otherwise: out. (To confirm
     with the fitted tables in S2b: latching the index and applying the current table instead
     differs only when the waveform bits change.)
     **Measured in S2b (`s_latch_b`, 40 switches per case; `s_latch_pn_b`):** the steady state
     follows this rule. A pulse-only step reads 0xff itself and leaves the latch (40 of 40);
     pulse + noise steps leave it too (60 of 60). In the first step after the waveform bits
     change, the reference reads the latched shape in 30 of 40 switches from triangle+saw to
     saw+pulse (1 fits the latched index, 9 fit neither) and in 30 of 40 returns after a
     pulse-only step; a switch from triangle+saw to saw alone reads
     saw(index) AND (saw(index) << 1) in all 40 cases, which neither reading gives. The rule
     above stays for now (the chip latches the shape); **the first step after a change of the
     waveform bits on Model B is not proven.** Tests pin how many of these reads the chip
     reproduces (29, 40, 31 and 5 of 40). Candidate for the next chip step (S2b review, measured
     at 29 indices per transition, exact for all eight switches to a single waveform): the latch
     holds the previous step's selected triangle/saw lines before the combining network
     (triangle+saw: x AND (x << 1); saw: x; triangle: tri(x)), and each read passes them through
     the combining stage of the current waveform bits, then the masks. Only the Model B read
     value is affected, not the output.
  6. Noise and another waveform bit together (write-back): if the noise pipeline is not 1 and the
     test bit is clear, the register's bits 20, 18, 14, 11, 9, 5, 2, 0 are ANDed with out bits
     11..4, and noise output := noise output AND out. Otherwise (in this case only), noise
     output := out. With noise alone or no noise bit, the noise output is not changed here.
- If no waveform bit is set: out and the read value hold. If the fade timer is running, decrement
  it. When it reaches 0: out := out AND (out >> 1); read value := out; if out is not 0, restart the
  timer with the fade interval †.
- Last, in either case: pulse comparison := (acc bits 23..12 ≥ PW). It is used in the next step.

**(d) Sync**, for n = 1, 2, 3 in turn:
- If n's rise flag is set (as it stands after part (c)), n's target (2, 3, 1) has its sync bit
  set, and NOT (n has its own sync bit set AND n's source's rise flag is set), then target
  acc := 0.
- So on Model A a top bit pulled down in (c) never syncs. If a source is itself reset by its own
  source in the same cycle its top bit rises, its target is not reset.

#### 8.15.3 Noise register
- Output: noise output bits 11..4 = register bits 20, 18, 14, 11, 9, 5, 2, 0; bits 3..0 = 0. It is
  recomputed at every shift and every fill, and changed by write-back as in (c) step 6.
- Shift, from the latch: if write-back applies for the pair (old, new), first the latch's tap bits
  (20, 18, 14, 11, 9, 5, 2, 0) are replaced by out bits 11..4. Then register := ((latch << 1) AND
  0x7fffff) OR ((latch bit 22 OR first-shift flag) XOR latch bit 17).
- Write-back applies unless any of these holds:
  - old ≤ 8;
  - new < 8;
  - new = 8 and old ≠ 0xf;
  - Model A, and (old AND 3, new AND 3) is (1, 2) or (2, 1);
  - old = 0xc or new = 0xc.

  These are bits 7..4 of the control register, as hex. Combined cases only; reference behaviour,
  followed (8.15.8).

#### 8.15.4 Envelope (identical for both models)
- Rate match positions (= the periods of 8.4.3 row 9 minus 1), rates 0–15: 8, 31, 62, 94, 148,
  219, 266, 312, 391, 976, 1953, 3125, 3906, 11719, 19531, 31250.
- Per step:
  1. The read sample := counter (0x1C shows oscillator 3's).
  2. If a period change is pending, apply it.
  3. If the phase pipeline is above 0, decrement it. Then by target phase:
     - attack: on reaching 1, the compare position := the decay rate's; on reaching 0,
       phase := attack, compare := attack rate, the counter is unfrozen;
     - decay/sustain: on reaching 0, phase := decay/sustain, compare := decay rate;
     - release: if (phase is attack and the pipeline reached 0) or (phase is decay/sustain and it
       reached 1), phase := release, compare := release rate. Otherwise nothing.
  4. Counter step. If the step pipeline is above 0, decrement it. If it reaches 0, this cycle is a
     counter step (skip 5 and 6). Unless frozen:
     - In the current phase (not the one it was scheduled in): attack: counter := counter + 1
       mod 256; if it becomes 0xff, target := decay/sustain and the phase pipeline := 3. This
       replaces any pending release, so a gate-off can be lost.
     - Decay/sustain or release: counter := (counter − 1) mod 256; if it becomes 0, frozen. (A
       step at 0 is possible after an attack cleared the freeze and wraps to 0xff: `d_env_cases_b`,
       envelope 3 at cycle 407 985.)
     - Then, if the counter is now 0xff or 0x00, 0x5d, 0x36, 0x1a, 0x0e or 0x06, a period change
       to 1, 1, 2, 4, 8, 16 or 30 is pending for the next step.
  5. Divider pipeline. Otherwise, if the divider pipeline is above 0, decrement it. If it reaches
     0: divider counter := 0; if (phase is decay/sustain and counter ≠ sustain level) or phase is
     release, the step pipeline := 1. Skip 6.
  6. Rate reset. Otherwise, if a rate reset is pending: position := 0; clear the pending flag. If
     the phase is attack: divider counter := 0 and step pipeline := 2. Otherwise, unless frozen:
     divider counter + 1; if it equals the period, divider pipeline := 2 (1 when the period is 1).
  7. If position ≠ the compare position: position := (position + 1) mod 32 767. Otherwise set the
     pending flag; the position stays.
- Precedence: at most one of counter step, end of the divider pipeline, or rate reset happens per
  cycle, in that order; a pending reset waits (informative: this cannot occur, because every
  pipeline completes within 4 cycles of a reset or gate write, while a match needs position ≥ 8).
  Pipelines that were decremented without completing
  do not use up the cycle.
- Writes:
  - Gate 0→1: target := attack, phase pipeline := 2. If a reset is pending or the divider pipeline
    is 2: step pipeline := 2 when (the period is 1 or the divider pipeline is 2), else 4.
    Otherwise, if the divider pipeline is 1: phase pipeline := 3.
  - Gate 1→0: target := release; phase pipeline := 3 if the step pipeline is above 0, else 2.
  - Attack/decay write: store both nibbles; in attack, compare := attack rate; in decay/sustain,
    compare := decay rate.
  - Sustain/release write: sustain level := nibble × 0x11; in release, compare := release rate.
  - Other edges and equal values do nothing.
- Consequences (informative):
  - A gate-on takes effect 2 cycles later (3 if the divider pipeline is in its last cycle).
  - A gate-off takes effect 1 cycle later from decay/sustain and 2 from attack, plus 1 with a step
    pending.
  - A pipelined step acts in the phase current when it executes, not the one it was scheduled in
    (`d_env_lost_gate_off_b`); a gate-off can be lost when the pending attack step reaches 0xff
    (same trace).
  - Steps come 2 cycles after a reset in attack, and 2 (divider period 1) or 3 cycles otherwise.
  - Raising the sustain level above the counter during decay lets it decay to 0.
  - The delay quirk waits up to 32 767 cycles.
  - The counter can wrap from 0xff to 0x00 in attack.

#### 8.15.5 Writes to oscillator registers
- F and PW apply from the next step.
- Control:
  - Waveform bits change: if the new value is 0, start the fade timer with the hold time †.
  - Test 0→1: acc := 0; noise pipeline := 0; latch := register; start the fill timer with the hold
    time †.
  - Test 1→0: a noise shift at once, with old = the previous waveform and new = the written one.
    The first-shift flag is set if any step ran under the test bit.
  - The read value changes only in the next step.
- Constants †: test hold time 50 000 (A) / 986 000 (B) and fill interval 15 000 / 314 300; fade
  hold time 54 000 / 800 000 and fade interval 1 400 / 50 000.

#### 8.15.6 Known reference artefacts (not reproduced; traces must avoid them)
- Writing the test bit in the cycle an oscillator's top bit rises leaves the reference's rise flag
  set while the bit is held, and the held oscillator then resets its sync target. The adapter's
  regeneration must reject traces that do this (it does: exit 3; no committed trace reaches it).
- The board high-pass dead band: −50.8 / −46.4 dBFS residual (Model A / B) after a DC step.
- The reference's filter scales with the clock (8.15.7).

#### 8.15.7 Analog reference conventions
- Output: board-filter output × 3/2 (A) or 5/2 (B) / 32 768.
- Resampler: input sample k = the output of step k+1; positions before 0 take step 1's value; flat
  to 20 kHz, ≥ 120 dB down from 24 kHz.
- n = ceil(end × 48 000 / clock) output samples. The chip runs, with no writes, up to q + half + 2
  samples: half = 962 (985 248 Hz) or 998 (1 022 727 Hz), q = floor((n−1) × clock / 48 000), which
  is 943–963 or 979–999 cycles past `end`.
- Board filters: high-pass ≈ 1.2 Hz, low-pass ≈ 15.4 kHz at 985 248 Hz †.
- Analog references exist, and are valid, at 985 248 Hz only; our filter is clock-independent.
- Measurements use `AnalogMeasure.h` only.
- Per-frame comparison: band 0 and the frame level are ignored where both sides are below
  −40 dBFS. Band 22 is compared up to 20 kHz on both sides, through one shared function
  (`SpecAnalysis`, `kSpecBandLimitHz`).
- The voice-DC gate uses the first frame after each volume write.

#### 8.15.8 Owner decision (2026-11-14): follow, own tables
- **The digital core follows the reference emulator's per-cycle behaviour** as specified in 8.15.1–
  8.15.5 (written from the reference data, with the emulator's source open). Its † timing
  constants are followed as stated.
- **Combined-waveform shapes** (the per-model tables of 8.15.2 (c) steps 2 and 5) **and all analog
  constants** (8.4.4, 8.15.7) are our own fits to generated data, never copied from the emulator's
  tables or source (Q5 stands for them).
- **Licence consequence (accepted by the owner):** B-Side is tied to GPL-compatible licensing for
  good (the reference is GPL-2.0-or-later; this project is AGPLv3, which is compatible).
- **The bit-exact rule (8.4.3) with our own tables (amended after the S2a review):** a field is
  *shape-dependent* from the step in which a combined-waveform table value (8.15.2 (c) step 2)
  first reaches it: through the output, the read value or the Model B latch; on Model A through
  the pull-down of the oscillator's own accumulator bit 23 and rise flag; through a sync or ring
  that reads such an accumulator; or through a noise write-back. It stays so until the reached
  state is overwritten by a value that does not depend on it: for an accumulator a test-bit reset
  or a sync reset from a source whose rise is not shape-dependent; for the noise register a fill
  to all ones; for the read value the next exact output. Exempt fields are `accN`, `noiseN` and
  `rd1B` only; envelope fields are never exempt. Shape-dependent fields are **exempt** from the
  exact comparison at those checkpoints; every other field at every checkpoint stays exact. The
  replay marks them from the trace's writes and the chip's events, reports per trace and field
  how many checkpoints were exempt and how many of those differ, and fails any `d_*`, `a_*`,
  `c_9_*` or `c_a_*` trace with an exemption (they use no table).
- **Exemption budget (S2b):** with our own tables the replay also pins, per trace and field, the
  number of exempt checkpoints that differ from the reference (`tests/bside/trace/
  exemption-budget.txt`, regenerated with each refit like a golden) and fails on any change: an
  increase is a regression, a decrease asks for the budget to be regenerated; unknown or
  duplicate entries fail. As fitted: 81 of 6850, all in `rd1B` of 12 `c_*` traces (one of them
  in a noise-combination trace, `c_b_b`); every `acc*` and `noise*` field is at 0, so those are
  effectively exact.
- **What the exemption leaves unproven (S2a review):** within the `c_*` traces an exempt field can
  also hide an oscillator-path error that is not a shape error (`rd1B` is the only oscillator
  field those traces observe). Until our own tables exist, the `c_*` family proves envelopes and
  whatever lies outside a table; the oscillator-path rules are proven by the `d_*` traces, the
  unit tests and the independent model (`tests/bside/unit/IndependentModel.h`, written from this
  section alone; never edited by whoever edits `source/bside/chip/`). With the tables (S2b) the
  combined-waveform outputs are compared with the reference per waveform, model and pulse width
  within the fit tolerance, and by the audio comparisons of 8.9.4. Wanted at the next
  regeneration: a trace with a full noise fill after a combined waveform (an exemption that ends
  inside a trace) and one that selects noise before the first shift.

### 8.16 Amendments from S2b and S2c (as built, 2026-11-15; these win over 8.4.7 and 8.6 where they differ)
The full list of readings, with reasons, is `docs/decisions.md` "B-Side S2c" (rows 1–16) and
"S2b-1"; the rules below are the ones another work stream can depend on.
- **Chip entry points for the engine (8.4.7):** `powerOnKeepingTime()` gives the power-on state
  while `cycle()` keeps running (a stolen voice); `clearEnvelopeCounters (n)` sets exactly
  envelope n's rate counter position and divider to 0 (the Clean policy's direct operation,
  8.6.4). `Config::shapes` selects the table set (fitted by default).
- **The engine** (`source/bside/engine/ProgramEngine.*`) is one note voice driven like the chip:
  render to `nextTickCycle()`, then run the tick. Tick 0 runs at the note-on cycle; the next
  tick's cycle is fixed when a tick runs, so a period change applies from the tick after the one
  already scheduled. Output goes through `ProgramSink` (the real chip or a recorder). A program
  given to the engine must have passed `validate()`.
- **Ownership and order inside a tick:** restart writes; the three wave lanes' rows; the
  host-driven registers of oscillators 1–3; then pulse lanes and the filter lane, each followed
  by its host-driven registers. Within an oscillator: frequency, envelope, control. A lane
  command always writes; a register no lane has written for this note follows the host and is
  written only when its value changes. An oscillator the host leaves off still gets its
  frequency when it is the sync or ring source of an active one.
- **Note-off:** a tick on the same cycle runs first; a lane with a `release` label jumps there; a
  wave lane without one clears its gate. **After note-off no table write can raise the gate**
  (the gate bit of every later control write is cleared; the other control bits still apply).
- **Restart section and policies (8.6.4):** Authentic applies no part of the restart section
  (no idle writes, no tick-0 writes, no `firstwave`, no reset on a steal, no direct clear).
  Precondition and Clean write the restart state when the voice goes idle, and at tick 0 on a
  retrigger that was not preconditioned; Clean also clears the envelope counters directly when
  the voice did not get the full lead. A stolen voice is reset (keeping time) and then gets the
  restart writes. Without a restart section, Precondition and Clean clear a still-high gate
  before a retrigger (owner to confirm). `firstwave` applies per active oscillator and ends at
  the table's first control write, at tick 1 or at note-off, whichever comes first.
- **Values:** register frequency = 440 × 2^((m − 69)/12) × 2^24 / clock (A4 = 440 Hz, equal
  temperament), Authentic rounds half up; macros are added after the lane or host value and
  clamped; ping-pong turns at a limit before the step and stops exactly on a limit.
- **Goldens (8.9.6):** `tests/bside/golden/` (72 traces; four hand-derived from this contract,
  the rest derived by `tools/model.py`, which follows the same readings and is therefore not an
  independent proof of them).

### 8.17 Amendments from S2b-2: waveform rendering (as built, 2026-11-16; these win over 8.4.5 and 8.4.7 where they differ)
The full list of readings, with reasons and measured figures, is `docs/decisions.md` "B-Side
S2b-2" (rows 1–27); the rules below are the ones another work stream can depend on.
- **Interface:** `Chip::render (out, n)` adds the sum of the three oscillators after their
  envelopes and converters, before routing, filter, volume and the board filters (those are S3).
  `renderUntil (out, max, untilCycle)` stops inside a sample so that a write lands at its exact
  cycle. Rendering never changes the digital state: after any sequence of renders and writes
  `DigitalState` equals pure stepping, and the output is bit-identical for any block split. A
  `Chip` must be constructed and prepared off the audio thread (tables are built and the BLEP
  tables fetched there); `render` allocates nothing.
- **Latency:** `Chip::kLatencySamples` = 8 internal samples (both kernels have 8 zero crossings).
- **What is rendered:** each oscillator as a continuous line through the digital accumulator,
  every discontinuity as a band-limited step or ramp at its exact sub-sample time. Triangle and
  saw in closed form (Model A: each converter bit as its own square wave, with hysteresis where a
  bit becomes too fast); pulse edges one cycle after the line crosses the width; combined
  waveforms as exact band-limited staircases of the fitted shapes (not mip tables); noise as
  sample-and-hold per noise clock; noise combined with another waveform, and no waveform, cycle
  by cycle from the digital output. The line leads the chip's own 1 MHz output by 0–1 cycle, sync
  resets are placed at the source's crossing, and Model A's one-cycle pull-down glitch is not
  rendered except where oscillator 1 sees it through ring.
- **Kernels** (`common/dsp/BlepTable.h` is `BasicBlepTable<zero crossings, β × 10, sample type>`;
  Vintage's `BlepTable` is the unchanged 8 / 8.0 / float instance): pulse edges, combined-waveform
  steps and the writes entering or leaving them use 8 zero crossings, Kaiser β 12, double
  precision, read with a second-order correction of the table interpolation; everything else the
  8 / 8.0 kernel. An 8 / 8.0 kernel attenuates at most about 85 dB of what folds into the band.
- **Alias budgets of 8.4.5:** met with at least 3 dB to spare in every gated row at every
  internal rate (the closest: Model B pulse width 4095 at 3.85 kHz, 88.2 kHz, 5.3 dB inside),
  measured at the internal rate by two methods. **S4 must gate after its decimator**, which has to
  attenuate 24–44 kHz by about 85 dB or more. No budget exists above 3.85 kHz.
- **Predictions and their guard:** sync resets, ring flips and Model A pull-downs are predicted
  from the lines and confirmed by the digital core; a prediction the core does not confirm is
  corrected band-limited and counted (`renderCorrections()`; above 1e-4 of a period:
  `renderLargeCorrections()`). Guaranteed by tests: no large correction and no mismatch between
  the renderer's gate and ring state and the chip's (`renderCheckStates`) over random patches and
  random write schedules without sync chains (50 000 patches and 3000 schedules in the hidden
  long runs). Sync chains (a synced oscillator that itself syncs another) are the documented
  exception: self-healing, counted, not gated.
- **Known defects left by the fourth review (to fix in the cost step, which reworks this code):**
  a synced pulse at frequency 0 keeps the wrong gate (reachable only by raw writes or trace
  replay; the program engine never writes frequency 0, and the state check skips a line frozen at
  a boundary); with the placeholder shapes only, a ring target can stay inverted for up to one
  source period after a width write in the step before the source's rise; a large frequency drop
  on a sync source between its rendered crossing and its digital rise leaves a timed-out
  prediction (a glitch of a few cycles, 4 of 3000 timings tried). Sync chains are not compared
  with the chip's output at all.
- **Known limits:** at exact power-of-two periods the chip visits only every N-th table cell and
  differs from the continuous line; sync on combined-waveform targets changes single harmonics
  against the chip's own output (the 0–1 cycle lead); the reference shows a second harmonic on
  the triangle that a converter linear in its bits cannot produce (a nonlinearity after the
  converters: S3). **Cost is open:** about 56 ns per internal sample per chip for a light Model B
  patch at 96 kHz, about 200 ns on Model A, several thousand for three combined waveforms at
  12 kHz, against about 31 ns per chip including the filter at S4 (8.5): a cost step comes before
  S4 (decisions row 14 lists the candidates).

### 8.18 Render cost step (contract, 2026-11-18; before S3 and S4)
Goal: bring `Chip::render` to a cost that lets S4 meet 8.5 (about 31 ns per chip per internal
sample at 96 kHz, filter included), and fix the three known defects of 8.17. No change of sound
is intended: what 8.17 guarantees stays guaranteed.

- **Unchanged (gates):** the interface and latency of 8.17 (`kLatencySamples` = 8); the digital
  state (bit-exact rule 8.4.3: every committed trace, the independent model, pure stepping equal
  to rendering, any block split bit-identical); every alias budget of 8.4.5, by both measurements,
  at every internal rate, with **at least 3 dB to spare** as today; no large correction and no
  state mismatch in `[schedule]`, `[corrections]` and the two hidden long runs; the reference
  comparisons (`bside_trace`); real-time rules (no allocation in `render`, bounded loops). No
  existing threshold is relaxed and no test is removed or weakened; a test whose expectation must
  change (a golden figure of the old code path) is listed with its reason.
- **Output may differ from today's** within those gates (another table resolution, another order
  of float operations). Where it differs, the step reports the largest difference against the
  renderer of `main` over the random patches (expected: below −100 dB of full scale).
- **Cost targets** (`chip_bench`, this Mac, non-LTO, best of 5, nothing else running; ns per
  internal sample per chip at 96 kHz; the 48 and 192 kHz columns are reported and must not be
  worse than today's):

  | Case | Today | Target |
  |---|---|---|
  | `silent` (nothing selected) | 60 | ≤ 3 (8.5: idle costs no CPU) |
  | `typical` (Model B) | 56 | ≤ 20 |
  | `typicalA`, `sawsA` (Model A) | 200, 263 | ≤ 24 |
  | new `s4gate` (Model A: pulse with a width write every tick, saw, one combined waveform, about A3–A4; the patch of the S4 gate, 8.5) | not measured | ≤ 24 |
  | `combined` (Model A, three combined waveforms) | 364 | ≤ 60 |
  | `comb12kA`, `comb12kB` | 4922, 4215 | ≤ 150 |
  | `worst`, `noise`, `noisesrc`, `landA` | 753, 166, 316, 640 | not slower; reported |

  The targets leave about 7 ns of the 31 for S3's filter (an estimate). A target that is not
  reached after a real attempt is reported with a per-stage breakdown (digital core, line
  simulation, sweeps, kernel reads, ring buffers) and what would be needed; the orchestrator then
  decides (accept, another round, or an owner question about the S4 budget).
- **Above 3.85 kHz** (Smooth allows 12 kHz; no alias budget exists there): combined waveforms may
  be rendered by a cheaper method (band-limited tables or a cap on the steps rendered per
  sample). The switch between methods must not click (hysteresis, as for the Model A converter
  bits), and the measured aliasing above 3.85 kHz is reported; at and below 3.85 kHz nothing
  changes.
- **The three defects of 8.17** (a synced pulse at frequency 0; the placeholder-shapes ring
  target after a width write in the step before the source's rise; a large frequency drop on a
  sync source between its rendered crossing and its digital rise) are each fixed with a
  regression test that fails on today's code. The state check no longer skips a line frozen at a
  boundary unless a reason is given.
- **The digital core** (`Chip.h`, `Chip.cpp`) may be made faster under the bit-exact gates.
  `tests/bside/unit/IndependentModel.h` is not edited by whoever edits `source/bside/chip/`.

#### 8.18.1 As built (2026-11-19; these win over the targets above and over 8.17 where they differ)
Full record: `docs/decisions.md` "B-Side render cost step" (rows 1-24). Reviewed independently
three times.
- **Cost reached** (`chip_bench`, this Mac, alone, ns per internal sample per chip at 96 kHz):
  `silent` 2.0, `typical` 13.4, `typicalA` 35.5, `sawsA` 41.9, `s4gate` 30.7, `combined` 92.2,
  `comb12kA` / `comb12kB` 183 / 160, `worst` 330, `noise` 166, `noisesrc` 182, `landA` 196.
  **Not met:** `typicalA`, `sawsA`, `s4gate` (target 24) and `combined` (target 60). Every
  further saving found for the Model A converter bits changed the 0-20 kHz band (measured -55
  to -63 dB re 1), so none was taken. **S4 consequence:** the gate patch of 8.5 costs about as
  much as the whole budget of 8.5 before the filter; S3 and S4 must either find a cheaper
  batched sample loop or put the Model A budget to the owner.
- **`comb12k` target raised to 200 ns** (orchestrator, 2026-11-18) to pay for the alias limit
  of the next point.
- **Above 3.85 kHz** a combined waveform keeps its exact staircase; the steps of one stretch
  (half a sample) are emitted as one step with time moments up to the fourth. At and below the
  register's top the output is bit-identical with the method off. **New limit (orchestrator;
  extends the combined-waveform row of 8.4.5 upwards):** aliasing in 0-20 kHz at most -80 dB re
  the strongest line up to 12 kHz, for shapes whose strongest line is above -50 dB re 1,
  measured not bin-centred, synced targets at a non-integer ratio. Measured worst -81.5 dB over
  1209 rows; 12 quieter rows are recorded (worst -76.1 dB).
- **Any split, now also at stops that are not writes:** a piece of the line simulation runs
  from a sample's start or a write to the sample's end; a `renderUntil` stop inside it only
  advances the digital core (hidden `[.render-split-long]`; older code differed by up to 0.396).
- **The three defects of 8.17 are fixed**, and two siblings on frozen lines (frequency 0).
- **Known defects left open, all older than this step** (reports only, no gate):
  1. Model B pulse widths 1 and 4095 miss -100 dB by up to 2.6 dB at frequencies with a
     small-denominator ratio to the internal rate (`[.render-exact]`).
  2. A synced target at an exact integer frequency ratio to its source carries a spurious line
     at half the source rate (-43.8 dB for a Model B saw at 3 kHz, where the chip's own output
     is at -89.8 dB); no gate measures an exact ratio (`[.render-sync-integer]`; which ratios
     are affected is not settled).
  3. A width write moves the rendered pulse edge two cycles ahead of the chip (8.17 says 0-1).
  4. A frozen line and one crawling below one accumulator step render up to 0.05 apart for one
     sample in a few cases; which is right is not settled (`[.render-frozen-crawl]`).
  Items 1 and 2 are the **alias follow-up step** (owner decision 2026-11-18: fix, no exemption).
- **Sanitizers are part of this code's gate:** the frequency-0 division of the first round
  passed every Release test on arm64 and would have crashed x86-64 hosts.

### 8.19 S3 in two parts (contract, 2026-11-19; refines the S3 row of 8.10)
S3 is split so that its first part can run beside the alias follow-up step (8.20), which owns the
renderer. 8.4.4, 8.9.4 and 8.15.7 stay the specification; this section says who builds what.

#### 8.19.1 S3a: filter and output stage as their own classes (new files only)
- **Files:** new `source/bside/chip/Filter.*` and `source/bside/chip/OutputStage.*` (JUCE-free,
  allocation only in `prepare`), their tests, new `chip_bench` cases, the source lists. **Not
  touched:** `ChipRender.cpp`, `RenderTables.*`, `Chip.h`, `Chip.cpp`, `ChipClock.h`,
  `tests/bside/unit/IndependentModel.h`, the existing renderer tests.
- **`Filter`** (one class, both models; 8.4.4): 2-pole state-variable, low-, band- and high-pass
  outputs summed by the mode bits; cutoff as a continuous register value 0 ... 2047, resonance
  0 ... 15; Model A with its non-linear integrators, its non-linear cutoff curve and
  `variation`; Model B clean and close to linear in the register. Clock-independent (8.15.7).
  Input: the sum of the oscillators routed to it, in the units `Chip::render` produces.
  Sample by sample at the internal rate; a `Part` can own it and lend it to several chips (8.5).
- **`OutputStage`** (per chip): filter output + direct sum → mixer with the voice DC → volume
  (4 bits; `volumeDc` off removes the voice DC before the volume) → board filters (8.15.7) →
  the reference's output scale (8.15.7). The nonlinearity after the converters (a second
  harmonic on the triangle at -34 / -40 dB, decisions "B-Side S2b-2" row 5) is located and
  modelled here or in the filter input, wherever the reference data puts it.
- **All constants are our own fits to generated data** (Q5, 8.15.8). The built adapter may be
  run on new traces for fitting, with outputs outside the repository; the emulator's source and
  tables are never opened. Committed reference data is not regenerated in this part: a trace
  that a gate still needs is proposed in the report.
- **Until S3b** the renderer gives one sum of the three oscillators, so the tests of this part
  drive the chain from that sum for traces whose sounding oscillators share one routing, and
  take register writes 0x15-0x18 from the trace. The traces that need separate buses
  (`a_routing_*`, any scripted trace with mixed routing) stay skipped and are listed.
- **Gates:** the analog rows of 8.9.4 for both models (corner at 16 cutoffs ±5 %, resonance peak
  ±1 dB at 0 ... 15, Model A harmonics 2-5 at three drive levels ±3 dB, voice DC step ±1 dB),
  measured with `AnalogMeasure.h` only, against the committed `a_*` data; the three modes and
  their sums (`a_modes_*`) within the scripted-trace tolerance; **stability:** every cutoff,
  resonance, mode and `variation` at every internal rate of 4.4, with full-scale noise, a cutoff
  or mode write every sample, and silence after a loud input: finite, bounded output and decay
  to below -120 dB with no input (no limit cycle, no denormal cost); block-size independent
  (bit-identical for any split); no allocation; sanitizers; the full `scripts/preflight.sh`.
- **Cost** (`chip_bench`, as 8.18): filter and output stage together **at most 7 ns per internal
  sample at 96 kHz on Model B**; Model A is reported with a per-stage breakdown (the Model A
  budget is the owner's question at S4, decided with these numbers).
- **Review:** independent (opus) with its own measurement of the same reference data and the
  skill `realtime-review`.

#### 8.19.2 S3b: the renderer's two buses and the whole chain (after 8.20 has merged)
`Chip::render` gets the meaning of 8.4.7, with its two buses in double precision (8.20.1): each
oscillator goes to the filter bus or the direct bus by its routing bit (oscillator 3 off as 8.4.3 row 14), a routing or volume write lands at
its cycle and meets the write-discontinuity budget of 8.4.5; "no waveform selected" fades as
8.4.4; the filter topology of 8.5; the remaining traces of 8.9.4 (routing, scripted traces);
the triangle segment of `a_dac_*`; filter sweeps of both models rendered for the owner (8.10).

### 8.20 Alias follow-up step (contract, 2026-11-19; owner decision 2026-11-18: fix, no exemption)
Fixes known defects 1 and 2 of 8.18.1 in the renderer. Runs beside S3a.

- **Files:** `source/bside/chip/` except the new `Filter.*` and `OutputStage.*`; the renderer
  tests; `chip_bench`. Not `tests/bside/unit/IndependentModel.h`.
- **Narrow pulses (defect 1):** Model B pulses of every width, 1 and 4095 included, meet -100 dB
  (8.4.5) at every internal rate **by the exact measurement** (a rectangular transform over
  whole signal periods), at frequencies with a small-denominator ratio to the internal rate as
  well as at bin-centred ones. `[.render-exact]` becomes a gate: a fast subset in the normal
  run, the full scan hidden. Aim for the 3 dB margin of 8.17; the margin reached is reported.
- **Integer-ratio sync (defect 2):** first settle, by a measurement stated in the report, which
  source-to-target ratios are affected (the two earlier findings disagree on ratios 2 and 4)
  and what the chip itself puts there (its per-cycle output through an ideal band limit).
  Then: a synced target at exact integer and small-rational ratios meets the sync budgets of
  8.4.5, measured not bin-centred; a line the chip's own output carries may stay at the chip's
  level + 1 dB. Exact ratios join the sync gate (`[.render-sync-integer]` becomes a gate).
- **Unchanged (gates):** everything 8.18 lists as unchanged and what 8.18.1 added (any split
  bit-identical, the -80 dB limit above 3.85 kHz, sanitizers). No threshold relaxed, no test
  removed or weakened. **Cost:** no `chip_bench` case more than 5 % above its figure in 8.18.1;
  every target met there stays met.
- **Output may change** where the two fixes need it; the largest difference against `main`
  over the random patches is reported, separately for patches without sync and without pulses
  (expected: none).
- **Optional, if the same code is open anyway:** defects 3 and 4 of 8.18.1. Otherwise they stay
  recorded.

#### 8.20.1 As built (2026-11-19; these win over 8.17 and 8.18.1 where they differ)
Full record: `docs/decisions.md` "B-Side alias follow-up step" (rows 1-13). Reviewed independently
(its own exact transform and sync model; no contract defect, three test gaps closed).
- **Narrow pulses (defect 1):** the cause was the conversion of the output to float, not the
  rendering. A narrow pulse sits half a converter range off zero; for a signal periodic in q
  samples the rounding errors repeat and gather on a few lines (the sum in double aliases at
  -115.6 dB at worst). `render` sums in double and converts through `render::OutputRounding`
  (`RenderTables.h`): the rounding error is noise-shaped, NTF (1 - 1.75 z^-1 + z^-2)
  (1 - 0.5 z^-1 + z^-2), at least 8.16 dB lower in 0-20 kHz at every internal rate and up to
  19.4 dB higher above the band (the decimator's range); the output stays within 4.75 float
  steps of the sum. After 128 equal values the conversion is plain and its state cleared, so a
  held or silent output is constant. Exact measurement, 6760 rows: worst -113.4 dB (96 kHz), a
  margin of at least 13.4 dB at every rate.
- **The shaping survives no later float addition (binding for S3b and S4).** Rendering into a
  non-zero float buffer or summing chips in float rounds again, unshaped: measured -96.5 dB
  into a buffer holding -0.75, and 0.86 dB of margin for two chips summed in float. So the
  buses of 8.19.2 are delivered in double precision, everything up to and including the sum
  of a part's chips stays in double, and the one conversion to float after that sum is shaped
  the same way. S4's alias gate after the decimator includes the exact measurement of narrow
  pulses on that whole path, with 1 and with 16 chips.
- **Exact ratios (defect 2):** the value and slope before a digital event at the sweep position
  come from the line's tracked cell. A reset on the same instant as the target's own wrap, fold
  or cell change used to lose its step every second source period: a saw at odd ratios, a
  triangle at odd and half-integer ratios, combined waveforms at every integer ratio (this
  settles the two earlier findings: both were right for the waveforms they measured). Exact
  ratios 1-8 and 1/4-7/2, all rates, 11 970 rows: worst -108.4 dB against -100 dB; no line
  needs the allowance for the chip's own level.
- **Gates:** `[render][alias][exact]` (also on the general sample path), `[render][alias]
  [sync-exact]` and `[render][rounding]` in the normal run; the hidden `[.render-exact]` and
  `[.render-sync-integer]` now assert.
- **Output and cost:** at most 4.8e-7 (-126 dB re 1) from the 8.18.1 renderer where no exact
  ratio is involved (the shaped rounding; no patch is bit-identical). Cost: 8.18.1's figures
  within 5 %; the conversion costs about 1 ns per sample on a moving output.
- **Compilers:** the shaping expression may be contracted into fused multiply-adds by one
  compiler and not another, so low output bits can differ between builds; no test compares
  output bits across builds.
- **Still open:** defects 3 and 4 of 8.18.1.

## 9. M7 — custom editor

The custom editor of Exitium Vintage Synth, replacing the preset bar + generic editor. Built on a
component library that the second instrument (section 8, S6) reuses.

| Input | Where |
|---|---|
| Owner decisions (tabbed workspace, sliders not knobs, graphs, part tint, hover help, live automation, own layout) | `docs/decisions.md`, "M7 UI direction" (2026-11-12 and 2026-11-14) |
| Approved wireframes, round 3: **the direction, not a pixel specification** (their overlaps, percent captions and side notes are not requirements) | `docs/ui/wireframes/*.png` |
| Help texts: single source, one per parameter | `docs/ui/help-texts.md` |
| Judgement calls of this contract, marked (review) | `docs/decisions.md`, "M7 editor contract" |

Items marked **(Un)** depend on an open owner question (9.8); until it is answered, build the
recommended option.

| Term | Meaning |
|---|---|
| edited part | Lower or Upper: what the part pages edit (UI state, not a parameter) |
| part page | Sound, Envelopes, LFO: shows `<p>.*` parameters of the edited part (`<p>` = `lower` or `upper`) |
| global page | Play, Arp + Seq, Effects, Browser: shows global parameters and both parts side by side |
| primary control | the one control that places a parameter (9.3.10 counts these) |
| secondary binding | a graph handle or shortcut that writes a parameter whose primary control is on the same page or frame |
| tick | one pass of the parameter mirror (9.1.5), at most 60 per second |

### 9.1 Architecture

#### 9.1.1 Files
```
source/common/ui/            vs::ui, JUCE (juce_gui_basics only); shared with B-Side; includes nothing from an instrument
  Theme.*                    tokens (9.2.3), fonts, LookAndFeel
  ParamMirror.*, ParamHandle.h   automation-follow and gestures (9.1.5)
  models/                    JUCE-free interaction logic of every control and graph (value <-> position, drag, wheel, keys)
  controls/                  Slider, SegmentedSwitch, Toggle, StepTypeToggle, ValueEntry
  graphs/                    GraphView (base), ResponseCurveView, EnvelopeView, LfoView, PianoRollView, ScopeView
  KeyboardStrip.*, StepGrid.*
  frame/                     EditorFrame, TopBar, PartSwitch, PageRail, HelpLine, HelpBubble, OutputMeter, OutputPopover
  HelpTexts.*, ControlRegistry.*
source/common/engine/UiFeed.h      JUCE-free: audio thread -> editor data (9.1.4)
source/common/shell/EditorState.h  JUCE-free: non-parameter UI state saved with the session (9.1.3)
source/common/presets/PresetActions.*  the file dialogs and save/load flows, moved out of PresetBar (PresetBar keeps using them)
source/vintage/ui/           VintageEditor (createEditor), one file per page, page registry, display models
source/vintage/dsp/FilterResponse.h   JUCE-free small-signal response of the voice filter (9.4)
```
- New `.cpp` files: `common/ui` → `VS_COMMON_SOURCES`, `vintage/ui` → `VS_VINTAGE_SOURCES`.
- Binary data target `VintageUiData`: `docs/ui/help-texts.md` and the font files (U2).
- `shell::InstrumentEditor` (S1a: preset bar + generic editor) stays as the fallback B-Side uses
  until S6. Vintage's `createEditor` returns `ui::VintageEditor`.
- Docs check (S1b extends it): `common/ui/models`, `UiFeed.h`, `EditorState.h` include no JUCE header.

#### 9.1.2 Editor ↔ processor boundary
| Data | Path | Thread |
|---|---|---|
| Parameter values (read) | `ParamMirror` polls `RangedAudioParameter::getValue()` of every parameter (9.1.5) | message |
| Parameter values (write) | `ParamHandle`: `beginChangeGesture` → `setValueNotifyingHost` … → `endChangeGesture` | message |
| Display text, parsing, default | the parameter's own `getCurrentValueAsText`, `getText`, `getValueForText`, `getDefaultValue`, `getLabel` (`ParamFactory.cpp` text functions): **never a second mapping** | message |
| Programs, banks, tones, files | `PresetManager` + `PresetActions`; change notifications through `PresetManager::changes()` | message |
| UI state (page, edited part, …) | `EditorState` owned by the processor (9.1.3) | message (editor), any (state save/restore) |
| Meters, held/sounding keys, sequencer step, host tempo | `UiFeed` owned by the processor (9.1.4) | audio writes, message reads |
| Host context menu of a parameter | `getHostContext()->getContextMenuForParameter` (where the host provides one) | message |

Rules:
1. The editor registers **no** parameter, APVTS or processor listeners. (Listener callbacks run on
   whatever thread changes a parameter, often the audio thread; polling keeps the audio thread free
   of editor work.)
2. The editor never calls the processor's test hooks or "read between blocks" accessors
   (`getLowerPart`, `getBlockParams`, `getLimiter`, `getArpSeq`, …) and never touches an engine object.
3. The processor never calls the editor. Nothing the audio thread does depends on whether an
   editor exists.
4. The editor takes no lock the audio thread takes (the audio thread takes none). It may take
   `PresetManager`'s locks through its public API, as the preset bar does.
5. Everything the editor owns stops in its destructor before members are destroyed: vblank/timer,
   open gestures (ended), file choosers (SafePointer pattern of `PresetBar`), popovers.

#### 9.1.3 Non-parameter UI state (`shell::EditorState`)
"Panel Mode is UI state" generalised: what the editor shows is saved with the session, is not a
parameter, not automatable, not part of a program, tone or bank, and never marks the program modified.

```cpp
namespace vs::shell
{
struct EditorState // all members std::atomic, relaxed; any thread
{
    std::atomic<int> page { 0 };        // index into the instrument's page list (Vintage: 0 Sound … 6 Browser)
    std::atomic<int> part { 0 };        // edited part: 0 Lower, 1 Upper
    std::atomic<int> arpView { 0 };     // view shown while arp.mode is Off: 0 Arpeggiator, 1 Sequencer
    std::atomic<int> sizePercent { 0 }; // 75 … 200; 0 = not chosen yet (fit to screen, 9.2.4)
    std::atomic<float> fxLastLevel[2] { 0.35f, 0.35f }; // delay, reverb level restored by the power switch (9.3.6)
};
}
```
- Saved by `getStateInformation` as a child `<UiState page="sound" part="lower" arpView="arp"
  size="100" delayLast="0.35" reverbLast="0.35"/>` of the state tree (page and view by name, so
  reordering pages never breaks a session). Read by `setStateInformation` into the atomics; an
  unknown or invalid attribute takes its default; a missing child (any session before M7) means all
  defaults. `stateVersion` stays 1 (pre-release). The editor reads `EditorState` when it opens and
  writes it on every change.
- Not saved: the browser's selection and search text, popover open/closed, scroll positions.
- This is the only change to the saved state in M7. S1a's identical-state proof (8.2.4 item 3) is
  S1a's; M7a lists the added child and adds a test that the Wave 4 fixture loads with defaults.

#### 9.1.4 Audio → editor feed (`UiFeed`)
```cpp
namespace vs
{
/** Written by the audio thread once per host block, read by the editor. Owned by the processor
    (lives as long as it). Every member is lock-free (static_asserted). Relaxed order except the
    held-order seqlock. JUCE-free: source/common/engine/UiFeed.h */
struct UiFeed
{
    // Output after the limiter and clamp, peak |x| per channel since the editor last took it.
    std::atomic<float> peak[2] { 0.0f, 0.0f };
    // Smallest limiter gain since the editor last took it (1 = no reduction).
    std::atomic<float> limiterGainMin { 1.0f };
    std::atomic<std::uint32_t> blockCount { 0 };   // +1 per processed block (bypassed blocks too)
    std::atomic<float> internalRate { 96000.0f };  // real-time voice rate (host rate x real-time os factor); set in prepareToPlay
    std::atomic<float> hostBpm { 120.0f };         // tempo the arp/seq used in the last block
    std::atomic<bool> hostPlaying { false };

    struct Part
    {
        std::atomic<std::uint64_t> held[2] { 0, 0 };     // keys the player holds that route to this part (bit n%64 of word n/64)
        std::atomic<std::uint64_t> sounding[2] { 0, 0 }; // notes of this part's sounding voices (after the arp/seq)
        std::atomic<std::int32_t> seqStep { -1 };        // 0..15: step that fired last while mode is Sequencer; -1 otherwise
        std::atomic<std::uint32_t> stepCount { 0 };      // steps fired by this part's arp/seq since reset
        // Held-key order for the arpeggiator preview (seqlock; up to 16 keys in press order).
        std::atomic<std::uint32_t> orderSeq { 0 };       // odd while being written
        std::atomic<std::uint64_t> order[2] { ~0ull, ~0ull }; // byte i = i-th key in press order, 0xFF = none
        std::atomic<float> liveCutoffLog2 { NAN };       // (U4 only) newest sounding voice's cutoff, log2 Hz
    } part[2];
};
}
```
| Item | Writer (audio thread, end of `processBlock` / `processBlockBypassed`) | Reader (editor) |
|---|---|---|
| `peak[c]` | block peak `p`; `if (p > peak.load()) peak.store (p)` | `exchange (0)` at most 30 times per second; a lost update can only hide a peak not larger than one already shown |
| `limiterGainMin` | block minimum `g`; `if (g < cur) store (g)` | `exchange (1)` |
| `held`, `sounding` | built from the router's key owners and the part's voices (128 + 16 checks per part), two relaxed stores per mask | relaxed loads; a mask may mix two blocks (display only) |
| `seqStep`, `stepCount` | from `ArpSeq` accessors (below) | relaxed |
| `order` | only when the held order changed: `orderSeq` +1 (odd), release fence, store words, `orderSeq` +1 (even, release) | acquire `s1`; if odd retry; load words; acquire fence; `s2`; use if `s1 == s2`, else retry up to 3 times, then keep the previous order |
| `internalRate` | `prepareToPlay` | relaxed |

- Write cost: about 30 atomic operations and 300 simple checks per block, no allocation, no
  system call. `voice_bench`/`fx_bench` within ±1 %; render regression and every audio test unchanged
  (the feed is write-only for the engine).
- Placement: M7a adds "9. publish the UI feed (reads only)" after step 8 of 8.2.3, in its own
  contract commit once S1a has merged (8.2 is S1a's contract while S1a runs). In the bypassed path
  peaks are 0 and `seqStep` −1.
- Additive JUCE-free accessors in `common/seq/ArpSeq.h` (M7d): `int lastSeqStep() const noexcept`
  (index of the Sequencer step that fired last, −1 when not in Sequencer mode or nothing fired since a
  restart) and `const KeyState& keys() const noexcept`. No behaviour change.
- `liveCutoffLog2` exists only if the owner chooses live markers (U4): each voice keeps the last
  sample's `log2Fc` (one store per sub-block, outside the per-sample loop; benchmark it, see
  engineering notes M6b).

#### 9.1.5 The live-automation rule
**Every slider, switch, graph handle and derived picture shows the parameter's current value within
one tick, whatever changed it, and is drawn from that same value.**

| Change source | Path to the screen |
|---|---|
| Host automation, host MIDI-learn, host-side parameter edits | parameter atomic → mirror tick |
| MIDI Program Change, host program list, preset bar/browser, Init, tone loads | `PresetManager` writes with `setValueNotifyingHost` on the message thread → mirror tick; name and modified dot via `changes()` and the existing 30 ms poll |
| Session restore (`setStateInformation`, any thread) | parameter atomics → mirror tick; `EditorState` re-read on `PresetManager::sessionRestored` |
| This editor (another control of the same parameter) | the writing control updates at once; every other binding at the next tick |

`ParamMirror` (one per editor):
- One entry per host parameter of the instrument (172), built once: parameter pointer, last value,
  gesture depth, time of the last external change, and a fixed list of subscribers (the components
  bound to it; registered at construction, never during a tick).
- `tick()`: for each entry load `getValue()` (normalised); if it differs bitwise from the last value,
  store it and notify the subscribers, which update their prepared drawing data and call `repaint()`.
  A burst of changes between two ticks is coalesced into one notification with the latest value;
  every subscriber of a program change is notified within the same tick, so JUCE paints the result
  as one frame.
- Driven by a `juce::VBlankAttachment` on the editor, at most one tick per 15 ms (≤ 66 Hz). If no
  vblank arrived for 100 ms while the editor is showing, a 30 Hz `juce::Timer` drives it instead
  (hosts and window systems that throttle vblank). Hidden or minimised: no ticks, nothing runs.
- Tests call `tick()` directly; nothing else is needed to bring the screen up to date.
- Cost: ≤ 50 µs per tick with all 172 parameters changed (measured in the UI bench).

Gestures (`ParamHandle`; hosts record automation only between begin and end):
| Interaction | Begin | Values | End |
|---|---|---|---|
| Drag (slider, handle, step bar) | mouse-down | each drag move | mouse-up |
| Click on a segment, toggle, track position | mouse-down | once | mouse-up |
| Double-click / menu "Reset to default" | before | default value | after |
| Typed value | on Enter | parsed value (`getValueForText`; unreadable text changes nothing) | after |
| Wheel, arrow keys | first notch / key | per notch / key | 300 ms after the last one |
| Multi-parameter drag (cutoff + resonance, painting steps) | each parameter when first touched | per move | all at mouse-up |

- Never a value without a gesture; never two begins without an end (depth counter, asserted); the
  editor's destructor ends any open gesture.
- **User drags while the host plays automation of the same parameter:** during the gesture the
  control shows the user's value. If the parameter takes another value (a host in Read mode keeps
  writing), the control shows the external-change marker. At mouse-up nothing is written; the next
  tick shows whatever value the parameter then has. (Hosts in Touch/Latch mode stop reading during
  the gesture; that is the host's job.)
- **External-change marker** (the wireframes' "automated" badge): VST3 does not tell a plugin which
  parameters have automation, so the marker means "changed by something other than this editor in
  the last 1.5 s" (host automation, MIDI control). A tick in which more than 8 parameters change
  (program load, session restore, Init) sets no markers. Shown as a ring on the thumb or handle and
  a small "AUTO" badge, fading out over 0.5 s.

#### 9.1.6 Threading summary
| Thread | Allowed in M7 code |
|---|---|
| audio | writing `UiFeed` (9.1.4) only. Nothing else in M7 runs on it. |
| message | everything in the editor: mirror ticks, gestures, painting, previews (9.4), `PresetManager` calls, file dialogs (async) |
| any (host) | `get/setStateInformation` touching `EditorState` atomics |
| other threads | none: the editor starts no threads |

### 9.2 Component library (`source/common/ui`)

#### 9.2.1 Interaction rules shared by every control
| Rule | Specification |
|---|---|
| Logic in models | every control's value ↔ position, drag, wheel, key and hit-test logic is a JUCE-free model class (`models/`); the component forwards events and paints. Tests drive the models (no display, no mouse) |
| Value text | `param.getCurrentValueAsText()`, then `" " + getLabel()` if the label is not empty. Panel parameters therefore show 0.0–10.0 (the wireframes' percent captions are illustrations) |
| Drag | relative; full track length = full range. Fine: Shift, or Cmd (macOS) / Ctrl (Windows, Linux): ×0.1. Click on the track jumps there |
| Wheel | over the control only (never scrolls a page): 1 % of the range per notch, 0.1 % fine; discrete parameters one step per notch |
| Double-click | reset to `getDefaultValue()` (also context menu "Reset to default") |
| Typed entry | click on the value text, or Enter when focused: a text field; Enter applies through `getValueForText`, Esc cancels |
| Context menu (right-click / Ctrl-click) | the host's menu for the parameter where provided, plus "Reset to default" and "Type a value…" |
| Keys (focused) | arrows ±1 % (fine ±0.1 %) or ±1 step; Page Up/Down ±10 %; Home/End min/max; Delete/Backspace default; Enter typed entry |
| Inactive | (9.3.9) dimmed (`text.disabled`, track at 40 %), still editable; accessibility description ends "(no effect in this mode)" |
| Hover | after 600 ms a help bubble (9.2.2); the help line updates at once on hover and focus |
| External change | marker of 9.1.5 |

#### 9.2.2 Components
| Component | Binds | Behaviour | Accessibility (`juce::AccessibilityHandler`) |
|---|---|---|---|
| `Slider` (horizontal, vertical; unipolar or bipolar from centre) | 1 float or integer parameter | label, value text, track and thumb in the part accent; bipolar for pan, balance, fine tune | role slider; name = label; value interface reads/sets text through the parameter |
| `SegmentedSwitch` | 1 choice or stepped integer (≤ 16 steps) | one segment per value; captions = choice labels unless the page lists aliases (alias count must equal the choice count; test); optional icons (arp pattern cards); arrow keys | role group of radio buttons, each with the full choice text |
| `Toggle` | 1 Off/On choice | latching button | role toggle button |
| `StepTypeToggle` | 1 Note/Rest/Tie choice | two latching buttons R and T; pressing the lit one returns to Note | two toggle buttons "Rest", "Tie" |
| `GraphView` (base) | handles with an x and/or y parameter each | axes, cached background grid (per size and scale), handles with 10 px hit radius, hover, drag with the fine modifier, double-click resets the handle's parameters, Tab moves focus between handles, arrows move them | role group; each handle its own child handler, role slider, named ("Filter cutoff") |
| `ResponseCurveView` | via page | log-frequency axis 20 Hz–20 kHz, dB axis −48…+24 dB; curve from a magnitude function (9.4) | — |
| `EnvelopeView` | via page | ADSR with handles (9.3.2) | — |
| `LfoView` | via page | waveform on a time axis, delay and cycle handles (9.3.3) | — |
| `PianoRollView` | display only | note bars over beats (9.3.5) | role image with a text summary ("C4 E4 G4 C5, 16 steps") |
| `ScopeView` | display only | one or two cycles of a waveform (9.4) | role image, text = waveform name |
| `KeyboardStrip` | `key.split_point` handle | keys coloured by owning part, split marker, held/sounding dots from the feed | split marker = slider with the note name as value |
| `StepGrid` | 16 × 3 step parameters + `seq.length` | pitch bars, velocity bars, R/T toggles, length flag, playing-step highlight (9.3.5) | each bar a slider ("Step 6 pitch"), toggles as above |
| `PageRail` | `EditorState::page` | icon + text buttons, current page lit | role radio group; Ctrl/Cmd+1…7 select pages |
| `PartSwitch` | `EditorState::part` | LOWER / UPPER segments, "Editing: LOWER" text beside it | radio group; name "Edited part" |
| `TopBar` | `master.volume` | part switch (part pages) or "Showing: whole instrument" (global pages), preset `<` name `>` with modified dot, Save, output meter, Master slider, LIMIT chip | — |
| `OutputMeter` | feed | two peak bars, −48…0 dBFS, peak hold 1 s, fall 20 dB/s, red above −0.1 dBFS | role progress bar, value "−12 dB" |
| `OutputPopover` | `master.limiter`, `master.limiter.ceiling` | opened by the LIMIT chip or the chevron; Off/On switch, ceiling slider, reduction meter 0–12 dB (feed); closes on Esc or click outside | role dialog |
| `HelpLine` | help texts | bottom row: "Label: text" of the hovered or focused control, else the page's one-line hint | live region (announced on focus change) |
| `HelpBubble` | help texts | after 600 ms hover, beside the control, max 320 px wide; hidden while dragging | not focusable; same text is the control's accessibility description |

#### 9.2.3 Theme tokens (dark; U3)
| Token | Value | Use | Contrast (WCAG 2) |
|---|---|---|---|
| `bg.window` | `#16181C` | window, rail | — |
| `bg.panel` | `#23262C` | panels | — |
| `bg.track` | `#3A3F48` | slider tracks, empty segments | — |
| `tint.lower` / `tint.upper` | `#3A3024` / `#1F3438` | panel headers and frame line of a part page | text on them 10.7 / 10.9 |
| `text.primary` | `#E8EAED` | labels, values | 12.6 on panel, 14.8 on window |
| `text.secondary` | `#A9AFB8` | captions, axis labels | 6.9 on panel |
| `text.disabled` | `#737985` | inactive (exempt from 4.5:1) | 3.5 on panel |
| `accent.lower` | `#F2A23A` (orange) | Lower: thumbs, fills, selected segments | 7.2 on panel, 5.0 on track |
| `accent.upper` | `#3FB8C9` (teal) | Upper | 6.4 on panel, 4.5 on track |
| `accent.global` | `#C9D1DB` | global pages' selections | 9.8 on panel |
| `text.onAccent` | `#16181C` | text on a lit segment | 8.5 / 7.5 / 11.5 on the three accents |
| `focus` | `#FFFFFF` 2 px ring + 1 px `bg.window` outer ring | keyboard focus | 15.2 on panel |
| `warn` | `#FF6B57` | clip mark, errors | 5.4 on panel |
| `cat.0` … `cat.11` | a 12-colour categorical set (M7f picks it; each ≥ 3:1 on `bg.panel`) | program categories | — |

- **Part identity never rests on colour alone:** the words "Editing: LOWER/UPPER", the lit
  segment text, the panel header text "LOWER PART"/"UPPER PART" on Play, and the part name in every
  accessibility name. Orange and teal stay distinct (yellow vs blue) for red-green colour vision
  deficiencies but have almost the same lightness (simulated contrast 1.1–1.3), so no information
  depends on telling them apart in greyscale. Categories likewise show their name (filter chips,
  detail panel, accessibility).
- Typography (logical px): 11 axis and small captions; 13 labels and values (tabular figures);
  15 panel headers (caps, letter spacing 0.04 em); 18 "Editing: …"; 22 the Arp + Seq mode switch.
  Minimum 11 at 100 % (never below 8.25 px at 75 %). Font: U2.
- Spacing: 4 px base grid; gutters 12, panel padding 12, control row height 32, segment height 28,
  slider track 6, thumb 14 × 20, panel corner radius 6.

#### 9.2.4 Size and scaling
| Item | Rule |
|---|---|
| Design size | 1360 × 784 logical px at 100 %: top bar 48, help line 24, page rail 112, content area 1248 × 712 |
| Resizing (U1) | fixed aspect ratio; 75–200 % (1020 × 588 … 2720 × 1568); corner resizer plus a "Size" menu (75, 90, 100, 125, 150, 175, 200 %); the size is UI state (9.1.3) |
| First open | the saved size; else 100 % if it fits in 90 % of the display's user area, else the largest menu size that fits |
| Layout | computed from the grid only, never from measured text (labels are truncated with an ellipsis), so it is identical on every platform and testable (9.6) |
| Drawing | vector only; no bitmaps except background grids cached per size and scale; no OpenGL |
| HiDPI | macOS: backing scale is automatic. Windows and Linux: the host's VST3 content scale (`setScaleFactor`) multiplies our size; per-monitor DPI changes repaint at the new scale |
| Host refuses a size | accept the size the host sets (`checkSizeConstraint` keeps the aspect); the Size menu is the dependable path in hosts without resizable plugin windows |
| Linux / X11 | DPI from the host's scale or `Xft.dpi`, often absent: default 100 %, the Size menu fixes it; resizing through the embedding varies by host; tests run under xvfb; Wayland-only sessions run the host under XWayland |

#### 9.2.5 Keyboard focus and accessibility
- Tab order: page rail → top bar (part switch, preset, Save, output, Master) → page content in
  reading order (panel by panel, left to right, top to bottom; inside a graph its handles) →
  output popover when open. Shift+Tab reverses. Esc closes a popover or text field.
- Every control has an accessibility handler with role, name ("Lower VCF cutoff" style: part + label),
  value text from the parameter and description = its help text. Page changes announce the page name.
- The whole editor is usable with keyboard alone (9.6 manual check with the platform screen reader).

### 9.3 Pages
Columns: **ID** (`<p>` = edited part), **Control** (9.2.2), **Captions** (only where they differ
from the choice labels), **Graph** (secondary bindings). All value text from the parameter (9.2.1).

#### 9.3.1 Sound (part page; 15 per part)
| Panel | Label | ID | Control | Captions / notes |
|---|---|---|---|---|
| Oscillator 1 | Octave (feet) | `<p>.vco1.range` | segmented | |
| | Waveform | `<p>.vco1.waveform` | segmented | scope above (9.4) |
| Link | Sync | `<p>.vco2.sync` | toggle | label "Sync", arrow text "2 → 1" (oscillator 2 restarts oscillator 1) |
| | Cross mod | `<p>.vco1.cross_mod` | vertical slider | "2 → 1" |
| Oscillator 2 | Octave (feet) | `<p>.vco2.range` | segmented | |
| | Pitch mode | `<p>.vco2.mode` | segmented | "Normal", "Low freq" |
| | Fine tune | `<p>.vco2.fine_tune` | bipolar slider | |
| | Waveform | `<p>.vco2.waveform` | segmented | scope above |
| Mix | Osc 1 ↔ Osc 2 | `<p>.mixer.source_mix` | slider | |
| High-pass + filter | Steepness | `<p>.vcf.slope` | segmented | |
| | Cutoff | `<p>.vcf.cutoff` | slider | graph handle A x |
| | Resonance | `<p>.vcf.resonance` | slider | graph handle A y |
| | High-pass | `<p>.hpf.cutoff` | slider | graph handle B x |
| | Key follow | `<p>.vcf.key_follow` | slider | |
| Amp | Level | `<p>.vca.level` | vertical slider | beside it a static signal-path list; its "Effects" item opens the Effects page |

Filter graph (`ResponseCurveView`): curve = `FilterResponse` (9.4) of the current values at middle C
without modulation; a dashed grey curve shows the other slope.
| Handle | Drag x | Drag y | Notes |
|---|---|---|---|
| A (on the curve at the cutoff) | `vcf.cutoff` = `map::cutoffFromHz (hzAtX)` | `vcf.resonance`: up = more, 200 px = full range | fine modifier on both |
| B (high-pass corner) | `hpf.cutoff` = `map::hpfFromHz (hzAtX)`, clamped 5 Hz–2.56 kHz | — | |

#### 9.3.2 Envelopes (part page; 16 per part)
Envelope 1 panel title is derived: "Envelope 1 (filter, pitch)" when `vcf.env_source` = ENV-1, else
"Envelope 1 (pitch)". Envelope 2: "Envelope 2 (loudness, filter)" or "Envelope 2 (loudness)".
| Panel | Label | ID | Control | Captions / graph |
|---|---|---|---|---|
| Envelope 1 | Attack, Decay, Sustain, Release | `<p>.env1.attack` `.decay` `.sustain` `.release` | 4 sliders | handles below |
| | Key follow | `<p>.env1.key_follow` | toggle | |
| | Direction | `<p>.env1.polarity` | segmented | "Normal", "Inverted"; when Inverted a mirrored ghost of the shape is drawn under the axis |
| What the envelopes do | Opens the filter by | `<p>.vcf.env_mod` | slider | |
| | Filter is moved by | `<p>.vcf.env_source` | segmented | "Envelope 1", "Envelope 2" |
| | Bends the pitch by | `<p>.vcomod.env_mod` | slider | note "Envelope 1 · oscillators: see LFO page, Pitch goes to" |
| | Playing softer closes the filter by | `<p>.vel.vcf` | slider | |
| Envelope 2 | Attack, Decay, Sustain, Release | `<p>.env2.attack` `.decay` `.sustain` `.release` | 4 sliders | handles below |
| | Key follow | `<p>.env2.key_follow` | toggle | |
| | Playing softer makes it quieter by | `<p>.vel.vca` | slider | |

(The code drives the pitch with Envelope 1 only, through its polarity; the filter with ENV-1 incl.
polarity or ENV-2; the VCA always with ENV-2. The wireframe's "Envelope 1 opens the filter" title
was only true for `env_source` = ENV-1, hence the derived titles.)

`EnvelopeView` per envelope; the x axis is the sliders' own scale (logarithmic time), not seconds:
slot width `S = 0.8 W / 3`; attack, decay, release each span `S × (0.1 + 0.9 v)` (`v` = normalised
parameter); the sustain plateau is fixed at `0.2 W`; y is level 0…1 (linear, as `sustain`).
| Handle | Drag x | Drag y |
|---|---|---|
| Peak (end of attack) | `envN.attack`: `v = ((x − x0) / S − 0.1) / 0.9` | — |
| Sustain start (end of decay) | `envN.decay` (x relative to the peak) | `envN.sustain` |
| Sustain end | — | `envN.sustain` |
| Release end | `envN.release` (x relative to the plateau end) | — |
Time labels under the handles are the parameters' text.

#### 9.3.3 LFO (part page; 9 per part)
| Panel | Label | ID | Control | Captions / graph |
|---|---|---|---|---|
| LFO | Waveform | `<p>.lfo.waveform` | segmented | |
| | Rate | `<p>.lfo.rate` | slider | cycle handle x |
| | Delay until full strength | `<p>.lfo.delay` | slider | delay handle x |
| The pitch (vibrato) | Amount | `<p>.vcomod.lfo_mod` | slider | |
| | Pitch goes to | `<p>.vcomod.target` | segmented | "Osc 1", "Both", "Osc 2"; note "also for the Envelope 1 pitch amount" |
| The pulse width | Amount | `<p>.vcomod.pwm` | slider | |
| | Driven by | `<p>.vcomod.pwm_source` | segmented | "LFO", "Manual", "Env 1" |
| The filter (wah) | Amount | `<p>.vcf.lfo_mod` | slider | |
| The volume (tremolo) | Amount | `<p>.vca.lfo_mod` | segmented | 0, 1, 2, 3 |

`LfoView`: linear time axis; window = one of 0.5, 1, 2, 5, 10, 20, 50 s. Outside a drag the window
always holds the delay and the first whole cycle, so no handle is ever clamped; the smallest window
that holds the delay plus two cycles is preferred while the first cycle ends within 80 % of it. The
window is kept during a drag, and after release as long as the delay handle is at most 90 % along
and the cycle handle between 10 % and 90 %; otherwise it is chosen again at release (amended after
the M7b review: handles jumped by up to 500 px at mouse-up).
The engine is silent for the first half of the delay and then fades in linearly, reaching full
strength at the delay time (`dsp::Lfo`): the picture shows a faint "starts" marker at delay / 2 and a
fade wedge that ends at the delay handle, captioned "Full strength after ...". Random and Noise are
previewed after two pre-roll cycles (the real LFO is free-running) with the caption "Example:
different every time".
| Handle | Drag x |
|---|---|
| Delay (end of the fade-in) | `lfo.delay` = `map::lfoDelayFromSec (t)`, 0–5 s |
| Cycle (right end of the first whole cycle after the delay) | `lfo.rate` = `map::lfoRateFromHz (1 / cycleSec)`, 0.05–40 Hz |
The mod wheel's vibrato amount is on Play (`mod.wheel_vco`).

#### 9.3.4 Play (global page; 4 global + 8 per part, both parts side by side)
| Panel | Label | ID | Control | Captions / graph |
|---|---|---|---|---|
| Keyboard layout | Whole / Dual / Split | `key.mode` | segmented | with a sentence explaining the chosen mode |
| | Split key | `key.split_point` | `KeyboardStrip` marker | value = note name; drag snaps to keys |
| | Balance Lower ↔ Upper | `mix.balance` | bipolar slider | |
| Character | Vintage | `global.vintage` | slider | |
| Lower part / Upper part (once each) | Voices | `<p>.assign.mode` | segmented | |
| | Unison voices | `<p>.assign.unison_voices` | segmented | |
| | Unison detune | `<p>.unison.detune` | slider | |
| | Glide time | `<p>.porta.time` | slider | |
| | Glide | `<p>.porta.mode` | segmented | "Always", "Legato only" |
| | Pitch bend range | `<p>.bend.range` | slider (stepped) | |
| | Mod wheel vibrato | `<p>.mod.wheel_vco` | slider | |
| | Pan | `<p>.mix.pan` | bipolar slider | |

`KeyboardStrip`: shows C1–C7 (MIDI 24–96, `noteName`, middle C = C4); shifts by octaves so the split
key is always visible with one octave to spare. Whole: all keys Lower; Dual: two stacked bars over
every key; Split: keys `< split_point` Lower, the rest Upper (the `KeyRouter` rule). **Held and sounding keys
(owner, 2026-11-15; replaces the earlier dots and rings, which read as buttons):** a key in
`UiFeed::part[p].held` is tinted with that part's accent colour (held by both parts: Lower in the
upper half, Upper in the lower half); a note in `.sounding` shows a small bar in the part's colour
with an outline in the window colour, on white keys in a row of its own just above the
octave-name row, on black keys at the foot (Lower left of Upper when both sound). The mark never
overlaps an octave name, at any scale. A pressed key is announced to assistive technology on the
press and where a glide ends.

**Playing the keys (owner, 2026-11-15).** Pressing a key on the strip plays that note; the split
marker moves only by its pill (drag, arrows, wheel, double-click), no longer by a press on the keys.
- Press = note on; release = note off; dragging onto another key releases the old note and plays
  the new one (one note at a time per mouse). Velocity follows the vertical position in the key:
  top = 40, bottom = 127, linear, fixed for that note. A press outside the keys plays nothing.
- The note is released when the mouse is released, the strip is hidden or destroyed (page change,
  editor closed), the editor stops showing (checked by its timer), or `cancelInteraction()` runs; a
  press ended that way plays nothing more until the next press. No note may hang.
- Path (`common/engine/UiNotes.h`, JUCE-free, owned by the processor like `UiFeed`): a fixed-size
  wait-free single-writer/single-reader queue of `{note, velocity}` (velocity 0 = off; capacity 64;
  ons and offs travel in the one queue, in order; a note on is accepted only while the queue has
  room for it and for the off of every note then on, so an off always fits behind its own on and
  can neither be lost nor overtake a later on; a note on that does not fit is dropped and its off
  is then never sent; at most 32 notes can be on at once). Dragging off the keys
  releases the note until the pointer is on a key again. In bypassed blocks the editor's notes are
  drained and dropped, like the host's MIDI. The message thread writes; the audio thread drains it at the start
  of every block (bypassed blocks too) and adds the notes as channel-1 note messages at offset 0 to
  the same event list as the host's MIDI, before that block's host events. From there they are
  ordinary notes: key mode, split, arp/seq, hold pedal and the `held`/`sounding` dots apply.
- `prepareToPlay` and `reset` empty the queue and release every editor-held note. With an empty
  queue the block is bit-identical to before (render regression unchanged); the drain costs one
  acquire load. No allocation, lock or system call on either side.
- Tests (9.6): press, drag across three keys and release produce exactly the expected on/off
  sequence in the processor's output (a sounding block, then silence after the release tail); the
  velocity rule; hiding the page mid-press sends the off; a 1000-press burst with a stalled audio
  thread leaves no note sounding once it drains; the split pill still works and a press on the keys
  no longer moves the split.

#### 9.3.5 Arp + Seq (global page; 58)
| Panel | Label | ID | Control | Captions / graph |
|---|---|---|---|---|
| Top | Off / Arpeggiator / Sequencer | `arp.mode` | segmented (large) | sentence for the mode |
| | Plays through | `arp.target` | segmented | Lower and Upper segments in their accents, Both split in two colours |
| 1 Pick a pattern (Arp view) | Pattern | `arp.pattern` | segmented, icon cards | "Up", "Down", "Up + Down", "Random", "As played" |
| | Octaves | `arp.octaves` | segmented | 1–4 |
| 2 Timing and feel (one panel, shown in both views) | Rate | `arp.rate` | segmented | "1/4", "1/8", "1/8 triplet", "1/16", "1/16 triplet", "1/32" |
| | Gate | `arp.gate` | slider | |
| | Swing | `arp.swing` | slider | |
| | Hold | `arp.hold` | toggle | "Hold (keep playing)" |
| | Tempo when the host is stopped | `arp.tempo` | slider | while the host plays: inactive, caption "host: 128 bpm" (feed) |
| Steps (Seq view) | Length | `seq.length` | slider | `StepGrid` length flag (x, snaps to steps) |
| | Step NN pitch | `seq.stepNN.note` | `StepGrid` pitch bar | drag y, 3 px per semitone, Shift snaps to octaves; double-click 0 |
| | Step NN type | `seq.stepNN.type` | `StepTypeToggle` | |
| | Step NN velocity | `seq.stepNN.velocity` | `StepGrid` velocity bar | click/drag y sets the value at the pointer |

- View: Arpeggiator or Sequencer follows `arp.mode`; while it is Off the view is
  `EditorState::arpView` (the last non-Off mode), dimmed, with "Off: choose Arpeggiator or Sequencer
  to hear it".
- **Painting steps:** a drag that crosses columns sets each crossed step's bar to the pointer's
  value (gestures per 9.1.5). Steps beyond `seq.length` are dimmed and editable.
- **Playing step:** the column `UiFeed::part[t].seqStep` is highlighted while `arp.mode` is
  Sequencer and `blockCount` advanced in the last 200 ms (`t` = target part; for Both, Lower,
  or Upper if Lower is silent). Updated at the tick rate.
- **Arpeggiator preview** (`PianoRollView`, 9.4): what the engine will play for the notes the player
  holds on a target part (from `UiFeed` held order, if any key is held now or was within 2 s) or
  else an example chord C4 E4 G4 pressed in that order, velocity 0.8. Shown over
  `max (4 beats, 8 steps)`; for Both in Dual or Split, Lower's and Upper's notes in their colours.
  The roll states its source ("example chord C E G" / "your keys").

#### 9.3.6 Effects (global page; 11)
| Panel | Label | ID | Control | Captions / graph |
|---|---|---|---|---|
| Chorus | Mode | `fx.chorus.mode` | segmented | "Off", "I", "II" |
| | Mix | `fx.chorus.mix` | slider | inactive while Off |
| Delay | Ping-pong | `fx.delay.ping_pong` | toggle (Off/On segments) | |
| | Time | `fx.delay.time` | slider | picture handle x (first echo) |
| | Repeats (feedback) | `fx.delay.feedback` | slider | picture handle y (second echo's height) |
| | Darkening (damping) | `fx.delay.damping` | slider | |
| | Level (0 = off) | `fx.delay.level` | slider | power switch (below) |
| Reverb | Pre-delay | `fx.reverb.predelay` | slider | picture handle x (start of the tail) |
| | Decay time | `fx.reverb.decay` | slider | picture handle x (RT60 point) |
| | Darkening (damping) | `fx.reverb.damping` | slider | |
| | Level (0 = off) | `fx.reverb.level` | slider | power switch |

**Delay and reverb on/off without new parameters (U5):** the engine already treats level 0 as off
(bit-exact bypass). Each panel header has a power switch, a secondary binding of the level: it is lit
while the level is above 0; switching off stores the level in `EditorState::fxLastLevel` and sets 0;
switching on sets the stored level (0.35 if none). While off, the header says OFF and the picture is
grey. Chorus needs no switch (its mode has Off).

#### 9.3.7 Browser (global page; no parameters)
| Element | Behaviour | API |
|---|---|---|
| Bank chooser | "Factory (read-only)", the current user bank, "Load bank file…"; Missing/Pending bank shown as in the preset bar | `bankName`, `bankStatus`, `PresetActions` |
| Grid 8 × 8 (A–H × 1–8) | name, slot, category stripe; click selects, double-click or Enter loads, arrows move the selection; current program outlined, modified dot on it | new `Meta slotMeta (int slot) const` (copy, any thread) and `std::uint32_t bankGeneration() const` (bumped by every bank change: the grid refreshes when it changes) |
| Categories | chips "All" + the distinct `category` values of the current bank, in order of first appearance (empty → "Uncategorised"); colour `cat.(index mod 12)`; one chip active at a time | from `slotMeta` |
| Search | case-insensitive substring of name or category; non-matching cells dimmed to 30 % (positions never move) | — |
| Selected program panel | name, slot, category (text + colour), author; Load program; tone loading (below); Save, Save as…, Init; Import… / Export bank… | `selectProgram`, `initialise`, `PresetActions` |
| **Load tone** | segmented "Take its: Lower tone \| Upper tone" (`ui.tone_source`; default Lower; when the selected program's `key.mode` is Whole its Upper tone is marked "(not heard in this program)"), then "→ Lower" / "→ Upper" | new `juce::String applyToneFromSlot (int slot, int sourceTone, int targetPart)` = `applyTone (toneOf (schema, program, sourceTone), targetPart)` |
| Save | user bank and a slot: `storeInBank (currentIndex)`; factory bank or detached: opens "Save as…" (program file or a user bank slot) | existing |
| Tone files | "Save tone…" / "Load tone file…" for the edited part | existing `saveToneFile` / `loadToneFile` |
- The top bar's `<` `>`, name and Save are the same flows on every page.
- As built in M7e: a fourth accessor `float slotValue (int slot, int paramIndex) const` (copy of one
  stored value, any thread) serves the "(not heard in this program)" mark; choosing a bank in the
  chooser shows it and loads its first program only when the current program is not modified.
- `PresetActions` takes over `PresetBar`'s menus and async file choosers unchanged; `PresetBar` calls
  it and keeps its tests (it is still B-Side's bar until S6).

#### 9.3.8 Frame, top bar, output popover and help (3 parameters)
| Element | ID | Control |
|---|---|---|
| Master | `master.volume` | slider in the top bar (value text "−6.0 dB") |
| Limiter Off / On | `master.limiter` | segmented, in the output popover (the LIMIT chip shows the state and opens the popover; it writes nothing) |
| Ceiling | `master.limiter.ceiling` | slider in the output popover; inactive while the limiter is Off |

- **Top bar as built in M7e (Vintage):** `<` `>`, slot, bank name, the program name button (236 px
  at 100 %: a 20-character name fits), Load..., Save. There is no Init button in the Vintage top bar
  (Init is on the Browser page); B-Side's bar keeps its buttons. Save is the Browser's Save (9.3.7).
  A menu choice that names a slot is refused with a message when `bankGeneration()` changed while
  the menu was open.
- **Program menu (owner, 2026-11-14; built in M7e):** clicking the program name in the top bar
  opens a menu of the current bank's 64 programs, grouped by `category` in order of first
  appearance (empty → "Uncategorised"), one submenu per category when the bank has more than one
  category, each entry "A3  Name", the current program ticked. Choosing an entry is
  `selectProgram (slot)`, the same flow as `<` `>` and the Browser's Load. The menu is built from
  `slotMeta` when it opens (never cached across `bankGeneration` changes); a Missing or Pending
  bank shows one disabled entry with the bank status. Below a separator: "Open Browser" (switches
  the page). Keyboard: Enter or Space on the focused name opens the menu. Test (9.6, M7e): the
  menu model (groups, order, tick) is a JUCE-free function tested against the factory bank and a
  bank with empty and duplicate categories.
- Part pages: frame line, panel headers and accents in the edited part's tint; "Editing: LOWER" in
  18 px beside the part switch. Global pages: neutral tint, "Showing: whole instrument" and both part
  colour chips. Editing Upper while `key.mode` is Whole shows a banner "Upper is not heard in Whole
  mode — switch to Dual or Split on the Play page".
- Help texts: `HelpTexts` parses the embedded `docs/ui/help-texts.md` (lines starting with
  ``- `id`: ``; `<part>.` → both prefixes; `NN` → 01–16 with the step number substituted into the
  text). The help line shows "Label: text"; for steps "Step 6 type: …".

#### 9.3.9 Inactive rules (display only; values stay editable)
| Control(s) | Inactive when |
|---|---|
| `<p>.assign.unison_voices`, `<p>.unison.detune` | `<p>.assign.mode` ≠ Unison |
| `<p>.porta.mode` | `<p>.porta.time` = 0 (Off) |
| `<p>.vcomod.pwm`, `<p>.vcomod.pwm_source` | neither oscillator's waveform is Pulse |
| `<p>.vco1.cross_mod` slider | never (but the Link panel notes "no effect while 0") |
| `key.split_point` | `key.mode` ≠ Split |
| `mix.balance` | `key.mode` = Whole |
| `fx.chorus.mix` | `fx.chorus.mode` = Off |
| delay / reverb controls except Level and the power switch | the level is 0 |
| `master.limiter.ceiling` | `master.limiter` = Off |
| `arp.tempo` | the host transport plays (`UiFeed::hostPlaying`) |
| every Arp + Seq control except `arp.mode` | `arp.mode` = Off |
| part pages for Upper | `key.mode` = Whole (banner, controls stay normal) |

#### 9.3.10 Completeness (172 of 172)
| Where | Scope | Primary controls | Count |
|---|---|---|---|
| Sound | per part, × 2 | 9.3.1 | 15 × 2 = 30 |
| Envelopes | per part, × 2 | 9.3.2 | 16 × 2 = 32 |
| LFO | per part, × 2 | 9.3.3 | 9 × 2 = 18 |
| Play | global + both parts | 9.3.4 | 4 + 8 × 2 = 20 |
| Arp + Seq | global | 9.3.5: 9 `arp.*`, `seq.length`, 16 × 3 steps | 58 |
| Effects | global | 9.3.6 | 11 |
| Top bar | global | `master.volume` | 1 |
| Output popover | global | `master.limiter`, `master.limiter.ceiling` | 2 |
| **Total** | | per-part 48 × 2 = 96, global 76 | **172** |
Every per-part suffix appears on exactly one of Sound, Envelopes, LFO, Play (15 + 16 + 9 + 8 = 48).
Enforced by the registry test (9.6); a new parameter without a primary control or help text fails it.

### 9.4 Derived pictures (the picture cannot disagree with the sound)
Every picture is computed on the message thread, only when one of its inputs changed (mirror tick),
by the engine's own JUCE-free code with the parameters' own mappings.
| Picture | Computed by | Inputs | Test (9.6) |
|---|---|---|---|
| Filter curve | new `dsp::FilterResponse::magnitudeDb (hz, …)` in `vintage/dsp/FilterResponse.h`: the exact small-signal (tanh ≈ identity) transfer of `OtaLadder` and `OnePoleHighPass` at `z = e^{jω}` at `UiFeed::internalRate`: per stage `H1 = g (1 + z⁻¹) / ((1 + g) − (1 − g) z⁻¹)` with `g = tan (π fc / fs)`, `LP = H1^n / (1 + k H1⁴)` (n = 4 or 2), × `comp = 1 + 0.5 k`, × the HPF's TPT response | `fc = map::cutoffHz (cutoff)` clamped like `Voice` (`2^3` Hz … `0.42 fs`), `k = map::resonanceK`, slope, `map::hpfHz`; middle C, no modulation | magnitude vs the real filter driven at −60 dBFS (linear region), 30 Hz … fs/4, 3 cutoffs × 3 resonances (k < 3.8) × 2 slopes × HPF off/on: within 0.2 dB |
| Oscillator scopes | `dsp::Oscillator` instances run offline (VCO-2 first, VCO-1 synced to its wrap exactly as `Voice::renderSample`), two cycles of VCO-2 at a display rate, after `kLatency` warm-up | waveforms, ranges (octave ratio), sync, Manual pulse width `0.5 − 0.45 × pwm` | scope samples equal a direct run of the same objects |
| Envelope shapes | `dsp::Envelope` run per segment at a display rate of 256 samples per segment, sustain from the parameter | the four times via `map::envTimeSec` | the drawn level at each segment end equals an `Envelope` rendered at 48 kHz at the same instants (± 0.5 %) |
| LFO waveform | `dsp::Lfo` (fixed preview seed) at 1000 points over the window, `trigger()` at 0 | `map::lfoRateHz`, `map::lfoDelaySec`, waveform | drawn points equal a direct `Lfo` run |
| Arp preview | two `seq::ArpSeq` instances (one per target part) run offline: settings from `ArpParamBinding::read`, transport playing at 120 bpm from ppq 0, 48 kHz, the preview notes as note-ons at offset 0, chunks of 4096 into a preallocated output (`kRecommendedCapacity`) | every `arp.*`/`seq.*` parameter; held order from the feed | the preview's notes, step positions and lengths equal the events the **processor** delivers to the part for the same chord and settings (rendered offline) |
| Delay picture | echoes at `n × map::delayTimeSec`, heights `map::delayFeedback^(n−1)` (the first echo has the height of the input; as the engine), alternating L/R with ping-pong | delay parameters | value checks |
| Reverb picture | pre-delay gap `map::reverbPreDelaySec`, then a −60 dB line at `map::reverbDecaySec` | reverb parameters | value checks |
| Live cutoff marker (U4) | `UiFeed::part[p].liveCutoffLog2` drawn on the frequency axis | feed | marker equals the stored value |

### 9.5 Performance and safety budgets
| Budget | Limit | Measured by |
|---|---|---|
| Mirror tick, all 172 changed | ≤ 50 µs | UI bench (hidden test `[.uibench]`) |
| Full page paint at 100 % / 200 % | ≤ 4 ms / ≤ 10 ms (reference Mac) | UI bench, software renderer |
| Repaint rate | parameters: once per changed tick (≤ 66 Hz); meters ≤ 30 Hz and only when a bar moves ≥ 0.5 dB; nothing repaints at rest | review + bench counter |
| Editor CPU, idle (no changes, no audio) | ≤ 0.5 % of a core | manual (Activity Monitor), checklist |
| Editor CPU, 16 parameters automated + arp + meters | ≤ 3 % of a core at 100 %, ≤ 6 % at 200 % | manual, checklist |
| Editor open | ≤ 250 ms to first paint | UI bench |
| Memory per open editor | ≤ 30 MB (grid caches) | manual |
| Audio thread | feed only (9.1.4): `voice_bench`/`fx_bench` ±1 %, no allocation (existing `processBlock` allocation test) | existing gates |
| Paint | our code allocates nothing in `paint()`: strings, paths and preview data are prepared on value change; painted through a no-op graphics context (so renderer-internal allocations do not count) our paint code makes a fixed number of allocations per page, 0 wherever no text is laid out in paint, and the same for every repeated paint (amended after the M7b–M7e reviews: the software renderer alone allocates hundreds of times per component, so "≤ 8 per component" could not be measured) | allocation test with `tests/seq_unit/AllocationCounter.h` |
| Open/close | 200 editor open/close cycles: no leak (JUCE leak detector in debug, ASan on Linux), no growth of processor-side state | test |
| Multiple instances | no static mutable state in `common/ui` except an immutable shared font holder (`juce::SharedResourcePointer`); two processors with two open editors stay independent | test |

### 9.6 Testing
| Test | What it proves | Runs |
|---|---|---|
| Help texts | parser rules; every one of the 172 IDs resolves to exactly one text; no text names an unknown ID; ≤ 2 sentences, ≤ 170 characters | all five targets |
| Registry completeness | an editor (not on screen) for Lower and for Upper: the primary placements equal the parameter list, each exactly once (9.3.10); every secondary binding's parameter has its primary on the same page or frame; segment alias counts equal choice counts | all five (Linux under xvfb) |
| Automation follow | for every parameter: set min, default and a random value from a background thread (`setValue`, as host automation does), `tick()`, then every binding of it shows `getValue()` and `getCurrentValueAsText()`; graph handles sit where the page mapping puts that value; derived pictures were recomputed; the same after `selectProgram`, `initialise`, tone loads and `setStateInformation` | all five |
| Gestures | through the models: begin before the first value, end after the last, counts balanced, for every interaction row of 9.1.5; multi-parameter drags; editor destroyed mid-drag ends the gesture; contested drag (host keeps writing) shows the marker and writes nothing at mouse-up | all five |
| Models | value ↔ position mappings, fine drag, wheel, keys, snapping, envelope/LFO/step-grid handle maths | all five (JUCE-free) |
| UiFeed | peak and gain-min protocols; seqlock under a writer thread for 10⁶ iterations with a checksum invariant; `seqStep` equals the sequencer's step; held/sounding masks for Whole/Dual/Split; audio unchanged (render regression, block-size tests) | all five, plus both sanitizer runs |
| UI state | round trip; defaults for a missing or garbled child; the Wave 4 fixture loads; UI state never marks modified, is not in programs | all five |
| Derived pictures | the 9.4 column "Test" | all five |
| Layout snapshot | per page × part × scale (100 %, 150 %): the component bounds tree as text equals a golden file in `tests/ui/golden/` exactly (layout never depends on text metrics, 9.2.4) | all five |
| Paint allocations, open/close, two instances | 9.5 | all five; open/close also under Linux ASan/UBSan (the editor tests are skipped only under **macOS** ASan, where CoreGraphics reports, as today) |
| `host_smoke` | additionally opens the plugin's own view through the VST3 host, resizes to 75 % and 200 %, closes | all five |
| pluginval strictness 10 | its editor tests (open, close, parameter changes with the editor open) | macOS locally; Windows and Linux x86_64 in CI (Linux: drop `--skip-gui-tests` and run under xvfb when CI returns) |

- **Image snapshot tests: no.** Glyph rasterisation (CoreText, DirectWrite, FreeType), antialiasing
  and the renderers differ per platform and per OS update; a tolerance loose enough to pass
  everywhere would miss real regressions, a strict one would fail on every machine. The layout
  snapshot (exact, text-free) and the binding tests cover what can break silently. A hidden test
  `[.uishots]` writes every page × part at 100 % and 200 % to PNG for human review (macOS), never compared.
- Manual: `docs/ui-test-checklist.md` "Custom editor" (owner, in hosts).

### 9.7 Milestones
M7 code starts only after **S1a has merged** (both touch the editor and the processor's block).
| Step | Scope | Acceptance gates | Agents (model) | Depends on |
|---|---|---|---|---|
| **M7a** | 9.1 (feed + step 9 contract commit, `EditorState`, mirror, gestures), `Theme`, models, Slider, SegmentedSwitch, Toggle, GraphView, ResponseCurveView, ScopeView, frame (top bar, rail, part switch, help line/bubble, meter, popover), `HelpTexts`, `ControlRegistry`, `FilterResponse`, **Sound page**, Size menu; other pages are placeholders that list their parameters with plain sliders so all 172 are reachable from day one | 9.6 rows: help texts, registry (complete via placeholders), automation follow, gestures, models, feed, UI state, filter curve, layout snapshot for Sound; 9.5 tick and paint budgets; `realtime-review` of the feed; preflight full | implementer **opus**; test agent **sonnet** (writes 9.6 tests from this contract, not from the code); fresh reviewer **opus** | S1a merged |
| **M7b** | Envelopes + LFO pages, EnvelopeView, LfoView | their rows of 9.6 and 9.4 | implementer **sonnet**; reviewer **sonnet** | M7a |
| **M7c** | Play + Effects pages, KeyboardStrip, effect pictures, power switches | their rows | implementer **sonnet**; reviewer **sonnet** | M7a |
| **M7d** | Arp + Seq page, StepGrid, StepTypeToggle, PianoRollView, ArpSeq accessors | preview == processor test; seqStep feed; gestures of painting; `realtime-review` of the accessors | implementer **sonnet**; reviewer **opus** (preview equivalence, feed) | M7a |
| **M7e** | Browser, `PresetActions` extraction, `slotMeta`/`bankGeneration`/`applyToneFromSlot` | preset tests unchanged + new API tests; tone-source flows; save flows from the browser and top bar | implementer **sonnet**; reviewer **opus** (files are untrusted input) | M7a |
| **M7f** | Polish: font (U2), final colours incl. `cat.*`, theme (U3), live markers (U4) if chosen, accessibility pass, HiDPI and Linux checks, budgets 9.5 measured, placeholder pages removed, docs (README, checklist, CHANGELOG) | all of 9.6 green; budgets met; manual checklist run by the orchestrator where possible | implementer **sonnet**; independent reviewer **opus** | M7b–M7e |
- M7b, M7c, M7d and M7e run **in parallel** after M7a: each adds new files only. A change to an
  M7a file (e.g. a new `GraphView` hook) is requested from the orchestrator and made in one serial
  step, so no two agents edit the same file.
- **Owner review points:** (1) end of M7a: the first clickable build (Sound page, frame, live
  automation, size); (2) end of M7b–M7e: all pages; (3) end of M7f: visual polish and hands-on in DAWs.
  The owner's answers to 9.8 are needed before M7f, U1 and U2 ideally before M7a.

### 9.8 Owner decisions (2026-11-14)
**The owner approved the round-3 wireframes and chose the recommended option in every row below.**

| Q | Question | Options | Recommendation |
|---|---|---|---|
| U1 | Window size and resizing | (a) fixed aspect, 75–200 % by corner and Size menu; (b) a few fixed sizes only; (c) free aspect (layout reflows) | **(a)**: one layout to test, works in every host, crisp at any size |
| U2 | Font (embedded, so text looks the same everywhere; licence must allow bundling with an AGPLv3 binary) | Inter (SIL OFL 1.1); Atkinson Hyperlegible (SIL OFL 1.1, designed for low vision); Noto Sans (SIL OFL 1.1); DejaVu Sans (permissive Bitstream Vera licence); Roboto (Apache 2.0); or the system font (no file, looks different per platform) | **Inter** (OFL 1.1): clear at small sizes, tabular figures for values. OFL fonts may be bundled with any software, including AGPLv3; the font keeps its own licence file in the repository. Nothing is downloaded until you choose |
| U3 | Theme | dark only; dark + light | **dark only** for v1.0 (tokens make light possible later) |
| U4 | Graphs and modulation | (a) static setting only; (b) static plus a live marker where the sound is now (filter cutoff of the newest note, playing step, keys); (c) fully animated curves | **(b)**: the picture stays readable and draggable, and you still see the sound move; (c) costs CPU and makes handles move under the mouse |
| U5 | Delay and reverb on/off | (a) power switch that sets the level to 0 and back (no new parameters, 9.3.6); (b) new parameters `fx.delay.on`, `fx.reverb.on` (174 parameters; programs without them load as On) | **(a)**: the engine already treats level 0 as off; no state or program change |
| U6 | Value display of 0–10 panel parameters | (a) as the host shows them today, 0.0–10.0; (b) percent (changes the parameters' text in hosts too) | **(a)**: one text everywhere |
| U7 | Help bubbles | (a) on, after 600 ms hover, plus the help line; (b) help line only; (c) a switch | **(a)** for now; add a switch if it gets in the way |
