# CLAUDE.md — Exitium Vintage Synth (VST3 instrument)

Instructions for AI coding agents working on this repository. They apply to every contributor's
agent. Humans: start with `README.md` and `CONTRIBUTING.md`.

Two plugins live here: **Exitium Vintage Synth**, a bi-timbral analog-style polysynth (two parts
of 16 voices, arpeggiator, step sequencer, effects, presets, its own editor), and **Exitium
B-Side**, a chip synth (in development, not yet released; see `docs/status.md`).

**Never use third-party brand names, part numbers, logos or copied panel artwork** in code, UI,
presets, docs, commit messages or briefs. The look, layout and naming are our own.

## Read before you work

| What | Where |
|---|---|
| Where the project is, what is parked, next steps, history | [`docs/status.md`](docs/status.md) |
| Contracts between work streams (interfaces, behaviour, gates) | [`docs/architecture.md`](docs/architecture.md) |
| Decisions in force and the dated decision log | [`docs/decisions.md`](docs/decisions.md) |
| Traps and the rules that came out of them | [`docs/engineering-notes.md`](docs/engineering-notes.md) |
| What lives where | [`docs/layout.md`](docs/layout.md) |
| How work is done here and why | [`docs/development-practices.md`](docs/development-practices.md) |

Skills in `.claude/skills/`:

| Skill | Use when |
|---|---|
| `build` | Configuring or building, or checking binary architectures |
| `validate-plugin` | Running tests, pluginval, the VST3 validator, golden renders |
| `add-parameter` | Adding or changing any parameter, preset field or saved state |
| `realtime-review` | Writing or reviewing any code reachable from `processBlock` |
| `release` | Version bumps, packaging, publishing a release |
| `orchestrate` | Starting or resuming milestone work with agents (`/orchestrate`) |

## Fixed facts

Summary only; the full text is in `docs/decisions.md`, "Standing decisions". An open decision is
never assumed: ask the owner, then record the answer there.

- **Licence:** AGPLv3 (JUCE under AGPLv3). Every release ships with or links to its complete
  source. Add no dependency that is incompatible with AGPLv3.
- **Format:** VST3 only. **Stack:** C++20, CMake 3.28 or newer, JUCE 9.0.5 and Catch2 v3.16.1 as
  submodules pinned to tags; upgrade them in their own commit.
- **Five targets:** macOS universal (arm64 and x86_64, macOS 11 or newer), Windows x64 and ARM64
  (MSVC, static runtime), Linux x86_64 and ARM64 (GCC, glibc 2.35 floor).
- **Identity:** company "Exitium"; Vintage `com.exitium.vintagesynth`, codes `Exit` / `Vsy1`;
  B-Side `com.exitium.bside`, code `Bsy1`. **Never change these after the first release.**
- **Version 0.9.0 is a public beta:** parameter IDs and the state format freeze at 1.0.0.
- **Releases:** one unsigned zip per platform, no installers, B-Side excluded
  (`docs/architecture.md` section 10; skill `release`).
- **Budget:** 32 voices at 2× oversampling under 15 % of one core at 48 kHz, effects included.
- **B-Side reference emulator:** a developer tool outside the repository; never linked, shipped
  or named here. Only generated reference data is committed.

## How work is done

- **Branches and pull requests.** `main` is protected: never commit to it directly, never
  force-push. Merge only when the owner approves. Delete the branch after the merge.
- **Contract first.** `docs/architecture.md` changes in its own commit, before the code.
- **Verification is local-first.** `scripts/preflight.sh` runs everything this Mac can check
  (`--quick` while iterating, the full default set before handing over).
- **CI never runs by itself.** `build.yml` is started by hand, by the orchestrator, once per
  merge-ready branch: `gh workflow run build.yml --ref <branch> -f targets=no-macos`. Agents
  never trigger CI. Windows and Linux x86_64 are confirmed by CI only: never claim them green
  without a passing run. Only the cheap `security` workflow and GitHub's CodeQL run
  automatically, and no workflow may gain an automatic trigger (the local gate checks this).
- **Supply chain:** third-party actions are pinned by full commit SHA; runner images are pinned.
- **Commits:** end the message with the `Co-Authored-By` line of the model that wrote it; never
  add a session link. Stage explicit paths.
- **Agents** work in their own worktree under `.claude/worktrees/` and never unpack, link or
  build inside the main checkout or a folder they did not create.
- **Never read** `.env` files, git-ignored paths or anything under `build/`. Hooks block command
  text that mentions the git-ignore file or `.env`; use the Edit tool for such text.
- **Publishing a release, tagging and anything else public or irreversible** needs the owner's
  confirmation each time.

## Hard rules

**Real-time audio thread** (`processBlock` and everything it calls; skill `realtime-review`):
- No allocation, locks, logging, file I/O, exceptions or blocking system calls.
- `juce::ScopedNoDenormals` at block start. Smooth continuous parameters; never smooth switches.
- MIDI is sample-accurate: split the block at event offsets. Never build a `juce::MidiMessage`
  on the audio thread; parse raw bytes.
- UI and DSP talk only through atomics or lock-free single-producer FIFOs. Preset loads are
  built off-thread and swapped in atomically.
- Any stage that adds delay is added to the reported latency.

**Parameters and state** (skill `add-parameter`):
- Parameter IDs are dotted and stable: `lower.vcf.cutoff`, `fx.reverb.level`.
- From 1.0.0 on, IDs are frozen (append only), and every state change bumps `stateVersion` and
  gets a migration plus a test that loads old state. Before 1.0.0 `stateVersion` stays 1.
- A new per-part parameter is one appended line in `PartParamList.h`; mappings go in
  `Mappings.h`; a new continuous parameter is smoothed only if added to `PartModulator::next`.

**DSP quality:**
- Oscillators use the shared BLEP/BLAMP table; sync and cross-mod reset at the exact sub-sample
  point. The alias gate is −100 dB (0–20 kHz) up to a 5 kHz fundamental; do not relax it.
- Renders are deterministic (drift is seeded per voice). No gate or threshold is loosened to
  make a test pass.

**Portability:** every change builds for all five targets with warnings as errors. No x86-only
intrinsics without a NEON path and runtime dispatch. GCC rejects float `==` and shadowing that
Clang accepts; `long` is 32 bits on Windows. The full trap list is in
`docs/engineering-notes.md`, "Known traps".

**Source lists:** new source files go into `VS_COMMON_SOURCES` or `VS_VINTAGE_SOURCES`
(`BS_SOURCES` for B-Side); a new test executable also into `scripts/sanitize-targets.sh`.

## Definition of done (per change)

1. `scripts/preflight.sh` passes in full: universal build, all tests, pluginval strictness 10,
   VST3 validator, sanitizers, Linux ARM64 container, security scan, repository hygiene.
2. Windows and Linux x86_64: one dispatched CI run per merge-ready branch (orchestrator). If a
   change cannot get one, list it in `docs/status.md` as merged without that verification.
3. Audio-thread code was reviewed with the skill `realtime-review`; parameter or state changes
   followed the skill `add-parameter`.
4. Records updated in the same pull request: `CHANGELOG.md`, `docs/status.md`, `README.md`,
   new judgement calls in `docs/decisions.md` (marked "(review)"), new traps in
   `docs/engineering-notes.md`.

Say plainly which checks you could not run and why. Report failures with their output.

## Check before you claim

Never state anything about the repository or a pull request from memory. Run the check in the
same turn as the sentence: `git status` and `git log` for local state, `git fetch` and a
comparison for pushed or behind, `gh pr view` / `gh pr checks` for pull requests and
`gh run view` for CI. An earlier result is a snapshot, not the present.

## Ending a turn: "What I need from you"

When a turn stops because the next step belongs to the person you work for, end it with a
section titled **"What I need from you"**:

- A numbered list; each item is one concrete action or one decision.
- Each item says what to do, where (command, file, app, URL) and what to send back.
- The most blocking or quickest item comes first.
- A decision lists its options and marks the recommended one.
- Every request goes in this section, even if it was mentioned earlier in the turn.
- After the list, one line on what you carry on with meanwhile, or "Blocked until #1".

When nothing is needed, end with "Nothing needed from you right now" and say whether the work
is finished or still going, and what comes next.
