# Design decisions log

Decisions made during implementation, with the reason for each. Newest milestone first.
Anything marked **(review)** is a judgement call that the product owner may want to change. Items marked "confirmed by owner" were reviewed and accepted.

## Product names (owner decision, 2026-11-12)

- **Every product name carries the company prefix: "Exitium Vintage Synth" and "Exitium B-Side".**
  This is the name shown in hosts, the bundle file name, the README and any site. Plugin codes and
  bundle identifiers do not change.
- The names are considered good enough for an open-source project and **can still be changed
  before the first release** if a conflict turns up. An official trademark search is optional, not
  a gate. After the first release the host-visible name stays.
- The rename of Vintage Synth is done at the start of M3 (it touches CMake, CI, tests and scripts).

## Wave 2 contracts (2026-11-12)

All (review).

| Decision | Why |
|---|---|
| Key modes: Whole = Lower only; Dual = both; Split = below the split point to Lower. The router remembers which part received each key's note-on. | A moved split point can never hang a note. |
| Changing the key mode releases all voices and forgets held keys. | Same rule as the assign-mode change in M2. |
| Balance is constant-power with both parts at unity in the centre; pan per part. Mono output is (L + R) / 2. | Predictable levels; Dual at centre is simply the sum of both parts. |
| Oversampling follows the host rate: 2× below 88.2 kHz, 1× at or above (offline 4× / 2×). | Keeps the internal rate at 88.2 kHz or more everywhere and brings 192 kHz sessions back inside the CPU budget (was 18 % for 32 voices). |
| MIDI stays omni; no per-part channels yet. | Not needed for Whole/Dual/Split; can be appended later. |
| Arpeggiator and sequencer follow the host transport when it plays and an internal clock when it is stopped. | Works in a DAW and when just playing the plugin in a host. |
| Arp modes: Up, Down, Up & Down, Random, As Played; 1–4 octaves; hold; swing; gate length. Sequencer: 16 steps with offset, Note/Rest/Tie and velocity; transposed by the lowest held key. | The classic set plus the two things players expect today (as-played order, swing). |

## M7 UI direction (owner decision, 2026-11-12)

- **The custom UI does not reproduce the reference panel's layout.** It gets its own arrangement,
  designed for plugin use (screen proportions, mouse and trackpad, resizing, clear signal flow,
  controls that are inactive in the current mode shown as inactive), with our own look and naming.
  The reference panel only defined *which functions exist*.
- Consequence for M7: the first deliverable is a layout proposal for the owner to choose from,
  before any UI code. The visual style is still to be decided with the owner.
- Noted from hands-on testing: Unison Detune and Unison Voices must look inactive outside Unison mode.

## Second instrument: chip synth (owner decision, 2026-11-12)

| Decision | Why |
|---|---|
| **A second instrument is planned:** a polyphonic synth modelled on a classic 3-voice sound chip from early-1980s home computers. (owner) | Owner's request. |
| **Separate plugin in this repository, sharing a common library** extracted after M3. | A host identifies a plugin by its codes and parameter list; the instruments share neither parameters nor presets, but they do share the allocator, events, parameter plumbing, effects, tests and CI. |
| **Own behavioural model, not an embedded emulator.** Digital parts bit-exact, analog parts modelled, one whole chip voice per note. | Research: the established emulators simulate every chip clock cycle, so 16 notes need 16 chips at an estimated 17–26 % of a core or more (estimate from a stand-in benchmark, not the real engine); they allocate and start threads at setup, quantise pitch and cutoff, and their sources contain third-party brand names. The own model is estimated at 2.5–5 % for 16 voices. Their licence (GPL v2 or later) would have been compatible. |
| **Every patch has a program sequencer** (wave/note, pulse and filter lanes on a tick clock), hard restart is a patch feature with explicit policies, and tables are patch data with a text format. (owner prompt, research) | The owner pointed out that the signature sounds come from how the chip was driven, not from the chip alone. Research confirmed it: every serious playable instrument of this kind added a per-patch table sequencer, and the classic drums, arpeggio chords and attack snap cannot be made without one. It must be in the first sounding milestone so the engine is not designed into a corner. |
| **Program-driven values step at the tick, unsmoothed; automation stays smooth.** | The stepped vibrato, per-tick cutoff steps and restart click are part of the character; smoothing them would also break the envelope-quirk timing. |
| **Filter per note by default, with a shared-filter topology per part.** | Per-note filters make a playable poly; the shared filter is needed for paraphonic patches, the early revision's summed overdrive, and replaying register traces against the oracle. |
| **Chip arpeggio (in-voice, tick rate, gate held) is separate from the shared arpeggiator.** | They are different things musically and technically. |
| **An established emulator is the test oracle, in CI only.** Never vendored, linked or shipped; reference data is committed under neutral names. (review) | Authenticity must be measurable; the brand rule and the real-time rules stay intact. |
| **Own name; no official names, part numbers or nicknames.** Revisions are "Model A" and "Model B". (owner) | Project brand rule; the original computer brand is in active commercial use again. |
| **Product name: "B-Side"** (owner). Always shown with the company name ("Exitium B-Side") in the plugin name, metadata, README and site. "Broadside", "Topside", "Flipside" and "Vicious" are reserved for modes and preset banks. | A music term that also nods at the chip. A web check found no synth, plugin or hardware with this name, but the term is common and crowded in search; US trademark filings for "B SIDE" exist in software and DJ-equipment classes for unrelated goods. **Open: the owner runs the official trademark searches (USPTO, EUIPO/TMview, WIPO, UKIPO; classes 9, 15, 41, 42) before release.** "Vicious" as a bank name could be read as a nod to an existing preset maker with a longer name (low to moderate risk). |
| **Listening reference: public recordings of the original hardware, by the owner's ear.** They are not downloaded into the repository or used as test data. | Recordings are lossy, of unknown chip revision, and copyrighted; the oracle covers objective tests. |

## Wave 1 — M2 polyphony, effect blocks, owner decisions (2026-11-12)

### Public-release audit (2026-11-12)
- Full-history scan found no secrets, keys, home-directory paths or third-party brand names. Three
  comments that pointed at specific hardware (a filter chip type, "panel photo", "the original")
  were reworded.
- pluginval downloads in CI get a SHA-256 check; CI artifacts are kept 7 days.
- Added `SECURITY.md`, `CONTRIBUTING.md` (AGPLv3-only contributions) and a PR template.
- Statements about the repository being private and CI cost are updated in the follow-up change
  right after the visibility flip, together with removing the draft-PR cost tier.

### Owner decisions and approvals
- **M2 audition approved by ear** (polyphonic demo: pads, steals, unison lead, pedal, velocity, solo bass).
- **Effects, first listen on the standalone blocks:** chorus approved at the stronger maximum;
  delay approved with a 700 ms default time and about 0.4 as a typical level; reverb liked. The
  settings of all three get their final evaluation in the plugin (M5 audition).
- **The repository goes public after Wave 1 is merged** (free CI runners, code scanning). Before the
  flip: history scan for secrets and brand names, `SECURITY.md`, `CONTRIBUTING.md`, branch protection.
- **Contributions are accepted under AGPLv3 only**, no contributor agreement.
- **Commit author e-mail stays as it is** (no history rewrite).
- **Linux x86_64 VST3 becomes a supported target**, built and validated in CI with a sanitizer job;
  the owner can test manually in a DAW on Linux. Work starts after Wave 1 merges.
- **Plugin host for hands-on testing: JUCE's AudioPluginHost**, built from the JUCE submodule
  (`scripts/dev-host.sh`). No new dependency.

### M2 behaviour (review unless marked)
| Decision | Why |
|---|---|
| A voice that starts from idle gets fresh oscillator start phases and noise seeds from its own deterministic generator, and its filters and ENV-1 are reset. A re-played Solo note no longer continues the previous phase. | A voice that stops inside a sub-block keeps evolving to the sub-block's end, so anything inherited by the next note made renders depend on the host block size (up to −5.7 dB in Solo). Free-running oscillators behave this way on the analog original. |
| Unison voices starting from idle share one base phase per note plus a fixed offset of k²/(2n) cycles for voice k. | With independent random phases, unison at detune 0 had a random level per note (−4.8 to −26.7 dB with 16 voices). This pattern sums to exactly √n, so the level equals one voice at any detune. At detune 0, even harmonics get a fixed colouring. |
| Unison retriggers and returns legato exactly like Solo. | The contract only defined it for Solo; one rule for both mono modes. |
| "A key held" means a physically held key for Legato portamento. For the LFO-delay restart, notes held by the sustain pedal also count. | Legato is a playing technique; but delayed vibrato must not drop out on ringing pedal notes when the next note is played. |
| In Poly, every new note glides from the previously played note. | As specified; chords glide internally when portamento is on, as on the classic. |
| A stolen note keeps its gate length: start and release are both delayed by the 3 ms fade. A steal of a voice whose envelope has already ended starts at once. | A stolen note shorter than 3 ms was silently dropped. |
| Stealing: "oldest in release" is the voice that entered release first; pedal-held voices count as held. Re-pressing a key retriggers its releasing or pedal-held voice. | Simple, deterministic rules. |
| A mode or unison-count change releases all voices and forgets held keys (they must be re-pressed). | Avoids stuck or orphaned notes across a mode change. |
| Idle voices are reported in the order they finished (sample-exact). | "Idle longest" must not depend on block splitting; it changed which voice a note landed on. |
| Envelope key follow follows a legato note change at the event. | Matches a keyboard CV; the times used to change one host block late. |
| Mod wheel: wheel × depth is added to the VCO LFO mod slider before the taper, capped at the slider maximum; it is subject to the LFO delay. | Same control path as the slider. |
| Portamento time applies per block; unison detune, velocity amounts, wheel depth and the wheel are smoothed. Retriggering a sounding voice smooths velocity and unison level over 5 ms. | No zipper noise or gain steps. |
| Pitch glide and drift are computed from absolute time in double precision, rounded once. | Per-sub-block float rounding integrates into phase (−74 dB between block sizes for glide). Now −118 dB or better with 16 voices at full Vintage. |
| Bend range labels are "0 st" … "12 st"; the velocity parameter names contain an arrow (UTF-8, guarded by a test). | Readability; "Velocity to VCA" is the fallback if a host shows it badly. |
| **CPU at high session rates (open):** cost scales with the sample rate. 32 voices at 2× cost 5.0 % of a core at 48 kHz but 18.2 % at 192 kHz, above the 15 % budget, which is defined at 48 kHz. Planned for M3: 1× oversampling when the host rate is 88.2 kHz or higher. | The internal rate is then at least 88.2 kHz at every host rate, which is what the alias gate needs. |

### Effect blocks (review unless marked)
| Decision | Why |
|---|---|
| Chorus mix curve: wet gain = mix^1.152 (0.45 at the default mix 0.5, 1.0 at full mix). The part of the wet signal that differs between left and right is capped at 0.7, and above the default the output is level-compensated. (strength confirmed by owner) | Full mix would otherwise cancel completely in mono; the worst mono dip at full mix is −12.5 dB, and −5.1 dB at the default. |
| A settled delay time is rounded to a whole sample (echo spacing within ±0.5 sample). | At fractional delays the interpolator darkened every repeat (0.5 dB per repeat at 10 kHz), depending on time × sample rate. |
| Delay time changes glide like tape (pitch bends during the change). | Click-free and musical. |
| Reverb high-frequency damping is limited at short decays (ratio not below about 0.37 at 1 s). | Heavier damping left a sparse, ringing tail above 15 kHz. |
| Reverb gets louder with longer decay (wet −13 dB at 0.5 s, +5 dB at 20 s at level 1). | Natural behaviour of the network; to be judged at the M5 audition. |
| Delay default time 700 ms, default level 0 (off). (confirmed by owner) | Owner's choice after listening. |

## Wave 1 — M2a, test infrastructure, CI cost (2026-11-12)

| Decision | Why |
|---|---|
| **Fast `exp2` / `tan` / `tanh` approximations in the voice path**, with tested bounds (relative error ≤ 2e-7 / 5e-7, absolute ≤ 2e-7; static pitch error ≤ 0.009 cent). They return NaN for NaN. | Profiling showed the voice is limited by its serial dependency chain per sample, where libm calls cost far more than in isolation. NaN must still reach the output guard. |
| **Voices render sample by sample across all voices** (interleaved, groups of 32) after a vectorised per-voice block-prepare phase; shared modulation is computed once per part in sub-blocks of up to 32 oversampled samples. | Interleaving lets the CPU overlap the voices' serial chains: 32 voices went from 11.3 % to 4.7 % of one core. Fully voice-by-voice rendering measured several times slower. |
| **Parameter smoothing stays per sample (closed form), not control rate.** | Linear control-rate ramps measured −55 dB of error on fast cutoff moves. |
| **Smoothers and the drift walk are kept as target + deviation.** | A plain one-pole stalls short of its target by an amount that depends on the step size, which made renders depend on block splitting and left tiny permanent pitch/cutoff offsets. |
| **Renders are independent of host block size and event positions at any Vintage amount** (tested: −110 dB between block sizes 512 and 500 at Vintage 1). | Offline bounces and golden renders must be reproducible. |
| **All Notes Off (CC 123 and the overflow marker) also clears the sustain pedal;** Reset Controllers clears bend, mod wheel and pedal. | A held pedal must never be able to keep notes hanging after a panic or an event overflow. |
| **Every MIDI event is applied exactly once:** before the block → offset 0, at or after its end → after the last sample, zero-length blocks still apply events. Malformed channel messages are dropped. | A note-off at the block boundary was being lost (possible hung note); masking bad data bytes could turn garbage into All Sound Off. |
| **Golden renders compare third-octave band energy per 25 ms frame, level envelope and tail** (3 dB per cell with ≤ 1 % outliers, 0.4 dB mean, 2 dB level, −80 dB gate). References store features, not audio. | Verified in CI: a reference generated on arm64 matches on x64 and with MSVC. Detects a 3.5 % cutoff change, a missing note or a 25 ms timing change. |
| **VST3 validator: built in CI only** from Steinberg's SDK at `v3.8.1_build_84` (pinned by commit SHA, MIT), cached, run as ctest `vst3_validator` on macOS and Windows x64. The test requires the validator's own "N tests passed, 0 tests failed" line. (owner decision) | JUCE does not bundle the validator; nothing is added to the repo or linked into the plugin. Exit code alone passed in 0.2 s and proved nothing. |
| **CI cost control while the repo is private:** docs-only changes don't build; draft PRs build Windows x64 only, the full matrix runs when a PR is marked ready, on `main` and on manual dispatch; 30-minute job timeout; Dependabot monthly. (review) | macOS minutes are billed at a multiple; MSVC is the one thing that cannot be checked locally. To be relaxed when the repo goes public (free standard runners). |
| **Push discipline:** one push per branch, after local tests, a local AddressSanitizer/UBSan run for engine changes, and the review fixes. Docs are batched with code. | 13 CI runs on the first orchestrated day, several of them avoidable. |
| **Effects (owner feedback and review):** chorus full mix reaches wet ≈ dry, with the default at the earlier maximum; no bulk clearing on the audio thread; reported tail capped at 30 s; level 0 fades out in 20 ms and drops the tail (review). | See `docs/architecture.md` section 5. |

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
| VCO-2 Lo Freq | VCO-2 stops tracking the keyboard and pitch bend and runs at 1–8 Hz (16'..2'), adjusted by Fine Tune. VCO pitch mods still apply. | Turns VCO-2 into a second, sync-capable LFO. |
| Waveforms | Both VCOs offer Sine, Triangle, Saw, Square, Pulse, Noise. | The reference layout is ambiguous per VCO, so a superset costs nothing. (confirmed by owner 2026-11-12) |
| VCO ranges | VCO-1: 32'..1' (6 steps). VCO-2: 16'..2' (4 steps). 8' = keyboard pitch. | As printed on the panel. |
| VCA 0-1-2-3 switch | **VCA LFO Mod depth** (tremolo dips of 0 / 25 / 50 / 100 %). | Its position under "LFO MOD" in the VCA section matches the usual 4-step VCA LFO switch of this kind of panel. (confirmed by owner 2026-11-12) |
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
