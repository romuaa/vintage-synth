---
name: add-parameter
description: Checklist for adding or changing a host-automatable parameter, preset field, or saved plugin state without breaking existing sessions and presets.
---

**Where things live after S0:** per-part parameters in `source/vintage/params/PartParamList.h`; this
instrument's globals in `source/vintage/params/ParameterLayout.cpp`; effect and arp/seq parameters in
`source/common/params/FxArpParams.cpp`; mappings in `source/common/params/Mappings.h` (shared) or
`source/vintage/params/PartMappings.h` (voice).

**B-Side (`source/bside/params/`):** the per-part parameters are one X-macro list, `PartParamList.h`
(`BS_PART_PARAMS (F, C, I)`: float, choice and stepped-integer entries in the contract's order, 8.7);
`lower.*` and `upper.*` are both generated from it. Mappings (value to unit, shared by display text and
DSP) are in `PartMappings.h`; the layout, the display text and `PartParamBinding` (cached pointers,
`read (PartParams&)`) are in `ParameterLayout.*`; globals are appended in `createParameterLayout()`.
Append only (the contract fixes the order), then update `kPartParamCount` / `kParameterCount`,
`tests/bside/integration/ParamTests.cpp` (its table is written by hand), the counts in `ShellTests.cpp`
and `tests/host_smoke/HostSmoke.cpp` (`kBSide`). B-Side smooths its continuous parameters in the part
(`Part::followParams`, `source/bside/engine/Part.cpp`): the 17 listed in its `goals` array move over 5 ms at the 1 ms control
points, and `mix.level` has its own per-sample 5 ms linear ramp on the part's sum (target latched at the block start). A new
continuous B-Side parameter is smoothed only if added there.

**Smoothing is not automatic for new continuous parameters.** Appending to the X-macro creates the
parameter and its binding, but `PartModulator::next` (`vintage/engine/Modulation.cpp`) only smooths the
parameters it lists. Add a new continuous parameter there (and to `ModBlock::Ctl` if voices need it
per sample), or it is applied unsmoothed at block rate.

# Add or change a parameter

Host sessions store parameters by ID. Breaking IDs silently breaks users' songs.

## Checklist

1. **ID:** dotted, lowercase, stable, e.g. `lower.vco2.fine_tune`. Per-part params exist for both
   `lower.` and `upper.`, generated from one definition so they can't diverge: add one line at the end
   of `VS_PART_FLOAT_PARAMS` or `VS_PART_CHOICE_PARAMS` in `source/vintage/params/PartParamList.h`. Global
   params go in `createParameterLayout()` (`source/vintage/params/ParameterLayout.cpp`).
   Put the value → unit mapping in `source/vintage/params/PartMappings.h` (shared ones in
   `source/common/params/Mappings.h`) so the display text and DSP agree.
   Update the expected count in `tests/host_smoke/HostSmoke.cpp` (172 for Vintage, 204 for B-Side).
2. **Append only:** add the new parameter at the end of the layout. After v1.0, never rename,
   remove, reorder, or change the range or type of an existing ID. Instead, add a new ID and
   migrate to it.
3. **Definition:** range, skew, default, unit, value-to-text and text-to-value.
   Switches are `AudioParameterChoice` with readable labels ("-24 dB", "ENV-2").
4. **DSP:** read the value through a cached atomic pointer. Smooth continuous params; switches aren't smoothed.
5. **State:** before v1.0, `stateVersion` stays 1 (0.9.0 is a beta whose state format may still change; the format freezes at 1.0.0); skip the rest of this step.
   From v1.0 on, bump `stateVersion` in `source/vintage/params/StateVersion.h` and add a migration step
   (old state → default or derived value). Add a test that loads a stored old-version state file
   (one old-state fixture exists, `tests/fixtures/state_m5.bin`; add one for each bump).
6. **Presets:** give every factory preset a sensible value. The preset loader must accept files
   that lack the field.
7. **UI and MIDI:** attach the parameter to a control. Add a default CC only if the CC map has room,
   and document it in the README.
8. Run the `validate-plugin` skill (automation round-trip and golden renders).

If an existing ID really must change meaning, stop and ask the user under "What I need from you".
