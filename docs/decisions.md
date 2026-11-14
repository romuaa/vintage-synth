# Design decisions log

Decisions made during implementation, with the reason for each. Newest milestone first.
Anything marked **(review)** is a judgement call that the product owner may want to change. Items marked "confirmed by owner" were reviewed and accepted.

## M7b results: Envelopes and LFO pages (2026-11-14)

| Decision | Why |
|---|---|
| **The Envelopes page follows the wireframe: "what it does" sits under a divider inside each envelope's panel,** not as a third panel (the contract's table lists it as its own panel). The titles are the derived ones of 9.3.2. (review) | The wireframe is the owner-approved direction; the table groups parameters, not rectangles. |
| **Envelope shape = the real RC curve of `dsp::Envelope`, run once per segment at 256 samples per segment** (the display rate that makes the segment's time 256 samples). As a fraction of its segment the curve does not depend on the time, only on the sustain, so the picture is cheap and exact; the handles sit on the segment ends (the decay end is 99 % of the way to the sustain, so up to 1 % of the level above the plateau handle). | 9.4. The test compares every segment end and the middle of long segments with an `Envelope` rendered at 48 kHz (0.5 %). |
| **When Envelope 1 is Inverted the axis moves to the middle of the picture** (level range 1 … -1), the shape stays above it at half the height, and a faint mirrored ghost is drawn below. The handles follow. (review) | A true mirror needs the same height under the axis as above it; clipping the ghost or drawing it at another scale would misstate what the filter does. Envelope 2 has no inverted mode and never shows a ghost. |
| **Time labels under the envelope handles are the parameters' own text, spread so they never overlap** (`spreadLabels`). | The slider values are one row below; the labels sit at the handles as 9.3.2 asks. |
| **The LFO picture's point i is the value at time (i + 1) / 1000 of the window,** so the fade-in ends exactly on the delay handle and a whole cycle ends exactly on the cycle handle (tested). The preview runs at 1000 / window Hz with the engine's own `Lfo`, so the 1 ms edge smoother of the real LFO acts on square and saw edges only in the 0.5 s window. | The contract fixes 1000 points over the window; the offset makes handle and curve agree. |
| **The LFO window is kept while a handle is dragged; a delay dragged past the window stops at its edge,** and the window is chosen again at mouse-up. | 9.3.3: the picture must not rescale under the pointer. |
| **Pulse width controls dim (display only) when neither oscillator's waveform is Pulse** (9.3.9), with "(no effect in this mode)" in their accessibility description. | Contract. |
| **`Panel::setText` repaints only the header strip** (shared change). | A title that follows a parameter (Envelope panels) must not repaint the panel's controls and pictures. |
| **The contract's paint budget "≤ 8 allocations per component" cannot hold for any component that draws text:** the software renderer allocates about 35 times per `drawText` and 5-30 per path fill (measured). The tests assert that repeated paints cost the same and that nothing is built in `paint()` by construction (strings, paths and the stroked outlines are prepared on change). (review) | The budget needs a different measure (a count of our own calls) or a different number; left for M7f. |
| **Parameter text of times is odd for non-round values** ("158.489 ms", "63.0957 ms": `juce::String (float, 0)` prints as many decimals as needed in `formatSeconds`, `ParamFactory.cpp`). Not changed here (shared display text, also shown by hosts); the sliders' value slot truncates it. (review) | Found while building the pages; a one-line fix to `formatSeconds` is for the orchestrator, because it changes what hosts display. |

## M7a results: editor foundation and Sound page (2026-11-14)

| Decision | Why |
|---|---|
| **The top bar's preset area is the existing preset bar** (bank, slot, name with `*`, `<` `>`, Load/Save/Init menus) until M7e builds `PresetActions` and the `<` name `>` Save design. (review) | The browser and `PresetActions` are M7e's scope; the bar keeps every file flow working now and its tests unchanged (`getBar()`). |
| **"Upper is not heard in Whole mode" is shown at the right of the help row, not over the page.** (review) | The wireframe layout has no free row; a banner over the page covered panel headers. The help row is always visible and still shows the hovered control's help on its left. |
| **Both parts' part pages exist at once; the edited part's is shown.** | Bindings register once at construction (9.1.5), so switching parts rebinds nothing. Costs about 40 more components. |
| **The session saves `UiState` only for an instrument that names its pages** (`setUiPageNames`); the skeleton always removes the child before restoring parameters. | B-Side's saved state stays unchanged until it has its own editor (S6). |
| **Sounding keys come from an optional `soundingNotes` of the part type** (checked at compile time). | B-Side's placeholder part need not implement it; its masks stay 0. |
| **The limiter reduction published per block is the limiter's gain at the block end**, not the minimum over the block's samples. (review) | The per-sample minimum would have to be tracked inside the limiter's loop (an engine change); the popover's meter is a display. A short peak inside a long block can be under-shown. |
| **The feed's sequencer step, step count, held-key order and live cutoff stay unpublished** (−1 / 0 / none / NaN) until M7d and M7f. | They need the `ArpSeq` accessors (M7d) and the per-voice cutoff store (U4, M7f, benchmarked). |
| **Budget 9.5 "mirror tick ≤ 50 µs" is measured for the mirror itself (0.7 µs for all 172 changed).** The whole editor update for all 172 (every binding's text and both Sound pages' pictures) is about 126 µs, and only in a tick where they all change. (review) | The pictures are recomputed once per tick, after all notifications, and only when an input changed. |
| **The window size is saved only when the user chooses one** (Size menu, corner, host resize), not the automatic first-open size. | Then a session opened on a smaller screen still fits it. |
| **Typed text a parameter cannot read changes nothing:** a choice needs one of its labels, a number needs a digit (or a word the parameter itself shows, such as "Off"). | `getValueForText` returns a value for any text; the contract says unreadable text changes nothing. |
| **Only Inter Regular and SemiBold are embedded (831 KB).** Values use Inter's default (proportional) figures: JUCE 9's font API has no OpenType feature switch for tabular figures; values are right-aligned so they do not jump much. (review, M7f) | Keeps the binary small; the contract asked for tabular figures. |
| **The oscilloscopes show two cycles of each oscillator; with Sync, two cycles of oscillator 2 with oscillator 1 at its octave ratio** (at least four points per cycle). | Sync is about the restarts; without sync the shape is what matters. |
| **The filter-curve test skips points predicted below −100 dB** (26 of 396). | There the float filter's own rounding dominates (engineering notes, "measure above the float floor"). |
| **The Link panel says "off at 0" while Cross mod is 0** (9.3.9 note). | Short enough for the 76 px panel. |
| **Placeholder pages label their sliders with the host parameter names** ("ENV-1 Attack"). | They are temporary; the real pages use the contract's labels. |
| **After review: the oscilloscopes use the voice's own code end to end** (`vintage/engine/OscPath.h`): `oscConfigFor` (what `Voice::setBlockParams` applies), the pulse-width mapping (`pwmCoefs`, `pwmAmount`, `pulseWidth`), and `oscPairSample` (what `Voice::renderSample` runs). The picture shows the pulse width at rest: LFO at its centre, ENV-1 at 0 (inverted: full), Manual as set; cross mod is drawn too. The page converts parameters exactly as the processor's binding does (`SoundPage::mirroredParams`). | The first version drew width 0.5 for every source but Manual and ignored cross mod. Shared functions make the picture and the sound unable to drift; the engine refactor was proved byte-identical (all render scripts plus a PWM/cross-mod/sync probe). |
| **The filter graph's frequency axis runs from 5 Hz (not 20 Hz) to 20 kHz** and the plot is inset by a handle's radius. (review) | The high-pass reaches down to 5 Hz: with a 20 Hz axis its handle sat half outside the graph and did not move between 5 and 20 Hz. |
| **Self-oscillation:** from k ≥ 4 (resonance ≥ 0.964) the curve is clamped at the top of the axis and the graph says "Rings by itself at the cutoff (self-oscillation)". The linear-picture test covers resonance up to 0.95 (k = 3.94) at 88.2, 96 and 192 kHz. | The small-signal response has no finite peak there; the tanh in the feedback bounds the real oscillation, which a linear curve cannot show. |
| **The external-change marker has its own token `marker` (#D59CF5, 7.1:1 on the panel) and a dashed outline;** `focus` stays the solid white ring only. | Contract 9.2.3 reserves `focus`; switches with a marker looked keyboard-focused. |
| **Delete and Backspace no longer reset a control** (deviation from the keys row of 9.2.1). (review) | A host may forward Delete meant for a clip or region; double-click and the context menu still reset. |
| **A double-click on a segment selects that segment;** reset is on the context menu (deviation from the double-click row of 9.2.1 for segmented switches). (review) | A fast second click on another choice must not jump back to the default. |
| **The wheel on discrete controls adds up fractions of a notch;** reversed scrolling is honoured on every control. | A trackpad flick produced one full step per event and raced through all waveforms. |
| **A session restored while the editor is open applies its saved window size.** | The size is part of the UI state; keeping the old size would silently disagree with the session. |
| **A restore takes out every `UiState` and every `Preset` child** (a hostile session may hold several); the first one counts. | One-removal left duplicates in the parameter tree, and each save added another. |
| **Unknown `UiState` attributes are dropped on the next save.** | Acceptable before v1.0 (no released sessions); from v1.0 a migration would keep what it does not understand if needed. |
| **A page that is hidden ends its open gestures** (keyboard page switch during a drag, a session restore changing the page). | 9.1.2 rule 5; otherwise a host gesture stayed open until the next interaction. |
| **Accessibility now:** explicit focus order (rail, top bar, page, help line, popover), a checked state on toggles and rail buttons, a value interface on graph handles (reads every parameter of the handle; typed text sets its first one), and the output meter announces at most once per second and only when its rounded value changed. **Left for M7f:** per-segment radio-button children of `SegmentedSwitch`, a screen-reader pass on macOS and Windows, the help line as a live region, focus order inside pages beyond reading position, the second (y) parameter of a graph handle through accessibility. | The basics a keyboard and screen-reader user needs first; the rest needs a real screen-reader pass. |
| **pluginval's editor tests run in the local macOS gate only;** CI still runs pluginval with `--skip-gui-tests` on Windows and Linux (the workflow is not changed in M7a). | Local-first verification; the CI change is carried with the workflow rewrite on another branch. |
| **The limiter reduction shown is the limiter's gain at each block's end** (see above); **the marker's dashed outline is built in `paint()` while a marker shows** (a small allocation only then). | Display only; no allocation at rest. |
| **Files differ slightly from 9.1.1:** `frame/FrameParts.*` holds PartSwitch, PageRail, HelpLine, HelpBubble, OutputMeter; `frame/EditorFrame.*` holds the top bar and the OutputPopover; the typed-value field is part of `controls/BoundComponent`. | Fewer small files; names and responsibilities as in 9.2.2. |
## B-Side S1d: tables in presets and state (2026-11-14, all review)

| Decision | Reason |
|---|---|
| **The hand-over uses four buffers per part, not three** (contract 8.6.7 says three). (review) | The audio thread has a committed buffer and, during its seqlock window, a candidate claim; with the published one these can be three different buffers, leaving the writer none. Four buffers (32 KB per part) make the writer's pick always possible; the exhaustive step exploration finds no state in which it fails. Audio side: one atomic load per block when nothing changed. |
| **Tables are stored, saved and restored verbatim** (owner decision, review round 1: comments, spacing and label names are the user's text). The canonical form is used only to compare tables (the modified marker: a change of comments or spacing alone does not mark the program modified, but the host is told) and, inside the reader, to compile. | Replaces "canonical everywhere". Limits and validation are unchanged (verbatim text passes the same limits). A table error or a session round trip never rewrites what the user wrote. |
| **`Instrument` also has `canonicaliseTable` (comparison only), `maxTableLines`, `maxTableBytes`, `maxFileBytes`, `maxJsonValues`** next to the contract's validator. (review) | Bounds must be checked before any copy, by the generic reader; comparison needs the writer. |
| **A table is lines joined by LF; a line may not contain LF, CR or NUL.** | Keeps lines and text in one-to-one correspondence, so line numbers in messages match the file and a JSON `\n` cannot smuggle a second line past the line count. |
| **Program files of an instrument with tables always write `"tables"` (empty lists for no table); tone files `"table"`.** Without tables nothing is written and the keys are ignored on reading. (review) | Fixed layout; empty = missing on reading, as the contract says. |
| **The file writer uses the short escapes for tab, line feed, carriage return, backspace and form feed.** | The table reader accepts tabs; `\u0009` is six bytes. Names never contain control characters (sanitised), so Vintage's files are byte-identical (shown). |
| **B-Side file cap 12 MiB, 160 000 JSON values**, set from the worst case found by code. (review) | The worst byte in a table line costs 2 bytes written for 1 (tab, quotation mark, backslash); 31-tab lines, 1024 lines, 32 768 bytes: 75 782 bytes per table, 9.70 MB for 128 tables; constructed worst bank 9 884 536 bytes and 136 327 values, 27 % headroom. Parse and check of it: 0.02 s. |
| **Every save refuses a file larger than the instrument's own reader accepts** ("Not saved: ..."), before anything is created. | A writer must not make what its reader refuses. With the caps above it cannot trigger for a legitimate bank; it guards the next change. |
| **A part's table error from a session is kept until the next apply or `setTable` replaces that part's table.** | The editor needs something to show; no new state of its own. |
| **A table is prepared (validated, compiled, the update built) before the hold; inside it `commitTables` (`noexcept`) only swaps the held lines and error with the update's and copies the program into a free buffer.** A table the sink cannot prepare is rejected like an invalid one (empty table, error). | No table work that allocates or can fail runs inside the hold, so audio and saved state cannot disagree (the old text is freed after the hold). The parameter writes of an apply are unchanged. |
| **`setTable`, and an applied tone whose table text differs, tell the host `nonParameterStateChanged`, on the message thread only.** | A table is not a parameter: the host must save the session. |
| **All duplicate `Preset` children are removed too** (shared `takeChildren`, linear time, same signature and meaning as the editor branch's helper). | Only the first was removed; a hostile session could keep growing the saved state. Normal sessions are unaffected. |

## B-Side S1c: table and trace formats (2026-11-14, all review)

Where the contract (8.6.5, 8.9.2–8.9.4) was silent, ambiguous or inconsistent, the reader implements
the reading below. Each row is a judgement call for the owner or the reviewer to confirm.

| Decision | Reason |
|---|---|
| **A row ending in `jump` or `repeat` costs no tick: its default delay is 0** (an explicit `ticks N` is allowed on it). `end`, `sweep` and `pingpong` rows always store delay 1 and reject `ticks`. | 8.6.2 says a jump "continues at the label (same tick)", and the arpeggio example is described as "one note per tick"; with a default delay of 1 on `jump loop` it would idle every fourth tick. The writer prints `ticks` for a row only when it differs from this default. |
| **Every cycle of rows without delay is rejected, `repeat` included** (a `repeat` row has two edges: its target and the next row; the check runs only when nothing else is wrong with the table). **Guarantee: any accepted table runs at most 64 rows (`kMaxRowsPerTick`, the lane's row count) in one tick of one lane, from any start row (the note start, after a delay, the `release` label) with any state of the repeat counters.** The rows that run without a delay form an acyclic graph, so a tick follows a path of distinct rows that ends at the first row that waits, stops or sweeps. The runtime's 64-rows-per-tick cap is therefore only a backstop. | Review follow-up: the first reading (a repeat is not endless) accepted nested `repeat a 255` rows over a `ticks 0` row, which can run more than 10^9 rows without time advancing. The literal contract reading ("a loop of rows that all have delay 0") is the safe one; a burst is written with `ticks 1` rows or a `sweep`. The model test (`tests/bside/unit/RowsPerTick.h`) checks the bound on every accepted table of the fuzz runs. |
| **The canonical text always reads back, and `validate()` says so:** the reader rejects a row whose canonical line (4 spaces of indent, `, ` between commands, numbers in register units, generated label names, ` ticks N`) is over 200 bytes ("the canonical form of this row is N bytes long") and a table whose canonical text is over 32 KB; `validate()` also checks at most 16 jump targets (plus `release`) per lane and those two limits. Property, enforced by tests and fuzzing: every table the reader accepts, and every `Program` `validate()` accepts, writes to text that reads back to the identical bytes. | Review follow-up (the writer's spelling is longer than compact input such as `res 1,res 1,...`). The alternative, lifting the line limit for the writer's output, would make the canonical text unreadable by the reader. |
| **Extra limit: at most 512 commands in a table.** | The contract bounds rows (64 per lane) but not commands per row; 8.6.7 requires a compiled program of at most 8 KB. 448 rows and 512 commands compile to 5 928 bytes. |
| **Text rules:** 32 KB = 32 768 bytes; columns are 1-based byte offsets; line length excludes the line end; LF or CRLF; text must be valid UTF-8 without control characters (C0, DEL and C1 U+0080..U+009F are rejected); a byte order mark is rejected with its own message; one message per line (the first problem on it). | A bounded reader of untrusted input stays simple and total; the preset reader is as strict. |
| **The writer regenerates label names:** `release` for the release row and `r<row>` for other jump targets; unreferenced labels, comments and spacing are dropped. The compiled program does not store names. | Round trip is defined on the compiled program (8.6.5). **Consequence for S6/8.8:** the editor must keep the user's own text; the session state's canonical text shows `r3:` for a label the user called `loop`. |
| **Command details:** relative pitch needs a sign, has at most two decimals and at most 96 semitones either way; absolute pitch is a MIDI number 0–127 or a note name C-1 to G9 (`#`, `b`, `B`; middle C = C4 = 60), and `wave <bits>` without a pitch means `~`; `key1`–`key3` are rejected as "not available yet" (S5); `set n%` is round(n × 4096 / 100) clamped to 4095 (so 50 % is 2048) with at most two decimals, and the canonical form writes register units; `pingpong` needs lo < hi and a step of 1–4095; `sweep` takes a signed step (sign optional, 0 allowed) and `x1`–`x255`; `mode` takes exactly the eight names of the contract; `route` takes digits 1–3 in any order without repeats; `firstwave` may be `none` and may contain `gate`; `ad`/`sr` are exactly two hex digits (either case on input). | The contract fixes ranges but not these spellings; each choice rejects rather than guesses. The `#` of a note name (`=C#6`) belongs to the name only directly after the note letter; everywhere else `#` starts a comment (`=55# x` and `=a3# x` are `=55` and `=a3` with a comment). |
| **Restart section:** every key at most once; `gateoff` defaults to `lead` when not given (stored resolved); the canonical form writes only non-default keys and omits a section whose keys are all default, so an absent section and a default section are the same program. | Same program, same bytes. |
| **Errors:** syntax and range errors are found line by line; undefined, duplicate and other-lane labels, labels naming no row and the loop check come after the whole text has been read, so they follow the other messages (the first 8 are kept). A label that is the last thing of its lane is an error ("names no row"). | Label targets can be defined after their use. |
| **Trace file:** writable registers 0x00–0x18 (the two analog inputs and the read registers are not writable); register and value are exactly two hex digits; `clock` 1 000–100 000 000 Hz; header words in any order but before the first write; `end` last. **The 64-byte line limit applies to the statement (bytes before `#`); a whole line with its comment may be 256 bytes.** Numbers may have leading zeros (the digit limit counts significant digits). | The contract's own example has comment lines longer than 64 bytes, which a plain 64-byte line limit would reject. |
| **Trace and reference limits are tied together:** at most 100 000 writes and 49 999 periodic checkpoints per trace, so at most 150 000 checkpoints (writes + periodic + the end), and a reference file holds one line of at most 107 bytes per checkpoint, which fits its 16 MB limit (`static_assert`s in the headers). Each writer refuses output its own reader would reject. The contract's 10^6 writes would need a 214 MB reference file. | Review follow-up: a valid 200k-write trace needed a 20.8 MB reference that `readReference` rejected. |
| **Checkpoint schedule:** one checkpoint after every write; periodic ones at N, 2N, ... strictly before `end`, taken before the writes of that cycle; one at `end` after all writes. A trace may ask for at most 49 999 periodic checkpoints (see the row above). | 8.9.2/8.9.4 say "every N cycles (and always after each write)" without ordering at equal cycles; a total count is needed to bound the reference file and the replay. |
| **`.bsref` / `.bsspec` layouts (the contract names the content but not the grammar):** see `source/bside/trace/ReferenceFormat.h`. Field ranges: 24-bit accumulators, 23-bit noise registers, 8-bit envelope counters, envelope phase 0–2, 15-bit rate counters, divider counters 0–255, two read registers. Spec: one `frame` line per 25 ms hop (level plus 23 bands, 0.1 dB, -1200..300), at most 100 000 frames, at most 64 `segment` lines of 120 bins at 20 Hz × 2^(k/12). Hashes: 64 hex digits for the trace, 40 or 64 for commits. | Lines stay short (the level envelope is per frame instead of one long line), every field is bounded, and nothing but numbers and hashes is stored (no names, no audio). |
| **Comparer:** "level ±0.5 dB" is the overall level of the whole render (power mean of the frame levels), not each frame; cells at or below -80 dB in both are ignored (as in the render harness); noise third octaves are derived from the 1/12-octave bins by power-averaging groups of four, bands with centres 50 Hz–16 kHz. Delay estimation (±256 samples, normalised cross-correlation over at least half the signal) is a helper for the side that has audio; the repository never stores reference audio. | 8.9.4 gives a level tolerance without saying over what, and asks for delay estimation against audio the repository must not hold. |
| **Replay is a template over the chip** (setup and snapshot callables) and the chip interface it needs is `advanceCycles`, `write`, `cycle`. `replay()` takes the trace file's bytes and refuses a reference whose SHA-256 or schedule does not match. | The real chip arrives in S2; the driver is proven now against a stub in the tests. |
| **New directory `source/bside/text/`** (shared scanner of the four readers) and a target `bs_juce_free` + `bside_unit` under `tests/bside/`, defined in `tests/bside/CMakeLists.txt` so the root `CMakeLists.txt` and the Vintage build are untouched. S1b will add its own `BS_SOURCES`. | Keeps S1c independent of the processor refactor running in parallel. |

## M7 editor contract (2026-11-14)

Contract: `docs/architecture.md` section 9. Open owner questions U1–U7 are in 9.8.

| Decision | Why |
|---|---|
| **The editor polls every parameter on a vblank-driven tick (≤ 66 Hz, 30 Hz timer fallback) and registers no parameter listeners.** (review) | Listener callbacks run on the thread that changed the parameter, often the audio thread; polling keeps all editor work on the message thread, coalesces bursts (a program change paints as one frame) and is testable by calling `tick()`. 172 loads per tick cost microseconds. Implements the owner's "UI reads parameter values on a UI timer". |
| **The "automated" marker means "changed by something other than this editor in the last 1.5 s";** a tick with more than 8 changes (program load, restore) sets none. (review) | VST3 does not tell a plugin which parameters carry automation. |
| **A drag that the host contests shows the user's value and writes nothing at mouse-up;** the next tick shows the parameter's real value. (review) | No snap-back writes that would fight the host; Touch/Latch hosts stop reading during the gesture anyway. |
| **Non-parameter UI state (page, edited part, arp view, window size, the delay/reverb level to restore) is saved in the session as a `UiState` child of the state;** not in programs, never "modified". (review) | Generalises "Panel Mode is UI state": reopening a session shows what you left. Page names, not indices, so reordering pages never breaks a session. |
| **Audio → editor data through one `UiFeed` of relaxed atomics (peaks with a raise/exchange protocol, key masks, step index) and one seqlock (held-key order).** (review) | Lock-free, allocation-free, about 30 atomic operations per block; nothing on the audio thread depends on whether an editor exists. |
| **Displayed values are the parameters' own text (panel parameters show 0.0–10.0, not the wireframes' percentages).** (review, U6) | One mapping: what the editor, the host and the automation lane show must agree. |
| **Every picture is computed by the engine's own JUCE-free code:** a new exact small-signal filter response, `Oscillator`, `Envelope`, `Lfo` and `ArpSeq` run offline on the message thread. The arpeggiator preview is tested against the processor's real output. (review) | The owner asked that the picture cannot disagree with the sound; running the same code is the only way to guarantee it. |
| **Envelope graph x axis is the sliders' (logarithmic) scale, not seconds;** the LFO graph uses real seconds with a window chosen from fixed steps and kept during a drag. (review) | Envelope times span 1 ms–10 s: a linear axis would hide short segments. The LFO is about rhythm, where real time reads best; a fixed window keeps the handle under the mouse. |
| **Envelope page titles are derived from `vcf.env_source`** ("Envelope 1 (filter, pitch)" or "(pitch)"); Envelope 1 polarity, missing from the wireframe, sits under Envelope 1. (review) | Checked in `Voice.cpp`: pitch modulation is always Envelope 1 (with its polarity), the filter takes ENV-1 or ENV-2, the VCA always ENV-2. The wireframe's fixed title was only true for one setting. |
| **Delay and reverb get a header power switch that sets the level to 0 and restores the last level;** no new parameters. (review, U5) | The engine already bypasses at level 0 bit-exactly; no state, program or host-list change. |
| **"Load tone" takes a source-part choice (Lower or Upper tone of the selected program) and two targets (→ Lower, → Upper).** (review) | Resolves the wireframe's open point: a program has two tones; Dual and Split programs need both to be reachable. |
| **The LIMIT chip only shows and opens the output popover; the popover holds the switch and the ceiling.** | Each parameter has exactly one control (testable completeness). |
| **Layout from a fixed grid, never from measured text; fixed aspect 75–200 %.** (review, U1) | Identical layout on every platform, so a text-free layout snapshot can be an exact cross-platform test. |
| **No image snapshot tests as a gate;** layout snapshots and binding tests instead, plus PNG renders for human review. | Glyph rasterisation and antialiasing differ per platform and OS update: a tolerant image diff misses regressions, a strict one fails everywhere. |
| **Control logic lives in JUCE-free model classes; components only forward events and paint.** | Interaction tests run headless on all five targets and under sanitizers without a display or mouse. |
| **Help texts: `docs/ui/help-texts.md` is the single source, embedded and parsed by the plugin; a test fails for a parameter without exactly one text.** | Docs and plugin can never drift; wording reviews happen in one file. |
| **Help-text corrections after checking the code:** velocity → VCF reduces the filter envelope for soft notes (not "brighter"); the mod wheel's vibrato uses the LFO; Unison is one note at a time; the split key itself plays Upper; step velocity's effect depends on the part's velocity settings; LFO delay is silent for half the time, then fades in; Low freq ignores keys, bend and glide; sync makes oscillator 2 set the pitch; PWM only changes the Pulse wave and Manual sets a fixed width. | Accuracy: the texts are the manual for most users. |
| **M7 code starts after S1a has merged; M7a (foundation + Sound page) is serial, M7b–M7e (pages) run in parallel adding new files only.** | Both S1a and M7 touch the editor and the block algorithm; new-files-only keeps parallel agents out of each other's way. |

## M7 UI direction (owner, 2026-11-14)

| Decision | Reason |
|---|---|
| **Tabbed workspace: every purpose gets the full window** (Sound, Shape, Play, Arp+Seq, Effects, Browser), with a slim persistent frame. Chosen from three wireframe concepts; a single signal-flow page seemed to be missing features and a both-parts-at-once page was far too busy. | Better visibility and larger controls that are easier with a mouse. |
| **Sliders, not round knobs,** for continuous controls. | Knobs are hard to use with a mouse. |
| **As simple as possible; graphs for visualisation and intuition.** A new user must be able to see how to set up the arpeggiator without a manual. | Owner's requirement. |
| **The UI follows automation live.** Sliders, graphs and switches move when host automation, MIDI control or a program change alters a parameter; nothing on screen may show a stale value. | Owner's requirement: an automation track must be seamlessly visible in the synth UI. Implementation note for M7: the editor reads parameter values on a UI timer (never from the audio thread), including the graphs derived from them. |
| **Round-2 wireframes approved as the direction (2026-11-14),** with three changes: a colour tint per part so it is obvious which part is being edited; the Shape page split into LFO and Envelope pages; hover help texts on every control. | Owner's review of the drafts. |
| **Round-3 wireframes approved; all recommended options of the editor contract (9.8, U1–U7) chosen (owner, 2026-11-14):** fixed-aspect scaling 75–200 %, embedded Inter font (SIL OFL 1.1), dark only, static graphs with live markers, level-0 power switch for delay and reverb, 0–10 value text, help bubbles after 600 ms. | Owner: "approved, all recommended". |

### After the first hands-on check of M7a (owner, 2026-11-14)
- **The frame and the Sound page are approved.** The placeholder pages (parameters as plain
  sliders) are "messy and hard to understand"; the owner looked at the round-3 wireframes again
  and confirmed them, so Play, Arp + Seq and the other pages are built as drawn. Rule for every
  page: no page may be a bare list of sliders; each has its picture and purpose-built controls.
- **Clicking the program name opens a list of the bank's programs grouped by purpose** (owner
  request). Contract: architecture 9.3.8. The groups are the programs' `category` values; the
  factory bank's categories (Bass, Brass, Keys, Lead, Pad, Strings, Sound FX, ...) are kept
  as they are. (review: the owner's examples were Horns, Keys, Leads, Noise, Radical, Strings;
  rename factory categories only if the owner asks after seeing the menu.)

## B-Side S1 contract (2026-11-14)

Contract: `docs/architecture.md` section 8. All (review); the owner decisions it waits for are
listed in its 8.12 (Q1–Q8).

| Decision | Why |
|---|---|
| **S1 is split into S1a (shared skeleton, Vintage unchanged), S1b (plugin shell with a test tone) and S1c (table and trace formats).** S1a is proven by bit-identical raw renders, an identical parameter dump and identical state and preset bytes. | S1a is the only step that touches the shipped instrument; making it reviewable alone, with a proof that cannot pass by accident, keeps the risk where it can be seen. |
| **The shared skeleton is a CRTP base (`shell::BiTimbralProcessor<Derived>`) with a `PartEngine` concept;** the block algorithm's order is written down and may not change. | Both instruments are bi-timbral with the same router, arp/seq, mixer, effects and output stage; sharing the orchestration (not only helpers) keeps the block-size and release rules in one place. Hooks run per block, so no per-sample virtual cost. |
| **No shared static library of our code; each plugin compiles the common sources.** B-Side's JUCE-free code has its own test executable without JUCE. | JUCE modules and `JucePlugin_*` definitions are per plugin target; a JUCE-free unit test binary keeps the inner loop fast and limits the gate's growth. |
| **Proposed identity: plugin code `Bsy1`, bundle ID `com.exitium.bside`, manufacturer `Exit`** (Q1). | Parallel to `Vsy1` / `com.exitium.vintagesynth`; distinct VST3 class ID. |
| **Two parts (Lower/Upper with Whole/Dual/Split)** like Vintage (Q2). | Reuses the router, mixer and arp/seq unchanged and allows layering Model A over Model B. |
| **One note = one whole chip** (three oscillators, one filter) by default; the hardware's shared-chip layout is S5. | A playable poly needs a filter per note; three oscillators per note are how the classic patches stack and ring-modulate. |
| **Chip time is an integer cycle count (64-bit + 32-bit fraction); the digital state is advanced in closed form per internal sample and must equal a naive per-cycle stepper and the reference emulator bit for bit.** | Integer time makes ticks and digital state independent of block size and oversampling; the stepper gives an exact check that needs no external data. |
| **Waveforms are band-limited; the 12-bit amplitude staircase is not rendered; envelope steps are not band-limited.** Alias budgets for B-Side: −100 dB for plain waveforms (as Vintage), −96/−80 dB for Model A DAC and combined waveforms, −90 dB for write discontinuities, a spectral match for noise (Q6). | The staircase lies above 20 kHz for any audible pitch; envelope steps are the chip's own content. The project rule forbids relaxing the gate, so the B-Side budgets need the owner's approval. |
| **Smooth resolution by default; Authentic rounds pitch, pulse width and cutoff to the registers.** In Authentic, notes above the frequency register's maximum (about 3.85 kHz) play at the maximum. | Smooth playing by default, the original stepping on request; clamping is what the chip does. |
| **Default chip model for Init: Model B** (Q3). | Predictable, clean filter; Model A is chosen per patch. |
| **The tick clock is an integer number of cycles; Frame rate = 19 656 / 17 095 cycles; every note has its own tick phase starting at its note-on.** | Exact, block-size independent ticks; per-note phase gives every note the same attack. A global phase like the hardware's is an S5 option. |
| **Seven lanes per tone (three wave/note, three pulse, one filter), up to 64 rows each; writes of a tick are applied at one cycle in a fixed order.** | One wave and one pulse lane per oscillator, because a note owns three oscillators; a fixed order makes register traces exact. |
| **Lane ownership:** a register follows the host parameter until a lane writes it for the note. | Tables and host parameters work together without one silently overriding the other. |
| **Note-off acts at its sample (release labels, gate off), not at the next tick** (Q7). | Up to 20 ms extra release latency would be felt when playing; the tick clock keeps running. |
| **Restart policies: Clean (default), Precondition, Authentic; Look-ahead in S5** (Q8). Clean resets the envelope's counters directly when a note did not get its full lead, so the envelope delay quirk never happens unless chosen. | Live playing cannot announce the next note; Clean is predictable, the other policies keep the original behaviour available. |
| **Velocity is a per-note gain after the chip** (plus an optional cutoff amount). | The chip has no per-voice level; changing envelopes or the volume register would change the sound, not only its level. |
| **The table text format is strict (errors, not clamping), bounded (32 KB, 1024 lines, 64 rows per lane) and line-based; in presets it is an array of lines.** | It is an authoring language and untrusted input; lines fit the strict JSON reader's string cap and diff well. |
| **A voice copies its compiled program (≤ 8 KB) at note-on; programs are published in three buffers inside the ApplyGate hold.** | No lifetime tracking on the audio thread, and parameters and table are always seen together. |
| **Per part 63 host parameters plus `chip.clock`; 202 in total after S2**, added in one serial step; behaviour lands over S2–S4. | Matches the project's append-once discipline and avoids parameter churn across milestones. |
| **Reference emulator: used only by an adapter outside this repository; reference data (state checkpoints and spectral features, never audio) is committed under neutral names with hashes only** (Q4). Analog constants and combined waveform shapes are fitted, not copied (Q5). | Keeps third-party names and code out of the repository and the plugin, keeps the licence question to a developer tool, and makes the authenticity tests reproducible offline. |

## B-Side S1b: plugin shell and gates for two plugins (2026-11-14)

All (review). Contract: `docs/architecture.md` 8.1, 8.2.5, 8.3.

| Decision | Why |
|---|---|
| **A setup mistake of an instrument throws `vs::ConfigError` from the constructor, in every build type:** `BoundParam::bind` (a parameter missing from the layout, named in the message) and `verifyConstructed()` (no `initPresets` call, named by the instrument's `kName`). The plugin entry point (`shell::createPluginProcessor<T>()`, the body of `createPluginFilter()`) catches `ConfigError`, writes the message to stderr and the JUCE log and calls `std::abort()`. | Before, the mistake was a `jassert` (debug only) and then a null dereference. An exception must not cross the plugin's C boundary into a host: the host would terminate without a word, or unwind through its own frames. A message and an abort is no worse than the old crash and says what is wrong; the exception form keeps the check testable (the tests build processors with a missing parameter group and with no `initPresets` call). Both mistakes are found by the first test or load of a new instrument, never by a user. |
| **`initPresets` cannot be checked inside the base constructor (it runs after it), so every use of the presets goes through `pm()`, which throws `ConfigError` when it is missing, and the entry point calls `verifyConstructed()` right after constructing.** | A forgotten call then fails at construction in the plugin and at the first use in a test, by name, never as a null pointer. The alternative (the base builds the presets from static hooks of the instrument) would change the contract's construction order for no gain. |
| **The plugin name is `Derived::kName`, a literal in each instrument; tests assert it equals the build's `JucePlugin_Name`.** | A second instrument in one test build (the contract tests use one) reports its own name; a typo in the literal is caught by the test, not by a host's plugin list. |
| **The `static_assert`s cover the per-block hooks (`readInstrumentParams`, `applyInstrumentParams`, `partPan`, `synthTailSeconds`, `part`), as listed in the contract; the methods of the `PartEngine` concept are not asserted.** | Those are what 8.2.2 calls hooks; asserting `noexcept` on a part's `render` would make Vintage's `Part` a change in this step, which must leave Vintage untouched. Candidate for S2. |
| **B-Side's programs keep the tone prefixes `lower.` and `upper.` although no parameter has them yet** (empty tones). | S2 appends the per-part parameters and the schema, the preset files and the bank code need no change; empty tones round-trip like any other (tested). |
| **The test tone: a sine per note from the shared voice allocator (Poly, 16 voices), level 0.3 × velocity per voice, pitch bend ±2 semitones, no mod wheel, a stolen voice restarts its attack from its current level; latency 0.** | Enough to exercise every path of the skeleton (steal, pedal, bend, sample-accurate events, All Notes/Sound Off) and to hear that the chain works; replaced in S2/S4. |
| **`VS_BUILD_BSIDE` is a CMake option (default ON); with `OFF` nothing of B-Side is configured or built. Both plugin targets are configured by one function, `vs_configure_plugin`; the AudioPluginHost no longer depends on the plugin bundle of B-Side (the dev-host script builds the one it opens).** | One place for definitions, warnings, LTO and the Linux link options, so the two plugins cannot drift. |
| **`host_smoke --profile vintage|bside <bundle>` (default vintage); ctest runs `host_smoke --profile vintage` and `host_smoke_bside`.** | The contract asks for a profile argument; an omitted profile keeps the old command line meaning. |
| **B-Side's tests are discovered with the ctest name prefix `bside: `.** | Both test executables are discovered into one ctest list; a name that exists in both (a Catch2 test case name) would make CMake refuse the second. |
| **One list of sanitizer test targets, `scripts/sanitize-targets.sh`, sourced by `preflight.sh` (mac-sanitize), `linux-check/inner.sh` (linux-sanitize) and the CI sanitizer job.** Required: `VintageSynthTests`, `render_regression`, `bside_unit`, `BSideTests` (a missing one is an error). Future, optional until they exist: `bside_render`, `bside_trace`. A test-looking target the build system defines (`*Tests`, `*_unit`, `*_render`, `*_trace`) that is in neither list is an error, and before ctest runs every step prints the number of tests registered per built target and fails on a built target with none. | A first version selected targets with `ninja -t targets \| grep -q` under `pipefail`: grep exits at its first match, ninja dies of SIGPIPE, the pipeline fails, and every target looked missing, so the sanitizers silently skipped half the tests and still said PASS (found in review). The list is captured once and searched with a shell pattern; one file keeps the three runners from drifting; silence is now impossible by construction. |
| **`preflight.sh` refuses `VS_PREFLIGHT_PLUGINS` other than `all` unless `--quick` or `--only` is given, and the summary always says which plugins pluginval and the validator covered.** | A variable left exported in a shell must not turn the hand-over gate into a one-plugin run unnoticed. |
| **The CI artifacts, the distribution load tests and the bundle merges are per plugin (`vst3-vintage-<row>`, `vst3-bside-<row>`, `vst3-vintage-windows-universal`, …).** | Contract 8.1.3: "pluginval, validator and bundle merge per bundle; one artifact per plugin". |
| **The user folder is `Documents/Exitium/B-Side/`, factory bank "Factory" with the single file `A1-init.json`; every other slot is Init.** | Contract 8.3. |
| **The JUCE modules of the test cores are compiled once, in `vs_juce_modules`, shared by `vs_test_core` and `bs_test_core`** (8.1.2 "Optional"). The modules link PRIVATE to the library (a PUBLIC link would compile JUCE's interface sources into every consumer again and the copies would collide); each core still takes its include directories and definitions from its own plugin target; the library is built with Vintage's definitions. | Cold test build on this Mac (arm64 Release, load 4 to 15): 45 s wall / 460 s CPU before, 26 s / 319 s after (CPU -31 %); Linux sanitizer step 10m34 before, 5m14 after (noisy). Safe because no JUCE module code the cores use reads a `JucePlugin_*` definition (the plugin-client module is not in it). The shipped plugin targets are untouched: `ninja -t commands` of both bundles is identical before and after. **Review:** a core that ever needs plugin-specific JUCE module behaviour (it would see Vintage's definitions) must stop using the shared library. |

## Wave 4: output stage, headroom and soft limiter (2026-11-13)

Owner decision "more headroom and an adjustable soft limiter"; the numbers below are judgement
calls. All (review) unless marked; the owner should confirm the trim and the limiter by ear.

**Owner, 2026-11-14:** the default level after the trim is good ("User can boost them if wanted");
Wave 4a approved for merge. Eight golden references for the factory bank are enough (not one per
program): the load, level and release tests cover all 64.

| Decision | Why |
|---|---|
| **Fixed output trim −6.02 dB (× 0.5), multiplied into the master gain.** The loudest chord (Dual, 32 voices, velocity 127, dry, default master −6 dB) now peaks at **−2.57 dBFS** (was +3.45); moderate effects −0.45 dBFS (was +5.57); extreme effects still reach the clamp without the limiter (+12.04 dBFS, was the same). Single notes and programs are 6 dB quieter. | The brief asked for ≤ about −1 dBFS: at least −4.45 dB was needed. −6.02 dB is the smallest power-of-two gain that meets it, leaves 1.5 dB of margin for presets louder than the test chord, and is exact in floating point: a render differs from the old one by exactly 0.5 per sample, which made the golden re-bless provable. Per-voice, per-part and effect gains are untouched, so relative program levels and the effect send balance stay. |
| Master volume range (−60 … +6 dB) and default (−6 dB) unchanged. | No strong reason to change them; the trim alone gives the headroom, and existing sessions keep their master setting. |
| **Soft limiter after master gain and trim, before the +12 dBFS clamp: stereo-linked, zero latency, no look-ahead.** Instant attack with a 20 ms hold and a 150 ms release (fast detector); a slow detector (20 ms attack, 1 s release) fed from the fast one; the envelope is the larger of the two (program-dependent release). Soft knee 3 dB wide below the ceiling: `out = C − 3·exp(−(L − (C − 3))/3)` dB, which never reaches the ceiling. | The owner preferred no look-ahead. Because the envelope is never below the current sample and the curve stays below the ceiling, no sample exceeds the ceiling even on a hard transient (measured: 0 dB overshoot; the clamp is never reached). The slow detector keeps the gain steady on sustained loud material (distortion −41 dB) while a short peak releases fast. |
| Measured: the loudest chord driven 6 dB over a −1 dBFS ceiling: **distortion −41.4 dB** (residual after a least-squares gain fit per 20 ms window), −27.4 dB if slow level changes over 2 s are counted too; level down 5.2 dB; output peak −1.15 dBFS. With only the fast detector (150 ms release) the distortion was −28 dB; with a 1.5 s single release −36 dB but slow recovery after every transient. | Basis for the release choice. |
| Gain exactly 1 below the knee start (ceiling − 3 dB): the samples are not touched, so quiet material is bit-identical with the limiter on or off. At the defaults (ceiling −1 dB) the knee starts at −4 dBFS; no golden scene reaches it (the loudest, `poly_chord_steal`, peaks at −4.28 dBFS). | Transparent where it is not needed; provable by test. |
| **Two parameters, appended: `master.limiter` (Off/On, default On) and `master.limiter.ceiling` (−12 … 0 dB, continuous, default −1 dB).** A golden scene (`limiter_drive`) drives the limiter. No release, knee or look-ahead controls. | Small surface; −1 dB leaves room for inter-sample peaks after conversion. Release is program-dependent, so a control adds little. |
| **Both are session settings like master volume:** saved with the host session, not stored in programs, tones or banks; loading or initialising a program leaves them alone and changing them does not mark the program modified. | Least surprising: an output-stage setting the player chose for the session should not change when browsing sounds. Consistent with master volume (section 6b.1). |
| The ceiling is smoothed (20 ms linear in dB). **The limiter switch is crossfaded over 5 ms — a deliberate exception to "never smooth switches".** The applied gain is g^m with m ramping 0 ↔ 1 (linear in dB). Fully off, the limiter does nothing (bit-identical to the path × trim, no cost); switched on from fully off it starts clean; below the knee the samples stay untouched during a fade. Measured: switching while reducing 8.4 dB changes the gain by at most 0.035 dB per sample. | Review: an unfaded switch stepped the level by the whole gain reduction (+6 to +24 dB) — a click on a host-automatable parameter. The "never smooth switches" rule exists for switches that change the sound's structure; this one only scales the level, so a fade is safe and keeps block-size independence (per-sample ramp). |
| A session missing a parameter loads it at its default (the processor adds an empty entry before applying the state). | A pre-Wave-4 session must not keep whatever limiter setting the instance happened to have. JUCE 9.0.5 already does this through an internal callback; the explicit step does not depend on that. |
| The ceiling is continuous and shown in 0.1 dB ("0.0", never "−0.0"). | A 0.1 dB interval snapped stored values, so a session did not restore the exact normalised value. |
| The limiter adds no latency and no tail; reported latency and tail length are unchanged. | Zero look-ahead; its gain is ≤ 1 and has no memory of the signal itself. |

## Wave 3 integration contract (2026-11-12)

All (review).

| Decision | Why |
|---|---|
| Effects stay global and in series (chorus → delay → reverb) on the stereo bus; 11 parameters. Delay time is free (1 ms–2 s, default 700 ms, ping-pong on by default as in the approved demo); tempo-synced delay can be appended later. | Matches what the owner approved by ear; per-part sends and delay sync are additive later. |
| One arpeggiator/sequencer settings set with a part selector (Lower, Upper, Both); the sequencer's 16 steps are host parameters (48 of them). | One set is easy to understand and automate; step parameters make the sequencer usable and testable before the custom UI exists and are saved with the state. Parameter count becomes 170. |
| With no usable host transport the arp/seq runs on its internal tempo parameter. | Works in simple hosts and when just playing. |

## VCF slope switch on sounding voices (2026-11-13)

(review)

| Decision | Why |
|---|---|
| **A VCF slope change crossfades the output between the stage-2 and stage-4 taps over 5 ms on every voice that is sounding** (held or releasing); idle voices and notes starting from idle take the slope at once. Outside the 5 ms the plain tap is used, so the steady sound is bit-identical (all render-regression scripts compared bit for bit). This applies to a hand flip of the switch as well as to a program change. | The two taps differ in phase and level, so switching between them stepped the output (0.18–0.33 on a held sine chord against its own 0.018). 5 ms is the parameter-smoothing time used elsewhere; the crossfade is a gain ramp between two correlated signals, so it has no step of its own. This does not smooth the switch value itself (the "never smooth switches" rule): the filter is in one mode or the other, only the output moves between them. |
| **Other switches are left as they are.** Measured on a held chord and in a program apply, waveform, range, sync, VCO-2 mode, ENV-1 polarity, VCF envelope source, ENV-1 key follow and chorus mode cause no step larger than the sound's own. | A waveform switch jumps the oscillator to the new shape at its current phase; the jump is no larger than the new waveform's own edges and is low-passed by the VCF, so it is not heard as a click. Not band-limited, so it can alias once; left as is unless the owner hears it. |

## M6b: the factory bank (2026-11-13)

All (review): drafted by an agent that cannot hear; the owner's audition decides.

| Decision | Why |
|---|---|
| **Bank layout by theme:** A Init + brass, B strings/pads, C keys/plucks/bells, D basses, E leads, F organ/percussion/effects, G Dual (G1–G4) and Split (G5–G8), H arpeggiator, sequencer and effect showcases. Spec: `docs/factory-sounds.md`. | Browsable by sound type; brass first because it is the genre's signature. |
| **The level rule is relative, in three groups** (sustained, percussive, bass): the standard phrase at velocity 100 within ±2 dB (unweighted RMS, note-on to last note-off) of the group median; plus a headroom check: a full-velocity eight-note chord at most 3 dB above Init's (RMS and peak) and below 0 dBFS. **Measured with the output limiter off;** with it on at its defaults no standard phrase may reach the knee (−4 dBFS). Delivered within ±0.6 dB; after the −6.02 dB output trim the medians are sustained −27.3, percussive −31.5, bass −32.8 dB, Init's chord is −20.5 dB RMS / −7.0 dBFS peak, the loudest phrase peaks at −8.3 dBFS (F5) and the loudest chord at −4.5 dBFS (C2): nothing touches the limiter. The contract's "chord below 0 dBFS" (`docs/architecture.md` 6b.4) is kept and the relative rule is added there. | Owner brief for M6b: the output stage keeps relative levels, so programs mainly need to match each other; measuring without the limiter keeps it from hiding a hot program. Unweighted RMS keeps the measure simple; bass has its own group because it carries more energy per perceived loudness. |
| **Levels are set with each part's VCA level only**; master volume is not stored in programs and was not touched. | The contract excludes master volume from programs; one control per part keeps trims easy to read. |
| **Split programs split at G3 (MIDI 55)** with the bass part in Lower. | Leaves a usable two-octave bass range and middle C and up for the other part. |
| **Arp Hold is off in every program;** Arp pattern Down and 16 unison voices are not used. | A held pattern never stops, so it fails "silent after the tail" and surprises when browsing. The unused options are shown by their neighbours. |
| **Programs with the −12 dB VCF slope (A7 B3 B4 B7 C5 D7 F4) step audibly when applied over sounding notes** (see the engine finding in the M6b report): kept, because the slope is a feature the bank must show. | The fix belongs in the engine, not in the presets. **Fixed** (2026-11-13): the slope now crossfades on sounding voices (see "VCF slope switch on sounding voices"). |
| **Render-regression references cover seven designed programs, loaded by name, not all 64** (`docs/architecture.md` 6b.5 asks for one per program). | Each reference is about 40 KB; 64 would add about 2.5 MB of features to the repository. The factory tests already check every program's sound, finiteness, release and level. Owner decision whether all 64 are wanted. |

## M6a results: preset and bank infrastructure (2026-11-13)

All (review) unless marked.

| Decision | Why |
|---|---|
| **Preset files are read by our own strict JSON reader** (RFC 8259 subset, 1 MB cap, depth ≤ 12, at most 4096 members per object, duplicate keys rejected, linear time). JUCE's JSON parser is not used for untrusted input. | Sanitizers and review: JUCE's parser overflows on long digit strings, also accepts single-quoted strings (which defeated a pre-check), and is quadratic in the number of keys. |
| **A session names its user bank by plain file name inside the banks folder, never by path; restoring a session reads no files.** The bank is resolved later on the message thread. All preset reads go through one bounded reader that accepts regular files only. | Review blocker: a crafted project could hang the host (path to a pipe), exhaust memory (a device file) or, on Windows, trigger a network connection (UNC path). |
| A missing user bank leaves the restored sound and name intact, detached from any slot and marked modified; the host is shown slot 1 of the factory bank. | Silently presenting a factory slot as current while another sound plays would mislead. |
| **Applying a program is serialised by one lock off the audio thread; the audio thread takes a consistent copy of all block parameters (sequence-counter read) and keeps the previous block's set while a change is in progress.** | Three hand-over gaps let a block read a half-applied parameter set; a test now proves no mixed set is ever observed. |
| Storing into or renaming a slot of a user bank writes the bank file at once (atomic replace). | The change was lost at the next session load. |
| Every preset file carries an instrument id; bank size, slot naming and file extensions belong to the instrument, not the shared code. Format 1 is not released yet, so this is free. | A file of the future second instrument must not load as all-defaults. |
| Number text in files is formatted and parsed without locale-dependent functions; names are stripped of control, bidirectional and zero-width characters. | Hosts can change the process locale; names reach the host's program list. |
| **MIDI Program Change timing: good enough for now (owner, 2026-11-13); the owner will test it properly later.** It is applied by the message thread shortly after it arrives: notes in the same block start on the old program and are then released, and in offline rendering the moment it takes effect is not deterministic. No Bank Select; Program Changes 64–127 are ignored. Every program change releases held notes. | Sample-accurate program changes need the bank precomputed for the audio thread: a larger design step. Owner decision whether it is needed. |
| The test that constructs the editor and preset bar is skipped under AddressSanitizer (a report inside the system graphics library); it runs in the normal builds. The bar's menus and file dialogs are not exercised by any test. | Needs a human at the screen. |

## M6 contract: presets and banks (2026-11-12)

All (review).

| Decision | Why |
|---|---|
| Three levels: tone (one part), program (whole instrument without master volume), bank (64 programs, 8 × 8). One read-only factory bank compiled in; user banks are files. | Mirrors how a bi-timbral instrument is used: sounds per part, combinations as programs. |
| Presets are JSON text, parameters by ID with plain values, tolerant loading, malformed files rejected whole. | Human-readable and diffable; survives added parameters in both directions. |
| Applying a program releases all notes first and sets parameters through the host-notifying API on the message thread. | Meets the real-time rule and keeps hosts and automation in step. |
| Factory programs are trimmed to a level rule (a full-velocity eight-note chord below 0 dBFS at the default master volume). | Answers part of the open output-level question at the preset level, whatever is decided for the output stage. |
| A minimal preset bar above the generic editor until the custom UI exists. | Makes presets usable and testable before M7. |
| Factory sounds are drafted by an agent from a written specification and auditioned by the owner. | Agents cannot hear. |

## Wave 3 results: effects and arp/sequencer in the plugin; oscillator fixes (2026-11-12)

All (review) unless marked.

| Decision | Why |
|---|---|
| **Host bypass is a clean stop:** 5 ms fade, then everything is released and reset (parts, key router, arp/seq incl. Hold and pedal, effects); MIDI is ignored while bypassed; keys still held are silent until pressed again. | JUCE's default bypass drops MIDI, so a note-off arriving during bypass hung the note; with the arpeggiator on the pattern never stopped, and old echoes returned afterwards. |
| **Parameter values are sanitised where they enter the engine:** non-finite → the parameter's default; otherwise clamped to its range. | A NaN from a host silenced the whole instrument for as long as it lasted. |
| **Integer and choice parameters are stepped for hosts** (including the split point). | JUCE's integer parameter reports itself as continuous to VST3 hosts, which then store values between steps. |
| **Reported tail = synth release + effects, capped at 30 s.** Synth part: 2.5 × the longest ENV-2 release among the parts that can sound (time to −100 dB), × 5.66 with envelope key follow, + 8 % for Vintage. Effects: the longest tail of the effects that are on. Hosts cannot be notified when it changes. | The tail ignored the synth's own release (up to 10 s). |
| **In Whole mode the arp/seq applies to Lower whatever the part selector says.** | Whole + selector Upper gave no arpeggio at all. |
| When the arp/seq is Off or a part is not selected, that part gets the router's events directly (bit-exact). A key-mode change resets both arp/seq instances and re-synchronises the pedal. | Keeps "off = exactly as before"; without the resync, voices piled up after a mode change with the pedal down. |
| A key-mode change drops a Hold-latched pattern; arp on → Off with keys held is silent until re-pressed; with the transport stopped the arp runs at its own tempo; arp/seq/effect automation is block-accurate; the play head's loop flags are not used; a step with velocity 0 plays at 1/127. | Consistent with the other release rules; accepted limits. |
| **Output level: more headroom by default, plus an adjustable soft limiter (owner, 2026-11-13).** Measured before the change: at the default master volume (−6 dB) a 32-voice Dual chord at full velocity peaked at +3.45 dBFS dry, +5.6 dBFS with moderate effects, and reached the +12 dBFS hard clamp with extreme effect settings. | Heavy chords must stay under 0 dBFS at the defaults; the limiter catches what is left and the player can set it or turn it off. The hard clamp stays as a safety net only. The headroom is a global output trim, so the relative levels of presets do not change. |
| **Hard sync with a sine VCO-1 is band-limited exactly** (the reset error of a sine is a gated sinusoid, integrated against the same windowed-sinc kernel). Aliasing went from −43…−60 dB to −109 dB or better; wanted partials move by at most 0.003 dB; all other waveforms are bit-identical. | The study's defect 1. Adding polynomial correction orders made it worse, because their residuals only cancel in the full series. |
| **Offline, a note that starts with Cross Mod above zero renders its whole voice at the real-time rate** (second oscillator pair, same code and coefficients), then is interpolated 2× to the part's offline rate; other notes are delayed 15 internal samples to stay aligned, and the reported offline latency grows accordingly. Decided per note at note start. | The study's defect 2: a bounce of a Cross Mod patch must sound like playback. Offline partials now differ from real time by ≤ 0.014 dB (was up to 55 dB). Oscillators alone were not enough: the filter's saturation also aliases differently with the rate. Aligning sync and Cross Mod timing instead was rejected: it would move real-time partials by up to 49.6 dB. |
| **The real-time Cross Mod sound is unchanged** (bit-identical renders against the previous version). **Classic Cross Mod is guarded by a regression test of 72 measured levels (+3 dB), not by the −100 dB gate.** | Owner decision to keep the sound. |
| The sync gate has one documented exception: a 20 % pulse with VCO-1 near 10 kHz at 88.2 kHz 1× measures −99.2 dB, which is the oscillator's own floor above the 5 kHz range the gate covers. | A longer correction table would change the saw, which had to stay bit-identical. |

## Local-first verification; CI offline (owner decision, 2026-11-12)

- **Everything that can be verified on the development Mac is verified there; GitHub Actions is a
  rare, deliberate confirmation step, permanently** (also after the repository is public). Reason:
  the account's included Actions minutes ran out after 31 build runs in one day.
- **CI is offline until the owner makes the repository public for a release** (up to a month).
  Until then: no workflow runs; branches are pushed for backup only.
- **Verified locally:** macOS universal (build, all tests, pluginval, and the VST3 validator built
  from the pinned SDK commit — the owner now allows building it locally), Linux ARM64 with GCC in a
  container, sanitizers on both, the security scan.
- **Not verified until CI returns: Windows x64, Windows ARM64, Linux x86_64.** No Windows virtual
  machine and no emulated x86_64 Linux builds (owner decision). Every merge made in this period is
  listed in `CLAUDE.md` under "Merged without Windows / Linux x86_64 verification", so the first
  CI run can be read against it.
- **Merges while CI is offline:** infrastructure-only changes (build, test, CI, scripts, docs) may
  be merged to `main` after the local gate passes (owner pre-approval); product milestones still
  wait for the owner's audition and approval.
- **pluginval exit crash on Linux x86_64** (SUCCESS, then a segmentation fault at exit): the
  investigation points to a known use-after-free in the JUCE host code inside pluginval 1.0.4's
  own build, fixed in later JUCE versions; not in our plugin. Not proven for our runs (no backtrace
  could be taken under emulation). To do when CI is back: confirm on real x86_64 hardware, then
  treat exactly that signature as a tracked warning until a fixed pluginval release is used.

## Cross Mod aliasing (owner decision after the study, 2026-11-12)

- **The current Cross Mod sound stays as the instrument's sound ("Classic").** The owner compared
  it by ear with a corrected version at musically typical settings: the difference was almost
  inaudible, and where audible the corrected version "takes the edge off"; it also costs about
  40 % more CPU for voices using cross-mod.
- **Quality rule, refined:** the −100 dB alias gate applies to all waveforms and to hard sync.
  **Cross Mod in Classic mode is exempt** and documented: about −36 to −80 dB at one octave with a
  saw, square or pulse modulator, worse at higher depths and modulator pitches; sine and triangle
  modulators measure about −87 dB at one octave.
- **A "Clean" option will be added later** (low priority, after Wave 3): a switch, default Classic,
  that corrects VCO-1 at the modulator's edges and tracks the pitch change inside each sample
  (study: −104 dB or better for VCO-2 up to 1 kHz and up to one octave; no cost when off).
- **Two defects found by the study are fixed now, independent of that choice:** hard sync with a
  sine VCO-1 aliases at −77 dB without any cross-mod (a missing slope correction; the sync gate only
  tested a saw); and the sync + cross-mod sound differs between real-time and offline rendering by
  up to 29 dB on single partials, so a bounce does not sound like playback.
- Study findings worth keeping: pure FM bandwidth is negligible at musical settings (sine into sine
  at one octave: about −225 dB); the aliasing comes from the modulator's edges passing through the
  exponential pitch mapping and from assuming a constant pitch within a sample; extra oversampling
  is a poor fix (twice the CPU for −53 to −68 dB); low-passing the modulator dulls the sound.

## Linux ARM64 and container checks (owner decision, 2026-11-12)

- **Linux ARM64 (aarch64) is a supported target,** built and validated in CI only; the owner has no
  such machine yet, so there is no manual DAW check. It is not performance-tuned for small boards.
  x86_64 and ARM64 are merged into one Linux bundle, like the Windows one.
- **Local Linux pre-flight runs in a native ARM64 container on the Mac** (`scripts/linux-check.sh`).
  x86_64 Linux is not emulated locally: it would be slow and unreliable under emulation, and CI
  builds and tests the real binary on real hardware.
- **CI runs the Linux bundle in containers of several distributions** to prove it loads above the
  glibc floor, not only on the image it was built on.
- **Headless DAW render: studied, not built.** An open-source Python host library loaded the
  plugin headless in a container and rendered a MIDI note deterministically, but it uses the same
  JUCE hosting layer as `host_smoke`, so it adds little host diversity and no project save/reload.
  A real open-source DAW would need a spike to prove headless VST3 rendering. The proprietary DAW
  the owner uses has an evaluation licence that does not mention automated use; nothing proprietary
  was downloaded or run. **Open for the owner:** add the Python host render to CI, spike the
  open-source DAW, or leave it.
- The draft-PR tier builds Windows x64 and Linux x86_64; Linux ARM64 is in the full tier only.

## Wave 2 results: M3 and M4 (2026-11-12)

All (review) unless marked.

| Decision | Why |
|---|---|
| **Pan and balance never boost:** unity at centre, the near side stays at unity, the far side falls to 0. | The first version boosted the remaining side by 3 dB per stage (+6 dB with both at an extreme). |
| **Mixer gains latch across a key-mode change** until the part starts a new note or is silent. | Dual → Whole made Lower's release tails audible that the balance had silenced. |
| **Key-mode change releases all voices of both parts** (even with the pedal down; pedal, bend and wheel are kept). A note-off for a key nobody owns is dropped; a key re-pressed after a split move can be owned by both parts and its note-off goes to both. | No hung or orphaned notes; verified by a 300,000-sequence fuzz against the real allocators. |
| **In Whole mode the Upper part is not rendered** (it still receives controllers); when it comes back its parameters jump to target and its LFO restarts. | No CPU for a silent part; the result does not depend on block size. |
| **Key mode and split point changes take effect at the start of the host block.** | Hosts deliver parameters per block; quantising further was not worth it. |
| **Oversampling: 4× below 44.1 kHz, 2× up to 88.2 kHz, 1× above (offline 4×/4×/2×).** Reported latency is the measured decimator delay plus BLEP: 6, 5 and 8 samples. | Internal rate ≥ 88.2 kHz everywhere; 32 voices at 192 kHz cost about 10 % of a core (was 19 %). The old latency figure included an upsampler the voice path does not use. |
| **Slow per-sample state is double precision** (envelope level, LFO phase, timers) and coefficients use `expm1`. | At 384 kHz internal a 10 s attack was 7.7 % off and decays stalled 0.4 dB above sustain. |
| **Behaviour suites run at 48 kHz (2×), 96 kHz (1×) and 192 kHz (1×).** Parameter, state and gain-law tests run at 48 kHz only. | Contract gate 4.4. Running them at high rates exposed a smoother rounding problem (fixed). |
| **Golden references can be stereo** (format version 2: one body per channel). | Upper's pan was barely covered by left-channel-only references. |
| **OPEN — hard sync combined with audio-rate cross-mod aliases at −44 to −71 dB**, at every oversampling factor; it predates M3 (the alias gate only measured plain waveforms and sync alone, which pass at ≤ −100 dB). The check stays at −100 dB and is tagged as an expected failure, so it is reported on every run. | Frequency modulation creates bandwidth that BLEP correction cannot remove. Needs a study: band-limit the modulator, raise oversampling while cross-mod is active, or define a separate, documented budget for cross-mod. **Owner decision needed after the study**, because the project rule says the gate must not be relaxed. |
| **The sub-block-equivalence test uses a smooth patch at −90 dB** and bounds the chaotic sync + cross-mod patch at −55 dB. | That patch amplifies 1e-7 differences; a real control-rate step still fails both bounds. |
| **After v1.0, parameter *IDs* are frozen, not list positions.** JUCE derives each VST3 parameter ID from a hash of the string ID; hosts key automation on it and our state is keyed by string ID. Order only affects generic parameter lists. A test checks the IDs are distinct. | Lets per-part parameters be added after release without breaking sessions. The "never reorder" wording in `CLAUDE.md` is to be refined before v1.0 (owner). |
| **Arp/sequencer: consumes the sustain pedal while on** (decision); legato overlap at 100 % gate when the pitch changes; host-position drift up to about one step is absorbed, larger is a jump; swing and rate changes keep the step count; a pattern change continues from the last played note; with the host playing, the first key waits for the next grid step; the sequencer starts on the grid step after the key press and takes its root from the lowest key in the pattern. | Review findings and choices the contract left open. Passing the pedal on made arpeggiated notes pile up in the synth. |
| **An intermittent crash of pluginval at process exit was seen once on Linux CI** (after SUCCESS; a re-run and later runs were clean). No cause found in our code. | Watch for it; capture a backtrace if it recurs. No CI retries added. |

## Product names (owner decision, 2026-11-12)

- **Every product name carries the company prefix: "Exitium Vintage Synth" and "Exitium B-Side".**
  This is the name shown in hosts, the bundle file name, the README and any site. Plugin codes and
  bundle identifiers do not change.
- The names are considered good enough for an open-source project and **can still be changed
  before the first release** if a conflict turns up. An official trademark search is optional, not
  a gate. After the first release the host-visible name stays.
- The rename of Vintage Synth is done at the start of M3 (it touches CMake, CI, tests and scripts).

## Wave 2 contracts (2026-11-12)

All (review).

| Decision | Why |
|---|---|
| Key modes: Whole = Lower only; Dual = both; Split = below the split point to Lower. The router remembers which part received each key's note-on. | A moved split point can never hang a note. |
| Changing the key mode releases all voices and forgets held keys. | Same rule as the assign-mode change in M2. |
| Balance is constant-power with both parts at unity in the centre; pan per part. Mono output is (L + R) / 2. | Predictable levels; Dual at centre is simply the sum of both parts. |
| Oversampling follows the host rate: 2× below 88.2 kHz, 1× at or above (offline 4× / 2×). | Keeps the internal rate at 88.2 kHz or more everywhere and brings 192 kHz sessions back inside the CPU budget (was 18 % for 32 voices). |
| MIDI stays omni; no per-part channels yet. | Not needed for Whole/Dual/Split; can be appended later. |
| Arpeggiator and sequencer follow the host transport when it plays and an internal clock when it is stopped. | Works in a DAW and when just playing the plugin in a host. |
| Arp modes: Up, Down, Up & Down, Random, As Played; 1–4 octaves; hold; swing; gate length. Sequencer: 16 steps with offset, Note/Rest/Tie and velocity; transposed by the lowest held key. | The classic set plus the two things players expect today (as-played order, swing). |

## M7 UI direction (owner decision, 2026-11-12)

- **The custom UI does not reproduce the reference panel's layout.** It gets its own arrangement,
  designed for plugin use (screen proportions, mouse and trackpad, resizing, clear signal flow,
  controls that are inactive in the current mode shown as inactive), with our own look and naming.
  The reference panel only defined *which functions exist*.
- Consequence for M7: the first deliverable is a layout proposal for the owner to choose from,
  before any UI code. The visual style is still to be decided with the owner.
- Noted from hands-on testing: Unison Detune and Unison Voices must look inactive outside Unison mode.

## Second instrument: chip synth (owner decision, 2026-11-12)

| Decision | Why |
|---|---|
| **A second instrument is planned:** a polyphonic synth modelled on a classic 3-voice sound chip from early-1980s home computers. (owner) | Owner's request. |
| **Separate plugin in this repository, sharing a common library** extracted after M3. | A host identifies a plugin by its codes and parameter list; the instruments share neither parameters nor presets, but they do share the allocator, events, parameter plumbing, effects, tests and CI. |
| **Own behavioural model, not an embedded emulator.** Digital parts bit-exact, analog parts modelled, one whole chip voice per note. | Research: the established emulators simulate every chip clock cycle, so 16 notes need 16 chips at an estimated 17–26 % of a core or more (estimate from a stand-in benchmark, not the real engine); they allocate and start threads at setup, quantise pitch and cutoff, and their sources contain third-party brand names. The own model is estimated at 2.5–5 % for 16 voices. Their licence (GPL v2 or later) would have been compatible. |
| **Every patch has a program sequencer** (wave/note, pulse and filter lanes on a tick clock), hard restart is a patch feature with explicit policies, and tables are patch data with a text format. (owner prompt, research) | The owner pointed out that the signature sounds come from how the chip was driven, not from the chip alone. Research confirmed it: every serious playable instrument of this kind added a per-patch table sequencer, and the classic drums, arpeggio chords and attack snap cannot be made without one. It must be in the first sounding milestone so the engine is not designed into a corner. |
| **Program-driven values step at the tick, unsmoothed; automation stays smooth.** | The stepped vibrato, per-tick cutoff steps and restart click are part of the character; smoothing them would also break the envelope-quirk timing. |
| **Filter per note by default, with a shared-filter topology per part.** | Per-note filters make a playable poly; the shared filter is needed for paraphonic patches, the early revision's summed overdrive, and replaying register traces against the oracle. |
| **Chip arpeggio (in-voice, tick rate, gate held) is separate from the shared arpeggiator.** | They are different things musically and technically. |
| **An established emulator is the test oracle, in CI only.** Never vendored, linked or shipped; reference data is committed under neutral names. (review) | Authenticity must be measurable; the brand rule and the real-time rules stay intact. |
| **Own name; no official names, part numbers or nicknames.** Revisions are "Model A" and "Model B". (owner) | Project brand rule; the original computer brand is in active commercial use again. |
| **Product name: "B-Side"** (owner). Always shown with the company name ("Exitium B-Side") in the plugin name, metadata, README and site. "Broadside", "Topside", "Flipside" and "Vicious" are reserved for modes and preset banks. | A music term that also nods at the chip. A web check found no synth, plugin or hardware with this name, but the term is common and crowded in search; US trademark filings for "B SIDE" exist in software and DJ-equipment classes for unrelated goods. **Open: the owner runs the official trademark searches (USPTO, EUIPO/TMview, WIPO, UKIPO; classes 9, 15, 41, 42) before release.** "Vicious" as a bank name could be read as a nod to an existing preset maker with a longer name (low to moderate risk). |
| **Listening reference: public recordings of the original hardware, by the owner's ear.** They are not downloaded into the repository or used as test data. | Recordings are lossy, of unknown chip revision, and copyrighted; the oracle covers objective tests. |

## Wave 1 — M2 polyphony, effect blocks, owner decisions (2026-11-12)

### Public-release audit (2026-11-12)
- Full-history scan found no secrets, keys, home-directory paths or third-party brand names. Three
  comments that pointed at specific hardware (a filter chip type, "panel photo", "the original")
  were reworded.
- pluginval downloads in CI get a SHA-256 check; CI artifacts are kept 7 days.
- Added `SECURITY.md`, `CONTRIBUTING.md` (AGPLv3-only contributions) and a PR template.
- Statements about the repository being private and CI cost are updated in the follow-up change
  right after the visibility flip, together with removing the draft-PR cost tier.

### Owner decisions and approvals
- **M2 audition approved by ear** (polyphonic demo: pads, steals, unison lead, pedal, velocity, solo bass).
- **Effects, first listen on the standalone blocks:** chorus approved at the stronger maximum;
  delay approved with a 700 ms default time and about 0.4 as a typical level; reverb liked. The
  settings of all three get their final evaluation in the plugin (M5 audition).
- **The repository goes public after Wave 1 is merged** (free CI runners, code scanning). Before the
  flip: history scan for secrets and brand names, `SECURITY.md`, `CONTRIBUTING.md`, branch protection.
- **Contributions are accepted under AGPLv3 only**, no contributor agreement.
- **Commit author e-mail stays as it is** (no history rewrite).
- **Linux x86_64 VST3 becomes a supported target**, built and validated in CI with a sanitizer job;
  the owner can test manually in a DAW on Linux. Work starts after Wave 1 merges.
- **Plugin host for hands-on testing: JUCE's AudioPluginHost**, built from the JUCE submodule
  (`scripts/dev-host.sh`). No new dependency.

### M2 behaviour (review unless marked)
| Decision | Why |
|---|---|
| A voice that starts from idle gets fresh oscillator start phases and noise seeds from its own deterministic generator, and its filters and ENV-1 are reset. A re-played Solo note no longer continues the previous phase. | A voice that stops inside a sub-block keeps evolving to the sub-block's end, so anything inherited by the next note made renders depend on the host block size (up to −5.7 dB in Solo). Free-running oscillators behave this way on the analog original. |
| Unison voices starting from idle share one base phase per note plus a fixed offset of k²/(2n) cycles for voice k. | With independent random phases, unison at detune 0 had a random level per note (−4.8 to −26.7 dB with 16 voices). This pattern sums to exactly √n, so the level equals one voice at any detune. At detune 0, even harmonics get a fixed colouring. |
| Unison retriggers and returns legato exactly like Solo. | The contract only defined it for Solo; one rule for both mono modes. |
| "A key held" means a physically held key for Legato portamento. For the LFO-delay restart, notes held by the sustain pedal also count. | Legato is a playing technique; but delayed vibrato must not drop out on ringing pedal notes when the next note is played. |
| In Poly, every new note glides from the previously played note. | As specified; chords glide internally when portamento is on, as on the classic. |
| A stolen note keeps its gate length: start and release are both delayed by the 3 ms fade. A steal of a voice whose envelope has already ended starts at once. | A stolen note shorter than 3 ms was silently dropped. |
| Stealing: "oldest in release" is the voice that entered release first; pedal-held voices count as held. Re-pressing a key retriggers its releasing or pedal-held voice. | Simple, deterministic rules. |
| A mode or unison-count change releases all voices and forgets held keys (they must be re-pressed). | Avoids stuck or orphaned notes across a mode change. |
| Idle voices are reported in the order they finished (sample-exact). | "Idle longest" must not depend on block splitting; it changed which voice a note landed on. |
| Envelope key follow follows a legato note change at the event. | Matches a keyboard CV; the times used to change one host block late. |
| Mod wheel: wheel × depth is added to the VCO LFO mod slider before the taper, capped at the slider maximum; it is subject to the LFO delay. | Same control path as the slider. |
| Portamento time applies per block; unison detune, velocity amounts, wheel depth and the wheel are smoothed. Retriggering a sounding voice smooths velocity and unison level over 5 ms. | No zipper noise or gain steps. |
| Pitch glide and drift are computed from absolute time in double precision, rounded once. | Per-sub-block float rounding integrates into phase (−74 dB between block sizes for glide). Now −118 dB or better with 16 voices at full Vintage. |
| Bend range labels are "0 st" … "12 st"; the velocity parameter names contain an arrow (UTF-8, guarded by a test). | Readability; "Velocity to VCA" is the fallback if a host shows it badly. |
| **CPU at high session rates (open):** cost scales with the sample rate. 32 voices at 2× cost 5.0 % of a core at 48 kHz but 18.2 % at 192 kHz, above the 15 % budget, which is defined at 48 kHz. Planned for M3: 1× oversampling when the host rate is 88.2 kHz or higher. | The internal rate is then at least 88.2 kHz at every host rate, which is what the alias gate needs. |

### Effect blocks (review unless marked)
| Decision | Why |
|---|---|
| Chorus mix curve: wet gain = mix^1.152 (0.45 at the default mix 0.5, 1.0 at full mix). The part of the wet signal that differs between left and right is capped at 0.7, and above the default the output is level-compensated. (strength confirmed by owner) | Full mix would otherwise cancel completely in mono; the worst mono dip at full mix is −12.5 dB, and −5.1 dB at the default. |
| A settled delay time is rounded to a whole sample (echo spacing within ±0.5 sample). | At fractional delays the interpolator darkened every repeat (0.5 dB per repeat at 10 kHz), depending on time × sample rate. |
| Delay time changes glide like tape (pitch bends during the change). | Click-free and musical. |
| Reverb high-frequency damping is limited at short decays (ratio not below about 0.37 at 1 s). | Heavier damping left a sparse, ringing tail above 15 kHz. |
| Reverb gets louder with longer decay (wet −13 dB at 0.5 s, +5 dB at 20 s at level 1). | Natural behaviour of the network; to be judged at the M5 audition. |
| Delay default time 700 ms, default level 0 (off). (confirmed by owner) | Owner's choice after listening. |

## Wave 1 — M2a, test infrastructure, CI cost (2026-11-12)

| Decision | Why |
|---|---|
| **Fast `exp2` / `tan` / `tanh` approximations in the voice path**, with tested bounds (relative error ≤ 2e-7 / 5e-7, absolute ≤ 2e-7; static pitch error ≤ 0.009 cent). They return NaN for NaN. | Profiling showed the voice is limited by its serial dependency chain per sample, where libm calls cost far more than in isolation. NaN must still reach the output guard. |
| **Voices render sample by sample across all voices** (interleaved, groups of 32) after a vectorised per-voice block-prepare phase; shared modulation is computed once per part in sub-blocks of up to 32 oversampled samples. | Interleaving lets the CPU overlap the voices' serial chains: 32 voices went from 11.3 % to 4.7 % of one core. Fully voice-by-voice rendering measured several times slower. |
| **Parameter smoothing stays per sample (closed form), not control rate.** | Linear control-rate ramps measured −55 dB of error on fast cutoff moves. |
| **Smoothers and the drift walk are kept as target + deviation.** | A plain one-pole stalls short of its target by an amount that depends on the step size, which made renders depend on block splitting and left tiny permanent pitch/cutoff offsets. |
| **Renders are independent of host block size and event positions at any Vintage amount** (tested: −110 dB between block sizes 512 and 500 at Vintage 1). | Offline bounces and golden renders must be reproducible. |
| **All Notes Off (CC 123 and the overflow marker) also clears the sustain pedal;** Reset Controllers clears bend, mod wheel and pedal. | A held pedal must never be able to keep notes hanging after a panic or an event overflow. |
| **Every MIDI event is applied exactly once:** before the block → offset 0, at or after its end → after the last sample, zero-length blocks still apply events. Malformed channel messages are dropped. | A note-off at the block boundary was being lost (possible hung note); masking bad data bytes could turn garbage into All Sound Off. |
| **Golden renders compare third-octave band energy per 25 ms frame, level envelope and tail** (3 dB per cell with ≤ 1 % outliers, 0.4 dB mean, 2 dB level, −80 dB gate). References store features, not audio. | Verified in CI: a reference generated on arm64 matches on x64 and with MSVC. Detects a 3.5 % cutoff change, a missing note or a 25 ms timing change. |
| **VST3 validator: built in CI only** from Steinberg's SDK at `v3.8.1_build_84` (pinned by commit SHA, MIT), cached, run as ctest `vst3_validator` on macOS and Windows x64. The test requires the validator's own "N tests passed, 0 tests failed" line. (owner decision) | JUCE does not bundle the validator; nothing is added to the repo or linked into the plugin. Exit code alone passed in 0.2 s and proved nothing. |
| **CI cost control while the repo is private:** docs-only changes don't build; draft PRs build Windows x64 only, the full matrix runs when a PR is marked ready, on `main` and on manual dispatch; 30-minute job timeout; Dependabot monthly. (review) | macOS minutes are billed at a multiple; MSVC is the one thing that cannot be checked locally. To be relaxed when the repo goes public (free standard runners). |
| **Push discipline:** one push per branch, after local tests, a local AddressSanitizer/UBSan run for engine changes, and the review fixes. Docs are batched with code. | 13 CI runs on the first orchestrated day, several of them avoidable. |
| **Effects (owner feedback and review):** chorus full mix reaches wet ≈ dry, with the default at the earlier maximum; no bulk clearing on the audio thread; reported tail capped at 30 s; level 0 fades out in 20 ms and drops the tail (review). | See `docs/architecture.md` section 5. |

## Wave 0 — contracts for M2–M5 (2026-11-12)

The interfaces are in `docs/architecture.md`. These are the judgement calls behind them.
All are marked **(review)**: the owner has not confirmed them yet.

| Decision | Why |
|---|---|
| **Stolen voices fade out over 3 ms, then start the new note** (the note starts 3 ms late). No spare voices for the fade. (review) | Click-free with a fixed 16 voices and a predictable CPU cost. 3 ms is inside the 2–5 ms spec and below audible timing error. |
| **Steal order:** oldest voice in release, else oldest held voice, never the newest. (review) | Releasing voices are the least audible to lose; protecting the newest note keeps the melody intact. |
| **Unison:** 2/4/8/16 voices (default 4), total detune spread 0–50 ct, output scaled by 1/√n. (review) | A selectable count keeps CPU in the user's hands; 1/√n keeps loudness steady when the count changes. |
| **Portamento is an RC glide in pitch**, time = 99 % of the interval, modes Always/Legato. (review) | Matches the analog behaviour and the envelope time definition already used. |
| **Velocity:** two per-part amounts, to VCA gain (default 0.5) and to VCF envelope depth (default 0). (review) | The classic had no velocity; a modest VCA default suits modern keyboards, and amount 0 restores the original behaviour. |
| **Sustain pedal (CC 64) and mod wheel vibrato (CC 1) are part of M2.** (review) | A polysynth without a sustain pedal is not playable; the wheel maps to the classic bender-section LFO vibrato. |
| **Bend range per part, 0–12 semitones, default 2.** | Replaces the fixed ±2 from M1, as planned. |
| **Parts are mono up to the mixer; stereo comes from pan and effects.** No per-voice unison stereo spread. (review) | One decimator per part instead of two: the decimator is a fixed per-part cost in a tight CPU budget. |
| **Effects are global, in series on the stereo bus:** chorus → delay → reverb. Per-part sends are deferred. (review) | Simplest structure that gives the classic sound; per-part sends can be appended later without breaking IDs. |
| **Whole mode plays the Lower part only** (16 voices). (review) | Simple and predictable; a 32-voice single patch is not needed for the CPU target. |
| **MIDI is parsed once into plain `NoteEvent`s**; parts and the arpeggiator work on those. Fixed buffer; overflow drops the rest of the block and sends All Notes Off. | Lets the arp/sequencer be a pure, testable event transformer, and keeps `juce::MidiMessage` off the audio thread. Overflow can never hang a note. |
| **CPU optimisation is part of M2**, target ≤ 0.30 % of a core per voice. | The extended benchmark measures 0.35 % per voice, so 32 voices ≈ 11.5 % of the 15 % budget. M2 adds per-voice work and the effects need about 1.5 %, so some margin has to be won back. (The earlier 0.47 % figure was a single unrepeated run and was not reproduced.) |
| **Golden renders compare perceptual features, not samples.** | arm64/x64 and clang/MSVC math libraries differ in the last bits; a self-oscillating filter amplifies that. |

### Process (owner decisions, 2026-11-12)
- **Orchestrated development:** work is done by agents briefed against `docs/architecture.md`; the
  orchestrator plans, verifies and merges (skill `orchestrate`).
- **Git:** agents commit on feature branches, pushed as PRs so CI validates Windows. Merging to
  `main` needs the owner's approval per milestone.
- **Audition gates:** the owner listens at the end of M2, M5 and M6, and sets the UI direction before M7.

## CI and infrastructure (2026-11-12)

| Decision | Why |
|---|---|
| Actions pinned by full commit SHA (version in a comment); Dependabot updates them weekly, grouped. | Tags are mutable; SHA pins are the supply-chain best practice. Dependabot keeps pins current. |
| Runner images pinned (`macos-26`, `windows-2025`, `windows-11-arm`, `ubuntu-26.04`), not `-latest`. | Reproducible builds; image upgrades become deliberate changes. |
| Windows CI: Ninja in the VS developer environment of the newest installed VS (`vswhere`). | Runner images change VS versions; Ninja is needed for the compiler cache. |
| Compiler cache: ccache on macOS, sccache on Windows (`hendrikmuhs/ccache-action`). | ccache handles the universal `-arch` pair; sccache is the mature MSVC cache. |
| LTO only on the shipped plugin; tests/benchmark use a non-LTO build of the same sources (`vs_test_core`). | Saves ~5 min of link-time code generation per Windows job. The shipped binary is still validated by host_smoke and pluginval. |
| Trivy fails the build only on fixable HIGH/CRITICAL findings; everything else is reported in the SARIF artifact. | Actionable gate without noise. Code scanning upload is off (private repo, no Advanced Security); `vars.CODE_SCANNING=true` turns it on. |
| Dependabot submodule PRs are merged only when they point at a release tag. | JUCE `develop`/`master` commits are not releases; upgrades are deliberate. |

## M1 — single-part voice engine (2026-11-12)

### Panel interpretation
| Panel element | Decision | Why |
|---|---|---|
| Sync switch | **VCO-1 is the slave and is hard-synced to VCO-2.** | Matches the classic architecture, where VCO-2 is the modulator for both Cross Mod and Sync, and VCO-2's Lo Freq mode makes it a usable sync/FM master. The parameter ID stays `vco2.sync` because the switch sits in the VCO-2 section. |
| Cross Mod | VCO-2 → VCO-1 **exponential** audio-rate FM, max ±4 oct (squared taper). | Classic analog cross-mod behaviour; the squared taper gives fine control at low settings. |
| VCO-2 Lo Freq | VCO-2 stops tracking the keyboard and pitch bend and runs at 1–8 Hz (16'..2'), adjusted by Fine Tune. VCO pitch mods still apply. | Turns VCO-2 into a second, sync-capable LFO. |
| Waveforms | Both VCOs offer Sine, Triangle, Saw, Square, Pulse, Noise. | The reference layout is ambiguous per VCO, so a superset costs nothing. (confirmed by owner 2026-11-12) |
| VCO ranges | VCO-1: 32'..1' (6 steps). VCO-2: 16'..2' (4 steps). 8' = keyboard pitch. | As printed on the panel. |
| VCA 0-1-2-3 switch | **VCA LFO Mod depth** (tremolo dips of 0 / 25 / 50 / 100 %). | Its position under "LFO MOD" in the VCA section matches the usual 4-step VCA LFO switch of this kind of panel. (confirmed by owner 2026-11-12) |
| ENV-1 shape switch | **ENV-1 polarity** (Normal / Inverted). Inverted gives -ENV-1 for pitch and VCF; for PWM it gives 1 - ENV-1 (PWM is unipolar). | The shape icon under ENV-1 marks the classic polarity switch. (confirmed by owner 2026-11-12) |
| ENV key follow (KYBD) | Per envelope on/off. When on, all times halve every 2 octaves above C4. | Common analog key-follow behaviour. |
| "1 / ALL / 2" ENV selector | UI edit target only (which envelope the faders edit). **Not a host parameter.** | Same reasoning as Panel Mode Lower/Upper. |
| VCF -12 dB | Output tapped after stage 2; **feedback still from stage 4.** | Resonance and self-oscillation behave the same in both slopes, and the rolloff is 12 dB/oct. A 2-pole loop can't self-oscillate. |
| HPF | 1-pole, non-resonant, 5 Hz (DC blocker) … 2.56 kHz. | The panel shows a single slider. |
| LFO waves | Sine, Triangle, Saw (falling), Square, Random (S&H), Noise (smoothed random at 8× rate). | Matches the six panel symbols. |
| LFO delay | Zero output for the first half of the delay time, then a linear fade-in over the second half. Re-triggered when a note is played with no other keys held. | Classic delayed-vibrato feel. |

### Ranges and tapers (`source/common/params/Mappings.h`)
These are the single source of truth: the parameter display text and the DSP both call these functions.

- VCF cutoff 20 Hz – 20.48 kHz, exponential (10 octaves). Env mod up to 7 oct. LFO mod up to ±4 oct (squared taper). Key follow 0–100 %.
- Resonance k = 4.15 × slider. It self-oscillates from about 96 %.
- Envelope times 1 ms – 10 s, exponential. Attack time = time to reach the peak (RC charging toward 1.3).
  Decay and release time = time to cover 99 % of the way to the target.
- LFO rate 0.05 – 40 Hz. LFO delay 0 – 5 s (squared taper).
- VCO pitch mod: LFO up to ±2 oct and ENV up to +4 oct, both squared taper. Fine tune ±1 semitone.
- VCA level is squared (perceptually even).
- Pitch bend is fixed at ±2 semitones for now. It becomes a setting in M2/M3.

### DSP architecture
- **Oscillator anti-aliasing: windowed-sinc BLEP/BLAMP table** (16 taps, Kaiser β = 8, cutoff 0.45 × oversampled rate)
  instead of 2-point PolyBLEP. PolyBLEP measured only −58 to −65 dB alias at 1–2 kHz fundamentals at 2×.
  The table version measures **≤ −113 dB for every wave up to 5 kHz**, and the test gate is set at −100 dB.
  The cost is 16 multiply-adds per waveform edge plus 8 oversampled samples of delay (4 base samples).
- **Oversampling: 2× real-time, 4× offline** (`isNonRealtime()` at prepare time). It uses JUCE's polyphase IIR
  decimator with a custom stage (transition 0.05, −100 dB). Voices render directly at the oversampled
  rate; the "upsampling" pass is fed silence, because JUCE's API needs it before decimating.
- **Latency is reported** to the host (decimator + BLEP delay, about 7 samples at 2×).
- **VCF: ZDF 4-pole OTA cascade.** The feedback is solved linearly, then passed through tanh at the cascade input.
  There's no iterative solve. Tests confirm exact cutoff (−12.04 dB / −6.02 dB at fc in the linear region),
  correct slopes, and self-oscillation at fc ±3 %.
- **Parameter handling: a single X-macro list** (`PartParamList.h`) generates the snapshot struct, the
  APVTS layout for both parts, the atomic binding and the per-sample smoothing (5 ms one-pole).
  Switches are applied per block without smoothing.
- **Both parts' parameters exist now** (`lower.*` and `upper.*`); only Lower is played until M3.
  Parameters can't be added later without appending, so defining the full set now keeps the order logical.
- **Vintage** (global, default 0 = clean). Per voice it controls static VCO offsets (±6 ct), slow pitch drift (±3 ct),
  cutoff offset (±0.12 oct) and envelope time spread (±8 %). It's seeded per voice, so renders are deterministic.
- **Mono note handling in M1**: last-note priority. Envelopes retrigger on each new note; returning to a held
  note changes pitch without a retrigger. M2 replaces this with the polyphonic allocator.

### Process
- **State version stays 1 until v1.0.** There are no released states to migrate, so pre-release parameter
  changes don't bump `stateVersion`. The append-only and migration rules apply from the first release.
- Test-only renders go to `$VS_RENDER_DIR`, because `build/` is ignored by git.
