# Engineering notes — lessons learned

Things that went wrong or surprised us, the fix, and the rule that follows from each.
`docs/decisions.md` records *what* we chose; this file records the *traps*. Newest milestone first.

## Wave 2: M3 bi-timbral and M4 arpeggiator/sequencer (2026-11-12)

| Problem | Cause | Fix / rule |
|---|---|---|
| Reported latency was 2–3 samples too high. | `juce::dsp::Oversampling::getLatencyInSamples()` is the round-trip latency including the upsampler; the voice path only decimates. | Measure the decimator's own delay at prepare time. **Verify latency with a phase measurement, not an onset threshold.** |
| Long envelopes were wrong at 192–384 kHz internal rates. | `1 − exp(−x)` in float, and float accumulators for level, phase and seconds, stall or drift at high rates. | Double precision for slow per-sample state; `expm1` for coefficients. |
| Block-size dependence grew with the sample rate. | Smoothers decayed per sub-block in a chain accumulate rounding with the number of sub-blocks. | Compute from the absolute time since the last retarget. |
| A block-size test passed at 48 kHz only by luck. | It fed events out of time order; the part applies an out-of-order event at the previous position. | Test rigs sort events. Run behaviour tests at every rate configuration. |
| Sync + cross-mod made equivalence measures jump by 30 dB. | The patch is chaotic. | Gate control-rate paths with a smooth patch; bound chaotic patches separately. |
| The alias gate had never measured cross-mod. | The original test covered plain waveforms and sync. | **A quality gate only covers what it measures: list the configurations a gate has not seen.** |
| Arpeggiated notes piled up while the sustain pedal was down. | The module used the pedal and also passed it to the part, which then deferred every note-off. | An event transformer either consumes a controller or passes it, never both. |
| A note-off could be lost when the output buffer was full. | The "owed" state was cleared even when the write failed. | Clear state only after a successful emit; reserve a slot for the overflow marker; port the reviewer's fuzz into the suite. |
| The timing "independent model" shared rules with the implementation. | It was written by the implementer. | State contract rules once as the spec; assert properties from musical time alone for the rest. |
| Tests invoked with a comma in a Catch2 filter matched nothing. | Catch2 splits filters on commas. | Filter by prefix with `*`. |

## Linux target and public-release prep (2026-11-12)

| Problem | Cause | Fix / rule |
|---|---|---|
| The first Linux build failed on `x != 0.0f`. | GCC's `-Wfloat-equal` is in JUCE's recommended warnings and is an error; Clang and MSVC accept the comparison. | Use `std::fpclassify` (or compare bit patterns). GCC-only warnings show up in the Linux CI row only. |
| A CI round trip showed only one compile error. | Ninja stops at the first error. | CI builds with `-- -k 0`, so one run reports every error. |
| Where to put the static C++ runtime flags. | Tests link `vs_test_core`, not the plugin. | `-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL` on `VintageSynth_VST3` only; the bundle check fails if `ldd` shows libstdc++ or a missing library. |
| A plugin built on a new distribution doesn't load on older ones. | glibc symbol versions. | Build releases on the oldest runner image offered (ubuntu-22.04 → glibc 2.35). |
| pluginval was downloaded in CI without a checksum. | Release assets can be replaced. | SHA-256 check before unzip on all platforms; bump version and hashes together. |
| A link to the agent session appeared in commit messages. | Default attribution setting. | Owner decision: commit messages end with the Co-Authored-By line only. |

## Wave 1: M2b polyphony (2026-11-12)

| Problem | Cause | Fix / rule |
|---|---|---|
| Notes on a reused voice differed with the host block size (up to −5.7 dB in Solo); a reused Poly voice started its filter envelope part-way open. | A voice that stops inside a sub-block keeps evolving to the sub-block's end. Oscillator phase, noise seeds, ENV-1, filters and drift were inherited by the next note. | **Every piece of per-voice state a later note inherits must be reset at restart or computed from absolute time.** |
| A new note landed on a different voice depending on block size. | Idle voices were reported in loop-index order when two finished in the same sub-block. | Order-dependent bookkeeping follows sample time, never loop index. |
| Glide and drift left −74 to −93 dB between block sizes. | A pitch value stepped per sub-block turns float rounding into a lasting phase difference. | Evaluate pitch terms from absolute time in double precision and round once. |
| Envelope times changed one host block after a legato note change. | The times were only re-applied at block start. | A state change caused by an event is applied at the event. |
| Unison at detune 0 had a random level on every note. | Independent random start phases sum coherently. | Give unison voices a fixed phase pattern whose sum is √n. |
| The block-size tests passed while all of the above existed. | No test reused a voice: Poly took a fresh voice per note. | **A block-size test only proves something if voices are reused** (Solo/Unison, or Poly after idle), with envelopes of different lengths. |
| Black-box tests written from the spec found nothing, but the code review found five defects. | Spec tests check stated behaviour; the defects were in properties the spec only states globally (block-size independence) or not at all (unison at detune 0). | Keep both: spec-first tests *and* an adversarial review with a fuzzer. |

## Wave 1: M2a, effects, test infrastructure (2026-11-12)

| Problem | Cause | Fix / rule |
|---|---|---|
| Windows CI crashed in one new test (`0xc0000374` heap corruption on x64, `0xc0000409` on ARM64); macOS passed. | The test rendered a full 256-sample block past the end of a 24000-sample vector. The MSVC heap detects that; macOS without a sanitizer does not. | **Run the test binary under AddressSanitizer + UBSan before pushing engine or test changes** (`-fsanitize=address,undefined` in a second build dir). Test renders go through a helper that checks bounds. |
| 32 voices cost 11.3 % although each math call is cheap in isolation. | The voice is one long serial dependency chain per sample; `exp2`/`tan`/`tanh` cost 3–4× more inside it than alone. | Interleave voices sample by sample, vectorise the coefficient work per block. **Profile by stubbing stages out, not by timing functions alone.** (`sample`/Instruments don't work in the sandbox.) |
| Tiny permanent pitch and cutoff offsets; renders depended on block size with Vintage on. | `s += (t − s)·a` stops moving once the step is below half an ulp, up to hundreds of ulps short, and the stall point depends on the step size. | Keep one-pole smoothers as target + decaying deviation. |
| `alignas` members made MSVC warn C4324 (padding) at `/W4`, an error in CI. | Warnings are errors. | Avoid `alignas` on struct members; MSVC-only warnings show up in CI only. |
| The VST3 validator test "passed" in 0.2 s. | ctest only looked at the exit code and hid the output. | Require the tool's own summary line with `PASS_REGULAR_EXPRESSION`. **Check that a new CI check can fail.** |
| Four effect tests could not fail for what they claimed (e.g. an interpolation test that linear interpolation would pass; a decay test that only used whole-sample delays). | Tests were written by the implementer against the implementation. | An independent reviewer asks of every test: "what wrong implementation would still pass?" |
| Waking an idle delay/reverb could clear megabytes in one audio block. | "Clear in slices while idle" was finished at once on wake. | No bulk clearing on the audio thread: hide stale content by gating reads; `reset()` must be O(1). |
| Counting allocations by replacing global `operator new` misses `malloc` (JUCE `HeapBlock`). | JUCE containers call `std::malloc` directly. | On macOS use libmalloc's `malloc_logger` hook, filtered to the test thread. The check is skipped on Windows and under ASan. |
| Agent worktrees started from `main`, not from the wave branch, and lacked the contract file. | The worktree tool branches from the default branch. | Name the base branch in every brief. |
| Submodules are empty in agent worktrees and the sandbox refuses `git config submodule.*`. | Sandbox. | Symlink `external/JUCE` and `external/Catch2` to the main checkout; commit with explicit paths (plain `git commit` may refuse; `commit-tree` works). |

## M0–M1 and CI hardening (2026-11-12)

### DSP

| Problem | Cause | Fix / rule |
|---|---|---|
| Aliasing measured only −52 to −65 dB at 1–2 kHz fundamentals. | 2-point PolyBLEP is too short a correction at 2× oversampling. A raw-alias diagnostic test (`[.diag]`) showed the oscillator was the cause, not the decimator. | Windowed-sinc BLEP/BLAMP table (16 taps, Kaiser β = 8): ≤ −113 dB. **When a quality gate fails, isolate the stage with a diagnostic test before tuning anything.** |
| The latency test failed after switching to the BLEP table. | The table delays the oscillator output by 8 oversampled samples. | Reported latency = decimator latency + `Oscillator::kLatency / osFactor`. **Any DSP stage that adds delay must add it to the reported latency, and the sample-accurate note-on test catches it.** |
| The filter slope test gave wrong slopes. | 1) Bilinear (TPT) warping near Nyquist bends the response. 2) At high attenuation the float precision floor dominates. | Measure at fc = 500 Hz with a 2 k / 4 k Hz pair and input amplitude 0.01 (linear region). **Measure filter responses far below Nyquist and above the float noise floor.** |
| JUCE `Oversampling` needs an upsampling pass before it can decimate. | API design. | Voices render directly at the oversampled rate; the upsampler is fed silence. |

### Real-time safety

| Problem | Fix / rule |
|---|---|
| Building a `juce::MidiMessage` from a `MidiBuffer` event can allocate (SysEx longer than the inline storage). | Parse raw bytes from `MidiMessageMetadata` (`data`, `numBytes`). **Never construct `MidiMessage` on the audio thread.** Covered by a test that sends SysEx. |
| Hosts may call `processBlock` with more samples than `prepareToPlay` announced. | `processBlock` renders in chunks of at most the prepared block size. |
| A NaN from a bug would reach the host's output. | Output guard: non-finite → 0, `jassertfalse`, reset the part; then clamp to ±4.0. |

### Build and CMake (JUCE 9.0.5)

| Problem | Fix / rule |
|---|---|
| Test targets couldn't find JUCE headers or saw different `JucePlugin_*` defines. | Tests take `$<TARGET_PROPERTY:VintageSynth,INCLUDE_DIRECTORIES>` and `…,COMPILE_DEFINITIONS>` from the plugin target. |
| `ctest` received a literal, unevaluated generator expression as the plugin path. | `JUCE_PLUGIN_ARTEFACT_FILE` itself contains genexes: wrap it in `$<GENEX_EVAL:…>`. |
| JUCE 9 warns (→ error) on `JUCE_DISPLAY_SPLASH_SCREEN`. | Don't define it. |
| `AudioFormat::createWriterFor(OutputStream*, …)` is deprecated (→ error). | Use the `AudioFormatWriterOptions` overload. |
| `-Wimplicit-int-float-conversion` in a test. | Warnings are errors in tests too; cast explicitly. |
| Hosting a VST3 in a console test app. | `juce_add_console_app` + `JUCE_PLUGINHOST_VST3=1` + `juce_audio_processors_headless`; register formats with `addHeadlessDefaultFormatsToManager`. |

### Testing a loaded VST3

| Problem | Fix / rule |
|---|---|
| The host saw 2159 parameters instead of 78. | JUCE's VST3 wrapper adds 2080 "MIDI CC" emulation parameters and "Bypass". Count our own parameters by excluding those names. |
| State round-trip compared stepped (choice) params unequal. | Normalised floats of stepped params differ after a round-trip; compare by step index. |

### CI

| Problem | Fix / rule |
|---|---|
| First CI run: both Windows jobs failed at configure: "Visual Studio 17 2022 could not find any instance of Visual Studio". macOS passed. | The runner images no longer ship VS 2022. Don't hardcode `-G "Visual Studio …"`; pass only `-A x64` / `-A ARM64` and let CMake pick the newest VS. |
| Actions were three majors behind (`checkout@v4`, `upload-artifact@v4`, `download-artifact@v4`) and referenced by mutable tags. | Updated to checkout v7.0.1, upload-artifact v7.0.2, download-artifact v8.0.2, pinned by commit SHA. Dependabot keeps them current. Mutable tags are a supply-chain risk: `aquasecurity/trivy-action` tags were hijacked in early 2026. Resolve SHAs with `gh api repos/<owner>/<repo>/commits/<tag> --jq .sha`. |
| Windows x64 CI took ~18 min (build 15 min). | LTO flags were PUBLIC on the plugin's shared code, so the plugin, `voice_bench` and the unit tests each ran a full MSVC link-time code generation (4.5 + 1 + 4 min). Fix: LTO PRIVATE on the plugin targets only; tests link `vs_test_core` (same sources, no LTO). Plus Ninja and a compiler cache (ccache/sccache) on every CI job. |
| First run after the speed-up was *slower* (macOS 14 min vs 7, Win x64 21 vs 20, Win ARM64 15.5 vs 16). | Cold compiler cache (0 hits of ~250 compiles; ~100 MB stored), plus `vs_test_core` compiles JUCE a second time. Judge cache changes on the **second** run; check `--show-stats` hit rate in the "Compiler cache statistics" step. Warm run (2026-11-12, docs-only commit): macOS 3.5 min, Win x64 6.7 min, Win ARM64 5.9 min at 98.4 % hits, so the cache pays off. |
| pluginval finishes in ~1–5 s in CI. | Not a bug: it ends with SUCCESS. The plugin has no GUI tests (`--skip-gui-tests`) and no inputs, so strictness 10 is quick. |
| Code scanning upload returns 403 on this private repo. | No GitHub Advanced Security. Trivy results go to the log and a SARIF artifact; upload is behind `vars.CODE_SCANNING`. |

### Environment and tooling (this Mac)

- **Rosetta is not installed**, so the x86_64 slice of the universal binary can be built and checked with
  `lipo`, but not executed. Windows x64 CI is the first place Intel code actually runs.
- **CI results:** `gh` is installed and authenticated; use `gh run list` / `gh run view <id> --log-failed`.
- **pluginval**: Claude runs it itself (user rule). Download the official v1.0.4 release into the scratchpad.
- **Hooks** block writing `.gitignore` and any command that mentions `.env`. Propose `.gitignore`
  changes as text and let the user apply them.
- **Never read files under `build/`** (gitignored). Audition renders go to `$VS_RENDER_DIR` in the scratchpad.
