---
name: validate-plugin
description: Run unit tests, golden-render regression tests, CPU benchmarks, pluginval (strictness 10), and the Steinberg VST3 validator. Use before declaring any change done.
---

# Validate

Run these in order and stop at the first failure. Report each step as pass, fail, or not run, with the reason.

1. **Unit tests + host smoke:** `ctest --test-dir build/mac --output-on-failure`. This includes
   `host_smoke`, which loads the real `.vst3` through JUCE's VST3 host (identity, parameter count,
   rendering at 44.1/48/96 kHz, silence after release, state round-trip). It doesn't replace pluginval.
2. **Golden renders** (from M6, when factory presets exist; not set up yet): `ctest --test-dir build/mac -L render`.
   Each factory preset renders a fixed MIDI file, and the output is compared against
   `tests/render_regression/golden/`.
   - Only regenerate goldens (`-DUPDATE_GOLDENS=ON`) for an **intended** sound change.
     State which presets changed and why.
3. **pluginval** (v1.0.4, same as CI). **Claude is allowed to run pluginval itself** (user rule,
   2026-11-12). Use the official v1.0.4 release from github.com/Tracktion/pluginval, unpacked into the
   session scratchpad (download it again if the scratchpad is new):
   ```bash
   <scratchpad>/pluginval/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 --validate-in-process \
     --skip-gui-tests "build/mac/VintageSynth_artefacts/Release/VST3/Vintage Synth.vst3"
   ```
   Pass = the output ends with `SUCCESS` and the exit code is 0. Only if the tool harness still
   refuses the command, ask the user under "What I need from you" to run it with `! `.
4. **VST3 validator:** runs in CI only (macOS and Windows x64) as ctest `vst3_validator`, built from
   Steinberg's SDK pinned in `.github/workflows/build.yml` (`VST3_SDK_TAG` / `VST3_SDK_SHA`). It is
   not built on this Mac (owner decision: CI only). Locally the test is absent unless
   `-DVS_VST3_VALIDATOR=<path>` is given. Report it from the CI run.
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

Windows CI: after a push, check with `gh run list --limit 3` and `gh run view <id> --log-failed`.
CI minutes cost money while the repo is private: push a branch once, when everything above is green.
Draft PRs build Windows x64 only; mark the PR ready (`gh pr ready <n>`) for the full matrix.

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
