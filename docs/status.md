# Project status

Where the project is, what is parked, what comes next, and the history of what was merged.
The orchestrator keeps this file current; `CLAUDE.md` links here. Read "Where the project is"
first when resuming.

## Where the project is (2026-11-20) — read this first when resuming

**Exitium Vintage Synth 0.9.0 (a public beta) is released** (2026-11-20, GitHub pre-release,
tag `v0.9.0` at commit `116c5f8`: three zips and `SHA256SUMS.txt`). **M8 is done.**
**Everything below is merged to `main` except B-Side S3a (parked, see below); no agent or build
is in flight.** **All five targets are confirmed** (2026-11-20): macOS by the local gate; Windows
x64 and ARM64 and Linux x86_64 and ARM64 by the first CI runs after the repository became public
("First CI runs" below).

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

**Owner decision 2026-11-19: Vintage is released first; B-Side is parked.** B-Side work stops
where it is and continues only after the Vintage release is complete. Nothing is in flight; no
agent is running.

**B-Side, parked state (resume here after the Vintage release):**
- Merged: everything up to and including the render cost step (PR #32) and the alias follow-up
  step (PR #33, 2026-11-19; see History).
- **S3a is unfinished and unmerged** on the pushed branch `bside/s3a-filter` (contract 8.19.1;
  two rounds of work, one independent review). Model B and the output stage are close (first
  review: corners within 3.1 %, peaks within 0.1 dB; cost after round 2 about 4.9 ns against 7,
  measured on a busy machine; round 2 not yet reviewed). **The Model A filter fails its gates:**
  in the reference its cutoff and resonance move with the level and DC routed into the filter;
  four level-dependent structures were tried and none came within tolerance (decisions on that
  branch, "B-Side S3a round 2", rows R1, R2, R12: corners up to 75 % off on the gate traces).
  The author failed the same gate twice, so the next step is the escalation of the skill
  `orchestrate`: a diagnostic test that isolates the level-dependent cutoff, then a stronger
  model. Also open there: the internal-rate dependence of the filter response (row R11, up to
  2.0 dB at 88.2 kHz), band-pass and two-tone distortion, the resonance-write DC step. Round 2
  found that Model A bends each voice before its envelope (`OutputStage::shapeVoice`, row R3):
  S3b must apply it per voice in the renderer. If the escalated attempt fails too, ask the
  owner: accept a documented Model A deviation, or spend more; Model B could merge separately.
- Developer-only material outside the repository, under `~/Library/Caches/bside-oracle/`:
  `s3a-park/` (the author's round 2 report, the reviewer's harness), `s3a/` (the author's
  fitting data), `s3a-review/` (the reviewer's held-out traces: an author must not read them).
- Then: **S3b** (8.19.2: two buses in double precision in `Chip::render`, 8.20.1; routing,
  topology, the remaining traces, filter sweeps for the owner), **S4** (polyphony 8.5, wiring
  into the plugin, decimator with the alias gate after it and the exact measurement on the
  summed path, factory patches, owner audition; **owner decision 2026-11-19:** the Model A CPU
  budget is decided at S4 with real numbers, filter and per-voice bend included), then B-Side's
  own release work. **Chip follow-ups, small:** the Model B read value in the first step after a
  waveform change (8.15.2 (c) step 5); at the next reference regeneration a full-noise-fill
  trace and a noise-before-first-shift trace; an independent model for the program engine's
  goldens; defects 3 and 4 of 8.18.1.

**Public since 2026-11-20** (owner flipped visibility after the pre-public audit; its decisions
are in `docs/decisions.md`, "Pre-public audit"). Actions are free now, and CI still runs only when
the orchestrator dispatches it (see "Decisions").

**First CI runs (2026-11-20), all dispatched by hand:**
- Run 44627905615 (`main`, `no-macos`): Linux ARM64 and the sanitizer job passed; Linux x86_64
  built and passed its unit tests, then hit the job's 30-minute limit (cold cache); Windows failed
  to build, in B-Side test, tool and benchmark sources only (`getenv` as an error, a
  signed/unsigned comparison, `1L << 40` with a 32-bit `long`).
- Run 44634121070 (`m8/ci-fixes`, `no-macos`): every Linux job green (x86_64 with pluginval and
  the validator, the seven distro load tests, sanitizers, ARM64); Windows built, 2 tests failed
  on both rows (test assumptions: `/proc/...` is writable on Windows, a path separator).
- Run 44638566407 (`m8/ci-fixes`, `windows`): Windows x64 (tests, pluginval, validator), Windows
  ARM64 (tests, host_smoke) and the merged Windows bundle green.
- **Release rehearsal:** run 44695944505 (`main` at `116c5f8`, `all`, `release=true`): every job
  green at the first attempt (all five targets, both merged bundles, the seven distro load
  tests, sanitizers, `pack`, the three `zip-smoke` jobs, `package`). It created the **draft
  release `v0.9.0`** (three zips and `SHA256SUMS.txt`, target commit `116c5f8`; no tag exists
  until the owner publishes). The orchestrator downloaded the zips: checksums match and
  `package-release.sh verify` passes on each.

**Next steps:**
1. **Vintage, M8** (skill `release`): **started 2026-11-19.** Order: (a) **done:** pre-public audit of the
   whole history, the workflow-trigger check in the local gate, the "nan" fix of the shared text
   parser (`common/params/ParamFactory.cpp`) and the tip cleanup after the audit; (b) **done:** the owner made the repository public;
   (c) **done:** the first Windows and Linux x86_64 runs and their fixes ("First CI runs"
   above); (d) packaging: version 0.9.0, one
   zip per platform, licences and install notes inside, a manual release workflow, B-Side
   excluded (`VS_BUILD_BSIDE=OFF`) **(built as contract 10: `scripts/package-release.sh`,
   the `release` input of `build.yml`)**; (e) **done:** the release rehearsal
   (`gh workflow run build.yml --ref main -f targets=all -f release=true`) gave the draft
   release `v0.9.0`; (f) **done (2026-11-20):** the owner installed from the zips and tried the plugin by hand in
   Cubase 15 Pro on macOS, Reaper on an Arch-based Linux, and Reaper and Cubase 15 Pro on
   Windows 11, automation record and playback included; no defect found; demo video
   https://youtu.be/IdVCa7lpVsQ; two other people praised the usability. Published on the
   owner's word as a pre-release. **The identity (class IDs, bundle ID, codes) is frozen from
   now on.** The next release is made as the skill `release` describes.
   **Owner's studio check passed for now (2026-11-19):** played in the dev host from a USB MIDI
   keyboard; sound, MIDI Program Changes from the keyboard, mod wheel, pitch bend and the
   arpeggiator are fine. Not done: a real screen-reader pass.
2. **B-Side:** resume from the parked state above, on the owner's word (the Vintage release it
   waited for is out).

**Deferred by the owner ("later"):** final Browser category colours, a top-bar redesign, digits of
equal width, playing from the computer keyboard, a "Clean" Cross Mod switch, **a go-through of the factory
preset set (2026-11-19: some presets sound exactly the same)**. **Not done anywhere:**
a real screen-reader pass; a hands-on check on Windows ARM64 or Linux ARM64.

**How work is run:** skill `orchestrate` (brief → implementer → independent reviewer → fixes →
orchestrator's full `scripts/preflight.sh` → PR → merge only on the owner's word). Lessons of
this wave are in `docs/engineering-notes.md`; the main one: an independent reviewer with its own
harness found real defects in every step, also after an author reported "all gates pass".
Developer-only tools outside the repository (reference adapter, tune-to-trace tool, rendered
listening files) live under `~/Library/Caches/bside-oracle/`; nothing there is named in the
repository.

## History (newest facts first within each area)

- **B-Side alias follow-up step is merged to `main`** (PR #33, owner approved 2026-11-19; contract as built: architecture 8.20.1; decisions "B-Side alias
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
- **The list "Merged without Windows / Linux x86_64 verification" (20 entries, 2026-11-12 to
  2026-11-20) is closed:** the runs of 2026-11-20 above compiled and tested all of it on MSVC
  (x64 and ARM64) and on Linux x86_64. What they found: test-code portability only, no defect in
  either plugin. The packer on real Windows and Linux bundles and the release jobs were proven
  by the release rehearsal (run 44695944505).
  **Local-first verification stays the policy:** run the local gates before any push; agents
  never trigger workflows. When a change cannot get a CI run, start a new such list here.
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

