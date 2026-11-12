# Vintage Synth

A bi-timbral, analog-style polyphonic synthesizer instrument in VST3 format, by **Exitium**.
It's a two-oscillator subtractive synth in the style of the classic 80s flagship polys, with our own
naming and design.

> **Status: in development (pre-release).** Milestone M1 is done: one part (Lower) plays as a mono
> synth with the complete voice. Polyphony, Upper part, arpeggiator, sequencer, effects, presets and
> the custom UI come in later milestones (see [Roadmap](#roadmap)). The plugin currently shows JUCE's
> generic parameter editor.

## Features

Working now (M1):

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
- **Sound quality:** anti-aliased oscillators (aliasing ≤ −113 dB up to a 5 kHz fundamental),
  2× oversampling in real time and 4× when rendering offline. Latency (about 7 samples) is reported to the host.

Planned: 16 voices per part, Poly/Unison/Solo, portamento, Whole/Dual/Split, arpeggiator, 16-step sequencer,
patch banks, and chorus, delay and reverb.

## Platforms

| OS | Architectures | Format |
|---|---|---|
| macOS 11+ | Universal (Apple silicon + Intel) | VST3 |
| Windows 10/11 | x64 and ARM64 in one bundle | VST3 |

## Building

Requirements: CMake ≥ 3.28, a C++20 compiler (Xcode on macOS, Visual Studio 2022 or newer on Windows), and
Ninja on macOS (`brew install cmake ninja`).

```bash
git clone --recurse-submodules git@github.com:romuaa/vintage-synth.git
cd vintage-synth
# (already cloned? run: git submodule update --init --recursive)
```

**macOS (universal binary):**

```bash
cmake -S . -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/mac --parallel
```

The plugin is at `build/mac/VintageSynth_artefacts/Release/VST3/Vintage Synth.vst3`. Copy it to
`~/Library/Audio/Plug-Ins/VST3/` to use it in a DAW.

**Windows:**

```bat
cmake -S . -B build/win-x64 -A x64
cmake --build build/win-x64 --config Release --parallel
```

Use `-A ARM64` for Windows on ARM. Copy the `.vst3` to `C:\Program Files\Common Files\VST3\`.
CI builds both and merges them into one bundle.

## Testing

```bash
ctest --test-dir build/mac --output-on-failure        # unit tests + host smoke test
build/mac/tests/voice_bench                           # CPU cost per voice
VS_RENDER_DIR=/some/dir build/mac/tests/VintageSynthTests "[.render]"   # audition WAV + spectrogram
```

- **Unit tests** (Catch2) cover oscillator aliasing and pitch, filter cutoff, slope and self-oscillation,
  envelope and LFO timing, sample-accurate MIDI, and determinism.
- **host_smoke** loads the built `.vst3` the way a DAW does and checks identity, parameters, rendering at
  44.1/48/96 kHz and state save/restore.
- **pluginval** at strictness 10 runs in CI on macOS and Windows x64:
  `pluginval --strictness-level 10 --validate-in-process --skip-gui-tests "<path>/Vintage Synth.vst3"`.

## Project layout

```
source/dsp/        DSP building blocks (oscillator, filters, envelope, LFO, drift); no JUCE dependency
source/engine/     voice, part (note handling, LFO, oversampling)
source/params/     parameter list, value mappings, parameter layout
source/            plugin processor
tests/             unit tests, CPU benchmark, VST3 host smoke test
docs/              design decisions and engineering notes
external/          JUCE and Catch2 (git submodules, pinned)
```

Design choices and their reasons are in [docs/decisions.md](docs/decisions.md); lessons learned are in
[docs/engineering-notes.md](docs/engineering-notes.md).

## Roadmap

| Milestone | Scope | Status |
|---|---|---|
| M0 | Project skeleton, CI, build for all targets | Done |
| M1 | Single-part voice engine | Done |
| M2 | Polyphony: 16 voices, Poly/Unison/Solo, portamento, voice stealing, velocity, bend range | Next |
| M3 | Bi-timbral: Upper part, Whole/Dual/Split | |
| M4 | Arpeggiator and 16-step sequencer | |
| M5 | Chorus, delay and reverb | |
| M6 | Presets, patch banks, golden-render regression tests | |
| M7 | Custom UI | |
| M8 | Hardening, signing, installers, release | |

## License

Copyright © Exitium. Licensed under the **GNU Affero General Public License v3.0**; see [LICENSE](LICENSE).
Built with [JUCE](https://juce.com) (AGPLv3). Every binary release comes with its complete source code.

VST is a registered trademark of Steinberg Media Technologies GmbH.
