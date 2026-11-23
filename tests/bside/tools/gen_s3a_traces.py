#!/usr/bin/env python3
"""The S3a gate traces (docs/architecture.md 8.19.1; docs/decisions.md "B-Side S3a" round 2): writes the
.bstrace files of the a_s3a_* family into a folder. Their .bsref and .bsspec come from the reference adapter
run on each file (docs/bside-oracle-adapter.md); the MANIFEST lists them like every committed file.

These traces are gates only: no constant of the chip's filter or output stage was fitted on them (the fitting
traces are separate and are not committed). Every segment is laid out for SpecAnalysis.h's plan: one group of
writes, then 48 frames of measurement after the 8 of settling (about 1.43 s).

  python3 -I gen_s3a_traces.py <folder>
"""
import os
import sys

CLOCK = 985248
SEG = 1428620 - 24642  # 8 frames of settling + 48 measured frames, as a_filter_*


def text(model, comment, writes, end):
    writes = sorted(writes, key=lambda w: w[0])  # stable: equal cycles keep their order
    lines = ['bside-trace 1'] + ['# ' + c for c in comment] + ['clock %d' % CLOCK, 'model %s' % model.upper()]
    lines += ['%d %02x %02x' % w for w in writes]
    lines.append('end %d' % end)
    return '\n'.join(lines) + '\n'


def noise_grid(model, bypass, settings, nvoices):
    """Noise on oscillator 3 (and 2, 1 for several voices, at other frequencies) through the low-pass; each
    segment sets cutoff, resonance and the sustain (retriggered: raising the sustain does not raise a counter)."""
    voices = [14, 7, 0][:nvoices]
    freqs = [0xffff, 0xd3a1, 0xb1c3]
    w = []
    for v, f in zip(voices, freqs):
        w += [(8, v, f & 0xff), (8, v + 1, f >> 8), (8, v + 5, 0x00), (8, v + 6, 0x80), (8, v + 4, 0x81)]
    routing = sum(1 << (v // 7) for v in voices)
    c = 24642
    for fc, res, sus in settings:
        w += [(c, 0x15, fc & 7), (c, 0x16, fc >> 3), (c, 0x17, (res << 4) | (0 if bypass else routing)), (c, 0x18, 0x1f)]
        for v in voices:
            w += [(c, v + 6, sus << 4), (c, v + 4, 0x80), (c + 2, v + 4, 0x81)]
        c += SEG
    return w, c


def modes(model, bypass, settings):
    w = [(8, 14, 0xff), (8, 15, 0xff), (8, 19, 0x00), (8, 20, 0x80), (8, 18, 0x81)]
    c = 24642
    for fc, res, mode in settings:
        w += [(c, 0x15, fc & 7), (c, 0x16, fc >> 3), (c, 0x17, (res << 4) | (0 if bypass else 4)), (c, 0x18, mode | 0x0f)]
        c += SEG
    return w, c


def tones(model, cases):
    """Held triangles: (cutoff, mode, routed, [(hz, sustain), ...]); silent voices hold the test bit."""
    seg = 24 * 1200 * CLOCK // 48000  # 0.6 s: 8 settling + 16 measured frames
    w = [(8, 0x15, 7), (8, 0x16, 0xff), (8, 0x18, 0x1f)]
    for b in (0, 7, 14):
        w += [(8, b + 5, 0), (8, b + 6, 0xf0), (8, b + 4, 0x08)]
    c = 24642
    for fc, mode, routed, voices in cases:
        w += [(c, 0x15, fc & 7), (c, 0x16, fc >> 3), (c, 0x18, mode | 0x0f)]
        rmask = 0
        for i, b in enumerate((0, 7, 14)):
            w.append((c, b + 4, 0x08))
            if i < len(voices):
                hz, sus = voices[i]
                f = int(round(hz * 2 ** 24 / CLOCK))
                w += [(c, b, f & 0xff), (c, b + 1, f >> 8), (c, b + 6, sus << 4)]
                if routed:
                    rmask |= 1 << i
        w.append((c, 0x17, rmask))
        for i, b in enumerate((0, 7, 14)):
            if i < len(voices):
                w.append((c + 2, b + 4, 0x11))
        c += seg
    return w, c


def steps(model):
    """Steps (frame levels after each write; 1 s apart): volume writes with a held voice (direct and through the
    low-pass), routing writes with silent voices, resonance writes with a held DC voice routed."""
    seg = CLOCK
    f = int(round(110 * 2 ** 24 / CLOCK))
    w = [(8, 0x15, 7), (8, 0x16, 0xff), (8, 0x18, 0x00)]
    c = CLOCK // 2
    def at(writes):
        nonlocal c
        w.extend((c, r, v) for r, v in writes)
        c += seg
    at([(0x00, f & 0xff), (0x01, f >> 8), (0x05, 0x00), (0x06, 0xf0), (0x04, 0x11)])  # held triangle, volume 0
    at([(0x18, 0x0f)])
    at([(0x18, 0x00)])
    at([(0x18, 0x08)])
    at([(0x17, 0x01), (0x18, 0x10)])  # routed to the low-pass
    at([(0x18, 0x1f)])
    at([(0x18, 0x10)])
    at([(0x04, 0x10), (0x18, 0x1f)])  # voice off, volume 15, low-pass
    at([(0x17, 0x03)])
    at([(0x17, 0x07)])
    at([(0x17, 0x00)])
    at([(0x17, 0x04)])
    at([(0x17, 0x01), (0x04, 0x48), (0x06, 0xf0), (0x04, 0x49)])  # a held DC voice (test bit + pulse) routed
    at([(0x17, 0xf1)])
    at([(0x17, 0x01)])
    at([(0x17, 0x81)])
    at([(0x17, 0x01)])
    return w, c


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    files = {}
    res_grid = [(fc, res, 8) for fc in (300, 600, 1024, 1500, 1900) for res in (0, 4, 8, 12, 15)]
    level_grid = [(fc, res, sus) for sus in (3, 15) for fc in (300, 600, 1024) for res in (0, 8, 15)]
    voices_grid = [(fc, res, sus) for sus in (5, 11) for fc in (300, 600, 1024, 1500) for res in (0, 15)]
    cutoff_grid = [(fc, 0, 8) for fc in (380, 380, 380, 380, 300, 340, 410, 459, 470, 480, 490, 500, 700, 900, 1200)]
    for bypass in (False, True):
        sfx = '_bypass' if bypass else ''
        w, end = noise_grid('a', bypass, res_grid, 1)
        files['a_s3a_res%s_a' % sfx] = text('a', ['S3a gate: noise (oscillator 3, sustain 8) through the low-pass; cutoff 300, 600, 1024, 1500, 1900',
                                                  'x resonance 0, 4, 8, 12, 15 (segments s00..s24, cutoff outer). Bypass twin: not routed.'], w, end)
        w, end = noise_grid('a', bypass, level_grid, 1)
        files['a_s3a_level%s_a' % sfx] = text('a', ['S3a gate: the level dependence: noise at sustain 3 (s00..s08) and 15 (s09..s17),',
                                                    'cutoff 300, 600, 1024 x resonance 0, 8, 15.'], w, end)
        w, end = noise_grid('a', bypass, voices_grid, 3)
        files['a_s3a_voices%s_a' % sfx] = text('a', ['S3a gate: three noise voices (oscillators 3, 2, 1 at three frequencies) routed together,',
                                                     'sustain 5 (s00..s07) and 11 (s08..s15), cutoff 300, 600, 1024, 1500 x resonance 0, 15.'], w, end)
        w, end = noise_grid('a', bypass, cutoff_grid, 1)
        files['a_s3a_cutoff%s_a' % sfx] = text('a', ['S3a gate: corners at resonance 0, sustain 8: cutoff 380 four times (s00..s03, repeatability),',
                                                     'then 300, 340, 410, 459, 470, 480, 490, 500, 700, 900, 1200.'], w, end)
        st = [(fc, res, m) for fc in (200, 1500) for res in (0, 12) for m in (0x10, 0x20, 0x40, 0x50, 0x30, 0x60, 0x70)]
        w, end = modes('a', bypass, st)
        files['a_s3a_modes%s_a' % sfx] = text('a', ['S3a gate: modes at cutoff 200 and 1500, resonance 0 and 12: low, band, high, notch, low+band,',
                                                    'band+high, all three (s00..s27). Noise on oscillator 3, sustain 8.'], w, end)
    dist = []
    for fc, mode in ((2047, 0x10), (1024, 0x10)):
        for n, sus in ((2, 10), (3, 4), (3, 7), (2, 12)):
            dist.append((fc, mode, True, [(160.0, sus)] * n))
    for n, sus in ((3, 6), (3, 12)):
        dist.append((1500, 0x20, True, [(160.0, sus)] * n))
    for n, sus in ((3, 5), (3, 12), (2, 9)):
        dist.append((2047, 0x10, False, [(160.0, sus)] * n))
    for sus in (6, 12):
        dist.append((2047, 0x10, True, [(160.0, sus), (211.0, sus)]))
        dist.append((2047, 0x10, False, [(160.0, sus), (211.0, sus)]))
    w, end = tones('a', dist)
    files['a_s3a_dist_a'] = text('a', ['S3a gate: held triangles (160 Hz; two-tone 160 and 211 Hz), voices in phase: low-pass at 2047 and 1024',
                                       '(2 voices sustain 10 and 12, 3 voices sustain 4 and 7), band-pass at 1500 (3 voices, sustain 6, 12),',
                                       'direct (3 voices sustain 5, 12; 2 voices sustain 9), two tones through the low-pass and direct at sustain 6',
                                       'and 12. Segments of 16 frames (s00..s16).'], w, end)
    for m in ('a', 'b'):
        w, end = steps(m)
        files['a_s3a_steps_%s' % m] = text(m, ['S3a gate: steps 1 s apart (frame levels after each write): a held triangle (110 Hz, sustain 15)',
                                               'and volume writes direct and through the low-pass; routing writes with silent voices; resonance writes',
                                               'with a held DC voice (test bit + pulse) routed to the low-pass.'], w, end)
    for name, t in files.items():
        with open(os.path.join(out, name + '.bstrace'), 'w') as f:
            f.write(t)
    print('%d traces written to %s' % (len(files), out))


if __name__ == '__main__':
    main()
