---
name: validate-plugin
description: Run unit tests, golden-render regression tests, CPU benchmarks, pluginval (strictness 10), and the Steinberg VST3 validator. Use before declaring any change done.
---

# Validate

Run these in order and stop at the first failure. Report each step as pass, fail, or not run, with the reason.

1. **Unit tests + host smoke:** `ctest --test-dir build/mac --output-on-failure`. This includes
   `host_smoke`, which loads the real `.vst3` through JUCE's VST3 host (identity, parameter count,
   rendering at 44.1/48/96 kHz, silence after release, state round-trip). Claude can run it even
   when pluginval is blocked. It doesn't replace pluginval.
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
4. **VST3 validator** (not wired up yet; it gets built from the VST3 SDK once added):
   `validator "<path>.vst3"`
5. **CPU benchmark:** `build/mac/tests/voice_bench`. M1 baseline on an Apple M5 Pro: 0.35–0.43 % of one core
   per voice at 2×, and 0.71–0.75 % at 4× (run-to-run spread).
   Budget: 16 voices per part with 2× oversampling, and 32 voices in Dual mode must stay under 15% of one
   core at 48 kHz. Flag any regression above 5%.

Windows CI: after a push, check with `gh run list --limit 3` and `gh run view <id> --log-failed`.

## Writing tests: traps we hit
- When a quality gate fails, write a hidden `[.diag]` test that isolates the stage before tuning anything.
- Filters: measure far below Nyquist (bilinear warping) and with small input (0.01) so the response
  stays linear and above the float floor. Our slope test uses fc = 500 Hz and a 2 k / 4 k pair.
- Alias measurements: coherent FFT (integer cycles) after the real decimator.
- Loaded-VST3 tests: JUCE's wrapper adds 2080 "MIDI CC" params + "Bypass"; exclude them. Compare
  stepped params by step index, not normalised value.
- New delay in the signal path breaks the sample-accurate note-on test until latency is updated.

DSP tests must cover: alias floor (below −90 dB up to a 5 kHz fundamental), VCF cutoff/Q accuracy,
ENV segment timing, sync/cross-mod continuity, sample-accurate note timing, and a
state save → load round-trip.

Windows x64 and ARM64 validation runs in CI (`.github/workflows/build.yml`). Report the CI result
or say it is pending.

## Audition render (manual listening and visual check)

```bash
VS_RENDER_DIR=<scratchpad>/renders build/mac/tests/VintageSynthTests "[.render]"
```
This writes `m1_demo.wav` and `m1_demo_spectrogram.png`. Never read renders under `build/`, because it's
gitignored. Point `VS_RENDER_DIR` at the scratchpad instead. Look at the spectrogram for aliasing (lines
folding downward), clicks (vertical lines), and noise floor problems.
