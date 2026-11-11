---
name: validate-plugin
description: Run unit tests, golden-render regression tests, CPU benchmarks, pluginval (strictness 10), and the Steinberg VST3 validator. Use before declaring any change done.
---

# Validate

Run these in order and stop at the first failure. Report each step as pass, fail, or not run, with the reason.

1. **Unit tests:** `ctest --test-dir build/mac --output-on-failure`
2. **Golden renders:** `ctest --test-dir build/mac -L render`.
   Each factory preset renders a fixed MIDI file, and the output is compared against
   `tests/render_regression/golden/`.
   - Only regenerate goldens (`-DUPDATE_GOLDENS=ON`) for an **intended** sound change.
     State which presets changed and why.
3. **pluginval** (v1.0.4, same as CI). Claude may not be allowed to run downloaded binaries.
   If it's blocked, ask the user under "What I need from you" to run this command with `! `:
   ```bash
   pluginval.app/Contents/MacOS/pluginval --strictness-level 10 --validate-in-process \
     --skip-gui-tests "build/mac/VintageSynth_artefacts/Release/VST3/Vintage Synth.vst3"
   ```
   Pass = the output ends with `SUCCESS` and the exit code is 0.
4. **VST3 validator** (not wired up yet; it gets built from the VST3 SDK once added):
   `validator "<path>.vst3"`
5. **CPU benchmark** (from M2 on): `build/mac/benchmarks/voice_bench`.
   Budget: 16 voices per part with 2× oversampling, and 32 voices in Dual mode must stay under 15% of one
   core at 48 kHz. Flag any regression above 5%.

DSP tests must cover: alias floor (below −90 dB up to a 5 kHz fundamental), VCF cutoff/Q accuracy,
ENV segment timing, sync/cross-mod continuity, sample-accurate note timing, and a
state save → load round-trip.

Windows x64 and ARM64 validation runs in CI (`.github/workflows/build.yml`). Report the CI result
or say it is pending.
