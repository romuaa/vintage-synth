---
name: orchestrate
description: Start or resume the orchestrator role in this repository - the project-specific rules (serial files, worktrees, gates, model routing, commit rules) on top of the generic orchestration process. Use when the user types /orchestrate or asks to continue milestone work with agents.
---

# Orchestrate (Exitium Vintage Synth and B-Side)

This file holds only what is specific to this repository. The process itself (startup, briefs,
the pipeline, acceptance, records, reporting) is the owner's generic `orchestrate` skill; where
the two differ, this file wins, and `CLAUDE.md` wins over both.

If the generic skill is not installed, the process in one line: contract first → brief →
implementer → independent reviewer with its own harness → fixes → your own full
`scripts/preflight.sh` → PR → merge only on the owner's word. You plan, brief, verify and merge;
agents write the code.

## Startup

1. Read `CLAUDE.md`, `docs/status.md` (where the project is and what comes next; trust it
   over this file), `CHANGELOG.md`, `docs/decisions.md`, `docs/engineering-notes.md`,
   `docs/architecture.md` (the contracts) and the skills in `.claude/skills/`.
2. Check the repository: `git status`, all branches, worktrees, stashes, `gh pr list`.
   `gh run list` shows the `security` and CodeQL runs and the builds you dispatched; `build`
   never runs by itself.
3. Tell the owner in a few lines what is merged, what is in flight and what comes next. **If
   `docs/status.md` says the owner said "wait", start nothing** until the owner says "continue".

## Limits specific to this project

- **Verification is local-first** (owner decision, permanent). Agents run `scripts/preflight.sh`
  (`--quick` while iterating, the full default set for engine changes) and never push or trigger
  CI. You rerun the full gate yourself before a PR. Windows and Linux x86_64 are confirmed by CI
  only: at most one dispatched run per merge-ready branch
  (`gh workflow run build.yml --ref <branch> -f targets=no-macos`), and you warn the owner before any batch. A merge that could not get a run is listed in
  `docs/status.md` as merged without Windows / Linux x86_64 verification. Never claim those
  platforms green without a passing run.
- **Owner gates:** the owner judges by ear or eye (auditions, editor pages, factory sounds).
  Prepare what they need: rendered WAV and spectrogram in the scratchpad via `VS_RENDER_DIR`,
  or the plugin in the dev host (`scripts/dev-host.sh`).
- **Judgement calls** (ranges, policies, panel interpretation): decide, log them in
  `docs/decisions.md` marked "(review)", and carry on.
- **Every brief names the hard rules that apply:** real-time safety, AGPLv3-compatible
  dependencies only, the alias gates, all five targets, never read ignored paths or `build/`.
- **No third-party brand names, part numbers or nicknames** anywhere in the repository, briefs
  included. Describe the B-Side chip generically; its revisions are Model A and Model B. The
  reference emulator and the other developer tools stay outside the repository and are never
  named in it.
- **Commit messages and PR descriptions never contain a `Claude-Session:` link** (owner
  decision). End commit messages with the Co-Authored-By line only, and say so in every brief.
- After a merge, delete the local branch too (GitHub deletes the remote one).

## Serial files (one agent at a time)

- `source/vintage/engine/Part.*`, `source/vintage/engine/Voice.*`, both `PluginProcessor.*`,
  and `source/common/shell/` (the shared processor skeleton).
- Parameter lists (`PartParamList.h`, the shared and B-Side parameter files): new parameter IDs
  are added in one serial step, by one agent, following the skill `add-parameter`.
- The source lists in `CMakeLists.txt` (`VS_COMMON_SOURCES`, `VS_VINTAGE_SOURCES`,
  `BS_SOURCES`) and `scripts/sanitize-targets.sh`.
- The page list in `VintageEditor::buildPages`.
- `docs/architecture.md`: changed only in its own commit, by you, before the code.
- `source/bside/chip/` and `tests/bside/unit/IndependentModel.h` are never edited by the same
  agent (the second is the independent check of the first).

Vintage (`source/vintage/`) and B-Side (`source/bside/`) never include each other, so work on
one runs in parallel with work on the other. At most 2–4 agents at once.

## Worktrees and builds

- Agent worktrees live in `.claude/worktrees/` (excluded locally; never stage that directory),
  each with its own build directory and branch. **Worktrees start from `main`, not from your
  current branch:** name the base branch in every brief.
- Submodules are empty in a fresh worktree, and the sandbox refuses `git config submodule.*`
  there. What works: replace the empty `external/JUCE` and `external/Catch2` directories with
  symlinks to the main checkout's copies. `git status` then errors in that worktree, so the agent
  commits with explicit paths (`git add <file>`, never `git add -A`) and you take the commit by
  branch name.
- Agents never unpack, link or build inside the main checkout or a folder they did not create.
- Agents iterate arm64-only (`-DCMAKE_OSX_ARCHITECTURES=arm64`). The universal build, pluginval
  and the benchmarks (`voice_bench`, `fx_bench`, `chip_bench`) run at your gate, one at a time,
  with no other build running, or the numbers mean nothing.
- When an agent is finished, remove its worktree (`git worktree remove --force`).

## Budgets (gates; current figures in the skill `validate-plugin` and the contracts)

- Vintage: 32 voices at 2× oversampling under 15 % of one core at 48 kHz, effects included.
- B-Side: the S4 render budget and the alias budgets of `docs/architecture.md` 8.4.5 and 8.17.

No item is done while a budget is exceeded.

## Model routing

| Model | Used for |
|---|---|
| Orchestrator (session model) | Contracts, briefs, acceptance, merges, decision log |
| `opus` | DSP and algorithm design, CPU optimisation, the chip model and its renderer, real-time code, independent review (with the skill `realtime-review` for audio-thread code), diagnosing a failed gate |
| `sonnet` | Implementation against a written interface: tests, benchmarks, parameter plumbing, preset I/O, CMake and CI files, UI components |
| `haiku` | Log triage, changelog and docs sync, simple searches |

An agent that fails the same gate twice is not retried: have a diagnostic test written that
isolates the stage, then hand the item one tier up.

## Reporting

End every turn that stops with the "What I need from you" section (the owner's global
instructions define it). Say plainly which gates were not run and why; report failures with
their output, not a summary.
