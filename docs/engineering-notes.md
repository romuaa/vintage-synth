# Engineering notes — lessons learned

Things that went wrong or surprised us, the fix, and the rule that follows from each.
`docs/decisions.md` records *what* we chose; this file records the *traps*. Newest milestone first.

## M1 (2026-11-12)

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

### Environment and tooling (this Mac)

- **Rosetta is not installed**, so the x86_64 slice of the universal binary can be built and checked with
  `lipo`, but not executed. Windows x64 CI is the first place Intel code actually runs.
- **CI results:** `gh` is installed and authenticated; use `gh run list` / `gh run view <id> --log-failed`.
- **pluginval**: Claude runs it itself (user rule). Download the official v1.0.4 release into the scratchpad.
- **Hooks** block writing `.gitignore` and any command that mentions `.env`. Propose `.gitignore`
  changes as text and let the user apply them.
- **Never read files under `build/`** (gitignored). Audition renders go to `$VS_RENDER_DIR` in the scratchpad.
