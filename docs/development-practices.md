# Development practices

This repository has a second purpose besides the two instruments: to show, on a real project,
how AI-assisted development, automated testing, CI/CD and DevOps practice fit together. This
page describes what we do and why. The rules themselves are in [`CLAUDE.md`](../CLAUDE.md) and
[`CONTRIBUTING.md`](../CONTRIBUTING.md); the lessons behind them are in
[`engineering-notes.md`](engineering-notes.md).

## AI-assisted development

A human owner directs AI coding agents. The owner makes the product decisions and judges what
only a person can judge (how it sounds and looks, a release, a cost). Agents write, test and
review the code.

| Practice | Why |
|---|---|
| **Instructions are files in the repository.** `CLAUDE.md` holds the rules, `.claude/skills/` the procedures, `docs/` the contracts, decisions and status. | An agent starts with no memory. What is not written down does not exist for the next session, or for the next contributor. |
| **Contract before code.** An interface, its behaviour and its gates are written into [`architecture.md`](architecture.md) in their own commit, then built against. | Several agents can work in parallel, and a reviewer has something to check the code against. |
| **A brief per work item.** It names the goal, the contract section, the files the agent may touch, the base branch, the gates and what to hand back. | A vague brief gives vague work. Writing the brief also shows whether the item is understood. |
| **An independent reviewer with its own test harness.** A fresh agent gets the code and the contract, never the author's conclusions. | On this project the reviewer found real defects in every step, also after the author reported that all gates pass. |
| **The orchestrator reruns the full gate and reads the raw output.** | An agent's summary is a claim. A check that "passes" in one second may not have run at all. |
| **Check before you claim.** Repository and pull request state is read from `git` and `gh` in the same turn as the sentence that states it. | Memory of an earlier command is a snapshot; people and other sessions change the repository meanwhile. |
| **Failing the same gate twice ends the retries.** A diagnostic test isolates the failing stage, then a stronger model takes over. | Repeating the same attempt costs time and hides the cause. |
| **Decisions and lessons are logged.** Judgement calls go to [`decisions.md`](decisions.md) marked "(review)" until the owner confirms them; traps go to `engineering-notes.md`. | The owner can review choices afterwards without being asked about each one, and a trap bites only once. |
| **Every turn that stops ends with "What I need from you".** A numbered list of concrete actions and decisions, with the recommendation marked. | The human is the scarcest resource. A request buried in a status report gets missed. |
| **The owner approves merges and anything public or irreversible.** | Autonomy for the routine, a human for what cannot be undone. |

How the work is run day to day is the skill
[`orchestrate`](../.claude/skills/orchestrate/SKILL.md).

## Automated testing

- **One local gate, `scripts/preflight.sh`.** It runs everything one machine can check: the
  universal build with warnings as errors, unit tests, golden-render regression tests,
  pluginval at strictness 10, the VST3 validator, address and undefined-behaviour sanitizers, a
  GCC build in a Linux container, a security scan and repository hygiene checks.
- **Gates are numbers, written in the contract:** the alias limit, the CPU budget, latency.
  A threshold is never loosened to make a test pass.
- **Tests come from the specification.** For risky parts a second, independent model written
  from the contract alone is compared with the implementation.
- **Tests must be able to fail.** Checks have self-tests that feed them a known bad input, and
  reviewers ask of each test whether it can fail.
- **Renders are deterministic,** so a change in sound shows up as a difference, not as an opinion.

## CI/CD

- **Local first, CI as confirmation.** The local gate runs before every push. GitHub Actions
  confirms the platforms a Mac cannot run (Windows with MSVC, Linux x86_64).
- **The build workflow starts only by hand** (`workflow_dispatch`), once per merge-ready branch.
  The local gate fails if any workflow gains an automatic trigger. Only the short security scan
  and GitHub's code scanning run on every push and pull request.
- **A release is one run of the same workflow** with `release=true`: it builds all five targets,
  packs one zip per platform, unpacks each zip on a clean runner and loads the plugin from it,
  and only then creates a draft release with checksums. A person installs from the draft and
  publishes it. The design is section 10 of `architecture.md`.
- **One script for CI and for the local gate** (`scripts/package-release.sh`,
  `scripts/sanitize-targets.sh`), so the two cannot drift apart.

## DevOps and supply chain

- `main` is protected: changes arrive through pull requests; no force-push.
- Third-party actions are pinned by full commit SHA; runner images and submodules are pinned to
  exact versions; Dependabot proposes updates monthly.
- Workflows get the least permissions they need; only the job that creates the draft release
  may write to the repository.
- Trivy scans for vulnerabilities, secrets and misconfiguration; CodeQL analyses the C++,
  Python and workflow code.
- Before the repository became public its whole history was audited for secrets and private
  data.
- Every release names the exact commit and submodule versions it was built from (`SOURCE.txt`
  in each zip), as the AGPLv3 requires.

## If you want to copy this

Start with three files: a `CLAUDE.md` with your hard rules and definition of done, one script
that runs every check you have, and a decision log. Add contracts and independent review when
more than one agent works at a time.
