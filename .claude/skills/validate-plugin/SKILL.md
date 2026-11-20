---
name: validate-plugin
description: Run unit tests, golden-render regression tests, CPU benchmarks, pluginval (strictness 10), and the Steinberg VST3 validator. Use before declaring any change done.
---

# Validate

**Run `scripts/preflight.sh` instead of the individual commands below** (they remain as reference):
`--quick` (hygiene, macOS build + tests + pluginval, Linux container) while iterating; the default set
(adds the security scan, the VST3 validator, and both sanitizer builds) before handing over; `--only <step>`
for one step, `--list` for the steps, `--clean` to remove its caches. The VST3 validator is built once from
the pinned SDK commit into `~/Library/Caches/vs-preflight/` and passes on `Result: N tests passed, 0 tests
failed`. Windows and Linux x86_64 are CI only, by a manual dispatch; agents never trigger CI.

Run these in order and stop at the first failure. Report each step as pass, fail, or not run, with the reason.

1. **Unit tests + host smoke:** `ctest --test-dir build/mac --output-on-failure`. This includes
   `host_smoke`, which loads the real `.vst3` through JUCE's VST3 host (identity, parameter count,
   rendering at 44.1/48/96 kHz, silence after release, state round-trip). It doesn't replace pluginval.
2. **Golden renders** (part of the local gate): `ctest --test-dir build/mac -L render`.
   Each factory preset renders a fixed MIDI file, and the output is compared against
   `tests/render_regression/golden/`.
   - Only regenerate goldens (`-DUPDATE_GOLDENS=ON`) for an **intended** sound change.
     State which presets changed and why.
3. **pluginval** (v1.0.4, same as CI). **Claude is allowed to run pluginval itself** (user rule,
   2026-11-12). Use the official v1.0.4 release from github.com/Tracktion/pluginval, unpacked into the
   session scratchpad (download it again if the scratchpad is new):
   ```bash
   <scratchpad>/pluginval/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 --validate-in-process \
     --skip-gui-tests "build/mac/VintageSynth_artefacts/Release/VST3/Exitium Vintage Synth.vst3"
   ```
   Pass = the output ends with `SUCCESS` and the exit code is 0. Only if the tool harness still
   refuses the command, ask the user under "What I need from you" to run it with `! `.
4. **VST3 validator:** ctest `vst3_validator`, built from Steinberg's SDK pinned in
   `.github/workflows/build.yml` (`VST3_SDK_TAG` / `VST3_SDK_SHA`). Locally the `validator` step of
   `scripts/preflight.sh` builds it once into `~/Library/Caches/vs-preflight/` and runs it; in CI
   it runs on macOS, Windows x64 and Linux x86_64. In a hand-made build the test is absent unless
   `-DVS_VST3_VALIDATOR=<path>` is given.
4b. **Sanitizers (before pushing engine or test changes):** configure a second build dir with
   `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64
   -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
   -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"` and run the whole `VintageSynthTests`
   binary. MSVC's heap catches overruns that macOS silently tolerates.
5. **CPU benchmark:** `build/mac/tests/voice_bench` (best of 3; run it with no build or other heavy
   job running). It prints one Part with one voice, then 1/16/32 voices driven directly, the
   per-part overhead and a projection against the budget. Baseline on an Apple M5 Pro, non-LTO
   `vs_test_core`, after M2 (2026-11-12): Part with one voice 0.32–0.36 % at 2×;
   **two Parts with 32 held voices at 2× = 4.9–5.1 % (0.157 % per voice)**, 9.7 % at 4×; the same
   with a steal every 20 ms and glide. (M1 was 11.3 % for 32 voices.) `voice_bench [base rate]
   [vintage]` measures other session rates: at 192 kHz, 2× costs 18.2 % (see decisions).
   `fx_bench`: chorus + delay + reverb about 0.55 % at 48 kHz (budget 1.5 %).
   This is slightly pessimistic for the shipped (LTO) plugin.
   Budget: 32 voices at 2× oversampling (Dual mode) under 15 % of one core at 48 kHz, effects
   included. 4× is offline only and has no real-time budget. Flag any regression above 5 %.

**Linux pre-flight (before pushing engine or test changes):** `scripts/linux-check.sh` builds
everything with GCC (warnings as errors, all errors shown) and runs the full ctest in a native
Linux ARM64 container; `scripts/linux-check.sh --sanitize` is the Clang ASan + UBSan run that
mirrors CI. About 1 min warm, 3 min cold; `--clean` removes its image and volume. It copies the
working tree by content, so it is safe across branch switches.

Linux and sanitizers in CI: the `linux-x64` row (ubuntu-22.04, GCC) runs everything including
pluginval and the validator; the `linux-sanitizers` job (ubuntu-24.04, Clang, ASan + UBSan,
`halt_on_error`) runs the tests without `host_smoke` in every CI tier. A Linux-only manual run:
`gh workflow run build.yml --ref <branch> -f targets=linux`.

Windows and Linux x86_64: a push starts no build. The orchestrator dispatches one run per
merge-ready branch (`gh workflow run build.yml --ref <branch> -f targets=no-macos`) and reads it
with `gh run view <id>` / `--log-failed`. Agents never trigger CI.

## Writing tests: traps we hit
- When a quality gate fails, write a hidden `[.diag]` test that isolates the stage before tuning anything.
- Filters: measure far below Nyquist (bilinear warping) and with small input (0.01) so the response
  stays linear and above the float floor. Our slope test uses fc = 500 Hz and a 2 k / 4 k pair.
- Alias measurements: coherent FFT (integer cycles) after the real decimator.
- Loaded-VST3 tests: JUCE's wrapper adds 2080 "MIDI CC" params + "Bypass"; exclude them. Compare
  stepped params by step index, not normalised value.
- New delay in the signal path breaks the sample-accurate note-on test until latency is updated.

DSP tests must cover: alias floor (≤ −100 dB up to a 5 kHz fundamental), VCF cutoff/Q accuracy,
ENV segment timing, sync/cross-mod continuity, sample-accurate note timing, and a
state save → load round-trip.

Windows x64 and ARM64 validation runs in CI (`.github/workflows/build.yml`). Report the CI result
or say it is pending. The `security` workflow (Trivy) must also be green.

## Audition render (manual listening and visual check)

```bash
VS_RENDER_DIR=<scratchpad>/renders build/mac/tests/VintageSynthTests "[.render]"
```
This writes `m1_demo.wav`, `m2_demo.wav` (polyphony) and `m5_fx_demo.wav` (effects), with spectrograms. Never read renders under `build/`, because it's
gitignored. Point `VS_RENDER_DIR` at the scratchpad instead. Look at the spectrogram for aliasing (lines
folding downward), clicks (vertical lines), and noise floor problems.
