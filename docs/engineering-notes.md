# Engineering notes — lessons learned

Things that went wrong or surprised us, the fix, and the rule that follows from each.
`docs/decisions.md` records *what* we chose; this file records the *traps*. Newest milestone first.

## B-Side render cost step (2026-11-18)

| Problem | Cause | Fix / rule |
|---|---|---|
| The first batched renderer missed a pulse edge for a whole period in a third of random patches; every existing test passed. | It cached the next event as a phase limit, and the finish wraps a pulse line into [0, 1) after the event: the limit stayed in the old coordinates. | A cached event limit lives in the line's coordinates: follow every wrap and re-derive after any correction. Prove a fast path against the slow one bit for bit (a test aid that switches it off), not against tolerances. |
| The batched path took three large corrections in 50 000 long-run patches that its own equivalence test never showed. | A time-based event (a scheduled Model A landing) was converted to a phase with the sweep time 0 instead of the time the line is at; the random generator rarely left a landing pending across a sample boundary. | Convert time-based events at the line's own time; give the equivalence test a generator that makes the rare state (slow pulling oscillators, control writes inside the upper half). |
| Coarse cells of a combined waveform moved the audio band by -26 dB although their cell means preserve the low harmonics. | A staircase of N cell means folds the shape's harmonics near N back onto the low ones (sample and hold of a box-filtered shape); our shapes' harmonics decay slowly. | Build a coarse staircase from the fine spectrum (harmonics below N / 2 matched exactly), not from means. |
| A finer linear kernel table made the narrowest pulse alias more (margin 5.3 -> 3.8 -> 3.4 dB at 512 Hermite, 2 048, 4 096 points). | The pulse's two edges, one cycle apart, nearly cancel; a linear read's error depends on the sub-sample offset and does not cancel between them, a smooth (cubic) read's does. | Keep the Hermite read for pulse edges; a linear refined table only for steps that do not come in cancelling pairs. Check the closest alias rows after any kernel change, not only the gate's pass. |
| The digital core got 3.5 ns slower per call after a small addition to `skip`. | The compiler stopped inlining `envAdvance` into the grown function. | Force the hot small helpers inline (`BS_CHIP_INLINE`) and move rare branches out of line; measure the "digital core only" rows after touching `skip`. |
| A table builder's `std::vector` failed "building the tables and preparing a chip allocate nothing". | The render tables promise no allocation; only the shared BLEP tables are exempt. | No heap in table builders: fixed arrays, or a direct computation (the coarse levels' spectrum up to harmonic 64 directly instead of an FFT). |
| The state check missed a stuck pulse gate for good (8.17 defect 1). | It skipped every line within three cycles' advance of a boundary; a frozen line sits on one for ever. | Skip only for a stated reason that ends (here: three cycles after a reset); a frozen line is checked wherever it sits. |
| Profiling inlined code with `sample` gave one number for the whole batched loop. | It reports functions, not lines. | Profile one case at a time (`chip_bench <case>`, `CHIP_BENCH_SECONDS=200`) and confirm a stage's share by an experiment that removes it (an early return in the emit) before optimising it. |
| A closed form divided by the increment and crashed on x86-64 at frequency 0; arm64 returned 0 and every test passed. | ARM64's integer division by zero yields 0 without a trap. | Run the sanitizer build (UBSan `halt_on_error`) before handing over any arithmetic on frequencies, widths or rates; test frequency 0 and increments below one accumulator step. |
| A "one cycle later" gate re-read never happened on a frozen line (8.17 defect 1 and its pull-down sibling). | It was scheduled as a phase, which a line with slope 0 never reaches. | Schedule a delayed event in time when the line may not move (`renderScheduleReread`); audit every phase-scheduled event for slope 0. |
| Coarse versions of a waveform clicked at every switch and leaked into the band once gated, synced or pulled down, although each was exact on its own. | A switch changes the waveform itself; a band-limited step only stops the aliasing of that change. Multiplying or cutting a staircase whose images lie above the band brings them down into it. | Above a limit, keep the waveform and change only how its steps are emitted (step moments); measure a cheaper method against the exact one in the band, near switches and with gates, sync and pull-downs, not only steady and alone. |
| A cost saving that leaves out content above 22 kHz still changed the band by -62 dB. | The left-out square waves are cut at phase jumps and modulated by per-sample envelope steps, which brings their energy down. | Measure an "inaudible" omission through writes (phase resets, waveform switches, gate toggles), not only on steady tones. |
| Two mutations survived every test however the tests were sharpened. | They were equivalent: a write always leaves its sample to the general path, and a rounding margin only matters on ties of about 1e-16. | Before adding a test for a surviving mutation, decide whether it is equivalent; if so, say why and keep the code as a defence. |
| The rendered output depended on where the caller's `renderUntil` stopped, also where no write followed (third review: 7 of 150 random cases, differences up to a large correction). | A piece (the renderer's unit of line simulation: pull cells, glitch carries, crossings, the step moments' stretches) ended at every stop, so its decisions were taken from where the caller happened to stop. | A stop that is not a write must not change the output: pieces start at a sample's start or at a write and end at the sample's end; a stop inside one only advances the digital core. Test splits at non-write cycles (random stops 1 to 3 000 cycles ahead, below and above the register's top) against the same case without them. |
| No test failed when the batched path dropped a pending gate re-read, or when `envInert` ignored a pending counter step (an earlier review called the latter equivalent). | Every write in the tests landed at a sample's start, so the event and its one-cycle delay fell into one sample, handled by the general path; `renderUntil` to a sample's end keeps that sample in progress, and the batched path takes over right after it. | Put writes 0 to 2 cycles before a sample's end (`renderUntil` to it) in every test of a one-cycle delay or a hand-over between the general and the batched path. |
| A renderer artefact was written down as a property of the chip ("synced targets cannot meet -80 dB"), and quiet shapes looked clean. | The measurement used an exact integer source/target ratio, where an older artefact of the renderer lives, and never measured the chip's own output; its "strongest line" included DC's leakage into the first bins. | Before calling an effect inherent, measure the chip's own per-cycle output the same way; vary ratios off integers; take the reference line outside DC's main lobe. |
| A divide by a zero increment passed every test on arm64 and was caught only by UBSan. | arm64 returns 0 for an integer division by zero without a trap; only x86-64 raises SIGFPE. | The frequency-0 regression test proves the guard only in the sanitizer build (UBSan `halt_on_error`) or on x86-64; say so at the test. |
| A shell command with a compiler include flag ending in the source directory's name was refused by the worktree guard. | The guard reads the word as the shell's `source` command. | Put such commands in a script file (in the scratchpad) and run the script. |

## B-Side parameter step (2026-11-18)

| Problem | Cause | Fix / rule |
|---|---|---|
| The contract's order mixes float, integer and choice parameters, so Vintage's two lists (all floats, then all choices) would give another order. | Vintage's X-macro lists are split by type, which fixes the order to floats first. | One list with three entry macros (`BS_PART_PARAMS (F, C, I)`), written in the contract's order. Repeated blocks (`oscN.*`) are one nested macro, so the three oscillators cannot differ. |
| `juce::String (float, 0)` is "default formatting", not "no decimals". | JUCE treats 0 decimal places as "as many as needed". | Build whole-number text from an `int` (`juce::roundToInt`); a signed zero ("-0.0") is shown without a sign. |
| A float `==` against zero to detect "-0.0" would not build on GCC. | `-Wfloat-equal` is an error there. | Decide on the text (`containsAnyOf ("123456789")`), not on the float. |
| The exponential 10 ... 400 Hz tick rate cannot be a plain `NormalisableRange (min, max)`. | The default range is linear. | A `NormalisableRange` with `convertFrom0to1` / `convertTo0to1` lambdas; the host value stays in Hz, so the binding, the preset files and the display text are all in Hz. |

## B-Side S2b-2: band-limited rendering (2026-11-15)

| Problem | Cause | Fix / rule |
|---|---|---|
| Hard sync aliased at -40 to -70 dB near coincident edges (ratios 1.0031, 1.9969). | The digital core reports a reset only at the cycle after the source's crossing, up to two cycles after the crossing itself; by then the rendered line had already passed the target's own edge. | Predict resets from the source's rendered line and let the digital event confirm them; a renderer must never learn about a discontinuity after it has rendered past it. |
| Combined waveforms with the pulse aliased at -50 to -75 dB from mip-mapped tables. | A band-limited table times a hard gate is not band-limited; BLEP and BLAMP fix only the jump and the slope at the edge. | Shapes with few value changes per period: render the staircase exactly (one BLEP per change). Measure the changes before choosing tables. |
| Ring modulation on a combined staircase aliased at -46 dB. | The renderer tracked only the cell of the last value change; after a ring flip (the other half of the table) that cell was stale. | Re-derive the current cell from the phase whenever the table offset changes. |
| A pulse gate stayed open forever after a Model A pull-down. | The next pulse edge was computed from the gate's state, which assumed a monotonic line. | After any phase jump, re-read the (one cycle late) comparison from the new line one cycle later. |
| Model A combined shapes stopped about 10 dB short of -96 dB; no code change helped. | An 8-zero-crossing, Kaiser beta 8 BLEP kernel attenuates what folds into the band by at most about 85 dB, and spiky shapes (single cells of a combined table) have flat spectra far above the band. | Isolate a budget failure with a kernel experiment before tuning anything else. For such shapes use a kernel with more stop band (B-Side: beta 11, same length) only where needed. |
| With the stronger kernel one nearly silent shape still missed the budget by 4 dB. | The BLEP tables interpolate linearly between 512 points per sample (about 1e-6 of a step); with about 111 dB of stop band that error is the floor. | Correct the interpolation with the kernel's own second derivative where the deep stop band is needed; re-check an optimisation that "changed nothing" whenever the limiting stage changes. |
| A tool command was blocked although it only edited source. | The command text contained a member access ending in "env[" after a dot, which the hook reads as the forbidden file name. | Edit such lines with the Edit tool, not through shell text. |
| A renderer's line simulation predicted sync and ring from a source it did not pull down (review H1). | The Model A pull-down lived only in the per-oscillator sweep; the joint simulation of the three lines had its own, simpler line. | Every model of a line that predicts events must apply every rule that moves the line; test predictions against the chip's own output stream, not only for aliasing. |
| Mutation results and later measurements silently used the mutated library. | The script restored files with a move, which keeps the old timestamp, so ninja never recompiled them. | After restoring a file, touch it (`os.utime`); rebuild from touched sources before trusting any later number. |
| A probe showed a growing drift that did not exist. | `Chip::render` adds to its output; the probe reused the buffer without clearing it. | Clear the buffer before every `render` call in tools and tests. |
| A complex comparison against the chip's stream "failed" for every ring source alike. | It compared at the target's multiples, where a ring-modulated signal has no lines: only window leakage was compared. | Compare at the signal's own lines (put source and target on multiples of one base). |
| An independent alias measurement reported -100 dB on narrow pulses near DC. | The Kaiser window's leakage of the large DC of a quiet shape, just outside the guard. | Remove the mean before windowing; look at where the worst bin lies before believing a figure. |
| Corrections stayed rare in static tests but frequent with writes, and every fix of one case seemed to move the count elsewhere. | Each prediction (a pull-down ahead of the chip's step, a sync reset not yet confirmed, an offset held as a phase) was made under the controls of its time; a write in between changed what the chip would do, and the renderer kept the old prediction. | At a write, re-decide everything the chip has not done yet from the digital state; keep offsets that are times as times (rescale with the frequency). Count corrections per write kind to find the kind that breaks. |
| A random test against the chip's stream found two defects that no alias gate, no correction count and no hand-made stream case had shown (an inverted ring, a stuck pulse gate). | Both left the line itself correct (no correction) and were periodic (no alias): only the waveform was wrong. | Compare the rendered output with the chip's own output stream under random schedules, smoothed to the audible band; a full-scale difference is a defect, not a tolerance question. |
| Exact ties between events (two lines crossing at the same instant) were silently dropped. | The next boundary was computed from the current position "plus one half period"; a position exactly on a boundary counted as passed. | Settle a boundary reached exactly by another event before computing the next one; test lines in step (same frequency, same phase). |
| A comparison with the chip's stream differed by up to 0.25 with nothing wrong. | The chip outputs its waveform sampled at the clock; spiky combined shapes at high frequencies beat against the clock, the continuous rendering does not. | Before calling a stream difference a defect, check it on a static patch; tolerate the clock-rate sampling, not more. |
| Three review rounds in a row each found new rendering defects. | The tests compared outputs and counted corrections; a wrong gate or ring state that left the line itself right was visible only where a smoothed output differed enough, and only for the write kinds the generators made. | Check the renderer's internal state against the chip's directly (a white-box check at every sample end), widen the generators to every write kind, and keep a long threaded run for manual use; a defect found by it becomes a fixed regression case. |
| A random schedule produced a sync chain that the contract excludes, and the renderer was blamed. | The generator removed the chain in its own view of all three controls but wrote only one oscillator. | A generator's constraint must hold for what it writes, not for its shadow of the registers. |
| Typical rendering costs about 52 ns per internal sample per chip at 96 kHz, about 3 times the digital core. | Per sample: a line simulation, three sweeps, the re-anchoring and the ring buffers, even when nothing happens. | Room for S4: skip the sweep of an oscillator without events in the sample (value, slope and next event cached), keep the envelope product out of the ring, vectorise the BLEP window; measure with `chip_bench` before and after. |

## B-Side S2b-1: combined-waveform tables (2026-11-15)

| Problem | Cause | Fix / rule |
|---|---|---|
| A Model A sweep of a saw combination never got above index 0x800. | The top-bit pull-down (8.15.2 (c) step 4) sends the accumulator back to the lower half whenever the output's bit 11 is 0. | Measure such entries one step at a time between sync resets (oscillator 2 resets oscillator 3 every 4096 cycles without a write); take the index from the reference's own accumulator, never from the plan alone. |
| The first pulse-with-noise samples looked like a table of the index. | The noise output decays with the time since the refill, and the index grew with that time: the two were correlated. Steps with a noise shift also changed the mask before the output. | Randomise frequency and delay independently, skip planned steps that meet a shift, read the mask from the reference's register. Then the value turned out to depend on the noise output only. |
| Model B tables came out shifted by one index. | Model B reads the latch of the previous triangle/saw step. | Map each Model B read to the accumulator one checkpoint earlier; confirm with spot samples (identical). |
| The adapter's `.bsspec` of a long noise trace would be megabytes. | Spectral frames for about 100 s of chip time. | Commit only `.bstrace` and `.bsref` for read-back measurements. |
| Tables computed with `exp` and a linear solve could differ between platforms. | Libraries and FMA contraction round differently in the last bits. | Keep every decision a margin away from its threshold (tested) and pin a hash of the built tables in a test that runs on every platform. |
| A refit from the committed parameters changed them (review). | The search accepted equal-cost random moves, so a round could wander along a plateau. | Accept strict improvements only and iterate rounds to a fixed point; `bside_shapefit verify` proves the committed file is one. |
| A mutation test of "no allocation" passed although the code allocated. | Clang may remove a `delete new T` pair entirely. | Make a probe allocation escape (a volatile pointer, or `malloc` into a global) before trusting an allocation-count test. |
| The random-start fit landed in different local minima from run to run of an exploration script. | Discrete objective (wrong bits), plateaus. | Deterministic generator per table, start from the committed parameters, pattern search plus joint random moves; never accept a worse refit. |
## B-Side S2c: patch program engine (2026-11-15)

| Problem | Cause | Fix / rule |
|---|---|---|
| A block-split comparison failed only when the timeline changed a host parameter. | A host change takes effect at the next sync or tick, and a block boundary is a sync: the effect moves with the block size. That is the contract's block-rate parameters, not an engine fault. | Compare block splits on timelines without host changes; test host changes by the cycle at which `sync` is called. |
| `-Wswitch-enum` (clang, an error) rejects a `switch` with `default` over `Op`. | The flag wants every enumerator listed. | List every enumerator in each case group, with a comment, instead of `default`. |
| A class with a virtual destructor and a user-declared destructor failed `-Wdeprecated-copy-with-dtor` when a derived class was moved. | The implicit copy of the abstract sink is deprecated next to a declared destructor. | Default the copy and move operations in the abstract base (protected). |
| `new int (5)` followed by `delete` did not move the allocation counter. | The optimiser removes the pair. | A test of a counter must use the allocation (sum a `std::vector`), and check the counter itself first. |
| A hand-derived golden, an independent model of the contract and the engine agreed on every line at the first run. | Three artefacts written from one reading agree even where the reading is wrong. | They prove the code does what the reading says, not that the reading is right: the readings are listed in `docs/decisions.md` for a person to judge. |
| A source-oscillator rule evaluated slot by slot gave a different result for different block splits. | Whether an off oscillator needs its frequency depends on the target's control, which a later wave lane may write in the same tick. | A decision that reads another lane's result must run after every lane that can change it (all wave lanes, then the host sync); the block-split test with host and period changes found it. |
| A mutation that removes a bounds check survived the sanitizer run. | UBSan reports an out-of-range array index but does not stop unless `halt_on_error=1`. | Run mutation checks of bounds guards with the preflight's `UBSAN_OPTIONS`; an index into a fixed member array is intra-object, so AddressSanitizer alone never sees it. |
| `[[gnu::noinline]]` is an unknown attribute on MSVC. | The counting allocator used a GNU attribute. | Use a macro (`__declspec(noinline)` on MSVC). A counting `operator new` must cover the over-aligned overloads too. |
| `chip.reset()` also zeroes the chip's cycle; the engine's cycles are absolute. | Two clocks with one name. | A sink that resets keeps an offset and exposes the engine's time; a test that replays a trace splits it at the reset. |
| Golden trace files with a leading `#` comment line still read as traces. | The reader accepts comments before the header. | Put the kind of each golden (hand-derived or model) in its first line. |
## M7f: editor polish (2026-11-15)

| Problem | Cause | Fix / rule |
|---|---|---|
| A paint that "allocates nothing" still allocated one block per text call. | A string literal passed to `Graphics::drawText` becomes a `juce::String` (one allocation) on every call; `Theme::font()` builds a `Font` (one more); both hide behind a context that skips text. | Keep literals as `const juce::String` members and take fonts from `Theme::fonts()` / `PaintFonts`; the whole-editor test (`PaintAllocationTests`) finds any miss. |
| A component test found no accessibility handler. | `Component::getAccessibilityHandler()` returns null for a component that is not on the desktop. | Ask `createAccessibilityHandler()` for the handler the component would attach (it is public), as the inventory test does. |
| An allow-list entry matched the wrong class. | Matching `BrowserPage` against the compiler's name also matched `BrowserPage::ToneSourceSwitch` (nested class names contain the outer one). | Compare the class's own (innermost) name (`classNameOf` in `tests/ui/EditorWalk.h`). |
| A paint check passed on a mutation that should have failed. | `clipRegionIntersects` false in the null context makes `drawText` return early, and a constant-false ternary is a `-Wunreachable-code` error. | Mutate with a runtime condition when showing that a test can fail; the text layout itself is measured with `NullGraphicsContext (bounds, true)`. |

## M7 Play page keys (2026-11-15)

| Problem | Cause | Fix / rule |
|---|---|---|
| A control on a page that is switched away never learns it was hidden. | JUCE calls `visibilityChanged()` only on the component whose `setVisible` changed, not on its children; `isShowing()` is false for every editor that is not on the desktop (all editor tests). | Anything that must stop when its page goes away (a held note, an open gesture) is ended by the page's `cancelInteraction()` and, as a safety net, by an ancestor-visibility walk up to (not including) the editor in the per-frame idle. |
| A single-writer pending-off mask can strand an off. | The writer retries only while an editor is open; if it closes with a full queue nobody calls it again. | Superseded by the next row: no side path for offs at all. |
| An off kept beside a lock-free queue (a pending-off mask the reader collects) overtook its own note on, and the note hung (review of 5a81815); taking the mask before reading the queue's end only moves the window (an off taken early, then a new on of that note published before the end is read, puts the off after the new on). | Two channels with no common order: the reader cannot tell where in the ring an off from the side channel belongs. | One FIFO for ons and offs. Never lose an off by reservation: a note on is accepted only if the queue has room for it and for the offs of every note then on (`used + notesOn + 2 <= capacity`), so an off always fits. Test writer/reader interleavings deterministically by making writer calls from inside the reader's emit callback. |

## B-Side S2a fidelity pass (2026-11-14)

| Problem | Cause | Fix / rule |
|---|---|---|
| The exemption of 8.15.8, implemented as its list of fields, failed three Model A `c_*` traces (`c_3_a`, `c_6_a`, `c_7_a`: acc3 from cycle 1 162; `c_ring_3_a`: acc3 from 1 040; `c_ring_order_a`: acc2 and rd1B from 2 563). | The list names the sync target of a pulled-down oscillator, not the pulled-down oscillator itself, nor the ring-modulated output that reads it. | Mark exempt fields by following every flow of the per-cycle rules from a combined-waveform table value (`ShapeExemption.h`), not by a list; run the literal list against the data too and report the difference as a contract gap. |
| The naive stepper's counter went to -1 where the chip's wrapped to 0xff. | A release step at counter 0 is possible when an attack phase cleared the freeze before any attack step; 8.15.4 writes "counter - 1" without the width. | Give every counter of a naive model the hardware's width; a random driver reaches such corners (5 to 11 times per 10^7 cycles here). |
| The fast path became 2 to 4 times slower (21 to 45 ns per internal sample at 96 kHz against 10 to 12, scratch benchmark on a loaded machine). | The pipelined envelope makes 3 of every 9 cycles of a rate-0 envelope non-trivial (pending, reset, divider pipeline), and every such cycle is a real envelope step. | Acceptable for S2a (about 0.2 to 0.5 % of a core per chip). Reviewer's measurements (round 1): 22 to 48 ns per internal sample depending on the envelope rate, and **226 ns in noise write-back mode** (noise with another waveform), which steps every cycle. Two equivalent cheaper forms for S4 (not implemented): the periodic envelope loops (sustain hold, frozen at 0, release at a constant divider) computed with modular arithmetic up to the next event; and jumping a locked noise register: register 0, latch 0, noise output 0 and the first-shift flag clear is a fixed point of both the write-back and the shift. |
| The exemption list ended exemptions too late and started one where no table exists. | Noise with only triangle or only saw was treated as a table case (every waveform with two bits), and a certain sync reset (acc := 0) did not end an accumulator exemption. All 400 exempt checkpoints of `c_9_*` and `c_a_*` were equal anyway: a sign that nothing there depended on a table. | Count, per exempt field, how many checkpoints actually differ (`exemptFieldsText`); an exemption that never differs is a hypothesis to test, not evidence. Every way an exemption ends needs a unit test when the data has none. |

## B-Side S2a: chip digital core (2026-11-14)

| Problem | Cause | Fix / rule |
|---|---|---|
| The fast path's event list differed from the per-cycle one although every state matched. | A closed-form jump reports the noise clocks of oscillator 1, then 2, then 3; stepping reports them cycle by cycle. | State the order in the sink contract (per oscillator in cycle order) and compare the stable-sorted lists. A renderer consumes events per oscillator. |
| Counting bit crossings in closed form overflows silently for long jumps. | `phase + jump × increment` exceeds 64 bits; the phase survives (2^64 is a multiple of the 2^56 wrap) but the crossing count does not, so a test that compares only phases misses it. Jumping frozen envelopes removed the 32 767-cycle bound that envelope events used to give. | Cap every closed-form jump (`kMaxSkip`) so the unwrapped sum fits; compare the noise registers, not only the accumulators. |
| A sink that read the chip inside a callback saw different states from the fast path and from stepping. | A closed-form jump advances the oscillators one after the other, so mid-jump the chip is in no real state. | Events carry everything a renderer needs (phases, increments, register, output); a sink must never read the chip. Tests compare the full payloads. |
| Write-back mode (noise plus another waveform) is never jumped. | The write-back depends on every cycle's output. | It costs about 8 ns per cycle (reviewer's measurement), about 0.8 % of a core per chip in that mode at the 50 Hz clock: count it in the S4 CPU budget (a voice that locked its noise up stays expensive until the waveform changes) and expect long traces with such writes to replay slowly. |
| The mutation runs found untested rules (sync chain rotations, write-back under the test bit, snapshot field order). | Row tests used one orientation and fields with equal or zero values. | Test every rotation of a three-way rule; give every field in a mapping test its own non-zero value so a swap shows. |
| An equivalence test against the code's own per-cycle step cannot catch a wrong definition. | Both paths share `stepCycle`. | The fast path is proven by the equivalence test; the definition by the 8.4.3 row tests and by an independent stepper written from the contract by someone else. Check the equivalence test with mutations of the fast path (S2a: 7 mutations, 5 caught, the 2 others proven equivalent). |
## B-Side reference data (2026-11-14)

| Problem | Cause | Fix / rule |
|---|---|---|
| `ctest -L trace` found none of the new tests. | `catch_discover_tests (... PROPERTIES LABELS "bside;trace")` passes the properties on unquoted: `trace` became a property name. | One label per discovered executable (or none); `ctest -L` is a regular expression, so a common prefix works for grouping. |
| A noise-register test that ran combination after combination tested an empty register. | Combined waveforms lock the noise register at 0; a short test pulse leaves one bit, which takes hundreds of shifts to spread. | Refill before each case (test pulse, then about 40 000 cycles of noise at the top frequency) and check the starting value in the reference. |
| The first spectra could not tell a filter corner within 5 %. | One 16384-point window of a noise excitation varies by several dB per bin. | Measure a filter as the ratio to a twin trace with the same writes and the filter bypassed: the excitation is deterministic, so its ripple cancels. |
| The reference emulator's "48 kHz" output is 48 012 Hz. | Its resampler truncates cycles per sample to 1/1024. | Never take a reference's sample rate on trust: measure the output rate (count samples per second of input) before comparing spectra. |
| Hashed text files would fail on a Windows checkout. | Git for Windows converts LF to CRLF on checkout by default, which changes every SHA-256 the MANIFEST and the references record. | `.gitattributes`: `tests/bside/reference/** -text`. Any committed file whose bytes are hashed needs it. |
| A decision row described a mechanism by its source comment, and the data said otherwise (ring fold, read-back "delay"). | The comment and the code differ in a detail (NOT of the source bit; a latch, not a one-cycle delay). | Validate every stated rule against the committed data with a small model before writing it down; the reviewer's models at 0 mismatches are the standard. |
| An emulator quirk that hardware cannot have (a held oscillator syncing every cycle) could enter the reference silently. | A flag not cleared in one write timing. | Name such artefacts, make the generator refuse traces that reach them, and keep our chip free of them. |
| A gate sweep "over every offset" never met one pipeline state. | Successive trials shifted by one cycle against the rate counter, but the divider counter's parity was locked to the trial length. | Sweep both clocks of a two-clock state: here half the trials wait one extra rate period. Check coverage in the data (count the states met), not in the generator. |
| An "evidence" citation showed nothing (a fade of a value that was already 0). | Written from the source's rules, not read back from the trace. | Quote the trace's own values at the cited cycles, and design the trace so the effect is large (deselect at a high output). |
| A field encoding read the same for two different states. | "position + pending" maps (p, pending) and (p + 1, not pending) to one value. | Prove an encoding one to one (or state-equivalent) before using it; spare bits of another field are better than a lossy fold. |
| Probe writes. | The reference holds the state only at checkpoints; `check N` is uniform and costly. | A write that changes nothing (00 to the unused cutoff low bits) places a checkpoint at any cycle. Digital traces only: in analog traces every write splits a spectral segment. |

## M7a: editor foundation (2026-11-14)

| Problem | Cause | Fix / rule |
|---|---|---|
| Opening the editor crashed in one test only (two processors, then the editor). | The frame called `setSize` first in its constructor; `resized()` ran before child members created later in the body existed. Whether it crashed depended on the heap. | **Call `setSize` last in a component's constructor.** |
| `lldb` could not attach in the agent sandbox. | Sandbox. | Read the stack from the crash report macOS writes (`~/Library/Logs/DiagnosticReports/<binary>-*.ips`, JSON after the first line). |
| A freshly written golden file never matched. | `juce::File::replaceWithText` writes CRLF line ends by default, on every platform. | Pass `"\n"` as the line ending, and compare without `\r` (a Windows checkout may convert). |
| The full editor update took 445 µs for a program change. | Each Sound page recomputed its curve and scopes once per changed input (4 + 9 inputs). | Mark pictures dirty in the notification; recompute once after all notifications of a tick (an idle hook of the mirror). |
| Editor tests saw no meter update. | The constructor's first tick used the real clock; the test's own clock started far below it. | Treat a clock that goes backwards as "due now"; tests drive `tick (ms)` themselves. |
| A prose line of `help-texts.md` parsed as an entry. | It starts with "- \`" and contains "\`: " later. | An entry's id is one token (letters, digits, `.`, `_`, `<>`). |
| `= default` comparison of a struct with float members failed to build. | `-Wfloat-equal` (an error) applies to defaulted `operator==` too. | Compare floats bitwise with `std::bit_cast`. |
| A test proved nothing about what the controls draw (a reviewer's mutation that stopped notifications passed). | The binding check compared a fresh read of the mirror with the mirror. | Check the **prepared drawing state** (`drawnValue`, `drawnText`, set only on notification), and compare pictures with what the engine itself read (`getBlockParams`), not with the page's own mapping. |
| Off-screen components return no accessibility handler. | JUCE creates handlers for components with a native window only. | Tests call `createAccessibilityHandler()` directly. |
| Counting repaints without a window. | `repaint()` goes nowhere off screen. | Install a recording `juce::CachedComponentImage` on the top component: every child's repaint arrives as `invalidate (area)`. |
| A refactor of the voice's oscillator code needed proof beyond the existing renders (they barely use PWM). | Coverage. | A temporary probe script copied into both builds' script folders, bit-compared, then removed. |
| The worktree guard refused shell commands whose text contained an ordinary English word that is also a git subcommand. | It scans the command text. | Put larger edits into a script file in the scratch folder and run that. |
## B-Side S1d: tables in presets and state (2026-11-14)

| Problem | Cause | Fix / rule |
|---|---|---|
| The hand-over test saw torn programs (about 50 per 1.5 s run) with a plausible-looking protocol. | The writer read the committed index and then the claim; the audio thread stores the committed index and then clears the claim, so a writer could read a stale index, then a cleared claim, and reuse a buffer that was in use. | The writer reads the claim before the committed index. Rule: when one thread publishes A then clears B, the other must read B then A. |
| Review mutants of the hand-over (claim cleared before the index is stored, no re-check of `published`) passed a two-thread stress test. | A stress test only finds races the scheduler happens to produce. | The protocol's steps go through a hook, and `HandoverModelTests` explores every interleaving with state-hash pruning (24 564 states, 10 s) and must catch four broken variants. Rule: a lock-free protocol gets an exhaustive step test, not only a stress test. |
| A legitimate table of 1024 tab-only lines made a 64-program bank of 26 MB; `saveBankFile` wrote it and `loadBankFile` refused it. | The table reader accepts tabs; the JSON writer escaped a tab as six bytes; the file cap was computed for quotation marks. | Short escapes for the control characters; the worst byte is found by code in the cap test; every save refuses output over the reader's cap. Rule: compute a cap from the worst input the reader accepts, by enumeration, and have the writer check its own output against it. |
| A hostile test session with a million escaped line ends took 9 s in `setStateInformation`. | JUCE's XML decoder, before our code runs (our part took 0 ms). | Not ours to bound; measure our part separately (decode time subtracted). |
| Every preset file read allocated and zeroed the whole file cap. | `readPresetFile` resized its buffer to `maxBytes + 1`; harmless at 1 MB, 12 MB per read with B-Side's cap. | Size the buffer from the file and grow to the cap. |
| Removing duplicate children one by one from the front is quadratic. | `ValueTree` erases at the front by shifting. | `takeChildren` walks from the back. |
| An error message cut at 1024 bytes could end inside a UTF-8 character. | A byte cut of a string that quotes user text. | `truncateUtf8` backs up to the character start. |
| macOS `timeout` does not exist. | Not installed. | `perl -e 'alarm N; exec @ARGV'`. |

## B-Side S1b: plugin shell and gates for two plugins (2026-11-14)

| Problem | Cause | Fix / rule |
|---|---|---|
| A second instrument in a test build still reported the first plugin's name. | The skeleton's `getName` returned the build's `JucePlugin_Name` macro, which belongs to the plugin target the test core copies its definitions from. | Identity that differs per instrument comes from the instrument (`Derived::kName`), never from a build macro; assert both agree for the real plugin. |
| A missing parameter or a forgotten `initPresets` call was a `jassert` (debug) and a null dereference (release). | No check in release builds. | Throw a named `ConfigError` at construction in every build type; the plugin entry point turns it into a message and an abort (an exception must not cross the plugin boundary). Test the checks with a second, deliberately broken instrument. |
| CMake refused the second test executable's tests. | `catch_discover_tests` registers every Catch2 test case name in one ctest namespace, and a name existing in two executables is an error. | `TEST_PREFIX` per executable (`bside: `). |
| The two test cores cannot be linked into one executable. | Each defines `createPluginFilter()` and takes its own `JucePlugin_*` definitions. | That is the guard: every test executable links exactly one core. Do not "fix" the duplicate symbol. |
| Two test cores each compiled all JUCE modules. | JUCE's module targets are INTERFACE libraries whose `.cpp` files are interface sources: whoever links them, PUBLIC or PRIVATE-into-a-target-with-sources, compiles them. | One static library links the modules PRIVATE (an empty `add_library(... STATIC)` is fine, the sources come from the modules) and the cores link that library; the cores keep taking include directories and definitions from their plugin target. Linking the modules PUBLIC from the library would put them back into every consumer. Prove the shipped targets did not change with `ninja -t commands <plugin targets>` before and after. |
| The sanitizer steps silently skipped most test executables and still printed PASS. | The selection ran `ninja -t targets all \| grep -q "^name:"` under `set -o pipefail`: `grep -q` exits at the first match, ninja is killed by SIGPIPE, the pipeline returns 141 and the target counts as missing. It only fails when the match is early in a long list, so it looked fine in a quick test. | **Never `grep -q` on the right of a pipe under pipefail**; capture the output in a variable and match with a shell `case` pattern. A selection that can skip must be impossible to miss: print what was built and how many tests each target has, and make "required" the default (`scripts/sanitize-targets.sh`). Only the targets that truly do not exist yet are optional. |
| A Python edit script in a heredoc and any command mentioning git together were refused in the worktree. | The guard cannot verify such compound commands. | Write the script to a scratch file with the Write tool and run it as its own command; run git as plain, separate commands. |
| `git status` fails in a worktree whose submodule directories are replaced by symlinks. | Git expects a gitlink directory, not a symlink. | `git status --short --ignore-submodules=all`. |
| The gate-time baseline and the new numbers are noisy. | Other agents build on the same machine and share the Docker volume and its compiler cache. | Report the load average next to each time and compare steps, not totals. |

## B-Side S1c: text readers and their tests (2026-11-14)

| Problem | Cause | Fix / rule |
|---|---|---|
| Error columns pointed at the wrong place in some messages. | `errors.add (line, c.column(), ..., tokenOrChar (c))`: the order in which function arguments are evaluated is unspecified, so the cursor had already moved when `c.column()` ran. | Read the column into a local before anything that advances the cursor. Never call a consuming function twice in one expression either (`c.token().empty() ? x : c.token()` consumed the token and returned an empty second one). |
| The note name `=C#6` was cut at the `#` (a comment). | The grammar allows a comment right after any token. | In a pitch token that starts with `=`, a `#` in the second character belongs to the note name; everywhere else `#` ends the token. |
| A test macro with a parameter called `what` broke `e.what`. | The preprocessor replaced the member name. | Give macro parameters names no member shares (`phrase`). |
| Cross-correlation "found" a delay of -9 for identical 10-sample signals and -256 for silence. | The overlap of one sample correlates perfectly by chance; and silence ties at every lag. | Ignore lags whose overlap is under half of the signal; on a tie take the smallest lag. |
| The note name `=a3#` must not pull a later `#` into the token. | The first rule kept a `#` in the second character of any `=` token. | A `#` belongs to a note only directly after the note letter. |
| The contract's own example trace is rejected by a plain 64-byte line limit. | Its comment lines are 85–105 bytes. | The limit applies to the statement (before `#`); the whole line may be 256 bytes. When a contract example and a contract limit disagree, make the example a test. |
| A hand-counted column in a test is wrong half the time. | Counting characters in a string literal by eye. | Write the test with the intended message first, run it, and check each reported position against the text before accepting it; do not "fix" the reader to match a miscounted test. |
| A fuzz test that only checks "no crash" misses leaks of work, not memory. | A hang or quadratic parser passes it. | Cap the wall time per input and count the bytes allocated per call with a replaced global `operator new` in the test executable (works under ASan); assert both. |
| The writer produced text the reader rejected (a compact 197-byte row became 233 bytes). | The canonical form is longer than input (indent, `, `, `set 25%` -> `set 1024`) while the reader bounds the line, so "reader output is writable" and "writer output is readable" are separate properties. | State the round-trip property for **both** sides, put every limit of the text format into `validate()`, and fuzz it on hostile `Program` bytes as well as on text. |
| A "finite" `repeat` loop of rows without delay was accepted and could run more than 10^9 rows in one tick. | The loop check ignored the repeat's jump edge. | A bound you rely on (rows per tick) needs a proof and a model test, not a runtime cap as the only guard: the check is now acyclicity of the delay-0 graph, and the model runs every start row with random counters. |
| A valid trace had a reference file the reader refused. | The trace's write limit (10^6) and the reference's file limit (16 MB) were chosen separately. | Derive file limits from count limits with a `static_assert` of the longest line, and make every writer refuse what its reader would. |
| Replacing the global `operator new` in a test hides small under-runs from AddressSanitizer. | The replacement allocates with a header, so ASan sees valid memory. | Measure allocation bounds only in builds without ASan; leave `operator new` alone under it. |
| `snprintf("%.1f")` in a report depends on the C locale. | Decimal comma. | Format numbers yourself (integer arithmetic) when the text is compared or parsed; `std::to_chars` for doubles is not available at the macOS 11 deployment target. |
| `git status` in a worktree with symlinked submodules shows an error about `external/Catch2`. | Git expects a submodule directory, not a symlink. | Harmless; commit explicit paths only and never `git add -A`. |

## B-Side S1a: shared processor skeleton (2026-11-14)

| Problem | Cause | Fix / rule |
|---|---|---|
| The contract's CRTP base read `Part` and `InstrumentParams` as member types of the derived class. | The base stores both (parameter sets, part references in member declarations), and the derived class is still incomplete where the base's members are declared. | Pass them as template arguments: `BiTimbralProcessor<Derived, Part, InstrumentParams>`. Only function bodies (instantiated later) may use the derived class's members. |
| Proving "no behaviour change" needs a dump tool that the old commit does not have. | A tool added to the tree would itself change the commit under test. | Keep the tool outside the repository and inject it into both builds with `-DCMAKE_PROJECT_VintageSynth_INCLUDE=<file>` and `cmake_language(DEFER … CALL …)` (it runs after `tests/` defined `vs_test_core`). Copy the baseline binaries aside before the source changes. |
| The unit-test binary reports a different assertion count on every run. | Threaded preset tests loop on timing. | Never use the assertion count as evidence of "the same tests ran"; compare test-case counts and the bit dumps. |
| The worktree guard refused `cd <worktree> && git …` and commit commands with a quoted message. | It cannot verify compound commands that run git. | Run git as plain, separate commands from the worktree; write the commit message to a scratch file and use `git commit -F`. |

## M6b: factory bank (2026-11-13)

| Problem | Cause | Fix / rule |
|---|---|---|
| Shell commands were refused by the hook that guards secret files. | Parameter IDs such as `vcf.env_mod` and `vcomod.env_mod` contain the guarded file name as a substring. | Write preset data and scripts that mention those IDs with the Write/Edit tools; build ID strings in scripts from parts. |
| Applying a program with the −12 dB VCF slope over a sounding chord made a step (5× the click threshold) 3 samples after the change. | **Confirmed by isolating each parameter of the program** (apply the old sound with one parameter changed, and the new program with one parameter kept): the slope alone made the step (0.10), every other change together none. Not specific to releasing voices or to the apply order: flipping the slope by hand on a held chord stepped too (0.18–0.33 against the chord's own 0.018; the earlier "held notes do not step" was most likely measured where the taps nearly agree: at cutoff 0.75 the same flip shows no step). Stage 2 and stage 4 differ in phase and level; the voice switched its output tap at once. | Sounding voices crossfade between the taps over 5 ms (`Voice::kSlopeFadeSeconds`). **Isolate a click by changing one parameter at a time, both ways round, before believing a mechanism.** |
| The crossfade's per-sample check made the voices 27 % slower in `voice_bench`. | One branch on a member flag inside the interleaved voice loop (it also stops the compiler keeping the filter state in registers across voices). | `renderBlock` picks a loop instantiation once per sub-block (`renderSample<MayFade>`). **Benchmark every change to the per-sample voice loop, however small.** |
| Tuning 64 levels by hand would not converge. | Every change moves the group median. | Measure, apply −deviation as a VCA trim (gain = level², so trim dB/40 in the exponent), repeat: three passes converged to ±0.6 dB. Keep the generator and trims outside the repository; the JSON files are the source. |

## Wave 4: output stage (2026-11-13)

| Problem | Cause | Fix / rule |
|---|---|---|
| A limiter with a 4 s release still measured −25 dB "distortion". | One least-squares gain fit over 2 s counts slow level changes (release) as error. | Measure distortion with a gain fit per short window (20 ms) and report the long-window figure separately. |
| A slow "program-dependent" detector changed nothing. | It followed the rectified signal, whose average sits far below its peaks. | Feed the slow detector from the peak detector, not from the raw signal. |
| Re-blessing goldens after a level change needed proof that nothing else changed. | — | Make the gain a power of two and compare the raw `[.bitdump]` floats: new = old × 0.5 except values that become subnormal (flushed to zero). |
| In an agent worktree, shell commands naming the top-level `source` directory as a bare word were refused by the sandbox. | It reads the word as the shell's `source` builtin. | Write `source/common`, `source/vintage` (with a slash and subfolder) in commands. |

## M6a: presets and banks (2026-11-13)

| Problem | Cause | Fix / rule |
|---|---|---|
| A session could make the host hang or exhaust memory at load. | A file path from the state was opened unchecked; "exists as file" is true for pipes and devices, whose size reads 0, and the read ran to end of file. | **State and preset files are untrusted input.** Store names, not paths; one bounded reader for regular files only; no file I/O in `setStateInformation` (hosts call it on any thread). |
| A pre-check written to protect a third-party parser was bypassed. | The parser accepted more syntax (single-quoted strings) than the check knew. | Don't guard a lenient parser with a second tokenizer: use one strict parser. Fuzz it in the suite, with the reviewer's token list. |
| A block could read a half-applied parameter set, and ThreadSanitizer saw nothing. | Each variable was atomic; the ordering was wrong (two holders of one flag, acknowledgement before the last read). | Test consistency with an invariant across many parameters, checked by the audio thread; sanitizers do not find logical races. |
| The implementing agent ran out of budget before running the gates. | A large task in one go. | The orchestrator runs the full gate on every hand-back, whatever the report says. |
| Parallel test runs collided in temporary folders. | Name generation is per process. | Random folder names in tests. |

## Wave 3: integration and oscillator fixes (2026-11-12)

| Problem | Cause | Fix / rule |
|---|---|---|
| Bypassing the plugin in a host hung notes (and an arpeggio never stopped). | JUCE's default `processBlockBypassed` drops the MIDI buffer; the note-off never arrives. | Always override bypass in an instrument: release, reset, ignore MIDI. |
| Hosts stored values between the steps of integer parameters. | `juce::AudioParameterInt` is not discrete for the VST3 wrapper. | Use a stepped integer parameter; test that every integer and choice parameter reports its step count. |
| A NaN parameter value kept the output guard firing on every block. | Target + deviation smoothers keep a NaN forever. | Sanitise at the parameter boundary; keep the guard as the last line. |
| A test harness "allocated on the audio thread". | `AudioBuffer::setSize` in the test itself, when growing. | Preallocate in allocation tests. |
| Linux UBSan flagged `memcmp` on an empty vector; macOS did not. | `memcmp(nullptr, …, 0)` is undefined. | Run both sanitizer builds. |
| A block-size fuzz showed large differences. | Event overflow bursts: the parser drops the rest of a chunk by design, and which events survive depends on chunk boundaries. | Exempt overflow bursts from block-size comparison; still check hangs and silence. |
| The reviewer's fuzz found what the implementer's tests missed (again). | — | **Port every reviewer fuzz into the suite**, with a larger seed count behind a hidden tag. |
| A sine reset could not be fixed with step + ramp + curvature corrections. | Higher polynomial orders have residual tails beyond the kernel window; they only cancel in the full series. | Band-limit the actual error signal by integrating it against the kernel. |
| Rendering only the oscillator pair at the real-time rate offline still left 3.8 dB differences. | The filter's input saturation aliases differently at each rate. | Rate-dependent non-linearities anywhere in the voice matter: keep the whole voice at one rate. |

## Linux containers and ARM64 (2026-11-12)

| Problem | Cause | Fix / rule |
|---|---|---|
| The new local container check reported a failing `host_smoke` that CI did not have. | The script copied sources with their original timestamps into a persistent build volume; after a branch switch a changed file could be older than the last build, so the build system kept a stale binary. | Copy by content and give changed files a fresh timestamp. **A cached build is only trustworthy if staleness is decided by content, not by time, when sources come from different checkouts.** |
| GCC 11 on 64-bit ARM failed in an audio codec bundled with JUCE. | A loop-optimisation warning in third-party C code, promoted to an error. | For GCC, that one C warning is not an error (`CMakeLists.txt`); our own code is unaffected. |
| The first container build was killed for lack of memory. | 18 parallel compile jobs in an 8 GB container VM. | Cap parallel jobs by the VM's memory. |
| The container check's sanitizer mode could not write the build volume. | The two base images gave the unprivileged build user different numeric ids. | Create the user with the same fixed id in every image. |
| The security scan failed on the new Dockerfile. | It ran as root and installed recommended packages. | Unprivileged user; `--no-install-recommends`; name the sanitizer runtime package explicitly (its name differs between distribution versions). |
| The container check reported a bogus failure ("unknown parameter id") after another branch had used it. | Files deleted since the last run were never removed from the shared build volume (the cleanup ran in the wrong directory), and two runs could overlap. | Fixed; runs now take a lock. A shared cache must mirror deletions too. |
| A helper printed a progress note that ended up inside a file path. | Command substitution captures stdout. | Functions that return a value on stdout send notes to stderr. |
| A plain-text deny-list of brand names would itself put the names into the repository. | The rule forbids the names everywhere. | The list is stored as SHA-256 hashes of normalised names; the check hashes words and short phrases (`scripts/preflight/name-check.py`). |
| pluginval has no Linux ARM64 release. | Upstream. | The ARM64 row runs tests, `host_smoke` and the VST3 validator only, like Windows ARM64. |
| A plugin built for glibc 2.35 really needs it. | Verified by a negative test: on a glibc 2.31 distribution loading fails with "version GLIBC_2.35 not found". | The floor is tested in CI, not assumed. |

## Wave 2: M3 bi-timbral and M4 arpeggiator/sequencer (2026-11-12)

| Problem | Cause | Fix / rule |
|---|---|---|
| Reported latency was 2–3 samples too high. | `juce::dsp::Oversampling::getLatencyInSamples()` is the round-trip latency including the upsampler; the voice path only decimates. | Measure the decimator's own delay at prepare time. **Verify latency with a phase measurement, not an onset threshold.** |
| Long envelopes were wrong at 192–384 kHz internal rates. | `1 − exp(−x)` in float, and float accumulators for level, phase and seconds, stall or drift at high rates. | Double precision for slow per-sample state; `expm1` for coefficients. |
| Block-size dependence grew with the sample rate. | Smoothers decayed per sub-block in a chain accumulate rounding with the number of sub-blocks. | Compute from the absolute time since the last retarget. |
| A block-size test passed at 48 kHz only by luck. | It fed events out of time order; the part applies an out-of-order event at the previous position. | Test rigs sort events. Run behaviour tests at every rate configuration. |
| Sync + cross-mod made equivalence measures jump by 30 dB. | The patch is chaotic. | Gate control-rate paths with a smooth patch; bound chaotic patches separately. |
| The alias gate had never measured cross-mod. | The original test covered plain waveforms and sync. | **A quality gate only covers what it measures: list the configurations a gate has not seen.** |
| Arpeggiated notes piled up while the sustain pedal was down. | The module used the pedal and also passed it to the part, which then deferred every note-off. | An event transformer either consumes a controller or passes it, never both. |
| A note-off could be lost when the output buffer was full. | The "owed" state was cleared even when the write failed. | Clear state only after a successful emit; reserve a slot for the overflow marker; port the reviewer's fuzz into the suite. |
| The timing "independent model" shared rules with the implementation. | It was written by the implementer. | State contract rules once as the spec; assert properties from musical time alone for the rest. |
| Tests invoked with a comma in a Catch2 filter matched nothing. | Catch2 splits filters on commas. | Filter by prefix with `*`. |

## Linux target and public-release prep (2026-11-12)

| Problem | Cause | Fix / rule |
|---|---|---|
| The first Linux build failed on `x != 0.0f`. | GCC's `-Wfloat-equal` is in JUCE's recommended warnings and is an error; Clang and MSVC accept the comparison. | Use `std::fpclassify` (or compare bit patterns). GCC-only warnings show up in the Linux CI row only. |
| A CI round trip showed only one compile error. | Ninja stops at the first error. | CI builds with `-- -k 0`, so one run reports every error. |
| Where to put the static C++ runtime flags. | Tests link `vs_test_core`, not the plugin. | `-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL` on `VintageSynth_VST3` only; the bundle check fails if `ldd` shows libstdc++ or a missing library. |
| A plugin built on a new distribution doesn't load on older ones. | glibc symbol versions. | Build releases on the oldest runner image offered (ubuntu-22.04 → glibc 2.35). |
| pluginval was downloaded in CI without a checksum. | Release assets can be replaced. | SHA-256 check before unzip on all platforms; bump version and hashes together. |
| A link to the agent session appeared in commit messages. | Default attribution setting. | Owner decision: commit messages end with the Co-Authored-By line only. |

## Wave 1: M2b polyphony (2026-11-12)

| Problem | Cause | Fix / rule |
|---|---|---|
| Notes on a reused voice differed with the host block size (up to −5.7 dB in Solo); a reused Poly voice started its filter envelope part-way open. | A voice that stops inside a sub-block keeps evolving to the sub-block's end. Oscillator phase, noise seeds, ENV-1, filters and drift were inherited by the next note. | **Every piece of per-voice state a later note inherits must be reset at restart or computed from absolute time.** |
| A new note landed on a different voice depending on block size. | Idle voices were reported in loop-index order when two finished in the same sub-block. | Order-dependent bookkeeping follows sample time, never loop index. |
| Glide and drift left −74 to −93 dB between block sizes. | A pitch value stepped per sub-block turns float rounding into a lasting phase difference. | Evaluate pitch terms from absolute time in double precision and round once. |
| Envelope times changed one host block after a legato note change. | The times were only re-applied at block start. | A state change caused by an event is applied at the event. |
| Unison at detune 0 had a random level on every note. | Independent random start phases sum coherently. | Give unison voices a fixed phase pattern whose sum is √n. |
| The block-size tests passed while all of the above existed. | No test reused a voice: Poly took a fresh voice per note. | **A block-size test only proves something if voices are reused** (Solo/Unison, or Poly after idle), with envelopes of different lengths. |
| Black-box tests written from the spec found nothing, but the code review found five defects. | Spec tests check stated behaviour; the defects were in properties the spec only states globally (block-size independence) or not at all (unison at detune 0). | Keep both: spec-first tests *and* an adversarial review with a fuzzer. |

## Wave 1: M2a, effects, test infrastructure (2026-11-12)

| Problem | Cause | Fix / rule |
|---|---|---|
| Windows CI crashed in one new test (`0xc0000374` heap corruption on x64, `0xc0000409` on ARM64); macOS passed. | The test rendered a full 256-sample block past the end of a 24000-sample vector. The MSVC heap detects that; macOS without a sanitizer does not. | **Run the test binary under AddressSanitizer + UBSan before pushing engine or test changes** (`-fsanitize=address,undefined` in a second build dir). Test renders go through a helper that checks bounds. |
| 32 voices cost 11.3 % although each math call is cheap in isolation. | The voice is one long serial dependency chain per sample; `exp2`/`tan`/`tanh` cost 3–4× more inside it than alone. | Interleave voices sample by sample, vectorise the coefficient work per block. **Profile by stubbing stages out, not by timing functions alone.** (`sample`/Instruments don't work in the sandbox.) |
| Tiny permanent pitch and cutoff offsets; renders depended on block size with Vintage on. | `s += (t − s)·a` stops moving once the step is below half an ulp, up to hundreds of ulps short, and the stall point depends on the step size. | Keep one-pole smoothers as target + decaying deviation. |
| `alignas` members made MSVC warn C4324 (padding) at `/W4`, an error in CI. | Warnings are errors. | Avoid `alignas` on struct members; MSVC-only warnings show up in CI only. |
| The VST3 validator test "passed" in 0.2 s. | ctest only looked at the exit code and hid the output. | Require the tool's own summary line with `PASS_REGULAR_EXPRESSION`. **Check that a new CI check can fail.** |
| Four effect tests could not fail for what they claimed (e.g. an interpolation test that linear interpolation would pass; a decay test that only used whole-sample delays). | Tests were written by the implementer against the implementation. | An independent reviewer asks of every test: "what wrong implementation would still pass?" |
| Waking an idle delay/reverb could clear megabytes in one audio block. | "Clear in slices while idle" was finished at once on wake. | No bulk clearing on the audio thread: hide stale content by gating reads; `reset()` must be O(1). |
| Counting allocations by replacing global `operator new` misses `malloc` (JUCE `HeapBlock`). | JUCE containers call `std::malloc` directly. | On macOS use libmalloc's `malloc_logger` hook, filtered to the test thread. The check is skipped on Windows and under ASan. |
| Agent worktrees started from `main`, not from the wave branch, and lacked the contract file. | The worktree tool branches from the default branch. | Name the base branch in every brief. |
| Submodules are empty in agent worktrees and the sandbox refuses `git config submodule.*`. | Sandbox. | Symlink `external/JUCE` and `external/Catch2` to the main checkout; commit with explicit paths (plain `git commit` may refuse; `commit-tree` works). |

## M0–M1 and CI hardening (2026-11-12)

### DSP

| Problem | Cause | Fix / rule |
|---|---|---|
| Aliasing measured only −52 to −65 dB at 1–2 kHz fundamentals. | 2-point PolyBLEP is too short a correction at 2× oversampling. A raw-alias diagnostic test (`[.diag]`) showed the oscillator was the cause, not the decimator. | Windowed-sinc BLEP/BLAMP table (16 taps, Kaiser β = 8): ≤ −113 dB. **When a quality gate fails, isolate the stage with a diagnostic test before tuning anything.** |
| The latency test failed after switching to the BLEP table. | The table delays the oscillator output by 8 oversampled samples. | Reported latency = decimator latency + `Oscillator::kLatency / osFactor`. **Any DSP stage that adds delay must add it to the reported latency, and the sample-accurate note-on test catches it.** |
| The filter slope test gave wrong slopes. | 1) Bilinear (TPT) warping near Nyquist bends the response. 2) At high attenuation the float precision floor dominates. | Measure at fc = 500 Hz with a 2 k / 4 k Hz pair and input amplitude 0.01 (linear region). **Measure filter responses far below Nyquist and above the float noise floor.** |
| JUCE `Oversampling` needs an upsampling pass before it can decimate. | API design. | Voices render directly at the oversampled rate; the upsampler is fed silence. |

### Real-time safety

| Problem | Fix / rule |
|---|---|
| Building a `juce::MidiMessage` from a `MidiBuffer` event can allocate (SysEx longer than the inline storage). | Parse raw bytes from `MidiMessageMetadata` (`data`, `numBytes`). **Never construct `MidiMessage` on the audio thread.** Covered by a test that sends SysEx. |
| Hosts may call `processBlock` with more samples than `prepareToPlay` announced. | `processBlock` renders in chunks of at most the prepared block size. |
| A NaN from a bug would reach the host's output. | Output guard: non-finite → 0, `jassertfalse`, reset the part; then clamp to ±4.0. |

### Build and CMake (JUCE 9.0.5)

| Problem | Fix / rule |
|---|---|
| Test targets couldn't find JUCE headers or saw different `JucePlugin_*` defines. | Tests take `$<TARGET_PROPERTY:VintageSynth,INCLUDE_DIRECTORIES>` and `…,COMPILE_DEFINITIONS>` from the plugin target. |
| `ctest` received a literal, unevaluated generator expression as the plugin path. | `JUCE_PLUGIN_ARTEFACT_FILE` itself contains genexes: wrap it in `$<GENEX_EVAL:…>`. |
| JUCE 9 warns (→ error) on `JUCE_DISPLAY_SPLASH_SCREEN`. | Don't define it. |
| `AudioFormat::createWriterFor(OutputStream*, …)` is deprecated (→ error). | Use the `AudioFormatWriterOptions` overload. |
| `-Wimplicit-int-float-conversion` in a test. | Warnings are errors in tests too; cast explicitly. |
| A thread test that asserts "the rare path ran at least once" within a fixed time failed once in the Linux sanitizer step (0 collisions in 1.5 s on a loaded machine), after passing many times. | A test that needs a collision runs until it has seen one, with a generous upper limit; a fixed short window only proves something on an idle machine. |
| GCC (Linux step only) fails with `-Wshadow` on a constructor parameter that has the name of a member of a JUCE base class; Clang accepts it. | Name constructor parameters so they cannot collide with base-class members (found in M7a's `CheckableHandler`); run the Linux step before handing over. |
| Hosting a VST3 in a console test app. | `juce_add_console_app` + `JUCE_PLUGINHOST_VST3=1` + `juce_audio_processors_headless`; register formats with `addHeadlessDefaultFormatsToManager`. |

### Testing a loaded VST3

| Problem | Fix / rule |
|---|---|
| The host saw 2159 parameters instead of 78. | JUCE's VST3 wrapper adds 2080 "MIDI CC" emulation parameters and "Bypass". Count our own parameters by excluding those names. |
| State round-trip compared stepped (choice) params unequal. | Normalised floats of stepped params differ after a round-trip; compare by step index. |

### CI

| Problem | Fix / rule |
|---|---|
| First CI run: both Windows jobs failed at configure: "Visual Studio 17 2022 could not find any instance of Visual Studio". macOS passed. | The runner images no longer ship VS 2022. Don't hardcode `-G "Visual Studio …"`; pass only `-A x64` / `-A ARM64` and let CMake pick the newest VS. |
| Actions were three majors behind (`checkout@v4`, `upload-artifact@v4`, `download-artifact@v4`) and referenced by mutable tags. | Updated to checkout v7.0.1, upload-artifact v7.0.2, download-artifact v8.0.2, pinned by commit SHA. Dependabot keeps them current. Mutable tags are a supply-chain risk: `aquasecurity/trivy-action` tags were hijacked in early 2026. Resolve SHAs with `gh api repos/<owner>/<repo>/commits/<tag> --jq .sha`. |
| Windows x64 CI took ~18 min (build 15 min). | LTO flags were PUBLIC on the plugin's shared code, so the plugin, `voice_bench` and the unit tests each ran a full MSVC link-time code generation (4.5 + 1 + 4 min). Fix: LTO PRIVATE on the plugin targets only; tests link `vs_test_core` (same sources, no LTO). Plus Ninja and a compiler cache (ccache/sccache) on every CI job. |
| First run after the speed-up was *slower* (macOS 14 min vs 7, Win x64 21 vs 20, Win ARM64 15.5 vs 16). | Cold compiler cache (0 hits of ~250 compiles; ~100 MB stored), plus `vs_test_core` compiles JUCE a second time. Judge cache changes on the **second** run; check `--show-stats` hit rate in the "Compiler cache statistics" step. Warm run (2026-11-12, docs-only commit): macOS 3.5 min, Win x64 6.7 min, Win ARM64 5.9 min at 98.4 % hits, so the cache pays off. |
| pluginval finishes in ~1–5 s in CI. | Not a bug: it ends with SUCCESS. The plugin has no GUI tests (`--skip-gui-tests`) and no inputs, so strictness 10 is quick. |
| Code scanning upload returns 403 on this private repo. | No GitHub Advanced Security. Trivy results go to the log and a SARIF artifact; upload is behind `vars.CODE_SCANNING`. |

### Environment and tooling (this Mac)

- **Rosetta is not installed**, so the x86_64 slice of the universal binary can be built and checked with
  `lipo`, but not executed. Windows x64 CI is the first place Intel code actually runs.
- **CI results:** `gh` is installed and authenticated; use `gh run list` / `gh run view <id> --log-failed`.
- **pluginval**: Claude runs it itself (user rule). Download the official v1.0.4 release into the scratchpad.
- **Hooks** block writing `.gitignore` and any command that mentions `.env`. Propose `.gitignore`
  changes as text and let the user apply them.
- **Never read files under `build/`** (gitignored). Audition renders go to `$VS_RENDER_DIR` in the scratchpad.
