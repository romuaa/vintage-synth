# CLAUDE.md — Vintage Synth (VST3 instrument)

Bi-timbral, 16-voice-per-part analog-style polysynth plugin: two VCOs (sync, cross-mod), HPF,
12/24 dB VCF, VCA, 2× ADSR, LFO with delay, Lower/Upper parts (Whole/Dual/Split), Solo/Unison/Poly,
arpeggiator, 16-step sequencer, patch banks, Chorus/Delay/Reverb sends.

The panel is functionally inspired by a classic 80s poly. **Never use third-party brand names,
logos, or copied panel artwork** in code, UI, presets, or docs. The look and naming are our own.

## Open decisions (do not assume — ask if work depends on them)

None right now. When a new one comes up, list it here; once decided, move it to "Decisions".

## Decisions

- **License: JUCE under AGPLv3**, so this whole project is AGPLv3 (`LICENSE`). Every binary
  release must ship with (or link to) its complete source. Don't add dependencies that are
  incompatible with AGPLv3 (check before adding any).
- **Format: VST3 only.** No AU, CLAP, or standalone (add to `FORMATS` only if decided later).
- **Voices: 16 per part** (32 total in Dual mode). This is the CPU budget target.
- **Character:** clean by default. One "Vintage" amount controls drift, voice-to-voice variation,
  and filter/envelope tolerance.
- **Stack:** JUCE 9.0.5 and Catch2 v3.16.1 as git submodules in `external/`, pinned to exact tags.
  Upgrade them deliberately, in their own commit. C++20, CMake ≥ 3.28, Ninja on macOS.
- **Targets:** macOS universal (`arm64;x86_64`, min macOS 11, the default in `CMakeLists.txt`); Windows
  x64 + ARM64 merged into one `.vst3` bundle (`Contents/x86_64-win`, `Contents/arm64-win`) by CI.
  MSVC static runtime.
- **Identity:** company "Exitium", bundle ID `com.exitium.vintagesynth`, manufacturer code
  `Exit`, plugin code `Vsy1`. **Never change these after the first release**, because hosts identify
  the plugin by them.
- **CI:** `.github/workflows/build.yml` builds, tests, and runs pluginval on macOS and Win x64.
  Win ARM64 is build + unit tests only (there's no ARM64 pluginval).

## Layout

```
source/   dsp/ engine/ fx/ params/ presets/ ui/   PluginProcessor.*  PluginEditor.*
resources/ factory_presets/ fonts/ graphics/
tests/    dsp_unit/ render_regression/ benchmarks/
installer/ mac/ win/
.github/workflows/
```

## Hard rules

**Real-time audio thread** (`processBlock` and everything it calls):
- No allocation, locks, logging, file I/O, exceptions, or blocking system calls.
- `juce::ScopedNoDenormals` at block start. Smooth continuous params; never smooth switches.
- MIDI is sample-accurate: split the block at event offsets.
- UI ↔ DSP only through atomics or lock-free SPSC FIFOs. Preset loads are built off-thread
  and swapped in atomically.

**Parameters and state:**
- Parameter IDs are dotted and stable: `lower.vcf.cutoff`, `upper.lfo.rate`, `fx.reverb.level`.
- After v1.0 IDs are **frozen**: never rename, remove, or reorder. Only append new ones.
- Every state change bumps `stateVersion` and gets a migration plus a test that loads old state.
- Panel Mode (Lower/Upper edit target) is UI state, not a host parameter.

**DSP quality:**
- Oscillators use PolyBLEP/BLAMP. Sync and cross-mod reset at the exact sub-sample point.
- The voice path is 2× oversampled (4× HQ option, also used for offline renders).
- The VCF is a ZDF/TPT 4-pole with a nonlinear feedback path.
- Analog drift is seeded per voice, so renders are deterministic.

**Portability:** no x86-only intrinsics without a NEON path and runtime dispatch. Every change
must build for all three targets (mac universal, win x64, win ARM64).

## Definition of done (per change)

1. Builds clean (warnings as errors) on all targets → skill `build`.
2. Unit + render-regression tests pass; pluginval strictness 10 passes → skill `validate-plugin`.
3. Audio-thread code was checked → skill `realtime-review`.
4. Any parameter or state change followed skill `add-parameter`.

Say plainly which of these steps you could not run locally (e.g. Windows ARM64 runs in CI only).

## Project skills (`.claude/skills/`)

| Skill | Use when |
|---|---|
| `build` | Configuring or building, or checking binary architectures |
| `validate-plugin` | Running tests, pluginval, VST3 validator, golden renders |
| `add-parameter` | Adding or changing any parameter, preset field, or saved state |
| `realtime-review` | Writing or reviewing any code reachable from `processBlock` |
| `release` | Signing, notarization, installers, version bumps |

## Ending a turn — "What I need from you"

If a turn stops because the next step is the user's, end it with a section titled
**"What I need from you"**.

- **Numbered list.** Each item is one concrete action or one decision.
- **Each item says** what to do, where to do it (command, file, app, URL), and what to send back.
- **Order:** the most blocking or quickest item first.
- **After the list,** one line on what you carry on with meanwhile, or "Blocked until #1".
- **Decisions:** give the options and mark your recommendation.
- **Never bury a request** mid-update. If you need it, it goes in this section, even if it
  was mentioned earlier in the turn.
- **Nothing needed:** end with "Nothing needed from you right now" plus whether the work is
  **finished** or **still going** (and what's next).

Example — user action needed:

```
## What I need from you
1. Install the build tools: run `! brew install cmake ninja` here. Send nothing back; I'll see the output.
2. Choose the JUCE license: commercial (closed source, paid) or AGPLv3 (source must be published).
   I recommend commercial if you plan to sell. Reply "commercial" or "AGPL".
3. Apple notarization: put your Team ID in `cmake/signing.cmake` → `APPLE_TEAM_ID`, then reply "done".

Meanwhile I'm writing the oscillator unit tests, which don't depend on any of these.
```

Example — nothing needed:

```
Nothing needed from you right now — M1 is finished: all tests and pluginval pass on macOS.
Windows ARM64 is still building in CI; I'll report when it's green.
```

Bad (avoid): "…filter is done. Also could you maybe check the license thing at some point? Next
I refactored the LFO…" A request buried in the middle like this is easy to miss.
