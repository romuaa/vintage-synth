# Manual test checklist (plugin host)

Hands-on checks in JUCE's AudioPluginHost (`scripts/dev-host.sh`, see README "Try it in a plugin
host"), before testing in real DAWs. Until M7 the plugin shows JUCE's generic parameter editor; the
same list applies to the custom UI later. Each line is one check with its expected result.

The host has no CPU meter: use Activity Monitor, and `voice_bench` for budgeting.

## Editor window
- Open and close the editor 50 times quickly: no crash, no memory growth, it reopens at the same size.
- Resize the editor (when resizable) and the host window: the layout follows, no repaint artefacts.
- Quit the host while the editor is open: clean quit.

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
