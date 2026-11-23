#!/usr/bin/env python3
"""Model A's cutoff register walked one code at a time across the cutoff converter's boundaries (docs/architecture.md
8.19.3, "Model A's cutoff curve at the converter's boundaries"; docs/decisions.md "B-Side Model A cutoff steps"): writes
the .bstrace files of the a_cutcode_* family into a folder. Their .bsref and .bsspec come from the reference adapter run
on each file (docs/bside-oracle-adapter.md); the MANIFEST lists them like every committed file.

One noise voice (oscillator 3, frequency 0xffff, sustain 4: a low level), routed to the low-pass, resonance 0, volume 15.
Every segment holds one cutoff code: one write group, then 16 measured frames after the 8 of settling (0.6 s), so each
segment's corner is measured like every other corner (AnalogMeasure.h). Only the codes that carry the gate of 8.19.3
(owner decision O9: the steps from 479 -> 480 up to 1600) are walked. The codes of one pass: at every multiple of 64
from 512 to 1600 the three codes b-2, b-1, b (one step inside the stretch below the boundary, then the boundary's
step); at every odd multiple of 32 from 480 to 1568 the two codes b-1, b (the boundary's step). Every boundary of the
converter's ladder where it falls back lies at a multiple of 32. The reference's corner at a code varies from segment
to segment (about 1 to 3 % between the codes 480 and 640), so the walk is measured more than once and the corner per
code is the mean over the passes: two passes over every code (up, then down), six more over the codes 479 .. 640
(each at another place in the file, so each pass sees other noise).

Every file has the same write cycles (60 segments), so the noise is the same at a segment index in every file. The one
bypass twin (the first file's writes, the voice not routed) therefore serves every file: with the voice not routed the
cutoff writes do not reach the output.

a_cutsweep_a: the owner's one-voice sweep (the listening case of the S3a study): one sawtooth (110 Hz, attack 0, decay 9,
sustain 15, release 8) through the low-pass at resonance 0, the cutoff written every 10 ms, 200 up to 1900 (0.5 s to
3 s, geometric) and back down to 200 (3 s to 5.5 s); the voice gated off at 6 s. Its steps are measured on the 48 kHz
renders (band levels one waveform period after each write against the period before, AnalogReferenceTests.cpp).

  python3 -I gen_cutcode_traces.py <folder>
"""
import os
import sys

CLOCK = 985248
SEG = 24 * 1200 * CLOCK // 48000  # 8 frames of settling + 16 measured frames
PER_FILE = 60


def pass_codes():
    codes = []
    for b in range(480, 1601, 32):
        codes += [b - 2, b - 1, b] if b % 64 == 0 else [b - 1, b]
    return codes


def dense_codes():
    codes = []
    for b in range(480, 641, 32):
        codes += [b - 2, b - 1, b] if b % 64 == 0 else [b - 1, b]
    return codes


def walks():
    """The sequence of codes over all files: pass up, pass down, then six passes over the dense codes, each rotated."""
    p = pass_codes()
    seq = p + p[::-1]
    d = dense_codes()
    for k in range(6):
        r = d[(5 * k + 3) % len(d):] + d[:(5 * k + 3) % len(d)]
        seq += r if k % 2 == 0 else r[::-1]
    while len(seq) % PER_FILE != 0:
        seq.append(2047)  # filler: measured, not used
    return seq


def text(comment, codes, bypass):
    w = [(8, 14, 0xff), (8, 15, 0xff), (8, 19, 0x00), (8, 20, 0x40), (8, 18, 0x81), (8, 0x17, 0x00 if bypass else 0x04),
         (8, 0x18, 0x1f)]
    c = 24642
    for fc in codes:
        w += [(c, 0x15, fc & 7), (c, 0x16, fc >> 3)]
        c += SEG
    lines = ['bside-trace 1'] + ['# ' + x for x in comment] + ['clock %d' % CLOCK, 'model A']
    lines += ['%d %02x %02x' % x for x in w]
    lines.append('end %d' % c)
    return '\n'.join(lines) + '\n'


def sweep_trace():
    def cut(c, fc):
        return [(c, 0x15, fc & 7), (c, 0x16, fc >> 3)]

    def ramp(c0, c1, lo, hi, step=0.01):
        n = int((c1 - c0) / (step * CLOCK))
        w = []
        for i in range(n + 1):
            w += cut(c0 + int(i * step * CLOCK), int(round(lo * (hi / lo) ** (i / n))))
        return w

    f = int(round(110.0 * 2 ** 24 / CLOCK))
    w = [(8, 0x18, 0x1f), (8, 0x17, 0x01)] + cut(8, 200)
    w += [(1000, 0, f & 0xff), (1000, 1, f >> 8), (1000, 2, 0x00), (1000, 3, 0x08), (1000, 5, 0x09), (1000, 6, 0xf8), (1000, 4, 0x21)]
    w += ramp(CLOCK // 2, 3 * CLOCK, 200, 1900) + ramp(3 * CLOCK, 5 * CLOCK + CLOCK // 2, 1900, 200) + [(6 * CLOCK, 4, 0x20)]
    w = sorted(w, key=lambda x: x[0])
    lines = ['bside-trace 1', '# Model A one-voice sweep (gen_cutcode_traces.py): a 110 Hz sawtooth through the low-pass, resonance 0,',
             '# cutoff 200 up to 1900 and back, a write every 10 ms.', 'clock %d' % CLOCK, 'model A']
    lines += ['%d %02x %02x' % x for x in w]
    lines.append('end %d' % (7 * CLOCK))
    return '\n'.join(lines) + '\n'


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    seq = walks()
    files = {}
    for i in range(len(seq) // PER_FILE):
        codes = seq[i * PER_FILE:(i + 1) * PER_FILE]
        files['a_cutcode_%d_a' % i] = text(['Model A cutoff codes across the converter boundaries (gen_cutcode_traces.py), file %d: noise' % i,
                                            '(oscillator 3, sustain 4) through the low-pass, resonance 0; segments s00..s59 hold the codes',
                                            ' '.join(str(x) for x in codes[:30]), ' '.join(str(x) for x in codes[30:])], codes, False)
    files['a_cutsweep_a'] = sweep_trace()
    first = seq[:PER_FILE]
    files['a_cutcode_bypass_a'] = text(['Bypass twin of every a_cutcode_*_a file (the writes of file 0, the voice not routed).'], first, True)
    for name, t in files.items():
        with open(os.path.join(out, name + '.bstrace'), 'w') as f:
            f.write(t)
    print('%d traces written to %s (%d codes)' % (len(files), out, len(seq)))


if __name__ == '__main__':
    main()
