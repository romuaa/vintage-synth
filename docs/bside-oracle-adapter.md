# B-Side: the reference-emulator adapter interface

Status: **interface only.** Nothing is downloaded, built or generated (owner decision Q4 is
pending, `docs/architecture.md` 8.9.5). This file is the contract for the adapter that will live
**outside** this repository, under the reference emulator's licence. The repository holds only the
neutral formats, our replay driver, the comparer and the generated reference data.

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
