---
name: build
description: Configure and build the plugin for macOS universal (arm64+x86_64) and Windows x64/ARM64, and verify binary architectures. Use for any build, CMake, or "does it compile" task.
---

# Build

Prereqs (macOS): Xcode + `brew install cmake ninja`. If `cmake` is missing, ask the user to run
`! brew install cmake ninja` under "What I need from you". Don't work around it.

Dependencies are git submodules. After a fresh clone, run `git submodule update --init --recursive`.

## macOS universal (local)

```bash
cmake -S . -B build/mac -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build build/mac --parallel
```

Verify both slices are present, for each format that was built:

```bash
lipo -archs "build/mac/VintageSynth_artefacts/Release/VST3/Vintage Synth.vst3/Contents/MacOS/Vintage Synth"
# expect: x86_64 arm64
```

Use `-DCMAKE_BUILD_TYPE=Debug` for debugging. Use a separate build dir (`build/mac-debug`).

## Windows (CI, or a Windows machine)

```bat
cmake -S . -B build/win-x64   -A x64
cmake -S . -B build/win-arm64 -A ARM64
cmake --build build/win-x64   --config Release --parallel
cmake --build build/win-arm64 --config Release --parallel
```

The release job merges both builds into one bundle: `Contents/x86_64-win/` and `Contents/arm64-win/`.
Check the machine type with `dumpbin /headers <file>.vst3 | findstr machine` (x64 = 8664, ARM64 = AA64).

This Mac has no Rosetta, so the x86_64 slice can only be checked with `lipo`, not run. Say so.

## CMake traps (JUCE 9.0.5)

- Test/benchmark targets compile plugin sources: give them
  `$<TARGET_PROPERTY:VintageSynth,INCLUDE_DIRECTORIES>` and `…,COMPILE_DEFINITIONS>`.
- `JUCE_PLUGIN_ARTEFACT_FILE` contains genexes; pass it as `$<GENEX_EVAL:$<TARGET_PROPERTY:VintageSynth_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>`.
- Don't define `JUCE_DISPLAY_SPLASH_SCREEN` (JUCE 9 warns → error).
- Use `AudioFormatWriterOptions` with `createWriterFor`; the old overload is deprecated (→ error).
- Warnings are errors everywhere, tests included. MSVC warnings only show up in CI.

## Rules

- Never commit `build/`. Dependencies are submodules pinned to release tags; never float to `main`. Actions are pinned by SHA; Dependabot proposes updates.
- An x86-only intrinsic or a missing NEON path is a build bug, even if the mac build passes.
- Windows can't be built locally on this Mac. Say that it was verified "via CI only".
- CI runs on push to `main` at GitHub `romuaa/vintage-synth` (private). Check it with `gh run list` and
  `gh run view <id> --log-failed`; report Windows as "pending CI" until a run passes.
- Don't pass `-G "Visual Studio NN YYYY"`: runner images change VS versions. Use only `-A x64|ARM64`
  so CMake picks the newest installed Visual Studio.
