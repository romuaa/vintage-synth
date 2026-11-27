# B-Side factory bank: sound specification (S4b)

The 16 programs of Exitium B-Side's factory bank (`presets/bside/factory/<slot>-<name>.json`, embedded at
build time; contract: `docs/architecture.md` 8.21.2). This file is the written specification the programs
were drafted from, the rules they are measured against, and the measurements that show each program does
what its line says. The owner auditions the bank (the S4 audition gate). The numbers show what a program
does (registers, pitches, levels, spectra); they do not say how it sounds, and nothing here claims that.

All names are our own: no brand, part number, nickname of a chip or machine, name of a tune, game,
composer, demo group or tracker, and nothing that reads as a reference to one. The repository's hashed
name check (`scripts/preflight/docs-check.sh`) covers `presets/` and this file.

## Layout

| Group | Theme |
|---|---|
| A | A1 Init, then leads (A2 to A4), basses (A5, A6) and the two table arpeggios (A7, A8) |
| B | Percussion (B1 to B3), pads (B4, B5), a sound effect (B6), a Dual layer (B7) and a Split (B8) |
| C to H | Init (slots 16 to 63 hold the default program, as before the bank was designed) |

The bank is 64 slots of 8 x 8; `makeBankFromFiles` fills the slots that have no file with Init, so the
host sees 64 programs and the first 16 are the designed ones (tested: `Bank: 16 programs A1 to B8 in slot
order ...`).

## Programs

Key mode: W = Whole, D = Dual, S = Split (split point G3, MIDI 55; Lower below, Upper at or above).
Every program is Model B unless named. "What it does" says what the program is made of (settings and
table rows); "Listen for" is an instruction to the listener, not a claim. Parameter IDs are in
`PartParamList.h`; the table format is `docs/architecture.md` 8.6 as amended by 8.13 to 8.16.

| Slot | Name | Category | Key mode | What it does | Listen for |
|---|---|---|---|---|---|
| A1 | Init | Init | W | Every parameter at its default: one pulse oscillator at 50 % through the low-pass at register 1024; the starting point and the level reference | Play a chord and compare the level of every other program with it |
| A2 | Moving Pulse | Lead | W | Pulse lead, Solo with Legato glide (`porta.time` 0.232; 0.3 before the second audition). The pulse lane sets the width to 768 and ping-pongs it between 768 and 3328 at 40 per tick; the wave lane holds the key's pitch for 8 ticks and then alternates it by +-0.2 semitone every three ticks (a six-tick period) | Hold one key for two seconds and follow whether the tone changes while you hold it; play two keys legato and follow the pitch between them |
| A3 | Synced Climb | Lead | W | Hard-sync lead: oscillator 2 (sawtooth) synced to oscillator 1, which is silent (waveform Off) and runs at the key's pitch as the sync source. The wave lane of oscillator 2 steps its pitch from +3 to +24 semitones above the key and back down, one semitone every two ticks | Hold one key for two seconds and follow how the pitch relation between the two oscillators moves |
| A4 | Glass Ring | Lead | W | Triangle on oscillator 1, ring-modulated by oscillator 3 for the first ten ticks (the wave lane sets the ring bit, then clears it); oscillator 3 is a silent ring source (a triangle with no gate) whose pitch the wave3 lane drops from +10.5 to +8.5 to +6.5 semitones above the key. The envelope decays from 15 to a sustain of 11. Not through the filter | Play one key and compare its first quarter second with the rest of the held note |
| A5 | Closing Door | Bass | W | Filter bass: sawtooth plus a pulse an octave down through the low-pass (resonance 9, base cutoff 1800); the filter lane sweeps the cutoff down by 75 per tick for 20 ticks and stops; `vel.cutoff` 0.6 lowers the cutoff for softer keys; Solo | Play the same low key softly and hard and compare how open the filter starts |
| A6 | Blip Bass | Bass | W | Pulse bass, width 512 (12.5 %), no filter (oscillator 1 is not routed through it), Solo. The wave lane plays the first two ticks one octave up (+12) and then the key's pitch | Play a low key and listen to its first forty milliseconds |
| A7 | Major Steps | Arpeggio | W | Chip arpeggio in the wave lane (the shell's arpeggiator is off): one key steps +0 +4 +7 +12 +7 +4 semitones, one pitch per tick, repeating; tick rate Tempo, 12 ticks per beat, multiplier 1; pulse width 25 %. The note-off clears the gate and ends the lane | Hold C4 for four seconds, then change `arp.tempo` and play it again |
| A8 | Minor Steps | Arpeggio | W | The minor counterpart: +0 +3 +7 +10 +12 +10 +7 +3, tick multiplier 2 (twice the rate of A7), and the pulse lane ping-pongs the width between 640 and 3200 at 128 per tick | Hold C4 for four seconds and compare the pace and the width movement with A7 |
| B1 | Short Kick | Percussion | W | One noise tick (tick 0, `=C7`) and then a triangle whose pitch falls by table `=55 =43 =36 =33 =31 =29 =28 =27` over eight ticks of 9.98 ms (tick multiplier 2) to 38.9 Hz; absolute pitches, so every key plays the same drum; decay 8, sustain 0; oscillator 2 doubles oscillator 1 in phase (the level) | Play three different keys and compare the drum |
| B2 | Dry Snare | Percussion | W | Noise and pulse alternate by table (`noise =D7`, `pulse =G3`, `noise =C7`, `pulse =E3`, then `noise =B6` held, the gate cleared at tick 10), pulse width 1536, tick multiplier 2; absolute pitches; oscillator 2 doubles oscillator 1 in phase (the level); the restart section is `mode classic`, `lead 2`, as drafted (restored once Clean stopped waiting, see "Percussion in time") | Play three different keys and compare the drum |
| B3 | Tick Hat | Percussion | W | Noise at `=E7` through the high-pass (cutoff 500, resonance 2), decay 6, sustain 0: decay only; oscillators 2 and 3 double oscillator 1 in phase, also through the high-pass (the level); the restart section is `mode off` (see "After the owner's audition") | Play eight hits and follow how long each one rings |
| B4 | Slow Saw Pad | Pad | W | Model A, Per Note: three sawtooths detuned by -14, 0 and +14 cents through the low-pass (cutoff 900, resonance 5); attack 9, sustain 12, release 9 | Play a four-note chord and follow how the sound arrives |
| B5 | Shared Chord Pad | Pad | W | Model A, Shared: a sawtooth and a 35 % pulse (+7 cents) of every note into one low-pass (cutoff 1100, resonance 6); attack 8, sustain 12, release 9 | Play the four-note chord in this program and in B4 and compare how the chord sounds against the single notes |
| B6 | Band Sweep | Sound FX | W | Noise through the band-pass (resonance 10); the filter lane sets the cutoff to 100 and adds 16 per tick for 120 ticks (to 2020); attack 5, release 11 (2.4 s nominal) | Hold a key for three seconds and then let go, and follow the sweep and the end |
| B7 | Pulse Over Pad | Dual | D | Lower: its own copy of the earlier pulse lead (Model B, pan -0.6): the pulse sweep of A2 (768 to 3328, +40 per tick) with the vibrato A2 had before the owner's audition (starts after 15 ticks, +-20 cents every 2 ticks, 12.53 Hz), no glide, cutoff 1500 resonance 3, `mix.level` -7.5; Upper: a Model A pad of two sawtooths at -10 and +10 cents (pan +0.6); both parts play every key | Play a chord and follow which side each layer is on |
| B8 | Bass And Lead | Split | S | Lower (below G3): the bass of A5; Upper (G3 and above): its own copy of the earlier pulse lead (the same table as B7's Lower: vibrato after 15 ticks at 12.53 Hz, not A2's 8 ticks and 8.35 Hz) with a glide (`porta.time` 0.3, Legato; A2's is 0.232); both Solo | Play low keys with one hand and a melody above G3 with the other |

## Standard phrases and level groups

Every program is measured with the phrase of its category (`tests/bside/integration/BankSound.h`), at
velocity 100, 48 kHz, the default master volume (-6 dB) and the fixed output trim (-6.02 dB), rendered
through the real preset path (`PresetManager::selectProgram` on `BSideProcessor`). Levels are measured with
the output limiter **off**, so the rule compares programs and not the limiter. The phrases are Vintage's
(`docs/factory-sounds.md`) where B-Side has the same categories; the two B-Side ones are fixed by the
contract (8.21.2).

| Phrase | Categories | Notes |
|---|---|---|
| Chord | Init, Pad, Sound FX, Dual | C4 E4 G4 C5 held 3 s |
| Melody | Lead | eight legato notes G4 to G5, 0.35 s each (each note-off 10 ms after the next note-on) |
| Bass line | Bass | eight staccato notes C2 to C3, 0.3 s apart (0.22 s held) |
| Percussion | Percussion | eight hits of C3 (note 48), 0.25 s apart, each held 0.2 s |
| Arpeggio | Arpeggio | C4 held 4 s: one key plays the chord |
| Split phrase | Split | ten staccato bass notes C2 to C3 below the split point with C4 E4 G4 held 3 s above |

**Level groups** (8.21.2 after the drafting): *sustained* (Lead, Pad, Sound FX, Dual, Split, and the table
arpeggios A7 and A8: one held voice), *percussive* (Percussion) and *bass* (Bass). Init is the reference and
belongs to no group.

**Level offsets** (8.21.2 after the owner's audition; `kLevelOffsetDb` in `BankSound.h`, the test and the hidden
render read it): a program may sit a set number of dB above its group's target, by the owner's ear.

| Slot | Name | Group | Offset dB |
|---|---|---|---|
| B1 | Short Kick | percussive | +4.5 (+3 before the second audition) |
| B2 | Dry Snare | percussive | +1.5 (0 before) |
| B3 | Tick Hat | percussive | +2.0 (+3.5 before the third audition, +5 before the second) |
| B4 | Slow Saw Pad | sustained | +3 |
| every other program | | | 0 |

B1, B2 and B3 are judged by their peak (the offset applies to the peak target; the percussive group has no program with offset 0 any more, and its target is the sustained group's median peak), B4 by its RMS. The sustained
group's median RMS is unchanged by leaving B4 out (-32.02 dB over the nine programs with offset 0); its
median phrase peak, the percussion target, moves from -20.44 to -20.47 dBFS (the 5th of nine peaks, A4's).
The headroom checks (rule 2) are unchanged.

**Level rule** (enforced over the embedded bank by `BankTests.cpp`, one test case over the 16 programs):

1. Loudness = unweighted RMS of both channels from the first note-on to the last note-off of the standard
   phrase. Each sustained and bass program is within 2 dB of its group's median RMS plus its offset (below).
   **Percussion is judged by its peak:** a drum's phrase peak is within 2 dB of the median phrase peak of the
   sustained group plus its offset, so a drum hit sits as loud as a lead note. A group's median is taken over
   its programs with offset 0.
2. An eight-note chord (C3 E3 G3 C4 E4 G4 C5 E5) at velocity 127, held 2 s: neither its RMS (from 0.55 s)
   nor its peak is more than 3 dB above Init's (Init: -19.49 dB RMS, -10.11 dBFS peak, so the limits are
   -16.49 dB and -7.11 dBFS), and its peak is below 0 dBFS. For percussion the check is only "below 0 dBFS
   with two keys down" (C3 and C4 at velocity 127, held 2 s): eight identical fixed-pitch hits summing in
   phase is not a playing situation and must not set the level.
3. The standard phrase peaks below 0 dBFS, and with the limiter **on** at its defaults (ceiling -1 dBFS,
   knee from -4 dBFS) it stays below the knee, so the limiter never shapes a factory sound played normally.
4. Also tested for every program in the same case: it loads (the selected slot holds the file's program), is
   finite, is not silent (peak above -50 dBFS, RMS above -60 dB; the drums play eight short hits, so
   Vintage's -40 dB would not fit them), is below -80 dBFS in the last 100 ms 0.75 s after the tail the
   processor reports, saves and restores its state (every parameter and both tables the same, and saving
   again gives the same bytes), and, applied over three held keys, releases them to silence.

Levels are trimmed with `mix.level` (a smoothed per-part gain, -24 to +6 dB; the chip's volume register
cannot trim, 8.21.2). Master volume, the limiter switch and its ceiling are session settings, not part of
a program. The programs' own sounds were drafted first; `mix.level` was then set so that the phrase RMS
lands on its group's target (sustained -32.0 dB, bass -33.5 dB) and a drum's phrase peak on the sustained
group's median peak (-20.44 dBFS), within the limits of rule 2. A3 is the program held closest by rule 2 (its
chord peak is the highest relative to its phrase).

**Raised in their content: B1, B2 and B3.** The drums needed +10.2, +11.2 and +5.7 dB on their drafted
`mix.level` (-2.3, -3.4 and +6.0), so B1 and B2 would have ended at +7.9 and +7.8 and B3 at +11.7 dB, beyond
`mix.level`'s +6 dB. Their chip volume is at 15 already, their envelopes start at full level (attack 0) and
the peak of a decay-only hit does not depend on the sustain, so none of those can raise them. What does:
**oscillator 2 doubles oscillator 1**: the same parameters (`osc2.*` as `osc1.*`) and a `[wave2]` lane that
is a copy of `[wave1]` (B2 also `[pulse2]` as `[pulse1]`), so the two oscillators get the same register writes
on the same cycles and sum in phase (+6.0 dB at the output, measured). The trims are then +1.9, +1.7 and
+5.6 dB. Each drum's test of what its row says was run again and passes unchanged, and now also checks that
oscillator 2's seven registers and envelope counter equal oscillator 1's at every 1 ms read (`Bank B1`,
`Bank B2`, `Bank B3`). After the owner's audition B3 doubles with oscillators 2 and 3 (the third oscillator
is a further +3.4 dB), see below.

### Measured figures

From `bside_metrics.tsv` of the hidden render (below), macOS arm64, 2026-11-25, after the third audition (the B3 row changed: `mix.level` 4.2 to 2.7, offset +3.5 to +2.0; the rows of B1 and B2 are those of the second audition; A2's glide does not move its phrase figures).
`mix.level` is Lower / Upper in dB. Phrase RMS and peak with the limiter off. The chord is the eight-note
chord of rule 2, for percussion the two-key chord (its RMS from 0.55 s says nothing for a drum, whose hits
are over by then). The deviation is from the group's median RMS plus the offset, for percussion from the
sustained group's median peak plus the offset; the offset column is `kLevelOffsetDb`.

| Slot | File | Name | Category | Group | Offset dB | `mix.level` | Phrase RMS dB | Phrase peak dBFS | Deviation dB | Chord peak dBFS | Chord RMS dB | Reported tail s | Phrase peak, limiter on |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A1 | A1-init | Init | Init | reference | 0 | 0 | -23.53 | -16.83 |  | -10.11 | -19.49 | 0.61 | -16.83 |
| A2 | A2-moving-pulse | Moving Pulse | Lead | sustained | 0 | -1.5 | -31.97 | -25.71 | +0.06 (RMS) | -24.69 | -30.49 | 0.50 | -25.71 |
| A3 | A3-synced-climb | Synced Climb | Lead | sustained | 0 | +2.2 | -32.05 | -20.37 | -0.02 (RMS) | -8.53 | -22.54 | 0.50 | -20.37 |
| A4 | A4-glass-ring | Glass Ring | Lead | sustained | 0 | +4.3 | -31.99 | -20.47 | +0.04 (RMS) | -11.28 | -22.67 | 0.72 | -20.47 |
| A5 | A5-closing-door | Closing Door | Bass | bass | 0 | -1.2 | -33.52 | -21.53 | -0.01 (RMS) | -20.81 | -31.32 | 0.34 | -21.53 |
| A6 | A6-blip-bass | Blip Bass | Bass | bass | 0 | +1.6 | -33.50 | -24.08 | +0.01 (RMS) | -22.85 | -32.77 | 0.22 | -24.08 |
| A7 | A7-major-steps | Major Steps | Arpeggio | sustained | 0 | -1.3 | -32.03 | -25.62 | -0.01 (RMS) | -10.88 | -21.49 | 0.34 | -25.62 |
| A8 | A8-minor-steps | Minor Steps | Arpeggio | sustained | 0 | -2.2 | -31.99 | -26.89 | +0.03 (RMS) | -10.18 | -20.22 | 0.34 | -26.89 |
| B1 | B1-short-kick | Short Kick | Percussion | percussive | +4.5 | +6.0 | -30.89 | -16.23 | -0.26 (peak) | -9.58 (two keys) | -92.84 | 0.61 | -16.23 |
| B2 | B2-dry-snare | Dry Snare | Percussion | percussive | +1.5 | +3.2 | -32.54 | -18.97 | -0.00 (peak) | -12.29 (two keys) | -88.06 | 0.61 | -18.97 |
| B3 | B3-tick-hat | Tick Hat | Percussion | percussive | +2.0 | +2.7 | -36.04 | -18.47 | -0.00 (peak) | -11.77 (two keys) | -106.91 | 0.61 | -18.47 |
| B4 | B4-slow-saw-pad | Slow Saw Pad | Pad | sustained | +3 | -3.1 | -29.01 | -15.66 | +0.01 (RMS) | -10.81 | -26.46 | 2.25 | -15.66 |
| B5 | B5-shared-chord-pad | Shared Chord Pad | Pad | sustained | 0 | -2.4 | -32.04 | -18.60 | -0.02 (RMS) | -18.18 | -43.37 | 2.25 | -18.60 |
| B6 | B6-band-sweep | Band Sweep | Sound FX | sustained | 0 | +2.7 | -32.02 | -15.87 | +0.00 (RMS) | -12.26 | -28.60 | 7.20 | -15.87 |
| B7 | B7-pulse-over-pad | Pulse Over Pad | Dual | sustained | 0 | -7.5 / -7.5 | -31.97 | -20.40 | +0.06 (RMS) | -15.49 | -28.62 | 2.25 | -20.40 |
| B8 | B8-bass-and-lead | Bass And Lead | Split | sustained | 0 | -4 / -4 | -32.03 | -21.22 | -0.00 (RMS) | -21.62 | -30.65 | 0.50 | -21.22 |

Targets (programs with offset 0): sustained median RMS -32.02 dB, bass median RMS -33.51 dB, sustained median
phrase peak -20.47 dBFS (the percussion target; A4's, the 5th of the nine sustained peaks with offset 0). With
the offsets the targets are B1 -15.97 dBFS (peak), B2 -18.97 dBFS (peak), B3 -18.47 dBFS (peak; -16.97 before the third audition) and B4 -29.02 dB (RMS).
Loudest phrase peak: B4, -15.66 dBFS (B6 -15.87, B1 -16.23, B3 -18.47; the limiter's knee is at -4 dBFS). Highest eight-note chord peak of the
designed programs: A3, -8.53 dBFS, 1.42 dB inside the -7.11 dBFS limit (Init's own is -10.11 dBFS); then A8
(-10.18) and A7 (-10.88). A3's chord is the sum of eight sawtooths whose accumulators all start at the same
cycle, so their peaks coincide. Before this rule the drums and the arpeggios were trimmed 13 dB below the
leads by that eight-note chord (eight identical absolute-pitch hits, in phase); with two keys a drum peaks at
-13.7 dBFS or below.

The level rule with offsets can fail (figures of the first audition round's offsets, +5 for B3; not repeated for the third audition's +2.0, whose test is the same code): B3 with `mix.level` 2.5 dB lower (+3.1) fails the level test with a
deviation of -2.60 dB from its target plus offset (-15.47 dBFS), and B4 at its earlier `mix.level` (-6.1,
before its +3 dB offset) with a deviation of -3.0 dB from -29.02 dB (both run on 2026-11-25, then restored).

### Cost of the tests

The level test is the one case that renders every program (about 12 s of the new default time on this
Mac); the per-program tests below take 0.1 to 0.6 s each.

## What each program does, measured

Registers are read back from the chip of the part's newest voice (`chain.chip().registerValue`) after every
1 ms block while the program plays, through the real processor (`probe` in `BankSound.h`; `VS_BANK_VERBOSE=1`
prints the series). A tick index n is the n-th tick the voice has run (tick 0 is the note-on tick). Tick
period at the default 50 Hz system clock: 19 656 cycles = 19.95 ms (multiplier 1), 9 828 cycles = 9.98 ms
(multiplier 2). Pitches are the frequency register F times clock / 2^24, compared with the note's
equal-tempered frequency to within one register step (0.059 Hz). Each line is a test in `BankTests.cpp`
(`Bank A2` ... `Bank B8`).

- **A2** (note C4, held 1.6 s). Pulse width register per tick, ticks 0 to 71: `768 808 848 ... 3288 3328
  3288 3248 ... 3048` (+40 per tick from tick 1, 3328 at tick 64, then back down at -40). Pitch of
  oscillator 1: constant for ticks 0 to 7 (159.6 ms), then in cents against the key `+19.7 +19.7 +19.7 -20.3
  -20.3 -20.3` repeating (the register's step is 0.4 cent) from tick 8: a six-tick period, 119.7 ms, 8.35 Hz.
  Before the owner's audition: constant for ticks 0 to 14 (299.3 ms), `+19.7 +19.7 -20.3 -20.3` from tick 15,
  a four-tick period, 79.8 ms, 12.53 Hz. Assign mode Solo and `porta.time` 0.232 (0.3 before the second audition, see there), Legato glide. The
  test presses C4 at 0.1 s, G4 at 0.4 s and lets go of C4 at 0.41 s, renders to 0.8 s, and reads the frequency register of the voice every
  1 ms (48 samples). It uses the program's own `porta.time` (it set 0.3 explicitly until the review; the control run sets 0). The window is
  the 401 reads from the second note-on to the end of the 0.8 s render, not 0.4 to 0.9 s after the second note-on. In it the pitch is between
  0.5 and 6.5 semitones above C4 for 150 reads (150 ms), against 0 with `porta.time` 0 (and 272 reads with 0.3; the figure of 251 reads that
  stood here fitted neither). The first read is +0.20 semitones (C4 with the table's vibrato at +20 cents) and the last is +7.19 (G4 with the
  vibrato at +19 cents: the glide has arrived, the 0.2 is the vibrato, not part of the interval). With 0.3 the last read was +7.08: the
  glide had not finished its approach, and 7.08 is not "0.2 + 7" of any table step.
- **A3** (C4, 1.5 s). Oscillator 2's control register 0x23 (sawtooth, sync, gate), oscillator 1's 0x00 with F
  at C4 (261.63 Hz). Oscillator 2 above oscillator 1 per tick, in semitones: `3 3 4 4 5 5 ... 24 24 23 23 22
  22 ...` (two ticks per step, up to +24 at ticks 42 and 43, then down). The same note with the table's
  `+sync` removed (through `PresetManager::setTable`): bit 1 is clear and the octave-band energies of the
  steady part (0.2 to 1.0 s) differ by up to 31.1 dB (centroid 4427 Hz with sync, 4498 Hz without).
- **A4** (C4, 1.0 s). Oscillator 1's control per tick: `0x15` for ticks 0 to 9, `0x11` from tick 10 (ring
  bit cleared); its pitch never moves. Oscillator 3's control `0x10` (triangle, no gate) and its frequency per
  tick: `479.8 479.8 479.8 427.5 427.5 427.5 380.8 ...` Hz (+10.5, +8.5, +6.5 semitones above C4). The same
  note with `+ring` removed: first 60 to 200 ms, spectral centroid 3896 Hz with ring and 589 Hz without
  (octave-band difference up to 39.0 dB); 0.5 to 1.0 s, 584 Hz in both (difference 0.0 dB).
- **A5** (C4, 1.0 s). Oscillator 1 `0x21` (sawtooth), oscillator 2 `0x41` (pulse) at 130.8 Hz (an octave
  below), mode Low, resonance 9, filters oscillators 1 and 2. Cutoff register per tick at velocity 100:
  `1594 1519 1444 ... 244 169 169 169 ...` (-75 per tick for 20 ticks, then constant). The cutoff at tick 0
  for velocity 127, 100, 64 and 20: `1725 1594 1420 1207`. Spectral centroid 2971 Hz in 0.06 to 0.25 s and
  695 Hz in 0.6 to 1.0 s of the audio.
- **A6** (C4, 0.3 s). Oscillator 1 frequency: 523.24 Hz at ticks 0 and 1, 261.62 Hz from tick 2 (the blip lasts
  39.0 ms, two ticks of 19.95 ms, to within the 1 ms of the read; before the owner's audition 523.24 Hz at tick 0
  only and 19.0 ms); pulse width register 512 (12.5 %);
  routing bit for oscillator 1 clear (`routing` = 6: oscillators 2 and 3 only, and neither sounds).
- **A7** (C4, 1.2 s). Semitones above the key per tick: `0 4 7 12 7 4` repeating for 24 ticks. Tick period
  41 052 cycles = 41.67 ms at 120 bpm (`arp.tempo`; the host gave no tempo), equal to the formula clock x 60
  / (bpm x 12 x 1); at 90 bpm the period is 120/90 as long. `arp.mode` is Off. One key: one voice sounds,
  and four different frequency registers appear. After the note-off the gate is clear and the pitch no
  longer moves.
- **A8** (C4, 1.0 s). Semitones per tick: `0 3 7 10 12 10 7 3` repeating for 40 ticks; the tick period is
  exactly half of A7's (20 526 cycles); pulse width register `640 768 896 ... 2560` (+128 per tick from tick
  1), 3200 at tick 20, 3072 at tick 21.
- **B1** (keys 36 and 60, 0.3 s). Oscillator 1 control `0x81` (noise, gate) at tick 0, `0x11` after; frequency
  per tick: `2093.0 196.0 98.0 65.4 55.0 49.0 43.6 41.2 38.9 38.9 ...` Hz (the fall is -1897, -98, -32.6, -10.4,
  -6.0, -5.4, -2.4, -2.3 Hz per tick, then none); before the owner's audition `2093.0 329.6 196.0 130.8 98.0 82.4
  73.4 73.4 ...`; the same for both keys. Envelope registers `0x08 0x08`. Oscillator 2's
  seven registers and envelope counter equal oscillator 1's at every read (the doubling).
- **B2** (key 48, 0.3 s). Oscillator 1 control per tick: `81 41 81 41 81 81 81 81 81 81 80 80 80 80` (noise,
  pulse, noise, pulse, noise ... then the gate cleared at tick 10); pitches `=D7 =G3 =C7 =E3 =B6`; pulse
  width 1536. Oscillator 2 equals oscillator 1 at every read (the doubling).
- **B3** (key 48, 0.5 s). Control `0x81`, F at `=E7` (2637.0 Hz; `=C7`, 2093.0 Hz, before the owner's audition),
  envelope registers `0x06 0x06`, mode High (4), cutoff register 500 and resonance 2 (1500 and 0 before),
  oscillators 1, 2 and 3 routed (`routing` 7), oscillators 2 and 3 equal to oscillator 1 at every read (the
  doubling). The envelope counter peaks at 253 at 3 ms, is at or below 128 at 37 ms, at or below 16 at 120 ms
  and 0 at 208 ms (the key goes up at 500 ms); the audio's RMS in the 150 ms before the key goes up is -91.7 dB
  with its DC and -102.0 dB without (-107.2 dB at the level before the owner's audition, which was measured with
  its DC; the test's bound is -100 dB and the test now takes the RMS with the window's mean removed, see
  "After the owner's audition"). 85 % of the energy of the note (0.05 to 0.3 s) is at or above 4 kHz (centroid
  11 052 Hz; 99 % and 13 730 Hz before).
- **B4** (chord, 1.2 s). `chip.model` Model A and `filter.topology` Per Note in the parameters the audio
  thread used; the newest voice's own filter reports Model A. Three oscillators `0x21`; the detune of
  oscillators 1 and 3 against 2: -14.0 and +13.9 cents. Mode Low, resonance 5, all three routed; attack
  nibble 9 (register `0x90`); the envelope counter reaches 200 of 255 at 199 ms.
- **B5** (chord). Model A, Shared in the parameters used; with the chord held there are four voices, and the
  part's shared filter reports Model A, mode Low (`0x10`), resonance 6, cutoff register 1100. The same chord
  with the topology set to Per Note: RMS -26.81 dB against -33.34 dB Shared (6.5 dB apart; octave bands differ
  by up to 7.5 dB).
- **B6** (C4, 2.7 s then 4 s). Control `0x81`, mode Band (2), routing 1, resonance 10; cutoff register
  `100 + 16 x tick` up to tick 120 (2020), read at ticks 0, 15 ... 120: `100 340 580 820 1060 1300 1540 1780
  2020`; release nibble 11; after the note-off the envelope counter is below 32 of 255 after 1.05 s.
- **B7** (keys 60 and 64). Key mode Dual; `chip.model` Lower Model B, Upper Model A; pan -0.6 and +0.6; both
  parts hold voices for both keys (60 and 64). Lower oscillator 1 control `0x41` and pulse width `768 + 40 x
  tick`; Upper oscillators 1 and 2 `0x21`, oscillator 3 `0x00`. With only the Lower part audible the left
  channel is 7.1 dB above the right; with only the Upper part audible the right is 6.9 dB above the left.
- **B8** (keys 48, 64 and the pair 54 and 55). Key mode Split, split point 55; both parts Solo. With the
  keys 48, 64, 54 and 55 pressed together Lower sounds 54 and Upper sounds 55 (Lower below the split point,
  Upper at or above). One key each: 48 sounds one voice in Lower and none in Upper; 64 the reverse. Lower:
  oscillator 1 `0x21`, oscillator 2 `0x41`, the cutoff falls by 75 per tick. Upper: control `0x41`, pulse
  width `768 + 40 x tick`, pitch constant to tick 14 and moving at tick 16.

## After the owner's audition (2026-11-25)

The owner's verdicts are in `docs/architecture.md` 8.21.2 ("After the owner's audition"). What changed, measured
before (the bank as the audition had it) and after (macOS arm64, 2026-11-25, `bside_metrics.tsv` and the register
and timing figures of the tests; every number is a measurement, none says how a program sounds).

| Program | Before | After |
|---|---|---|
| A2 Moving Pulse | vibrato starts after 15 ticks = 299.3 ms; period 4 ticks = 79.8 ms, 12.53 Hz | starts after 8 ticks = 159.6 ms (-46.7 %); period 6 ticks = 119.7 ms, 8.35 Hz (-33.4 %). `mix.level` -1.5 (unchanged), phrase RMS -31.97 dB, peak -25.71 dBFS (both unchanged) |
| A6 Blip Bass | octave blip lasts 1 tick, measured 19.0 ms | 2 ticks, measured 39.0 ms (x2.05); nothing else changed. `mix.level` +1.6 (unchanged), phrase RMS -33.51 to -33.50 dB, peak -24.02 to -24.08 dBFS |
| B1 Short Kick | `mix.level` +1.9, phrase peak -20.45 dBFS, RMS -34.94 dB; triangle `=64 =55 =48 =43 =40 =38` (329.6 Hz to 73.4 Hz in six ticks); energy 0 to 125 Hz -4.24 dB (re the sum of the nine bands) | `mix.level` +4.8, peak -17.43 dBFS (target -17.47), RMS -32.09 dB; triangle `=55 =43 =36 =33 =31 =29 =28 =27` (196 Hz to 38.9 Hz in eight ticks); 0 to 125 Hz -2.13 dB; length 270 ms against 265 ms (below) |
| B3 Tick Hat | `mix.level` +5.6, peak -20.37 dBFS, RMS -40.38 dB; two oscillators at `=C7`, high-pass cutoff 1500 resonance 0, `[restart]` classic lead 2 | `mix.level` +5.6, peak -15.57 dBFS (target -15.47), RMS -33.14 dB; three oscillators at `=E7`, cutoff 500 resonance 2, `[restart]` `mode off` |
| B4 Slow Saw Pad | `mix.level` -6.1, RMS -32.01 dB, peak -18.66 dBFS, chord peak -13.81 dBFS, chord RMS -29.46 dB | `mix.level` -3.1, RMS -29.01 dB (target -29.02), peak -15.66 dBFS, chord peak -10.81 dBFS (limit -7.11), chord RMS -26.46 dB (limit -16.49) |

`mix.level` stayed inside -24 to +6 dB; B3 is raised in its content (the third oscillator and the lower cutoff
give +5.0 dB of peak at an unchanged `mix.level`), B1 needed only the trim. Octave-band energies of the phrase
(dB re the sum; bands 0 to 125 Hz, 125 to 250, 250 to 500, 500 to 1k, 1k to 2k, 2k to 4k, 4k to 8k, 8k to 16k,
above 16k):

| | 0 | 125 | 250 | 500 | 1k | 2k | 4k | 8k | 16k |
|---|---|---|---|---|---|---|---|---|---|
| B1 before | -4.2 | -5.6 | -6.6 | -17.2 | -17.1 | -15.8 | -15.3 | -15.9 | -19.6 |
| B1 after | -2.1 | -6.5 | -15.9 | -16.6 | -17.0 | -16.1 | -15.6 | -15.3 | -18.8 |
| B3 before | -38.7 | -51.5 | -48.5 | -45.3 | -35.4 | -22.7 | -12.2 | -2.4 | -4.4 |
| B3 after | -33.6 | -48.5 | -46.0 | -33.7 | -20.7 | -9.1 | -4.0 | -5.9 | -6.7 |

B1's length is the time from a single hit's note-on (held 0.2 s) to the end of the last 5 ms of audio whose RMS is
within 43 dB of the hit's sample peak: 265 ms before, 270 ms after (the envelope is unchanged: decay 8); the test
asks for at most 1.5 times the earlier length.

### Second audition (2026-11-25): A2's glide and the kit balance

The owner's verdicts are in `docs/architecture.md` 8.21.2 ("Second audition"): A6, B1, B3 and B4 are right; A2's glide to
the played note is about 40 % faster; the kit balance (the three drums together) becomes B1 +4.5 dB, B2 +1.5 dB,
B3 +3.5 dB. Measured on macOS arm64, 2026-11-25.

**A2.** Only `porta.time` changed, 0.3 to 0.232 (the glide's 99 % time `5 x porta.time^2` s: 0.450 to 0.269 s). Measured
by the test `Bank A2 Moving Pulse` on a legato octave step (C4 held, C5 pressed 0.3 s later; osc 1's frequency register read
every 0.25 ms from the second note-on):

| | before (0.3) | after (0.232) |
|---|---|---|
| until within 50 cents of the played note | 279.5 ms | 166.2 ms (-40.5 %) |
| first sample within 10 cents of the played note | 399.2 ms | 215.2 ms (-46.1 %) |

The 10-cent figure is not the one that scales with the glide: the table's vibrato (+-20 cents once it has started, 159.6 ms
after the note-on) takes the pitch in and out of a 10-cent window, so the first crossing jumps with the vibrato's phase
(for `porta.time` 0.230, 0.232 and 0.235 it is 211, 215 and 280 ms; the 50-cent time is 163, 166 and 170 ms). The test
gates the 50-cent time at 167.7 ms (60 % of 279.5) within 4 ms and reports the 10-cent one. Nothing else in A2 changed:
phrase RMS -31.97 dB and peak -25.71 dBFS as before, the octave-band energies identical to 0.1 dB. B7's pulse lead has no
glide, and B8's lead is its own table; neither changed.

**Kit balance.** The tables of the drums are unchanged; only `mix.level` moved (B1 4.8 to 6.0, B2 1.7 to 3.2, B3 5.6 to
4.2). Targets: the sustained group's median phrase peak (-20.47 dBFS) plus the offset.

| | offset | target peak | `mix.level` before / after | peak before / after dBFS | RMS before / after dB | deviation after |
|---|---|---|---|---|---|---|
| B1 Short Kick | +4.5 | -15.97 | +4.8 / +6.0 | -17.43 / -16.23 | -32.09 / -30.89 | -0.26 dB |
| B2 Dry Snare | +1.5 | -18.97 | +1.7 / +3.2 | -20.47 / -18.97 | -34.04 / -32.54 | 0.00 dB |
| B3 Tick Hat | +3.5 (+2.0 after the third audition) | -16.97 (-18.47) | +5.6 / +4.2 (+2.7) | -15.57 / -16.97 (-18.47) | -33.14 / -34.54 (-36.04) | 0.00 dB |

B1 is at the top of `mix.level`'s range (+6 dB), 0.26 dB under its target, which is inside the test's 2 dB and which
`mix.level` cannot close. Raising B1 in content was tried and rejected (the files are not kept): a third oscillator
doubling the other two (`osc3.*` as `osc2.*`, a copy of `[wave2]` as `[wave3]`, `mix.level` +2.7) gave a peak of -16.01 dBFS
but a hit-to-hit peak spread of 1.09 dB, over the gate's 1 dB (0.84 dB with two oscillators), and a third oscillator on the
triangle alone gave 1.32 dB. A change that moves the spread is also a change of character, so B1 keeps its content and
sits 0.26 dB short.

Octave-band energies of the phrase (dB re the sum; 0 to 125 Hz, 125 to 250, 250 to 500, 500 to 1k, 1k to 2k, 2k to 4k,
4k to 8k, 8k to 16k, above 16k) are identical before and after, to the 0.1 dB the table prints, as the tables are unchanged:

| | 0 | 125 | 250 | 500 | 1k | 2k | 4k | 8k | 16k |
|---|---|---|---|---|---|---|---|---|---|
| B1 | -2.1 | -6.5 | -15.9 | -16.6 | -17.0 | -16.1 | -15.6 | -15.3 | -18.8 |
| B2 | -11.2 | -3.7 | -8.6 | -10.8 | -11.8 | -12.3 | -12.2 | -12.2 | -15.1 |
| B3 | -33.6 | -48.5 | -46.0 | -33.7 | -20.7 | -9.1 | -4.0 | -5.9 | -6.7 |

Lengths are unchanged (B1 270 ms; the reported tail 0.61 s). Percussion in time, after (onset ms / peak dBFS; every gate
holds; an onset is counted from -30 dB of each hit's own peak, so the onsets did not move, B1's are identical to the
table below):

| Program | Hit 1 | Hit 2 | Hit 3 | Hit 4 | Hit 5 | Hit 6 | Hit 7 | Hit 8 | Spread |
|---|---|---|---|---|---|---|---|---|---|
| B1 | 0.56 / -16.58 | 0.65 / -16.76 | 0.60 / -16.23 | 0.62 / -16.93 | 0.56 / -16.30 | 0.60 / -17.07 | 0.56 / -16.91 | 0.54 / -16.56 | 0.10 ms, 0.84 dB |
| B2 | 0.56 / -19.29 | 0.67 / -18.97 | 0.71 / -19.52 | 0.67 / -19.63 | 0.73 / -19.39 | 0.56 / -19.77 | 0.62 / -19.08 | 0.62 / -19.39 | 0.17 ms, 0.80 dB |
| B3 (second audition) | 0.90 / -17.26 | 0.81 / -17.45 | 0.67 / -17.49 | 0.67 / -16.97 | 0.67 / -17.35 | 0.58 / -17.26 | 0.62 / -17.22 | 0.65 / -17.60 | 0.31 ms, 0.62 dB |
| B3 (third audition) | 0.90 / -18.76 | 0.81 / -18.95 | 0.67 / -18.99 | 0.67 / -18.47 | 0.67 / -18.85 | 0.58 / -18.76 | 0.62 / -18.72 | 0.65 / -19.10 | 0.31 ms, 0.63 dB |

Mean onset delays: B1 0.59 ms, B2 0.64 ms, B3 0.70 ms. The two-key headroom peaks are -9.58 (B1), -12.29 (B2) and
-10.27 dBFS (B3; -11.77 after the third audition). Goldens: `prog_a2_moving_pulse` and `prog_b2_dry_snare` are regenerated (A2: the glide; B2: +1.5 dB);
A5, A7, B4 and B7 are byte-identical, B1 and B3 have no golden. Each regenerated golden fails on a changed parameter
(A2 with `porta.time` 0.3, the old sound: cells over 3 dB 2.4 %, 245 % of the limit; B2 with `mix.level` 0: 94 % of
cells, level 3.2 dB).

### Third audition (2026-11-25): the hat 1.5 dB down

The owner's verdict (`docs/architecture.md` 8.21.2, "Third audition"): A2 is right; the hat goes down another 1.5 dB, B3's
offset becomes +2.0 dB, and with that the kit and the bank are accepted. Only `mix.level` of B3 moved, 4.2 to 2.7 dB (the table
and the content are unchanged), so every spectral figure of B3 and its onsets are those of the second audition. Measured on
macOS arm64, 2026-11-25 (`bside_metrics.tsv`, `Bank B1 to B3 in time ...`, `Bank B3 Tick Hat ...`):

| | offset | target peak | `mix.level` before / after | peak before / after dBFS | RMS before / after dB | deviation after | two-key chord peak before / after dBFS |
|---|---|---|---|---|---|---|---|
| B3 Tick Hat | +3.5 / +2.0 | -16.97 / -18.47 | +4.2 / +2.7 | -16.97 / -18.47 | -34.54 / -36.04 | 0.00 dB | -10.27 / -11.77 |

The level test, the in-time gate, the headroom check, the limiter-knee check and the silence check of B3 pass at the new level.
Percussion in time, B3 after the third audition (onset ms / peak dBFS; the onsets are those of the table below):

| Program | Hit 1 | Hit 2 | Hit 3 | Hit 4 | Hit 5 | Hit 6 | Hit 7 | Hit 8 | Spread |
|---|---|---|---|---|---|---|---|---|---|
| B3 | 0.90 / -18.76 | 0.81 / -18.95 | 0.67 / -18.99 | 0.67 / -18.47 | 0.67 / -18.85 | 0.58 / -18.76 | 0.62 / -18.72 | 0.65 / -19.10 | 0.31 ms, 0.63 dB |

Mean onset delay B3 0.70 ms (unchanged), reported tail 0.61 s. The loudest phrase peak of the bank stays B4's (-15.66 dBFS);
B3's is now below B1's (-16.23) and B6's (-15.87). The kit of the three drums: peaks -16.23, -18.97 and -18.47 dBFS, each on its target
plus offset (B1 0.26 dB short, the rest 0.00). The silence check of B3 measures -94.6 dB over its last 150 ms with the DC, -104.9 dB with the
mean removed, and a mean of -95.0 dBFS (see "The silence check of B3").

### Percussion in time (B1 to B3)

The gate (`Bank B1 to B3 in time ...`, `BankTests.cpp`): over the eight hits of the percussion phrase (0.25 s apart,
each held 0.2 s, rendered through the real processor with the limiter off), per hit the onset is the first sample
(of either channel) at or above -30 dB of that hit's peak, counted from the hit's note-on (the hit's window runs to
the next note-on), and the peak is that hit's sample peak. Every hit's onset within 1 ms of the others', every
hit's peak within 1 dB of the others', and the three drums' mean onset delays within 2 ms. Per hit, onset in ms /
peak in dBFS:

| Program | Hit 1 | Hit 2 | Hit 3 | Hit 4 | Hit 5 | Hit 6 | Hit 7 | Hit 8 | Spread |
|---|---|---|---|---|---|---|---|---|---|
| B1 before | 0.56 / -20.68 | 0.54 / -20.73 | 0.67 / -21.08 | 0.65 / -20.45 | 0.58 / -21.68 | 0.62 / -20.74 | 0.67 / -20.90 | 0.62 / -21.08 | 0.12 ms, 1.23 dB |
| B2 before and after | 0.56 / -20.79 | 0.67 / -20.47 | 0.71 / -21.02 | 0.67 / -21.13 | 0.73 / -20.89 | 0.56 / -21.27 | 0.62 / -20.58 | 0.62 / -20.89 | 0.17 ms, 0.80 dB |
| B3 before | 1.04 / -24.02 | **33.94** / -21.94 | 0.71 / -21.50 | **33.96** / -20.37 | 0.75 / -21.37 | **33.92** / -21.31 | 0.75 / -20.82 | **33.94** / -21.57 | 33.25 ms, 3.64 dB |
| B1 after | 0.56 / -17.78 | 0.65 / -17.96 | 0.60 / -17.43 | 0.62 / -18.13 | 0.56 / -17.50 | 0.60 / -18.27 | 0.56 / -18.11 | 0.54 / -17.76 | 0.10 ms, 0.84 dB |
| B3 after | 0.90 / -15.86 | 0.81 / -16.05 | 0.67 / -16.09 | 0.67 / -15.57 | 0.67 / -15.95 | 0.58 / -15.86 | 0.62 / -15.82 | 0.65 / -16.20 | 0.31 ms, 0.62 dB |

Mean onset delays after: B1 0.59 ms, B2 0.64 ms, B3 0.70 ms (the three drums within 0.11 ms); the largest single
onset is 0.90 ms (B3's first hit). These are the times from the note-on to the point where a hit reaches -30 dB
of its peak (attack 0).

**Diagnosis of B3 (the audition round).** Hits 2, 4, 6 and 8 began 33.9 ms after their note-on, hits 1, 3, 5,
7 at 0.7 to 1.0 ms: the envelope counter read back every millisecond stayed at 0 for 32 ms with the gate set and
then rose: 33.3 ms is 32 768 cycles, the wait of the envelope's rate counter. The hits that waited had come to the
note-on 42 ms after their voice went idle (idle at 258 ms, note-on at 300 ms), more than the full lead of
`[restart] mode classic, lead 2` (2 ticks of 19.95 ms = 39.9 ms), and `ProgramEngine::noteOn` cleared the envelope
counters under Clean only for a note without the full lead. With `lead 4` (79.9 ms) the same wait appeared on a hit
0.5 s (second hit) and 0.7 s (fourth hit) after the previous one (notes held 0.2 s, B3 with two oscillators). B2
(tick multiplier 2, lead 2 = 20 ms) showed it at spacings of 0.35, 0.5 and 0.7 s; B1 at no spacing from 0.125 to
1.0 s. The first hit of the old B3 (-24.02 dBFS) equals every hit when the hits are 1.0 s apart (the voice is back
at its power-on state), 3 dB below the hits that follow it in the phrase. For the audition B2 and B3 were given
`[restart]` `mode off` (a note on a voice with no restart state was always cleared).

**The mechanism, proven in the engine (S4b, after the audition).** The round above inferred that the restart
state, once older than the lead, should leave the envelope ready and that only the missing clear was at fault. An
engine-and-chip test (`ProgramEngineChipTests.cpp`, "Clean never waits: the mechanism") shows why the lead cannot
help: the restart state's `sr 00` (release rate 0, match position 8) keeps the idle voice's rate counter looping over
positions 0 to 8 for as long as it is idle (a ten-cycle loop: 8 is held one more cycle with its reset pending), so
an old restart state is not a fixed state. The note's gate edge starts the envelope's two-step phase pipeline,
whose first step sets the compare to the **decay** match (decay 6: 266) and whose second sets the attack match
(attack 0: 8). When the note's cycle finds the counter AT 8 with its reset not yet pending, the first step moves it
to 9 under the decay compare, the second sets the compare to 8 with the counter at 10, and the counter runs through
all 32 767 positions before it meets 8 again: the attack's first step comes 32 771 cycles after the note instead of
fewer than 16 (12 with the clear; both models, leads 2, 4 and 8 alike; one phase in ten). It needs an attack-0 tone (a longer attack's
match is above the loop) whose decay is not 0. The writes that matter are the note's gate (its pipeline), on the
note's cycle; the counter's phase at that cycle depends only on how long the voice has been idle, which the spacing
of the hits sets. A clear on the note's cycle before tick 0's writes therefore cures it (the counter starts at 0
and is at 2 when the attack match arrives): the contract's remedy is right, the inferred cause ("the lead was
thought to suffice") is what was wrong.

**The fix: Clean never waits (8.21.2).** Under Clean the engine clears the envelope counters of all three
oscillators at every note start, full lead or not (`ProgramEngine::noteOn`; Precondition and Authentic unchanged).
The gate (`Clean never waits: every hit starts as soon as the first ...`, `bside_unit`, engine and chip, digital
only, about 2.8 s): restart modes off, classic and phase, leads 1 to 8, spacings 0.125, 0.25, 0.35, 0.5, 0.7 and
1.0 s, Models A and B, two attack-0 tones (B3's hat, a host-envelope pulse), five hits each (576 cases): every
onset within 1 ms of the first hit's. Before the fix the largest difference was 33.25 ms (64 cases: the hat at
0.7 and 1.0 s in modes classic and phase); after it 0.10 ms. The former hidden test (B3's old restart section, two
hits 0.5 s apart) is now a default test: 33.04 ms before, 0.90 / 0.67 ms after.

**B2 back to its drafted restart section** (`mode classic`, `lead 2`, as on commit 7d78b61; the owner kept it as
drafted). Six hits held 0.2 s at each spacing, through the processor (`Bank B2 in time at wider spacings`, a
default test), onset ms per hit:

| B2, spacing | Hit 1 | Hit 2 | Hit 3 | Hit 4 | Hit 5 | Hit 6 | Largest difference |
|---|---|---|---|---|---|---|---|
| 0.35 s, engine before the fix | 0.56 | 0.65 | 0.65 | **33.83** | 0.65 | 0.69 | 33.27 ms |
| 0.35 s, after | 0.56 | 0.65 | 0.65 | 0.58 | 0.65 | 0.69 | 0.12 ms |
| 0.5 s, engine before the fix | 0.56 | 0.71 | **33.83** | 0.67 | 0.65 | 0.69 | 33.27 ms |
| 0.5 s, after | 0.56 | 0.71 | 0.73 | 0.67 | 0.65 | 0.69 | 0.17 ms |
| 0.7 s, engine before the fix | 0.56 | **33.81** | 0.67 | 0.60 | 0.75 | 0.69 | 33.25 ms |
| 0.7 s, after | 0.56 | 0.65 | 0.67 | 0.62 | 0.75 | 0.69 | 0.19 ms |

The percussion phrase's B2 line above is unchanged by the restored section (the same eight onsets and peaks), and
so is B2's render golden (every feature 0.000 from the reference). B3 keeps `mode off` (the owner's audition
verdict). Under Precondition and Authentic a note does not clear the counters (8.6.4), so the wait can still
happen there; the drums were not measured under them.

**Peaks.** The peak of a noise hit depends on which values of the noise register fall in its loudest few
milliseconds, so it varies from hit to hit by an amount that depends on the content (B3 with `=C7`, cutoff 1500,
two oscillators: 3.64 dB spread; B1 with its previous table: 1.23 dB) and on the spacing of the hits. The values
above are what the gate phrase gives with the content now in the bank; they are deterministic (the render is),
and the bank was chosen among variants for a peak spread under 1 dB with some margin (B3 0.62 dB, B1 0.84 dB, B2
0.80 dB). At other spacings of the same hits (re-measured in the review, `[.s4b-measure]`: spacings 0.125, 0.15, 0.2, 0.25, 0.3,
0.4, 0.5, 0.7 and 1.0 s, notes held 0.8 of the spacing and, from 0.3 s on, also 0.2 s; B3 at its level after the third audition) the peak
spread is 0.83 to 2.14 dB for B1 (2.14 dB at 0.2 s held 0.16 s; 0.84 dB at 0.25 s), 0.75 to 1.47 dB for B2 (and 0 at 1.0 s held 0.2 s) and
0.62 to 1.58 dB for B3 (1.58 dB at 0.15 s; 0 at 1.0 s). The hold changes the figure only for B2 at 1.0 s (1.06 dB held 0.8 s, 0 held 0.2 s).
The earlier statement (0.8 to 1.9 dB for B1, 0.7 to 1.5 dB for B2, 0.7 to 1.6 dB for B3) missed the 0.2 s spacing for B1. The onset spread
over the same cases stays at or below 0.90 ms (B3 at 0.125 s), inside the 1 ms bound; B1's and B2's are 0.62 ms or below; only the phrase of the contract is gated.

**The gate can fail.** On the previous bank the same test reported the B3 line above (onset spread 33.25 ms, peak
spread 3.64 dB; it also failed B1's 1 dB peak bound with 1.23 dB), and a second default test feeds the measurement
a synthetic late hit (34 ms) and a weak one (3 dB) and checks that it sees both.

**The silence check of B3.** `Bank B3 Tick Hat` asks that the audio of the last 150 ms before the key goes up
(350 to 500 ms after the note-on) be below -100 dB. The measure is the RMS of that window with its mean
removed, the threshold the same -100 dB. Reason (a measurement): the window holds a constant DC, the slow tail (the
board's 1.2 Hz high-pass: e-fold every 0.13 s) of the DC that the envelope pushed through during the hit; it grows with the
drum's level and with the number of oscillators (B3's DC residual in the window: -111.9 dB with one oscillator, -99.9 dB with two
and -92.9 dB with three at the same cutoff, against peaks of -25.8, -19.8 and -16.4 dBFS), and it had been -107.2 dB at the level
before the owner's audition. At the level of the second audition (`mix.level` 4.2) the window measured -102.0 dB with the mean
removed and -91.7 dB with it; at the third audition's (2.7) it measures -104.9 dB and -94.6 dB, and the mean is -95.0 dBFS.
Nothing in the window moves.

With the mean removed the RMS would pass any constant offset (review, W1). The same test therefore also asks that the absolute value of the
window's mean (the larger of the two channels) be below -80 dBFS (the DC tail is 15 dB under that). It can fail: with a constant 5.0e-4
added to the left channel before the measure (a temporary edit, reverted), the RMS check passed (-104.9 dB) and the mean bound failed
(-66.3 dBFS, against -80).

## The measuring and rendering tool

`tests/bside/integration/BankSound.h` holds the phrases, the measurements and the register probe; the
enforcing tests (`BankTests.cpp`) and the hidden render (`BankRender.cpp`) both use it, so both measure the
same thing. It includes nothing of Vintage (Vintage's figure enters as a documented constant, below).

```
VS_RENDER_DIR=<dir> build/<dir>/tests/bside/BSideTests "Render bside_bank*"
```

(hidden, `[.render]`) writes one WAV per program (`<slot>-<name>.wav`, the phrase plus up to 4 s of release, 48
kHz, 24 bit), `bside_audition.wav` (all 16 with 0.4 s of silence between them), `bside_index.txt` (start
time of each program) and `bside_metrics.tsv` (per program: `mix.level`, peak, RMS and its deviation from
the group median, for percussion the peak's from the sustained median peak, spectral centroid, the chord's
peak and RMS (for percussion the two-key chord's), the reported tail, the phrase and chord
peaks with the limiter on, and the energy of nine octave bands, dB re the sum, 0 to 125 Hz ... above 16 kHz).
Nothing is written without `VS_RENDER_DIR`, and never into the repository. With `VS_BANK_DIR=<folder>` the
render reads program files from that folder (named like the factory files) instead of the embedded bank, so a
draft can be measured without rebuilding. The audition files are rendered with the limiter on at its
defaults, as a new instance plays; the table is also printed to the console.

## The `kChipGain` calibration (8.21.2)

`kChipGain` (`source/bside/engine/Part.h`) is the one gain between the chip's units and the part's output.
8.21.1 calibrated it so that one note of Init at velocity 1 peaked at -12 dBFS at the part's output on Model
B. 8.21.2 moves it once, to the rule: B-Side's Init, on the Chord phrase (C4 E4 G4 C5 held 3 s, velocity
100), measures within 1 dB of Exitium Vintage Synth's Init on the same phrase, unweighted RMS of both
channels from the first note-on to the last note-off, so the two instruments sit at the same loudness in a
session; the eight-note chord's headroom still holds, and if both could not, the headroom would win.

How both were measured: the real plugin processor, 48 kHz, real-time path, block size 256, velocity 100,
output limiter off, master volume at its default (-6 dB), the fixed output trim (-6.02 dB), unweighted RMS of
both channels.

| | Vintage's Init | B-Side's Init, old `kChipGain` 0.9739 | B-Side's Init, new `kChipGain` 1.2822 |
|---|---|---|---|
| Chord phrase, RMS | -23.530 dB | -25.920 dB | -23.531 dB |
| Chord phrase, peak | -10.660 dBFS | -19.218 dBFS | -16.829 dBFS |
| Eight-note chord at velocity 127, peak | -6.993 dBFS | -12.500 dBFS | -10.111 dBFS |
| Eight-note chord, RMS from 0.55 s | -20.535 dB | | -19.494 dB |

Vintage's figures come from its own tooling, unchanged: `FactoryRig (0, false)`, `phraseFor`, `measure` of
`tests/preset_unit/FactorySound.h`, as the hidden `Render m6b_factory audition and measurements` runs them (its
printout, with three decimals for this measurement: `A1 Init ... peak -10.660, rms -23.530, chord_peak -6.993,
chord_rms -20.535`, 2026-11-24, macOS arm64). They enter B-Side's tests as a documented constant,
`kVintageInitChordRmsDb = -23.530` in `PartTests.cpp`, never through an include.

The move is x1.3166 (+2.39 dB): 0.9739 to 1.2822. The new gain lands 0.001 dB from Vintage's figure (the rule
asks for 1 dB). Headroom: B-Side's eight-note chord on Init at full velocity peaks at -10.11 dBFS (10.1 dB
below full scale) through the processor, and at +8.45 dBFS at the part's own output, before the master volume
and the trim (+6.06 dBFS before the move; the part's output was never limited to 0 dBFS: the master volume and
the trim are what bring it below). Both conditions hold, so the headroom did not have to win.

Assertions moved by the new gain (every one is a threshold measured at the part's output that scales with
the gain; none was loosened beyond what the gain moves):

- `PartTests.cpp`, "Part: one note of the Init program at velocity 1 peaks at -12 dBFS +-0.5 dB on Model B
  (kChipGain)": rewritten to the rule of 8.21.2 (Init's Chord phrase within 1 dB of the Vintage constant, and the
  full-velocity eight-note chord below 0 dBFS through the processor), in the same commit as the gain.
- `PartLevelTests.cpp`, the fingerprint of the 0 dB render (macOS arm64): regenerated,
  `0x1eec9dec60bde539` to `0xcc5c9c3f74e56c40`.
- `PartTests.cpp`, "Part Shared: a silent idle voice leaves ...": `leftAfter[1] <= leftAfter[0] + 5` became `+ 8`.
  The quiet level is -120 dB at the part's output (`kQuietLevel`), so +2.39 dB of gain asks 2.4 dB more decay
  of the voice's own signal: the Shared voice leaves 9 ms after going idle (Per Note 1 ms); the bound was met
  at 6 ms or less before.
- `PartReviewTests.cpp`, "Review: random parameters every block ...": `worst < 4.0` became `worst < 5.3`
  (4.0 x 1.3166 = 5.27); measured peaks 4.81, 5.01 and 5.17 at the three host configurations.
- `host_smoke` (B-Side windows only, `tests/host_smoke/HostSmoke.cpp`): the note's peak window 0.045 .. 0.072
  became 0.059 .. 0.095 (x1.3166). Program 2 of the test is now A3 Synced Climb rather than Init, and its note
  at velocity 0.8 peaks at 0.0778 (44.1, 48 and 96 kHz: 0.0796, 0.0778, 0.0780); the names expected for programs
  1 and 2 became "Moving Pulse" and "Synced Climb".

Changed by the bank, not by the gain: `ShellTests.cpp`, "B-Side: 64 programs, all Init ...", now expects the
16 designed names and Init from slot 16 on.

## Where the specification could not be met as written

- **A3, "the sweep macro moving the synced pitch".** `macro.sweep_depth` scales only the `sweep` and
  `pingpong` steps of the pulse and filter lanes (8.7, `ProgramEngine::stepSweep`); the wave lane has no pitch
  sweep command, and no macro moves a lane's pitch. What is built is the closest thing: the wave lane of
  oscillator 2 steps its own pitch row by row (one semitone every two ticks, up 21 and down 20), so the synced
  pitch moves by the table, not by the macro. What is missing is a pitch sweep in the wave lane (a command that
  adds a number of semitones per tick, scaled by `macro.sweep_depth`) if the macro is meant to move it.
- **B3**'s "a longer open variant on velocity above 100 is not required" is not built.

## Applying a program while notes sound

A program applied while keys are held fades the sounding voices of each part whose parameters it changes over
3 ms and makes them idle **before** the new program's parameters take effect, as a structural switch does
(`docs/architecture.md` 8.21.2 after the drafting; `Part.h`, "PROGRAM APPLY"). Held keys do not retrigger; a key
pressed during the fade starts when it ends, with the new program, and keeps its length. The part recognises an
apply by its committed table: every program, tone and state apply publishes the part's table inside the hold
that writes its parameters, so a new table generation arriving with changed parameters is an apply; a
parameter moved by hand or by automation, the pedal, All Notes Off, a key-mode change and a table set by hand
(which changes no parameter) never fade the part.
**A waveform parameter changed by hand on a held note is a register write and steps the output at once, as on
the chip: that is a property of the instrument, not a defect.**

**A program that differs from the sounding one only in its table** (its parameters equal the ones in force; for example A7's parameters
with A8's table) is not faded (`docs/architecture.md` 8.21.2, "An apply that changes only the table", review finding D1). The shell
releases the held notes as for any preset change; they end in the old table's release (A7's release lane: `gate off`), and the new table
plays from the next note. Measured (`S4bReviewTests.cpp`, a default test): `appliesFaded()` stays 0; the voice is still active 5 ms after
the apply and idle by 1 s; the largest step after the apply is within Vintage's rule (3 x the largest before + 0.002); a note played after it
steps through the new table's semitones (A8's `0 3 7 10 12 ...`, the same as an instance that had the variant from the start), not the old
one's (`0 4 7 12 ...`).

Measured with three keys held (C3 G3 E4, velocity 100) and the change 0.25 s later; the largest sample step in
the 9 600 samples before the change and in the 4 800 after it, against Vintage's rule (after at most 3 x before
+ 0.002; `PresetManagerTests.cpp`, "Applying a program while notes sound releases them without a click"):

| Change | Step before | Step after, before the fade was built | Step after, now |
|---|---|---|---|
| Cutoff 300 to 1500 by hand (Init, triangles) | 0.00205 | 0.00200 | 0.00200 (holds) |
| Waveform Triangle to Pulse by hand | 0.00205 | 0.02241 | 0.02241 (steps: the property) |
| Each of the 16 programs applied over Init in Dual (triangles, cutoff 300, Lower hard left, Upper hard right), per part | 0.00195 to 0.00205 | up to 0.06685; 29 of the 32 parts break the rule | 0.00078 on every part (holds) |
| Each program applied over the one before it (A1 over B8, ...), per channel | 0.0004 to 0.088 | within the rule | within the rule |

The tests are the `[apply]` cases of `BankTests.cpp`: the step rule for all 16 programs in both directions,
the waveform property (it steps, no fade, the notes keep sounding), a key pressed during the fade, a tone
applied to one part (the other part bit-identical whatever the tone), and block lengths 1, 61 and 512.

## Run time

Since the faster tests (2026-11-26) the slower test cases of this step are tagged `[slow]`: they run in
the full tier, not in the smoke tier. The times below were measured before that.

The default `BSideTests` run took 76 s on this Mac before this step (1:16, `time`, arm64 Release, no other
build running) and 93 s after (+17 s; the limit is +20 s): the level test 12.3 s, the fifteen program tests
0.1 to 0.6 s each (about 4 s together), the bank test 0.1 s, the rewritten level test of `PartTests.cpp`
about 0.1 s. Nothing is hidden behind a tag except the audition render. `bside_unit` is unchanged (about 250 s
with `--durations yes`; its five slowest cases are 38.5, 28.0, 22.8, 17.6 and 13.2 s).

After the drafting (the silent apply and the level groups): 92.1 s before and 93.2 s after (`time`; the sum
of `--durations yes` 91.9 s and 93.2 s). The five `[apply]` cases take 1.2 s together (the step rule for
all 16 programs in both directions 0.33 s: two processors, each used for all 16); the level test stays at
about 12 s (the two-key chord of the drums replaces their eight-note chord).

After the owner's audition (2026-11-25): the default `BSideTests` run took 94.0 s (`time`, `--durations yes`; 93.2 s
before this round): the level test 12.0 s, the percussion-in-time gate 0.36 s, the synthetic check of the gate
under 0.01 s, the rewritten B1 test 0.63 s (it renders the earlier table too), B3 0.21 s. The hidden known-defect
test and the audition render are not in the default run.

Clean never waits (S4b, after the audition; one binary, run with and without the new cases, `time`, macOS arm64
Release): `BSideTests` 94.9 s without and 96.5 s with them (the hat test, formerly hidden, 0.18 s; B2 at three
spacings 0.43 s); `bside_unit` 256.0 s without and 255.4 s with them (the noise of the machine; `--durations yes`
gives the gate 2.83 s and the mechanism test 0.04 s). The default B-Side run grows by about 3.5 s.

## Deliberate exclusions

- The shell's arpeggiator and step sequencer are off in every program: A7 and A8 show the chip technique (the
  wave lane), not the shell's.
- No effects: no program uses the chorus, delay or reverb, so the level rule measures the instrument.
- Tables that loop end their wave lane at the note-off (`release:` with `gate off` and `end`), so a released
  voice goes idle once its envelope has run out and the part does not keep stepping a silent arpeggio.
- Slots C1 to H8 are Init until a later bank step.

## Coverage

Computed from the files (each row: programs that use the feature in a part that sounds).

| Feature | Programs |
|---|---|
| Model A | B4 B5 B7 (Upper) |
| Shared topology | B5 |
| Dual / Split | B7 / B8 |
| Solo | A2 A5 A6 B8 |
| Legato glide | A2 B8 |
| Hard sync | A3 |
| Ring modulation | A4 |
| Pulse lane (set, ping-pong) | A2 A7 A8 B2 B7 B8 |
| Wave lane pitch rows | A2 A3 A4 A6 A7 A8 B1 B2 B3 B7 B8 |
| Wave lane absolute pitch | B1 B2 B3 |
| Filter lane (sweep, set) | A5 B6 B8 |
| Hard restart section | B1 B2 |
| Restart section with `mode off` (no restart state) | B3 |
| Tick rate Tempo | A7 A8 |
| Tick multiplier above 1 | A8 B1 B2 |
| Two oscillators doubled in phase (the level) | B1 B2 |
| Three oscillators doubled in phase (the level) | B3 |
| Velocity to cutoff | A5 B8 |
| Noise waveform | B1 B2 B3 B6 |
| High-pass / band-pass / low-pass | B3 / B6 / A2 A3 A5 A7 A8 B4 B5 B7 B8 |
