# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [Semantic Versioning](https://semver.org/). Released so far: 0.9.0 (a public beta).

## [Unreleased]

### Faster tests: two tiers and parallel runs (2026-11-26)
- **Two test tiers.** A test case that took 1 s or longer in the reference CI run is tagged `[slow]` (ctest label
  `slow`): *smoke* is everything else (`ctest -LE slow`), *full* is every test (`ctest`), unchanged in content. Test
  cases whose result depends on elapsed time or the scheduler are tagged `[serial]` and run alone.
- **Parallel ctest everywhere**: `scripts/preflight.sh` (mac, mac-sanitize, linux, linux-sanitize), the CI build
  rows and the CI sanitizer job run as many tests at a time as the machine has logical cores
  (`scripts/test-jobs.sh`; `VS_TEST_JOBS=<n>` overrides, `1` is the old serial run).
- **`scripts/preflight.sh --quick` runs the smoke tier**; new `--tests smoke|full` (smoke is refused for the default
  and `--full` runs); the summary names the tier and the job count.
- **CI input `tests`** (`full` default, `smoke`): the build rows run build, smoke tier, pluginval, artifact upload,
  then (full only) the slow tests. A release run needs `tests=full`. The run summary names the tier.
- Test code: the B-Side shell file test names its temp folder with a UUID instead of the clock.

### B-Side S4a: the chip part plays in the plugin (2026-11-24)
- **Exitium B-Side now plays its chip.** The test tone is gone: each part is 16 note voices, each a whole chip chain
  (renderer, filter, output stage), with the allocator, glide, bend, one vibrato oscillator per part, unison, velocity
  to volume and to cutoff, key follow, the three macros, tick rate from the host's tempo or `arp.tempo`, pan and a
  tail from the release times. Every parameter of 8.7 marked S3 or S4 is wired; the parameter list (202), the state
  and saved programs are byte-identical to before.
- **Latency is reported:** one figure for both models and both parts (Model A's chain; Model B is padded), every
  delay of the chain included: 31 / 22 / 41 samples at 44.1 / 48 / 96 kHz with the default oversampling.
- **Idle voices cost nothing and return silently:** a released voice renders on until it is below -120 dB (at most
  500 ms, then a 3 ms fade), the chip's time keeps running while it is idle, and a note starts settled on the chip's
  static level. Structural switches (model, filter topology, clock, resolution, volume DC) fade the part over 3 ms and
  continue in the new configuration with no design work on the audio thread (15 to 19 us per voice).
- **Shared filter topology:** one filter per part, the chips' shares moving over 3 ms when a note joins or leaves.
  A property of the topology, pinned by a test: the filtered sum is heard at the mean of the note gains, so a note of
  another velocity moves the held notes' filtered level smoothly (up to 5.9 dB on Model B and 7.2 dB on Model A at the
  extreme of velocity 1 against 0.01 with `vel.volume` at 100 %).
- **Cost** (share of one core at 48 kHz, non-LTO test build, the orchestrator's bench): 16 voices 4.39 to 4.51 % on
  Model B (gate 5 %) and 9.71 to 9.73 % on Model A (gate 11 %, owner decision O11); Dual 8.80 to 8.82 % and 19.47 to
  19.49 % (gates 10 and 22 %); Dual with the effects 9.47 to 9.62 % and 20.07 to 20.19 % (gates 15 and 27 %); 16 idle
  voices 0.08 %. The chip chain got a block path for stretches where nothing changes (the same bits, 47.8 million
  samples compared): `chainB` 26.7 to 26.9 ns and `chainA` 57.9 to 59.6 ns per internal sample at 96 kHz (were 30.6 to
  30.9 and 63.4 to 64.2).
- **Two independent reviews** with their own tests (`tests/bside/review/`) found and had fixed: a bump on held notes
  when a released note left a Shared filter, a click at the end of a structural switch on Model B, a note that came
  out up to 3 ms long after a delayed start, a latency pad without margin at host rates below 16 kHz, and All Sound
  Off leaving chips one internal sample ahead of the part (in Shared the output then depended on the host's block
  length).
- **Known, for the owner's ear in S4b:** a stolen voice restarts from the chip's power-on envelope value, as 8.6.4
  asks, which is a thump on every steal with many held notes; `kChipGain` is calibrated on the note's onset peak, so
  a steady pulse sits near -17.9 dBFS. No factory sounds yet (S4b).

### B-Side: Model A's cutoff jump at code 512 fixed; below 480 unchanged (2026-11-23)
- Scope by owner decision: only the jump at cutoff code 512 and the converter's boundaries above it. For every
  register value up to and including 479.0 Model A's corner is bit-identical to the previous form (pinned by a test
  at arbitrary values and variations, on both of the filter's paths).
- From code 480 up the mapping follows the reference's corner steps: within 2 percentage points per code step from
  479 -> 480 up to 1600 (worst 1.43; was 7.3 at 512). The fall at 511 -> 512 is -15.4 % (was -22 %; the reference's
  -15.0 % at a low level). In the owner's one-voice sweep every write from code 480 up is within 0.5 dB of the
  reference's step at the same write (worst 0.34 dB; was 0.79), with one named exception: the jump at 512 in the
  0.8 to 2 kHz band, where the reference does not move and ours falls 0.46 dB (Model A's level dependence, parked).
  In 2 to 5 kHz that jump is -1.44 dB (was -2.23; the reference's -1.67).
- Still open, recorded with Model A's parked level dependence: below code 480 the sweep keeps the previous steps
  (383 -> 384: -1.42 dB against the reference's -0.49 dB in 2 to 5 kHz), and the reference's steps at 255 -> 256 and
  319 -> 320 are missing in ours.
- New reference data: `a_cutcode_*` (the cutoff register walked one code at a time across the boundaries from 480 to
  1600, noise at a low level) and `a_cutsweep_a` (the one-voice sweep), with the reference's step per write as
  constants. A cutoff every sample (`processWithCutoff`, Model A) got cheaper from code 480 up (29 -> 21 ns in
  `chip_bench`); below 480 it costs what it did.

### B-Side S3b-2 closing round after the fourth review (2026-11-23)
- Tests: the write gate holds resonance writes alone through the low-, band-, high- and band + high-pass (0 <-> 15 and
  4 <-> 12, volume DC on and off) besides the combinations: 240 rows in the full set, worst -90.7 dB (Model B, 88.2 kHz,
  band + high-pass; budget -90); combinations of resonance changes (several in one sample, two writes 0 to 10 cycles
  apart, the stated capacities at and beyond their limits) equal a twin of the documented rule to 1e-12; a read past
  the end in the test's measure (found by AddressSanitizer) fixed.
- Records: the cost of a resonance write with the cutoff moving (about 2 to 3 us; a full tick up to 6 us per voice),
  the Shared owner's obligation for the one-state rule, the limits with what bursts beyond them cost.

### B-Side S3b-2 round after the third review (2026-11-23)
- Writes that change the filter's dynamics hold in any combination: per voice the filter keeps one state, each
  resonance write changes its dynamics at the write's place between two samples and each cutoff write from its own
  sample on, and the configurations being blended are output equations on that state. A volume or mode write right
  after a resonance write no longer ends its placement early (it did: -78.7 dB on Model A), a cutoff write inside a
  resonance write's window lands at its sample (it was up to 8 samples late), and switching a voice to a shared filter
  at any moment is safe (it crashed inside a resonance write's window).
- Tests: the excess over an ideal render at 32 times the rate for resonance writes alone and with a volume, mode or
  cutoff write 1 to 200 cycles before or after, both models, 88.2, 96 and 192 kHz: worst -97.7 dB (budget -90); the
  place of the dynamics by itself; combined writes bit for bit across any block split and across switches to Shared.
- The reduced alias gate fails if any single pinned row is taken off the list (all 16 tried).

### B-Side S3b-2 round after phases (2026-11-23)
- Resonance writes are placed between samples: the filter's dynamics change at the write's place as a mixture of the
  switch at the sample before and after it, while the output equation follows the band-limited blend. (This held for a
  resonance write on its own only; replaced after the third review, see there.) Against an ideal
  render of the same writes at eight times the rate (a write every tick, resonance 0 <-> 15 and 4 <-> 12, low-pass and
  band-pass, both models, 88.2 and 96 kHz) the excess was up to -78.4 dB and is now at most -95.9 dB (budget -90).
- The alias gate's four-times-the-rate reference now runs Model B's mixer input through the chain's own pre-filter, as
  Model A's did: the two Model B rows reported in the second review round equal their reference. Every row of the full
  set meets its rule; the 16 pinned Model A rows (owner decision O6/O8) are unchanged.
- The write budget's reference line is the output's strongest line other than DC (with volume DC on the output's mean
  had been taken as the line, flattering those rows by about 20 dB); every row still meets it.
- The alias gate's part of the normal test run takes 26 s (was 161 s): every listed row in its worst constellation and
  the common reset, a reduced set of the others; the full set by its tag.

### B-Side S3b-2 second review round (2026-11-23)
- Alias gate over voice phases: every multi-voice row runs in five phase constellations (all voices at phase 0
  among them) and the worst counts. **Known deviation (owner decision O6), its real extent:** with Model A's mixer
  overdriven (two or three full-level voices through the low-pass at full resonance) the static curves fold back up to
  12.4 dB over the limit at 88.2 kHz, 11.1 dB at 96 kHz and 0.2 dB at 128 kHz internal rate, at 2 kHz and at
  3.85 kHz, depending on the voices' phases. 16 such rows are listed in the test with their worst figures and gated at
  figure + 1 dB, two-sided (a row more than 2 dB better, or inside the limits, fails as stale).
- Two Model B rows through the resonant band-pass seemed to exceed the four-times-the-rate reference + 1 dB by 0.5 and
  0.9 dB in one phase constellation; the round after phases found the reference at fault (see there).
- Shared: the owner of a shared filter switches a mode or resonance write at the nearest sample
  (`ChipVoice::sharedSwitchSample`, new): within 0.5 samples of Per Note's band-limited step (was up to 1.06). The
  0.02-sample timing remains Per Note's; `ChipVoice.h` states what an owner must do to reach it (S4).
- GCC's contraction of multiply-adds is off for the chip code in every target that compiles it, the B-Side plugin
  included, set in one place in the build.
- The mixer curve's continuation has its own unit test; at most five configurations are blended at once (tested).
  (Resonance writes: see the round after phases.)

### B-Side S3b-2 closing round (2026-11-23)
- Alias gate (8.19.4): a real-chip row now passes inside its model's absolute limit or within 1 dB of the
  four-times-the-rate reference. (The figures below held for one phase constellation; see the second review round.)
- **Known deviation (owner decision O6):** with Model A's mixer overdriven (two or three full-level voices at 2 kHz
  through the low-pass at full resonance, 88.2 and 96 kHz internal rate) the static curves fold back to -91.2 to
  -97.0 dB against the -99 dB limit, up to 18 dB above an ideally oversampled chain. These 8 rows are listed in the
  test with their measured figures and gated at figure + 1 dB (closed list: a row that improves or that does not
  overdrive the mixer fails the test). To be revisited with the Model A filter form and the S4 budget.
- The chip renderer no longer re-evaluates Model A's converter bits at every sample end (the branch was unreachable;
  they switch at writes). Output unchanged.

### B-Side S3b-2 review round (2026-11-23)
- Write timing: Model A's output-function coefficients now follow the interpolated sample they act on (mode steps
  landed 0.31 samples early); resonance writes blend two configurations like mode writes; every volume, routing, mode
  and resonance write lands within 0.0005 samples of where `latencySamples ()` says, Per Note, and volume and routing
  writes in Shared too (tested per type, both models); a routing write that changes Model A's voice counts within 0.1
  samples (the gain's step times the signal's). (Corrected in the second review round: Shared mode and resonance
  writes were up to 1.06 samples late, now within 0.5.) A shared filter's owner hands over its output function per
  sample.
- Model A's mixer curve continues smoothly past its fitted range (no flat clamp); the gated reference traces are
  unchanged. Model A's 2x half-bands are about 100 dB; every half-band has the fewest pairs that meet the pass band.
  Model B's filter, output stage and pre-filter now take 6.3 to 6.4 ns per sample at 96 kHz (budget 7: met).
- The alias gate restated (8.19.4): met everywhere except Model A driven hard through the resonant low-pass (4 rows
  with ideal voices, -93.4 to -96.7 dB against -99; 38 real-chip rows above the four-times-the-rate reference) and 6 rows
  within 0.7 dB of the reference; the owner decided in the closing round (pinned, see above).

### B-Side S3b-2: aliasing of the static curves (2026-11-23)
- The chain's static curves run on band-limited signals: Model B's mixer input passes a linear-phase half-band FIR
  (pass band to 20 kHz within 0.01 dB) before the curve; Model A's two buses pass the same FIR before the filter, and the
  filter's output function, the mixer gain and the mixer curve run at twice the internal rate between two short
  half-band filters (`HalfBand.*`, new). In Shared the 2x part runs per chip on its share (the owner hands over the
  shared filter's linear output and low-pass value: `ChipVoice::finishShared` takes them).
- Model A's voice bend is folded into the renderer (`Chip::renderBendVoices`, on in `ChipVoice`, off for a bare chip):
  its steps and slope changes are those of the bent waveform, so the bend no longer folds; `OutputStage::shapeVoice`
  is gone (`render::bend`).
- The chain's latency grows by the filters' delay (Model B 8 + 21 samples at 96 kHz, Model A 8 + 21 + 7; at 88.2 kHz
  8 + 41 and 8 + 41 + 7); writes still land at their cycle in the output.
- Alias gate of 8.19.4 (decisions "B-Side S3b-2" A2, A3): met on every row whose curves stay inside their limits, at
  every rate (Model B at worst -111.0 dB with real chips, -126.1 dB with ideal voices; Model A -115.6 / -117.7 dB);
  not met on 9 rows where two or three full-level voices through Model A's low-pass at resonance 15 drive the mixer past
  the limit of its curve (-78.9 to -98.3 dB). Real chips through the band- and high-pass are reported: there the
  renderer's own components, weighed by the filter against voice 1's fundamental, decide the figure.
- Wanted signal: pass band within 0.023 dB, products of sines within 0.011 dB and harmonics above -40 dB within
  0.06 dB of the exact curves. Cost: Model B's filter, output stage and pre-filter take 7.6 to 7.8 ns per sample at
  96 kHz, over the 7 ns budget (not met); Model A's added stages about 13.5 ns plus 4 ns for the bend.

### Tests: the editor feed's seqlock test no longer depends on the scheduler (2026-11-23)
- "UiFeed: the held-order seqlock never hands out a torn copy" failed once on the Linux ARM64 CI runner: the writer
  thread never paused, so the reader was refused 5 million times and saw no consistent copy (`consistent > 0`). No
  torn copy was handed out; the feed's code is unchanged. The writer now waits every 4096 writes until the reader
  has taken one more consistent copy (with a 10 s deadline that fails the test instead of hanging it).

### B-Side S3b-1: two buses and the whole chain (2026-11-23)
- The chip renderer delivers its two buses in double precision (`Chip::renderBuses`): the oscillators routed to the
  filter and the others, with each bus's envelope sum and voice count; oscillator 3 can be muted on the direct path.
  A routing write moves an oscillator between the buses at its exact cycle, band-limited. The single-sum render is
  unchanged.
- `ChipVoice` (new): one chip's whole chain, the buses -> filter -> output stage, in double up to one shaped
  conversion to float, Per Note or with a shared filter. Volume and mode writes land at their cycle, band-limited
  (with volume DC on a volume write is the chip's own step, at its full size); the cutoff runs per sample while it
  moves. Not wired into the plugin yet (S4).
- The trace gates now run through the real chain; every earlier analog gate still passes, and the triangle of
  `a_dac_*` is gated (harmonics within 3 dB). Model A's per-voice bend is applied on the buses; its aliasing and the
  mixer curves' are the next step's (S3b-2).
- GCC 13 compiles the chip sources and the B-Side tests with warnings as errors (a bounds warning in the renderer).
- Fix round after the independent review: a shared filter now gives each chip its static output per sample (one
  chip is exactly Per Note, and the output no longer depends on the owner's block length); routing, mute, volume and
  mode writes land one cycle earlier, where the reference shows them (within a quarter of a cycle on average); a full
  write queue keeps the order of the writes; the chain's latency is a run-time value (`latencySamples ()`). The Shared
  documentation states that Model B equals Per Note for identical voices only (-39 to -32 dB with different notes).
  New tests pin the volume with volume DC off, the cutoff's per-sample path and the writes' sample, and the static DC.

### B-Side S3a fix round after the third review (2026-11-23)
- Model B's band/high second-order term is removed: it matched noise floors but put harmonics on tones that the
  reference does not have, aliased far above the budget and broke the Shared claim. Model B's filter is linear again;
  its high-pass stop-band floor on noise at mid and full level stays about 6 to 7 dB low (a recorded known deviation).
  `s3aB` 5.5 ns at 96 kHz (budget 7).
- Phase: the filter's numerator keeps the prototype's zeros on their side of the unit circle (was always the
  minimum-phase factor, up to 180 degrees off). The phase of the linear response is gated at 30 degrees from the
  prototype over the rate gate's set (seven settings at 88.2 and 96 kHz gated at their own figure + 2, at most 33.7
  degrees); `processWithCutoff` within 1 degree of `setCutoff`. Magnitudes are unchanged.
- Tests: the Shared test also runs where the band- and high-pass carry the signal (volume DC on and off); the input
  guard's threshold is pinned. `Filter.h` documents the table rebuild cost of a resonance or mode write and the
  subnormal input path; its claims are reduced to what is tested.

### B-Side S3a closing step (2026-11-23)
- The filter's response no longer depends on the internal rate: matched poles and a numerator designed per mode
  combination, at no per-sample cost. Within 0.19 dB (Model B) and 0.10 dB (Model A's linear part) of the analog
  prototype at every rate of 4.4 (was up to 3.2 dB at 88.2 kHz); tested over every mode sum, resonance and cutoff.
- `Filter::processWithCutoff` reads the mapping from a small table (within 0.05 dB of `setCutoff`) and now follows
  Model A's damping, band weights and `variation` at the cutoff given.
- Shared topology: every chip's stage gets the filter's static DC in full (`OutputStage::sharedFilterShare`): Model B
  equals n Per Note chips for n = 1 .. 16; Model A's difference (summed distortion) is documented and tested.
- Model B's band- and high-pass part carries a second-order term (0.10), the reference's stop-band floor at mid and full
  level (was about 10 dB low; held-out traces now within 0.2 dB). Removed again in the fix round above.
- Tests that can fail: the board low-pass corner, the volume with held voices (DC off and on, applied once), every
  resonance and cutoff law; each shown failing on a mutation. Test sources build with GCC 13.
- Cost (`chip_bench`, 96 kHz, indicative): `s3aB` 5.9 ns (budget 7), `cutB` 10.4 ns. Open: `ChipRender.cpp` fails GCC
  13's `-Warray-bounds` (GCC 11 builds it).

### B-Side S3a round 2 (2026-11-20)
- Model A's held-tone distortion is per voice: `OutputStage::shapeVoice` bends each voice before its envelope (the
  renderer applies it per voice in S3b); the output stage and the filter's output functions refitted with it. Held tones
  through the low-pass and direct at drives never fitted: within 2.2 dB.
- Volume DC off: the volume follows the board high-pass; volume and routing writes with held or silent voices no longer
  move the output's DC (below -60 dBFS, tested).
- Input guards off the dependency chain, Model B `s3aB` 4.9 ns at 96 kHz; `Filter::processWithCutoff` for a cutoff every
  sample (3.5 ns extra on Model B); the Shared topology defined and tested for 1 to 16 chips.
- Test harness renders each voice alone (no sync or ring), so `a_routing_*` is compared; new committed gate traces
  `a_s3a_*` (13 files, generator `tests/bside/tools/gen_s3a_traces.py`).
- **Open (reported, not gated):** Model A's cutoff and Q do not follow the routed DC, the level or the resonance as the
  reference does (corners up to 75 % off on `a_s3a_level_a`); modes at cutoff 200; Model A routing; the resonance-write
  DC step; band-pass and two-tone distortion; the internal-rate dependence (decisions "B-Side S3a round 2").

### B-Side S3a: filter and output stage (2026-11-19)
- `source/bside/chip/Filter.*`: the chip's 2-pole state-variable filter for both models (low-, band- and
  high-pass summed by the mode bits, inverted as the reference; continuous cutoff 0 .. 2047, 16 resonance
  steps). Model B close to linear in the register; Model A with its own cutoff converter (11-bit ladder, the
  steps at bit boundaries), a fitted cutoff curve, Q against the cutoff, `variation`, and its distortion as
  static functions of the low-pass state. Clock-independent, no allocation, can be shared by several chips.
- `source/bside/chip/OutputStage.*`: voice DC (per voice and envelope), mixer (a static curve after a gain
  that falls with the inputs connected), 4-bit volume, `volumeDc` off, board filters and the reference's
  output scale. All constants are our own fits to generated data (`docs/decisions.md` "B-Side S3a").
- Gates against the committed analog data (`bside_trace`): filter corners at 16 cutoffs (worst 3.9 % Model A,
  1.8 % Model B; allowed 5 %), resonance peaks 0 .. 15 (0.22 / 0.06 dB; allowed 1), Model A distortion
  harmonics 2 .. 5 at three drives (within 2.3 dB; allowed 3), Model A voice DC steps (0.3 dB; allowed 1), the
  modes within the scripted tolerance (Model A 4.47 % outliers, mean 0.94 dB; Model B 2.48 %, 0.57 dB).
  `a_routing_*` stays skipped until the renderer has two buses (S3b).
- Unit tests: stability at every setting and internal rate (full-scale noise, writes every sample, decay to
  -120 dB with no limit cycle or subnormal state), any block split bit-identical, no allocation.
- `chip_bench`: new cases `filterB`, `filterA`, `outputB`, `outputA`, `s3aB`, `s3aA` (filter + output stage:
  6.5 ns Model B, 7.6 ns Model A per internal sample at 96 kHz on this Mac, indicative).

### CI: pluginval teardown crash (2026-11-23)

- Linux x86_64: pluginval 1.0.4 (the newest build that runs on glibc 2.35) crashes in about 6 to 7 % of runs
  after all tests have completed (a use-after-free in the JUCE 8.0.3 it is built with, fixed in JUCE 04e167d646).
  `build.yml` (the "pluginval (Linux)" step and the distribution job) and `scripts/linux-check/distro-check.sh` now run
  it through `scripts/linux-check/pluginval-run.sh`, under gdb (added to the package lists), which accepts that one
  crash signature and nothing else. Self-test with canned outputs: `scripts/linux-check/pluginval-run-selftest.sh`,
  run by `scripts/preflight.sh --only docs`. The negative distribution check is unchanged.

### Release rehearsal and documentation (2026-11-20)

- The release run of the build workflow passed at its first attempt (run 44695944505, all five
  targets, the three release jobs) and created the draft release `v0.9.0`.
- The owner tried the zips by hand (Cubase 15 Pro on macOS; Reaper on an Arch-based Linux; Reaper
  and Cubase 15 Pro on Windows 11; recording and playing back automation included), found no defect and had the draft published as a pre-release (on 2026-11-21 the owner had the mark removed, so GitHub shows 0.9.0 as the latest release)
  on 2026-11-20. The tag `v0.9.0` is at commit `116c5f8`.
- `CLAUDE.md` is now short: the rules, the fixed facts, the definition of done and two rules that
  were the owner's private ones until now ("Check before you claim", and ending a turn with
  "What I need from you"). The project status moved to `docs/status.md`, the directory guide to
  `docs/layout.md`, the full list of standing decisions to the top of `docs/decisions.md` and
  the trap list to the top of `docs/engineering-notes.md`.
- New `docs/development-practices.md` and a README section: the repository as a worked example
  of AI-assisted development, automated testing, CI/CD and DevOps practice.
- Documentation brought up to date after an audit: README (status, CI and security, roadmap),
  `CONTRIBUTING.md`, `SECURITY.md`, the skills `add-parameter`, `validate-plugin`, `build`,
  `release` and `orchestrate`, the manual test checklist.
- A demo video recorded by the owner with 0.9.0 is linked from the README, the screenshots page
  and the release notes: https://youtu.be/IdVCa7lpVsQ
- The second Windows CI run (44634121070) found two test assumptions that hold only off Windows
  (an unwritable path, a path separator); fixed in the tests.

### First Windows and Linux x86_64 CI run (2026-11-20)

The first GitHub Actions run on Windows (MSVC) and Linux x86_64 (run 44627905615) found these; fixed here.

- Windows x64 and ARM64: the build failed in B-Side test, tool and benchmark sources only (the plugins, the
  Vintage tests, `host_smoke`, `BSideTests` and `bside_independent` compiled and linked; no test had run yet).
- `getenv` is deprecated on MSVC (C4996, an error in the B-Side targets): one header-only test helper,
  `tests/support/Env.h` (`testenv::get`, `_dupenv_s` on Windows), now serves every call site in `tests/`.
  `source/` has no `getenv`.
- `tests/bside/tools/ShapeFit.cpp`: `1L << 40` is undefined on Windows (`long` has 32 bits); the fit costs are
  now `std::int64_t` (a real bug there, not only a warning; identical on macOS and Linux).
- Signed/unsigned comparison in `ChipReplayTests.cpp` (C4389), a shadowed `now` in `PresetTestHelpers.h` (C4458)
  and a double-to-float range construction in `ReverbTests.cpp` (C4244) cleaned up; no test expectation changed.
- `build.yml`: the `build` job's timeout is 60 minutes (was 30, sized for a warm cache: the Linux x86_64 row
  hit it during pluginval on a cold cache).

## [0.9.0] - 2026-11-20

First public release of Exitium Vintage Synth, a public beta.

### The instrument
- Bi-timbral analog-style polysynth in VST3 format: two parts (Lower and Upper), 16 voices each,
  played as Whole, Dual or Split.
- Per part: two oscillators (sine, triangle, saw, square, pulse, noise) with sync and cross
  modulation, high-pass filter, 12/24 dB low-pass filter with resonance, amplifier, two envelopes,
  an LFO with delay, Poly/Unison/Solo, portamento, velocity, pitch bend and mod wheel.
- One "Vintage" amount for drift and voice-to-voice variation; clean at zero.
- Arpeggiator and 16-step sequencer, in time with the host.
- Chorus, delay and reverb; an output limiter.
- 64 factory programs, your own programs and banks, loading the sound of one part only.
- Its own editor: pages Sound, Envelopes, LFO, Play, Arp + Seq, Effects and Browser; pictures that
  follow the sound; every control follows host automation live; 75 to 200 % size.

### Platforms
- macOS 11 or newer (Apple silicon and Intel), Windows 10 or newer (x64), Linux with glibc 2.35 or
  newer (x86_64).
- Experimental: Windows ARM64 and Linux ARM64 (in the same downloads).

### Known limits of this beta
- The plugin is not signed with a developer certificate and there is no installer: unpack the zip
  and copy the plugin into your VST3 folder; `INSTALL.txt` in each zip explains the system warning.
- **Parameter IDs and the saved-state format are not frozen before 1.0.0:** a session or program
  saved with this beta may not load unchanged in a later version.
- Not tested with a screen reader. Some factory programs sound alike; the set will be revised.
- Cross Mod keeps its classic sound, which aliases at high settings.

## Development history before 0.9.0

### M8 release hardening (2026-11-19)
- Release packaging (docs/architecture.md section 10): `scripts/package-release.sh` (`pack`, `verify`, `sums`,
  `changelog`, `selftest`), texts under `resources/release/` (INSTALL per platform, SOURCE, THIRD-PARTY derived
  from what the build compiles), one zip per platform with `SHA256SUMS.txt`; `verify` works from the zip alone
  (layout, version, slices, B-Side strings, metadata entries, symlinks, macOS `codesign --verify`).
- `build.yml`: input `release` (Vintage only, then the jobs `pack`, `zip-smoke` and `package`, which creates a draft
  release); never run yet (CI is offline). The `mac` step of `scripts/preflight.sh` packs and verifies the zip.
- `verify` hardened after review (no symlinks, duplicate/case-colliding entries, exact file and mode lists, UTF-16 B-Side scan, platform-specific install text, licence comparison); release runs share one concurrency group, check the tag and that the repository is public, and the draft text carries an experimental-ARM64 header.
- Second review round: B-Side is recognised by class IDs and bundle identifier (`resources/release/IDENTITY.txt`) instead of a 4-character binary search; `verify` checks the full entry list and unsafe names; `build.yml`: `repo-public` in `plan`, `linux-distros` needs a successful `plan`, and the Linux dependency check (`! ldd | grep -q`, which could never fail) is rewritten.
- Fixed: the `ldd` checks in `build.yml` and `scripts/linux-check/inner.sh` (`! ldd | grep -q`) could never fail; they now capture the output and fail on a missing library or a shared libstdc++/libgcc.
- Fixed: the macOS bundle's ad-hoc signature did not cover `moduleinfo.json` (JUCE writes it after the linker signs),
  so `codesign --verify --deep --strict` failed on every build; the bundle is now signed again (ad hoc) as the last
  build step.
- Changed: the local gate (`scripts/preflight/docs-check.sh`) now enforces the Actions trigger policy over every
  file in `.github/workflows/`: `workflow_dispatch` only, except `security.yml` (push to `main`, pull_request,
  one cron at most weekly, workflow_dispatch). `pull_request_target` and `workflow_run` always fail.
- Added: `scripts/preflight/test-workflow-triggers.sh`, the policy itself plus a self-test on fixture workflows
  (run on every docs check) that proves each violation is caught.
- Removed: a tracked Python bytecode file (`tests/bside/golden/tools/__pycache__/model.cpython-315.pyc`, which
  embedded a developer's home path). The docs check now fails on any tracked `__pycache__/`, `.pyc` or `.pyo`
  file and on any absolute home path (macOS, Windows or Linux user directory) in a tracked text or binary file.
- Changed: a test comment no longer names a third-party product; the hashed brand deny-list gained one more
  entry (a filter integrated-circuit part number found in old history).
- Added: screenshots of every editor page (`docs/images/`, `docs/screenshots.md`; the README shows the Sound
  page). Removed: the Browser wireframe picture. The B-Side reference-data manifests state provenance and licence.
  Pre-public audit decisions: `docs/decisions.md`.
- Fixed: typed parameter text such as "nan" gave NaN to 70 float parameters; text naming NaN or infinity, or a
  result that is not finite, now gives the parameter's default (as in B-Side). Test over all 172 parameters.

### B-Side alias follow-up step (2026-11-19)
- Fixed: Model B pulses of width 1 and 4095 missed the -100 dB alias budget by up to 2.7 dB at frequencies
  with a small-denominator ratio to the internal rate. The cause was the float conversion of the output (its
  rounding errors repeat with the signal and gather on a few lines), not the rendering. The conversion's
  rounding error is now noise-shaped (at least 8 dB lower in 0 .. 20 kHz; plain rounding once the output has
  stood still for 128 samples, so a silent output stays constant). Exact measurement over 6 760 rows at every
  internal rate: worst -113.4 dB (was -97.3).
- Fixed: a synced target at an exact integer or half-integer frequency ratio lost the step (saw) or slope
  change (triangle, combined waveforms) of every second reset, where the reset fell on the same instant as the
  target's own wrap, fold or cell change: a line at half the source's rate, up to 0 dB re the strongest line
  for Model A combined waveforms. The values before an event now come from the line's own state. Exact
  ratios 1 to 8, 1/4 to 7/2: worst -108.4 dB re the strongest line (budget -100). Ratios 2 and 4 were affected
  for combined waveforms only.
- New gates: the exact measurement of narrow pulses and synced targets at exact ratios, a fast subset in the
  suite and full scans in the hidden `[.render-exact]` and `[.render-sync-integer]` (which now assert).
- Output differs from the render cost step by at most 4.8e-7 (-126 dB re 1, the rounding) in patches without
  sync; `chip_bench` unchanged within its scatter.
- Review round 1: the rounding is a class of its own (`render::OutputRounding`, `RenderTables.h`) measured
  directly (in-band suppression at every rate, 4.75 float steps, settle count, cleared state); the exact gate
  also runs on the general path. Recorded: the cost of re-rounding into a non-zero buffer (-96.5 dB) and of two
  chips in one float buffer (0.86 dB margin), which bind S3b and S4.

### B-Side render cost step, fourth review (2026-11-19)
- Tests and records only. The step moments' alias test now measures synced targets at a non-integer ratio
  (2.71) against the same -80 dB as plain and ring targets, and the strongest line outside DC's main lobe:
  at most -81.5 dB re the strongest line where it is above -50 dB re 1; quieter shapes (-60 to -50 dB) are
  printed and recorded (worst -76.1 dB, about -136 dB re 1). The earlier claim that synced targets cannot
  meet -80 dB was wrong.
- Recorded as open, older than this step: synced targets at an exact integer frequency ratio (hidden report
  `[.render-sync-integer]`); a width write that changes the rendered gate two cycles before the chip; a few
  cases where a frozen line and one crawling below one accumulator step render differently (hidden report
  `[.render-frozen-crawl]`).
- Tests that kill two more mutations (a frozen landing left pending to the batched path, a frozen source's
  ring flip without oscillator 1's delay) and an assertion on the fitted tables that the gate re-read's
  timing after a pull-down cannot change the output.

### B-Side render cost step, third review (2026-11-18)
- Fixed: the output depended on where `renderUntil` stopped without a write (pieces now start at a sample's
  start or a write and end at its end; random non-write stops are tested bit-identical, 150 seeds in the
  suite, 4 000 in the hidden `[.render-split-long]`); a frozen Model A saw combination pulled down by a width
  or waveform write (no prediction, a large correction, a ring target's old top bit).
- Above the register's range the step moments' stretches end at two fixed points per sample: plain and ring
  targets alias at most -106 dB re the strongest line (was -79.6), asserted at -80 dB.
  `comb12k` 184 (Model A) and 162 ns (Model B) at 96 kHz, `chip_bench` alone (target 200).
- Tests that kill the surviving mutations of the review, including an `envInert` counter-example (a write in a
  sample's last cycle), which an earlier review had called equivalent.

### B-Side render cost step, review fixes (2026-11-18)
- Fixed: a division by a zero increment in the Model A pull-down's closed form (SIGFPE on x86-64 at frequency
  0); a frozen Model A pull-down kept its pulse gate (one-cycle re-reads are now scheduled in time on frozen
  lines); `BS_CHIP_INLINE` is plain `inline` on MSVC (C4714).
- Above the register's range combined waveforms keep their staircase and emit a stretch's steps by their time
  moments (no switch click; the audio band within about -78 dB re 1 of one-by-one steps, against -18 dB of the
  coarse levels it replaces); `comb12k` 132 / 103 ns at 96 kHz.
- Tests: the sanitizer build passes every `bside` and `trace` test; regression tests for frequency 0, the frozen
  pull-down, switches without clicks, the entry hysteresis, frozen lines in the state check, the converter
  bits against the frequency, a pending envelope step; hidden `[.render-exact]` report.
- Measured and not adopted: dropping a free saw's converter bit at half a toggle per sample (-62 dB in the
  band around phase resets and waveform switches).

### B-Side render cost step (2026-11-18)
- `Chip::render` per internal sample per chip at 96 kHz (`chip_bench`, this Mac, back to back with the branch
  point): silent 61 -> 1.9 ns, typical 57 -> 13, Model A typical 217 -> 36, three Model A saws 297 -> 44,
  the new `s4gate` case (the S4 gate's patch) 265 -> 32, Model A combined 395 -> 90, three combined waveforms
  at 12 kHz 5 104 -> 131 (Model A) and 4 272 -> 117 (Model B), worst 780 -> 316, noise 199 -> 164, noise+saw
  source 332 -> 178, Model A landings 652 -> 193; every case faster at 48 and 192 kHz too.
- How: a batched path that renders whole samples with the digital core advanced once, bit-identical to the
  general path; envelopes that cannot change skip their repeating states; the Model A pull-down in closed
  form in the digital core; envelopes after the oscillators in the write-back's per-cycle mode; both BLEP
  kernels as tap-major tables (the long one by Hermite interpolation, refined for combined waveforms' cell
  steps); the Model A converter terms as tables; written-out sweeps for triangle, saw and combined lines.
- Above the register's range (Smooth, to 12 kHz) combined waveforms use coarse staircases whose in-band
  harmonics are the fine one's (aliasing -101 dB at worst there, no budget); at and below it nothing changes.
- Fixed the three defects 8.17 listed (a synced pulse at frequency 0, a ring target after a width write in the
  step before its source's rise, a frequency drop between a sync source's rendered crossing and its rise),
  each with a regression test that fails on the old renderer; the state check now checks frozen lines.
- Not met (decisions "B-Side render cost step" rows 12, 13): Model A triangle and saw patches (36, 44, 32 ns
  against 24) and Model A combined (90 against 60).
- `chip_bench`: case names as arguments, `CHIP_BENCH_SECONDS`, the `s4gate` case. Tests: `[batch]`, `[levels]`,
  `[defects]`, new equivalence cases, hidden `[.render-above]` report.

### B-Side parameter step, review fixes (2026-11-18)
- Typed text: every B-Side text -> value path is finite and in range (NaN, inf, 1e999 give the default);
  `parseFirstNumber` reads sign and exponent; choices accept any case and a bare number naming one label.
- `PartMappings.h`: pulse width, PW offset and cutoff offset are continuous (Smooth); `authenticRegister` rounds.
  The tick-rate range uses the mapping functions. Unused `pulseWidthPercent` removed; wave defaults use
  `kOscWavePulse` / `kOscWaveOff`.
- Tests that kill the surviving mutations (choice and clock upper bound, NaN default, all `PartParams` defaults,
  tick clamp, log-domain envelope lookup).

### B-Side parameter step (2026-11-18)
- Exitium B-Side now reports 202 host parameters (docs/architecture.md 8.7): the 75 shared ones, then
  `lower.*` (63), `upper.*` (63) and `chip.clock`. Inert: nothing on the audio thread reads them yet
  (the plugin still plays its test tone).
- `source/bside/params/`: `PartParamList.h` (one X-macro list for both parts), `PartMappings.h` (pulse width,
  macro offsets, envelope nominal times, exponential tick rate, vibrato and detune cents), `ParameterLayout.*`
  (layout, display text and text parsing, `PartParamBinding`, `ChipClockBinding`).
- Sessions and program files saved before this step still load, the new parameters at their defaults;
  `stateVersion` stays 1.
- Tests: `tests/bside/integration/ParamTests.cpp` (hand-written contract table, ranges, labels, display text,
  text round trip, state and file round trips, old sessions, symmetry, binding); counts in `ShellTests.cpp`
  and `HostSmoke.cpp` are now 202.

### Orchestrate skill trimmed (2026-11-16)
- `.claude/skills/orchestrate/SKILL.md` now holds only what is specific to this repository (serial files,
  worktrees, local-first gates, budgets, model routing, commit rules). Removed: the finished Wave 0–4 plan
  and the steps from the time CI ran on every push (draft PRs, "green in CI before done", three targets).
  The process itself is the owner's generic skill, which reads this file first.

### B-Side S2b-2 third review fixes (2026-11-16)
- Fixes: ring state from a per-cycle source's accumulator at writes and activation; a predicted reset dropped
  when a write stops the source's rise; no pull-down confirmed by a sync reset; the gate re-read after a
  width write just before the rise (from the chip's first landing); the rising edge after a late reset;
  no late falling edge at width 0; no carried flip back across a write; the gate re-derived after a
  correction.
- `Chip::renderCheckStates` / `renderStateMismatches` (white-box check, tests only); `[schedule]` with every
  write kind (gate, sync, ring, raw bytes, restarts, dense writes, Authentic); the reviews' cases as
  regression cases; hidden long runs `[.render-corrections-long]` (50 000 patches) and
  `[.render-schedule-long]` (3 000 schedules), both clean.

### B-Side S2b-2 second review fixes (2026-11-16)
- Fixes: sync and ring from noise+saw sources (taken from the digital core); oscillator 1's one-cycle glitch
  of its source oscillator 3; the pull-down's pulse gate at the rise from whole cycles; the Model A
  pull-down followed landing by landing above one cell per step (rises, writes, corrections); predictions
  across a write (pending pull-downs and resets, offU rescaled with the frequency, ring state after the
  sources' pull-downs); two lines crossing at the same instant (an inverted ring); a pulse edge lost after a
  reset reached late (a stuck gate).
- Hysteresis of the Model A converter bits: taken back below half the limit; seeded afresh on a waveform
  change.
- Fades of a deselected output on the long kernel.
- Tests: random write schedules against the chip's own output stream (`[schedule]`, noise combinations,
  sync, ring, test bit) with two regression cases; noise combinations in the random-patch test; fades and
  per-cycle noise combinations in the write-discontinuity gate; `[dac]` tightened (from the second frame,
  tolerances just above the model's figures, saws only); the short-kernel golden replaced by an
  independent model of a free-running saw (`[kernel]`); hysteresis cases of the review.
- `Chip::renderLargeCorrections()` (corrections above 1e-4 of a period) and a test with writes a few cycles
  into a sample (`[corrections]`); `chip_bench`: a noise+saw source case and a control-write-per-tick case.

### B-Side S2b-2 review fixes (2026-11-15)
- Fix: sync and ring from a Model A saw combination (the line simulation now applies the pull-down).
- The stronger kernel (8 zero crossings, Kaiser beta 12, double precision) for every pulse and combined-
  waveform step on both models; `BasicBlepTable` takes the sample type too (Vintage unchanged).
- Gates extended: pulse widths 1 to 4095, combined waveforms at four widths, combined sync targets, sync
  chains, ring on combined targets; a second, independent measurement (Kaiser window, 20 fundamentals);
  comparison with the chip's own output stream; converter levels and harmonics against the reference;
  no corrections over random patches; physical peak; timing through `render()`; smooth start; hysteresis.
- Output pre-filled at activation; hysteresis for the Model A converter terms; tables built in place;
  BLEP tables fetched in `prepare`; `chip_bench` cases of the review.

### B-Side S2b-2 follow-up: a stronger kernel for Model A's combined waveforms (2026-11-15)
- `common/dsp/BlepTable.h`: `BasicBlepTable<zeroCrossings, betaTenths>`; `BlepTable` stays Vintage's
  8 / 8.0 (bit-identical tables; render goldens, alias gates and `voice_bench` unchanged).
- B-Side: steps of Model A combined waveforms use 8 zero crossings, Kaiser beta 11 (with a second-order
  correction of the table interpolation); every other step keeps the shared kernel, unchanged bit for bit
  (golden test). Latency stays 8. The three Model A combined rows are normal alias gates now (12 dB inside).
- `chip_bench`: a Model A combined case (the long kernel's cost).

### B-Side S2b-2 — band-limited waveform rendering (2026-11-15)
- `Chip::render` / `Chip::renderUntil` (`source/bside/chip/ChipRender.cpp`, design in `Chip.h` "RENDERING"):
  per oscillator the band-limited waveform after the waveform converter, times the envelope converter,
  summed (filter, mixer, volume and board filters follow in S3). BLEP/BLAMP from the shared table at every
  edge, reset and write, placed at its exact sub-sample time; combined waveforms as exact band-limited
  staircases; noise as band-limited sample-and-hold; sync resets predicted from the source's line; Model A
  pull-down predicted; writes split a sample at their cycle (`renderUntil`). Latency 8 internal samples.
- `source/bside/chip/RenderTables.*`: converter model (resistor ladder; Model A ratio 2.2 unterminated,
  Model B ideal) and the combined-waveform step tables, built at prepare.
- Tests (`ChipRenderTests.cpp`, `RenderReferenceTests.cpp`): alias budgets of 8.4.5 at every internal rate
  of 4.4 (all rows pass except three Model A combined shapes: a known shortfall, isolated to the BLEP
  kernel, test marked should-fail); digital state after rendering equals stepping (random writes, both
  models, any split); bit-identical audio across block sizes; state independent of the internal rate; no
  allocation; exact write timing and latency; noise spectra within 0.18 dB of the reference (allowed 1.5).
- `chip_bench` (tests/bside/benchmarks): ns per internal sample per chip, typical and worst case.
### M7f editor polish (2026-11-15)
- Our own paint code allocates nothing, in any component (strings, fonts and paths are made when a value or size
  changes; text layout inside the framework's renderer is not counted, 9.5). The one known exception is the dashed
  outline of the external-change marker, built in `paint()` for the 2 s a marker shows. `Slider`,
  `SegmentedSwitch` and `Toggle` lay their label, value and captions out on change (`PreparedText::setLine`
  reproduces `Graphics::drawText` exactly); `Theme` owns the paint fonts; `Panel`, the page-rail icons, the
  frame, the help bubble, the Browser and Sound pages and the preset name button make their strings, fonts and
  paths once. New test `PaintAllocationTests` walks every component of every page (both parts, four states, and
  with the output popover, help bubble and hover help shown) and checks that no marker is showing.
- Accessibility: the Browser grid (list; press loads), category chips and tone source switch (groups with
  a settable value), the arp mode and pattern switches (named), a spoken name for a pressed key ("C4,
  Upper"). New `AccessibilityInventoryTests`: every clickable component has a role, a name, keyboard
  focus and a place in the Tab order, or an allow-list reason.
- Keyboard strip (owner decision): a held key is tinted in the holding part's colour (Dual: both, Lower in
  the upper half), a sounding note is a small bar with a 2 px dark outline (white keys: a row of its own above the
  octave names, so it never meets one; black keys: at the foot); no dots or rings. Pixel, contrast and geometry
  tests of the rendered strip. A key pressed is announced to screen readers on the press and where a glide ends.
- Theme tokens for repeated radii, picture alphas, the white-key colour and key mixes, the category hue
  parameters and the preset button's font sizes (values unchanged; all pictures compared byte for byte).
- One shared no-op graphics context for the UI tests.

### B-Side S2b-1 second review: stronger tests (2026-11-15)
- Exemption budget is a golden: equality, unknown and duplicate keys fail, a bless run fails after writing.
- New measurement `s_latch_pn_b`: pulse with noise leaves the Model B latch alone (60 of 60); a test runs
  the chip against it. The `s_latch_b` test runs the chip too and pins how many reads it reproduces.
- Allocation test counts the chip's construction; docs: wave::shape is the only valid shape read (assert
  for pulse with noise), renderers split the sample at the restart calls; decisions corrected, the
  reviewer's latch hypothesis recorded.

### B-Side S2b-1 review fixes and contract decisions (2026-11-15)
- Pulse with noise is now a table of the noise output (Tpn, contract decision A), fitted with the same
  network; new measurement plans `s_pn_noise_*` / `s_pn_index_*` (one noise output across 63 indices; two
  indices across many noise outputs). `c_c_*` now agree at every exempt checkpoint.
- The fit is a fixed point of the documented procedure (`bside_shapefit fit` iterates; `verify` checks);
  parameters `constexpr`/`constinit`; bits 3..0 constrained (inputs and runs from above) with a
  plausibility test; tolerances tightened; margin test at 1e-6; `tests/bside/shapes/** -text`.
- Exemption budget per `c_*` trace and field (`tests/bside/trace/exemption-budget.txt`, decision B):
  81 of 6 850 exempt field checkpoints differ (placeholder 1 752).
- Model B latch experiment `s_latch_b` (question C): results pinned by a test; reported, chip unchanged.
- Chip: `powerOnKeepingTime()` and `clearEnvelopeCounters (n)` for the program engine, with tests.

### B-Side S2b-1 — our own combined-waveform tables (2026-11-15)
- `source/bside/chip/CombinedWaveforms.*`: a parametric bit-line network model per chip model and
  waveform combination (pull-up/pull-down conductance per selected waveform, neighbour coupling, a
  common pulse node, per-bit slopes, threshold; 11 numbers at most per combination), solved exactly per
  index; the 16 x 4096 step-2 shapes per model are computed from the parameters once per process at the
  first `prepare` (about 10 ms, static storage, no heap). Noise with two or more of triangle, saw and
  pulse, and pulse with noise, use one fitted constant per combination. `CombinedWaveformParams.cpp` holds
  the fitted parameters (generated, hexadecimal literals).
- The chip uses the tables wherever 8.15.2 (c) step 2 does (output, read value, Model B latch, Model A
  pull-down, noise write-back) through `Config::shapes` (`ShapeSet::fitted` by default); the S2a AND
  placeholder remains only as `ShapeSet::placeholder`.
- Measurement data `tests/bside/shapes/` (33 traces and references, own MANIFEST): sweeps, sync-reset
  spot samples and noise samples on oscillator 3 read back through 0x1B (bits 11..4), generated by
  `bside_shapefit gen` and fitted by `bside_shapefit fit` (`tests/bside/tools/ShapeFit.cpp`, JUCE-free,
  deterministic). Fit per full table: Model A 94.5-99.8 %, Model B 91.9-98.8 % of the entries exact in
  every observed bit (details: `docs/decisions.md`).
- `bside_trace`: the shape data (integrity, the pulse mask, the noise-with-pulse finding, fit tolerances
  per plan) and a before/after replay of every `c_*` trace: the exempt field checkpoints that differ from
  the reference fall from 1 752 (placeholder) to 111 (fitted) of 6 850. `bside_unit`: pinned table hashes,
  decision margins, the chip following the tables at every place, fast path = stepping for all 15
  waveform values. The naive stepper reads the same tables (data, not logic).
- New test executable `bside_independent` (in `scripts/sanitize-targets.sh`): the independent model's
  tests, built with the placeholder shape set because that model has no table interface yet.
### B-Side S2c: the patch program engine (2026-11-15)
- `source/bside/engine/`: `ProgramEngine` (the voice's tick clock, seven lanes and every command of 8.6.3, note-off
  and `release`, hard restart with the Clean, Precondition and Authentic policies, `firstwave`, macros, Smooth and
  Authentic resolution), `TickClock.h` (the tick period of Frame, Free and Tempo), `ProgramSink.h` (what the engine
  writes), `RegisterRecorder.h` (the sink of the golden traces), `ChipProgramSink.h` (the same writes into the
  real chip). JUCE-free, no allocation after `prepare`. Judgement calls: `docs/decisions.md`, "B-Side S2c".
- Tests: unit tests per command and edge, golden write traces (`tests/bside/golden/`, 72 files, compared as exact
  text; four tables at tick.mult 1 hand-derived and never regenerated, the rest model-derived by
  `tests/bside/golden/tools/model.py` through `gen.py`), block-split independence (including host and tick-period
  changes between chunks), the engine on the real chip against the replayed trace at many checkpoints (steals
  included), the Smooth path of the chip sink, property tests on random tables (ranges, no allocation).
- After review: once the note is off the table cannot raise the gate; Authentic does not apply the restart section;
  `firstwave`'s override ends with the table's first control write or the note-off; an inactive oscillator that is
  the sync or ring source of an active one gets its frequency; `ChipProgramSink` keeps the engine's absolute time
  across a chip reset; bounds checks for corrupt programs; `RegisterRecorder` moved to the tests.
- `ChipProgramSink` uses the chip's restart entry points: a steal is `powerOnKeepingTime()` (the cycle offset of the
  first version is gone) and Clean's direct clear is `clearEnvelopeCounters` on all three oscillators; a real test on
  the chip shows the attack then starts from a cleared rate counter, and the live-versus-replay test applies both
  markers. Engine and golden files unchanged.
### M7 Play page: the keyboard strip's keys play notes (architecture 9.3.4, 2026-11-15)
- `source/common/engine/UiNotes.h` (JUCE-free): a wait-free single-writer/single-reader queue of
  64 `{note, velocity}` from the editor to the audio thread, ons and offs in one FIFO. A note off is
  never lost: every note on reserves the slot of its off, and a note on that does not fit with the
  offs owed is dropped (the newest on). So an off is queued at once even if the editor closes right
  after. One acquire load when nothing was written. (Review fix: the first version kept full-queue
  offs in a side mask that could overtake their own note on.)
- Shell: the processor owns it (`getUiNotes()`); `processBlock` adds the notes as channel-1 note
  messages at offset 0 of the block's first chunk, before the host's events (zero-length blocks and
  blocks longer than the prepared size included); bypassed blocks, `prepareToPlay` and `reset`
  drain and drop them. Nothing changes with an empty queue (render regression unchanged).
- `KeyboardStrip`: a press on a key plays it, a drag glides (old off, new on), velocity 40 at the
  top of the key to 127 at the bottom; released on mouse-up, when the strip or its page is hidden,
  when the editor closes or stops showing (its 30 Hz timer), and by `cancelInteraction()`; a press
  that was ended that way plays nothing more until the next mouse-down. The split marker now moves only by its pill
  (drag by the grab point, double-click resets), arrows and wheel. Help text, page caption and hint
  updated. Tests: `tests/ui/UiNotesTests.cpp` (incl. a two-thread run), `tests/ui/PlayKeysTests.cpp`;
  the split-key tests now drag the pill and check that a press on the keys leaves the split alone.

### B-Side S2a fidelity pass — the digital core follows the per-cycle specification 8.15 (2026-11-14)
- `source/bside/chip/` rewritten to `docs/architecture.md` 8.15 (which wins over 8.4.3): power-on state
  (accumulators 0x555555, noise 0x7ffffe, envelopes 0xaa in release with a reset pending, Model B read
  latch 0x555); step order (a) oscillator advance, (b) envelopes, (c) outputs 1, 2, 3, (d) sync; the noise
  register's latch and 2-step pipeline after a bit-19 rise, the first-shift flag, the fill under the test
  bit and the shift when it is cleared, the write-back table of 8.15.3; the triangle `<< 1` shape, the ring
  fold with XOR NOT, the pulse comparison of the previous step, the Model A top-bit pull-down, the Model B
  read latch, the hold and fade of a deselected output; envelope phase, counter and divider pipelines with
  their precedence, the 32 767-position rate counter, the divider period as a state, lost gate-offs, the
  gate and rate-write rules; the timing constants per model. `stepCycle` stays normative; `advanceCycles`
  is rebuilt on the new state (per-oscillator jumps between sync-relevant rises, envelope jumps between
  non-trivial steps) and proven equal to it. The combined-waveform table stays the AND placeholder.
- `bside_trace` replays every committed trace through the chip (`replayChipExact`, `ShapeExemption.h`):
  exact at every checkpoint with the 8.15.8 exemption (shape-dependent fields marked from the writes and
  the chip's per-step events, counted per trace; a `d_*` trace with any exemption fails); the envelope
  traces also as Model A; a second chip driven by `advanceCycles` must equal the stepped one in its full
  state at every checkpoint. Result: 76 traces, 81 replays, 0 mismatches (`d_*` 20 075 checkpoints all
  exact, `c_*` 921 exact and 4 315 exempt, `a_*` 902 exact). The analog test stays skipped (no audio path).
- Tests: the row tests are rewritten per 8.15 rule (the 8.4.3 expectations that 8.15 contradicts are
  replaced); the naive stepper (`ChipStepperTests.cpp`) is rewritten from 8.15 and compares every state
  field; the five formerly ambiguous cases are kept as regression sequences; the equivalence tests compare
  the full new state and the fill events.
- Judgement calls and the specification gaps found: `docs/decisions.md`, "B-Side S2a fidelity pass".
- Review round 1: a combined-waveform table is used for two or more of triangle, saw and pulse, or pulse
  with noise (`wave::usesTable`); noise with only triangle or only saw is exact, so `c_9_*`/`c_a_*` are now
  exact tests of the write-back path (0 exempt, asserted, as for every `d_*` and `a_*` trace). An
  accumulator exemption also ends at a certain sync reset; the tracker keeps the noise register and the
  noise output apart. `[replay]` lines list per field the exempt and the differing checkpoints. New unit
  tests: the table rule, every way an exemption ends, the 8-bit envelope wrap (cycles named from
  `d_env_cases_b`/`d_env_rates_b`), the Smooth-mode latch-step copy into the noise output. `c_*` exempt
  checkpoints 4 315 -> 3 915 of 5 236; still 0 mismatches.

### B-Side S2a — the chip's digital core (2026-11-14)
- Digital unit gate by an independent test agent: `tests/bside/unit/ChipStepperTests.cpp`, a naive per-cycle
  stepper written from 8.4.1-8.4.3 alone, compared field by field with `stepCycle` and `advanceCycles` after
  every cycle/write and at random mid-gap points (10^7 cycles per model by default, `bside_unit
  "[.stepper-long]"` 10^8). No defect found; five ambiguous contract sentences are pinned by
  `[ambiguity]` tests with minimal sequences (both readings shown).
- `source/bside/chip/` (JUCE-free, no allocation, noexcept, trivially copyable): register file of 8.4.1,
  three 24-bit oscillators with a 32-bit fraction for Smooth input, sync (with the chain case), ring, test
  bit, 23-bit noise registers clocked on bit-19 rises with the combined-waveform write-back, three
  envelope generators (15-bit rate counters, exponential divider latched at the threshold values, delay
  quirk, freeze at 0, gate edges), the read registers 0x1B and 0x1C, `ChipClock` (32.32 cycles per
  internal sample), and `wave::` (the 12-bit waveform outputs S2b renders from).
- `Chip::stepCycle` is the normative per-cycle definition (header of `Chip.h`); `advanceCycles` is a
  closed-form fast path proven equal to it (10^7 random cycles with random writes per model in ctest;
  `bside_unit "[.chip-long]"` runs 10^8). An event sink reports sync resets and noise clocks at their
  cycle: the seam for S2b's band-limited rendering. No waveform rendering, filter, DAC or output yet.
- `source/bside/trace/ChipReplay.h`: the replay driver bound to the real chip (`setupChip`,
  `snapshotChip` in the 20-field `.bsref` order, `replayChip`); the stub-based replay tests are unchanged.
- Tests (`bside_unit`): every row of 8.4.3, the fast path against stepping (states and events), block
  size 1/7/61/512 and internal rate independence of the digital state, replay with the real chip.
- Judgement calls where 8.4.3 says "confirm" or is silent: `docs/decisions.md` (all review).
- Review round 1: events are self-contained (`SyncResetEvent` with the source's sub-cycle crossing and
  the target's phase, `NoiseClockEvent`, new `NoiseWriteBackEvent`); a sink must not read the chip; the
  noise events follow `Sink::kWantsNoiseEvents` in both paths; time, sample, envelope and event-order
  conventions in the `Chip.h` header, `ChipClock::position` for sub-sample positions. Accessors assert
  0..2; a default-constructed chip advances at the default clock and rate; Authentic rounds half away
  from zero; `setFilter` keeps the external-input bit at 0. New tests: sync chains in every rotation and
  all three together, no write-back under the test bit, every snapshot field (incl. the dividers) with
  its own value. Four rows in `docs/decisions.md` are now marked known open deviations (attack from 0xff,
  write and pipeline delays, test-bit noise reset, oscillator-3 read with no waveform).

### B-Side reference data: contract 8.15 and owner decision (2026-11-14, `bside/reference-data`)
- `docs/architecture.md` 8.15 "Amendments from the reference data": the per-cycle specification of
  the chip's digital core (step order, envelope pipelines and precedence, lost gate-off, sync after
  all outputs, ring fold XOR NOT source, Model B read latch from 0x555, test bit, noise fill and
  fade constants), the known reference artefacts and the analog reference conventions (985 248 Hz
  only, -40 dBFS floor, band 22 to 20 kHz). Earlier contract rows it replaces point to it. An
  independent per-cycle model written from the text matches every checkpoint of every committed
  `d_*` trace.
- Owner decision "follow, own tables": the digital core follows the reference's per-cycle
  behaviour; combined-waveform shapes and analog constants stay our own fits. B-Side is therefore
  tied to GPL-compatible licensing (`docs/decisions.md`). Open: how combined-waveform checkpoints
  are compared (8.15.8).
- `main` (B-Side S1d) merged in; the reference data set itself is unchanged (76 traces, 3.93 MB).

### B-Side reference data, review round 2 (2026-11-14)
- Gap traces: lost gate-off and a step executed in another phase (`d_env_lost_gate_off_b`), the Model B
  read-back latch (`d_read_latch_*`), the envelope converter at all 256 counter values
  (`a_env_dac_*`); gate-sweep probes thinned from 7 to 5 cycles per edge. 76 traces, 3.93 MB.
- Shared frame analysis counts bins below 20 kHz only (band 22 = 16..20 kHz on both sides); the
  `.bsspec` files are regenerated with it. The estimator checks that its plateau iteration converged
  and searches the resonance peak from an octave below the corner.
- The adapter rejects traces that reach a known reference artefact (sync from an oscillator whose
  test bit was set in the cycle its top bit rose); none of the committed traces does.
- Decision rows corrected: ring fold (own top bit XOR NOT source), sync after all outputs, the
  Model B read latch, envelope precedence, lost gate-off, exact run-past-end formula, -40 dBFS
  comparison floor, how Model-B-only envelope traces are replayed for Model A.

### B-Side reference data, review round 1 (2026-11-14)
- **Data set extended and split** (71 traces, 3.83 MB): every combined waveform in its own `c_*`
  trace from a test-bit reset; new digital traces (deselected-waveform fade, pulse width and frequency
  extremes, ring with pulse/noise, sync from/to frozen or test-held voices, gate edges swept over
  every rate-counter position at divider periods 1 and 2, more envelope cases, seeded random
  catch-alls); new analog traces (filter modes, routing and oscillator-3 off, envelope and waveform
  converters); Model A filter segments 1.2 s. Envelope traces for Model B only (the script proves
  the Model A copies identical).
- **Rate field one to one:** the pending rate-counter reset is bit 7 of `div` (no format change).
- `source/bside/trace/AnalogMeasure.*`: **one shared analog estimator** (corner, plateau, resonance
  peak, tone level) with tests; Model A corners now within 2.4 %, peaks within 0.14 dB of 4-second
  measurements.
- Reference resampler alias-free in 0..24 kHz (stopband 24 kHz, was 28 kHz); reference conventions
  (time base, scale, power-on, run-past-end) written down in `docs/bside-oracle-adapter.md`; the
  decision rows rewritten one per question, with precise delays and the board filter's quantisation.

### B-Side reference data (2026-11-14)
- The reference-emulator adapter exists (outside the repository, `docs/bside-oracle-adapter.md`) and
  wrote the **first reference data set**: 29 synthetic traces in `tests/bside/reference/` with their
  `.bsref` (digital state at 10 904 checkpoints: power-on, every waveform and combination, test bit
  and noise register reset, sync and ring, every envelope rate, envelope quirks, both read registers,
  both models, both clocks) and `.bsspec` (filter corner at 16 cutoff values, resonance 0-15,
  Model A distortion at three drive levels, voice DC steps, noise at 8 frequencies; both models;
  `*_bypass` twins for filter responses), and a `MANIFEST` of hashes. 2.08 MB.
- `source/bside/trace/SpecAnalysis.*` (JUCE-free): the measurement side of the spec format (frames
  as the render harness, 1/12-octave segment spectra, the segment plan derived from a trace's writes,
  since the trace format has no analysis plan). The adapter compiles it, so both sides measure alike;
  `render_regression` checks it against the harness (at most one 0.1 dB rounding step per cell).
- `bside_trace` (ctest label `trace`): MANIFEST hashes, canonical files, reference and spec belong to
  their trace, segment plan; the replay and analog-tolerance tests report themselves skipped until
  the chip exists. `bside_trace` is now a required sanitizer target.
- Contract questions from the data (combined waveforms in the digital state, rate-counter wrap,
  divider rule, pipelines, test-bit noise fill, power-on state, board high-pass, clock-scaled
  reference filter) are in `docs/decisions.md` (review) for the owner and the S2/S3 contract.
### M7c — Play and Effects pages (2026-11-14)
- **Play** (global page, both parts side by side): key mode with a sentence that explains the
  chosen mode, a keyboard strip (six octaves coloured by the part that plays each key, bars naming
  each part's range, the split marker to drag, dots for held keys and rings for sounding notes from
  the audio feed), balance, Vintage, and a panel per part (voices, unison, glide, bend range, mod
  wheel, pan) with the inactive rules of 9.3.9.
- **Effects** (global): chorus, delay and reverb in signal order, each with its picture computed
  from the effect blocks' own constants (delay echoes, reverb tail, chorus tap delays); handles on
  the delay and reverb pictures write time, repeats, pre-delay and decay time; delay and reverb
  have a power switch (level 0 and back, no new parameter).
- New: `KeyboardStrip`, `DelayView`, `ReverbView`, `ChorusView`, `PowerSwitch`, JUCE-free models
  `KeyboardModel.h` and `FxPictureModels.h`; tests `tests/ui/PlayEffectsTests.cpp`.
### M7b — Envelopes and LFO pages (2026-11-14)
- **Envelopes page** for both parts: two ADSR pictures drawn from the engine's own envelope code
  (the real RC curves), each with four draggable handles (peak = attack, sustain start = decay and
  sustain, sustain end = sustain, release end = release; time labels under them), the four sliders,
  key follow, Envelope 1's direction (an inverted Envelope 1 shows a mirrored ghost under the axis),
  and "what the envelope does" (filter amount and source, pitch bend, velocity amounts). The panel
  titles name what each envelope drives and follow the filter source.
- **LFO page** for both parts: the LFO's waveform from the engine's own LFO on a time axis with a
  delay handle (end of the fade-in) and a cycle handle (sets the rate), waveform, rate and delay
  controls, and the four "the LFO moves ..." panels (pitch, pulse width, filter, volume); the pulse
  width controls dim while neither oscillator is set to Pulse.
- New shared pieces: `EnvelopeView`, `LfoView`, JUCE-free `EnvelopeModel.h` and `LfoModel.h`;
  `Panel::setText` repaints only its header.
- Tests (`tests/ui/EnvLfoModelTests.cpp`, `EnvLfoPageTests.cpp`): handle maths and label spreading,
  window choice, the envelope shape against a 48 kHz `Envelope`, the LFO preview against a direct
  `Lfo` run, automation follow of every control, handle and picture (host writes from another thread,
  programs, Init, tones, session restore), handle gestures, titles, ghost, dimming, repaint scope,
  paint-allocation stability, layout snapshots (100 % and 150 %, both parts).

### M7b review round 1 (2026-11-14)
- LFO picture: delay wording and fade-in wedge with the delay handle on its envelope line, "starts"
  marker, Random/Noise pre-rolled with an "Example" caption, windows up to 50 s with a keep-while-it-suits
  rule, contested drags show the user's value, "key released" below the axis.
- Tests: contested drags, window rule, captions, time labels, panel header and title, nothing repaints at
  rest, paint allocations through a context that draws nothing; M7a's repaint-scope test now records.
### M7e — Browser page, `PresetActions`, program menu (2026-11-14)
- **Browser page:** bank chooser, search, category chips (colour per category, "More..." when many),
  an 8 × 8 grid of the bank's programs (name, slot, category stripe; the current program outlined with a
  dot when changed; click selects, double-click or Enter loads, arrow keys move), and the selected
  program's panel: Load program, **Load tone** (take its Lower or Upper tone into Lower or Upper only),
  Save, Save as, Init, tone files for the edited part, Import and Export of banks. Follows program
  changes and bank changes live.
- **Program menu:** clicking the program name in the top bar (or Enter/Space on it) lists the bank's
  programs grouped by category, current one ticked, with "Open Browser" below.
- **`PresetActions`** (`source/common/presets/`): the file dialogs, menus and save/load flows moved out of
  `PresetBar`, shared by the bar and the Browser (a test dialog replaces the real one in tests).
- **`PresetManager`:** `slotMeta`, `bankGeneration`, `applyToneFromSlot` (and `slotValue`, see
  `docs/decisions.md`); no change to how programs reach the audio thread.
- JUCE-free logic in `common/ui/models/ProgramModels.h` (category groups, menu model, search, grid movement).

### M7a — custom editor: foundation, frame, Sound page (2026-11-14)
- The plugin opens its own editor (`source/vintage/ui/`): a 1360 × 784 canvas scaled as a whole,
  75–200 % at a fixed aspect (corner resizer and a Size menu), page rail, top bar (part switch with
  "Editing: LOWER/UPPER", the preset bar, output meter, Master, LIMIT chip and output popover with
  limiter switch, ceiling and reduction meter), help line and hover bubbles (texts embedded from
  `docs/ui/help-texts.md`), dark theme tokens, embedded Inter font.
- **Sound page** for both parts (orange Lower, teal Upper): oscillators with live oscilloscope
  pictures from the voice's own oscillators, link (sync, cross mod), mix, high-pass + filter with a
  draggable response curve computed from the engine's own filter maths (cutoff/resonance handle,
  high-pass handle, dashed other slope), amp with the part's signal path. Envelopes, LFO, Play,
  Arp + Seq, Effects and Browser are placeholder pages that list their parameters with plain
  sliders, so all 172 parameters are reachable now (real pages: M7b–M7e).
- **Live automation:** the editor polls every parameter on a vblank-driven tick (30 Hz timer
  fallback) and registers no parameter listener; every control, handle and picture follows host
  automation, MIDI, program changes and session restores within one tick; host gestures for every
  interaction; an "AUTO" marker shows changes by anything other than the editor.
- Shared component library `source/common/ui/` (JUCE-free interaction models, mirror, gestures,
  controls, graphs, frame, help texts, control registry) for both instruments.
- Processor: the block algorithm gains step 9, publishing the audio → editor feed
  (`common/engine/UiFeed.h`: peaks, limiter gain, held and sounding keys per part, tempo, block
  count); the session saves the editor's UI state as one `UiState` child (page, edited part, size, …;
  9.1.3). Proven: raw render output byte-identical at block sizes 512 and 61, parameter list
  identical, saved state identical except the new child, pre-M7 sessions load with defaults;
  `voice_bench` processor path +0.75 %, full plugin path +0.9 % (best of 3, within noise).
- Tests (`tests/ui/`): models and gestures, help texts, UI feed (incl. a seqlock stress), UI state,
  filter curve vs the real filter and oscilloscope equality, registry completeness, automation
  follow for every parameter, contested drags, editor closed mid-drag, open/close and two instances,
  a text layout snapshot of the Sound page; hidden `[.uishots]` (PNGs) and `[.uibench]` (budgets).
- The Inter font (SIL OFL 1.1) is in `resources/fonts/inter/` with its licence and source record;
  the release checklist ships its licence text with every binary.
- The local macOS gate runs pluginval with its editor tests; CI still skips pluginval's GUI tests on
  Windows and Linux.
- After review: the oscilloscopes run the voice's own oscillator configuration, pulse-width mapping
  and per-sample code (`vintage/engine/OscPath.h`, shared with `Voice`; audio byte-identical); the
  filter graph spans 5 Hz–20 kHz and states self-oscillation; the external-change marker has its own
  colour and a dashed outline; hidden pages end their gestures; accessibility basics (focus order,
  checked states, handle values, quiet meter); discrete wheel adds up fractions; segment
  double-click selects; Delete/Backspace no longer reset; a restored session applies its window
  size; duplicate `UiState`/`Preset` children of a hostile session are all removed.
### B-Side S1d: patch tables in presets and state (2026-11-14, `bside/s1d-preset-tables`)
- `presets::Tone` and `Program` carry table lines; `Instrument` gets an optional table validator, canonicaliser
  and limits (lines, bytes, file cap, JSON values). Vintage sets none: its program, tone and bank files and its
  saved state are byte-identical to `origin/main` (hidden `[.presetdump]` test, `diff -r`).
- Program files `"tables": {"lower": [...], "upper": [...]}`, tone files `"table": [...]`; a table the reader
  rejects rejects the whole file, message prefixed by part and program name. Tables are stored, saved and
  restored **verbatim** (comments, spacing, label names); the canonical form is only used to compare tables
  (the modified marker) and, inside the reader, to compile.
- The JSON writer writes tab, line feed, carriage return, backspace and form feed with the short escapes
  (a tab was six bytes; a legitimate table of tab-only lines made a 26 MB bank that `loadBankFile` refused).
  B-Side caps, from the worst case found by code (worst byte costs 2 bytes written for 1; 31-tab lines):
  12 MiB per file and 160 000 JSON values; the constructed worst bank is 9 884 536 bytes and 136 327 values
  (27 % headroom). Every save refuses a file larger than the instrument's own reader accepts (nothing written).
- Session state: child `Tables` (`lower`, `upper`, the user's text); a malformed table gives that part an
  empty table and `PresetManager::tableError(part)`; all duplicate `Tables` and `Preset` children are removed.
- `PresetManager`: `TableSink` (owned by the manager; `prepareTable` before the hold, `publishTable` and
  `emptyTable` inside it: the table part of the hold only swaps strings with the held ones and copies a program into a free buffer, `noexcept`; the parameter writes of an apply are as before), `setTable`, `currentTable`,
  `tableError`; the host is told "non-parameter state changed" when a table changes. Error messages are cut
  between UTF-8 characters.
- B-Side: `bside/program/ProgramHandover.h` (four buffers per part, wait-free audio side, protocol for two parts
  in one seqlock window documented and tested) behind `BSideProcessor::tableHandover(part)`. Its step hook
  lets `HandoverModelTests` explore every interleaving (24 564 states) and catch four broken variants.
- `readPresetFile` sizes its buffer from the file size instead of the cap.

### B-Side S1b review follow-up (2026-11-14)
- S1c merged in (`bside_unit`, `bs_juce_free`): `tests/bside/CMakeLists.txt` holds both test structures.
- Sanitizer target selection fixed: `ninja -t targets \| grep -q` under `pipefail` made every target look
  missing, so the sanitizer steps skipped tests silently. One shared `scripts/sanitize-targets.sh` (preflight,
  Linux container, CI job): required targets are an error when missing, unlisted test-looking targets are an
  error, and each step prints the tests registered per target before ctest runs.
- `preflight.sh` refuses `VS_PREFLIGHT_PLUGINS` other than `all` for the default and `--full` runs; the summary
  names the plugins covered.
- The JUCE modules of the test cores are compiled once (`vs_juce_modules`) and shared by `vs_test_core` and
  `bs_test_core`: cold test build 45 s wall / 460 s CPU before, 26 s / 319 s after on this Mac; the plugin
  targets' compile and link commands are identical, Vintage dumps byte-identical. Full gate afterwards (machine
  load about 3 to 5): total 17m25 (mac 3m07, linux 2m46, mac-sanitize 6m15, linux-sanitize 5m14) against
  14m05 for Vintage alone (warm).
- `getPresets()` throws `ConfigError` like the other preset uses (tested); `TestTonePart` applies an event at or
  beyond the block's end after the last sample and ignores a non-finite pitch bend (both tested); the dev host
  has no build dependency on the Vintage bundle; the CI distribution job checks both bundles in one container;
  `--skip-gui-tests` carries a TODO for the custom editor.

### B-Side S1b — the plugin shell and gates for two plugins (2026-11-14)
- **Skeleton tightened first** (`docs/architecture.md` 8.2.5): the plugin name comes from the
  instrument (`Derived::kName`); a missing parameter (`BoundParam::bind`) or a missing `initPresets`
  call fails at construction with a named `ConfigError` in every build type, and the plugin entry
  point `shell::createPluginProcessor<T>()` turns it into a message plus abort instead of an
  exception crossing the host boundary; `static_assert`s that the per-block hooks are `noexcept`.
  Vintage unchanged, proven on this Mac (universal Release build): raw `[.bitdump]` output of all 15
  scripts and the stress script at block sizes 512 and 61, the new `[.statedump]` (name, latency,
  tail, programs, all 172 parameters with default, steps and display text at 0, .25, .5, .75, 1) and
  the default saved state are byte-identical to the build before the change.
- **Exitium B-Side exists** as a second VST3 plugin (`source/bside/`, CMake option `VS_BUILD_BSIDE`,
  default ON): identity `Bsy1` / `com.exitium.bside` / manufacturer `Exit`, state tag `BSideState`,
  the 75 shared parameters, preset identity `b-side` (8 × 8, `.bsprog`/`.bstone`/`.bsbank`, folder
  `Exitium/B-Side/`) with one Init program, the shared editor, and a placeholder test-tone part
  (16-voice sine, 5 ms attack and release, latency 0). No chip sound yet (S2 to S4).
- Tests: `BSideTests` (own test core `bs_test_core`; never linked with Vintage's): the skeleton
  contract with a second instrument, the part, parameters, programs, state, bypass, guard, arp,
  preset files, no allocation in `processBlock`. `host_smoke --profile vintage|bside` and
  `host_smoke_bside`; `vst3_validator_bside`; `[.statedump]` in `render_regression`.
- Gates cover both plugins: `scripts/preflight.sh` (lipo, pluginval 10 and validator per bundle,
  sanitizers, `VS_PREFLIGHT_PLUGINS=vintage|bside|all`), the Linux container check, the docs check
  (the instruments include nothing from each other; `bside/chip|program|trace` JUCE-free),
  `scripts/dev-host.sh --plugin bside`, and `build.yml` (every row builds both; per-plugin
  pluginval, dependency check, artifacts, distribution tests, bundle merges). **`build.yml` has not
  run** (CI is offline); only its YAML and plan table are checked.
- Gate time on this Mac (full default gate, machine load 4 to 10 from other work, so indicative):
  Vintage only (before) cold 18m52 / warm 14m05; both plugins cold: mac 5m13 (before 4m17), linux
  3m35 (2m41), linux-sanitize 8m24 (4m55); warm: total 22m18 (mac 2m42 vs 2m13, linux 3m11 vs 1m44,
  mac-sanitize 5m46 vs 5m35, linux-sanitize 10m34 vs 4m30). The macOS build step is within the
  contract's targets (+22 % cold, +22 % warm against +50 % and +25 %); the whole gate is not
  (warm +58 %), because the Linux steps, above all the
  sanitizer build of a second JUCE test core, are not. The optional shared JUCE-module library
  (8.1.2) was not tried: it is the obvious next lever.

### B-Side S1c, JUCE-free formats (2026-11-14)
- `source/bside/program/`: the strict reader and compiler of the patch table text format (8.6.5),
  the compact `Program` (a fixed 5.9 KB trivially copyable struct, byte equality = program
  equality), and the canonical writer. Every limit and error class of the contract, errors with
  line and column, at most 8 messages. No runtime or tick engine yet.
- `source/bside/trace/`: the `.bstrace` trace, `.bsref` reference and `.bsspec` spec readers and
  writers (bounded, strict), SHA-256, the checkpoint schedule, the replay driver (template over the
  chip, proven against a stub) and the comparer with the 8.9.4 tolerance table.
- `tests/bside/`: new JUCE-free executable `bside_unit` (about 5 s to build and run), part of
  `ctest`, the sanitizer steps of `scripts/preflight.sh` and `scripts/linux-check.sh`. Spec tests
  for every command and error class, the four example tables, bounds tests for every limit, round
  trips, and a deterministic fuzz test (short in the normal run; 10^6 cases hidden under
  `[.fuzz-long]`).
- Review follow-up: the canonical text always reads back (reader rejects rows whose canonical form is over
  200 bytes; `validate()` checks every text limit); every cycle of rows without delay is an error and an
  accepted table runs at most 64 rows per tick; trace limits lowered to 100 000 writes so a reference
  file always fits its 16 MB limit and every writer refuses what its reader would; C1 controls and the
  byte order mark are rejected with their own messages; `replay()` verifies the trace SHA-256.
- `docs/bside-oracle-adapter.md`: the interface of the external reference-emulator adapter
  (nothing is downloaded or generated). `docs-check` now also keeps `source/bside/{text,program,trace}`
  JUCE-free and `source/bside` free of `vintage/` includes.

### B-Side S1a — shared processor skeleton (2026-11-14)
- Refactor, no behaviour change: everything instrument-independent in Vintage's processor moved
  into `source/common/shell/` (`docs/architecture.md` 8.2) so the second instrument can reuse it.
  `shell::BiTimbralProcessor` (CRTP base: block chunking, MIDI, Program Change hand-over, key router
  and both arp/seq instances, transport, the seqlock parameter read and preset release, key modes,
  mixer, effects, output stage, bypass, tail, programs, presets, state), `shell::InstrumentEditor`
  (preset bar above the generic editor) and shared helpers. The shared global parameters are
  appended by `addMasterVolume`, `addKeyMixParameters` and `addLimiterParameters`
  (`common/params/GlobalParams`). `VintageSynthProcessor` supplies its parts, per-part parameters,
  Vintage amount, synth tail, preset schema and state identity through per-block hooks.
- The render-regression harness renders through a processor factory (Vintage's default in
  `tests/render_regression/VintageInstance.cpp`).
- Proven unchanged on this Mac: the raw output of every render script and the stress script at
  block sizes 512 and 61 is byte-identical, as are the parameter list (IDs, names, ranges,
  defaults, steps, display texts), saved states, the `state_m5.bin` fixture load, and saved
  program, tone and bank files; `voice_bench`/`fx_bench` within noise. No test changed.
- The docs check's include rule also names `bside/` and states that it covers `common/shell/`.

### M7 editor contract (2026-11-14)
- `docs/architecture.md` section 9 is the contract for the custom editor (M7a–M7f): architecture and
  threading, the live-automation rule, the shared component library, theme and scaling, page
  specifications placing all 172 parameters, derived pictures, budgets and tests. The approved
  wireframes are in `docs/ui/wireframes/`, the help texts (checked against the engine) in
  `docs/ui/help-texts.md`; the manual checklist has a "Custom editor" section. Documentation only.

### B-Side S1 contract (2026-11-14)
- `docs/architecture.md` section 8 is now the contract for the second instrument's S1–S4: the
  shared processor skeleton (S1a), the plugin shell (S1b), table and trace formats (S1c), the chip
  model, polyphony, the patch program engine with its text format, the parameter list, presets,
  the test and authenticity plan and the milestone breakdown. Judgement calls in `docs/decisions.md`;
  owner decisions pending (Q1–Q8). Documentation only.

### VCF slope switch without a click (2026-11-13)
- Changing the VCF slope while notes sound (by hand, by automation, or by applying a program such as
  the factory −12 dB programs A7 B3 B4 B7 C5 D7 F4) no longer steps the output: sounding voices
  crossfade between the filter taps over 5 ms. Steady sound is bit-identical; CPU cost unchanged.
- The program-apply click test covers −12 dB programs again (both directions); a new Part test flips
  the slope on a held chord at every host rate and checks block-size independence.

### M6b — the factory bank (2026-11-13)
- 64 designed factory programs (A1–H8) replace the 8 placeholders: brass, strings and pads, keys,
  plucks and bells, basses, leads, organ, percussion and sound effects, Dual layers and Splits,
  arpeggiator and sequencer programs, effect showcases. Specification, level rule and feature
  coverage: `docs/factory-sounds.md`.
- Levels matched per group (sustained, percussive, bass): every program's standard phrase is within
  2 dB of its group's median, and no full-velocity eight-note chord is more than 3 dB above Init's
  or reaches 0 dBFS (measured with the output limiter off). With the limiter on at its defaults, no
  program's standard phrase reaches its knee. Enforced by tests, together with: every program
  sounds, stays finite and falls silent once its reported tail has passed.
- Hidden audition render (`Render m6b_factory*`): one WAV per program, the concatenated audition
  with an index, and a measurement table (peak, RMS, attack, release tail, spectral centroid,
  stereo width).
- Render-regression references for seven designed programs replace the placeholder ones.

### Output stage: headroom and soft limiter (2026-11-13)
- A fixed −6.02 dB output trim: the loudest chord (Dual, 32 voices, full velocity, dry) now peaks
  at −2.57 dBFS at the default master volume (was +3.45 dBFS). Everything is 6 dB quieter; the
  relative levels of programs and effects are unchanged. **Sessions saved earlier play 6.02 dB
  quieter.** Golden renders re-blessed: the raw renders are exactly the old ones × 0.5, and 13 of
  14 reference files shift by exactly the trim; `selftest_basic` also took the within-tolerance
  drift it had gathered since its last bless (e3de0bd, M2b). New golden scene `limiter_drive`.
- A soft limiter on the output, on by default: stereo-linked, no added latency, transparent below
  its knee, never above the current (smoothed) ceiling. Two parameters, `master.limiter` and
  `master.limiter.ceiling` (−12 … 0 dB, default −1 dB); session settings like master volume, not
  part of programs. Switching it on or off is crossfaded over 5 ms. 172 parameters in total.
- A session that lacks a parameter (saved by an older version) loads it at its default.

### M6a — presets and banks, infrastructure (2026-11-13)
- Tones, programs and banks as text files; a read-only factory bank (8 placeholder programs for
  now); loading and saving from a small preset bar; MIDI Program Change; host program list;
  sessions remember the current program.
- Preset files and session data are treated as untrusted input: strict reader, bounded file
  access, no file access while a session loads.

### M5 — effects and arpeggiator/sequencer in the plugin (2026-11-12)
- Chorus, delay and reverb on the stereo bus (11 parameters); arpeggiator and 16-step sequencer in
  front of each part with a part selector, locked to the host tempo (58 parameters). 170 in total.
- Host bypass is a clean stop; non-finite parameter values are sanitised; integer parameters are
  stepped for hosts; the reported tail includes the synth's release.
- Hard sync with a sine VCO-1 no longer aliases (−109 dB or better, was −43 to −60 dB).
- Offline renders of Cross Mod patches now match real-time playback.

### Local-first verification (2026-11-12)
- `scripts/preflight.sh`: one local gate for everything a Mac can check, including the VST3
  validator built from the pinned SDK commit, the security scan and repository hygiene checks.
- CI no longer runs automatically; it is started by hand as a confirmation for Windows and Linux x86_64.

### Linux ARM64, distro load tests, local Linux check (2026-11-12)
- Linux ARM64 is a supported, CI-validated target; x86_64 and ARM64 ship in one Linux bundle.
- CI load-tests the Linux x86_64 build with pluginval in containers of Ubuntu 22.04 and 26.04,
  Debian 12 and 13, Fedora and Arch, plus a negative test below the glibc floor.
- `scripts/linux-check.sh`: GCC build and tests (or sanitizers) in a Linux container on a Mac.

### M3 — bi-timbral (2026-11-12)
- The product is now called **Exitium Vintage Synth** (name in hosts and bundle file name;
  plugin codes and identifiers unchanged).
- Upper part with Whole, Dual and Split key modes; a key router that can never hang a note when the
  split point or mode changes; part balance and per-part pan; true stereo output. 101 parameters.
- Oversampling follows the host rate (4× below 44.1 kHz, 2× up to 88.2 kHz, 1× above): 32 voices
  cost about 5 % of a core at 48 kHz and about 10 % at 192 kHz.
- Exact reported latency (6, 5 or 8 samples by configuration); accurate long envelopes and LFO
  timing at high sample rates.
- Known issue: hard sync combined with audio-rate cross-mod aliases above the −100 dB gate.

### M4 — arpeggiator and step sequencer module, not wired in yet (2026-11-12)
- Arpeggiator (Up, Down, Up & Down, Random, As Played; 1–4 octaves; hold; swing; gate length) and a
  16-step sequencer (offset, Note/Rest/Tie, velocity), locked sample-accurately to the host tempo
  or an internal clock. Standalone module in `source/common/seq/` with tests; connected in a later step.

### Linux target and public-release preparation (2026-11-12)
- Linux x86_64 VST3 is a supported target: built in CI on Ubuntu 22.04 (glibc 2.35 or newer, static
  C++ runtime) with the unit tests, host smoke test, render tests, VST3 validator and pluginval.
- A CI job runs the tests under AddressSanitizer and UBSan on every push.
- pluginval downloads are verified by SHA-256; CI artifacts are kept for 7 days.
- `SECURITY.md`, `CONTRIBUTING.md` (contributions under AGPLv3 only) and a pull request template.
- `scripts/dev-host.sh` returns to the terminal after launching the host.

### M2 — polyphony (2026-11-12)
- 16 voices per part with Poly, Unison (2/4/8/16 voices, detune) and Solo assign modes; voice
  stealing with a 3 ms fade; sustain pedal; portamento (Always/Legato); velocity to VCA and VCF;
  bend range 0–12 semitones; mod wheel vibrato. Nine new parameters per part (96 in total).
- MIDI is parsed once into note events; every event is applied exactly once; All Notes Off also
  clears the sustain pedal; malformed messages are dropped.
- Voice engine about 2.3× faster: 32 voices cost about 5 % of one core at 48 kHz (was 11.3 %).
- Renders are independent of the host block size, also at full Vintage and with reused voices.
- Tests: allocator unit tests, 39 black-box tests written from the specification, sanitizer-clean.
- Only the Lower part plays until M3.

### M5 — effect blocks, not wired in yet (2026-11-12)
- Chorus (two modes, mix up to wet = dry), stereo/ping-pong delay with tape-style time glide, and a
  16-line hall reverb, as standalone JUCE-free blocks in `source/common/fx/` with tests and `fx_bench`
  (all three: about 0.55 % of one core at 48 kHz). They are connected to the plugin in a later step.

### Developer tools (2026-11-12)
- `scripts/dev-host.sh` builds the plugin and JUCE's AudioPluginHost and opens the host with the
  synth already wired, for hands-on testing. Manual checklist in `docs/ui-test-checklist.md`.

### Wave 1 — test infrastructure and CI cost (2026-11-12)
- Golden-render regression harness (`tests/render_regression/`, ctest label `render`): text event
  scripts rendered through the real processor and compared by perceptual features, not samples.
  One self-test reference; preset goldens follow in M6.
- Steinberg VST3 validator runs in CI on macOS and Windows x64 (ctest `vst3_validator`), built from
  the SDK pinned at `v3.8.1_build_84`.
- CI cost control: docs-only changes don't build, draft pull requests build Windows x64 only,
  job timeouts, monthly Dependabot.

### Wave 0 — contracts and orchestration (2026-11-12)
- `docs/architecture.md`: contracts for M2–M5 (note events, voice allocation, new per-part
  parameters, CPU targets, effect block interface, arp/sequencer and test-infrastructure outlines).
- Development workflow: agents on feature branches, pull requests for CI, owner approval per
  milestone (skill `orchestrate`).
- Compiler cache verified on a warm CI run: macOS 3.5 min, Windows x64 6.7 min, Windows ARM64 5.9 min.

### CI and project infrastructure (2026-11-12)
- CI builds macOS universal, Windows x64 and Windows ARM64 on every push and pull request, runs the
  unit tests and the VST3 host smoke test on all three, and pluginval (strictness 10) on macOS and
  Windows x64. Windows x64 and ARM64 are merged into one `.vst3` bundle.
- All GitHub Actions updated to their latest releases and pinned by commit SHA; runner images
  `macos-26`, `windows-2025`, `windows-11-arm`, `ubuntu-26.04`. Read-only default token permissions.
- Windows CI no longer hardcodes a Visual Studio version (the runners dropped VS 2022).
- Faster CI: link-time optimisation only on the shipped plugin (tests use a non-LTO build of the same
  sources), Ninja everywhere, and a compiler cache (ccache on macOS, sccache on Windows).
- Dependabot: weekly updates for GitHub Actions and the JUCE/Catch2 submodules.
- Trivy repository scan (vulnerabilities, secrets, misconfigurations) on push, pull request and weekly.
- README, CHANGELOG, design decision log (`docs/decisions.md`) and lessons learned
  (`docs/engineering-notes.md`).

### M1 — single-part voice engine (2026-11-12)
- Two VCOs (sine, triangle, saw, square, pulse, noise) with windowed-sinc BLEP/BLAMP anti-aliasing
  (≤ −113 dB aliasing up to a 5 kHz fundamental), sub-sample hard sync, exponential cross-mod, and
  VCO-2 Lo Freq mode.
- Non-resonant HPF; 4-pole ZDF OTA-ladder VCF with −12/−24 dB slopes and self-oscillation; VCA with
  4-step LFO tremolo.
- Two RC-style ADSR envelopes with key follow; ENV-1 polarity. LFO with six waves and delayed fade-in.
- 2× oversampling in real time, 4× offline; latency reported to the host.
- All `lower.*` and `upper.*` parameters (78 in total with master volume and Vintage), generated
  from one list. Only the Lower part plays, monophonically, until M2/M3.
- Global Vintage amount: seeded per-voice pitch drift, tuning, cutoff and envelope tolerances.
- Sample-accurate, allocation-free MIDI handling; output NaN/Inf guard and clamp.
- Tests: DSP unit tests, part behaviour, VST3 host smoke test, CPU benchmark, audition render.

### M0 — project skeleton (2026-11-12)
- JUCE 9.0.5 (AGPLv3) and Catch2 v3.16.1 as pinned submodules; CMake + Ninja; C++20.
- Plugin identity: Exitium, `com.exitium.vintagesynth`, manufacturer `Exit`, plugin `Vsy1`. VST3 only.
- Project rules (`CLAUDE.md`) and skills for build, validation, parameters, real-time review and release.
