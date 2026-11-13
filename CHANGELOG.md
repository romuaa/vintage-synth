# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [Semantic Versioning](https://semver.org/). Nothing has been released yet.

## [Unreleased]

### Output stage: headroom and soft limiter (2026-11-13)
- A fixed −6.02 dB output trim: the loudest chord (Dual, 32 voices, full velocity, dry) now peaks
  at −2.57 dBFS at the default master volume (was +3.45 dBFS). Everything is 6 dB quieter; the
  relative levels of programs and effects are unchanged. Golden renders re-blessed (old × 0.5 exactly).
- A soft limiter on the output, on by default: stereo-linked, no added latency, transparent below
  its knee, never above its ceiling. Two parameters, `master.limiter` and
  `master.limiter.ceiling` (−12 … 0 dB, default −1 dB); session settings like master volume, not
  part of programs. 172 parameters in total.

### M6a — presets and banks, infrastructure (2026-11-13)
- Tones, programs and banks as text files; a read-only factory bank (8 placeholder programs for
  now); loading and saving from a small preset bar; MIDI Program Change; host program list;
  sessions remember the current program.
- Preset files and session data are treated as untrusted input: strict reader, bounded file
  access, no file access while a session loads.

### M5 — effects and arpeggiator/sequencer in the plugin (2026-11-12)
- Chorus, delay and reverb on the stereo bus (11 parameters); arpeggiator and 16-step sequencer in
  front of each part with a part selector, locked to the host tempo (58 parameters). 170 in total.
- Host bypass is a clean stop; non-finite parameter values are sanitised; integer parameters are
  stepped for hosts; the reported tail includes the synth's release.
- Hard sync with a sine VCO-1 no longer aliases (−109 dB or better, was −43 to −60 dB).
- Offline renders of Cross Mod patches now match real-time playback.

### Local-first verification (2026-11-12)
- `scripts/preflight.sh`: one local gate for everything a Mac can check, including the VST3
  validator built from the pinned SDK commit, the security scan and repository hygiene checks.
- CI no longer runs automatically; it is started by hand as a confirmation for Windows and Linux x86_64.

### Linux ARM64, distro load tests, local Linux check (2026-11-12)
- Linux ARM64 is a supported, CI-validated target; x86_64 and ARM64 ship in one Linux bundle.
- CI load-tests the Linux x86_64 build with pluginval in containers of Ubuntu 22.04 and 26.04,
  Debian 12 and 13, Fedora and Arch, plus a negative test below the glibc floor.
- `scripts/linux-check.sh`: GCC build and tests (or sanitizers) in a Linux container on a Mac.

### M3 — bi-timbral (2026-11-12)
- The product is now called **Exitium Vintage Synth** (name in hosts and bundle file name;
  plugin codes and identifiers unchanged).
- Upper part with Whole, Dual and Split key modes; a key router that can never hang a note when the
  split point or mode changes; part balance and per-part pan; true stereo output. 101 parameters.
- Oversampling follows the host rate (4× below 44.1 kHz, 2× up to 88.2 kHz, 1× above): 32 voices
  cost about 5 % of a core at 48 kHz and about 10 % at 192 kHz.
- Exact reported latency (6, 5 or 8 samples by configuration); accurate long envelopes and LFO
  timing at high sample rates.
- Known issue: hard sync combined with audio-rate cross-mod aliases above the −100 dB gate.

### M4 — arpeggiator and step sequencer module, not wired in yet (2026-11-12)
- Arpeggiator (Up, Down, Up & Down, Random, As Played; 1–4 octaves; hold; swing; gate length) and a
  16-step sequencer (offset, Note/Rest/Tie, velocity), locked sample-accurately to the host tempo
  or an internal clock. Standalone module in `source/common/seq/` with tests; connected in a later step.

### Linux target and public-release preparation (2026-11-12)
- Linux x86_64 VST3 is a supported target: built in CI on Ubuntu 22.04 (glibc 2.35 or newer, static
  C++ runtime) with the unit tests, host smoke test, render tests, VST3 validator and pluginval.
- A CI job runs the tests under AddressSanitizer and UBSan on every push.
- pluginval downloads are verified by SHA-256; CI artifacts are kept for 7 days.
- `SECURITY.md`, `CONTRIBUTING.md` (contributions under AGPLv3 only) and a pull request template.
- `scripts/dev-host.sh` returns to the terminal after launching the host.

### M2 — polyphony (2026-11-12)
- 16 voices per part with Poly, Unison (2/4/8/16 voices, detune) and Solo assign modes; voice
  stealing with a 3 ms fade; sustain pedal; portamento (Always/Legato); velocity to VCA and VCF;
  bend range 0–12 semitones; mod wheel vibrato. Nine new parameters per part (96 in total).
- MIDI is parsed once into note events; every event is applied exactly once; All Notes Off also
  clears the sustain pedal; malformed messages are dropped.
- Voice engine about 2.3× faster: 32 voices cost about 5 % of one core at 48 kHz (was 11.3 %).
- Renders are independent of the host block size, also at full Vintage and with reused voices.
- Tests: allocator unit tests, 39 black-box tests written from the specification, sanitizer-clean.
- Only the Lower part plays until M3.

### M5 — effect blocks, not wired in yet (2026-11-12)
- Chorus (two modes, mix up to wet = dry), stereo/ping-pong delay with tape-style time glide, and a
  16-line hall reverb, as standalone JUCE-free blocks in `source/common/fx/` with tests and `fx_bench`
  (all three: about 0.55 % of one core at 48 kHz). They are connected to the plugin in a later step.

### Developer tools (2026-11-12)
- `scripts/dev-host.sh` builds the plugin and JUCE's AudioPluginHost and opens the host with the
  synth already wired, for hands-on testing. Manual checklist in `docs/ui-test-checklist.md`.

### Wave 1 — test infrastructure and CI cost (2026-11-12)
- Golden-render regression harness (`tests/render_regression/`, ctest label `render`): text event
  scripts rendered through the real processor and compared by perceptual features, not samples.
  One self-test reference; preset goldens follow in M6.
- Steinberg VST3 validator runs in CI on macOS and Windows x64 (ctest `vst3_validator`), built from
  the SDK pinned at `v3.8.1_build_84`.
- CI cost control: docs-only changes don't build, draft pull requests build Windows x64 only,
  job timeouts, monthly Dependabot.

### Wave 0 — contracts and orchestration (2026-11-12)
- `docs/architecture.md`: contracts for M2–M5 (note events, voice allocation, new per-part
  parameters, CPU targets, effect block interface, arp/sequencer and test-infrastructure outlines).
- Development workflow: agents on feature branches, pull requests for CI, owner approval per
  milestone (skill `orchestrate`).
- Compiler cache verified on a warm CI run: macOS 3.5 min, Windows x64 6.7 min, Windows ARM64 5.9 min.

### CI and project infrastructure (2026-11-12)
- CI builds macOS universal, Windows x64 and Windows ARM64 on every push and pull request, runs the
  unit tests and the VST3 host smoke test on all three, and pluginval (strictness 10) on macOS and
  Windows x64. Windows x64 and ARM64 are merged into one `.vst3` bundle.
- All GitHub Actions updated to their latest releases and pinned by commit SHA; runner images
  `macos-26`, `windows-2025`, `windows-11-arm`, `ubuntu-26.04`. Read-only default token permissions.
- Windows CI no longer hardcodes a Visual Studio version (the runners dropped VS 2022).
- Faster CI: link-time optimisation only on the shipped plugin (tests use a non-LTO build of the same
  sources), Ninja everywhere, and a compiler cache (ccache on macOS, sccache on Windows).
- Dependabot: weekly updates for GitHub Actions and the JUCE/Catch2 submodules.
- Trivy repository scan (vulnerabilities, secrets, misconfigurations) on push, pull request and weekly.
- README, CHANGELOG, design decision log (`docs/decisions.md`) and lessons learned
  (`docs/engineering-notes.md`).

### M1 — single-part voice engine (2026-11-12)
- Two VCOs (sine, triangle, saw, square, pulse, noise) with windowed-sinc BLEP/BLAMP anti-aliasing
  (≤ −113 dB aliasing up to a 5 kHz fundamental), sub-sample hard sync, exponential cross-mod, and
  VCO-2 Lo Freq mode.
- Non-resonant HPF; 4-pole ZDF OTA-ladder VCF with −12/−24 dB slopes and self-oscillation; VCA with
  4-step LFO tremolo.
- Two RC-style ADSR envelopes with key follow; ENV-1 polarity. LFO with six waves and delayed fade-in.
- 2× oversampling in real time, 4× offline; latency reported to the host.
- All `lower.*` and `upper.*` parameters (78 in total with master volume and Vintage), generated
  from one list. Only the Lower part plays, monophonically, until M2/M3.
- Global Vintage amount: seeded per-voice pitch drift, tuning, cutoff and envelope tolerances.
- Sample-accurate, allocation-free MIDI handling; output NaN/Inf guard and clamp.
- Tests: DSP unit tests, part behaviour, VST3 host smoke test, CPU benchmark, audition render.

### M0 — project skeleton (2026-11-12)
- JUCE 9.0.5 (AGPLv3) and Catch2 v3.16.1 as pinned submodules; CMake + Ninja; C++20.
- Plugin identity: Exitium, `com.exitium.vintagesynth`, manufacturer `Exit`, plugin `Vsy1`. VST3 only.
- Project rules (`CLAUDE.md`) and skills for build, validation, parameters, real-time review and release.
