# Factory bank: sound specification (M6b)

The 64 programs compiled into the plugin (`presets/factory/<slot>-<name>.json`, embedded at build
time; contract: `docs/architecture.md` 6b). This file is the written specification the programs
were drafted from, the rules they are measured against, and the proof that the bank covers the
instrument. The owner auditions the bank (M6 audition gate); the numbers only show that each
program does what its line says, not that it sounds good.

All names are our own. No name refers to a product, preset, artist, song or brand; the
repository's hashed name check (`scripts/preflight/docs-check.sh`) covers `presets/` and this file.

## Layout

| Group | Theme |
|---|---|
| A | Init, then brass: the 80s poly brass section, from soft swells to stabs |
| B | Strings and pads: slow strings, hollow and glassy pads, filter-swept and inverted-envelope pads |
| C | Keys, plucks and bells: chorus keys, plucks, cross-mod and sync bells, mallets |
| D | Basses: poly, Solo and Unison basses, resonant, PWM, sync, sub |
| E | Leads: sync, cross-mod, glide, unison, whistle, siren, Lo Freq wobble, thin pulse |
| F | Organ-like, percussive and sound effects |
| G | Dual layers (G1–G4) and Splits (G5–G8, bass below G3, other part above) |
| H | Arpeggiator (H1–H4), step sequencer (H5–H6), effect showcases (H7–H8) |

## Programs

Key mode: W = Whole, D = Dual, S = Split (split point G3, MIDI 55). Assign mode per part: Poly
unless named. Ranges and tapers are in `docs/decisions.md`; IDs in `PartParamList.h`.

| Slot | Name | Category | Key mode | Musical intent | Shows |
|---|---|---|---|---|---|
| A1 | Init | Init | W | Every parameter at its default: the starting point and the level reference | defaults |
| A2 | Copper Fanfare | Brass | W | Bright two-saw brass with a filter "blat" on each chord, a little chorus | filter envelope, velocity → VCF, chorus I |
| A3 | Low Sun Horns | Brass | W | Darker, slower horn section an octave lower, slightly unstable | 16' VCO-1, Vintage, reverb |
| A4 | Harbour Stabs | Brass | W | Short brass stabs that drop to a low sustain, roomy | fast filter decay, reverb |
| A5 | Reed Pulse Brass | Brass | W | Reedy brass with a slowly moving pulse width | PWM (LFO), chorus I |
| A6 | Summit Horn | Brass | W, Unison 4 | Big mono brass lead with legato glide and delayed vibrato | Unison, Legato portamento, LFO delay, delay |
| A7 | Velvet Horns | Brass | W | Soft, round horns; the filter follows the amplifier envelope | VCF from ENV-2, −12 dB, ENV-1 key follow, chorus II |
| A8 | Bright Parade | Brass | W | Brass with an octave-up VCO-2 and a pitch "scoop" on VCO-1 | VCO-2 4', VCO pitch from ENV-1 |
| B1 | Slow Tide Strings | Strings | W | Slow-attack string ensemble, wide chorus, hall | slow envelopes, HPF, vibrato, chorus II |
| B2 | Winter Ensemble | Strings | W | Pulse strings with delayed vibrato, drifting | PWM (LFO), LFO delay, Vintage, chorus I |
| B3 | Glass Hall | Pad | W | Hollow square-wave pad, octave up, slow filter breathing | −12 dB, VCF LFO, chorus II |
| B4 | Hollow Lantern | Pad | W | Hollow pulse/triangle pad, gentle resonance | PWM (LFO), −12 dB, chorus I |
| B5 | Drift Ice | Pad | W | Pad whose filter opens over seconds, resonant sweep | slow ENV-1 on the VCF, resonance, long reverb |
| B6 | Night Bloom | Pad | W | Bright attack that darkens as the pad swells (inverted filter envelope) | ENV-1 inverted, PWM (LFO) |
| B7 | Ember Choir | Pad | W | Vocal-like band of pulse and saw between HPF and a resonant LPF | HPF, −12 dB, reverb pre-delay, LFO delay |
| B8 | Quiet Orbit Pad | Pad | W | Dark sub-octave pad with slow sine filter sweep and soft tremolo | LFO sine, VCF LFO, VCA tremolo, Vintage |
| C1 | Polished Tines | Keys | W | Chorus-heavy bell-like electric keys, velocity-bright | sine + triangle, VCF from ENV-2, velocity → VCF, ENV-2 key follow |
| C2 | Tin Roof Pluck | Pluck | W | Resonant saw pluck with a mono slap-back delay | ping-pong off, velocity → VCF |
| C3 | Ice Bell | Bell | W | Clear bell from cross-modulated sines, long ring | Cross Mod, ENV-2 key follow, reverb |
| C4 | Temple Chime | Bell | W | Metallic chime: cross-mod and sync together | Cross Mod + sync, chorus I |
| C5 | Pinched Wire | Keys | W | Thin, funky plucked-string keys | manual pulse width, HPF, −12 dB, resonance |
| C6 | Dusk Marimba | Pluck | W | Wooden mallet with a tiny pitch blip | VCO pitch from ENV-1 (both VCOs), key follow |
| C7 | Harp Rain | Pluck | W | Harp-like pluck that rings out; higher notes decay faster | ENV-1 and ENV-2 key follow, reverb |
| C8 | Frost Music Box | Bell | W | High music-box bells with echoes | both VCOs at 2', delay |
| D1 | Deep Current | Bass | W | Classic saw + square-sub poly bass, punchy filter | 16' + 16', filter envelope |
| D2 | Rubber Floor | Bass | W, Solo | Squelchy resonant mono bass with legato glide | Solo, Legato portamento, resonance 0.62 |
| D3 | Granite Unison | Bass | W, Unison 4 | Thick detuned mono bass | Unison, Vintage |
| D4 | Pulse Engine | Bass | W | Bass whose pulse width snaps with each note | PWM from ENV-1, chorus I |
| D5 | Sub Cellar | Bass | W, Solo | Pure low sub (triangle + sine), almost no filter movement | Solo, sine/triangle |
| D6 | Sync Growl | Bass | W, Solo | Growling sync bass with an envelope sweep on VCO-1 | sync, VCO pitch from ENV-1 |
| D7 | Picked Wire Bass | Bass | W | Plucked bass, decays to silence, ENV-1 pulse-width snap | −12 dB, PWM from ENV-1, velocity → VCF |
| D8 | Freight Line | Bass | W, Solo | Driving mono bass, always glides, wide bend | Always portamento, bend 7 st, Vintage |
| E1 | Solar Sync | Lead | W, Solo | Screaming sync lead, sweep on each note, delayed vibrato | sync, VCO-1 pitch from ENV-1, LFO delay, delay |
| E2 | Liquid Glide | Lead | W, Solo | Hollow square lead that always glides, mono echo | Always portamento, ping-pong off |
| E3 | Cross Wire | Lead | W, Solo | Aggressive cross-mod lead | Cross Mod (Classic) |
| E4 | Comet Tail | Lead | W, Unison 8 | Huge detuned unison lead with glide | Unison 8, unison detune, portamento |
| E5 | Reed Whistle | Lead | W, Solo | Soft triangle/sine lead, slow-onset vibrato | LFO sine, LFO delay |
| E6 | Neon Siren | Lead | W, Solo | Resonant pulse lead with a falling filter sweep | LFO saw on the VCF, manual pulse width, resonance |
| E7 | Wobble Wire | Lead | W, Solo | VCO-2 in Lo Freq mode cross-modulates VCO-1: a warbling lead | VCO-2 Lo Freq, Cross Mod |
| E8 | Thin Air | Lead | W, Solo | Thin narrow-pulse lead, octave bends | manual pulse width, HPF, bend 12 st |
| F1 | Chapel Stops | Organ | W | Two square "stops" (8' + 4'), gate envelope, no velocity | organ envelope, chorus II |
| F2 | Parlour Organ | Organ | W | Pulse organ with a key-click filter blip and tremolo | VCA tremolo, manual pulse width |
| F3 | Snap Tom | Percussion | W | Tuned tom: sine with a fast pitch drop and a touch of noise | VCO pitch from ENV-1, noise |
| F4 | Dust Hats | Percussion | W | Noise hats through high HPF, short and bright | noise, HPF, −12 dB |
| F5 | Hand Drum | Percussion | W | Resonant hand drum, triangle + noise | VCO pitch from ENV-1, noise |
| F6 | Storm Front | Sound FX | W | Wind and rain: noise through a resonant filter moved by smoothed noise | noise, LFO noise on the VCF, resonance |
| F7 | Laser Rain | Sound FX | W | Repeating sync zaps chopped by a square LFO, echoed | sync, VCO pitch from ENV-1, LFO square on the VCA |
| F8 | Orbit Beacon | Sound FX | W | Bubbling sample-and-hold filter over a Lo Freq warble | VCO-2 Lo Freq, Cross Mod, LFO random on the VCF |
| G1 | Twin Horizon | Dual | D | Strings (left) with brass (right): the classic layered brass-strings | Dual, pan, balance |
| G2 | Glass Over Velvet | Dual | D | Darkening pad under a cross-mod bell | Dual, ENV-1 inverted, Cross Mod |
| G3 | Octave Choir | Dual | D | Two PWM layers an octave apart, panned wide | Dual, pan, PWM |
| G4 | Stereo Twins | Dual | D | The same brass hard left and right, detuned against each other, drifting | Dual, pan ±1, Vintage 0.7 |
| G5 | Basement Duo | Split | S, Lower Solo | Mono bass under slow strings | Split, Solo bass |
| G6 | Two Rooms | Split | S, Upper Solo | Poly bass under a sync lead | Split, sync, balance |
| G7 | Corner Club | Split | S | Round bass under chorus keys | Split, chorus I |
| G8 | Stage Split | Split | S | Bass under brass | Split |
| H1 | Glass Steps | Arpeggio | W | Square pluck arpeggio, up two octaves, slight swing, echoes | arpeggiator Up, swing, delay |
| H2 | Racing Lights | Arpeggio | W | Fast up-and-down sync arpeggio over three octaves | Up & Down, 3 octaves, sync |
| H3 | Random Rain | Arpeggio | W | Random triplet arpeggio of cross-mod bells in a hall | Random pattern, 1/8T, Cross Mod |
| H4 | Upstairs Arp | Split | S, Lower Solo | Arpeggio on the Upper part (as played, 1/16T) over a mono bass | arp on one part, As Played, Split |
| H5 | Motor Line | Sequence | W, Solo | 16-step driving bass sequence with accents | step sequencer, step velocities |
| H6 | Clockwork | Sequence | W | Swung 16-step pluck pattern with rests and ties | sequencer rests and ties, swing |
| H7 | Deep Space Echo | Showcase | W | Slow pad into long ping-pong delay and a long, pre-delayed reverb | all three effects, long tails |
| H8 | Chorus Garden | Showcase | W | Plain saw + pulse made wide by chorus II at full mix | chorus II at 100 %, reverb damping |

## Standard phrases and level groups

Every program is measured with the phrase of its category (`tests/preset_unit/FactorySound.h`),
at velocity 100, 48 kHz, the default master volume (−6 dB) and the fixed output trim (−6.02 dB),
rendered through the real preset path (`PresetManager::selectProgram` on the processor). Levels are
measured with the output limiter **off**, so the rule compares programs and not the limiter.

| Phrase | Categories | Notes |
|---|---|---|
| Chord | Brass, Strings, Pad, Organ, Sound FX, Dual, Showcase, Init | C4 E4 G4 C5 held 3 s |
| Melody | Lead, and any chord category whose Lower part is Unison or Solo in Whole mode | eight legato notes G4–G5, 0.35 s each |
| Bass line | Bass | eight staccato notes C2–C3, 0.3 s apart |
| Broken chord | Keys, Pluck, Bell, Percussion | eight notes C4–E5, 0.25 s apart, then a 1 s chord |
| Held chord | Arpeggio | C4 E4 G4 held 4 s |
| Held note | Sequence | C3 held 4 s (the sequence is transposed by the lowest key) |
| Split phrase | Split | ten staccato bass notes C2–C3 below the split point with C4 E4 G4 held 3 s above |

**Level groups** (at most three): *sustained* (Brass, Strings, Pad, Organ, Sound FX, Dual, Showcase,
Lead, Split), *percussive* (Keys, Pluck, Bell, Percussion, Arpeggio, Sequence) and *bass* (Bass).
Init is the reference and belongs to no group.

**Level rule** (enforced over the embedded bank by `FactoryBankTests.cpp`):
1. Loudness = unweighted RMS of both channels from the first note-on to the last note-off of the
   standard phrase. Each program is within ±2 dB of its group's median. (The bank as delivered is
   within ±0.6 dB; group medians: sustained −27.3 dB, percussive −31.5 dB, bass −32.8 dB.)
2. An eight-note chord (C3 E3 G3 C4 E4 G4 C5 E5) at velocity 127, held 2 s: neither its RMS (from
   0.55 s) nor its peak is more than 3 dB above Init's (Init: −20.5 dB RMS, −7.0 dBFS peak), and its
   peak is below 0 dBFS. (Loudest chord as delivered: −4.5 dBFS, C2.)
3. The standard phrase peaks below 0 dBFS (loudest: −8.3 dBFS, F5).
4. With the limiter **on** at its defaults (ceiling −1 dBFS, knee from −4 dBFS): no standard phrase
   reaches the knee, so the limiter never shapes a factory sound played normally. The test also lists
   the full-velocity chords that reach the knee: none as delivered.

This replaces the contract's earlier wording in `docs/architecture.md` 6b.4 ("a full-velocity
eight-note chord stays below 0 dBFS at the default master volume"): that check is kept (rule 2),
and the relative rule is added on top.

Levels are set with each part's VCA level only. Master volume, the limiter switch and its ceiling are
session settings, not part of a program (`docs/architecture.md` 6b.1, 6c), and are left at their
defaults. The output trim scales every program equally (exactly × 0.5), so relative levels are
unchanged by it.

## Measurements

`VS_RENDER_DIR=<dir> VintageSynthTests "Render m6b_factory*"` (hidden, `[.render]`) writes one WAV per
program, `m6b_audition.wav` (all 64 in slot order), `m6b_index.txt` (start time of each program) and
`m6b_metrics.tsv` with, per program: peak (dBFS), RMS and its deviation from the group median, attack
(note-on until the 5 ms RMS envelope first reaches 90 % of its maximum before the first note-off),
release tail (last note-off until the envelope stays 60 dB below its maximum, effects included),
spectral centroid (magnitude-weighted, mid signal, over the notes), stereo width (side/mid energy,
dB; −99 = mono), the eight-note chord's peak and RMS, and the tail the processor reports to the host
(all with the limiter off), plus the phrase and chord peaks with the limiter on. The audition files
are rendered with the limiter on at its defaults, as a new instance plays.

The automated tests also require, for every program: it loads without parse errors, is distinct
from Init and from every other program, renders finite audio that is not silent (RMS above −40 dB),
and is below −80 dBFS once the reported tail has passed after the last note-off (nothing stuck).

## Deliberate exclusions

- **Arp Hold** is off in every program: a held pattern never ends by design, so it cannot pass the
  "silent after the tail" rule, and a program that keeps playing after the keys are released is a
  surprise when browsing. It is one switch away.
- **Arp pattern Down** and **16 unison voices** are not used; the neighbours (Up, Up & Down, Random,
  As Played; 4 and 8 voices) show the same machinery.
- **Hard-panned splits:** Split programs keep both parts centred so the bass stays in the middle.

## Coverage

Computed from the files (each row: programs that use the feature in a part that sounds).

| Feature | Programs | Count |
|---|---|---|
| Poly | all except those below and A1 | 48 |
| Unison | A6 D3 E4 | 3 |
| Solo | D2 D5 D6 D8 E1 E2 E3 E5 E6 E7 E8 G5 G6 H4 H5 | 15 |
| Portamento (Always, Legato) | A6 D2 D8 E2 E4 | 5 |
| VCO sync | C4 D6 E1 F7 G6 H2 | 6 |
| Cross Mod | C3 C4 E3 E7 F8 G2 H3 | 7 |
| VCO-2 Lo Freq | E7 F8 | 2 |
| Noise waveform | F3 F4 F5 F6 | 4 |
| Pulse width mod, LFO | A5 B2 B4 B6 B7 G3 H4 | 7 |
| Pulse width, manual | C5 E6 E8 F2 H8 | 5 |
| Pulse width mod, ENV-1 | D4 D7 | 2 |
| VCO pitch from ENV-1 | A8 C6 D6 E1 F3 F5 F7 G6 H2 | 9 |
| VCO vibrato (LFO) | A6 B1 B7 E1 E2 E3 E4 E5 E7 E8 G1 G5 G6 | 13 |
| LFO delay | A6 B2 B7 E1 E2 E3 E4 E5 E7 E8 G6 | 11 |
| LFO waveform other than triangle | B8 E5 (sine), E6 (saw), F7 (square), F8 (random), F6 (noise) | 6 |
| HPF | B1 B2 B7 C5 E8 F4 G1 G5 | 8 |
| VCF −12 dB | A7 B3 B4 B7 C5 D7 F4 | 7 |
| VCF resonance ≥ 0.5 | D2 E6 F6 F8 | 4 |
| VCF from ENV-2 | A7 C1 | 2 |
| VCF LFO mod | B3 B8 E6 F6 F8 | 5 |
| ENV-1 inverted | B6 G2 | 2 |
| Envelope key follow | A7 C1 C3 C6 C7 | 5 |
| VCA tremolo | B8 F2 F7 | 3 |
| Velocity → VCF | A2 A3 A4 A5 A6 A7 A8 C1 C2 C5 D2 D7 D8 E8 G1 G4 G7 G8 H5 | 19 |
| Bend range other than 2 st | D8 E8 | 2 |
| Vintage | A3 B2 B8 D3 D8 G4 | 6 |
| Dual | G1 G2 G3 G4 | 4 |
| Split | G5 G6 G7 G8 H4 | 5 |
| Part pan | G1 G3 G4 | 3 |
| Balance | G1 G6 | 2 |
| Arpeggiator (incl. one part only: H4) | H1 H2 H3 H4 | 4 |
| Step sequencer | H5 H6 | 2 |
| Swing | H1 H6 | 2 |
| Chorus I | A2 A5 A8 B2 B4 B6 C1 C4 C5 C7 D4 E4 F2 G2 G5 G7 G8 H2 | 18 |
| Chorus II | A7 B1 B3 B5 B7 B8 F1 F6 G1 G3 H7 H8 | 12 |
| Delay (ping-pong) | A6 C8 E1 E4 E6 E8 F4 F7 F8 G6 H1 H2 H4 H6 H7 | 15 |
| Delay, ping-pong off | C2 E2 | 2 |
| Reverb | 46 programs | 46 |
| Reverb pre-delay | B7 H7 | 2 |
