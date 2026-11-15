import os, sys
from model import *

OUT = sys.argv[1]
P1 = 19656

SNARE = """bside-table 1
[restart]
mode classic
lead 2
[wave1]
    adsr 0 9 0 9, wave noise+gate =C#7
    wave pulse+gate =A3
    wave pulse+gate =F3
    wave noise+gate =D6 ticks 4
    wave noise =D6        # gate off: the release rings out
    end
[pulse1]
    set 2048
"""
ARP = """bside-table 1
[wave1]
    wave pulse+gate +0 ticks 0
loop:
    pitch +0
    pitch +4
    pitch +7
    jump loop
release:
    gate off ticks 0
    jump loop
[pulse1]
    set 25%
"""
PULSE = """bside-table 1
[restart]
mode phase
lead 2
[wave1]
    wave pulse+gate +0 ticks 12
vib:
    pitch +0.12 ticks 2
    pitch -0.12 ticks 2
    jump vib
[pulse1]
    set 512
    pingpong 512 3584 48
"""
FILTER = """bside-table 1
[restart]
mode phase
lead 2
ad 00
sr 00
[wave1]
    adsr 0 9 a 9, wave saw+gate +0
    end
[filter]
    route 1, mode lp, res 12, volume 15, cutoff 1800
    sweep -96 x16
    end
"""
KICK = """bside-table 1
[restart]
mode phase
lead 2
[wave1]
    adsr 0 8 0 8, wave tri+gate =C3
    pitch =F2
    pitch =C2
    pitch =G1 ticks 3
    wave tri =G1
    end
[filter]
    mode lp, cutoff 600, route 1
    sweep -100 x4
    end
"""
HIHAT = """bside-table 1
[restart]
mode classic
lead 1
[wave1]
    adsr 0 3 0 3, wave noise+gate =F#7
    wave noise+gate =C8 ticks 2
    gate off
    end
[filter]
    mode hp, res 4, cutoff 1500, route 1
    end
"""
MATRIX = """bside-table 1
[restart]
mode %s
lead 2
%s[wave1]
    wave pulse+gate =C4
    pitch =G4
    end
"""

def host_pulse():
    h = default_host()
    h['osc'][1] = dict(enabled=True, bits=0x20, sem=59.88, pw=2048, ad=0, sr=0xf6, sync=False, ring=False)
    return h

def spec(name, table, host, policy, events_fn, end_fn, kind, mults):
    return dict(name=name, table=table, host=host, policy=policy, events=events_fn, end=end_fn, kind=kind, mults=mults)

S = []
S.append(spec('snare', SNARE, default_host(), 'clean', lambda P, m: [(0, 'on', dict(policy='clean')), (44312 // m, 'off', {})], lambda P: 9 * P - 1, 'hand', (1, 2, 4)))
S.append(spec('arpeggio', ARP, default_host(), 'clean', lambda P, m: [(0, 'on', dict(policy='clean')), (44312 // m, 'off', {})], lambda P: 5 * P - 1, 'hand', (1, 2, 4)))
S.append(spec('pulselead', PULSE, host_pulse(), 'precondition', lambda P, m: [(0, 'on', dict(policy='precondition')), (157348 // m, 'off', {})], lambda P: 15 * P - 1, 'hand', (1, 2, 4)))
S.append(spec('filterbass', FILTER, default_host(), 'authentic', lambda P, m: [(0, 'on', dict(policy='authentic'))], lambda P: 18 * P - 1, 'hand', (1, 2, 4)))
S.append(spec('kick', KICK, default_host(), 'clean', lambda P, m: [(0, 'on', dict(policy='clean')), (2 * P + P // 2, 'off', {})], lambda P: 10 * P - 1, 'model', (1, 2, 4)))
S.append(spec('hihat', HIHAT, default_host(), 'precondition', lambda P, m: [(0, 'on', dict(policy='precondition')), (P + P // 4, 'off', {})], lambda P: 6 * P - 1, 'model', (1, 2, 4)))

def matrix_events(policy):
    def f(P, m):
        return [(0, 'idle', dict(policy=policy)), (3 * P, 'on', dict(policy=policy)), (4 * P + 1000 // m, 'off', {}),
                (6 * P, 'idle', dict(policy=policy)), (7 * P, 'on', dict(policy=policy)), (9 * P, 'on', dict(policy=policy)),
                (11 * P, 'on', dict(policy=policy, stolen=True))]
    return f

for mode in ('off', 'classic', 'phase'):
    for policy in ('clean', 'precondition', 'authentic'):
        S.append(spec('restart_%s_%s' % (mode, policy), MATRIX % (mode, ''), default_host(), policy, matrix_events(policy), lambda P: 13 * P - 1, 'model', (1,)))
for mode, policy in (('classic', 'clean'), ('phase', 'precondition'), ('phase', 'authentic')):
    S.append(spec('firstwave_%s_%s' % (mode, policy), MATRIX % (mode, 'firstwave tri\n'), default_host(), policy, matrix_events(policy), lambda P: 13 * P - 1, 'model', (1,)))

os.makedirs(OUT, exist_ok=True)
for s in S:
    for m in s['mults']:
        P = P1 // m
        out = run(s['table'], s['host'], P, s['events'](P, m), s['end'](P))
        kind = {'hand': 'hand-derived (m1) / scaled model (m2, m4)', 'model': 'independent model (tests/bside/golden/model.py), reviewed'}[s['kind']]
        text = to_text(out, s['end'](P), 'golden %s, tick.mult %d, Authentic, 50 Hz system, %s' % (s['name'], m, kind))
        open(os.path.join(OUT, '%s_m%d.bstrace' % (s['name'], m)), 'w').write(text)
        print(s['name'], m, len(out))
