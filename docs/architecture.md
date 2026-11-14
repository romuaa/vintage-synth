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
| 3 | Triangle output = bits 22..11 of (accumulator XOR (top bit ? all ones : 0)). With ring on, the top bit used for the fold is the own top bit XOR the ring source's top bit. |
| 4 | Pulse output = 0xFFF when accumulator bits 23..12 ≥ PW, else 0. The test bit forces it high (confirm). |
| 5 | Noise: 23-bit shift register clocked on every rising edge of accumulator bit 19; new bit 0 = bit 22 XOR bit 17; output bits 11..4 = register bits 20, 18, 14, 11, 9, 5, 2, 0; output bits 3..0 = 0. |
| 6 | Sync: a rising edge of the source's top bit resets the target's accumulator to 0 in the same cycle. Sources (also for ring): 1 ← 3, 2 ← 1, 3 ← 2. Chains (a source reset in the same cycle): confirm. |
| 7 | Test bit: accumulator held at 0 while set; the noise register is reset (value and timing: confirm). |
| 8 | Combined waveforms (several waveform bits): the chip's combined output per model (8.4.5). A combination with noise also writes the combined output back into the noise register's output bits, so noise can lock up (silent) until the test bit resets it. |
| 9 | Envelope rate periods in cycles, nibble 0..15: 9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251. At about 1 MHz: attack 2 ms … 8 s; decay and release 3 × those. |
| 10 | Attack: counter + 1 per period up to 0xFF, then decay. Decay and release: − 1 per (period × divider); divider 1 above 0x5D, 2 from 0x5D, 4 from 0x36, 8 from 0x1A, 16 from 0x0E, 30 from 0x06 (exact threshold timing: confirm); the counter stops at 0 and does not wrap. Sustain level = nibble × 0x11; lowering it during sustain lets the counter decay to the new level, raising it does not raise the counter. |
| 11 | Rate counter: 15 bits, compared for equality with the period and reset on a match. After a change to a period below the current count the envelope waits until the counter wraps (up to 32 768 cycles, about 33 ms): the **envelope delay quirk**. |
| 12 | Gate: a rising edge starts attack, a falling edge starts release, from the current counter. |
| 13 | Read registers: 0x1B = oscillator 3 output bits 11..4; 0x1C = oscillator 3 envelope counter. |
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
| Board filter | 1-pole high-pass about 16 Hz and 1-pole low-pass about 16 kHz (typical coupling and output network of a host board) | same |

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
| Digital unit gates | every row of 8.4.3; closed form = a naive **per-cycle stepper** written by the test agent from 8.4.3 alone, for 10^8 random cycles with random writes, both models | no |
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
(hex; one line in the file). `.bsspec`: the trace's output at 48 kHz reduced to the render-regression
feature format, plus a 1/12-octave averaged spectrum per marked segment for the analog
measurements. Never audio.

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
| Scripted traces: band energy per third-octave per 25 ms frame | ±3 dB per cell with ≤ 5 % outliers, mean deviation ≤ 1 dB; level ±0.5 dB |

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
