# Design decisions log

Decisions made during implementation, with the reason for each. Newest milestone first.
Anything marked **(review)** is a judgement call that the product owner may want to change. Items marked "confirmed by owner" were reviewed and accepted.

## Wave 0 — contracts for M2–M5 (2026-11-12)

The interfaces are in `docs/architecture.md`. These are the judgement calls behind them.
All are marked **(review)**: the owner has not confirmed them yet.

| Decision | Why |
|---|---|
| **Stolen voices fade out over 3 ms, then start the new note** (the note starts 3 ms late). No spare voices for the fade. (review) | Click-free with a fixed 16 voices and a predictable CPU cost. 3 ms is inside the 2–5 ms spec and below audible timing error. |
| **Steal order:** oldest voice in release, else oldest held voice, never the newest. (review) | Releasing voices are the least audible to lose; protecting the newest note keeps the melody intact. |
| **Unison:** 2/4/8/16 voices (default 4), total detune spread 0–50 ct, output scaled by 1/√n. (review) | A selectable count keeps CPU in the user's hands; 1/√n keeps loudness steady when the count changes. |
| **Portamento is an RC glide in pitch**, time = 99 % of the interval, modes Always/Legato. (review) | Matches the analog behaviour and the envelope time definition already used. |
| **Velocity:** two per-part amounts, to VCA gain (default 0.5) and to VCF envelope depth (default 0). (review) | The classic had no velocity; a modest VCA default suits modern keyboards, and amount 0 restores the original behaviour. |
| **Sustain pedal (CC 64) and mod wheel vibrato (CC 1) are part of M2.** (review) | A polysynth without a sustain pedal is not playable; the wheel maps to the classic bender-section LFO vibrato. |
| **Bend range per part, 0–12 semitones, default 2.** | Replaces the fixed ±2 from M1, as planned. |
| **Parts are mono up to the mixer; stereo comes from pan and effects.** No per-voice unison stereo spread. (review) | One decimator per part instead of two: the decimator is a fixed per-part cost in a tight CPU budget. |
| **Effects are global, in series on the stereo bus:** chorus → delay → reverb. Per-part sends are deferred. (review) | Simplest structure that gives the classic sound; per-part sends can be appended later without breaking IDs. |
| **Whole mode plays the Lower part only** (16 voices). (review) | Simple and predictable; a 32-voice single patch is not needed for the CPU target. |
| **MIDI is parsed once into plain `NoteEvent`s**; parts and the arpeggiator work on those. Fixed buffer; overflow drops the rest of the block and sends All Notes Off. | Lets the arp/sequencer be a pure, testable event transformer, and keeps `juce::MidiMessage` off the audio thread. Overflow can never hang a note. |
| **CPU optimisation is part of M2**, target ≤ 0.30 % of a core per voice. | The extended benchmark measures 0.35 % per voice, so 32 voices ≈ 11.5 % of the 15 % budget. M2 adds per-voice work and the effects need about 1.5 %, so some margin has to be won back. (The earlier 0.47 % figure was a single unrepeated run and was not reproduced.) |
| **Golden renders compare perceptual features, not samples.** | arm64/x64 and clang/MSVC math libraries differ in the last bits; a self-oscillating filter amplifies that. |

### Process (owner decisions, 2026-11-12)
- **Orchestrated development:** work is done by agents briefed against `docs/architecture.md`; the
  orchestrator plans, verifies and merges (skill `orchestrate`).
- **Git:** agents commit on feature branches, pushed as PRs so CI validates Windows. Merging to
  `main` needs the owner's approval per milestone.
- **Audition gates:** the owner listens at the end of M2, M5 and M6, and sets the UI direction before M7.

## CI and infrastructure (2026-11-12)

| Decision | Why |
|---|---|
| Actions pinned by full commit SHA (version in a comment); Dependabot updates them weekly, grouped. | Tags are mutable; SHA pins are the supply-chain best practice. Dependabot keeps pins current. |
| Runner images pinned (`macos-26`, `windows-2025`, `windows-11-arm`, `ubuntu-26.04`), not `-latest`. | Reproducible builds; image upgrades become deliberate changes. |
| Windows CI: Ninja in the VS developer environment of the newest installed VS (`vswhere`). | Runner images change VS versions; Ninja is needed for the compiler cache. |
| Compiler cache: ccache on macOS, sccache on Windows (`hendrikmuhs/ccache-action`). | ccache handles the universal `-arch` pair; sccache is the mature MSVC cache. |
| LTO only on the shipped plugin; tests/benchmark use a non-LTO build of the same sources (`vs_test_core`). | Saves ~5 min of link-time code generation per Windows job. The shipped binary is still validated by host_smoke and pluginval. |
| Trivy fails the build only on fixable HIGH/CRITICAL findings; everything else is reported in the SARIF artifact. | Actionable gate without noise. Code scanning upload is off (private repo, no Advanced Security); `vars.CODE_SCANNING=true` turns it on. |
| Dependabot submodule PRs are merged only when they point at a release tag. | JUCE `develop`/`master` commits are not releases; upgrades are deliberate. |

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
