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
- **pluginval: Claude runs it itself** (official Tracktion release v1.0.4, unpacked in the session
  scratchpad). Don't hand it to the user unless the harness refuses the command. See skill `validate-plugin`.
- **Repo:** GitHub `romuaa/vintage-synth` (private), branch `main`. Commit and push only when the user asks.
- **CI:** `.github/workflows/build.yml` builds, tests, and runs pluginval on macOS and Win x64.
  Win ARM64 runs build, unit tests and host_smoke (there's no ARM64 pluginval). Check results with
  `gh run list` / `gh run view <id> --log-failed` (gh is installed and authenticated). Never claim
  Windows is green without a passing run. CI builds every target with Ninja and a compiler cache
  (ccache on macOS, sccache on Windows) inside the newest Visual Studio found by `vswhere`; never
  hardcode a VS version. LTO applies to the shipped plugin only; tests and `voice_bench` link
  `vs_test_core` (same sources, no LTO), so add new source files to `VS_PLUGIN_SOURCES`.
  Runner images are pinned (`macos-26`, `windows-2025`, `windows-11-arm`, `ubuntu-26.04`);
  Dependabot doesn't bump them, so check github.com/actions/runner-images when touching CI.
  Pushing to `main` cancels a running build of `main` (concurrency group): wait for a run you
  still need before pushing again.
- **Supply chain:** every third-party action is pinned by full commit SHA with a `# vX.Y.Z` comment.
  `.github/dependabot.yml` updates Actions and submodules weekly. Before merging a submodule PR,
  check it points at a release tag and update the versions named here and in README.
- **Security scan:** `.github/workflows/security.yml` runs Trivy (vuln, secret, misconfig) on push,
  PR and weekly; HIGH/CRITICAL fixable findings fail it. The repo is private without code
  scanning, so results are in the job log and the `trivy-results` SARIF artifact. Setting the repo
  variable `CODE_SCANNING=true` turns on the SARIF upload once code scanning is available.
- **Panel interpretation, DSP ranges and design choices:** see `docs/decisions.md`. Add every new
  judgement call there (with the reason), and mark the ones the user should review.

## Status

- M0 skeleton: done. M1 single-part voice engine: done (mono, Lower part only; demo approved by ear;
  all panel interpretations confirmed by the owner).
- CI: green on macOS universal, Windows x64 and Windows ARM64 (build, tests, host_smoke, pluginval
  where available); Trivy green; Dependabot active. History: `CHANGELOG.md`.
- Open CI item: confirm the compiler cache pays off. The first (cold) run after enabling it took
  macOS 14 / Win x64 21 / Win ARM64 15.5 min. On the next push, compare the times and the cache hit
  rate. If the warm run isn't clearly faster, revisit (`docs/engineering-notes.md`, CI section).
- **Next: M2 polyphony** — 16 voices per part, Poly/Unison/Solo assign modes, portamento, voice
  stealing with a 2–5 ms fade, velocity, configurable bend range. Replaces the mono last-note logic
  in `engine/Part`; keep `voice_bench` within the CPU budget (see skill `validate-plugin`).
- Later: M3 Upper part + Whole/Dual/Split, M4 arpeggiator + 16-step sequencer, M5 chorus/delay/reverb,
  M6 presets/banks + golden renders, M7 custom UI, M8 hardening/signing/release.

## Layout

```
source/dsp/      JUCE-free DSP blocks: Oscillator (+BlepTable), Filters, Envelope, Lfo, Drift
source/engine/   Voice, Part (note handling, LFO, oversampling), Oversampler
source/params/   PartParamList.h (X-macro list of all per-part params), Mappings.h (value -> unit),
                 ParameterLayout (APVTS layout + binding), StateVersion.h
source/          PluginProcessor.*   (later: fx/ presets/ ui/)
tests/dsp_unit/  Catch2 tests; RenderDemo.cpp is a hidden "[.render]" audition render
tests/benchmarks/ voice_bench (CPU cost per voice)
tests/host_smoke/ loads the built .vst3 through JUCE's VST3 host (ctest "host_smoke"; runs on every CI target)
docs/decisions.md  design decision log (what we chose and why)
docs/engineering-notes.md  lessons learned (traps, causes, rules); read before starting a milestone
README.md        public overview, build and test instructions
CHANGELOG.md     what was done, per milestone (update it with every milestone/infra change)
.github/         workflows/build.yml (build+test+pluginval), workflows/security.yml (Trivy), dependabot.yml
external/        JUCE, Catch2 (submodules)
```

**Adding a per-part parameter:** add one line to `VS_PART_FLOAT_PARAMS` or `VS_PART_CHOICE_PARAMS`
in `PartParamList.h` (append at the end). The layout, binding and smoothing follow automatically.
Put any physical mapping in `Mappings.h` so the display text and the DSP agree.

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
- From v1.0 on, every state change bumps `stateVersion` and gets a migration plus a test that loads
  old state. Before v1.0, `stateVersion` stays 1 (there are no released states to migrate).
- Panel Mode (Lower/Upper edit target) is UI state, not a host parameter.

**DSP quality:**
- Oscillators use the windowed-sinc BLEP/BLAMP table (`dsp/BlepTable.h`). Sync and cross-mod reset at
  the exact sub-sample point. The alias gate is ≤ −100 dB (0–20 kHz) up to a 5 kHz fundamental; don't relax it.
- The voice path is 2× oversampled in real time and 4× offline. Report total latency (decimator + BLEP).
- The VCF is a ZDF/TPT 4-pole with a nonlinear feedback path.
- Analog drift is seeded per voice, so renders are deterministic.

**Portability:** no x86-only intrinsics without a NEON path and runtime dispatch. Every change
must build for all three targets (mac universal, win x64, win ARM64).

## Known traps (details in `docs/engineering-notes.md`)

- Never build a `juce::MidiMessage` on the audio thread (SysEx allocates); parse raw bytes.
- Any stage that adds delay must be added to the reported latency.
- JUCE's VST3 wrapper adds 2080 "MIDI CC" params + "Bypass"; exclude them when counting ours.
- Wrap `JUCE_PLUGIN_ARTEFACT_FILE` in `$<GENEX_EVAL:…>`; tests take include dirs/defines from the plugin target.
- Measure filters far below Nyquist and above the float floor; isolate a failing gate with a diagnostic test.
- Rosetta isn't installed: the x86_64 slice is checked with `lipo` only, never run locally.
- Hooks block writing the git-ignore file and any command text mentioning it or `.env` (even inside a
  script); use the Edit tool for such text. Never read anything under `build/`.

When something new bites, add a row to `docs/engineering-notes.md` and, if it's a rule, a line here.

## Definition of done (per change)

1. Builds clean (warnings as errors) on all targets → skill `build`.
2. Unit + render-regression tests pass; pluginval strictness 10 passes → skill `validate-plugin`.
3. Audio-thread code was checked → skill `realtime-review`.
4. Any parameter or state change followed skill `add-parameter`.
5. Docs updated: `CHANGELOG.md`, the Status section here, README (features/roadmap), new judgement
   calls in `docs/decisions.md`, new traps in `docs/engineering-notes.md`.

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
