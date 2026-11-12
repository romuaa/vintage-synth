---
name: add-parameter
description: Checklist for adding or changing a host-automatable parameter, preset field, or saved plugin state without breaking existing sessions and presets.
---

# Add or change a parameter

Host sessions store parameters by ID. Breaking IDs silently breaks users' songs.

## Checklist

1. **ID:** dotted, lowercase, stable, e.g. `lower.vco2.fine_tune`. Per-part params exist for both
   `lower.` and `upper.`, generated from one definition so they can't diverge: add one line at the end
   of `VS_PART_FLOAT_PARAMS` or `VS_PART_CHOICE_PARAMS` in `source/params/PartParamList.h`. Global
   params go in `createParameterLayout()` (`source/params/ParameterLayout.cpp`).
   Put the value → unit mapping in `source/params/Mappings.h` so the display text and DSP agree.
   Update the expected count in `tests/host_smoke/HostSmoke.cpp` (currently 78).
2. **Append only:** add the new parameter at the end of the layout. After v1.0, never rename,
   remove, reorder, or change the range or type of an existing ID. Instead, add a new ID and
   migrate to it.
3. **Definition:** range, skew, default, unit, value-to-text and text-to-value.
   Switches are `AudioParameterChoice` with readable labels ("-24 dB", "ENV-2").
4. **DSP:** read the value through a cached atomic pointer. Smooth continuous params; switches aren't smoothed.
5. **State:** before v1.0, `stateVersion` stays 1 (no released states); skip the rest of this step.
   From v1.0 on, bump `stateVersion` in `source/params/StateVersion.h` and add a migration step
   (old state → default or derived value). Add a test that loads a stored old-version state file
   from `tests/dsp_unit/state_fixtures/`.
6. **Presets:** give every factory preset a sensible value. The preset loader must accept files
   that lack the field.
7. **UI and MIDI:** attach the parameter to a control. Add a default CC only if the CC map has room,
   and document it in `docs/midi-implementation.md`.
8. Run the `validate-plugin` skill (automation round-trip and golden renders).

If an existing ID really must change meaning, stop and ask the user under "What I need from you".
