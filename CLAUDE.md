# CLAUDE.md — Exitium Vintage Synth (VST3 instrument)

This file instructs AI coding agents working on this repository. Humans: start with `README.md`.

Bi-timbral, 16-voice-per-part analog-style polysynth plugin: two VCOs (sync, cross-mod), HPF,
12/24 dB VCF, VCA, 2× ADSR, LFO with delay, Lower/Upper parts (Whole/Dual/Split), Solo/Unison/Poly,
arpeggiator, 16-step sequencer, patch banks, Chorus/Delay/Reverb sends.

The panel is functionally inspired by a classic 80s poly. **Never use third-party brand names,
logos, or copied panel artwork** in code, UI, presets, or docs. The look and naming are our own.
The custom UI (M7) also uses **its own layout**, designed for plugin use; it does not reproduce the
reference panel's arrangement (owner decision).

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
  MSVC static runtime. **Linux x86_64 and ARM64** (owner decisions): built on `ubuntu-22.04` /
  `ubuntu-22.04-arm` for a glibc 2.35 floor, GCC, static libstdc++/libgcc, merged into one bundle
  (`Contents/x86_64-linux`, `Contents/aarch64-linux`). ARM64 is CI-validated only (no pluginval
  build exists for it) and not performance-tuned for small boards. Never raise that runner image
  above the oldest one offered without a decision. **Before pushing engine or test changes run
  `scripts/linux-check.sh`** (Docker; native Linux ARM64 container; GCC build + all tests, about
  1 min warm; `--sanitize` for ASan/UBSan): it catches GCC-only errors without a CI round trip.
  The x86_64 Linux binary is proven in CI only: `gh workflow run build.yml --ref <branch> -f targets=linux`
  runs the Linux rows, the sanitizer job and the distro load tests.
- **Identity:** company "Exitium", bundle ID `com.exitium.vintagesynth`, manufacturer code
  `Exit`, plugin code `Vsy1`. **Never change these after the first release**, because hosts identify
  the plugin by them.
- **pluginval: Claude runs it itself** (official Tracktion release v1.0.4, unpacked in the session
  scratchpad). Don't hand it to the user unless the harness refuses the command. See skill `validate-plugin`.
- **Repo:** GitHub `romuaa/vintage-synth` (private), branch `main`.
- **Workflow: orchestrated agents, branches + PR** (skill `orchestrate`). Agents commit on feature
  branches; the orchestrator pushes them as PRs so CI validates Windows. **Merge to `main` only after
  the owner approves that milestone.** Never commit to `main` directly, never force-push.
  The owner auditions at the end of M2, M5 and M6 and sets the UI direction before M7.
- **Contracts:** `docs/architecture.md` defines the interfaces agents build against (note events,
  voice allocation, new parameters, effects, arp/seq). Change it only in its own commit.
- **CI:** `.github/workflows/build.yml` builds, tests, and runs pluginval on macOS and Win x64.
  Win ARM64 runs build, unit tests and host_smoke (there's no ARM64 pluginval). Check results with
  `gh run list` / `gh run view <id> --log-failed` (gh is installed and authenticated). Never claim
  Windows is green without a passing run. CI builds every target with Ninja and a compiler cache
  (ccache on macOS, sccache on Windows) inside the newest Visual Studio found by `vswhere`; never
  hardcode a VS version. LTO applies to the shipped plugin only; tests and `voice_bench` link
  `vs_test_core` (same sources, no LTO), so add new source files to `VS_COMMON_SOURCES` or `VS_VINTAGE_SOURCES` (`VS_PLUGIN_SOURCES` is their union).
  Runner images are pinned (`macos-26`, `windows-2025`, `windows-11-arm`, `ubuntu-26.04`);
  Dependabot doesn't bump them, so check github.com/actions/runner-images when touching CI.
  Pushing to `main` cancels a running build of `main` (concurrency group): wait for a run you
  still need before pushing again.
- **Verification is local-first (owner decision).** Everything this Mac can check is checked here by
  `scripts/preflight.sh`: macOS universal build + tests + pluginval, the VST3 validator (built once
  from the pinned SDK commit into `~/Library/Caches/vs-preflight/`), ASan/UBSan, the Linux ARM64
  container, the security scan, repository hygiene (incl. a hashed brand-name check and "no session
  link in commits"). `--quick` while iterating, the default set before handing over.
- **CI never runs by itself** (no PR, branch or `main` triggers on `build`). The orchestrator
  dispatches it by hand, once per merge-ready branch, when Actions minutes are available:
  `gh workflow run build.yml --ref <branch> -f targets=no-macos` (`all` before a release; also
  `windows`, `linux`, `linux-x64`). Agents never trigger CI. **Windows and Linux x86_64 are
  confirmed by CI only**: never claim them green without a passing dispatched run. A dispatch only
  works for refs once the workflow file is on the default branch.
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

- **Wave 3 is merged to `main`** (PR #9, owner approved 2026-11-13): M5 (effects and arp/sequencer
  in the plugin, 170 parameters, host bypass, sine-sync alias fix, offline = real time for Cross
  Mod), S0 (common library `source/common/`) and M6a (presets and banks, preset bar, 8 placeholder
  programs). Verified by the local gate only.
- **Wave 4a is merged to `main`** (owner approved 2026-11-14): M6b, the 64 factory programs
  (`docs/factory-sounds.md`, level rule enforced by tests, eight golden references by owner
  decision); the output stage (fixed −6.02 dB trim, soft limiter `master.limiter` /
  `master.limiter.ceiling` as session settings, 172 parameters; default level approved by the
  owner); the VCF slope crossfade on sounding voices. Verified by the local gate only.
  **Next:** B-Side S1; the owner's by-ear notes on single factory programs; M7 UI direction.
- MIDI Program Change timing: accepted as is for now (owner will test later).
- **M3 bi-timbral: done** (Upper part, Whole/Dual/Split, balance and pan, stereo, oversampling by
  host rate, product renamed to Exitium Vintage Synth). **M4 arp/sequencer module: done**, standalone
  in `source/common/seq/`, not wired in. Audition approved by the owner.
- **CI is offline** (no Actions minutes) until the owner makes the repo public for a release.
  **Local-first verification is the permanent policy** (see decisions): run the local gates before
  any push; never trigger workflows; agents get no CI budget.
- **Merged without Windows / Linux x86_64 verification** (check these first when CI is back):
  1. Linux ARM64 target, distro load tests, `scripts/linux-check.sh`, a GCC/ARM64 warning setting
     in `CMakeLists.txt`, a float-comparison rewrite in `source/common/fx/FxCommon.h` (2026-11-12).
  2. `scripts/preflight.sh` and the manual-only CI workflow (the reworked `build.yml` itself has
     never run on GitHub: its first dispatch is also its first test) (2026-11-12).
  3. Wave 3 (PR #9): effects and arp/sequencer integration, the common-library split, presets and
     banks incl. the strict JSON reader and file dialogs (2026-11-13).
  4. Wave 4a: factory bank and its tests, output trim and soft limiter, VCF slope crossfade,
     the rule that parameters missing from a session load at their defaults (2026-11-14).
- **Cross Mod:** the owner chose to keep today's sound ("Classic"); it is exempt from the −100 dB
  alias gate (documented in decisions). A "Clean" switch comes later, low priority. Two defects
  from the study are being fixed: sine-carrier sync aliasing, and bounce ≠ playback for sync + cross-mod.
- **M2 polyphony: done** (16 voices, Poly/Unison/Solo, steal fade, pedal, portamento, velocity, bend
  range, mod wheel; audition approved by ear; 32 voices = 5 % of a core at 48 kHz). Lower part only.
- M0 skeleton: done. M1 single-part voice engine: done (mono, Lower part only; demo approved by ear;
  all panel interpretations confirmed by the owner).
- CI: green on macOS universal, Windows x64 and Windows ARM64 (build, tests, host_smoke, pluginval
  where available); Trivy green; Dependabot active. History: `CHANGELOG.md`.
- Compiler cache confirmed: a warm CI run takes macOS 3.5 / Win x64 6.7 / Win ARM64 5.9 min (98 % hits).
- **Wave 0 (contracts): done** — `docs/architecture.md`; judgement calls in `docs/decisions.md`
  are marked (review) until the owner confirms them.
- **Wave 1: done, awaiting the owner's merge approval** (one PR). Contains M2, the M5 effect blocks
  (`source/common/fx/`, standalone, not wired in; first listen approved), the golden-render harness, the
  VST3 validator in CI, CI cost controls and the plugin-host script.
- **Planned second instrument: "B-Side",** a chip synth as a separate plugin in this repository
  (`docs/architecture.md` section 8); starts with extracting a common library after M3.
- **Done after Wave 1:** Linux x86_64 target with a sanitizer CI job; public-release audit
  (`SECURITY.md`, `CONTRIBUTING.md`). The owner flips visibility; then one follow-up change removes
  the draft-PR cost tier and the "private repo" wording. **Next: Wave 2** (M3 + M4). Open engine item for M3: 1× oversampling at host rates ≥ 88.2 kHz
  (32 voices cost 18 % of a core at 192 kHz).
- Then: Wave 2 = M3 Upper part + Whole/Dual/Split, with M4 arp/sequencer in parallel;
  Wave 3 = integrate M4/M5 + M6 presets/banks/goldens; Wave 4 = M7 custom UI + M8 release.

## Layout

```
source/common/    shared by all instruments; must not include anything from source/vintage/
  dsp/            FastMath, DspUtil, BlepTable (JUCE-free)
  engine/         NoteEvent, EventParser, VoiceAllocator, KeyRouter, PartMixer (JUCE-free), Oversampler
  fx/             Chorus, Delay, Reverb (JUCE-free)
  seq/            arpeggiator and step sequencer (JUCE-free)
  params/         parameter factories, effect and arp/seq parameters + bindings, shared Mappings.h
source/vintage/   this instrument: PluginProcessor.*
  dsp/            Oscillator, Filters, Envelope, Lfo, Drift (JUCE-free)
  engine/         Voice, Part, Modulation (shared per-part modulation)
  params/         PartParamList.h (X-macro list of all per-part params), PartMappings.h,
                  ParameterLayout (assembles the layout), StateVersion.h
tests/dsp_unit/  Catch2 tests; RenderDemo.cpp is a hidden "[.render]" audition render
tests/benchmarks/ voice_bench (CPU cost per voice)
tests/host_smoke/ loads the built .vst3 through JUCE's VST3 host (ctest "host_smoke"; runs on every CI target)
tests/render_regression/ golden-render harness (ctest label "render"; scripts/ and golden/ feature references)
docs/architecture.md  contracts between work streams (interfaces, behaviour, gates)
docs/decisions.md  design decision log (what we chose and why)
docs/ui-test-checklist.md  manual checks in the plugin host
scripts/dev-host.sh  builds plugin + JUCE AudioPluginHost and opens the synth in it (-DVS_BUILD_DEV_HOST)
scripts/preflight.sh  the local gate: every check this Mac can run (see "Verification is local-first")
scripts/linux-check.sh  GCC build + tests (or sanitizers) in a Linux ARM64 container (a preflight step)
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
- Oscillators use the windowed-sinc BLEP/BLAMP table (`common/dsp/BlepTable.h`). Sync and cross-mod reset at
  the exact sub-sample point. The alias gate is ≤ −100 dB (0–20 kHz) up to a 5 kHz fundamental; don't relax it.
- The voice path is 2× oversampled in real time and 4× offline. Report total latency (decimator + BLEP).
- The VCF is a ZDF/TPT 4-pole with a nonlinear feedback path.
- Analog drift is seeded per voice, so renders are deterministic.

**Portability:** no x86-only intrinsics without a NEON path and runtime dispatch. Every change
must build for all five targets (mac universal, win x64, win ARM64, Linux x86_64 and ARM64 with GCC).

## Known traps (details in `docs/engineering-notes.md`)

- Never build a `juce::MidiMessage` on the audio thread (SysEx allocates); parse raw bytes.
- Any stage that adds delay must be added to the reported latency.
- JUCE's VST3 wrapper adds 2080 "MIDI CC" params + "Bypass"; exclude them when counting ours.
- Wrap `JUCE_PLUGIN_ARTEFACT_FILE` in `$<GENEX_EVAL:…>`; tests take include dirs/defines from the plugin target.
- Measure filters far below Nyquist and above the float floor; isolate a failing gate with a diagnostic test.
- Run the tests under ASan/UBSan before pushing: MSVC's heap catches overruns macOS tolerates.
- A new continuous parameter is not smoothed unless added to `PartModulator::next`.
- Per-voice state inherited by a later note must be reset or computed from absolute time; a
  block-size test only proves something if voices are reused.
- GCC rejects float `==`/`!=` (`-Wfloat-equal` is an error); Clang and MSVC accept it. Use `std::fpclassify`.
- Rosetta isn't installed: the x86_64 slice is checked with `lipo` only, never run locally.
- Hooks block writing the git-ignore file and any command text mentioning it or `.env` (even inside a
  script); use the Edit tool for such text. Never read anything under `build/`.

When something new bites, add a row to `docs/engineering-notes.md` and, if it's a rule, a line here.

## Definition of done (per change)

1. `scripts/preflight.sh` passes (`--quick` while iterating; the full default set before handing
   over): universal build with warnings as errors, all tests incl. render regression, pluginval
   strictness 10, VST3 validator, sanitizers, Linux ARM64 container, security scan, hygiene.
2. Windows and Linux x86_64: one dispatched `no-macos` CI run per merge-ready branch when CI is
   available; otherwise add the change to "Merged without Windows / Linux x86_64 verification".
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
| `orchestrate` | Starting or resuming milestone work with agents (`/orchestrate`) |

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
