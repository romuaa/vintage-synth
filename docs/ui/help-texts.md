# Hover help texts

The editor's help bubble and help line show these texts (`docs/architecture.md` 9.3.8). This file
is the **single source**: the plugin embeds it and parses it at startup; a test fails when a
parameter has no text, has two, or a text names an ID that does not exist.

Format (one entry per line, nothing else on the line):

    - `<id>`: <text>

- `<part>.` stands for `lower.` and `upper.`: one entry covers both parts.
- `NN` in `seq.stepNN.*` stands for 01 … 16; the editor puts the step number into the text.
- `ui.*` entries are controls that are not parameters.
- Plain language, at most two short sentences, no third-party names. Every statement was checked
  against the engine code (2026-11-14); change a text when the behaviour changes.

## Oscillators and mixer (per part)

- `<part>.vco1.range`: The octave of oscillator 1, in organ-pipe feet. 8' is normal pitch; each smaller number is one octave higher.
- `<part>.vco1.waveform`: The basic tone of oscillator 1. Saw is bright, Square hollow, Pulse nasal, Triangle soft, Sine pure, Noise a hiss without pitch.
- `<part>.vco1.cross_mod`: Oscillator 2 bends the pitch of oscillator 1 at audio speed. Gives bell-like and metallic tones.
- `<part>.vco2.range`: The octave of oscillator 2, in organ-pipe feet. 8' is normal pitch; each smaller number is one octave higher.
- `<part>.vco2.waveform`: The basic tone of oscillator 2. Saw is bright, Square hollow, Pulse nasal, Triangle soft, Sine pure, Noise a hiss without pitch.
- `<part>.vco2.mode`: Normal follows the keys. Low freq ignores keys, bend and glide and runs slowly (about 2 Hz at 8'), to wobble oscillator 1 through cross mod or sync.
- `<part>.vco2.sync`: Restarts oscillator 1 at every cycle of oscillator 2, so oscillator 2 sets the pitch. Set oscillator 1 higher, or sweep it with the envelope, for a tearing lead.
- `<part>.vco2.fine_tune`: Tunes oscillator 2 up to a semitone sharp or flat. A few cents against oscillator 1 make the sound thicker.
- `<part>.mixer.source_mix`: Blends the two oscillators: 0 is oscillator 1 only, 10 is oscillator 2 only.

## Filters and amplifier (per part)

- `<part>.hpf.cutoff`: Removes the lows below this frequency. Higher settings make the sound thinner.
- `<part>.vcf.cutoff`: The brightness of the sound. Lower settings are darker.
- `<part>.vcf.resonance`: Boosts the sound right at the cutoff. High settings whistle; near the top the filter rings by itself.
- `<part>.vcf.slope`: How steeply the filter cuts above the cutoff. −24 dB is steep and dark, −12 dB gentler and brighter.
- `<part>.vcf.env_mod`: How far the envelope moves the filter on each note. An inverted Envelope 1 closes it instead of opening it.
- `<part>.vcf.env_source`: Which envelope moves the filter. Envelope 2 is the one that also shapes the loudness.
- `<part>.vcf.lfo_mod`: How far the LFO sweeps the filter up and down, like a wah.
- `<part>.vcf.key_follow`: Opens the filter more for higher keys. At 10 high and low notes stay equally bright.
- `<part>.vca.level`: The volume of this part.
- `<part>.vca.lfo_mod`: How much the LFO wobbles the volume (tremolo). 0 is none, 3 is the deepest.

## Envelopes (per part)

- `<part>.env1.attack`: How long Envelope 1 takes to rise to its peak after you press a key.
- `<part>.env1.decay`: How long Envelope 1 takes to fall from the peak to the sustain level while you hold the key.
- `<part>.env1.sustain`: The level Envelope 1 stays at while you hold the key.
- `<part>.env1.release`: How long Envelope 1 takes to fall to zero after you let go.
- `<part>.env1.key_follow`: Makes Envelope 1 faster on high keys and slower on low keys.
- `<part>.env1.polarity`: Normal: Envelope 1 moves the filter and pitch up. Inverted: it moves them down instead.
- `<part>.env2.attack`: How long the sound takes to reach full volume after you press a key.
- `<part>.env2.decay`: How long the volume takes to fall from its peak to the sustain level while you hold the key.
- `<part>.env2.sustain`: The volume the sound keeps while you hold the key.
- `<part>.env2.release`: How long the sound takes to fade out after you let go.
- `<part>.env2.key_follow`: Makes Envelope 2 faster on high keys and slower on low keys.

## LFO and pitch modulation (per part)

- `<part>.lfo.waveform`: The shape of the wobble. Random jumps to a new value every cycle; Noise wanders quickly.
- `<part>.lfo.rate`: How fast the LFO (a slow wobble) repeats, from slow sweeps to fast trills.
- `<part>.lfo.delay`: How long after you start playing the LFO wobble is at full strength. It is silent for the first half, then fades in.
- `<part>.vcomod.lfo_mod`: How much the LFO bends the pitch up and down (vibrato).
- `<part>.vcomod.env_mod`: How far Envelope 1 bends the pitch on each note. The oscillators it affects are set by Pitch goes to.
- `<part>.vcomod.target`: Which oscillators the LFO and Envelope 1 pitch amounts move: oscillator 1, both, or oscillator 2.
- `<part>.vcomod.pwm`: How narrow the Pulse wave gets: a fixed width with Manual, or how far it moves with LFO or Envelope 1. Only the Pulse wave changes.
- `<part>.vcomod.pwm_source`: What sets the pulse width: the LFO, a fixed Manual setting, or Envelope 1.

## Playing (per part)

- `<part>.assign.mode`: Poly plays chords. Unison stacks several voices on one note at a time; Solo plays one voice, one note at a time.
- `<part>.assign.unison_voices`: How many voices play each note in Unison mode.
- `<part>.unison.detune`: How far apart the stacked voices are tuned in Unison mode. More detune sounds wider.
- `<part>.porta.time`: How long the pitch glides from one note to the next. Off: the notes jump.
- `<part>.porta.mode`: Always glides to every note. Legato glides only when you play notes that overlap.
- `<part>.bend.range`: How far the pitch wheel bends, in semitones (12 is one octave).
- `<part>.mod.wheel_vco`: How much vibrato the mod wheel adds. It uses the LFO's speed and shape.
- `<part>.vel.vca`: How much quieter soft notes are. At 0 every note plays at full volume.
- `<part>.vel.vcf`: How much softer playing reduces the filter envelope, so soft notes sound darker. No effect when the filter envelope amount is 0.
- `<part>.mix.pan`: Moves this part left or right.

## Whole instrument

- `key.mode`: Whole: the Lower sound on all keys. Dual: both sounds on every key; Split: Lower below the split key, Upper from it upwards.
- `key.split_point`: The lowest key of the Upper part in Split mode. Keys below it play Lower.
- `mix.balance`: The volume balance between the Lower and Upper parts. No effect in Whole mode.
- `global.vintage`: Adds analog looseness: slow pitch drift and small differences in pitch, filter and envelopes between voices.
- `master.volume`: The overall output volume. It is saved with your session, not with a program.
- `master.limiter`: A safety net that keeps loud peaks below the ceiling. Quiet sound passes untouched.
- `master.limiter.ceiling`: The highest level the limiter lets through. Lower leaves more headroom but is quieter.

## Effects

- `fx.chorus.mode`: Off, or one of two chorus settings that make the sound wider and shimmering.
- `fx.chorus.mix`: How much of the chorus is blended in.
- `fx.delay.time`: The time between echoes, from 1 ms to 2 s.
- `fx.delay.feedback`: How many times the echo repeats. Higher settings give longer trails.
- `fx.delay.damping`: Makes each repeat darker than the one before, like a natural echo.
- `fx.delay.ping_pong`: On bounces the echoes between left and right.
- `fx.delay.level`: How loud the echoes are. At 0 the delay is off.
- `fx.reverb.predelay`: A short gap before the reverb starts, which keeps the original sound clear.
- `fx.reverb.decay`: How long the reverb takes to fade away. Long settings sound like a big hall.
- `fx.reverb.damping`: Makes the reverb tail darker, like a room with soft walls.
- `fx.reverb.level`: How loud the reverb is. At 0 the reverb is off.

## Arpeggiator and step sequencer

- `arp.mode`: Off, Arpeggiator (plays the keys you hold one at a time) or Sequencer (plays your step pattern).
- `arp.target`: Which part the arpeggiator or sequencer plays: Lower, Upper or both. A part it does not play gets your keys as usual.
- `arp.pattern`: The order the held notes play in. Up & Down does not repeat the top and bottom notes.
- `arp.octaves`: How many octaves the pattern climbs through before it starts again.
- `arp.rate`: How long each step is, as a note length at the song tempo. A triplet (T) fits three steps into the time of two.
- `arp.gate`: How long each note sounds within its step. Short is choppy, 100 % is smooth.
- `arp.swing`: Delays every second step so the rhythm bounces. 50 % is straight.
- `arp.hold`: Keeps the pattern playing after you let go. The next keys you press start a new pattern.
- `arp.tempo`: The tempo used while the host is stopped. While the host plays, its tempo is used.
- `seq.length`: How many steps play before the pattern starts again.
- `seq.stepNN.note`: The pitch of this step in semitones, counted from the lowest key you hold.
- `seq.stepNN.type`: Note plays the step, Rest stays silent, Tie holds the previous note on without a new attack.
- `seq.stepNN.velocity`: How hard this step is played. What that changes depends on the part's velocity settings.

## Interface items that are not parameters

- `ui.part`: Chooses which part the Sound, Envelopes and LFO pages edit. The page is tinted in that part's colour.
- `ui.page`: Opens a page. Each page does one job; your sound is not changed by switching pages.
- `ui.preset`: The current program. A dot means it was changed since it was loaded.
- `ui.output`: The output level and limiter. Click to open the output settings.
- `ui.fx_power`: Switches this effect on or off by setting its level. Switching it on again brings back the last level.
- `ui.load_program`: Loads the selected program: both sounds and all settings. Volume and limiter stay as they are.
- `ui.tone_source`: Which sound of the selected program to take: its Lower or its Upper tone.
- `ui.load_tone_lower`: Puts the chosen sound into your Lower part. Nothing else changes.
- `ui.load_tone_upper`: Puts the chosen sound into your Upper part. Nothing else changes.
- `ui.search`: Type part of a name or category. Programs that do not match are dimmed.
- `ui.size`: Sets the window size. Saved with your session.
