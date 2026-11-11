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
cmake -S . -B build/win-x64   -G "Visual Studio 17 2022" -A x64
cmake -S . -B build/win-arm64 -G "Visual Studio 17 2022" -A ARM64
cmake --build build/win-x64   --config Release --parallel
cmake --build build/win-arm64 --config Release --parallel
```

The release job merges both builds into one bundle: `Contents/x86_64-win/` and `Contents/arm64-win/`.
Check the machine type with `dumpbin /headers <file>.vst3 | findstr machine` (x64 = 8664, ARM64 = AA64).

## Rules

- Never commit `build/`. Dependency versions are pinned in CMake; never float to `main`.
- An x86-only intrinsic or a missing NEON path is a build bug, even if the mac build passes.
- Windows can't be built locally on this Mac. Say that it was verified "via CI only".
