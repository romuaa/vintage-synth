# B-Side: the reference-emulator adapter interface

Status: **built; first reference data committed** (2026-11-14, `tests/bside/reference/`, owner
decision Q4 in `docs/architecture.md` 8.12). This file is the contract for the adapter, which lives
**outside** this repository, under the reference emulator's licence. The repository holds only the
neutral formats, our replay driver, the comparer, the spec analysis and the generated reference data.

## What the adapter is

A command-line program that reads one `.bstrace` file, drives the reference emulator cycle by
cycle, and writes a `.bsref` and a `.bsspec` file. It never writes audio. It has no connection
to the plugin build: no CMake target of this repository builds, downloads or links it or the
emulator it wraps.

```
bside-oracle <trace.bstrace> --ref <out.bsref> --spec <out.bsspec>
```

Configuration is outside the repository: the plugin's tests find the built adapter through the
environment variable `BS_ORACLE` (a path), and only a developer who sets it can regenerate data.
`bside_trace` (ctest label `trace`) reads the committed data only; a test without data reports
itself skipped, never passed.

## Input: `.bstrace`

Format, bounds and error messages: `source/bside/trace/TraceFormat.h` (at most 100 000 writes and 49 999 periodic checkpoints, so a reference has at most 150 000 `state` lines). The adapter must reject a
file this repository's reader would reject (the same grammar, the same bounds), and must not
interpret anything beyond it. `clock` and `model` select the reference emulator's clock and chip
revision. The writes are applied at their exact cycles, in file order for equal cycles.

## Output 1: `.bsref` (digital state)

Format: `source/bside/trace/ReferenceFormat.h`. One `state` line per checkpoint, in the order of
`checkpointSchedule()` (`TraceFormat.h`):

| Checkpoint | Cycle | State is read |
|---|---|---|
| after every write | the write's cycle | right after that write was applied |
| periodic (`check N`) | N, 2N, ... strictly before `end` | before any write of that cycle |
| end | `end` | after every write at or before `end` |

The 20 fields are the digital state of 8.4.3 in the order `acc1 acc2 acc3 noise1 noise2 noise3
env1 env2 env3 phase1 phase2 phase3 rate1 rate2 rate3 div1 div2 div3 rd1B rd1C` (value ranges in
`ReferenceFormat.h`). The header words `trace-sha256` (SHA-256 of the trace file's bytes),
`adapter` and `emulator` (commit hashes of the adapter and of the emulator, 40 or 64 hex digits)
bind the file to its origin. **Hashes only**: no names, no URLs, no versions in words.

## Output 2: `.bsspec` (audio features)

Format: `source/bside/trace/ReferenceFormat.h`. The adapter renders the trace's output
at 48 kHz (mono), reduces it to the render-regression feature layout (23 bands, 25 ms hop, 0.1 dB
integers, one `frame` line per hop) and, for every segment the trace's analysis plan marks, the
1/12-octave averaged spectrum (120 bins from 20 Hz, 0.1 dB). The feature extraction is the same
one the repository's render-regression harness uses, so both sides measure alike.

**Output reference point.** The adapter takes the chip's output after its board filter, before the
emulator's own playback stage (no added background noise, no soft limiter, no 16-bit rounding), as
floats scaled so that 1.0 is that stage's 16-bit full scale. It resamples the per-cycle output to
48 kHz itself (a linear-phase filter flat to 20 kHz, at least 120 dB down from 28 kHz, exact rate).
Output sample i lies at chip cycle i x clock / 48000; a trace renders ceil (end x 48000 / clock)
samples.

**Analysis plan.** The trace format carries none, so the plan is derived from the trace alone
(`source/bside/trace/SpecAnalysis.h`, `segmentPlan()`): the write cycles split the render into
gaps; each gap that leaves at least 14 whole frames after 8 frames of settling (200 ms) is a
segment `s00`, `s01`, ... (at most 64). A trace meant for analog measurements therefore places
its writes at the boundaries of the segments it wants and nowhere else. `analyseRender()` builds
the whole spec; the adapter and our own tests call the same code.

The audio itself is never stored. Delay estimation (8.9.4: one constant delay within +-256
samples, by cross-correlation) therefore happens where both signals exist: inside the adapter when
it measures the reference, and in our own tests when they render ours; `estimateDelay()`
(`Compare.h`) is the shared helper.

## What the repository does with them

1. `readTrace`, `readReference`, `readSpec` (bounded, strict, no exception escapes).
2. `referenceMatchesTrace()`: same checkpoint count and cycles as the trace's schedule, and the
   SHA-256 of the trace bytes.
3. `replay()` (`Replay.h`): checks the reference against the trace bytes (SHA-256) and schedule, resets our chip from the trace, advances to each cycle, writes,
   compares the 20 fields at every checkpoint **exactly**.
4. `Compare.h`: the audio tolerances of 8.9.4 (corner +-5 %, resonance +-1 dB, distortion +-3 dB,
   voice DC +-1 dB, noise third-octave +-1.5 dB, scripted traces 3 dB per cell with at most 5 %
   outliers, mean 1 dB, level 0.5 dB).

## Provenance rules (from 8.9.5, repeated because they bind the adapter's author)

- The reference emulator is pinned by commit and archive SHA-256; the adapter by commit;
  `tests/bside/reference/MANIFEST` records hashes only.
- Regenerating reference data is reviewed like a golden re-bless.
- Our analog constants are fitted to the generated data, never copied from the emulator's source.

## Regenerating the reference data

Only on the development Mac, deliberately, and reviewed like a golden re-bless:

1. Commit the repository change first: the adapter compiles this repository's `source/bside/`
   (trace reader, writers, spec analysis) from the checkout and refuses uncommitted sources, so the
   MANIFEST can name the commit (`sources-commit`).
2. Run the adapter's one-command regeneration script (kept with the adapter, outside this
   repository) with the checkout's path. It checks the emulator pin (commit and archive SHA-256),
   builds the adapter, writes the traces, runs the adapter on each, replaces
   `tests/bside/reference/` and writes its `MANIFEST`. An optional self-check proves, on every
   trace, that the adapter's cycle loop is the emulator's own (sample for sample and state for state).
3. Review the diff; run `bside_trace` (ctest label `trace`): it checks that every file is listed in
   the MANIFEST with its SHA-256, that each reference and spec is canonical and belongs to its trace,
   and that each spec follows the segment plan. Commit the data in its own commit.

The traces are synthetic register scripts (no recorded music): `d_*` pin the digital state (writes
of 00 to register 0x15, which these traces never use otherwise, are "probes" that only place a
checkpoint), `a_*` are the analog measurements of 8.9.4 (`*_bypass` twins repeat a trace with the
voices not routed to the filter, so a filter response is the ratio of two spectra with the same
excitation). Each trace's comments say what it pins. Analog references exist for the 50 Hz clock
only; the reference's filter scales with the configured clock (`docs/decisions.md`).
