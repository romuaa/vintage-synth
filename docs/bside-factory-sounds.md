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
| A2 | Moving Pulse | Lead | W | Pulse lead, Solo with Legato glide (`porta.time` 0.3). The pulse lane sets the width to 768 and ping-pongs it between 768 and 3328 at 40 per tick; the wave lane holds the key's pitch for 15 ticks and then alternates it by +-0.2 semitone every two ticks | Hold one key for two seconds and follow whether the tone changes while you hold it; play two keys legato and follow the pitch between them |
| A3 | Synced Climb | Lead | W | Hard-sync lead: oscillator 2 (sawtooth) synced to oscillator 1, which is silent (waveform Off) and runs at the key's pitch as the sync source. The wave lane of oscillator 2 steps its pitch from +3 to +24 semitones above the key and back down, one semitone every two ticks | Hold one key for two seconds and follow how the pitch relation between the two oscillators moves |
| A4 | Glass Ring | Lead | W | Triangle on oscillator 1, ring-modulated by oscillator 3 for the first ten ticks (the wave lane sets the ring bit, then clears it); oscillator 3 is a silent ring source (a triangle with no gate) whose pitch the wave3 lane drops from +10.5 to +8.5 to +6.5 semitones above the key. The envelope decays from 15 to a sustain of 11. Not through the filter | Play one key and compare its first quarter second with the rest of the held note |
| A5 | Closing Door | Bass | W | Filter bass: sawtooth plus a pulse an octave down through the low-pass (resonance 9, base cutoff 1800); the filter lane sweeps the cutoff down by 75 per tick for 20 ticks and stops; `vel.cutoff` 0.6 lowers the cutoff for softer keys; Solo | Play the same low key softly and hard and compare how open the filter starts |
| A6 | Blip Bass | Bass | W | Pulse bass, width 512 (12.5 %), no filter (oscillator 1 is not routed through it), Solo. The wave lane plays the first tick one octave up (+12) and then the key's pitch | Play a low key and listen to its first twenty milliseconds |
| A7 | Major Steps | Arpeggio | W | Chip arpeggio in the wave lane (the shell's arpeggiator is off): one key steps +0 +4 +7 +12 +7 +4 semitones, one pitch per tick, repeating; tick rate Tempo, 12 ticks per beat, multiplier 1; pulse width 25 %. The note-off clears the gate and ends the lane | Hold C4 for four seconds, then change `arp.tempo` and play it again |
| A8 | Minor Steps | Arpeggio | W | The minor counterpart: +0 +3 +7 +10 +12 +10 +7 +3, tick multiplier 2 (twice the rate of A7), and the pulse lane ping-pongs the width between 640 and 3200 at 128 per tick | Hold C4 for four seconds and compare the pace and the width movement with A7 |
| B1 | Short Kick | Percussion | W | One noise tick (tick 0, `=C7`) and then a triangle whose pitch falls by table `=64 =55 =48 =43 =40 =38` over six ticks of 9.98 ms (tick multiplier 2); absolute pitches, so every key plays the same drum; decay 8, sustain 0; oscillator 2 doubles oscillator 1 in phase (the level) | Play three different keys and compare the drum |
| B2 | Dry Snare | Percussion | W | Noise and pulse alternate by table (`noise =D7`, `pulse =G3`, `noise =C7`, `pulse =E3`, then `noise =B6` held, the gate cleared at tick 10), pulse width 1536, tick multiplier 2; absolute pitches; oscillator 2 doubles oscillator 1 in phase (the level) | Play three different keys and compare the drum |
| B3 | Tick Hat | Percussion | W | Noise at `=C7` through the high-pass (cutoff 1500), decay 6, sustain 0: decay only; oscillator 2 doubles oscillator 1 in phase, also through the high-pass (the level) | Play eight hits and follow how long each one rings |
| B4 | Slow Saw Pad | Pad | W | Model A, Per Note: three sawtooths detuned by -14, 0 and +14 cents through the low-pass (cutoff 900, resonance 5); attack 9, sustain 12, release 9 | Play a four-note chord and follow how the sound arrives |
| B5 | Shared Chord Pad | Pad | W | Model A, Shared: a sawtooth and a 35 % pulse (+7 cents) of every note into one low-pass (cutoff 1100, resonance 6); attack 8, sustain 12, release 9 | Play the four-note chord in this program and in B4 and compare how the chord sounds against the single notes |
| B6 | Band Sweep | Sound FX | W | Noise through the band-pass (resonance 10); the filter lane sets the cutoff to 100 and adds 16 per tick for 120 ticks (to 2020); attack 5, release 11 (2.4 s nominal) | Hold a key for three seconds and then let go, and follow the sweep and the end |
| B7 | Pulse Over Pad | Dual | D | Lower: the pulse lead of A2 without the glide (Model B, pan -0.6); Upper: a Model A pad of two sawtooths at -10 and +10 cents (pan +0.6); both parts play every key | Play a chord and follow which side each layer is on |
| B8 | Bass And Lead | Split | S | Lower (below G3): the bass of A5; Upper (G3 and above): the lead of A2; both Solo | Play low keys with one hand and a melody above G3 with the other |

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

**Level rule** (enforced over the embedded bank by `BankTests.cpp`, one test case over the 16 programs):

1. Loudness = unweighted RMS of both channels from the first note-on to the last note-off of the standard
   phrase. Each sustained and bass program is within 2 dB of its group's median RMS. **Percussion is judged
   by its peak:** a drum's phrase peak is within 2 dB of the median phrase peak of the sustained group, so a
   drum hit sits as loud as a lead note.
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
`Bank B2`, `Bank B3`).

### Measured figures

From `bside_metrics.tsv` of the hidden render (below), macOS arm64, 2026-11-24. `mix.level` is Lower / Upper
in dB. Phrase RMS and peak with the limiter off. The chord is the eight-note chord of rule 2, for percussion
the two-key chord (its RMS from 0.55 s says nothing for a drum, whose hits are over by then). The deviation
is from the group's median RMS, for percussion from the sustained group's median peak.

| Slot | File | Name | Category | Group | `mix.level` | Phrase RMS dB | Phrase peak dBFS | Deviation dB | Chord peak dBFS | Chord RMS dB | Reported tail s | Phrase peak, limiter on |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A1 | A1-init | Init | Init | reference | 0 | -23.53 | -16.83 | | -10.11 | -19.49 | 0.61 | -16.83 |
| A2 | A2-moving-pulse | Moving Pulse | Lead | sustained | -1.5 | -31.97 | -25.71 | +0.05 (RMS) | -24.67 | -30.49 | 0.50 | -25.71 |
| A3 | A3-synced-climb | Synced Climb | Lead | sustained | +2.2 | -32.05 | -20.37 | -0.03 (RMS) | -8.53 | -22.54 | 0.50 | -20.37 |
| A4 | A4-glass-ring | Glass Ring | Lead | sustained | +4.3 | -31.99 | -20.47 | +0.03 (RMS) | -11.28 | -22.67 | 0.72 | -20.47 |
| A5 | A5-closing-door | Closing Door | Bass | bass | -1.2 | -33.52 | -21.53 | -0.01 (RMS) | -20.81 | -31.32 | 0.34 | -21.53 |
| A6 | A6-blip-bass | Blip Bass | Bass | bass | +1.6 | -33.51 | -24.02 | +0.01 (RMS) | -22.85 | -32.77 | 0.22 | -24.02 |
| A7 | A7-major-steps | Major Steps | Arpeggio | sustained | -1.3 | -32.03 | -25.62 | -0.02 (RMS) | -10.88 | -21.49 | 0.34 | -25.62 |
| A8 | A8-minor-steps | Minor Steps | Arpeggio | sustained | -2.2 | -31.99 | -26.89 | +0.03 (RMS) | -10.18 | -20.22 | 0.34 | -26.89 |
| B1 | B1-short-kick | Short Kick | Percussion | percussive | +1.9 | -34.94 | -20.45 | -0.02 (peak) | -13.68 (two keys) | -104.41 | 0.61 | -20.45 |
| B2 | B2-dry-snare | Dry Snare | Percussion | percussive | +1.7 | -34.04 | -20.47 | -0.03 (peak) | -13.79 (two keys) | -89.56 | 0.61 | -20.47 |
| B3 | B3-tick-hat | Tick Hat | Percussion | percussive | +5.6 | -40.38 | -20.37 | +0.06 (peak) | -17.02 (two keys) | -119.48 | 0.61 | -20.37 |
| B4 | B4-slow-saw-pad | Slow Saw Pad | Pad | sustained | -6.1 | -32.01 | -18.66 | +0.01 (RMS) | -13.81 | -29.46 | 2.25 | -18.66 |
| B5 | B5-shared-chord-pad | Shared Chord Pad | Pad | sustained | -2.4 | -32.04 | -18.60 | -0.02 (RMS) | -18.18 | -43.37 | 2.25 | -18.60 |
| B6 | B6-band-sweep | Band Sweep | Sound FX | sustained | +2.7 | -32.02 | -15.87 | -0.01 (RMS) | -12.26 | -28.60 | 7.20 | -15.87 |
| B7 | B7-pulse-over-pad | Pulse Over Pad | Dual | sustained | -7.5 / -7.5 | -31.97 | -20.40 | +0.05 (RMS) | -15.49 | -28.62 | 2.25 | -20.40 |
| B8 | B8-bass-and-lead | Bass And Lead | Split | sustained | -4.0 / -4.0 | -32.03 | -21.22 | -0.01 (RMS) | -21.62 | -30.65 | 0.50 | -21.22 |

Targets: sustained median RMS -32.02 dB, bass median RMS -33.51 dB, sustained median phrase peak -20.44 dBFS
(the percussion target; the mean of A4's -20.47 and B7's -20.40, the 5th and 6th of the ten sustained peaks).
Loudest phrase peak: B6, -15.87 dBFS (the limiter's knee is at -4 dBFS). Highest eight-note chord peak of the
designed programs: A3, -8.53 dBFS, 1.42 dB inside the -7.11 dBFS limit (Init's own is -10.11 dBFS); then A8
(-10.18) and A7 (-10.88). A3's chord is the sum of eight sawtooths whose accumulators all start at the same
cycle, so their peaks coincide. Before this rule the drums and the arpeggios were trimmed 13 dB below the
leads by that eight-note chord (eight identical absolute-pitch hits, in phase); with two keys a drum peaks at
-13.7 dBFS or below.

The percussion rule can fail: B1 with `mix.level` 4 dB higher (+5.9) fails the level test with
`Short Kick [percussive]: phrase peak -16.4535 dB, target (the sustained group's median peak) -20.4379 dB,
deviation 3.98439 dB`.

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
  oscillator 1: constant for ticks 0 to 14, then in cents against the key `+19.7 +19.7 -20.3 -20.3` repeating
  (the register's step is 0.4 cent) from tick 15. Assign mode Solo and `porta.time` 0.3 read from the
  parameters the audio thread used; with a second key pressed over the first (C4 then G4) the pitch passes
  through 251 one-millisecond blocks between the two keys (0.4 to 0.9 s after the second note-on), against 0
  with `porta.time` 0; it ends 7.08 semitones above C4 (0.2 + 7 = the table's last vibrato step).
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
- **A6** (C4, 0.3 s). Oscillator 1 frequency: 523.24 Hz at tick 0, 261.62 Hz from tick 1 (the blip lasts
  19.0 ms, one tick being 19.95 ms, to within the 1 ms of the read); pulse width register 512 (12.5 %);
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
  per tick: `2093.0 329.6 196.0 130.8 98.0 82.4 73.4 73.4 ...` Hz (the fall is -1763, -133.6, -65.2, -32.8,
  -15.6, -9.0 Hz per tick, then none); the same for both keys. Envelope registers `0x08 0x08`. Oscillator 2's
  seven registers and envelope counter equal oscillator 1's at every read (the doubling).
- **B2** (key 48, 0.3 s). Oscillator 1 control per tick: `81 41 81 41 81 81 81 81 81 81 80 80 80 80` (noise,
  pulse, noise, pulse, noise ... then the gate cleared at tick 10); pitches `=D7 =G3 =C7 =E3 =B6`; pulse
  width 1536. Oscillator 2 equals oscillator 1 at every read (the doubling).
- **B3** (key 48, 0.5 s). Control `0x81`, F at `=C7` (2093.0 Hz), envelope registers `0x06 0x06`, mode High (4),
  oscillators 1 and 2 routed, oscillator 2 equal to oscillator 1 at every read (the doubling). The envelope
  counter peaks at 253 at 3 ms, is at or below 128 at 37 ms, at or below 16 at 120 ms and 0 at 208 ms (the key
  goes up at 500 ms); the audio's RMS in the 150 ms before the key goes up is -107.2 dB (-118.8 dB before the
  level was raised by 11.6 dB; the test's bound is -100 dB). 99 % of the energy of the note (0.05 to 0.3 s) is
  at or above 4 kHz (centroid 13 730 Hz).
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

The default `BSideTests` run took 76 s on this Mac before this step (1:16, `time`, arm64 Release, no other
build running) and 93 s after (+17 s; the limit is +20 s): the level test 12.3 s, the fifteen program tests
0.1 to 0.6 s each (about 4 s together), the bank test 0.1 s, the rewritten level test of `PartTests.cpp`
about 0.1 s. Nothing is hidden behind a tag except the audition render. `bside_unit` is unchanged (about 250 s
with `--durations yes`; its five slowest cases are 38.5, 28.0, 22.8, 17.6 and 13.2 s).

After the drafting (the silent apply and the level groups): 92.1 s before and 93.2 s after (`time`; the sum
of `--durations yes` 91.9 s and 93.2 s). The five `[apply]` cases take 1.2 s together (the step rule for
all 16 programs in both directions 0.33 s: two processors, each used for all 16); the level test stays at
about 12 s (the two-key chord of the drums replaces their eight-note chord).

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
| Hard restart section | B1 B2 B3 |
| Tick rate Tempo | A7 A8 |
| Tick multiplier above 1 | A8 B1 B2 |
| Two oscillators doubled in phase (the level) | B1 B2 B3 |
| Velocity to cutoff | A5 B8 |
| Noise waveform | B1 B2 B3 B6 |
| High-pass / band-pass / low-pass | B3 / B6 / A2 A3 A5 A7 A8 B4 B5 B7 B8 |
