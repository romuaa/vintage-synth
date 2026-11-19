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
- **B-Side (owner, 2026-11-14; `docs/architecture.md` 8.12):** identity `Bsy1` /
  `com.exitium.bside`; two parts; Init uses chip Model B; mimic the original chip as closely as
  possible (its own alias budgets, 8.4.5, approved). **Reference emulator approved as a developer
  tool only:** built outside the repository (`~/Library/Caches/bside-oracle/`), never linked or
  shipped, never named in the repository; only generated reference data is committed.
- **M7 editor (owner, 2026-11-14; `docs/architecture.md` 9.8):** round-3 wireframes approved;
  fixed aspect, 75–200 % (1360×784 at 100 %); embedded font Inter (SIL OFL 1.1, its licence file
  kept in the repository); dark theme only; static graphs with live markers; delay/reverb on/off
  as a level-0 switch (no new parameters); values shown as hosts show them (0–10); help bubbles
  after 600 ms plus the help line.
- **M7 UI (owner, 2026-11-14):** the tabbed wireframes are approved as the direction, with a
  colour tint per part, Shape split into LFO and Envelope pages, and hover help on every control.
- **Panel interpretation, DSP ranges and design choices:** see `docs/decisions.md`. Add every new
  judgement call there (with the reason), and mark the ones the user should review.

## Status

### Where the project is (2026-11-16) — read this first when resuming

**Everything below is merged to `main`; no branch, agent or build is in flight.** Verified by the
local gate only (Windows and Linux x86_64: the list "Merged without ... verification" below).

- **Exitium Vintage Synth is feature-complete up to M7:** engine, effects, arp/seq, presets and
  64 factory programs, the custom editor with all pages (M7a–M7f). The owner has tried and
  approved every page.
- **Exitium B-Side:** the plugin is still the S1 shell with test tones. The chip model exists as
  a JUCE-free library, not wired in: digital core (S2a), own combined-waveform tables (S2b-1),
  program engine (S2c), band-limited rendering before the filter (S2b-2).

**Repository housekeeping (2026-11-16, owner's word):** all merged branches are deleted, on GitHub
and locally. Two branches exist: `main` and `study/crossmod-aliasing` (kept and pushed by owner
decision: the Cross Mod aliasing study's experimental code, built against the old `source/dsp/`
layout, never to be merged as is; the reference for the deferred "Clean" Cross Mod switch).
GitHub deletes a PR's branch on merge; delete the local branch after each merge too.

**Skill `orchestrate` exists twice (2026-11-16):** the owner's personal, generic one
(`~/.claude/skills/orchestrate/`) and this repository's (`.claude/skills/orchestrate/SKILL.md`).
The personal one is the one a session lists and holds the process; its first instruction is to
read this repository's file, which holds only what is specific to this project (serial files,
worktrees, gates, model routing, commit rules) and overrides it. If a session did not read the
repository's file, read it by hand before orchestrating. Checked 2026-11-16: `/orchestrate`
loads the personal skill and reaches the repository's file through that instruction.

**In flight (2026-11-19):** B-Side **S3a** (filter and output stage as their own classes,
contract 8.19.1; branch `bside/s3a-filter`, based on `bside/contracts-s3a-alias`): Model B is
accepted by the first review apart from cost; **Model A is being reworked** (resonance away from
the fitted cutoffs, cutoff moving with the input level, distortion and intermodulation, the Shared
topology, volume clicks with `chip.volume_dc` off). The **alias follow-up step** is done and
awaits the owner's merge approval (see History). Until both merge, the first line of this
section does not hold.

**Going public (owner, 2026-11-18):** the repository becomes public only once the release work is
done, **B-Side's release work included** (so after S4 and M8 for both plugins). Until then CI
stays offline; every Windows and Linux x86_64 check waits for that moment.

**Next steps, in the order the owner was given:**
1. **B-Side render cost step:** done and merged (PR #32, 2026-11-19; architecture 8.18.1).
2. **B-Side parameter step:** done and merged (PR #31, 2026-11-18).
3. **S3a** (in flight; 8.19.1): `Filter` and `OutputStage` in new files, fitted to reference
   data, both models; stays out of the renderer. **Alias follow-up step:** done, awaiting merge
   (8.20.1).
4. **S3b** (8.19.2; after the alias step and S3a have merged, it edits the renderer): two buses
   in double precision in `Chip::render` (8.20.1: no float addition before the part's sum),
   routing, topology, the remaining reference traces, filter sweeps for the
   owner to hear. Then **S4** (polyphony 8.5, wiring into the plugin, decimator with the alias
   gate after it, factory patches, owner audition). **Owner decision 2026-11-19:** the Model A
   CPU budget (the S4 gate patch costs about 31 ns before the filter, the whole budget of 8.5)
   is decided at S4 with real numbers, filter included.
   **Chip follow-ups, small:** the Model B read value in the first step after a waveform change
   (candidate rule in 8.15.2 (c) step 5); at the next reference regeneration a full-noise-fill
   trace and a noise-before-first-shift trace; an independent model for the program engine's
   goldens (today derived by the engine author's own model).
5. **Vintage, M8:** hardening, signing, installers, release (skill `release`); before it the
   owner's studio check of the factory sounds and MIDI Program Change timing.

**Deferred by the owner ("later"):** final Browser category colours, a top-bar redesign, digits of
equal width, playing from the computer keyboard, a "Clean" Cross Mod switch. **Not done anywhere:**
a real screen-reader pass; any Windows or Linux x86_64 run.

**How work is run:** skill `orchestrate` (brief → implementer → independent reviewer → fixes →
orchestrator's full `scripts/preflight.sh` → PR → merge only on the owner's word). Lessons of
this wave are in `docs/engineering-notes.md`; the main one: an independent reviewer with its own
harness found real defects in every step, also after an author reported "all gates pass".
Developer-only tools outside the repository (reference adapter, tune-to-trace tool, rendered
listening files) live under `~/Library/Caches/bside-oracle/`; nothing there is named in the
repository.

### History (newest facts first within each area)

- **B-Side alias follow-up step: done on `bside/alias-followup`, awaiting the owner's merge
  approval** (2026-11-19; contract as built: architecture 8.20.1; decisions "B-Side alias
  follow-up step"). Narrow Model B pulses: the cause was the float conversion of the output (its
  rounding errors gather on a few lines for a periodic signal), fixed by a noise-shaped
  conversion; exact measurement now at least 13.4 dB inside -100 dB at every rate (was up to
  2.7 dB outside). Synced targets at exact ratios: a reset that met the target's own wrap lost
  its step every second period; now at least 8.4 dB inside the budget over 11 970 rows.
  Reviewed independently (no contract defect; test gaps closed). **Binding for S3b and S4:**
  any later float addition undoes the shaping, so the buses and the part's sum stay in double
  and S4 gates the summed path by the exact measurement. Open: defects 3 and 4 of 8.18.1.

- **B-Side render cost step is merged to `main`** (PR #32, owner approved 2026-11-19; contract as built: architecture 8.18.1; decisions "B-Side render cost step").
  `Chip::render` at 96 kHz, ns per chip per internal sample: silent 61 -> 2, typical Model B
  56 -> 13, Model A typical 217 -> 36, the S4 gate patch 265 -> 31, three combined waveforms at
  12 kHz about 5000 -> 183. **Model A is still above its target of 24** (every further saving
  changed the audible band); S3/S4 must find more or ask the owner about the Model A budget.
  The three defects of 8.17 and two siblings fixed; output no longer depends on where
  `renderUntil` stops; above 3.85 kHz combined waveforms use step moments (alias limit -80 dB).
  Three independent reviews, each with findings (the first: a division by zero that only
  x86-64 or UBSan shows). **Open, older defects, reports only:** narrow pulses and
  integer-ratio sync (the alias follow-up step), a width write's edge two cycles early, frozen
  against crawling lines.

- **B-Side parameter step is merged to `main`** (PR #31, owner approved 2026-11-18): the 127 host parameters of 8.7 (63 per part from one X-macro list in
  `source/bside/params/PartParamList.h`, then `chip.clock`; 202 in all), mappings in
  `bside/params/PartMappings.h` (continuous register values for Smooth, `authenticRegister` for
  Authentic), a binding per part (`PartParamBinding`). **Inert:** nothing on the audio thread
  reads them until S4. Typed text always gives a finite value in range; choices accept any case
  and a bare number. Reviewed independently (no contract difference; test gaps closed). The JSON
  value cap of B-Side preset files rose to 192 000. **Owner decision 2026-11-18:** `chip.clock` is saved
  with a program (it is part of the sound). **Follow-up, small:** Vintage's shared
  text parser (`common/params/ParamFactory.cpp`) still turns typed "nan" into a NaN value.

- **Wave 3 is merged to `main`** (PR #9, owner approved 2026-11-13): M5 (effects and arp/sequencer
  in the plugin, 170 parameters, host bypass, sine-sync alias fix, offline = real time for Cross
  Mod), S0 (common library `source/common/`) and M6a (presets and banks, preset bar, 8 placeholder
  programs). Verified by the local gate only.
- **Wave 4a is merged to `main`** (owner approved 2026-11-14): M6b, the 64 factory programs
  (`docs/factory-sounds.md`, level rule enforced by tests, eight golden references by owner
  decision); the output stage (fixed −6.02 dB trim, soft limiter `master.limiter` /
  `master.limiter.ceiling` as session settings, 172 parameters; default level approved by the
  owner); the VCF slope crossfade on sounding voices. Verified by the local gate only.
  The owner checks the factory sounds by ear later, in the studio; adjust then if needed.
- **M7b–M7e (all editor pages) are merged to `main`** (owner tried them and approved,
  2026-11-15; the Play page keys play notes at the owner's request): Envelopes and LFO (pictures from the real envelope and LFO code), Play
  (keyboard strip whose keys play notes) and Effects (pictures, on/off switches), Arp + Seq (step grid, pattern cards,
  preview computed by the real arpeggiator; small audio-thread additions for the playing step and
  held-key order, audio byte-identical), Browser (grid, categories, search, tone loading) and the
  program menu on the program name. Each page reviewed independently and fixed; the placeholder
  page is removed; time parameters now print whole milliseconds ("45 ms").
- **M7f (editor polish) is merged to `main`** (owner tried it and approved, 2026-11-15): held
  keys tinted and sounding notes marked on the keyboard strip (owner decision; no dots), no
  allocation in the editor's own paint code (whole-editor test; the dashed external-change
  outline is the known exception), accessibility handlers for the Browser grid, chips and tone
  switch with an inventory test over the whole editor, theme tokens, one shared test graphics
  context. Reviewed independently and fixed. **Deferred by the owner ("later"):** final category
  colours, a top-bar redesign, digits of equal width, playing from the computer keyboard. No real
  screen-reader pass was done.
- **M7a (editor foundation) is merged to `main`** (owner tried it and approved, 2026-11-14): the
  custom editor frame (page rail, part switch with tint, preset bar, output meter and limiter
  popover, help line and bubbles, 75–200 % scaling), the Sound page with live pictures, placeholder
  pages for the rest, live automation by polling (no parameter listeners), the audio → editor feed
  and the `UiState` session child. **Owner feedback for the next pages:** the frame and the Sound
  page look good; pages made only of sliders (Play, Arp + Seq) are messy and not intuitive, so
  those pages need purpose-built controls and pictures (redesign in `docs/ui/` first, owner
  approves before code); clicking the preset name must open a list of the programs, grouped by
  purpose (Horns, Keys, Leads, Noise, Radical, Strings, ...). **Next:** that redesign, then
  M7b–M7e; only the page list in `VintageEditor::buildPages` is a serial edit.
- **B-Side S2b-1 and S2c are merged to `main`** (owner approved 2026-11-15): our own
  combined-waveform tables (`chip/CombinedWaveforms.*`: a small fitted resistor-network model per
  chip model, tables computed from its parameters; measurements in `tests/bside/shapes/`, fit tool
  `bside_shapefit gen | fit | verify | report`; exempt checkpoints that differ from the reference
  fell from 1752 to 81 of 6850 and are pinned by a budget file) and the patch program engine
  (`source/bside/engine/`: tick clock, lanes, commands, restart policies, 72 golden write traces).
  Contract as built: architecture 8.15.2, 8.15.8, 8.16. Both reviewed independently and fixed.
  Not wired into the plugin yet. **Open:** the first Model B step after a waveform change is not
  proven (8.15.2 (c) step 5); pulse combinations fit worst (about 92 % of entries exact on
  Model B).
- **B-Side S2b-2 (band-limited waveform rendering) is merged to `main`** (owner approved
  2026-11-16, with the open points below): `Chip::render` (`chip/ChipRender.cpp`, `RenderTables.*`): the three
  oscillators after envelope and converter, before the filter; latency 8 internal samples; every
  alias budget of 8.4.5 met with at least 3 dB to spare (measured before any decimator). The
  shared `BlepTable.h` is now a template; Vintage's instance is bit-identical. Reviewed
  independently four times (each of the first three rounds found defects in the prediction of
  sync, ring and pull-downs around writes; a state check against the chip now guards them).
  Contract as built: architecture 8.17. Not wired into the plugin. **Open:** three small known
  defects (8.17: a synced pulse at frequency 0, a placeholder-shapes ring case, a frequency drop
  on a sync source); cost is far above
  the S4 budget (a cost step comes before S4); S4 must gate aliasing after its decimator.
  **Next:** the render cost step, the parameter step (8.7), then S3 (filter and output path).
- **B-Side S2a (chip digital core) is merged to `main`** (PR #24, owner approved 2026-11-15):
  `source/bside/chip/` follows the per-cycle specification (8.15); every committed
  trace replays without a mismatch (`bside_trace`; fields that depend on a combined-waveform
  table are exempt until S2b, counted per trace), and eight real tunes replay exactly
  (developer-only, outside the repository). An independent model written from the contract alone
  (`tests/bside/unit/IndependentModel.h`) agrees with the chip. Not wired into the plugin yet.
  **Next:** S2b (combined-waveform tables, waveform rendering), S2c (program engine); the chip's
  fast path needs its cost reduced in S4 (engineering notes).
- **B-Side reference data is merged to `main`** (owner approved 2026-11-14; the owner listened to
  the reference emulator on eight tunes and confirmed it as the sound to match):
  76 traces with digital references and audio features in `tests/bside/reference/` (generated by
  the adapter outside the repository), the analysis tools (`SpecAnalysis`, `AnalogMeasure`), the
  test target `bside_trace`, and the per-cycle chip specification (architecture 8.15). **Owner
  decision 2026-11-14: "follow, own tables"**: the digital core follows the reference per cycle;
  combined-waveform shapes and analog constants are our own fits; B-Side stays GPL-compatible for
  good. Reviewed twice; the specification replays all 26 digital traces without a mismatch.
  **Next:** the S2a fidelity pass against it, then S2b (waveforms), S2c (program engine).
  A developer-only tune-to-trace tool lives outside the repository (owner approved 2026-11-14).
- **B-Side S1d (patch tables in presets and sessions) is merged to `main`** (owner approved
  2026-11-14): tables in program, tone and bank files and in the session, stored as the user wrote
  them; a wait-free hand-over of the compiled table to the audio thread (`ProgramHandover.h`,
  exhaustively model-tested; nothing reads it until S2c). Contract as built: architecture 8.14.
  Vintage files and states byte-identical. Reviewed twice.
- **B-Side S1b is merged to `main`** (plugin shell): a second plugin "Exitium B-Side" (test-tone
  part, shared parameters, presets, state; no chip model yet), `VS_BUILD_BSIDE` (default ON), the
  local gate, `host_smoke`, the dev host (`scripts/dev-host.sh --plugin bside`) and the CI
  workflow extended to two plugins. Vintage proven unchanged (build commands, dumps, bundle).
  **Next on B-Side:** the oracle adapter and reference data, the preset-model extension (8.8),
  then S2 (chip digital core, program engine).
- **B-Side S1c (JUCE-free part) is merged to `main`** (owner approved 2026-11-14): patch table
  reader, writer and compiler (`source/bside/program/`), trace, reference and spec formats with
  replay driver and comparer (`source/bside/trace/`), shared scanner (`source/bside/text/`), test
  target `bside_unit`, adapter interface `docs/bside-oracle-adapter.md`. Reviewed twice with
  independent fuzzing; guarantees in architecture 8.13. Not done: the preset-model extension (8.8)
  and any reference data.
- **B-Side S1a is merged to `main`** (owner approved 2026-11-14): the shared processor skeleton
  `source/common/shell/` (pure refactor; raw renders, parameter list, states and preset files
  byte-identical, shown by the author and again by an independent reviewer). Contract as built:
  architecture 8.2.5. **Next:** S1b (plugin shell; first tighten the skeleton per 8.2.5), S1c
  (table and trace readers, in progress on `bside/s1c-formats`), and M7a (editor foundation).
- **M7 editor contract is written** (architecture section 9, wireframes and help texts in
  `docs/ui/`). M7 code starts only after S1a has merged (both touch the editor); M7a first.
- **M7 UI direction (owner, 2026-11-14):** a tabbed workspace, one purpose per full page (Sound,
  Shape, Play, Arp+Seq, Effects, Browser), as simple as possible, sliders instead of knobs (knobs
  are hard with a mouse), graphs for visualisation. Every control and graph follows host
  automation and program changes live. Wireframe drafts are being iterated outside
  the repository; no UI code before the owner approves a draft.
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
  5. S1a: the shared processor skeleton, a header-only CRTP template that MSVC has never
     compiled (highest portability risk on this list) (2026-11-14).
  6. S1c: the B-Side table and trace readers and `bside_unit` (2026-11-14).
  7. S1b: B-Side itself, the two-plugin CMake and `build.yml` rewrite (never run), the shared
     `vs_juce_modules` library on MSVC, `scripts/sanitize-targets.sh` on a runner (2026-11-14).
  8. M7a: the custom editor, the shared UI library, the embedded font and help-text data,
     pluginval with its editor tests (CI still passes `--skip-gui-tests`) (2026-11-14).
  9. S1d: patch tables in presets, `BasicProgramHandover<Hook>` (`[[no_unique_address]]` and
     `if constexpr` on MSVC), short JSON escapes for control characters (2026-11-14).
  10. Reference data: `bside_trace`, `SpecAnalysis`, `AnalogMeasure`, byte-exact reference files
      (`.gitattributes`, no line-ending conversion on Windows) (2026-11-14).
  11. M7b–M7e: the editor pages, `PresetActions`, new `PresetManager` accessors, the feed's
      held-key order, the editor → audio note queue (`UiNotes`); the zero-allocation paint tests
      run on macOS only (2026-11-15).
  12. S2a: the chip digital core and its tests (JUCE-free; 64-bit arithmetic and bit fields
      that MSVC has never compiled) (2026-11-15).
  13. S2b-1 and S2c: combined-waveform tables (`constinit`, `constexpr` array fills, hexadecimal
      float literals, table hashes pinned across compilers but never computed by MSVC or on
      x86_64), the program engine, the allocation counter's `BS_NOINLINE` (2026-11-15).
  14. M7f: accessibility handlers, prepared text in controls, the editor walk's class-name
      parser (its MSVC branch has never run) (2026-11-15).
  15. S2b-2: the renderer and the `BasicBlepTable` template (double-precision instance,
      floating-point contraction may differ: one test accepts 1e-6 outside Apple Clang),
      `chip_bench` (2026-11-16).
  16. B-Side parameter step: a nested X-macro with `#N` stringizing (MSVC's preprocessor has never
      seen it), a `juce::AudioParameterChoice` subclass, `std::optional` number parsing (2026-11-18).
  17. Render cost step: the reworked renderer and digital fast path (`std::bit_cast`, the
      inline macro's MSVC branch, a closed form that divided by zero on x86-64 before its fix:
      no x86-64 run has ever executed this code), kernel tables (2026-11-19).
  18. Alias follow-up step: `render::OutputRounding` (an expression one compiler may contract
      into fused multiply-adds and another not: low output bits can differ; the new exact and
      rounding tests have never run on MSVC or x86_64) (2026-11-19).
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
source/common/    shared by all instruments; must not include anything from source/vintage/ or source/bside/
  dsp/            FastMath, DspUtil, BlepTable (JUCE-free)
  engine/         NoteEvent, EventParser, VoiceAllocator, KeyRouter, PartMixer, UiFeed (audio → editor), UiNotes (editor → audio notes)
                  (JUCE-free), Oversampler
  fx/             Chorus, Delay, Reverb, SoftLimiter (JUCE-free)
  seq/            arpeggiator and step sequencer (JUCE-free)
  ui/             editor library for both instruments: models/ (JUCE-free interaction models),
                  ParamMirror (polls parameters per frame), controls/, graphs/, frame/, Theme,
                  HelpTexts, ControlRegistry
  params/         parameter factories, shared globals (GlobalParams: master volume, key/mix, limiter),
                  effect and arp/seq parameters + bindings, shared Mappings.h
  presets/        preset model, strict JSON, files, manager, factory bank, preset bar
  shell/          BiTimbralProcessor (CRTP skeleton: MIDI, key modes, arp/seq, mixer, effects, output
                  stage, bypass, programs, state), InstrumentEditor (preset bar + generic editor),
                  EditorState (JUCE-free; saved as the `UiState` child of a session)
source/vintage/   this instrument: PluginProcessor.* (VintageSynthProcessor : shell::BiTimbralProcessor),
                  ui/ (VintageEditor, PageList and one file pair per page: Sound, Envelopes, Lfo, Play,
                  ArpSeq, Effects, Browser)
  dsp/            Oscillator, Filters, Envelope, Lfo, Drift (JUCE-free)
  engine/         Voice, Part, Modulation (shared per-part modulation), OscPath (the oscillator
                  path shared by Voice and the editor's oscilloscope)
  params/         PartParamList.h (X-macro list of all per-part params), PartMappings.h,
                  ParameterLayout (assembles the layout), StateVersion.h
source/bside/     Exitium B-Side (second plugin): PluginProcessor.*, engine/TestTonePart (placeholder until
                  S2/S4), params/, presets/ (JUCE-bound; BSideTables = the table validator and sink);
                  program/ProgramHandover.h (table to the audio thread); text/ program/ trace/ are JUCE-free (strict
                  readers, patch tables, trace formats); chip/ is the chip model (digital core, combined-waveform tables; JUCE-free);
                  engine/ProgramEngine is the patch program engine (JUCE-free). Vintage and B-Side never
                  include each other
tests/bside/reference/  generated reference data (traces, digital references, audio features);
                  regenerated only with the adapter outside the repository, reviewed like a golden
tests/bside/shapes/  combined-waveform measurements (own MANIFEST); tests/bside/golden/ program-engine
                  write traces (tools/ derives them); tests/bside/tools/ShapeFit.cpp = bside_shapefit
tests/bside/      bside_unit (JUCE-free, ctest label "bside") always; bside_independent (the independent
                  model against the chip with placeholder shapes; never link it with bs_juce_free); with VS_BUILD_BSIDE also
                  bs_test_core + BSideTests. `bside_unit "[.fuzz-long]"` is the hidden 10^6-case fuzz run
presets/bside/factory/  B-Side factory programs (JSON, embedded)
scripts/sanitize-targets.sh  the one list of sanitizer test targets (preflight, Linux container, CI)
resources/fonts/inter/  the embedded UI font with its licence (OFL.txt) and source record
docs/ui/         wireframes and help-texts.md (the single source of the editor's help texts)
tests/ui/        editor tests (part of VintageSynthTests); hidden "[.uishots]" and "[.uibench]"
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
- Shared processor code is reached only through per-block hooks; instrument types are template
  arguments of `BiTimbralProcessor`, not member types of the derived class.
- Agents must never unpack, link or build inside the main checkout or a folder they did not create
  (stray links inside `external/JUCE` and `external/Catch2` have appeared twice).
- Never link `vs_test_core` and `bs_test_core` into one executable. A new test executable must be
  added to `scripts/sanitize-targets.sh`.
- Never `grep -q` on the right side of a pipe under `pipefail` (the left side dies with SIGPIPE and
  the pipeline "fails"); capture the output first. `VS_PREFLIGHT_PLUGINS` is for `--quick`/`--only`.
- GCC's `-Wshadow` is an error and also fires for a constructor parameter that shadows a member of
  a JUCE base class; only the Linux step shows it.
- `tests/bside/unit/IndependentModel.h` is the independent check of the chip: whoever edits
  `source/bside/chip/` must not edit it (and the reverse).
- A hand-over that publishes A and then clears B must be read B then A; change `ProgramHandover.h`
  only together with its model test (`HandoverModelTests.cpp`).
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
