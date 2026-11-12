# Contributing

Thanks for your interest in Vintage Synth. Bug reports, fixes and ideas are welcome.

**Licence:** by submitting a contribution you agree that it is licensed under the GNU Affero
General Public License v3.0 only, the same as the project (inbound = outbound); there is no
contributor agreement and no copyright assignment.

## Building and testing

The README has the full instructions ([Building](README.md#building) and [Testing](README.md#testing)).
The short version on macOS:

```bash
git clone --recurse-submodules https://github.com/romuaa/vintage-synth.git
cd vintage-synth
cmake -S . -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/mac --parallel
ctest --test-dir build/mac --output-on-failure
```

On Windows use the commands in the README. You do not need every platform: say in the pull request
what you built and tested, and CI covers the rest.

## Rules that are not negotiable

- **Real-time audio thread.** Anything reachable from `processBlock` must not allocate, lock, log,
  do file I/O, throw, or make blocking calls. Smooth continuous parameters; never smooth switches.
- **No third-party brand names, logos or copied panel artwork** in code, UI, presets or docs.
  The look and naming are our own. Do not copy code from other synths either: published
  algorithms are fine with a citation, and anything copied needs an AGPLv3-compatible licence.
- **Parameter IDs are stable.** They are dotted (`lower.vcf.cutoff`). Once 1.0 ships they are
  frozen: only append new ones, never rename, remove or reorder. Saved state must keep loading.
- **Warnings are errors on all targets** (macOS universal, Windows x64, Windows ARM64). No x86-only
  intrinsics without a NEON path and runtime dispatch.
- **DSP changes come with tests.** Keep the oscillator alias gate and the render-regression tests
  passing, and do not loosen a threshold to make a test pass without explaining why.
- New source files go into `VS_PLUGIN_SOURCES` in `CMakeLists.txt`.

## Pull requests

- Keep each pull request focused and say what and why; the template has a short checklist.
- Open it as a draft while you iterate. Draft pull requests build Windows x64 only; marking it
  ready runs the full matrix (macOS universal, Windows x64, Windows ARM64). A security scan runs
  on every pull request. Docs-only changes do not trigger a build.
- A maintainer may need to approve the workflow run for a first-time contributor.
- Dependency upgrades (JUCE, Catch2) are deliberate and happen in their own commit.

## How the project is developed

Vintage Synth is built by a human owner directing AI coding agents. Agents write and review much
of the code against written contracts in `docs/architecture.md`; the owner makes the product
decisions. Judgement calls and their reasons are logged in `docs/decisions.md`, and lessons learned
in `docs/engineering-notes.md`. If your change makes a new judgement call, add a line there.
Human contributors follow the same rules and face the same checks as the agents.
