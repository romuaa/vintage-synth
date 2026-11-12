# Design decisions log

Decisions made during implementation, with the reason for each. Newest milestone first.
Anything marked **(review)** is a judgement call that the product owner may want to change. Items marked "confirmed by owner" were reviewed and accepted.

## M1 — single-part voice engine (2026-11-12)

### Panel interpretation
| Panel element | Decision | Why |
|---|---|---|
| Sync switch | **VCO-1 is the slave and is hard-synced to VCO-2.** | Matches the classic architecture, where VCO-2 is the modulator for both Cross Mod and Sync, and VCO-2's Lo Freq mode makes it a usable sync/FM master. The parameter ID stays `vco2.sync` because the switch sits in the VCO-2 section. |
| Cross Mod | VCO-2 → VCO-1 **exponential** audio-rate FM, max ±4 oct (squared taper). | Classic analog cross-mod behaviour; the squared taper gives fine control at low settings. |
| VCO-2 Lo Freq | VCO-2 stops tracking the keyboard and pitch bend and runs at 1–8 Hz (16'..2'), adjusted by Fine Tune. VCO pitch mods still apply. | Turns VCO-2 into a second, sync-capable LFO, as on the original. |
| Waveforms | Both VCOs offer Sine, Triangle, Saw, Square, Pulse, Noise. | The panel photo can't be read reliably per VCO, so a superset costs nothing. (confirmed by owner 2026-11-12) |
| VCO ranges | VCO-1: 32'..1' (6 steps). VCO-2: 16'..2' (4 steps). 8' = keyboard pitch. | As printed on the panel. |
| VCA 0-1-2-3 switch | **VCA LFO Mod depth** (tremolo dips of 0 / 25 / 50 / 100 %). | Its position under "LFO MOD" in the VCA section matches the original's 4-step VCA LFO switch. (confirmed by owner 2026-11-12) |
| ENV-1 shape switch | **ENV-1 polarity** (Normal / Inverted). Inverted gives -ENV-1 for pitch and VCF; for PWM it gives 1 - ENV-1 (PWM is unipolar). | The shape icon under ENV-1 marks the classic polarity switch. (confirmed by owner 2026-11-12) |
| ENV key follow (KYBD) | Per envelope on/off. When on, all times halve every 2 octaves above C4. | Common analog key-follow behaviour. |
| "1 / ALL / 2" ENV selector | UI edit target only (which envelope the faders edit). **Not a host parameter.** | Same reasoning as Panel Mode Lower/Upper. |
| VCF -12 dB | Output tapped after stage 2; **feedback still from stage 4.** | Resonance and self-oscillation behave the same in both slopes, and the rolloff is 12 dB/oct. A 2-pole loop can't self-oscillate. |
| HPF | 1-pole, non-resonant, 5 Hz (DC blocker) … 2.56 kHz. | The panel shows a single slider. |
| LFO waves | Sine, Triangle, Saw (falling), Square, Random (S&H), Noise (smoothed random at 8× rate). | Matches the six panel symbols. |
| LFO delay | Zero output for the first half of the delay time, then a linear fade-in over the second half. Re-triggered when a note is played with no other keys held. | Classic delayed-vibrato feel. |

### Ranges and tapers (`source/params/Mappings.h`)
These are the single source of truth: the parameter display text and the DSP both call these functions.

- VCF cutoff 20 Hz – 20.48 kHz, exponential (10 octaves). Env mod up to 7 oct. LFO mod up to ±4 oct (squared taper). Key follow 0–100 %.
- Resonance k = 4.15 × slider. It self-oscillates from about 96 %.
- Envelope times 1 ms – 10 s, exponential. Attack time = time to reach the peak (RC charging toward 1.3).
  Decay and release time = time to cover 99 % of the way to the target.
- LFO rate 0.05 – 40 Hz. LFO delay 0 – 5 s (squared taper).
- VCO pitch mod: LFO up to ±2 oct and ENV up to +4 oct, both squared taper. Fine tune ±1 semitone.
- VCA level is squared (perceptually even).
- Pitch bend is fixed at ±2 semitones for now. It becomes a setting in M2/M3.

### DSP architecture
- **Oscillator anti-aliasing: windowed-sinc BLEP/BLAMP table** (16 taps, Kaiser β = 8, cutoff 0.45 × oversampled rate)
  instead of 2-point PolyBLEP. PolyBLEP measured only −58 to −65 dB alias at 1–2 kHz fundamentals at 2×.
  The table version measures **≤ −113 dB for every wave up to 5 kHz**, and the test gate is set at −100 dB.
  The cost is 16 multiply-adds per waveform edge plus 8 oversampled samples of delay (4 base samples).
- **Oversampling: 2× real-time, 4× offline** (`isNonRealtime()` at prepare time). It uses JUCE's polyphase IIR
  decimator with a custom stage (transition 0.05, −100 dB). Voices render directly at the oversampled
  rate; the "upsampling" pass is fed silence, because JUCE's API needs it before decimating.
- **Latency is reported** to the host (decimator + BLEP delay, about 7 samples at 2×).
- **VCF: ZDF 4-pole OTA cascade.** The feedback is solved linearly, then passed through tanh at the cascade input.
  There's no iterative solve. Tests confirm exact cutoff (−12.04 dB / −6.02 dB at fc in the linear region),
  correct slopes, and self-oscillation at fc ±3 %.
- **Parameter handling: a single X-macro list** (`PartParamList.h`) generates the snapshot struct, the
  APVTS layout for both parts, the atomic binding and the per-sample smoothing (5 ms one-pole).
  Switches are applied per block without smoothing.
- **Both parts' parameters exist now** (`lower.*` and `upper.*`); only Lower is played until M3.
  Parameters can't be added later without appending, so defining the full set now keeps the order logical.
- **Vintage** (global, default 0 = clean). Per voice it controls static VCO offsets (±6 ct), slow pitch drift (±3 ct),
  cutoff offset (±0.12 oct) and envelope time spread (±8 %). It's seeded per voice, so renders are deterministic.
- **Mono note handling in M1**: last-note priority. Envelopes retrigger on each new note; returning to a held
  note changes pitch without a retrigger. M2 replaces this with the polyphonic allocator.

### Process
- **State version stays 1 until v1.0.** There are no released states to migrate, so pre-release parameter
  changes don't bump `stateVersion`. The append-only and migration rules apply from the first release.
- Test-only renders go to `$VS_RENDER_DIR`, because `build/` is ignored by git.
