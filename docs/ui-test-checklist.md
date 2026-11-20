# Manual test checklist (plugin host)

Hands-on checks in JUCE's AudioPluginHost (`scripts/dev-host.sh`, see README "Try it in a plugin
host"), before testing in real DAWs. From M7a the plugin shows its own editor (contract:
`docs/architecture.md` section 9); the general sections apply to every editor, "Custom editor" to
the M7 one. Each line is one check with its expected result.

The host has no CPU meter: use Activity Monitor, and `voice_bench` for budgeting.

## B-Side shell (until its chip sound exists)
Open it with `scripts/dev-host.sh --plugin bside` (the "Exitium B-Side" node). Its sound is a sine per
key (a placeholder), so these checks are about the plugin around it; the general sections below apply too.
- Play keys: a sine per note, up to 16 at once, soft 5 ms start and stop, no clicks. Pitch bend moves it ±2 semitones.
- The editor shows the preset bar above the generic parameter list (master volume, key mode, split point,
  balance, effects, arpeggiator/sequencer, limiter, then the chip parameters, which do nothing yet). The program menu lists 64 programs, all "Init".
- Set Key Mode to Split or Dual: both parts sound (identically); the split point and balance act.
- Switch the arpeggiator on and hold a chord: it arpeggiates the sine; reverb, delay and chorus act.
- Save a program and a bank from the preset bar: files end in `.bsprog` and `.bsbank`, in `Documents/Exitium/B-Side/`.
  A Vintage Synth program file refuses to load into it, and the other way round.
- Bypass the plugin while holding a chord: it fades out and stays silent; un-bypass: nothing hangs.
- Save the session, reload it: all values come back. Both plugins in one project: independent.

## Editor window
- Open and close the editor 50 times quickly: no crash, no memory growth, it reopens at the same size.
- Resize the editor (when resizable) and the host window: the layout follows, no repaint artefacts.
- Quit the host while the editor is open: clean quit.

## Custom editor (M7)
Start it with `scripts/dev-host.sh`:
- The editor opens on the Sound page at 100 % (or the largest menu size that fits the screen); the
  Size menu (top right) and the corner resizer keep the proportions; text stays sharp at 200 %;
  close and reopen the session: page, edited part and size come back.
- LOWER / UPPER: the frame line, headers, sliders and curve turn orange / teal and the text says
  "Editing: LOWER/UPPER"; with key mode Whole, editing Upper shows the warning at the right of the
  help row.
- Sound page: oscillator octave and waveform switches change the pictures above them (Pulse with
  PWM source Manual shows the width; Sync shows the restarts); the filter curve follows cutoff,
  resonance, steepness and high-pass; drag the cutoff dot (left/right cutoff, up/down resonance,
  Shift fine) and the high-pass dot; double-click a dot resets it; the dashed curve is the other
  steepness. Play a resonant saw sweep: the peak you hear is where the curve shows it.
- Signal path: "Effects > Out" opens the Effects page.
- Output: the meter moves with the sound and turns red above -0.1 dBFS; LIMIT and the chevron
  open the output popover (limiter switch, ceiling, reduction meter); Esc or a click outside closes it.
- Host automation of cutoff with the Sound page open: slider, handle and curve move together and
  show the AUTO marker; a program change moves everything at once without markers.

Live automation and program changes:
- Draw host automation for cutoff, a switch (VCF slope) and a sequencer step; play it back with the
  editor open on that page: slider, graph handle and curve, switch and step move with the sound,
  never a stale value; the "AUTO" marker appears while they move and fades after they stop.
- Same with automation on a page that is not shown, then switch to it: it shows the current values at once.
- Map a MIDI controller to cutoff in the host and turn it: the slider and filter curve follow.
- Send MIDI Program Changes, use `<` `>` and the browser: every page shows the new program's values at
  once, the name changes, the modified dot clears; master volume and limiter stay.
- Record automation by dragging a slider and a filter-curve handle (cutoff and resonance together):
  the host records one clean gesture per parameter; playing it back reproduces the move.
- Drag a slider while the host plays automation of it in Read mode: the slider follows your hand, the
  marker shows; after letting go it jumps to the automated value and the host's lane is unchanged.

Parts, pages and help:
- Switch Lower/Upper: the tint, "Editing: LOWER/UPPER" and every value change; Play, Arp + Seq,
  Effects and Browser show "whole instrument". Check it in greyscale (system colour filter): the
  edited part is still obvious from the text.
- Edit Upper in Whole mode: the banner says Upper is not heard.
- Hover every control on every page: a help bubble after about half a second, the help line at once,
  text matches the control.
- Inactive looks: unison controls outside Unison, split key outside Split, balance in Whole, ceiling
  with the limiter off, delay/reverb controls at level 0, arp controls with Arp/Seq Off.
- Every control: drag, fine drag (Shift, Cmd/Ctrl), wheel (a trackpad flick moves a switch by a step or two, not through all choices), double-click resets sliders and graph handles (on a switch it selects; reset is in the context menu), Delete does nothing, typed value (click
  the value, Enter), right-click shows the host's parameter menu where the host offers one.
- Keyboard only: Tab through the page in reading order, arrows change the focused control, Esc
  closes popovers; with the screen reader on, controls read "part, name, value".

Graphs:
- Filter: drag the cutoff/resonance handle and the high-pass handle; the sound matches the curve
  (sweep a saw with resonance: the peak is where the curve shows it); switching the slope redraws.
- Envelopes: drag each point; the times shown match the sliders; inverting Envelope 1 shows the mirrored ghost.
- LFO: drag the delay and cycle handles; the vibrato you hear starts and repeats where the graph shows.
- Arp preview: change pattern, octaves, rate, gate, swing: the preview matches what plays; hold a chord:
  the preview switches to your keys.
- Sequencer: draw steps by dragging across columns, set Rest/Tie, drag the length flag; with the host
  playing the highlighted step follows the sound.
- Effects: delay/reverb power switch turns off (level 0) and back on to the previous level.

Play page keyboard:
- Press a key: the note plays; lower in the key is louder; dragging glides one note at a time;
  dragging off the keys or releasing stops it. Switch page, close the editor or hide the window
  while a key is pressed: the note stops, nothing hangs.
- A held key is tinted in its part's colour (Dual: Lower in the upper half, Upper in the lower
  half); a sounding note shows a small bar above the octave names, also during the release tail
  and on notes the arpeggiator or sequencer plays. The bars never cover "C3", "C4", ...
- The split marker moves only by its pill (drag, arrows, wheel, double-click), not by a press on
  the keys. With the plugin bypassed, the keys play nothing.
- Program name in the top bar: a click opens the list of programs grouped by category.

Browser and files:
- Click, double-click, arrows + Enter in the grid; categories and search dim the rest; Load program,
  Load tone (Lower or Upper tone of the selected program into Lower or Upper), Save into a user bank,
  Save as, Init, Import and Export bank. Close the editor while a file dialog is open: no crash.

Size, screens, performance:
- Size menu 75–200 % and the corner resizer: layout identical in proportion, text sharp; the size
  comes back after reopening the session. Move the window between a normal and a HiDPI monitor.
- Two instances with open editors: independent values, meters and pages.
- Activity Monitor: editor open and idle ≤ 0.5 % of a core; 16 automated parameters + arp + meters
  ≤ 3 % at 100 % size.

## Parameters
- Move every parameter while holding a note: audible change, no clicks or zipper noise.
- Fast sweeps of cutoff, resonance and pitch while playing: no clicks, no crash, no sudden silence.
- Displayed values use the documented units (Hz, ms, cents, panel 0–10).
- Reset to default (double-click, or the host's reset): returns to the default value.

## Output level and limiter
- Hold a full-velocity chord in Dual with 16 notes per part: no harsh clipping at the default
  master volume, limiter on or off.
- Limiter on, ceiling −1 dB, master +6 dB, extreme effects (chorus II, delay and reverb at full
  level): loud but clean, no crackle; the host's output meter never passes the ceiling.
- Sweep the limiter ceiling from 0 to −12 dB and back while a loud chord sounds: the level follows
  smoothly, no clicks or zipper noise.
- Toggle the limiter while quiet material plays (below the ceiling minus 3 dB): no audible change.
- Toggle the limiter while a loud chord is being limited: the level changes smoothly (5 ms fade), no click.
- Load another program and initialise: the limiter switch and ceiling stay as set.

## State
- Set an unusual patch, File > Save, quit, relaunch, File > Open: identical sound and values.
- Two instances (duplicate the node): independent parameters, both sound, no cross-talk.

## Audio setup
- Change the buffer size (32 to 2048) while a note is held: no crash, the note continues or restarts cleanly.
- Change the sample rate (44.1 / 48 / 96 kHz) while playing: pitch stays correct, no hang.
- Switch the output device while playing: recovers without a crash.

## Playing
- 16-note chord, then a 17th note: all sound, the oldest is stolen without a click.
- Poly, Unison and Solo modes: switch while holding notes; voices release without a click.
- Pitch wheel, mod wheel and sustain pedal from a hardware controller respond as designed;
  pedal up releases the notes it was holding.
- Portamento in Always and Legato modes; unison detune from 0 to maximum.
- Toggle Bypass on the node while a note sounds: silence without a click, resumes afterwards.

## Silence and robustness
- After release the output falls to digital silence and CPU use drops.
- Unplug the MIDI keyboard or mute the input device: no crash.
- Hold a note for 10 minutes: level and pitch stay stable, CPU stays flat.
- One idle instance uses very little CPU; a 16-note chord stays within the `voice_bench` figures.
