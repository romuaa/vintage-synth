# Exitium Vintage Synth

A bi-timbral, analog-style polyphonic synthesizer instrument in VST3 format, by **Exitium**.
It's a two-oscillator subtractive synth in the style of the classic 80s flagship polys, with our own
naming and design.

> **Status: in development (pre-release).** Milestone M3 is done: two 16-voice parts (Lower and
> Upper) with Whole, Dual and Split modes and stereo output. Arpeggiator, sequencer and effects in
> the plugin, presets and the custom UI come in later milestones (see [Roadmap](#roadmap)). The
> plugin shows its own editor (M7a: frame, Sound page, live automation; the other pages are
> placeholders listing their parameters until M7b–M7e).

## Features

Working now (M3):

- **Two parts:** Lower and Upper, each a complete 16-voice synth. Whole (one sound), Dual (both
  layered, up to 32 voices) or Split (bass below, lead above, movable split point), with balance,
  pan per part and stereo output.

- **16 voices** with Poly, Unison (2–16 voices with detune) and Solo modes, click-free voice stealing,
  sustain pedal, portamento (always or legato), velocity to level and filter, bend range up to an
  octave, mod wheel vibrato. 32 voices use about 5 % of one CPU core at 48 kHz.

- **Two VCOs:** sine, triangle, saw, square, pulse and noise on both. VCO-1 range 32'–1', VCO-2 16'–2'.
  Hard sync (VCO-1 to VCO-2) and exponential cross-mod at audio rate. VCO-2 Lo Freq mode turns it into a
  second, sync-capable LFO. PWM from LFO, ENV-1 or manual. Pitch modulation from LFO and ENV-1.
- **Filters:** non-resonant high-pass, then a 4-pole resonant low-pass (−24 or −12 dB/oct) that
  self-oscillates. Cutoff modulation from ENV-1 or ENV-2, LFO, and keyboard follow.
- **VCA** with level and four-step LFO tremolo depth.
- **Two ADSR envelopes** with analog-style curves and keyboard-follow for times. ENV-1 can be inverted.
- **LFO:** sine, triangle, saw, square, random (sample and hold) and noise, with delayed fade-in.
- **Vintage amount:** 0 is exactly clean; turning it up adds per-voice tuning offsets, slow pitch drift,
  and filter and envelope tolerances. Deterministic, so renders repeat exactly.
- **Sound quality:** anti-aliased oscillators (measured aliasing ≤ −113 dB up to a 5 kHz fundamental),
  2× oversampling in real time and 4× when rendering offline. Latency (about 7 samples) is reported to the host.
- **Output:** 6 dB of fixed headroom and a soft limiter (on by default, ceiling −12 … 0 dB, no added
  latency, never above the current (smoothed) ceiling) so that heavy chords and effect settings do
  not clip.

Planned: arpeggiator and 16-step sequencer, chorus, delay and reverb (these modules exist and are
tested, but are not connected to the plugin yet), and patch banks.

## Platforms

| OS | Architectures | Format |
|---|---|---|
| macOS 11+ | Universal (Apple silicon + Intel) | VST3 |
| Windows 10/11 | x64 and ARM64 in one bundle | VST3 |
| Linux | x86_64 and ARM64 in one bundle (glibc 2.35 or newer) | VST3 |

There are no binary releases yet: build from source. Until v1.0 the parameter set may still change.

## Building

Requirements: CMake ≥ 3.28, a C++20 compiler (Xcode on macOS, Visual Studio 2022 or newer on Windows), and
Ninja on macOS (`brew install cmake ninja`).

```bash
git clone --recurse-submodules https://github.com/romuaa/vintage-synth.git
cd vintage-synth
# (already cloned? run: git submodule update --init --recursive)
```

**macOS (universal binary):**

```bash
cmake -S . -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/mac --parallel
```

The plugin is at `build/mac/VintageSynth_artefacts/Release/VST3/Exitium Vintage Synth.vst3`. Copy it to
`~/Library/Audio/Plug-Ins/VST3/` to use it in a DAW.

**Two plugins.** The same build also produces **Exitium B-Side**, the second instrument (a chip synth, in
development: today a shell around test tones, see the [Roadmap](#roadmap)), at
`build/mac/BSide_artefacts/Release/VST3/Exitium B-Side.vst3`. `-DVS_BUILD_BSIDE=OFF` builds only
Exitium Vintage Synth and its tests, exactly as before B-Side existed. Build one plugin with
`cmake --build build/mac --target VintageSynth_VST3` or `--target BSide_VST3`.

**Windows:**

```bat
cmake -S . -B build/win-x64 -A x64
cmake --build build/win-x64 --config Release --parallel
```

Use `-A ARM64` for Windows on ARM. Run the tests with `ctest --test-dir build/win-x64 -C Release`.
Copy the `.vst3` to `C:\Program Files\Common Files\VST3\`. CI builds both and merges them into one bundle.

**Linux (x86_64):**

```bash
sudo apt install build-essential cmake ninja-build libasound2-dev libjack-jackd2-dev ladspa-sdk \
  libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
  libxinerama-dev libxrandr-dev libxrender-dev libxi-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
mkdir -p ~/.vst3 && cp -r "build/VintageSynth_artefacts/Release/VST3/Exitium Vintage Synth.vst3" ~/.vst3/
```

The Linux build links the C++ runtime statically; at run time it needs glibc 2.35 or newer,
libfontconfig and libfreetype. CI builds it on Ubuntu 22.04. So far Linux is validated by CI only
(tests, pluginval on x86_64, the VST3 validator); the x86_64 build is also load-tested in containers
of several distributions (Ubuntu, Debian, Fedora, Arch). ARM64 is CI-validated and not
performance-tuned for small boards. A check in a DAW on a real machine is pending.

Developers on a Mac with Docker can run `scripts/linux-check.sh` before pushing: it builds and tests
with GCC in a native Linux ARM64 container (`--sanitize` for the AddressSanitizer/UBSan run).

## Try it in a plugin host

`scripts/dev-host.sh` (macOS) builds the plugin and JUCE's AudioPluginHost (about 2 minutes the first
time on Apple silicon) and opens the host with MIDI Input → Exitium Vintage Synth → Audio Output already wired.
`scripts/dev-host.sh --plugin bside` does the same for Exitium B-Side (`--plugin vintage` is the default).

- Open the editor: double-click the plugin's node ("Exitium Vintage Synth" or "Exitium B-Side").
- Play without a MIDI keyboard: the on-screen piano at the bottom of the host window (mouse, or the
  computer keys A W S E D F T G Y H U J K; Z and X shift the octave). A hardware keyboard also works.
- Audio device and buffer size: Options > Audio Settings. macOS may ask for microphone permission
  because the host opens the default input; denying is fine.
- Flags: `--build-only`, `--no-build`, `--debug`, `--wait` (stay attached to the host), and `--install` (also copies the plugin to
  `~/Library/Audio/Plug-Ins/VST3/`; the only option that writes outside the repository).
- The host keeps its settings in `~/Library/Preferences/Juce Audio Plugin Host.settings`.

What to check by hand: [docs/ui-test-checklist.md](docs/ui-test-checklist.md).

## Before you push

Run `scripts/preflight.sh --quick` while working and `scripts/preflight.sh` before you push (needs
CMake, Ninja and Docker; the first run downloads pluginval, the VST3 validator sources and a scanner
image into `~/Library/Caches/vs-preflight/`). It builds and tests both macOS universal plugins, runs
pluginval and the VST3 validator on each bundle, the sanitizers, the Linux ARM64 container build, a
security scan and repository checks (`VS_PREFLIGHT_PLUGINS=vintage` or `bside` limits pluginval and the
validator to one plugin while iterating; the hand-over run always covers both). `scripts/preflight.sh --clean` removes what it cached. Windows and Linux
x86_64 are confirmed by a manually started CI run.

## Testing

```bash
ctest --test-dir build/mac --output-on-failure        # unit tests + host smoke test
build/mac/tests/voice_bench                           # CPU cost per voice
VS_RENDER_DIR=/some/dir build/mac/tests/VintageSynthTests "[.render]"   # audition WAV + spectrogram
```

- **Unit tests** (Catch2) cover oscillator aliasing and pitch, filter cutoff, slope and self-oscillation,
  envelope and LFO timing, sample-accurate MIDI, and determinism.
- **host_smoke** loads the built `.vst3` the way a DAW does and checks identity, parameters, rendering at
  44.1/48/96 kHz and state save/restore (`host_smoke --profile vintage|bside <bundle>`; ctest runs
  `host_smoke` and `host_smoke_bside`).
- **BSideTests** (`tests/bside/`) test Exitium B-Side's shell: the skeleton contract, parameters,
  programs, state, bypass, the guard, the arpeggiator and no allocation in `processBlock`.
- **bside_trace** (ctest label `trace`) checks the committed chip reference data
  (`tests/bside/reference/`: register traces with the reference emulator's digital state and audio
  features, `docs/bside-oracle-adapter.md`); the replay against our chip reports itself skipped
  until the chip exists.
- **pluginval** at strictness 10 runs in CI on macOS and Windows x64:
  `pluginval --strictness-level 10 --validate-in-process --skip-gui-tests "<path>/Exitium Vintage Synth.vst3"`.

## CI and security

- `build` workflow: macOS universal, Windows x64, Windows ARM64 and Linux x86_64 builds with unit
  tests and the VST3 host smoke test; pluginval and the VST3 validator on macOS, Windows x64 and
  Linux; merged Windows bundle; a Linux job that runs the tests under AddressSanitizer and UBSan. Ninja and a compiler
  cache keep it fast.
- `security` workflow: [Trivy](https://trivy.dev) scans the repository for vulnerable dependencies,
  committed secrets and misconfigurations on every push, pull request and weekly.
- Dependabot keeps GitHub Actions (pinned by commit SHA) and the JUCE and Catch2 submodules up to date.

## Project layout

```
source/dsp/        DSP building blocks (oscillator, filters, envelope, LFO, drift); no JUCE dependency
source/engine/     voice, part (note handling, LFO, oversampling)
source/params/     parameter list, value mappings, parameter layout
source/            plugin processor
source/common/     shared by both instruments (dsp, engine, fx, seq, params, presets, shell)
source/bside/      Exitium B-Side: processor, test-tone part, parameters, presets
tests/             unit tests, CPU benchmark, VST3 host smoke test; tests/bside/ for B-Side
docs/              design decisions and engineering notes
external/          JUCE and Catch2 (git submodules, pinned)
```

Design choices and their reasons are in [docs/decisions.md](docs/decisions.md); lessons learned are in
[docs/engineering-notes.md](docs/engineering-notes.md); the history of changes is in [CHANGELOG.md](CHANGELOG.md).

## Roadmap

| Milestone | Scope | Status |
|---|---|---|
| M0 | Project skeleton, CI, build for all targets | Done |
| M1 | Single-part voice engine | Done |
| M2 | Polyphony: 16 voices, Poly/Unison/Solo, portamento, voice stealing, velocity, bend range | Done |
| M3 | Bi-timbral: Upper part, Whole/Dual/Split | Done |
| M4 | Arpeggiator and 16-step sequencer | Module done, not connected |
| M5 | Chorus, delay and reverb | Blocks done, not connected |
| M6 | Presets, patch banks, golden-render regression tests | |
| M7 | Custom UI | M7a done (frame, Sound page, live automation); M7b–M7f next |
| M8 | Hardening, signing, installers, release | |

A second instrument is planned in this repository: **B-Side**, a polyphonic synth modelled on a classic 3-voice
sound chip from early-1980s home computers, as its own plugin sharing the common engine code. Step S1a
is done: the instrument-independent processor (MIDI, key modes, arpeggiator/sequencer, effects,
output stage, bypass, presets, state) is a shared skeleton in `source/common/shell/`. Step S1b is done:
**Exitium B-Side** exists as a second VST3 plugin (`source/bside/`, built unless `-DVS_BUILD_BSIDE=OFF`)
with the shared parameters, 64 programs, state, the arpeggiator and the effects around a placeholder
test-tone part. It makes no chip sound yet; the chip model follows in S2 to S4. The first reference
data for it (digital state at chosen cycles and audio features of 76 synthetic register traces,
generated with a reference emulator outside this repository) is committed in `tests/bside/reference/`.

## License

Copyright © Exitium. Licensed under the **GNU Affero General Public License v3.0**; see [LICENSE](LICENSE).
Built with [JUCE](https://juce.com) (AGPLv3). Every binary release comes with its complete source code.

Third-party components: [JUCE](https://juce.com) (AGPLv3), Catch2 (BSL-1.0, tests only), the
Steinberg VST3 SDK as bundled with JUCE (MIT), and the Inter typeface embedded in the editor, which
keeps its own licence (SIL Open Font License 1.1; `resources/fonts/inter/OFL.txt`, source and
checksums in `resources/fonts/inter/SOURCE.txt`). CI uses pluginval (GPLv3), the VST3 SDK validator (MIT),
Trivy (Apache-2.0), ccache and sccache as separate tools; none of them is linked into or shipped
with the plugin.

Contributions are welcome under the same licence: see [CONTRIBUTING.md](CONTRIBUTING.md).
To report a security problem, see [SECURITY.md](SECURITY.md).

VST is a registered trademark of Steinberg Media Technologies GmbH.
