## What and why

<!-- One or two sentences: what changes, and the reason. Link an issue if there is one. -->

## Checklist

- [ ] Builds without warnings (warnings are errors); below I say what I could not build or run
- [ ] Tests pass (`ctest --test-dir build --output-on-failure`); new DSP behaviour has a test
- [ ] Real-time safety considered: nothing reachable from `processBlock` allocates, locks, logs or blocks
- [ ] Parameter IDs unchanged (new ones only appended, with the mapping in `Mappings.h`)
- [ ] No third-party brand or product names, logos or copied artwork in code, UI, presets or docs
- [ ] Docs updated where relevant (`CHANGELOG.md`, `README.md`, `docs/decisions.md` for judgement calls)
- [ ] I am contributing this under AGPLv3, and it is my own work (or compatibly licensed and credited)

## Tested on / not tested on

<!-- e.g. "macOS arm64 locally; Windows left to CI" -->
