# Project status

Where the project is, what is parked, what comes next, and the history of what was merged.
The orchestrator keeps this file current; `CLAUDE.md` links here. Read "Where the project is"
first when resuming.

## Where the project is (2026-11-27) — read this first when resuming

**RESUME EXACTLY HERE (written 2026-11-27).**
- **S4b is merged (pull request 48, merge commit
  `c3078d0`, owner's word "merge", 2026-11-27). S4 is done. The work now is B-Side's first
  release.** The owner's decisions on its scope are `docs/decisions.md`, "B-Side's first
  release: owner decisions" (F1 to F5): S5 comes after the release; one repository release
  for both plugins as version 0.10.0; a factory bank of 64 programs drawn from a survey of
  original tunes; both editor windows show the plugin's name; B-Side's editor pages are
  still open. The steps, in order, each with its own contract before it starts (**none of
  these contracts exists yet, except the product name's**):
  1. **Product name in both editors** (contract 9.3.8, branch `ui/plugin-name`): built,
     reviewed independently ("mergeable after small fixes"; fixed), and the orchestrator's
     full `scripts/preflight.sh` passed on `1d0ea1e` (18m13s, all seven steps, full tier).
     A first gate run had failed in the macOS sanitizer step on the new B-Side editor test:
     the known false positive in the system's graphics code when an editor is created
     (`docs/engineering-notes.md`); the test skips there like the Vintage editor tests.
     **Waits for:** the owner's look at the pictures, one CI run for Windows and Linux
     x86_64 (not dispatched yet; the owner is asked first), the owner's merge word.
     **Open after it:** the eight pictures in `docs/images/` were taken before the name
     block existed and do not show it.
  2. **Survey for the bank of 64** (developer-only, outside the repository, in the developer
     folder of `docs/bside-oracle-adapter.md`, subfolder `bank64-survey`, `REPORT.md`): done.
     300 tunes analysed from their register data (nothing listened to); 48 programs
     proposed (11 leads, 8 basses, 7 arpeggios, 9 percussion sounds, 4 pads, 5 effects, 2
     Dual, 2 Split), 35 as they are and 13 with a named compromise that S5 lifts. **Waits
     for the owner:** "go" on the list, the arpeggio speed (frame or tempo) and whether to
     fill all 64 slots now. Then: the bank's contract (8.21.2) is extended, the programs are
     written and rendered, and the owner auditions them. A finding for the S5 contract:
     pulse-width and filter movement that keeps running across notes is common in the tunes
     and is not on the S5 list.
  3. **B-Side editor design:** wireframes and help texts in `docs/ui/` for the owner's
     approval; no code before that. Today `docs/ui/wireframes/` holds Vintage's eight
     pictures only.
  4. **B-Side editor build**, page by page, as M7 was done.
  5. **Packaging:** the packer, the identity check and the release run extended to B-Side.
  6. **Release 0.10.0:** rehearsal run, the owner's hands-on check, the owner publishes.
  - **CI time: the owner said "leave" (2026-11-27).** The 20-minute target for a full CI run
    is not chased further; the slow tests stay in the build row and the sanitizer tests in
    one job (`docs/decisions.md`, "Faster tests").
  - **Cleaned up after the merge (2026-11-27):** the remote branch `bside/s4b-sounds`
    (deleted by GitHub), the local branches `bside/s4b-sounds` and `bside/s4b-tiers`, the
    worktrees `.claude/worktrees/fast-tests` and `.claude/worktrees/s4b-tiers`. Branches
    left: `main`, `bside/s3a-modela` and `study/crossmod-aliasing` (both kept on purpose).
  - **CI run 45903351762 on `4b53c8e` (`targets=no-macos`, `tests=full`): green at the first
    attempt on every job**, Windows x64 included (both bundle jobs and the seven distro checks
    too); the pull request's CodeQL and security scan passed. Job totals: Windows x64 23m49s
    (build 10m22s on the refreshed compiler cache, smoke tests 0m57s, slow tests 10m35s),
    Windows ARM64 17m31s, Linux x86_64 16m13s, Linux ARM64 11m28s, sanitizers 26m43s (tests
    21m09s). Windows x64 and the sanitizer job are still over the 20-minute target, by test
    time, not build time. A `tests=smoke` run has not been tried.
  - **How the branch got here:** CI run 45706438078 (on `274bf15`) had Windows x64 cancelled by
    its 60-minute limit while the unit tests were still running. The owner did not raise the
    limit; the tests were made faster instead ("Faster tests" below, pull request 49, merged
    2026-11-26). Then, on this branch: `main` merged in (only `CHANGELOG.md` conflicted; both
    sides kept), `bside_render` registered through `vs_discover_tests`, and 30 of the branch's
    own test cases tagged `[slow]` (measured here at 0.3 s or more; tag strings only, no test
    body or threshold changed; none needed `[serial]`; done by an agent, diff read by the
    orchestrator, no independent reviewer). 1,235 tests, 212 of them slow. The orchestrator's
    full `scripts/preflight.sh` passed on `1f6f139` (13m05s, all seven steps, full tier, 18
    jobs); the commits after it change only this file.
  - The audition files are outside the repository (the developer folder of
    `docs/bside-oracle-adapter.md`, subfolders `s4b-audition`, `-2`, `-3`).

**B-Side S4, how it went (2026-11-24 to 2026-11-27; all merged).**
- **S4 is done.** The owner decided Model A's CPU budget (O11: Model A has its own gate at its
  measured cost; Model B keeps 5 % / 10 %). The contract is `docs/architecture.md` 8.21:
  S4a (the part and its wiring into the plugin) and S4b (factory programs, level rule, render
  goldens, the owner's audition). The orchestrator's nine readings in that contract
  (`docs/decisions.md`, "B-Side S4 contract") were confirmed by the owner on 2026-11-24;
  row 1 sets Model A's figures at 11 % (16 voices), 22 % (Dual), 27 % (Dual with effects)
  and 30 % (16 voices at a 192 kHz host).
- **S4a is merged (pull request 47, 2026-11-24, owner's word "merge").** Built, reviewed twice
  by independent reviewers with their own tests, fixed twice. The orchestrator's full
  `scripts/preflight.sh` passed on the final code (80 min, all seven steps) and every CPU gate
  passed at the orchestrator's bench (16 voices: 4.39 to 4.51 % on Model B, 9.71 to 9.73 % on
  Model A). **CI run 45455730779: Windows ARM64, Linux x86_64, Linux ARM64, the sanitizers and
  the distro checks passed. Windows x64 is not a green job:** its build, unit tests and
  pluginval steps all succeeded, but the job hit its 60-minute limit during clean-up and was
  marked cancelled, so the Windows bundle job did not run. The owner chose to merge without a
  rerun. The Windows x64 time problem was handled by "Faster tests" below
  (pull request 49).
- **What S4b is (built 2026-11-25, merged 2026-11-27, pull request 48):** `mix.level` per part (204 parameters), `kChipGain` 1.2822 (Init as
  loud as Vintage's Init), 16 factory programs with the level rule and per-program offsets,
  a program apply that fades the part, `bside_render` goldens for six programs. **The owner
  auditioned the bank in three rounds and accepted it** (`docs/decisions.md`, "S4b audition:
  the owner's decisions"): the steal stays as it is, Shared keeps the average of the notes'
  gains, the new Init level stays; A2, A6, B1, B3 and B4 were changed to the owner's ear and
  the kit balance set (offsets B1 +4.5, B2 +1.5, B3 +2.0, B4 +3 dB). **An engine defect was
  found by the hat and fixed:** under Clean a note could start 33 ms late; Clean now clears
  the envelope counters at every note start (8.21.2). An independent review with its own
  tests found no click and no real-time defect; its findings are fixed or written into the
  contract. The orchestrator's full `scripts/preflight.sh` passed on commit `4907ba4`
  (85 min, all seven steps, 1,235 tests on macOS) and every CPU gate passed at the
  orchestrator's bench (16 voices: 4.33 to 4.36 % on Model B, 9.59 to 9.63 % on Model A;
  chain 26.7 to 26.9 ns and 57.4 to 57.5 ns). CI run 45903351762 was
  green on every job (the resume item above).
- **S4b's readings await the owner's review:** rows 1 to 38 in `docs/decisions.md`, "S4b
  contract: orchestrator's calls".
- **Open after S4b:** B1 has no level trim left upwards (`mix.level` at +6 dB); the drums'
  hit-to-hit peak spread is within 1 dB only on the gate's phrase (up to 2.1 dB at other
  spacings: noise peaks are a draw); under Precondition and Authentic a note can still wait
  33 ms, as on the chip.
- **S4a's readings await the owner's review:** 20 by the implementer, 8 from the fix round and
  the calls on both reviews, all in `docs/decisions.md` under "B-Side S4 contract".
- **Owner decisions that bound S4** (`docs/decisions.md`, O1 to O11): Model B is accepted by
  ear; no further effort goes into Model A's sound (O10); its open points below are known
  deviations, not work items, unless the owner reopens them.
- **Open, recorded, not being worked on:** Model A's sweep opens later but faster than the
  reference and closes the same way; its cutoff steps below code 480; the 16 pinned
  overdriven Model A alias rows; mode-plus-cutoff write rows at -83 to -86 dB against the
  reviewers' ideals (unestablished); cutoff-table rebuilds on every resonance write
  (avoidable cost); a thin margin (write gate -90.7 dB against -90).
- **Kept on purpose:** remote branches `bside/s3a-modela` (the signal-dependent Model A form,
  not accepted, exists nowhere else) and `study/crossmod-aliasing`. Developer-only material
  under `~/Library/Caches/bside-oracle/` (listening files, review harnesses) survives a
  restart; nothing else from the sessions needs to.
- **Docker:** the agents' leftovers were removed on 2026-11-24 (owner's word). The images
  `vs-linux-check:*` belong to the local gate and are rebuilt by it when missing.

**Faster tests (pull request 49, merged 2026-11-26; contract `docs/architecture.md` 7.1; owner's
"go").** Why: two Windows x64 CI jobs in a row were cancelled at the 60-minute limit (pull
requests 47 and 48). The causes found: tests ran one at a time (20 to 28 minutes per target),
164 of 1,229 tests carried 90 % of that time, and the Windows x64 compiler cache on `main` was
stale after a compiler update on the runner image (0 % hits, a 31-minute build; a cache saved on
a feature branch is not readable from another branch). The limit is not raised (owner).
- **Built, reviewed independently, fixed:** tests tagged `[slow]` (182 of the 1,192 on `main`)
  give a smoke tier (`ctest -LE slow`) and the unchanged full tier; 23 tests with a time limit
  run alone (`[serial]`); every ctest call of the gates is parallel (`scripts/test-jobs.sh`,
  `VS_TEST_JOBS`); `scripts/preflight.sh --quick` runs the smoke tier; `build.yml` has the input
  `tests` (`full` default, `smoke`). The reviewer built `main` and the branch and found the same
  1,192 tests and labels, and no failure in ten random-order runs at 4 to 54 jobs, also under
  load; its findings (a guard script that could not fail, a docs check that did not read the
  workflow's use of the tier) are fixed and shown failing on mutations.
- **Orchestrator's gate on `05e175e`:** the full `scripts/preflight.sh` passed in 13m20s, all
  seven steps, full tier, 18 jobs (the same gate took 85 minutes on the S4b branch). Read from
  the gate's summary; `--quick` took 2m33s at the implementer's run.
- **CI run 45892104628 on `56d4092` (`targets=no-macos`, `tests=full`, owner's "dispatch"):
  green after one rerun of one job.** All four build rows and the sanitizer job passed at the
  first attempt, so the workflow change, the guard script under Git Bash and the changed test
  files on MSVC and GCC x86_64 are confirmed. Measured:

  | Job | Build | Smoke tests | Slow tests | Job total |
  |---|---|---|---|---|
  | Windows x64 (compiler cache still cold) | 27m45s | 0m57s | 10m58s | 43m02s |
  | Windows ARM64 | 8m43s | 0m49s | 6m41s | 18m00s |
  | Linux x86_64 | 5m36s | 0m37s | 6m30s | 14m41s |
  | Linux ARM64 | 6m12s | 0m33s | 5m43s | 13m15s |
  | Linux sanitizers | 4m19s | - | 25m01s (all tests) | 30m12s |

  Smoke meets its target (within 10 minutes per row) once the build is warm. **Two rows miss the
  20-minute full target:** Windows x64, expected until the run on `main` refreshes its cache,
  and the sanitizer job, whose tests take 25 minutes on four cores (46 minutes before).
- **The one failed job, `distro debian-13`:** pluginval's host crashed with the known signature
  (`docs/engineering-notes.md`, "pluginval 1.0.4 crashes at teardown") but between "Open plugin
  (cold)" and "Open plugin (warm)" of B-Side, not after the last test, so the wrapper rejected
  it as designed. The branch changes no plugin source. The rerun of that job passed; the
  accepted signature is unchanged.
- **Run on `main` after the merge (45898183484 on `5fa04c4`, owner's "yes"): green at the first
  attempt, every job.** It refreshed all five compiler caches. Windows x64 itself still built
  cold in this run (28 minutes, 0 % hits of 705 compiles, job total 43m25s) and saved the new
  cache at 22:10 UTC; the next Windows x64 run is the first that can show a warm build. Other
  job totals: Linux ARM64 13m19s, Windows ARM64 17m29s, Linux x86_64 23m20s (its build took
  9m12s and its slow tests 11m19s here, against 5m36s and 6m30s in the run before: runner
  variance, not measured further), sanitizers 26m06s.
- **Open:** whether the pluginval host crash in the middle of a run returns (seen once; then
  decide between a retry of the whole pluginval call and nothing); the two slowest tests
  (115 s and 77 s in CI) and the other heavy gates could be made cheaper, each a judgement on
  what a smaller set still gates. **Closed by the owner ("leave", 2026-11-27):** the
  20-minute target is not chased further, so no larger runner, no split sanitizer job and no
  job of their own for the slow tests.

**Exitium Vintage Synth 0.9.0 (a public beta) is released** (2026-11-20; shown as the latest release on GitHub since 2026-11-21, owner's word;
tag `v0.9.0` at commit `116c5f8`: three zips and `SHA256SUMS.txt`). **M8 is done.**
**Everything below is merged to `main`, B-Side S3 and S4 included (2026-11-27). Model A's
dynamics stay open as a known deviation (owner decisions O4, O5, O7 and O10).** **All five targets are confirmed** (2026-11-20): macOS by the local gate; Windows
x64 and ARM64 and Linux x86_64 and ARM64 by the first CI runs after the repository became public
("First CI runs" below).

- **Exitium Vintage Synth is feature-complete up to M7:** engine, effects, arp/seq, presets and
  64 factory programs, the custom editor with all pages (M7a–M7f). The owner has tried and
  approved every page.
- **Exitium B-Side:** S1 to S4 are merged (2026-11-27). The plugin plays its chip model: two
  parts of 16 note voices, both chip models with their filters, the patch program engine,
  16 factory programs (A1 to B8) and 204 host parameters. It still uses the placeholder
  editor and has no release; the owner's decisions on its first release (0.10.0) are in
  `docs/decisions.md`, "B-Side's first release: owner decisions".

**Repository housekeeping (2026-11-16, owner's word):** all merged branches are deleted, on GitHub
and locally. Kept since then: `study/crossmod-aliasing` (pushed by owner
decision: the Cross Mod aliasing study's experimental code, built against the old `source/dsp/`
layout, never to be merged as is; the reference for the deferred "Clean" Cross Mod switch).
The branches kept today are listed under "Kept on purpose" above.
GitHub deletes a PR's branch on merge; delete the local branch after each merge too.

**Skill `orchestrate` exists twice (2026-11-16):** the owner's personal, generic one
(`~/.claude/skills/orchestrate/`) and this repository's (`.claude/skills/orchestrate/SKILL.md`).
The personal one is the one a session lists and holds the process; its first instruction is to
read this repository's file, which holds only what is specific to this project (serial files,
worktrees, gates, model routing, commit rules) and overrides it. If a session did not read the
repository's file, read it by hand before orchestrating. Checked 2026-11-16: `/orchestrate`
loads the personal skill and reaches the repository's file through that instruction.

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
1. **Vintage:** 0.9.0 is released (2026-11-20; M8 is done). The identity (class IDs, bundle
   ID, codes) is frozen. The next release is 0.10.0, together with B-Side (skill `release`).
   How M8 went: History, "Vintage M8".
2. **B-Side:** S1 to S4 are merged. Next: the six steps of the resume item at the top
   (product name in both editors, the survey for the bank of 64, editor design, editor
   build, packaging, release 0.10.0). S5 comes after that release (owner decision F1).

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

- **Vintage M8, as it went (record, moved here 2026-11-27; written as a list of next steps).** **started 2026-11-19.** Order: (a) **done:** pre-public audit of the
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
  owner's word as a pre-release; the owner had that mark removed on 2026-11-21. **The identity (class IDs, bundle ID, codes) is frozen from
  now on.** The next release is made as the skill `release` describes.
  **Owner's studio check passed for now (2026-11-19):** played in the dev host from a USB MIDI
  keyboard; sound, MIDI Program Changes from the keyboard, mod wheel, pitch bend and the
  arpeggiator are fine. Not done: a real screen-reader pass.

### B-Side S3 and the state when parked (records, moved here 2026-11-27)

Written while the work was in flight and kept as written: "in flight", "waiting for the owner",
"open findings" and "unmerged" describe those days. Today S3a, S3b and S4 are all merged, the
branch `bside/s3a-filter` is gone, the S4 budget question was answered by owner decision O11,
and the obligations for S4 were met in S4a.

**B-Side is resumed (owner's word, 2026-11-22)** after the 0.9.0 release it waited for (owner
decision 2026-11-19: Vintage first). The owner's three answers on resuming are in
`docs/decisions.md`, "B-Side resumed: owner decisions" (O1: if the escalated Model A attempt
fails, a pull request with Model B and the output stage only, plus listening files; O2: a cheap
rate correction inside 7 ns or decide at S4; O3: S3b may start stacked on the S3a branch).

**B-Side S3a, merged, and S3b, in flight (2026-11-23):** `bside/s3a-filter` had `main` merged in (the tests give
the parked figures).
- **Diagnostic, done** (`tests/bside/trace/FilterDiagnosticTests.cpp`, hidden: run it with
  `bside_trace "[s3a-diag]"`; it asserts nothing the gates do not and took 249 s under the
  sanitizers in CI; report
  and data in the developer cache, `s3a-diag/REPORT.md`): the Model A corner errors are 99 %
  wrong coefficients, not a missing stage or the estimator. The reference is a two-pole whose
  two integrators follow the instantaneous node levels (DC acts on the low-pass integrator,
  swing on the band-pass one, a shared floor near 220 Hz); no law in a followed DC or rms comes
  closer than 13 to 15 % rms. Limits of the reference itself: cutoff 380 repeats only within
  3 %; at cutoff 300 to 380 with swing its shape is 1.2 to 2.6 dB away from any two-pole.
- **Escalated attempt on the Model A filter: failed, closed (2026-11-23).** Build round and fix
  round by a stronger model; the accuracy gates are not met. Its result, a signal-dependent
  form as a second entry point (`Filter::processSignalDependent`), is **not part of S3a** and
  stays on the pushed branch `bside/s3a-modela` (never merged as is). Figures of that form
  after the fix round: corners within 5 % on 58 % of the fitting and 55 % of the held-out
  segments (rms 10.3 / 10.7 %); on the gate traces worst corners 18.4 % (resonance), 33.5 %
  (level), 31.8 % (voices), 6.2 % (cutoff sweep); at rest `a_filter_a` 28 of 32 within 5 %
  (worst 9.1 %) and `a_modes_a` outside (6.52 % / 1.05 dB); about 35 ns per sample against 7,
  and it would alias inside its loop. `Filter::process` keeps round 2's fixed form for Model A
  (committed gates pass; the `a_s3a_*` corners stay reported, up to 74.7 % and 224.5 %).
  **Owner decision O1 applies: the owner judges the Model A deviation by ear.** Listening
  files (reference, fixed form, signal-dependent form; six cases with a README) are in the
  developer cache, `s3a-esc/listen/`. What a further attempt would need: decisions on that
  branch, "B-Side S3a round 3", rows T12 to T18.
- **Closing step, done (2026-11-23)** (contract 8.19.3; decisions "B-Side S3a closing step",
  rows C1 to C13, and "orchestrator's calls"). Built, reviewed independently a third time
  ("mergeable after small fixes"), fixed. On the branch:
  - the rate correction (matched poles and a designed output row at no per-sample cost): worst
    0.19 dB Model B, 0.10 dB Model A against the analog response at every rate (was 3.2 dB);
    the reviewer's own time-domain measurement gave 0.18 / 0.10 dB. The output row keeps the
    prototype's phase: within 30 degrees, but for seven named settings at 88.2 and 96 kHz
    (band-pass and low + band at low cutoffs, at 20 kHz), at most 33.7 degrees, gated at their
    figure plus 2;
  - `processWithCutoff` on a table of the mapping (within 0.05 dB and 0.8 degrees), following
    Model A's damping, band weights and `variation` at the cutoff given;
  - the Shared topology's DC: Model B equals n Per Note chips (also with signal in the band-
    and high-pass); Model A a documented, pinned difference of up to 2.7 dB at 16 chips;
  - tests that can fail (each shown failing on a mutation); the test sources build with GCC 13.
  **Known deviations left:** Model B's high-pass stop-band floor is about 6 to 7 dB low on
  noise at mid and full level (a second-order term fitted for it was removed again: right on
  noise, 6 to 12 dB too much second harmonic on band-pass and pulse tones, aliasing at -36 dB;
  decisions row C6); Model A's dynamics (above).
  **Orchestrator's gate on `6a47319`:** the full `scripts/preflight.sh` passed (65m43s: docs,
  scan, mac, validator, linux, both sanitizer runs). `chip_bench` alone, 96 kHz, three runs:
  `s3aB` 5.5 to 5.6 ns (budget 7), `s3aA` 7.5 to 7.7, `cutB` 10.4 to 10.6, `cutA` 27.2 to 27.4.
  **Open, outside S3a:** `ChipRender.cpp` fails one GCC 13 warning (`-Warray-bounds`); GCC 11,
  the compiler of CI and the local gate, accepts it. To fix in S3b, which reworks that file.
  **Windows and Linux x86_64, first CI run (45107913251, on `61983f3`):** Windows x64 and
  ARM64, Linux x86_64 and ARM64, both bundles and six of seven distribution checks passed.
  **Two jobs did not:** (1) `linux-sanitizers` was cancelled at its 40-minute limit with about
  27 of 1077 tests left (28 minutes on `main`; S3a's tests add about 8 minutes there): the
  diagnostic is hidden and the limit is 60 minutes now; (2) the Ubuntu 22.04 distribution
  check: pluginval ends with a segmentation fault at exit for **both** plugins, after every
  test passed; Vintage's code and the check are identical to `main`, where the same job
  passed on 2026-11-20 (the image and its packages are fetched by tag at run time), so the
  cause is outside this branch. **Second run (45112409601, on `35df64f`, Linux only):** the
  sanitizer job passed (53 minutes) and Linux ARM64 passed; Linux x86_64 failed in the same
  way, this time on B-Side in the build job (so the distribution checks had nothing to test).
  **Diagnosis:** a use-after-free in the validation tool itself at teardown (its bundled
  framework version; fixed upstream later), on Linux x86_64 in about 6 to 7 % of runs, with
  any plugin. Pull request 42 (`ci/pluginval-teardown`) runs the tool under a debugger and
  accepts a crash with that exact signature only; its run 45121955371 is green. Pull request
  42 is merged and `main` is merged in here. **Third run (45140132683, on `3faf2cc`, Linux
  only): green.** Linux x86_64, Linux ARM64, the sanitizer job, the bundle and all seven
  distribution checks passed; every pluginval invocation ended normally (no crash to accept
  in this run). Windows x64 and ARM64 were confirmed by the first run on `61983f3`; since
  then only the workflow, one test tag and records changed.
- **S3b (2026-11-23; owner decision O3).** S3a is merged (pull request 41, on the owner's
  word, 2026-11-23). Contract 8.19.4, two steps, each its own pull request: S3b-1 on
  `bside/s3b-chain` (pull request 43, now against `main`), S3b-2 on `bside/s3b-alias`.
  **S3b-1, built, reviewed independently, fixed; waiting for the owner:** the renderer's two
  buses in double precision, routing / volume / mode writes at their cycle, `ChipVoice` (the
  whole chain `Chip` -> `Filter` -> `OutputStage`, Per Note or with a shared filter), the
  traces S3a had to skip, now through the real chain. The review found real defects (Shared
  depended on the owner's block length; write timing a cycle late against the reference; a
  full write queue reordered writes; tests that could not fail); all fixed, the contract
  amended (8.19.4, "After the independent review"), decisions D1 to D20.
  **Orchestrator's gate on `379ffa9`:** the full `scripts/preflight.sh` passed (72m01s: docs,
  scan, mac, validator 47 of 47 for both plugins, linux, both sanitizer runs). `chip_bench`
  alone, three runs, 96 kHz: `s3aB` 5.5 to 5.9 ns (budget 7), `chainB` 26.9 to 27.4,
  `chainA` 42.6 to 45.6, `busesB` 19.9 to 20.6, `busesA` 34.0 to 34.2, `moveB` 34.7 to 36.8,
  `moveA` 65.4 to 66.5.
  **Not met:** the buses' cost allowance of 1 ns over the single sum holds for `typical` but
  not for `noise` (+1.2 to +2.5 ns at 192 kHz; one attempt, no gain; D20). Recorded, not
  widened. **For the owner's S4 budget question:** Model A's whole chain is about 43 ns per
  internal sample at 96 kHz on the S4 gate's patch (Model B about 27), before S3b-2 adds to
  it. **Windows and Linux x86_64:** the first CI run (45168692336) failed in one
  job, Linux ARM64, on a Vintage test that depended on the scheduler (the editor feed's
  seqlock test; fixed in the test, feed unchanged); the second (45175007274, on `44bc753`)
  was green on every job. **Merged** (pull request 43, on the owner's word, 2026-11-23).
  **Listening files** (filter sweeps of both models through the chain) are in the developer
  cache, `s3b1/listen/`.
  **S3b-2, the aliasing of the static curves, on `bside/s3b-alias`: built, reviewed
  independently four times, fixed after each; the fourth review found no defect in the
  code. Merged (pull request 44, on the owner's word "merge both when green", 2026-11-24;
  full local gate on `8405f75`, quick gate and CI run 45274663699 green on `6f5eebf`, after a
  first run failed to build two test files on Windows).** As built:
  a linear-phase half-band pre-filter before Model B's mixer curve (the curve at the internal
  rate); Model A's voice bend folded into the renderer; Model A's pre-filter on both buses and
  its output function, gain and mixer curve at twice the rate; Model A's mixer curve continued
  smoothly past the range it was fitted on. The chain's latency is a run-time value
  (`ChipVoice::latencySamples ()`; Model B 8 + 21 samples at 96 kHz, Model A more).
  **The reviews found, and the rounds fixed:** Model A's output-function coefficients half a
  2x sample early (mode writes 0.31 samples early); voice counts not delayed with the
  pre-filter; a Shared mode write taking effect at the end of the owner's block; resonance
  writes switching the filter at the window's first sample (up to -78 dB of excess), and a
  first fix that broke when another write landed inside its window (third review): replaced
  by one rule, the filter keeps one state and every change of its dynamics lands at its own
  place (resonance between two samples, cutoff from its own sample on); the excess over an
  ideal render (the same chain at 32 times the rate) is at most -97.7 dB for resonance
  alone and combined with volume, mode and cutoff writes at 1 to 200 cycles' distance; a test that passed while hiding failing rows; a
  test-side reference without Model B's pre-filter; several tests that could not fail.
  **The alias gate as it stands (8.19.4, restated after the first review):** absolute limits
  with ideal voices on every row and with real chips on the direct and low-pass rows; real
  chips elsewhere within 1 dB of the same chain with its curves at four times the rate; every
  multi-voice row over five constellations of voice phases. Full set: 22456 measurements, no
  row failing outside the pinned list.
  **Known deviation, pinned (owner decisions O6 and O8):** Model A with its mixer overdriven
  (two or three full-level voices through the low-pass at resonance 15): 16 listed rows, the
  excess over the limit up to 12.4 dB at 88.2 kHz, 11.1 dB at 96 kHz and 0.2 dB at 128 kHz,
  none above; each row gated two-sided at its worst figure. To revisit with the Model A
  filter form and the S4 budget.
  **Orchestrator's gate on `8405f75`:** the full `scripts/preflight.sh` passed (69m11s: docs,
  scan, mac with 1123 tests, validator 47 of 47 for both plugins, linux, both sanitizer
  runs). An earlier run of it had failed in the macOS sanitizer step on a read past the end
  of a buffer in a test's own measuring code (fixed in the test; figures moved by at most
  0.3 dB). `chip_bench` alone, three runs, 96 kHz: Model B filter + output stage +
  pre-filter (`s3bB`) 6.3 to 6.4 ns (budget 7); `chainB` 30.6 to 30.9; `chainA` 63.4 to
  64.2 (S3b-1: about 43); `busesA` 44.1 to 44.4; `moveB` 40.8 to 41.1; `moveA` 93.0 to 93.5.
  **For the owner's S4 budget question:** Model A's whole chain is about 64 ns per internal
  sample (the second reviewer: 16 Model A voices at 96 kHz are about 10 % of a core against
  the 5 % gate). A resonance write costs about 0.4 to 0.6 us with the cutoff still and 2 to
  3 us with it moving; a full tick of writes 3.5 to 6 us per voice.
  **The write gate:** 240 rows against an ideal render at 32 times the rate, all inside
  -90 dB; the thinnest is -90.7 dB (Model B, resonance writes through band + high-pass at
  88.2 kHz).
  **Obligations for S4:** the plugin target compiles the chip sources with the contraction
  rule (`bs_chip_fp_rules`, already applied to `BSide`); a shared filter's owner switches
  mode and resonance at `ChipVoice::sharedSwitchSample ()` (within half a sample; 0.6 on
  Model A's output) or blends as `ChipVoice.h` describes to reach 0.02; `ChipVoice::prepare`
  takes 0.5 to 8 ms (filter design), off the audio thread; the reported latency includes the
  pre-filters.
  **Open, small:** GCC 11 ran one statement three times in a range-for of the alias test
  (written out instead; cause not isolated; the third review found no undefined behaviour
  in the test's code); one unpinned row of the full set sits at its limit (Model A, 128 kHz,
  high-pass) and has only been run on this Mac; two bend mutations are reported, not tested;
  the third and fourth reviewers measured -83 to -86 dB against their own ideal renders on
  rows that combine a mode write and a cutoff write at resonance 15 (output bit-identical to
  the merged S3b-1 code; dependent on the reference's timing convention; not established as
  a defect); two of three cutoff-table rebuilds per resonance write are avoidable.
- **Model A's filter form, parked (owner decisions O4, O5, O7, O8):** the owner compared the
  fixed form, the signal-dependent form and three in-between candidates by ear against the
  reference: none accepted (fixed too soft, signal-dependent too harsh, the best candidate a
  little better everywhere but still brighter on the resonant sweep, at about ten times the
  fixed form's cost). The fixed form stays until the S4 budget decision. **Owner, 2026-11-23
  (O10): Model B is accepted by ear ("practically perfect; some minor audible things, ok");
  Model A opens later but faster than the reference and closes the same way, so the start
  and end of a sweep are cleaner than the reference; no further effort goes into Model A
  for now beyond finishing what is in flight.** **The cutoff jump at code 512, fixed and merged (scope O9; pull request 45, on the owner's
  word "merge both when green", 2026-11-24; full local gate on `69284ce`, the branch with
  S3b-2 merged in; CI run 45282103029 green on `9acd3e3`):** from code
  480 up Model A's cutoff mapping follows the reference (the fall at 511 -> 512 is -15.4 %
  against the reference's -15.0 %; in the owner's one-voice sweep the jump there is -1.44 dB
  in 2 to 5 kHz, was -2.23, reference -1.67; every write at or above 480 within 0.34 dB of
  the reference's step in that band); **up to register value 479.0 the mapping is
  bit-identical to the S3a form.** Reviewed independently twice (the first version, with a
  hand-shaped curve near code 384, failed its review; the second had the fall at 512 tuned to
  a gate, put back on the reference). **One named exception:** at the write 510 -> 515 the
  0.8 to 2 kHz band moves by 0.46 dB where the reference barely moves (a miss of 0.53 dB,
  gated at 0.58): the level dependence of Model A's corner. **Open, with Model A's parked
  dynamics:** below code 480 the sweep keeps its steps (383 -> 384: -1.42 dB against the
  reference's -0.49 dB), the reference's steps at 255 -> 256 and 319 -> 320 are missing, and
  the start and end of a sweep are cleaner than the reference (O10). Three reported corner
  lines in the join (codes 480 to 500) moved 1.3 to 3.8 points the wrong way; the resonant
  sweep's largest step fell from 2.33 to 1.45 dB (reference 0.79), not gated.
- **Independent review of round 2, done** (no commits; logs in the developer cache,
  `s3a-review2/`). **Model B and the output stage: mergeable after small fixes.** Open findings,
  to be fixed after the escalated attempt (they touch the same files):
  1. The static curves (Model B mixer curve, Model A per-voice bend and mixer, Model A output
     quintic) run at the internal rate and alias: -67 dB (Model B) and -53 dB (Model A) for a
     3.85 kHz saw at 88.2 kHz, against budgets of -100 and -80 dB. No contract gate covers it
     yet. To settle in the S3b contract before the renderer's interface is fixed.
     **Design study done (2026-11-23, prototypes in the developer cache, `s3b-alias/`):** every
     stage can meet its budget at every rate with 3 dB to spare. Model B mixer: a linear-phase
     half-band filter on the mixer sum before the curve (about +0.3 ns in the chain at 96 kHz,
     13 to 39 samples of latency). Model A voice bend: folded into the renderer (bent steps and
     bent slope changes; exact, no latency; about +2.5 to 5 ns per voice). Model A filter output
     function and mixer: the same pre-filter, then the curves at twice the rate (about +7.5 ns,
     24 to 46 samples). Antiderivative forms and plain 2x oversampling do not reach the budgets.
     Open: the methods move weak in-band products of bright tones by a few dB (judgement call
     for the S3b contract); a Model A filter with signal-dependent rates aliases inside its
     loop and would need the filter itself run at 2x to 4x.
  2. Rate dependence confirmed (1.94 dB at 88.2 kHz in round 2's metric, 3.31 dB over all modes
     up to 20 kHz), and a correction at no per-sample cost exists: matched poles and a fitted
     output row (low-pass 1.79 -> 0.02 dB; the band-pass alone needs a better numerator). This
     is the cheap correction of owner decision O2.
  3. Fitting hygiene: the resonance tables were fitted at the gate's own setting (cutoff 1024);
     harmless for Model B (held out within 0.16 dB), but Model A's committed corner gate
     (4.97 %) does not generalise (held out 13.4 %).
  4. `AnalogReferenceTests.cpp` and `ReferenceDataTests.cpp` do not build on GCC 13
     (a dangling-reference warning; GCC 11, the CI compiler, is clean).
  5. Model B high-pass and band-pass: the stop-band floor is about 10 dB too low at mid and full
     level on held-out traces (outliers up to 19.5 %); the committed mode gate passes.
  6. Shared topology: its DC differs from n Per Note chips (up to +3.4 dB on a volume step,
     Model B, 16 chips); the header comment claims otherwise and the test is blind to DC.
  7. Model A `processWithCutoff` keeps the stored Q and band-pass weights and ignores
     `variation`.
  8. Test gaps: nothing pins the board low-pass frequency; the volume-DC-off test writes the
     volume to 0 and so cannot see the volume applied before the high-pass; no cost assertion.

**B-Side, state when parked on 2026-11-19 (the background of the work in flight):**
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

### Earlier history

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

