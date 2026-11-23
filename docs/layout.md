# Repository layout

What lives where. Vintage (`source/vintage/`) and B-Side (`source/bside/`) never include each
other; `source/common/` includes neither.

```
source/common/    shared by all instruments; must not include anything from source/vintage/ or source/bside/
  dsp/            FastMath, DspUtil, BlepTable (JUCE-free)
  engine/         NoteEvent, EventParser, VoiceAllocator, KeyRouter, PartMixer, UiFeed (audio → editor), UiNotes (editor → audio notes)
                  (JUCE-free), Oversampler
  fx/             Chorus, Delay, Reverb, SoftLimiter (JUCE-free)
  seq/            arpeggiator and step sequencer (JUCE-free)
  ui/             editor library for both instruments: models/ (JUCE-free interaction models),
                  ParamMirror (polls parameters per frame), controls/, graphs/, frame/, Theme,
                  HelpTexts, ControlRegistry
  params/         parameter factories, shared globals (GlobalParams: master volume, key/mix, limiter),
                  effect and arp/seq parameters + bindings, shared Mappings.h
  presets/        preset model, strict JSON, files, manager, factory bank, preset bar
  shell/          BiTimbralProcessor (CRTP skeleton: MIDI, key modes, arp/seq, mixer, effects, output
                  stage, bypass, programs, state), InstrumentEditor (preset bar + generic editor),
                  EditorState (JUCE-free; saved as the `UiState` child of a session)
source/vintage/   this instrument: PluginProcessor.* (VintageSynthProcessor : shell::BiTimbralProcessor),
                  ui/ (VintageEditor, PageList and one file pair per page: Sound, Envelopes, Lfo, Play,
                  ArpSeq, Effects, Browser)
  dsp/            Oscillator, Filters, Envelope, Lfo, Drift (JUCE-free)
  engine/         Voice, Part, Modulation (shared per-part modulation), OscPath (the oscillator
                  path shared by Voice and the editor's oscilloscope)
  params/         PartParamList.h (X-macro list of all per-part params), PartMappings.h,
                  ParameterLayout (assembles the layout), StateVersion.h
source/bside/     Exitium B-Side (second plugin): PluginProcessor.*, engine/TestTonePart (placeholder until
                  S2/S4), params/, presets/ (JUCE-bound; BSideTables = the table validator and sink);
                  program/ProgramHandover.h (table to the audio thread); text/ program/ trace/ are JUCE-free (strict
                  readers, patch tables, trace formats); chip/ is the chip model (digital core, combined-waveform tables; JUCE-free);
                  engine/ProgramEngine is the patch program engine (JUCE-free). Vintage and B-Side never
                  include each other
tests/bside/reference/  generated reference data (traces, digital references, audio features);
                  regenerated only with the adapter outside the repository, reviewed like a golden
tests/bside/shapes/  combined-waveform measurements (own MANIFEST); tests/bside/golden/ program-engine
                  write traces (tools/ derives them); tests/bside/tools/ShapeFit.cpp = bside_shapefit;
                  tests/bside/tools/gen_*_traces.py write the analog gate traces (a_s3a_*, a_cutcode_*, a_cutsweep_a);
                  tests/bside/trace/ the bside_trace gates (AnalogChain.h our chain, AnalogGates.h, SweepSteps.h,
                  CutSweepReference.h: the reference's sweep steps as constants)
tests/bside/      bside_unit (JUCE-free, ctest label "bside") always; bside_independent (the independent
                  model against the chip with placeholder shapes; never link it with bs_juce_free); with VS_BUILD_BSIDE also
                  bs_test_core + BSideTests. `bside_unit "[.fuzz-long]"` is the hidden 10^6-case fuzz run
presets/bside/factory/  B-Side factory programs (JSON, embedded)
scripts/sanitize-targets.sh  the one list of sanitizer test targets (preflight, Linux container, CI)
resources/fonts/inter/  the embedded UI font with its licence (OFL.txt) and source record
docs/ui/         wireframes and help-texts.md (the single source of the editor's help texts)
tests/ui/        editor tests (part of VintageSynthTests); hidden "[.uishots]" and "[.uibench]"
tests/dsp_unit/  Catch2 tests; RenderDemo.cpp is a hidden "[.render]" audition render
tests/benchmarks/ voice_bench (CPU cost per voice)
tests/host_smoke/ loads the built .vst3 through JUCE's VST3 host (ctest "host_smoke"; runs on every CI target)
tests/render_regression/ golden-render harness (ctest label "render"; scripts/ and golden/ feature references)
docs/architecture.md  contracts between work streams (interfaces, behaviour, gates)
docs/decisions.md  design decision log (what we chose and why)
docs/ui-test-checklist.md  manual checks in the plugin host
scripts/dev-host.sh  builds plugin + JUCE AudioPluginHost and opens the synth in it (-DVS_BUILD_DEV_HOST)
scripts/preflight.sh  the local gate: every check this Mac can run (see "Verification is local-first")
scripts/linux-check.sh  GCC build + tests (or sanitizers) in a Linux ARM64 container (a preflight step)
scripts/package-release.sh  packs and verifies the release zips (contract 10); texts in resources/release/
docs/engineering-notes.md  lessons learned (traps, causes, rules); read before starting a milestone
README.md        public overview, build and test instructions
CHANGELOG.md     what was done, per milestone (update it with every milestone/infra change)
.github/         workflows/build.yml (build+test+pluginval), workflows/security.yml (Trivy), dependabot.yml
external/        JUCE, Catch2 (submodules)
```

**Adding a per-part parameter:** add one line to `VS_PART_FLOAT_PARAMS` or `VS_PART_CHOICE_PARAMS`
in `PartParamList.h` (append at the end). The layout, binding and smoothing follow automatically.
Put any physical mapping in `Mappings.h` so the display text and the DSP agree.

