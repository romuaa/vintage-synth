---
name: realtime-review
description: Review checklist for code reachable from processBlock (voices, filters, FX, arp/sequencer, MIDI handling) — real-time safety, numerical robustness, portability. Use when writing or reviewing audio-thread code.
---

# Real-time review

Trace every call reachable from `processBlock`, including any third-party functions it calls. Flag each violation with `file:line`.

## Must not happen on the audio thread
- Heap allocation: `new`, `std::vector` growth, `std::string`, `std::function` captures,
  `juce::String`, building `juce::var`/`ValueTree` objects.
- Locks or waits: `std::mutex`, `CriticalSection`, `MessageManagerLock`, condition variables.
- Logging, `DBG` in release builds, file or network I/O, exceptions, `dynamic_cast`-heavy paths.
- Unbounded loops, or loops bounded by user data without a hard cap.

## Must happen
- `ScopedNoDenormals` at block start. Feedback paths (filters, reverb, delay) must be denormal-safe.
- Parameter reads through cached atomics. Smoothing per sample or per sub-block.
- Sample-accurate MIDI: the block is split at event offsets, never quantized to block start.
- Voice stealing has a short fade (about 2–5 ms), never a hard cut.
- `prepareToPlay` handles every sample rate (44.1–192 kHz) and every block size from 1 to max,
  including a block size that varies from call to call.
- Tempo-synced code (arp, sequencer, delay) tolerates missing playhead info, tempo changes,
  and loop jumps.
- NaN/Inf guard at the output stage in debug builds (assert), and a safe clamp in release builds.

## Portability
- SIMD code has SSE2 and NEON paths, or uses `juce::dsp::SIMDRegister` / xsimd.
- No reliance on `long` being 64-bit or on `char` signedness.

Report: a list of violations with fixes. If there are none, say "real-time review: clean" and
list the files checked.
